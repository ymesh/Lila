#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * setGeoreference {crs, origin: [easting, northing, height], unitToMetre, wgs84: {latitude, longitude, height}} puts the open
 * level on the Lila scene's place on the Earth, so the level's origin is the scene's origin:
 *   - the GeoReferencing plugin's AGeoReferencingSystem (made when the level has none): flat planet, the scene's projected
 *     CRS, the origin given in it;
 *   - Cesium for Unreal's ACesiumGeoreference (the level's, made when it has none): cartographic origin at the WGS 84
 *     latitude, longitude and ellipsoidal height.
 * Either is skipped when its plugin is not enabled. One undo transaction. Returns {geoReferencingSystem, cesium, message}.
 *
 * importPointCloud {file, folder?, name?, label?} imports a .las written in the level's coordinates (metres) through the
 * LiDAR Point Cloud plugin, keeps its original coordinates, and shows it on an ALidarPointCloudActor labelled label (made
 * when the level has none). Returns {asset, actor, assigned}.
 */
void AddGeoreferenceMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
