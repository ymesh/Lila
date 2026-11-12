#pragma once

#include "SettlementCore.h"

#include <lila/GeometryNodes.h>

#include <cstdint>
#include <functional>
#include <string>

namespace lila::ext
{
class Context;
}
namespace lila
{
struct Args;
}
namespace lila::scene
{
class Graph;
}

namespace settlement
{
/**
 * The site in a geometry: the largest closed curve or loose-edge loop is the outline, open ones are fixed streets, loose
 * points are the centre (the first) and attractors. planeY is the average height of what was given, for a flat ground.
 */
Site siteFrom(const lila::gn::Geometry& g, double* planeY = nullptr);

/** A mesh in the geometry as ground, else a plane at planeY. */
Ground groundFrom(const lila::gn::Geometry& g, double planeY);
/** A mesh in the geometry as a walkable surface, else none. */
WalkSurface walkFrom(const lila::gn::Geometry& g, const WalkOptions& o);
/** The edits input's JSON; an empty object for empty text. Throws std::runtime_error when it is not a JSON object. */
json parseEdits(const std::string& text);

/** Options from named values (a node's inputs, or a command's flags). Throws std::runtime_error for a bad profile, pattern or overrides. */
Options optionsFrom(const std::function<double(const char*)>& number, const std::function<int64_t(const char*)>& integer,
                    const std::function<std::string(const char*)>& text, const std::function<bool(const char*)>& flag);

struct Parts
{
    lila::gn::GeometryPtr streets, lots, buildings, water, levels;
};

/**
 * Streets (ribbons and centrelines), lots (outlines), buildings (envelope volumes and footprint curves) and levels
 * above the ground (deck streets, deck outlines, elevated roads, ramps, skybridges, air lanes as curves).
 */
Parts toGeometry(const Layout& l, const Ground& ground);

/** The layout a settlement modifier shows, rebuilt from its inputs, ground, walkable mesh and edits. */
struct Rebuilt
{
    Layout layout;
    Ground ground;
    WalkSurface walk;
    Options options;
    json values;
    std::string mesh, modifier;
    Site site;
};

/**
 * Throws CommandError naming cmd when name is not a settlement or its layout fails. edits, when given, is used in place
 * of the modifier's edits input.
 */
Rebuilt rebuildSettlement(lila::scene::Graph& g, const std::string& name, const char* cmd, const json* edits = nullptr);

bool registerNodes(lila::ext::Context& ctx);
void registerCommands(lila::ext::Context& ctx);
/** settlementSendToUnreal. */
void registerUnrealCommands(lila::ext::Context& ctx);
void registerUi(lila::ext::Context& ctx);
/** settlementExport (files: JSON, GeoJSON, USD, DXF, Unity, Godot). */
void registerExportCommands(lila::ext::Context& ctx);
/** Appends the placements flag's entries (a list or its JSON text) to the layout's placements; CommandError naming cmd when malformed. */
void addPlacements(Layout& l, const lila::Args& a, const char* cmd);
/** settlementRoadMesh, settlementFurniture, settlementNames, settlementLanes and settlementTransit. */
void registerStreetscapeCommands(lila::ext::Context& ctx);

// Terrain (TerrainLink.cpp): Terrain commands are called by name, so the Terrain extension is never linked.

/** True when the node is a terrain the Terrain extension answers for (terrainInfo knows it). */
bool isTerrainGround(const std::string& node);
/**
 * Sites scored from the terrain's own analysis: terrainSample fields (slope, floodRisk, distanceToWater, prominence,
 * solar, ...), terrainWaterInfo crossings, terrainSites resources and townCentres, terrainBuildable land, refined by
 * terrainRegionStats over each best site. analysis carries riverArea, seaLevel and floodReturn through.
 */
std::vector<SiteScore> suggestOnTerrain(const std::string& terrain, const Polygon& outline, const Ground& ground, const SiteOptions& o,
                                        const json& analysis, json* report = nullptr);
/** The terrain's rivers (terrainWaterInfo) as water lines, each its mean width. */
std::vector<WaterLine> terrainRivers(const std::string& terrain, const json& analysis);
/** terrainRoute from a to b at maxGrade (rise over run); empty when there is no way, with why in note. */
std::vector<Vec2> terrainRoad(const std::string& terrain, Vec2 a, Vec2 b, double maxGrade, std::string* note);
/**
 * Fits a settlement on a Terrain ground: main roads routed by terrainRoute (kept in its edits) and, with rivers, the
 * terrain's rivers crossing the outline added as water. Null when the ground is not a Terrain.
 */
json followTerrain(lila::scene::Graph& g, const std::string& settlement, const std::string& ground, bool rivers, const char* cmd);
/** settlementGrade, settlementReplan, settlementExplain, settlementCheck. */
void registerTerrainCommands(lila::ext::Context& ctx);
} // namespace settlement
