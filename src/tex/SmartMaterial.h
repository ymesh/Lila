#pragma once

#include "core/Export.h"

#include "tex/Layers.h"

#include <filesystem>
#include <string>
#include <vector>

/**
 * Smart materials and smart masks: saved layers (usually a folder) and saved mask stacks, as `.lilasmart` JSON:
 *
 *   { "format": "lilasmart", "version": 1, "name": "Rusty Iron", "description": "...", "kind": "material" | "mask",
 *     "layer": {layer, as Layer::toJson},          // kind material
 *     "masks": [{mask, as Mask::toJson}, ...] }    // kind mask
 *
 * Their generators bind to mesh maps and anchors by name, so applying one to any texture set adapts to that set's bakes.
 * Ids in the file are only for references inside it (instance sources); applying gives everything fresh ids.
 *
 * Tiers, later winning on the same name: built-in (<resources>/smartmaterials), extension packs (<package>/resources/
 * smartmaterials of loaded extensions), the user's (<config>/smartmaterials), the workspace's (.lila/smartmaterials).
 */
namespace lila::tex
{
struct LILA_SCENE_API SmartMaterial
{
    std::string name;
    std::string description;
    /** material or mask. */
    std::string kind = "material";
    /** Lowercase words to filter by (metal, stone, stylized...): the file's, or the user's override of them. */
    std::vector<std::string> tags;
    json layer;
    json masks = json::array();
    std::filesystem::path file;
    std::string tier;

    json toJson() const;
    /** Throws std::invalid_argument. */
    static SmartMaterial fromJson(const json& j);
};

/** Built-in, extension pack, user and workspace folders (missing ones included). */
LILA_SCENE_API std::vector<std::filesystem::path> smartMaterialDirs();
/** Every smart material and mask, the winning one per name, by name. */
LILA_SCENE_API std::vector<SmartMaterial> listSmartMaterials();
/** By file path, or by name (file stem or its "name") across the tiers. Throws std::invalid_argument. */
LILA_SCENE_API SmartMaterial loadSmartMaterial(const std::string& nameOrPath);
/** Throws std::runtime_error when it can't write. */
LILA_SCENE_API void saveSmartMaterial(const SmartMaterial& m, const std::filesystem::path& path);

/**
 * Replaces a smart material's tags for this user, kept in <config>/smartmaterials/tags.json so built-in and extension ones
 * can be retagged too; tags equal to the file's drop the override. Tags are trimmed and lowercased. Throws std::runtime_error.
 */
LILA_SCENE_API void setSmartMaterialTags(const std::string& name, const std::vector<std::string>& tags);

/** Tags cleaned up: trimmed, lowercase, no empties or repeats, sorted. */
LILA_SCENE_API std::vector<std::string> normalizeTags(const std::vector<std::string>& tags);

/**
 * The layer ready to insert into stack: fresh layer and mask ids (instance sources follow), anchor names made unique in the
 * stack (masks and bindings reading them follow).
 */
LILA_SCENE_API Layer instantiateLayer(const json& layer, LayerStack& stack);
/** Masks ready to add to a layer of stack: fresh ids. */
LILA_SCENE_API std::vector<Mask> instantiateMasks(const json& masks, LayerStack& stack);
} // namespace lila::tex
