#pragma once

#include "LilaLinkMethods.h"

class UMaterialFunction;

namespace LilaLink
{
/**
 * importGroom: a groom Alembic (Lila's exportGroom, groom_* attributes and guides) imported or reimported as a groom asset,
 * with MF_LilaHair (hair colour from the groom, root darkening, roughness, scatter, specular, the strand tangent) and
 * M_LilaHair on it as the default material; the function is for users' own hair materials.
 */
void AddGroomMethods(TMap<FString, FLilaLinkMethod>& Methods);

/** MF_LilaHair, built or upgraded when needed. */
UMaterialFunction* HairFunction(bool bRebuild, TArray<FString>& OutCreated);
} // namespace LilaLink
