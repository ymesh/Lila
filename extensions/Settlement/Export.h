#pragma once

#include "SettlementCore.h"

#include <lila/Curves.h>

#include <map>
#include <string>
#include <vector>

/**
 * Settlement export: World Partition cells, live-sync manifests and their diff, and the file formats (GeoJSON, USD,
 * DXF, the engine JSON the Unity and Godot importers read). Everything here is pure data, for tests and commands alike.
 */
namespace settlement
{
// ---------------------------------------------------------------------------------------------------------------------
// Cells

/** A square cell of the partition grid, by index along world X and world Z. */
struct Cell
{
    int64_t x = 0, z = 0;
    auto operator<=>(const Cell&) const = default;
    /** "x_z", as cell keys and asset names carry it. */
    std::string name() const;
    Vec2 center(double size) const { return {(double(x) + 0.5) * size, (double(z) + 0.5) * size}; }
};

Cell cellAt(Vec2 p, double size);

/** A road's polyline cut at the cell borders: the part inside one cell. */
struct RoadPiece
{
    int line = 0; // RoadNetwork::lines
    std::vector<glm::dvec3> pts;
};

/** What lies in a cell: lots and envelopes by their centroid, junctions, parking and placements by position, roads cut. */
struct CellContents
{
    std::vector<int> lots, envelopes, junctions, parking, placements;
    std::vector<RoadPiece> roads;
};

std::map<Cell, CellContents> partition(const Layout& l, const RoadNetwork& net, double cellSize);

/** A polyline cut at the borders of square cells, each piece with the cell it lies in (end points shared). */
std::vector<std::pair<Cell, std::vector<glm::dvec3>>> cutByCells(std::span<const glm::dvec3> pts, double cellSize);

// ---------------------------------------------------------------------------------------------------------------------
// Stable ids and live sync

/** A lot's stable id: Lot::key, or its centroid to the metre for a lot without one. */
std::string lotKey(const Lot& lot);
/** A road line's stable id: its street's with its ends (a street runs on through junctions), or the edit path for surface paths. */
std::string roadKey(const StreetLine& s);
std::string junctionKey(const Junction& j);

/** Every element's stable id and a hash of what it holds: lots, envelopes (by lot), roads, junctions, placements. */
std::map<std::string, uint64_t> elementHashes(const Layout& l, const RoadNetwork& net);

/** One object live sync sends and keeps: its key, what it is, the hash of what it holds and the element ids in it. */
struct SyncItem
{
    std::string key;
    std::string kind; // roads, paths, massing, pcg, place, zoneGraph
    uint64_t hash = 0;
    std::vector<std::string> ids;
    Cell cell;         // roads (meshes), massing, pcg and place items
    std::string asset; // place items: the placements' asset
};

/** Which objects a send makes, and the settings that change what they hold (hashed into every item). */
struct SyncOptions
{
    std::string key;                 // the settlement's id, the prefix of every item key
    std::string roads = "landscape"; // landscape, splines, meshes or none
    bool massing = true, pcg = true, partition = true, zoneGraph = true;
    std::string settings;
};

/**
 * The items of a send: roads (one per cell with road meshes, else one for ground roads and one for surface paths),
 * massing and PCG data per cell, placements per cell and asset, and the lane graph. roadMeshes holds each cell's road
 * mesh hash when roads is meshes.
 */
std::vector<SyncItem> syncItems(const Layout& l, const RoadNetwork& net, const std::map<Cell, CellContents>& cells, const SyncOptions& o,
                                const std::map<Cell, uint64_t>& roadMeshes = {});

/** A mesh's faces grouped by the cell of each face's centre, each group recentred on its cell's middle at height 0. */
std::map<Cell, geo::Mesh> meshByCell(const geo::Mesh& m, double cellSize);
uint64_t meshHash(const geo::Mesh& m);
/** An asset path as a name part: letters, digits and underscores. */
std::string safeName(std::string_view s);

/** What changed since the manifest of the last send. */
struct SyncDiff
{
    std::vector<std::string> added, changed, removed, unchanged; // item keys
    std::vector<std::string> addedIds, changedIds, removedIds;   // element ids
};

/** {items: {key: {kind, hash}}, elements: {id: hash}}, as written after a send. */
json syncManifest(std::span<const SyncItem> items, const std::map<std::string, uint64_t>& elements);
SyncDiff diffSync(const json& previous, std::span<const SyncItem> items, const std::map<std::string, uint64_t>& elements);
json toJson(const SyncDiff& d);

/**
 * Placements from JSON: [{id, asset, position: [x, y, z], rotation: [x, y, z, w], scale: [x, y, z] or a number, tags}],
 * world cm. Throws std::runtime_error naming the entry when id or asset is missing or a vector is malformed.
 */
std::vector<Placement> placementsFrom(const json& list);

/** FNV-1a over text, for item hashes. */
uint64_t hashText(std::string_view s);

// ---------------------------------------------------------------------------------------------------------------------
// Files

/** Driving lane centrelines of a road network, offset from each road by its cross-section, pointing the way traffic goes. */
struct LaneLine
{
    int line = 0;
    int lane = 0;
    bool forward = true;
    double width = 0.0;
    std::vector<glm::dvec3> pts;
};
std::vector<LaneLine> laneLines(const Layout& l, const RoadNetwork& net);

/**
 * A GeoJSON FeatureCollection: streets (graph edges), junctions, blocks, lots, envelopes, lanes and placements as
 * features with a "layer" property and their fields. Coordinates are metres, x east and -z north; heights stay in
 * properties.
 */
json toGeoJson(const Layout& l, const RoadNetwork& net, const std::string& name);

/** USD text (.usda): Scopes of BasisCurves (streets, lanes), Meshes (blocks, lots, envelopes) and Xforms (junctions, placements) with lila: attributes. */
std::string toUsda(const Layout& l, const Ground& ground, const RoadNetwork& net, const std::string& name);

/** Plan layers for the DXF writer: Streets, Paths, Water, Blocks, Lots, Buildings, Lanes, in file units with Z up. */
std::vector<std::pair<std::string, lila::curve::Splines>> dxfLayers(const Layout& l, const Ground& ground, const RoadNetwork& net, double cmPerUnit);

/** Engine axes: unity is left-handed (z flipped), godot right-handed; both Y up, metres. */
enum class EngineAxes : uint8_t
{
    Unity,
    Godot,
};

/** The engine JSON (format lila.settlement.engine) the Unity and Godot importers read: flat float arrays, engine space. */
json toEngineJson(const Layout& l, const Ground& ground, const RoadNetwork& net, const std::string& name, EngineAxes axes);

/** Massing: a closed prism per envelope, as meshes per cell (positions relative to the cell centre at height 0). */
std::map<Cell, geo::Mesh> massingByCell(const Layout& l, double cellSize);
/** Every envelope's prism in one mesh, world space. */
geo::Mesh massing(const Layout& l);
} // namespace settlement
