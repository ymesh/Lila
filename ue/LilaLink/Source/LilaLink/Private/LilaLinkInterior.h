#pragma once

#include "LilaLinkServer.h"

class UMaterial;

namespace LilaLink
{
/**
 * Fake window interiors for Lila's Weathering windows. MF_LilaInterior in the masters folder: a room drawn from one image
 * (a one-point perspective photo or painting of a room, its back wall in the middle) seen through a flat card with
 * parallax, so the room has depth from every angle. Inputs InteriorRoomMap, InteriorDepth (0 to 0.95, the back wall's share
 * of the image is 1 minus it), InteriorTiling (rooms across the card), InteriorTint, InteriorEmissive and UVs; outputs
 * BaseColor, Emissive, Roughness and RoomUV. Users drop it into their own materials; M_LilaInterior is the generated
 * material on it.
 *
 * weatheringInteriors {folder, regenerate?, interiors: [{name, textures: {param: texture asset path}, scalars: {param:
 *   value}, vectors: {param: [r, g, b]}}]}
 * builds MF_LilaInterior and M_LilaInterior when missing (or regenerate), then makes or updates each instance in folder.
 * Returns {master, built, instances: [path], created, warnings}.
 */
void AddInteriorMethods(TMap<FString, FLilaLinkMethod>& Methods);

/** Builds MF_LilaInterior and M_LilaInterior on it when either is missing or bRegenerate. Null with OutWarnings on failure. */
UMaterial* InteriorMaster(bool bRegenerate, bool& bOutBuilt, TArray<FString>& OutCreated, TArray<FString>& OutWarnings);
} // namespace LilaLink
