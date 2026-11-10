#pragma once

#include "core/Export.h"

#include "uv/Chart.h"
#include "uv/Hose.h"
#include "uv/Layout.h"
#include "uv/Metrics.h"
#include "uv/Pack.h"
#include "uv/Project.h"
#include "uv/Seams.h"
#include "uv/Unwrap.h"

#include <functional>
#include <string>
#include <string_view>

/** UV tools: islands, unwrapping, projections, auto seams, straighten and orient, packing, metrics. */
namespace lila::uv
{
struct UnwrapFacesOptions
{
    UnwrapOptions unwrap;
    /**
     * Keep each island where it was (centre, area and rough rotation) when it had uvs; else pack. Islands kept in place
     * that come out overlapping each other are packed instead, at the size they came out at.
     */
    bool keepPlacement = true;
    /** Pack the unwrapped islands into 0-1 instead (just these islands). */
    bool pack = false;
    PackOptions packing;
    /** Cut closed or holed islands open into disks. When off, nothing is unwrapped while any island is not a disk. */
    bool openIslands = true;
};

struct UnwrapFacesResult
{
    size_t islands = 0;
    size_t failed = 0;
    Index seamsAdded = 0; // cuts needed to open closed or holed islands
    /** Islands that are not disks, when openIslands is off: the mesh is left as it was. */
    size_t closed = 0;
};

/**
 * Unwraps the islands of the faces (all when empty) along their seams, opening any that are not disks. Islands whose old
 * uvs were unusable (folded or empty) take the texel density of the rest of the mesh's uvs, so they come out at its scale.
 */
LILA_SCENE_API UnwrapFacesResult unwrapFaces(geo::Mesh& mesh, std::span<const Index> faces, std::string_view uvSet, const UnwrapFacesOptions& opts = {});

struct AutoUvOptions
{
    AutoSeamOptions seams;
    UnwrapOptions unwrap;
    PackOptions pack;
    /** Runs on the seams before unwrapping and returns the seams it added, such as the mirror images of the cuts. */
    std::function<Index(geo::Mesh&, std::span<const Index> faces)> afterSeams;
};

struct AutoUvResult
{
    size_t islands = 0;
    Index seams = 0;
    PackResult pack;
};

/** One call for game assets: auto seams, SLIM unwrap of every island, then pack into 0-1. */
LILA_SCENE_API AutoUvResult autoUv(geo::Mesh& mesh, std::span<const Index> faces = {}, std::string_view uvSet = "uv0", const AutoUvOptions& opts = {});

struct LightmapOptions
{
    /** The lightmap resolution the asset will use in Unreal (its minimum). */
    int resolution = 64;
    /** Texels between charts; bilinear filtering needs at least 2. */
    double padding = 2.0;
    /** Packing search effort (see PackOptions::effort); lightmaps are small, so a thorough search is cheap. */
    int effort = 2;
    /** Rotation step for the charts, degrees (0 keeps their orientation). */
    int rotateStep = 90;
    std::string_view source = "uv0";
    std::string_view target = "uv1";
};

struct LightmapResult
{
    bool ok = false;
    size_t islands = 0;
    size_t rewrapped = 0; // source islands that folded or overlapped and were unwrapped again
    PackResult pack;
};

/**
 * Unreal lightmap channel: the source set's islands (or auto seams when it is missing), each made
 * non-overlapping, packed with no overlaps and padding for the lightmap resolution.
 */
LILA_SCENE_API LightmapResult generateLightmapUvs(geo::Mesh& mesh, const LightmapOptions& opts = {});

/**
 * Corners of the faces plus every corner sharing their uv around the same point without crossing a seam, so moving them
 * keeps islands whole and leaves islands that only touch across a cut behind.
 */
LILA_SCENE_API std::vector<Index> uvCornersOfFaces(const geo::Mesh& mesh, std::string_view uvSet, std::span<const Index> faces);

/** Every corner of the points, each once. */
LILA_SCENE_API std::vector<Index> cornersOfPoints(const geo::Mesh& mesh, std::span<const Index> points);

/** UV set the tools use when a command does not name one ("uv0" by default). */
LILA_SCENE_API const std::string& currentUvSet();
LILA_SCENE_API void setCurrentUvSet(std::string name);
} // namespace lila::uv
