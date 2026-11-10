#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * morphPaths {skeletalMesh, paths: [{curve, weights, shapes: [{name, values}]}], postProcess?}: keeps Lila's morph
 * paths on the mesh for the Lila Morph Paths anim node, and with postProcess runs that node from a post-process Anim
 * Blueprint when the mesh has none. Lila's unrealMorphPaths calls it.
 */
void AddMorphPathMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
