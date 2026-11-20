#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * groundTintMaterials: MF_LilaGroundTint (the tint and wetness Lila's Ground Tint modifier wrote into vertex colour R and G,
 * darkening and wetting any ground material) and M_LilaGroundTint built on it, made when missing, older or regenerate.
 */
void AddWeatheringGroundMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
