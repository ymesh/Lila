#pragma once

#include "MasonryArch.h"
#include "MasonryCore.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace masonry
{
/** A wall of a plan, with its own params and its region (index into Plan::regions). */
struct WallPlan
{
    Footprint footprint;
    WallParams params;
    int region = 0;
    std::vector<int> openingRegions; // per params.openings
};

/** A floor or roof slab: a closed outline (Y ignored) between two heights. Ruined slabs break into pieces. */
struct SlabPlan
{
    std::vector<glm::dvec3> outline;
    double y0 = 0.0, y1 = 30.0;
    int region = 0;
};

/** What lies on a floor: cobbles (setts in staggered rows), flags (flagstones of many lengths) or tiles (a pitched roof of overlapping tiles). */
struct PavingSpec
{
    std::vector<glm::dvec3> outline; // Y ignored
    double y = 0.0;                  // the surface it lies on (tiles: the eaves)
    std::string kind = "cobbles";
    double size = 12.0;              // cm: a cobble's length, a flag's row width, a tile's width
    double pitch = 35.0;             // tiles: roof slope, degrees
    glm::dvec3 uphill{0.0, 0.0, 1.0}; // tiles: the way the roof rises, horizontal
};
std::vector<Stone> pavingStones(const PavingSpec& p, double joint, uint64_t seed);

/** Everything a masonry node lays out, before it is laid: walls, loose elements, slabs, and the named regions. */
struct Plan
{
    std::vector<WallPlan> walls;
    std::vector<Stone> elements;           // columns, stairs, vaults, freestanding arches; region set
    std::vector<geo::Mesh> elementCore;    // convex fill under stairs
    std::vector<SlabPlan> slabs;
    std::vector<std::string> regions;
    BuildOptions look;                     // stone shaping, and the default wall params
    double pitch = 0.0;                    // nominal course height, for the ruin's scales
};

/** Footprints as walls sharing one set of options, regions wall_0, wall_1, ... (or region, region_1, ...) and their openings. */
Plan planWalls(std::span<const Footprint> footprints, const BuildOptions& o, const std::string& region = {});

/** Parses a building spec (see specSchema) into a plan. Throws std::runtime_error naming what is wrong. */
Plan planBuilding(const nlohmann::json& spec, const BuildOptions& look);

/** The building spec's shape, for agents: every key, its type, default and meaning. */
nlohmann::json specSchema();

/** A column standing on base: plinth, base moulding, drums with entasis, capital. Stones in world space. */
struct ColumnSpec
{
    glm::dvec3 base{0.0};
    double height = 600.0;
    double diameter = 60.0;
    int drums = 0;          // 0: about one drum per diameter of shaft
    bool square = false;    // a pillar of square blocks
    bool plinth = true;
    bool capital = true;
    double entasis = 0.08;  // how much narrower the shaft is at the top, a fraction of the diameter
};
std::vector<Stone> columnStones(const ColumnSpec& c, double joint, uint64_t seed);

/** A straight flight of steps going up along direction from start (the bottom front edge's middle). */
struct StairSpec
{
    glm::dvec3 start{0.0};
    glm::dvec3 direction{1.0, 0.0, 0.0};
    double width = 200.0;
    int steps = 8;
    double rise = 18.0;
    double going = 30.0;
};
/** The treads, each split across the width into stones, and the fill under them as convex cells. */
std::vector<Stone> stairStones(const StairSpec& s, double stoneLength, double joint, std::vector<geo::Mesh>* fill);

/** A barrel vault springing from the line a-b, or a dome on a springing circle. */
struct VaultSpec
{
    bool dome = false;
    bool groin = false;                          // two barrels crossing over a square bay centred on centre, span wide
    glm::dvec3 from{0.0}, to{1000.0, 0.0, 0.0}; // barrel: the axis at springing height
    ArchSpec profile;                            // barrel: the cross section; span, ring
    glm::dvec3 centre{0.0};                      // dome
    double radius = 300.0;                       // dome: intrados
    double ring = 40.0;                          // dome: shell thickness
    double oculus = 0.0;                         // dome: radius of the opening at the top
};
std::vector<Stone> vaultStones(const VaultSpec& v, const Units& stone, uint64_t seed);

/** A freestanding arch ring springing at origin (the middle of the springing line), spanning along direction. */
std::vector<Stone> archRing(const ArchSpec& a, const glm::dvec3& origin, const glm::dvec3& direction, double depth);
} // namespace masonry
