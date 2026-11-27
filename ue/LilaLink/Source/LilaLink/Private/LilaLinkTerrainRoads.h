#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * Methods behind Lila's terrain road sends:
 *
 * setTerrainRoads {id, landscape, location, roads: [{id, label, kind, halfWidth, sideFalloff, endFalloff, raiseTerrain, lowerTerrain,
 * layer, points, mesh?, spans}]} lays the roads of Lila's terrain id as landscape splines on its landscape (the one setLandscape
 * tagged for id, else the landscape labelled landscape): a control point per point (position relative to location, Unreal
 * space, with the tangents, up and roll of setSpline's points), half-width and falloffs from the road, segments painting
 * layer and raising or lowering the landscape as asked (never inside bridge or tunnel spans), carrying mesh {asset,
 * forwardAxis, scaleToWidth} as their spline mesh. In a partitioned world the roads go on one landscape spline actor tagged
 * for id. Sending again replaces what the last send made. The landscape redraws its splines layer at once and the regions it
 * changed are marked dirty for Save All; in a World Partition level the regions the earlier and new roads reach are loaded
 * first. Replies {actor, roads, controlPoints, segments, regionsLoaded, paintLayersRemoved, warnings}.
 *
 * removeTerrainRoads {id} deletes them, the same way. Replies {removed, regionsLoaded, paintLayersRemoved, warnings}.
 */
void AddTerrainRoadMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
