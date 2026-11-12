#pragma once

#include "SettlementCore.h"

#include <functional>

namespace lila::ext
{
class Context;
}

/**
 * Blocks, lots, land use, landmarks and building envelopes, laid between the streets of a finished street graph.
 *
 * Envelope schema: what toJson writes under "envelopes" (and settlementEnvelopes returns), one per buildable lot. Lengths
 * cm, 2D points [x, z], heights world Y. Consumers (UE PCG graphs, Masonry, library placement, any pipeline) may rely on:
 *
 * - lot (index into lots), lotId (stable id), use, groundUse (what the ground floor is for), era, wealth (0 low, 1 middle,
 *   2 high), landmark (its name, empty for none), route (outline, massing, library, masonry or unreal).
 * - footprint: counter-clockwise ring of the buildable area after setbacks. sides: street, party or open per side (side k
 *   runs from point k to k + 1). partyWith: per side, the lot across a party wall (-1 none); both neighbours mark the same
 *   wall, so consumers build it once.
 * - frontSetback, sideSetback: the distances the footprint keeps from the lot's street sides and from its other sides.
 * - tiers: [{footprint, bottom, top}], the volume. The first runs from base; each next one is stepped in above the last.
 * - floors, floorHeight, height (walls), base (bottom of the foundation), ground (lowest ground under the footprint), top
 *   (wall top), foundationDepth (highest ground to base), foundationSource (terrainRegionStats or ground).
 * - roof: gable, hip, flat, mansard or dome. ridge: unit [x, z] along the ridge. roofHeight: wall top to ridge.
 * - front: unit [x, z] toward the street. entrances: [[x, z], ...] on the street sides, the main one first.
 * - corner: none, wrap (facades on both streets), chamfer (the corner cut) or tower. random: a stable 0 to 1 variation.
 */
namespace settlement
{
const char* blockTypeName(BlockType t);
bool parseBlockType(std::string_view name, BlockType* out);
const char* lotMethodName(LotMethod m);
bool parseLotMethod(std::string_view name, LotMethod* out);
const char* routeName(BuildingRoute r);
bool parseRoute(std::string_view name, BuildingRoute* out);
bool parseUse(std::string_view name, Use* out);

/** The profile's era, or the one its name and pattern suggest: medieval, early, industrial, modern or future. */
std::string eraOf(const Profile& p);
/** Medieval and early towns: markets, churches, burgage plots, fields outside. */
bool preModern(std::string_view era);

/** Throws std::runtime_error for an unknown era, blockType or lotMethod in the profile. */
void validateParcelProfile(const Profile& p);
/** The minimum-area box over the polygon's side directions: centre, unit axes (u the long one), half sizes. */
void boxOf(const Polygon& p, Vec2& c, Vec2& u, Vec2& v, double& hu, double& hv);

/** Reads blockOverrides, lotOverrides, buildingRoutes and foundations from the edits. Throws std::runtime_error naming what is wrong. */
void applyParcelEdits(Site& site, const json& edits);

/**
 * Per graph edge, 0 to 1: how central the street is in the network, half closeness (integration) and half betweenness,
 * from shortest paths out of up to 256 evenly spread junctions. 0 for edges that are not streets.
 */
std::vector<double> streetAccess(const StreetGraph& g);

/** Blocks, their types, lots, landmarks, land use and envelopes, into the layout. */
void planParcels(Layout& l, const Site& site, const Ground& g, const Options& o);
/**
 * planParcels again after the street graph changed inside lo-hi: blocks are remade everywhere, but a block whose stable id
 * survives outside the region keeps its lots and envelopes as they were. Landmarks are not re-placed. Returns the blocks cut afresh.
 */
int replanParcels(Layout& l, const Site& site, const Ground& g, const Options& o, Vec2 lo, Vec2 hi);
/** True when the polygons share area: sides crossing, or a corner more than tol inside the other. */
bool polygonsShareArea(const Polygon& a, const Polygon& b, double tol);

/** The lots a block's method cuts it into. Side tags are street edge ids (-1 none), as planParcels keeps them until uses are set. */
std::vector<Polygon> cutBlock(const Block& b, const Layout& l, const Options& o, std::vector<Polygon>* courtyards = nullptr,
                              std::vector<std::string>* warnings = nullptr);
/** The envelope of a buildable lot: setbacks by block type, floors, tiers, roof, entrances, corner, foundation. */
Envelope parcelEnvelope(const Lot& lot, const Block& block, const Layout& l, const Ground& g, const Options& o, const Site& site);
/** The footprint's size along the street (width) and into the lot (depth), and its centre on those axes. */
void frontageFit(const Envelope& env, double& width, double& depth, Vec2& centre);
/** A library building on its lot as an engine placement: centred on the footprint at the ground, turned to the street. */
Placement libraryPlacement(const Envelope& env, const LibraryPick& pick);
/** Two touching polygons as one (both counter-clockwise, sharing part of their boundary). Empty when they do not join into one ring. */
Polygon joinPolygons(const Polygon& a, const Polygon& b, double tolerance = 2.0);
/** Marks footprint sides that coincide with a neighbour's as party walls on both, with the lot across. */
void matchPartyWalls(std::vector<Envelope>& envelopes);
/** The lowest and highest ground under a footprint, sampled as envelopes are: corners, centre and a grid inside. */
void envelopeGround(const Polygon& footprint, const Ground& g, double& lo, double& hi);

json blockJson(const Block& b);
/** The lot fields beyond toJson's base set. */
json lotExtras(const Lot& lot);
/** The envelope fields beyond toJson's base set. */
json envelopeExtras(const Envelope& env);
/** Block types, lot methods, uses, wealth, lot area distribution, landmarks and warnings. */
json parcelStats(const Layout& l);

/**
 * Greybox massing of one envelope: tiers as prisms, the roof from the straight skeleton, a dome or a mansard on top.
 * Returns false when the roof the envelope names could not be built on its footprint and a flat cap stands in.
 */
bool appendMassing(const Envelope& env, const Layout& l, geo::Mesh& into, const std::function<void(int part)>& tagFace);

/** settlementBlocks, settlementSubdivide, settlementLots, settlementLandUse, settlementPlaceLandmark, settlementEnvelopes, settlementFoundations, settlementBuildings. */
void registerLotCommands(lila::ext::Context& ctx);
} // namespace settlement
