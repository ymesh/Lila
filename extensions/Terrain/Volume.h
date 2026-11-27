#pragma once

#include "Terrain.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace terrain
{
/**
 * One volume edit over a heightfield, in the heightfield's own space (cm, Y up). Kinds: cave (tunnel curves with radius
 * and noise, branches, chambers), tunnel (a road tunnel along path at road level, flat floor), overhang (an undercut along
 * a cliff line, deeper in soft strata), arch (from path[0] to path[1]), sphere, box, capsule (path, radius) and mesh (a
 * closed mesh through an Object Info node). subtract carves; otherwise it adds.
 */
struct VolumeOp
{
    std::string id;
    std::string kind = "cave";
    bool subtract = true;
    std::string material = "rock";
    /** Width of a smooth join with the ground, cm; 0 is a sharp one. */
    double blend = 0.0;
    uint32_t seed = 0;

    std::vector<glm::dvec3> path;
    double radius = 300.0;
    /** Wall roughness as a fraction of the radius, and the size of its bumps, cm. */
    double noise = 0.25;
    double noiseSize = 400.0;
    int branches = 0;
    double branchLength = 2000.0;
    int chambers = 0;
    double chamberRadius = 600.0;

    /** Overhang: how far it undercuts, how high the cut runs above the ground, strata thickness, and how much less a hard band cuts. */
    double depth = 400.0;
    double height = 600.0;
    double strata = 150.0;
    double hardness = 0.35;
    /** Overhang: a shelf this far out over the cut, cm. */
    double lip = 0.0;

    /** Arch: tube thickness in its plane, width across it; carve opens the rock under it. */
    double thickness = 250.0;
    double width = 400.0;
    bool carve = true;

    glm::dvec3 center{0.0};
    glm::dvec3 size{100.0};
    std::string object;

    nlohmann::json toJson() const;
    /** Throws std::invalid_argument on unknown kinds. */
    static VolumeOp fromJson(const nlohmann::json& j);
};

const std::vector<std::string>& volumeKinds();

/** How the volume becomes meshes and heights. Lengths cm. */
struct VolumeSettings
{
    /**
     * On: what the heightfield can hold goes into it, the rest becomes mesh pieces with a hole under each. Off: the
     * heightfield stays as it was and every changed place becomes mesh, with holes under it.
     */
    bool split = true;
    /** Voxel edge; 0 picks one from the smallest feature. Always a whole fraction of the heightfield cell. */
    double voxelSize = 0.0;
    /** Rock left over a cave for the heightfield to stay on top of it; thinner and the ground opens there. */
    double minRoof = 200.0;
    /**
     * Where the heightfield's cells are too coarse for the volume's shape (a sheer wall between two samples) by more than
     * this, mesh it as well; 0 leaves it to the heightfield, reported as heightError.
     */
    double maxHeightError = 0.0;
    /** Samples of hole beyond each opening, so seams sit on undisturbed ground. */
    int margin = 1;
    /** How far mesh edges drop below the ground at the seam. */
    double skirt = 200.0;
    /** Levels of detail per piece, the first full resolution and each next one at twice the voxel. */
    int lods = 1;
    /** Pieces larger than this across are cut into chunks; 0 keeps each piece whole. */
    double chunkSize = 25600.0;
    /** Triplanar texture repeat. */
    double uvScale = 400.0;
    /** Largest voxel count; past it the voxel grows, with a warning. */
    double maxVoxels = 16.0e6;

    nlohmann::json toJson() const;
    static VolumeSettings fromJson(const nlohmann::json& j);
};

/** One mesh of the split: a chunk of a connected region that the heightfield can't hold. */
struct VolumePiece
{
    std::string name;
    int region = 0;
    int chunk = 0;
    /** LOD0 first; heightfield space; face int "material" names in materialNames(), corner "normal" and "uv0". */
    std::vector<std::shared_ptr<const lila::geo::Mesh>> lods;
    lila::geo::Bounds bounds;
    /** Heightfield cells it has a hole over, and cells it lies under (ground left on top). */
    size_t holeCells = 0;
    size_t buriedCells = 0;
    std::vector<std::string> materials;
};

struct VolumeOpResult
{
    bool applied = false;
    lila::geo::Bounds bounds;
    std::string message;
};

struct VolumeResult
{
    VolumeSettings settings;
    double voxel = 0.0;
    int voxelsPerCell = 1;
    size_t bricks = 0;
    size_t voxels = 0;
    /** Samples whose height the volume moved (split on), cells cut open, cells left on top of a cave, hole samples. */
    size_t heightChanged = 0;
    size_t openCells = 0;
    size_t buriedCells = 0;
    size_t holeSamples = 0;
    size_t holeCells = 0;
    /** Largest height difference between the volume and the heightfield where the heightfield holds it, cm. */
    double heightError = 0.0;
    /** Columns the heightfield can't hold and no piece covers; 0 when every region became a mesh. */
    size_t unmeshedColumns = 0;
    /** Seam totals: the hole outline's length, the part no mesh edge sits on, and the largest height gap along it. */
    double seamLength = 0.0;
    double seamUncovered = 0.0;
    double seamGap = 0.0;
    std::vector<VolumePiece> pieces;
    std::map<std::string, VolumeOpResult> ops;
    std::vector<std::string> warnings;
    /** Every piece's LOD0, joined, for the viewport. */
    std::shared_ptr<const lila::geo::Mesh> mesh;
};

/**
 * Applies the ops in order over h (the ground: solid below it) and splits the result. h gets the heights the volume moved
 * (split on) and a "holes" mask (1 on hole samples; a cell with any hole corner is a hole). meshes holds the meshes of mesh
 * ops by object name, in the heightfield's space. Deterministic at any thread count.
 */
VolumeResult buildVolume(Heightfield& h, const std::vector<VolumeOp>& ops, const VolumeSettings& s,
                         const std::map<std::string, std::shared_ptr<const lila::geo::Mesh>>& meshes = {});

/** Cells with any corner sample of the holes mask above 0.5, as nx-1 by nz-1 flags. */
std::vector<uint8_t> holeCells(const Heightfield& h);

/** What lies along the holes' outline: its length, the part with no mesh over it, and the largest gap between mesh and ground. */
struct SeamReport
{
    double length = 0.0;
    double uncovered = 0.0;
    double maxGap = 0.0;
    size_t samples = 0;
    glm::dvec3 worst{0.0};
};

/**
 * Measures the seams between h (with its holes mask) and the meshes, independently of how they were made: points along
 * every outline edge, a little inside the hole, are cast down onto the meshes and compared with the ground's height.
 */
SeamReport measureSeams(const Heightfield& h, const std::vector<std::shared_ptr<const lila::geo::Mesh>>& meshes, int samplesPerEdge = 4);

/** Points seen from above that disagree with the holes: mesh on top outside a hole, or no mesh inside one. */
struct FootprintReport
{
    size_t checked = 0;
    size_t uncoveredInHole = 0;
    size_t meshOutsideHole = 0;
};

FootprintReport measureFootprint(const Heightfield& h, const std::vector<std::shared_ptr<const lila::geo::Mesh>>& meshes, double tolerance);

/** What a Terrain Volume node (by name) of a modifier made on its last evaluation. */
void publishVolumeResult(uint64_t modifier, const std::string& node, std::shared_ptr<const VolumeResult> r);
std::shared_ptr<const VolumeResult> volumeResult(uint64_t modifier, const std::string& node);
/** Every Terrain Volume node's result of a modifier, by node name. */
std::map<std::string, std::shared_ptr<const VolumeResult>> volumeResults(uint64_t modifier);

/** The Terrain Volume node and its commands (terrainCave, terrainTunnel, terrainOverhang, terrainArch, terrainVolumeStamp, ...). */
bool registerVolume(lila::ext::Context& ctx);
/** Forgets the results of modifiers no longer in the scene. */
void forgetGoneVolumeResults();

/**
 * The volume checks terrainCheck adds for a terrain's modifier: seam cracks over tolerance, holes the mesh doesn't fill,
 * regions with no mesh, ops that did nothing. world places the heightfield; issues are terrainCheck's shape.
 */
nlohmann::json volumeIssues(uint64_t modifier, const glm::dmat4& world, double tolerance);

/** Writes each piece as <name>_<piece>.fbx (LODs as an LOD group, in world space about the piece's centre) and a json list; null when there are none. */
nlohmann::json exportVolume(uint64_t modifier, const glm::dmat4& world, const std::filesystem::path& folder, const std::string& name);

/**
 * After a landscape send: the pieces go to the connected Unreal editor as static meshes placed where they belong, and the
 * holes onto the landscape's visibility layer (LilaLink setLandscapeVolume). settings are exportUnreal's for that send.
 * Null when the terrain has no volume pieces and no holes. Throws std::runtime_error.
 */
nlohmann::json sendVolumeToUnreal(uint64_t modifier, const glm::dmat4& world, const nlohmann::json& settings, const std::string& id,
                                  const std::string& label, const std::string& material);
} // namespace terrain
