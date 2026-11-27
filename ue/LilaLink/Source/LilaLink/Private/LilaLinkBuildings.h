#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * buildingMaterials: MF_LilaBuildingVariation (the per-building palette Lila sends as instance colour on shared module
 * meshes, else writes into vertex colour, RGB the painted colour and A the dirt, laid over a trim sheet or any base colour) with M_LilaBuilding built on it, made when missing,
 * older or regenerate; and a material instance per entry of instances (one per building style).
 *
 * buildingInteriors {folder, glassAssets?: [mesh paths], interiors: [{building, name, mode: levelInstance | dataLayer,
 *   assembly?: the makeLevelInstance key, groups?: [placement group keys]}]}: the interiors of enterable buildings, streamed
 *   apart from their shells. levelInstance: the Level Instance makeLevelInstance placed for the key goes on a data layer
 *   DL_<name> too; dataLayer: the groups' actors go on DL_<name> directly. Interior meshes collide as their own triangles.
 *   On glassAssets, material slots named bld_glass get MI_Bld_InteriorCard (on M_LilaInterior: the fake room behind the
 *   windows of buildings without one) and bld_glassClear MI_Bld_GlassClear (translucent). Returns {interiors: [{building,
 *   dataLayer, actors}], glass: {assets, slots}, created, warnings}.
 */
void AddBuildingMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
