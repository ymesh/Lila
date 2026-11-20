#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * Materials for Lila's ivy and other growth. MF_LilaFoliageWind, in the masters folder, sways a mesh by its vertex colour
 * (R how far a vertex sways, G its phase, B 1 on leaves, which also flutter) and outputs a World Position Offset that
 * users' own foliage materials can take. M_LilaIvy, beside it, is a masked two-sided foliage master on that function with
 * Tint, BaseColorMap (alpha is the opacity mask), Roughness and SubsurfaceColor; artists own it once made (regenerate
 * rebuilds its graph in place).
 *
 * ivyMaterials {folder, regenerate?, parent?, meshes: [static mesh paths], instances: [{name, slot, scalars: {param: v},
 *   vectors: {param: [r, g, b]}, files: {param: {file}}}]}
 * makes or updates each instance in folder on the master (or parent) and sets it on every slot named slot of the meshes.
 * Returns {master, built, instances, assigned: [{mesh, slot, material}], created, warnings}.
 */
void AddIvyMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
