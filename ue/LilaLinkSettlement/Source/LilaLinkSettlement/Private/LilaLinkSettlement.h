#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Methods behind Lila's settlementSendToUnreal and road profile commands, registered with LilaLink under the feature
 * "settlement" (positions are Unreal space, cm):
 *
 * setSettlementData {id, asset, label, lots, envelopes, roads, intersections, parking} writes a PCG data asset at asset (a
 * package path), replacing its data: pin Lots (a polygon per lot), Envelopes (a polygon per footprint), EnvelopePoints (a
 * point per envelope), Roads (a spline per road), Intersections (a point per junction: Control, Approaches, Phases,
 * Radius) and ParkingSpots (a point per space, bounds the space), each with its fields as attributes. Replies {asset,
 * lots, envelopes, roads, intersections, parking}.
 *
 * setZoneGraph {id, label, folder, profiles: {name: {lanes: [{width, direction, tags}]}} (lanes right to left),
 * roads: [{points, profile, tags}], junctions: [{center, points: [{position, forward, profile, vehicle, reverse}], tags,
 * control, phases, crosswalks}], parking, parkingType} makes the lane profiles (Lila_<name>) and tags in the ZoneGraph
 * settings, then an actor holding a spline zone shape per road and a polygon zone shape per junction, and asks ZoneGraph to
 * rebuild. When the project has MassTraffic, its empty lane filters get Lila's tags, the tags it already names for
 * vehicle, intersection, crosswalk and trunk lanes go on the matching shapes too, and folder gets its traffic light and
 * parking data (see LilaLinkSettlementTraffic.h). Replies {actor, shapes, profiles, massTraffic, warnings}.
 *
 * removeSettlement {id} deletes the ZoneGraph actor made for id. Replies {removed, laneProfilesRemoved, configBackup?}.
 *
 * Lane profiles are reference-counted by level in Config/LilaLink/LaneProfileUsers.json (LilaLink::SetLaneProfileUser):
 * a Lila_ profile goes from the settings when no level's settlement uses it any more, on removeSettlement or a resend
 * that stops using it (setZoneGraph replies laneProfilesRemoved). Tags stay: MassTraffic's filters and other shapes name
 * them.
 *
 * Lane profiles, tags and MassTraffic's lane filters live in the project's DefaultPlugins.ini: before a send changes it,
 * the file as it was is copied to Saved/LilaLink/ConfigBackups (a .absent note when it did not exist yet), and the reply
 * names the copy as configBackup.
 *
 * setRoadGrammar {asset, label, modules: [{symbol, size, scalable}], grammar} writes a road cross-section as a PCG data
 * asset: pin ModulesInfo (Symbol, Size, Scalable, DebugColor per module, as Subdivide Segment reads them) and pin Grammar
 * (a Grammar string attribute). Replies {asset, modules}.
 *
 * readRoadGrammar {asset} reads a road definition's slots: from such a data asset, or from any asset holding arrays of
 * structs with a name (Symbol, Name, Slot, Id) and a size (Size, Width), and a Grammar string if it has one. Replies
 * {asset, class, modules: [{symbol, size}], grammar}.
 *
 * Under the feature "settlementCells", for World Partition:
 *
 * setSettlementCell {id, cell, asset, label, center, size, graph?, lots, envelopes, roads, intersections, parking} writes the
 * cell's PCG data asset as setSettlementData does, then makes or updates the cell's actor (tagged by id and cell, at
 * center, a box of size across giving it the cell's bounds) with a PCG component; given a graph, the component runs it
 * with the asset as its SettlementData parameter. Replies {asset, lots, envelopes, roads, actor, spawned, generated,
 * warnings}.
 *
 * removeSettlementCells {id, cells?: ["x_z", ...], folder?} deletes those cell actors, or every cell of id, with their data
 * assets unless another level's cells name them; with every cell going, folder's PCG data assets no cell names go too.
 * Replies {removed, assetsDeleted, assets, assetsKept?}.
 */
namespace LilaLinkSettlement
{
inline const FString Feature = TEXT("settlement");
inline const FString CellsFeature = TEXT("settlementCells");
void RegisterMethods();
void RegisterCellMethods();
/** setSettlementData's work: writes the PCG data asset Params name. Null with OutError set on failure. */
TSharedPtr<FJsonValue> WriteSettlementData(const TSharedPtr<FJsonObject>& Params, FString& OutError);
} // namespace LilaLinkSettlement
