#pragma once

#include <glm/glm.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * The prop library: folders of assets, searched together. Tiers, first wins on the same id:
 *
 *   workspace   <workspace>/Library/          checked in with the project
 *   studio      each folder in pref stagehand library.paths, then env LILA_LIBRARY_PATH (';' or ':' separated)
 *   user        <user prefs folder>/library/, beside the material library
 *   starter     <user prefs folder>/starter-library/: industrial, debris and facility props built from code on first
 *               use (pref stagehand library.starter turns it off)
 *
 * An asset is a folder <Library>/<Collection>/<Asset>/ holding asset.json, the model it names (.glb by default; .gltf,
 * .fbx, .obj and .lila load too) and thumb.png. Its id is "<Collection>/<Asset>".
 *
 *   asset.json: { "name", "model": "Crate.glb", "tags": [], "description", "size": [x, y, z] (cm, bounds),
 *                 "physics": { "role", "material", "shape" }, "unreal": "/Game/Props/SM_Crate", "snapPoints": [],
 *                 "assembly": { "kind", "members": [{ "asset", "matrix": [16, column-major, relative to the assembly] }] } }
 *
 * An assembly holds other assets (assemblies too) where they stood; its model, when it has one, is the rest of the group.
 */
namespace stagehand::library
{
using json = nlohmann::json;

struct Asset
{
    std::string id;
    std::string name;
    std::string collection;
    std::string tier;
    std::filesystem::path dir;
    std::filesystem::path model;
    std::filesystem::path thumbnail; // empty when there is none
    std::vector<std::string> tags;
    std::string description;
    glm::dvec3 size{0.0};
    json physics = json::object();
    std::string unreal;
    json snapPoints = json::array();
    /** Null unless the asset is an assembly: {kind, members: [{asset, matrix}]}. */
    json assembly;

    json toJson() const;
};

struct Root
{
    std::string tier;
    std::filesystem::path dir;
};

/** Where assets are looked for, in the order they win. Folders that don't exist are left out. */
std::vector<Root> roots(bool existingOnly = true);
/** The folder of a writable tier: "workspace" (needs a workspace open), "user", or a studio folder by path. */
std::filesystem::path tierDir(const std::string& tier);
/**
 * Library folders another extension adds for this session (Weathering's placeholder pieces), searched after every other
 * tier under their own tier name. Adding the same folder again does nothing; remove takes it out.
 */
void addExtensionRoot(const std::string& tier, const std::filesystem::path& dir);
void removeExtensionRoot(const std::filesystem::path& dir);
/** Where the starter props are built, and whether they are in use. */
std::filesystem::path starterDir();
bool starterEnabled();
/** Builds the starter props unless they are there at the current version (or force). Returns dir, built and assets (ids). */
json buildStarter(bool force);
/** A path from UTF-8 text. */
std::filesystem::path utf8Path(std::string_view utf8);

/** Every asset, the winner per id, by collection then name. Scans on first use. */
const std::vector<Asset>& all();
std::optional<Asset> find(const std::string& id);
void reload();
/** Folders that failed to load on the last scan, "path: why". */
const std::vector<std::string>& errors();

struct Query
{
    /** Words matched against name, id, collection, tags and description: every word must match, typos within one edit allowed. */
    std::string text;
    std::vector<std::string> tags;
    std::string collection;
    std::string tier;
    /** Largest bounds side allowed, cm; 0 for any. */
    double maxSize = 0.0;
    double minSize = 0.0;
    size_t limit = 0;
};
/** Best match first. */
std::vector<Asset> search(const Query& q);
std::vector<std::string> collections();
std::vector<std::string> tags();

/** How well a word matches a field, 0 for not at all: exact, prefix, substring, then within one edit. */
double wordScore(const std::string& word, const std::string& field);
} // namespace stagehand::library
