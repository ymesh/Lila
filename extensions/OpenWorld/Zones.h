#pragma once

#include "RemixSpec.h"

#include <glm/glm.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * Zones: the gameplay layer over an open-world map. A zone set holds typed layers (districts, police and military
 * jurisdiction, cartel territory, ambient spawn sets, traffic and pedestrian density, wanted response, interiors, mission
 * markers, or a user type); each layer holds zones, each a region (polygons with holes, and/or a painted mask) with typed
 * attributes. Layers overlap freely; inside a layer the zone with the highest priority wins, then the later one. Lengths
 * are cm on the world XZ plane (+X east, -Z north). Every id is stable: the same id replaces.
 */
namespace openworld::zones
{
using json = nlohmann::json;

/** Cells per side of a mask tile. */
inline constexpr int kTile = 64;

/**
 * A painted mask: a value 0 to 255 per cell of a grid anchored at the world origin, stored as sparse 64 x 64 tiles, so it
 * has no fixed extent. Region layers count a cell as inside from 128 up; density layers scale the zone's density by it.
 */
struct Mask
{
    double cell = 500.0;
    std::map<std::pair<int, int>, std::vector<uint8_t>> tiles;

    bool empty() const { return tiles.empty(); }
    uint8_t cellValue(int cx, int cz) const;
    void setCell(int cx, int cz, uint8_t v);
    /** The cell's value at a world point, 0 to 1 (nearest cell). */
    double at(Vec2 p) const;
    /** The cell a world point falls in. */
    glm::ivec2 cellOf(Vec2 p) const;
    /** Drops tiles with nothing in them. */
    void prune();
    /** Cells with a value, as [min, max] inclusive; false when empty. */
    bool cellBounds(glm::ivec2& lo, glm::ivec2& hi) const;
    size_t setCells(uint8_t threshold = 1) const;
};

/** How a stroke changes a mask. */
struct Dab
{
    Vec2 center{0.0};
    double radius = 1000.0;
    /** Target value 0 to 1; erase lowers toward 0 instead. */
    double value = 1.0;
    /** 0 to 1: how far toward the target one dab goes at the centre. */
    double strength = 1.0;
    /** 0 hard edge to 1 fading from the centre. */
    double softness = 0.5;
    bool erase = false;
};
/** Paints one dab; returns the cells changed. */
size_t paint(Mask& m, const Dab& d);

struct Zone
{
    std::string id;
    std::string name;
    /** Where the name came from: given, fictionalName, settlement, numbered. */
    std::string nameSource;
    int priority = 0;
    json attrs = json::object();
    /** Outer rings; holes cut out of every ring they lie in. */
    std::vector<Polygon> polygons;
    std::vector<Polygon> holes;
    Mask mask;
    /** Where the zone was derived from ({kind: drawn|painted|district|terrain|water, ...}); null when made by hand. */
    json source;

    bool hasMask() const { return !mask.empty(); }
};

struct Layer
{
    std::string id;
    std::string type;
    std::string label;
    int priority = 0;
    /** Zones may not overlap (zoneCheck reports overlaps). */
    bool exclusive = false;
    /** Zones must cover the playable area (zoneCheck reports gaps). */
    bool mustCover = false;
    bool visible = true;
    /** Values scale by the painted weight (traffic, pedestrians) rather than counting from half up. */
    bool density = false;
    glm::vec3 color{0.6f};
    /** Attributes every zone of the layer starts with. */
    json defaults = json::object();
    /** Unreal World Partition data layer the layer's volumes go in; empty for the layer's label. */
    std::string dataLayer;
    std::vector<Zone> zones;

    Zone* zone(std::string_view id);
    const Zone* zone(std::string_view id) const;
};

/** A built-in or user layer type: what a new layer of it starts as. */
struct LayerType
{
    std::string id;
    std::string label;
    bool density = false;
    bool exclusive = false;
    bool mustCover = false;
    json defaults = json::object();
    /** What fictionalName names its zones as (district, government, business, ...). */
    std::string nameKind = "district";
    glm::vec3 color{0.6f};
    std::string doc;
    bool builtIn = true;
};
/** The built-in types: district, police, military, territory, pedSpawn, vehicleSpawn, boatSpawn, aircraftSpawn, traffic, pedestrians, wanted, interiors, missions. */
const std::vector<LayerType>& builtInTypes();

struct ZoneSpec
{
    int version = 1;
    /** Naming culture of generated zone names. */
    std::string culture = "mexican";
    /** Mask cell size, cm, shared by every mask of the set. */
    double maskCell = 500.0;
    /** Where layers that must cover are checked; empty for the ground's extent, else every zone's bounds. */
    std::vector<Polygon> playArea;
    std::vector<LayerType> types;
    std::vector<Layer> layers;

    Layer* layer(std::string_view id);
    const Layer* layer(std::string_view id) const;
    /** A type by id, user types first. Null when unknown. */
    const LayerType* type(std::string_view id) const;
};

json toJson(const ZoneSpec& s);
/** Throws std::runtime_error naming the bad field. */
ZoneSpec zonesFromJson(const json& j);
json zonesSchema();
json toJson(const Mask& m);
Mask maskFromJson(const json& j, double cell);
json toJson(const LayerType& t);
LayerType typeFromJson(const json& j);

/** A layer of type t with its defaults. */
Layer makeLayer(const LayerType& t, std::string id);

/** Inside the zone's polygons (1) or its mask's value (0 to 1), whichever is more. */
double weight(const Zone& z, Vec2 p);
/** In the zone: weight from half up, or above 0 in a density layer. */
bool contains(const Layer& l, const Zone& z, Vec2 p);
/** The zone's extent (polygons and mask); false when it has none. */
bool bounds(const Zone& z, Vec2& lo, Vec2& hi);
/** Area in m2: polygons less holes plus mask cells outside them (sampled), roughly. */
double areaM2(const Zone& z, const Layer& l);

/** The zones of a layer that hold p, winner first (priority, then the later zone). */
std::vector<const Zone*> zonesAt(const Layer& l, Vec2 p);

/** A layer's value at p: its winner's attributes, density scaled by the weight. */
json queryLayer(const Layer& l, Vec2 p);
/** Every layer's value at p, keyed by layer id. */
json query(const ZoneSpec& s, Vec2 p);

/** Each layer's winners sampled over a grid: zone index per sample (-1 none) and how many zones hold it. */
struct Raster
{
    Vec2 origin{0.0};
    double cell = 1000.0;
    int nx = 0, nz = 0;
    std::vector<int> winner;
    std::vector<uint8_t> count;

    Vec2 at(int x, int z) const { return origin + Vec2(x + 0.5, z + 0.5) * cell; }
};
Raster rasterize(const Layer& l, Vec2 lo, Vec2 hi, double cell);

/** Every zone's extent, false when there are none. */
bool specBounds(const ZoneSpec& s, Vec2& lo, Vec2& hi);

struct Issue
{
    std::string kind; // gap, overlap, empty, unnamed, noDensity
    std::string layer;
    std::vector<std::string> zones;
    Vec2 at{0.0};
    Vec2 lo{0.0}, hi{0.0};
    double areaM2 = 0.0;
    std::string what;
};
/**
 * Gaps where a must-cover layer leaves the play area uncovered, overlaps where an exclusive layer's zones share ground
 * (regions of at least minCells samples), and zones with no region. playArea empty: the zones' bounds.
 */
std::vector<Issue> check(const ZoneSpec& s, const std::vector<Polygon>& playArea, double cell, int minCells = 2);
json toJson(const Issue& i);

/** Outlines of a mask's cells from threshold up, as rings simplified to tolerance (cm); holes come back separately. */
void maskOutline(const Mask& m, uint8_t threshold, double tolerance, std::vector<Polygon>& rings, std::vector<Polygon>& holes);

/** Convex pieces covering a simple polygon. */
std::vector<Polygon> convexPieces(const Polygon& poly);
/** Convex pieces covering a ring less the holes inside it. */
std::vector<Polygon> convexPieces(const Polygon& ring, const std::vector<Polygon>& holes);

/** A stable colour for a zone from its id (region layers) or the layer's own (density layers). */
glm::vec3 zoneColor(const Layer& l, const Zone& z);

/** Engine export: layers and zones with polygons, mask outlines and mask image references. */
struct ExportedMask
{
    std::string layer, zone;
    glm::ivec2 lo{0}, size{0};
    std::vector<uint8_t> values; // row 0 at the lowest Z
};
json exportJson(const ZoneSpec& s, const std::string& maskPrefix, std::vector<ExportedMask>* masks);
/** Back from exportJson's document, masks read by the callback (file name to values with the given size). */
ZoneSpec importJson(const json& doc, const std::function<std::vector<uint8_t>(const std::string& file, glm::ivec2 size)>& readMask);

std::string base64(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> unbase64(std::string_view text);
} // namespace openworld::zones
