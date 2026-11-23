#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * setSky {atmosphere?, heightFog?} sets the open level's sky from Lila's:
 *   - atmosphere: a SkyAtmosphere's settings under its property names (made when the level has none), and a SkyLight
 *     capturing the scene in real time so the sky lights it;
 *   - heightFog: an ExponentialHeightFog's density, falloff, height, colour and directional inscattering, with volumetric fog.
 * One undo transaction. Returns {skyAtmosphere, skyLight, heightFog}: the actor labels set, or empty.
 *
 * placeVolume {file, folder, name, label, centre, axes, resolution, densityScale, scatterColor, absorptionColor, emissionColor}
 * imports an OpenVDB file as a Sparse Volume Texture, makes MF_LilaVolume and M_LilaVolume when missing, a material instance
 * of it for this volume, and shows it on a HeterogeneousVolume labelled label (reused when there is one). centre and axes are
 * the centre voxel and the voxel axes in Unreal space. Returns {texture, material, actor}.
 *
 * volumeMaterials {regenerate?}: MF_LilaVolume (density and colours to albedo, extinction and emissive) and M_LilaVolume,
 * the Volume-domain material on it that samples a Sparse Volume Texture. Returns {function, material, created, warnings}.
 */
void AddSkyMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
