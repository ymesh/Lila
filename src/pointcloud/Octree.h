#pragma once

#include "core/Export.h"
#include "pointcloud/PointSource.h"

#include <nlohmann/json.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

/**
 * Point cloud caches (.lilapc): the points of a scan sorted into an octree built out of core, so a cloud of hundreds of
 * millions of points opens at once and streams in by level of detail. Each node holds a grid sample of its cube (one
 * point per 1/128 of its size), the finer points in its children; leaves hold the rest. Points are quantised to 16 bits
 * within their node, with 8-bit colour, LAS class and 16-bit intensity: 12 bytes a point.
 */
namespace lila::pointcloud
{
/** One packed point as stored and as the GPU reads it: three 32-bit words. */
struct PackedPoint
{
    uint16_t x, y, z;
    uint8_t r, g, b, classification;
    uint16_t intensity;
};
static_assert(sizeof(PackedPoint) == 12);

/** Grid cells per node edge: a node's points are about size / kGrid apart. */
inline constexpr int kGrid = 128;

struct OctreeNode
{
    glm::dvec3 min{0.0}; // cloud-local cm
    double size = 0.0;   // cube edge, cm
    uint64_t offset = 0; // byte offset of its points in the file
    uint32_t count = 0;
    uint32_t firstChild = 0; // index of its first child; children are consecutive in the order of childMask's bits
    uint8_t childMask = 0;
    uint8_t depth = 0;
    uint16_t pad0 = 0;
    uint32_t pad1 = 0;

    double spacing() const { return size / kGrid; }
};
static_assert(sizeof(OctreeNode) == 56);

struct BuildOptions
{
    /** File coordinates to cloud-local cm (the georeference's placement and the unit and axis change). */
    glm::dmat4 toLocal{1.0};
    /** Where the temporary files go; next to the output when empty. */
    std::filesystem::path tempDir;
    /** Called now and then with the fraction done; false cancels (the build throws). */
    std::function<bool(double)> progress;
    /** Points sorted in memory at once; a larger cube is split through temporary files first. */
    uint64_t inMemory = 24'000'000;
    /** Stored in the cache's header as it is (source, CRS, georeference...). */
    nlohmann::json metadata;
};

struct BuildStats
{
    uint64_t points = 0;
    size_t nodes = 0;
    int depth = 0;
    double seconds = 0.0;
};

/** Builds out's cache from every point of source. Throws std::runtime_error on I/O failure or cancel. */
LILA_SCENE_API BuildStats buildOctree(PointSource& source, const std::filesystem::path& out, const BuildOptions& options);

/** An open cache: its header and node table in memory, points read on demand from any thread. */
class LILA_SCENE_API PointCloudFile
{
public:
    /** Throws std::runtime_error when the file is missing or not a cache. */
    explicit PointCloudFile(const std::filesystem::path& path);
    ~PointCloudFile();

    const std::vector<OctreeNode>& nodes() const { return mNodes; }
    const nlohmann::json& metadata() const { return mMeta; }
    const std::filesystem::path& path() const { return mPath; }
    uint64_t pointCount() const { return mPoints; }
    bool hasColor() const { return mMeta.value("color", false); }

    /** The node's points; thread safe. */
    std::vector<PackedPoint> read(uint32_t node) const;
    /** A packed point back to cloud-local cm. */
    static glm::dvec3 position(const OctreeNode& n, const PackedPoint& p)
    {
        return n.min + glm::dvec3(p.x, p.y, p.z) * (n.size / 65535.0);
    }

private:
    std::filesystem::path mPath;
    nlohmann::json mMeta;
    std::vector<OctreeNode> mNodes;
    uint64_t mPoints = 0;
    mutable std::mutex mMutex;
    std::FILE* mFile = nullptr;
};
} // namespace lila::pointcloud
