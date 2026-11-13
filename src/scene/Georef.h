#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Georeferencing: where the scene sits on the Earth. A scene's georeference is a coordinate reference system (an EPSG
 * code or WKT, through PROJ) and the projected coordinates of the scene's origin. Geometry stays near the origin; scene
 * point p is at origin + p in the CRS, cm converted to the CRS's units. Axes follow Unreal's georeferencing (east +X,
 * south +Y, up +Z) through Lila's Unreal axis mapping: a Z-up scene has east +X and north +Y, a Y-up one north +X and
 * east +Z, up along the up axis in both. Saved with the scene; nothing is moved when it changes.
 */
namespace lila::scene::georef
{
struct Georeference
{
    /** As given: "EPSG:32633", "EPSG:32633+5773", WKT or a PROJ string. */
    std::string crs;
    /** The scene origin in the CRS: easting, northing, height, in the CRS's linear units. */
    glm::dvec3 origin{0.0};

    bool operator==(const Georeference&) const = default;
};

/** What PROJ knows of a CRS. */
struct CrsInfo
{
    bool valid = false;
    std::string name;      // "WGS 84 / UTM zone 33N"
    std::string authority; // "EPSG:32633" when it has one
    bool projected = false;
    bool geographic = false;
    /** Metres per CRS unit along the horizontal axes, and along the vertical one (1 with none). */
    double unitToMetre = 1.0;
    double verticalToMetre = 1.0;
    std::string unitName;
    std::string error;
};

/** The scene's georeference; nullopt when it has none. */
LILA_SCENE_API std::optional<Georeference> current();
/** Undoable, saved with the scene; nullopt clears it. Throws CommandError when PROJ doesn't know the CRS. */
LILA_SCENE_API void setCurrent(const std::optional<Georeference>& g);

LILA_SCENE_API CrsInfo crsInfo(const std::string& crs);
/** A CRS that is projected (or compound with a projected part), so scene axes map onto its grid. */
LILA_SCENE_API bool usable(const CrsInfo& info);

/** Scene point (cm, scene axes) to the CRS's projected coordinates, and back. zUp is the scene's up axis. */
LILA_SCENE_API glm::dvec3 toProjected(const Georeference& g, const glm::dvec3& scene, bool zUp);
LILA_SCENE_API glm::dvec3 fromProjected(const Georeference& g, const glm::dvec3& projected, bool zUp);
/** Projected coordinates to latitude, longitude (degrees) and height, and back; nullopt when PROJ can't. */
LILA_SCENE_API std::optional<glm::dvec3> toGeographic(const std::string& crs, const glm::dvec3& projected);
LILA_SCENE_API std::optional<glm::dvec3> fromGeographic(const std::string& crs, const glm::dvec3& latLonHeight);
/** Projected coordinates to WGS 84 latitude, longitude and ellipsoidal height, as Cesium and Unreal's georeferencing take them. */
LILA_SCENE_API std::optional<glm::dvec3> toWgs84(const std::string& crs, const glm::dvec3& projected);
/**
 * A level laid out on the globe instead of the grid: Cesium for Unreal and a round-planet GeoReferencing system put a
 * level's X, Y, Z on the east, south and up of the tangent plane at the origin, where the scene uses the CRS's grid.
 * Both frames meet at the origin and part by the grid's convergence and scale and the Earth's curvature further out.
 */
struct GlobeFrame
{
    Georeference georeference;
    glm::dvec3 originEcef{0.0};
    glm::dmat3 ecefToEnu{1.0};
    /** Unreal level coordinates on the grid (cm, X east, Y south, Z up, from the scene origin) to the globe frame's. */
    LILA_SCENE_API glm::dvec3 toGlobe(const glm::dvec3& gridCm) const;
    /** Turns directions at gridCm from the grid frame to the globe frame (convergence, tilt). */
    LILA_SCENE_API glm::dmat3 rotationAt(const glm::dvec3& gridCm) const;
};
LILA_SCENE_API std::optional<GlobeFrame> globeFrame(const Georeference& g);

/** The scene georeference as Unreal, USD and glTF carry it: crs, origin, the origin's WGS 84 latitude/longitude/height. Null with none. */
LILA_SCENE_API nlohmann::json exportJson();
/** Points from one CRS to another, axes in east/north (or lon/lat) order; nullopt when PROJ can't. */
LILA_SCENE_API std::optional<std::vector<glm::dvec3>> convert(const std::string& from, const std::string& to, const std::vector<glm::dvec3>& points);

/**
 * A file's coordinates in its CRS (easting, northing, height in CRS units, from file.origin) to its own scene axes in cm
 * (the frame toProjected uses), before place() moves them into the scene's georeference.
 */
LILA_SCENE_API glm::dmat4 localFrame(const Georeference& file, bool zUp);
/** The CRS as OGC WKT 1 (what LAS and .prj files carry); empty when PROJ doesn't know it. */
LILA_SCENE_API std::string wkt(const std::string& crs);

/** The scene's up axis, from prefs. */
LILA_SCENE_API bool sceneZUp();

/**
 * How a georeferenced file's geometry, given in its own CRS relative to its own origin (in scene axes and cm, as the
 * importer reads it), lands in the scene. With no scene georeference the file's becomes the scene's (adopted) and the
 * geometry stays as it is; with one, the geometry is moved (and turned for grid convergence across CRSs) into it.
 */
struct Placement
{
    glm::dmat4 matrix{1.0};
    bool adopted = false;
    /** One line for the user: which of the two happened, and to what. */
    std::string message;
};
/** Sets the scene georeference when it is adopted. Throws CommandError when the file's CRS is unusable. */
LILA_SCENE_API Placement place(const Georeference& file);

/** Moves top-level nodes by m (Placement::matrix), through their translate and rotate channels. */
LILA_SCENE_API void placeRoots(Graph& g, const std::vector<uint64_t>& roots, const glm::dmat4& m);

/**
 * A file's georeference read back from what exportJson wrote (crs and origin), placed as place() does; nullopt when j holds
 * none. The roots are moved into the scene's georeference; the message goes to the log.
 */
LILA_SCENE_API std::optional<std::string> placeImported(Graph& g, const nlohmann::json& j, const std::vector<uint64_t>& roots);

/** CRSs in PROJ's database whose name or code contains query (case-insensitive): "EPSG:32633  WGS 84 / UTM zone 33N". */
LILA_SCENE_API std::vector<std::pair<std::string, std::string>> search(const std::string& query, size_t limit = 50);

/** georeference, setGeoreference, georeferenceConvert, georeferenceSearch. */
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::georef
