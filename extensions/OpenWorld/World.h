#pragma once

#include "OsmDoc.h"

#include <nlohmann/json.hpp>

#include <glm/glm.hpp>

#include <functional>

#include <string>
#include <vector>

namespace lila::ext
{
class Context;
}

/**
 * The world spec: one JSON that builds a whole open-world map from its parts (World Remix, terrain spec, settlements,
 * Buildings, slot rules, site kits, zones, export) with worldBuild, reads back with worldSpec and is checked as one with
 * worldCheck. A built world is a group node holding the spec it was built from and what each step made.
 */
namespace openworld::world
{
using json = nlohmann::json;

/** The node attribute holding the spec a world was built from. */
inline constexpr const char* kSpecAttr = "worldSpec";
/** The node attribute holding what worldBuild made: terrain, remix, settlement meshes, zone set, site ids, files. */
inline constexpr const char* kBuiltAttr = "worldBuilt";

/** JSON schema of the world spec (lila://schema/world-spec). */
const json& worldSpecSchema();

/**
 * Checks a spec's shape: known top-level keys, the types of each section, settlement and site entries. Throws
 * std::runtime_error naming the first bad field.
 */
void validateSpec(const json& spec);

/**
 * Relative file paths in the spec (remix sources, terrain source file, coast bathymetry, settlement OSM files) made
 * absolute against dir. A path not in dir for which elsewhere returns true (found relative to the workspace) stays as
 * given; a file found nowhere is placed in dir, where worldFetch writes it.
 */
json resolvePaths(json spec, const std::string& dir, const std::function<bool(const std::string&)>& elsewhere = {});

/**
 * The part of an OSM document inside an area, for one settlement of a larger map. world holds each node's world XZ
 * (cm, in document order); lo and hi are the area's corners. Lines (roads, rivers) are cut where they leave the area,
 * with a new node on the edge, so a long highway brings only its stretch inside; a line that leaves and comes back
 * becomes several ways (ids from 2^52 up). Closed ways (buildings, land use) go whole when most of their nodes are
 * inside. Tagged nodes inside are kept, and relations keep the members that stayed. keep, when given, narrows the area
 * to where it holds (a remix source's presence), lines cut where they leave it.
 */
OsmDoc clipOsm(const OsmDoc& d, const std::vector<glm::dvec2>& world, glm::dvec2 lo, glm::dvec2 hi, const std::function<bool(glm::dvec2)>& keep = {});

/** Commands worldBuild, worldSpec, worldCheck, worldMap; resources and the make_open_world prompt. */
/** The bathymetry files a remix spec's sources blend into its seabed, comma separated; empty when none. worldBuild skips the terrain spec's own bathymetry then. */
std::string remixBathymetryFiles(const json& remix);

void registerWorld(lila::ext::Context& ctx);
/** Level > Open World entries and the toolkit's World group. */
void registerWorldUi(lila::ext::Context& ctx);
} // namespace openworld::world
