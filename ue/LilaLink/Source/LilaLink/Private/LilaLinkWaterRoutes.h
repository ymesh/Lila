#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * Methods behind Lila's terrain route sends: the boat and air route graph as spline actors, never ZoneGraph assets, so any
 * navigation or traffic system can be driven from them.
 *
 * setWaterRoutes {id, label, lanes, crafts?, laneProfiles?, folder?} makes or updates the routes of Lila's terrain id. Each
 * lane {id, kind, craft, air, from, to, main, points: [{position, width?, depth?, clearance?}]} becomes an actor with a
 * USplineComponent and a ULilaWaterRoute holding the craft, kind, links, lane profile width and per-point data. Positions
 * are Unreal space, cm. Actors are matched by lane id under the terrain, updated in place, and the ones no longer sent are
 * deleted. Replies {actors, created, updated, removed}.
 *
 * removeWaterRoutes {id} deletes the routes sent for terrain id. Replies {removed}.
 */
void AddWaterRouteMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
