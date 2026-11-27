#pragma once

#include "LilaLinkServer.h"

class ALandscape;
class ULandscapeSplinesComponent;

namespace LilaLink
{
/**
 * Methods behind Lila's curve to spline sends:
 *
 * setSpline {id, label, mode, location, splines, mesh?, landscape?, width?, deformTerrain?, join?, folder?} builds what Lila's curve id
 * makes, or updates it in place when it already exists. mode "actor" is an actor at location (Unreal space, cm) with a spline
 * component per entry of splines and, with mesh {asset, forwardAxis}, a spline mesh component per segment. mode "landscape"
 * adds control points and segments to the landscape's splines (a landscape spline actor in a partitioned world), with mesh as
 * their spline mesh; with join, splines meeting at one point share its control point. Each spline is {closed, points: [{position (relative to location), arriveTangent, leaveTangent, up, roll,
 * scale}]}. Replies {actor, components, segments, warnings}.
 *
 * Landscape mode also takes per spline layer (paint layer, added to the landscape when missing), layerWidth (share of the width
 * it paints, 0-1) and deform (overrides deformTerrain), and replies paintLayers, paintLayersAdded and regionsLoaded (landscape
 * regions a World Partition level had unloaded, loaded so the redraw reaches them). Paint layers it adds are Lila's (an
 * ALandscape tag LilaLink.PaintLayer:<layer>): once no landscape spline paints one, it leaves the landscape and Lila's
 * generated material, unless something other than splines painted it. paintLayersRemoved lists them.
 *
 * removeSpline {id} deletes what setSpline made for id. Replies {removed, regionsLoaded, paintLayersRemoved}.
 */
void AddSplineMethods(TMap<FString, FLilaLinkMethod>& Methods);

/**
 * In a World Partition level, loads the landscape streaming proxies under Box (XY) that are unloaded, so a splines layer
 * redraw reaches them; they stay loaded until the level closes, for Save All to write what changed. Returns how many it loaded.
 */
int32 LoadLandscapeUnder(UWorld* World, const FBox& Box);

/** In a World Partition level, loads every unloaded landscape spline actor, so tagged ones are found and all paint is known. */
int32 LoadSplineActors(UWorld* World);

/**
 * World bounds of what a landscape splines component's points named with Prefix (all with an empty Prefix) shape and paint:
 * each control point grown by its half width and falloffs.
 */
FBox SplineBounds(const ULandscapeSplinesComponent* Component, const FString& Prefix);

/**
 * Has the landscape redraw its splines edit layer now (added first with bAddLayer) and marks the regions it changed dirty for
 * Save All. Lila's paint layers no spline paints any more go first; their names are added to LayersRemoved.
 */
void RedrawSplineLayer(ALandscape* Landscape, bool bAddLayer, TArray<FString>& Warnings, TArray<FString>* LayersRemoved = nullptr);
} // namespace LilaLink
