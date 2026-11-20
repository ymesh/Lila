#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * grungeMaterials: MF_LilaGrunge (Lila's live grunge masks read from vertex colour and UV sets, styled per mask with world
 * noise, no UVs needed) with M_LilaGrunge (vertex colour, UV 3, edge distances in UV 4-5) and M_LilaGrungeUV (UV 3 to 7), built when created,
 * older or regenerate; and a material instance per entry of instances, parented to the one its layout needs.
 */
void AddGrungeMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
