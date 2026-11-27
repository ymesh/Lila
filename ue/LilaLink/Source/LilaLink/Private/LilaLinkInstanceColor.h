#pragma once

#include "LilaLinkServer.h"

class UMaterialExpression;

namespace LilaLink
{
struct FFunctionGraph;

/**
 * The instance colour Lila sends as custom data 0-3 (linear RGB, amount), as a float4 in G: per-instance custom data on
 * ISM/HISM instances, else the primitive's custom data (actors), through the vector parameter LilaInstanceColor.
 * Zero where nothing was sent. With bSecondTone, the second tone in custom data 4-7 (parameter LilaInstanceColor2).
 *
 * Custom data layout Lila sends: 0-3 colour, 4-7 second tone (only when any instance of the group has one), then the
 * names declared with unrealCustomData.
 */
UMaterialExpression* InstanceColorData(FFunctionGraph& G, int32 X, bool bSecondTone = false);

/**
 * The colour, or the second tone where the scalar SecondTone is above 0.5: what a material slot of a two-tone piece
 * (trim, frames) reads. SecondTone null reads the colour.
 */
UMaterialExpression* InstanceToneData(FFunctionGraph& G, int32 X, UMaterialExpression* SecondTone);

/**
 * instanceColorMaterials {regenerate?, assets?: [static mesh paths]}: MF_LilaInstanceColor (the base colour replaced or
 * tinted by the instance colour) and M_LilaInstanceColor on it, built when missing, older or regenerate; every material
 * slot of the given meshes gets M_LilaInstanceColor. Returns {function, material, assigned, created, warnings}.
 */
void AddInstanceColorMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
