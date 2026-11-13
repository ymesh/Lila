#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/** Point cloud files: LAS/LAZ (LASzip) and E57 (libE57Format) read in batches, LAS/LAZ written. */
namespace lila::pointcloud
{
/** One point as a file gives it: position in the file's coordinates (double), colour, intensity and LAS class. */
struct RawPoint
{
    glm::dvec3 position{0.0};
    uint8_t rgb[3]{255, 255, 255};
    uint8_t classification = 0;
    uint16_t intensity = 0;
};

/** What a point file says about itself before its points are read. */
struct SourceInfo
{
    uint64_t count = 0;
    /** Bounds of the positions as the file states them; nullopt when it states none. */
    std::optional<std::pair<glm::dvec3, glm::dvec3>> bounds;
    /** The CRS (WKT or "EPSG:n"); empty when the file names none. */
    std::string crs;
    bool color = false;
    bool intensity = false;
    bool classification = false;
    /** Human-readable kind: "LAZ 1.4, format 7". */
    std::string format;
};

class LILA_SCENE_API PointSource
{
public:
    virtual ~PointSource() = default;
    virtual const SourceInfo& info() const = 0;
    /** Up to max more points into out (cleared first); false once there are none. */
    virtual bool next(std::vector<RawPoint>& out, size_t max) = 0;
    /** Back to the first point. */
    virtual void rewind() = 0;
};

/** LAS, LAZ or E57 by extension; throws std::runtime_error naming the file's problem. */
LILA_SCENE_API std::unique_ptr<PointSource> openSource(const std::filesystem::path& path);
/** The extensions openSource reads. */
LILA_SCENE_API bool readable(const std::filesystem::path& path);

/** Writes points to LAS (or LAZ by extension) in the given CRS (WKT VLR), scaled to 1 mm around their centre. */
LILA_SCENE_API void writeLas(const std::filesystem::path& path, const std::vector<RawPoint>& points, const std::string& crsWkt);
/** writeLas from a source read once (its count and bounds must be known), for clouds too big for memory. */
LILA_SCENE_API void writeLas(const std::filesystem::path& path, PointSource& source, const std::string& crsWkt);
} // namespace lila::pointcloud
