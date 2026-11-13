#pragma once

#include "core/Export.h"
#include "scene/Georef.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lila::geo
{
class Mesh;
}

/**
 * Georeferenced photogrammetry and scan exports: the files beside a mesh that say where it is.
 *
 *   metadata.xml         <SRS> and <SRSOrigin> (DJI Terra, ContextCapture / iTwin Capture); SRS may be EPSG:n, WKT or ENU:lat,lon
 *   <name>_offset.xyz    Pix4D's offset of the local mesh (also offset.xyz, <name>.offset.xyz, <name>_offset.txt)
 *   <name>.rcinfo        RealityScan / RealityCapture's shifted project output: the shift as an origin or offset element
 *   <name>.prj           the CRS as WKT (Pix4D, Metashape, RealityScan, DJI Terra, ESRI tools); only one named after the mesh
 *
 * A mesh with a CRS and no offset whose coordinates are absolute (Metashape exports without a shift, RealityScan's plain
 * project output) is rebased on its rounded centre, which becomes its origin.
 */
namespace lila::scene::geoSidecar
{
struct Found
{
    georef::Georeference georeference;
    /** The files that said so, for the import message. */
    std::vector<std::filesystem::path> files;
    /** Where the CRS and the offset came from, in words: "metadata.xml", "Pix4D offset", ... */
    std::string source;
    bool hasOffset = false;
};

/** The georeference the files beside a mesh give; nullopt when there are none or they give no CRS. */
LILA_SCENE_API std::optional<Found> find(const std::filesystem::path& meshFile);

/**
 * Places meshes read from meshFile (their points in the file's own coordinates: easting, northing, height in the CRS's
 * units, relative to the offset) into the scene: sets the scene's georeference from the file when it has none, or moves
 * and turns them into the one it has. Points of absolute files are rebased first. Returns the message for the user, or
 * nullopt when the file has no georeference (the meshes are untouched).
 */
LILA_SCENE_API std::optional<std::string> place(const std::filesystem::path& meshFile, const std::vector<geo::Mesh*>& meshes);
} // namespace lila::scene::geoSidecar
