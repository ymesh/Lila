#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <string>

/**
 * Texture sets to Unreal: a set exports through its Unreal preset (incrementally, so only changed tiles and channels are
 * rewritten), then LilaLink's importTextures updates the textures and the material instance of the set's slot. Painterly and
 * Toon materials parent to LilaLink's stylised masters (lila:painterly, lila:toon) with their palette values; other shading
 * models use the materialParent pref, else send textures only.
 */
namespace lila::link
{
using json = nlohmann::json;

/**
 * importTextures params for a set's export result (tex::exportSet). material is the slot's materialInputs. options: folder,
 * materialParent, materialFolder, save, meshes ({Lila node: Unreal mesh}), slotMeshes (Lila nodes using the set's
 * materials) and materials ({material: null for every map, or the map names it takes}; without it, material's name gets
 * every map).
 * Unless all is set, only textures with a changed file (every tile of a UDIM set when one changed) go. warnings collects
 * what was skipped.
 */
LILA_SCENE_API json textureSetParams(const json& exportResult, const json& material, const json& options, bool all, json* warnings = nullptr);

/**
 * Exports and queues every set that goes to Unreal on save: those sent with unrealSendTextureSet (remembered in the "unreal"
 * pref /textureSets) and those whose slot a mapped mesh uses. Main thread; the sends run on the link worker. Returns how many
 * sets had something to send.
 */
LILA_SCENE_API int syncTextureSets();

/** Registers unrealSendTextureSet. */
LILA_SCENE_API void registerTextureSetCommands();
} // namespace lila::link
