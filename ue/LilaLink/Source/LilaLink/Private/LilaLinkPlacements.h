#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * Level placements of Lila instance groups, keyed by Lila node id and tagged on the actors they make:
 *
 * placeInstances {group, mesh, mode: actors | ism | hism, placements: [{id, name, location, rotation (quat xyzw), scale, customData?, components?}]}
 *   makes the open level hold exactly these placements of the static mesh: an AStaticMeshActor per member, or one actor
 *   with an (H)ISM component holding every member as an instance. Members no longer sent go; switching mode swaps one
 *   representation for the other. A placement's components (Lila attribute components) go onto its actor; an (H)ISM
 *   holder takes the first placement's. One undo transaction. Returns {spawned, updated, removed, mode, warnings, assets}.
 *   A placement's lightingChannels [bool, bool, bool] set its component's Lighting Channels 0, 1 and 2 (an (H)ISM's
 *   from the first placement, as instances share them).
 *   hlodLayer (an HLOD layer asset path) puts every actor of the group on that layer; "none" leaves them out of HLODs.
 * placeLights {lights: [{id, name, kind: point | spot | directional | area | disc, location, forward, up, color, intensity,
 *   units, useTemperature, temperature, range, innerConeAngle, outerConeAngle, width, height, sourceRadius, castShadows,
 *   lightingChannels?}]} spawns or updates a light actor per Lila light, matched by node id (forward is where it shines).
 *   Returns {spawned, updated, warnings}.
 * removePlacements {groups?: [...], ids?: [...]} removes what placeInstances made for those groups or nodes. Returns {removed}.
 *   levelInstances=true also removes Lila Level Instances made only of those groups, their DL_Interior_ data layer (with
 *   its asset) and their level file with its external actors, once nothing else uses them: {levelInstances, dataLayers,
 *   levelsDeleted}.
 * listPlacements {} lists them: [{actor, group, node?, instances?}].
 *
 * Slots: placeInstances with actorClass (a class path) instead of mesh spawns an actor of that class per placement. A
 *   placement's tags (LilaSlot:<id>, LilaSlotType:<type>, LilaSlotData:<key>=<value>) replace the actor's LilaSlot tags;
 *   instanced ones put slotType and slotData in their holder's ULilaInstanceData (columns slotType, slot.<key>).
 * writeSlotTable {path, rows} rewrites the ULilaSlotTable data asset at path, so game code finds slots by id.
 *
 * A placement's physics {simulate, role, material, density, friction, restitution, mass?, collision: simple |
 *   complexAsSimple} sets its component's physical material (PM_<material>, made beside the mesh), and on an actor its
 *   Simulate Physics (Movable when it simulates) and mass; the first placement's collision sets the mesh's collision
 *   complexity, so UCX_ hulls or its own triangles are what it collides with.
 * makeLevelInstance {key, name, kind: levelInstance | packedLevelActor, level, groups: [...]} moves the actors those
 *   placement groups made into a new level (level, or level_2, ... when taken) and places it as a Level Instance or
 *   Packed Level Actor tagged with the key; one made earlier for the key is removed first. Only actors loose in the level
 *   count: with none and an instance already made for the key, that instance stays ({actor, level, kind, unchanged}).
 *   Returns {actor, level, kind, actors, replaced}. Placement and removal RPCs leave actors inside Level Instances alone.
 */
void AddPlacementMethods(TMap<FString, FLilaLinkMethod>& Methods);

/**
 * A package that uses Package and is not World's own (World's actors are judged by what is in memory, so ones just
 * removed don't count) nor Package's own external actors; empty when there is none, so Package may be deleted.
 */
FString UsedOutside(class UWorld* World, const FString& Package);

/** Deletes the data layers no actor of World uses any more, and their assets unless another level uses them; returns how many went. */
int32 DeleteUnusedDataLayers(class UWorld* World, const TArray<const class UDataLayerAsset*>& Assets, TArray<FString>& OutErrors);
} // namespace LilaLink
