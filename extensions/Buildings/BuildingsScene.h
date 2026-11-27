#pragma once

#include "Buildings.h"
#include "Interior.h"

#include <filesystem>
#include <functional>
#include <set>
#include <memory>
#include <string>
#include <vector>

namespace lila::ext
{
class Context;
}
namespace lila
{
struct Args;
}

/** The scene side of Buildings: the style library, the live node and what it published, commands and UI. */
namespace buildings
{
// Style library ---------------------------------------------------------------------------------------------------------

/** Shipped styles (resources/styles), then the user's (<config>/building-styles), then the workspace's (BuildingStyles); later ones of an id win.
 *  Opening another workspace reloads them on the next lookup. */
void loadStyles(const std::filesystem::path& shippedDir);
void reloadStyles();
std::shared_ptr<const Style> findStyle(const std::string& id);
std::vector<std::shared_ptr<const Style>> allStyles();
/** The shipped style closest to an id the library lacks, by the words of its id against shipped ids and tags
 *  ("colonialCentro" -> colonial, "malecon" -> seafront); residential when nothing matches. */
std::shared_ptr<const Style> nearestStyle(const std::string& id);
/** Bumped whenever a style is added, saved or reloaded. Any thread. */
uint64_t stylesRevision();
std::filesystem::path userStyleDir();
std::filesystem::path workspaceStyleDir();
/** Writes a style to the user's (scope "user") or workspace's folder and loads it. Returns the file. */
std::filesystem::path saveStyle(const Style& s, const std::string& scope);

// The live node ---------------------------------------------------------------------------------------------------------

/** The geometry node type and its group. */
inline constexpr const char* kNodeType = "Buildings";
inline constexpr const char* kGroupName = "LilaBuildings";
/** Mesh node string attribute marking a building mesh (its kind: "cell" or "standalone") and its settlement. */
inline constexpr const char* kMeshMark = "buildingsNode";
inline constexpr const char* kMeshSettlement = "buildingsSettlement";

/** What a Buildings node made on its last evaluation, kept per modifier for commands, checks and the paint tool. */
struct Published
{
    uint64_t modifier = 0;
    std::string cell, lod;
    std::vector<Spec> specs;
    std::vector<std::shared_ptr<const Result>> results;
    std::map<std::string, int> byId;
    std::vector<std::string> warnings;
    /** Each building's generation cache key (spec, style and edits), as results. */
    std::vector<uint64_t> keys;
    double ms = 0.0;      // last evaluation, milliseconds
    int generated = 0;    // buildings generated (cache misses) in it
    size_t slots = 0, faces = 0;
    /** The lod input is auto: what shows follows the camera (setViewLod). */
    bool autoLod = false;
    /** The buildings' footprints at their ground, world space. */
    Vec3 lo{1e300}, hi{-1e300};
};
std::shared_ptr<const Published> published(uint64_t modifier);
/** Every modifier's last evaluation; entries whose modifier left the scene (a scene replaced) are dropped. */
std::vector<std::shared_ptr<const Published>> allPublished();
/** allPublished into out, reusing its memory. */
void allPublished(std::vector<std::shared_ptr<const Published>>& out);
void forgetPublished(uint64_t modifier);

void registerNode(lila::ext::Context& ctx);
/**
 * The building cache beside a saved scene (<stem>_cache/buildings.bin): every generated building, written after a save
 * that changed buildings and read when the scene opens, so reopening skips the grammar for buildings whose spec, style
 * and edits are unchanged. A cache from another build of Buildings, or a damaged entry, is ignored (logged) and those
 * buildings regenerate.
 */
void registerCache(lila::ext::Context& ctx);
std::filesystem::path cacheFile(const std::filesystem::path& scene);
/** Writes the cache for scene now (the save hook writes it on a background thread). Returns buildings written, -1 when unchanged. */
long long writeCache(const std::filesystem::path& scene);
/** Reads the cache for scene into the generation cache. Returns buildings read. */
size_t readCache(const std::filesystem::path& scene);
/** What a modifier with lod auto shows (near, hlod or far); true when it changed, so the caller reruns it. */
bool setViewLod(uint64_t modifier, const std::string& lod);
/** Leaves one building's shell and modules out of its mesh (Edit Rooms' cutaway); empty for none. True when it changed. */
bool setCutawayBuilding(const std::string& id);
/** Drops cached generation results (after a style edit). */
void clearGenerationCache();

/** The material name a building face uses in the scene (palette-indexed render, trim, roof). */
std::string materialFor(const Result& r, int mat);
/** Every material name a style's palette can produce, with its linear colour, for creating the Lila materials. */
std::vector<std::pair<std::string, glm::vec3>> styleMaterials(const Style& s);

void registerCommands(lila::ext::Context& ctx);
void registerCheck(lila::ext::Context& ctx);
void registerUi(lila::ext::Context& ctx);
void registerPaintTool(lila::ext::Context& ctx);
/** The building the Attribute Editor section and the paint tool work on (set by picking one). */
std::string& currentBuilding();
/** Rebuilds open Attribute Editor building sections. */
void refreshSections();
void registerAgent(lila::ext::Context& ctx);
void registerUnreal(lila::ext::Context& ctx);
/** Switches lod auto building meshes between near, hlod and far by their distance from the viewport camera. */
void registerViewLod(lila::ext::Context& ctx);
/** Rebuilds a settlement's stale buildings after commands that edit it. */
void registerLiveRefresh(lila::ext::Context& ctx);
void unregisterLiveRefresh();

/** A bay under a ray: its building, path, kind, corners and the parts in it (window, balcony, awning, ac...). */
struct BayHit
{
    std::string building, name, bay, kind, mesh;
    bool balcony = false;
    Vec3 point{0.0};
    Vec3 corners[4];
    std::vector<std::string> parts;
};
/** The nearest bay facing a ray, from what the nodes last published (no evaluation). Main thread. */
std::optional<BayHit> pickBay(Vec3 origin, Vec3 dir);

/** The shipped LilaBuilding.lilatrim the shell's uv0 follows. */
void setShippedTrimSheet(const std::filesystem::path& p);

/** buildingCheck as a function: issues for the given modifiers (all when empty), interiors held to the profile (the scene's when null). */
nlohmann::json checkBuildings(const std::vector<uint64_t>& modifiers, double tolerance, const nlohmann::json& profile = nullptr);

// Shared by the command files ---------------------------------------------------------------------------------------------

struct BuildingLoc
{
    std::string mesh, modifier;
    std::shared_ptr<const Published> pub;
    int index = -1;
};
/** Where a building is (its mesh, modifier and what it published), after evaluating. */
std::optional<BuildingLoc> locateBuilding(const std::string& id);
/** A building's edits object. */
nlohmann::json buildingEditsOf(const std::string& id);
/** Changes buildings' edits, one setAttr per modifier. */
void changeBuildingEdits(const std::vector<std::string>& ids, const std::function<void(const std::string&, nlohmann::json&)>& change, const char* cmd);
/** Building ids from positional args, building=, polygon=, all= or the selection. */
std::vector<std::string> buildingTargets(const lila::Args& a, const char* cmd, bool allowEmpty = false);
void markNodeAttr(const std::string& node, const char* attr, const std::string& value);
std::string nodeAttrString(const std::string& plug);
std::string safeNodeName(const std::string& s);
/** The Lila materials building meshes use, for these styles and the shared module ones. Returns how many were made. */
int ensureBuildingMaterials(const std::set<std::string>& styleIds);

// Interiors (W4) ----------------------------------------------------------------------------------------------------------

/** Group attribute naming the building an interior belongs to; part meshes carry their part id and the hash they were made with. */
inline constexpr const char* kInteriorMark = "buildingInterior";
inline constexpr const char* kPartMark = "interiorPart";
inline constexpr const char* kPartHash = "interiorHash";

void registerInterior(lila::ext::Context& ctx);
void registerRoomTool(lila::ext::Context& ctx);
/** The scene's character profile (buildingCharacterProfile). */
CharacterProfile characterProfile();
/** Regenerates the interiors of these buildings (every enterable one when empty); hand-edited room meshes are kept. */
nlohmann::json refreshInteriors(const std::vector<std::string>& ids);
/** The interior a building has now, generated from what its node published. Null when it isn't enterable. Main thread. */
std::shared_ptr<const Interior> interiorOf(const std::string& id);
/** The interior's group node path, or empty. */
std::string interiorGroupOf(const std::string& id);
/** Ids of the buildings marked enterable. */
std::vector<std::string> enterableBuildings();
/** Interior issues for buildingCheck: unreachable rooms, doors with no opening in the room meshes, stairs, door sizes, no entry. */
nlohmann::json interiorIssues(const std::vector<std::string>& ids, const CharacterProfile& p);

/** A room under a ray: its building, room id, kind, floor and the point on its floor. */
struct RoomHit
{
    std::string building, room, kind;
    int floor = 0;
    Vec3 point{0.0};
    std::vector<std::array<Vec2, 4>> rects;
    double y = 0.0;
};
/** The room whose floor a ray meets first (floor >= 0 limits it to that floor). Main thread. */
std::optional<RoomHit> pickRoom(Vec3 origin, Vec3 dir, int floor = -1, const std::string& building = {});
} // namespace buildings
