#pragma once

#include "LilaLinkServer.h"

class UMaterial;

namespace LilaLink
{
/**
 * Materials for Lila's trim sheets: one master, M_LilaTrim in the masters folder, built the first time it is asked for and
 * then left to artists (regenerate rebuilds its graph in place, keeping references), and an instance per sheet.
 *
 * The master reads the sheet's baked maps (normal, AO, curvature, cavity, height) through uv0 and turns them into paint:
 * worn edges from curvature, dirt in cavities, a tiling detail layer whose scale follows the sheet's world size, a second
 * material blended in by vertex colour red, and optional parallax from the height map. UsePaintedMaps swaps the procedural
 * paint for the sheet's textured maps from Lila (base colour, ORM). Detail, VertexBlend, Parallax and UsePaintedMaps are
 * static switches, so an instance that leaves one off compiles none of it. The graph is MF_LilaTrim, beside the master,
 * which users' own materials can call too (outputs BaseColor, Roughness, Metallic, AmbientOcclusion, tangent-space Normal);
 * its inputs default to the parameters the instances set.
 *
 * trimMaterials {folder, regenerate?, meshes: [static mesh paths], instances: [{name, slot, textures: {param: texture asset
 *   path}, scalars: {param: value}, vectors: {param: [r, g, b]}, switches: {param: bool}}]}
 * makes or updates each instance (one per sheet palette) in folder under the master and sets it on every slot of the meshes
 * named its slot. Returns {master, masterVersion, built, instances, assigned: [{mesh, slot, material, changed}], created,
 * warnings}; a mesh that isn't there is a warning "no static mesh <path>".
 *
 * trimDecals {folder, regenerate?, decals: [{id, name, instance: {name, textures, scalars, vectors}, location, forward, up,
 *   size: [depth, width, height], sortOrder?}]}
 * builds MF_LilaDecal and the deferred decal material M_LilaDecal on it (in the masters folder, like the trim master), makes
 * or updates each decal's instance in folder and places a decal actor per entry in the open level (forward is the projection
 * direction, up the decal's top, both in Unreal space), tagged with its id so a resend updates it. meshes: [{mesh: static
 * mesh path, slot, instance}] puts a decal instance on that slot of each mesh, so it renders as a mesh decal (edge decal
 * strips). Returns {master, built, spawned, updated, decals: [{id, actor, material}], assigned: [{mesh, slot, material,
 * changed}], created, warnings}.
 */
void AddTrimMethods(TMap<FString, FLilaLinkMethod>& Methods);

/** Builds MF_LilaTrim and M_LilaTrim on it when either is missing or bRegenerate. Null with OutWarnings on failure. */
UMaterial* TrimMaster(bool bRegenerate, bool& bOutBuilt, int32& OutVersion, TArray<FString>& OutCreated, TArray<FString>& OutWarnings);
} // namespace LilaLink
