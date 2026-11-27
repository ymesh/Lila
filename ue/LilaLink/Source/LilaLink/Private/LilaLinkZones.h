#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * Methods behind Lila's zoneSendToUnreal: the gameplay zones of a Lila zone set.
 *
 * setZones {id, label, folder?, asset?, dataLayers?, layers: [{id, type, label, priority, density, exclusive, dataLayer,
 * color, zones: [{id, name, priority, attrs, outline, holes, pieces, bottom, top, mask?}]}]} makes or updates, for the
 * set id: one ALilaZoneVolume per zone, its brush the convex pieces extruded from bottom to top (Unreal space, cm), with
 * the zone's layer, name, priority and attributes; one ULilaZoneData asset ZD_<label> under asset (default
 * /Game/Lila/Zones) holding every layer, zone, outline and mask; and, in a World Partition level with dataLayers, one data
 * layer per zone layer (asset DL_<dataLayer> under asset/DataLayers) holding that layer's volumes. Volumes are matched by
 * layer and zone id, updated in place, and the ones no longer sent are deleted. Replies {volumes, created, updated,
 * removed, asset, dataLayers}.
 *
 * removeZones {id, label?, asset?} deletes the volumes sent for set id, the set's data layers no actor uses, and ZD_<label>
 * unless another level's volumes name it (asset registry referencers, so per set: other sets' volumes don't keep it).
 * Replies {removed, dataLayers, zoneData, warning?}.
 */
void AddZoneMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
