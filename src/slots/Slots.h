#pragma once

#include "core/Export.h"
#include "core/Registry.h"
#include "scene/Plug.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lila::ext
{
class Context;
}
namespace lila::geo
{
class Mesh;
}
namespace lila::scene
{
class Graph;
}

/**
 * Slots: typed placed parts (a door, a sign, a lamp, a palm, a crop row) with stable ids that generators publish and the
 * user swaps without touching the generator. A slot's payload comes from its generator, then swap rules in order (global,
 * region, owner), then the scene's override for that slot id, which survives regeneration. Pins freeze a slot, or every
 * slot of an owner, where it is.
 *
 * Generators publish slots two ways:
 *   - a command writes a slotSet node (writeSet), which keeps them in the scene file and draws them instanced;
 *   - a geometry-nodes graph runs applyToInstances (or the Slots node), which swaps each instance's reference to its
 *     resolved payload and publishes the slots under the modifier.
 *
 * Rules, overrides and pins are stored in the scene (scene::data keys "slots.rules", "slots.overrides", "slots.pins");
 * every edit through this API records undo.
 */
namespace lila::slots
{
using json = nlohmann::json;
using scene::NodeId;

// Types

/** Where a slot's frame sits on what it is attached to. Origin is always the bottom centre of the footprint. */
enum class Attach : uint8_t
{
    Ground,  // +Y up, +Z the front (a lamp's arm, a sign's face)
    Wall,    // on a facade: +Z out of the wall, +Y up
    Ceiling, // hangs: +Y down into the room
    Roof,    // on a roof plane, +Y along its normal
    Free,    // anywhere, frame as given
};
LILA_SCENE_API const char* attachName(Attach a);
LILA_SCENE_API std::optional<Attach> parseAttach(std::string_view name);

/** The procedural stand-in shapes slots draw with until a real payload is assigned. */
LILA_SCENE_API const std::vector<std::string>& placeholderShapes();

struct LILA_SCENE_API SlotType
{
    std::string id; // dotted, most general first: "door.shop"
    std::string label;
    std::vector<std::string> tags;
    /** Bounds the part is expected to fill, cm: width (x), height (y), depth (z). */
    glm::dvec3 footprint{100.0};
    Attach attach = Attach::Ground;
    /** One of placeholderShapes(). */
    std::string placeholder = "box";
    std::string doc;

    json toJson() const;
};

/** Every slot type, owner-tagged like commands. */
LILA_SCENE_API Registry<SlotType>& types();
/** Adds or replaces a type (Lila's own). */
LILA_SCENE_API void registerType(SlotType t);
/** For extensions: owned by ctx and removed when it unloads. False, with a log message, if the id is taken. */
LILA_SCENE_API bool registerType(ext::Context& ctx, SlotType t);
/** The type itself, or its nearest registered dotted ancestor ("door.shop.double" -> "door.shop" -> "door"). Null when none. */
LILA_SCENE_API const SlotType* resolveType(std::string_view id);
/**
 * Whether a rule's type pattern covers a slot type: "*" covers everything, "door" covers "door" and "door.shop",
 * "door.*" only the children, and a comma list any of its entries.
 */
LILA_SCENE_API bool typeMatches(std::string_view pattern, std::string_view type);

// Payload

/** What fills a slot. Empty fields are unset: overlaying a payload replaces only the fields it sets. */
struct LILA_SCENE_API Payload
{
    std::string item;              // library item id (Stagehand: "Collection/Asset")
    std::vector<std::string> pool; // variant pool: item ids, one picked per slot by its id
    std::string poolTag;           // variant pool from library tags ("fictional_signs", "a+b" for both)
    std::string mesh;              // a scene mesh node (path) drawn and exported as is
    std::string placeholder;       // one of placeholderShapes(), sized by the type's footprint
    std::string unrealMesh;        // Unreal static mesh path for HISM ("/Game/Props/SM_Door")
    std::string unrealClass;       // Unreal actor class, spawned per slot ("/Game/BP/BP_Door.BP_Door_C")
    std::optional<bool> hidden;    // true removes the part
    json data = json::object();    // gameplay key/values (locked, shopType, owner, missionId); merged key by key

    bool empty() const;
    json toJson() const;
    static Payload fromJson(const json& j);
    /** Fields `over` sets replace these; data merges, a null value removing the key. */
    void overlay(const Payload& over);
    /** A one-line summary: "item Street/Lamp", "pool tag fictional_signs", "class /Game/BP_Door". */
    std::string describe() const;
};

// Slots

struct LILA_SCENE_API Slot
{
    /** Stable: makeId(owner, path). */
    std::string id;
    std::string type;
    /** World matrix of the attach frame (scale included). */
    glm::dmat4 transform{1.0};
    /** The owner's stable id: a building, lot, street, arrangement or terrain. */
    std::string owner;
    /** Regions the slot lies in, outermost first ("settlement:Town", "district:3"), for region rules and slotFind. */
    std::vector<std::string> regions;
    std::vector<std::string> tags;
    /** What the generator put there. */
    Payload payload;

    json toJson() const;
    static Slot fromJson(const json& j);
};

/** owner + "/" + path, each part trimmed of slashes at its ends. */
LILA_SCENE_API std::string makeId(std::string_view owner, std::string_view path);
/** "lot/1a2b/f0/b3/door": path parts joined with "/", numbers as decimal and 64-bit keys as hex. */
LILA_SCENE_API std::string pathOf(std::initializer_list<std::string_view> parts);
LILA_SCENE_API std::string hex(uint64_t key);
/** FNV-1a 64 of an id: what Unreal custom data and per-instance ids carry. */
LILA_SCENE_API uint64_t idHash(std::string_view id);

// Rules, overrides, pins

enum class Level : uint8_t
{
    Global,
    Region, // a settlement, district or any region a slot lists
    Owner,  // a building, lot, street or arrangement
};
LILA_SCENE_API const char* levelName(Level l);
LILA_SCENE_API std::optional<Level> parseLevel(std::string_view name);

/** A swap rule: slots it covers take its payload over what they had. Applied by level, then in list order. */
struct LILA_SCENE_API Rule
{
    std::string id; // unique in the scene, "r1" if not given
    std::string name;
    Level level = Level::Global;
    std::string type = "*"; // typeMatches pattern
    /** Level Region: a region the slot lists (prefix match, "district:" for every district). */
    std::string region;
    /** Level Owner: the owner id (prefix match, "lot/1a2b" covers "lot/1a2b/f0"). */
    std::string owner;
    /** The slot must carry every one. */
    std::vector<std::string> tags;
    Payload payload;
    bool enabled = true;

    bool covers(const Slot& s) const;
    json toJson() const;
    static Rule fromJson(const json& j);
};

/** A per-slot override, kept by slot id with what the slot was when it was made, so an orphan can be found again. */
struct LILA_SCENE_API Override
{
    Payload payload;
    std::string type;
    glm::dvec3 at{0.0};
    std::string owner;

    json toJson() const;
    static Override fromJson(const json& j);
};

/** A frozen slot: where it was and what filled it when pinned. */
struct PinnedSlot
{
    Slot slot; // payload resolved at pin time, or as generated when swappable
    /** Rules and overrides still apply over the generated payload; only regeneration is held off. */
    bool swappable = false;
};
/** A pin on one slot (key = its id) or on every slot of an owner (key = "owner:" + owner id). */
struct LILA_SCENE_API Pin
{
    std::string key;
    std::vector<PinnedSlot> slots;
    json toJson() const;
    static Pin fromJson(const json& j);
};

/** Rules, overrides and pins as the scene holds them; immutable, swapped whole on every edit. */
struct State
{
    std::vector<Rule> rules;
    std::map<std::string, Override> overrides;
    std::map<std::string, Pin> pins;
    uint64_t revision = 0;
};
using StatePtr = std::shared_ptr<const State>;

/** The current state. Any thread. */
LILA_SCENE_API StatePtr state();
/** Re-reads the scene's data, after a load or an edit that bypassed this API. Main thread. */
LILA_SCENE_API void refresh();
/** Bumped by every change to rules, overrides, pins or published slots. */
LILA_SCENE_API uint64_t revision();

/** Adds the rule (id assigned when empty, or replaced when taken). Undoable. Returns its id. */
LILA_SCENE_API std::string addRule(Rule r, int index = -1);
/** False when there is no such rule. Undoable. */
LILA_SCENE_API bool removeRule(std::string_view id);
/** Moves a rule to index in the list. Undoable. */
LILA_SCENE_API bool moveRule(std::string_view id, int index);
LILA_SCENE_API void setRules(std::vector<Rule> rules);

/** Overlays payload onto the slot's override (made if missing); clear drops it first. Undoable. */
LILA_SCENE_API void setOverride(const std::string& id, const Payload& payload, bool clear = false);
LILA_SCENE_API bool clearOverride(const std::string& id);
/** Moves an override from one id to another (an orphan to the slot that replaced it). Undoable. */
LILA_SCENE_API bool reassignOverride(const std::string& from, const std::string& to);

/**
 * Pins a slot (by id) or every slot of an owner ("owner:" + id) as they resolve now; swappable keeps their frames and
 * generated payloads but lets rules and overrides swap them. Undoable. False when nothing matched.
 */
LILA_SCENE_API bool pin(scene::Graph& g, const std::string& key, bool swappable = false);
LILA_SCENE_API bool unpin(const std::string& key);
/** Whether a slot id, or an owner, is pinned (directly or through its owner). */
LILA_SCENE_API bool isPinned(std::string_view key);

// Resolving

struct Resolved
{
    Payload payload;
    /** What set it, in order: "default", "rule:<id>", "override", "pin". */
    std::vector<std::string> sources;
    /** The pool entry chosen, when a pool decided the item. */
    std::string poolPick;
    bool pinned = false;
};

/** Pool tag -> item ids, from whoever owns the library (Stagehand registers "item"). */
using TagResolver = std::function<std::vector<std::string>(const std::string& tags)>;
LILA_SCENE_API Registry<TagResolver>& tagResolvers();

/** The slot's payload after rules and its override, its pool resolved by its id. Any thread. */
LILA_SCENE_API Resolved resolve(const Slot& s, const State& st);
LILA_SCENE_API Resolved resolve(const Slot& s);

// Publishing

/** Every slot in the scene: slotSet nodes, geometry-node publishers, and pinned slots no generator makes any more. Main thread. */
LILA_SCENE_API std::vector<Slot> collect(scene::Graph& g);
/** Only the slots `keep` takes (asked of each slot as generated, before its node's transform), without copying the rest. */
LILA_SCENE_API std::vector<Slot> collect(scene::Graph& g, const std::function<bool(const Slot&)>& keep);
/** The slot by id, or nothing. Main thread. */
LILA_SCENE_API std::optional<Slot> findSlot(scene::Graph& g, std::string_view id);
/** The node publishing a slot: its slotSet or the modifier's mesh. kNoNode for pinned orphans. */
LILA_SCENE_API NodeId publisherOf(scene::Graph& g, std::string_view id);

/** Publishes slots under a node (a geometry-nodes modifier's id), replacing what it published before. Any thread. */
LILA_SCENE_API void publish(NodeId publisher, std::vector<Slot> slots);
LILA_SCENE_API void unpublish(NodeId publisher);
/** Drops what publishers no longer in g published: a replaced scene's (clearing a graph deletes no node one by one). */
LILA_SCENE_API void unpublishGone(const scene::Graph& g);
/** What a publisher published, in its own space, unpinned: the list the "slotIndex" instance attribute indexes. Any thread. */
LILA_SCENE_API std::vector<Slot> publishedSlots(NodeId publisher);
/** A slotSet's or a publisher's slots in world space with pins applied (pinned slots it no longer makes included). Main thread. */
LILA_SCENE_API std::vector<Slot> slotsOf(scene::Graph& g, NodeId node);

/** Called on the main thread after rules, overrides or pins change (edits, undo, scene loads). Returns an id. */
LILA_SCENE_API int addListener(std::function<void()> fn);
LILA_SCENE_API void removeListener(int id);

/** The slotSet node type: a DAG node holding slots in its local space and drawing their resolved payloads instanced. */
inline constexpr const char* kSetType = "slotSet";
/**
 * Writes slots (transforms in world space) into the slotSet named `name` under parent, made when missing. Undoable.
 * They are stored as generated; pinned ones read back with their pinned frame and payload. A pinned slot whose generated
 * frame or type differs, or a new slot under a pinned owner, is a conflict in the returned stats
 * ({"set", "written", "pinned", "conflicts": [ids]}).
 */
LILA_SCENE_API json writeSet(scene::Graph& g, const std::string& name, NodeId parent, const std::vector<Slot>& slots,
                             const std::string& publisherDoc = {});
/** The slots a slotSet node holds, world space. */
LILA_SCENE_API std::vector<Slot> readSet(scene::Graph& g, NodeId set);

// Visuals

/** A resolved payload as geometry: the mesh to draw (local to the slot frame) and where it came from. */
struct Visual
{
    std::string key; // "item:Street/Lamp", "mesh:|lamp", "placeholder:pole/100x400x100"
    std::shared_ptr<const geo::Mesh> mesh;
    /** Set when the payload asked for something that couldn't be had, and the placeholder stands in. */
    std::string missing;
};

/**
 * Turns a payload reference into a mesh: registered per scheme ("item" by Stagehand, "mesh" by Lila). load is false
 * from evaluation hooks, where a provider must not change the scene. Main thread. Null with why set when it can't.
 */
using MeshProvider = std::function<std::shared_ptr<const geo::Mesh>(scene::Graph& g, const std::string& ref, bool load, std::string& why)>;
LILA_SCENE_API Registry<MeshProvider>& meshProviders();

/** The visual for a resolved payload on a slot type. Cached by key. Main thread. */
LILA_SCENE_API Visual visualFor(scene::Graph& g, const Resolved& r, const std::string& type, bool load = true);
/** The cached visual for a key, any thread; null when not made yet. */
LILA_SCENE_API std::shared_ptr<const geo::Mesh> cachedVisual(const std::string& key);
/** The key visualFor would use. Any thread. */
LILA_SCENE_API std::string visualKey(const Resolved& r, const std::string& type);
/** A shipped placeholder shape sized to a footprint, base at the origin, front +Z. */
LILA_SCENE_API std::shared_ptr<const geo::Mesh> placeholderMesh(const std::string& shape, const glm::dvec3& footprint);
/** A node's meshes (its own and its descendants'), in its space, as one mesh; null when it has none. For providers. */
LILA_SCENE_API std::shared_ptr<const geo::Mesh> combinedMesh(scene::Graph& g, NodeId root);
/** Drops cached visuals (after a library reload). */
LILA_SCENE_API void clearVisuals();

/**
 * A payload's colour from its data key "color" ("color2" for the second tone of a two-tone piece): "#rrggbb" (sRGB), or [r, g, b] / [r, g, b, a] linear with a how much it
 * shows. Linear rgb and amount; nothing when there is none or it doesn't parse. Drawn per instance and sent to Unreal as
 * per-instance custom data.
 */
LILA_SCENE_API std::optional<glm::vec4> dataColor(const json& data, const char* key = "color");

/** Per mesh, the instance matrices (world) a slotSet's resolved slots draw with, hidden ones left out. Main thread. */
struct DrawBatch
{
    std::shared_ptr<const geo::Mesh> mesh;
    std::string key;
    std::vector<glm::mat4> transforms;
    /** Per transform, dataColor of the resolved payload (zero for none); empty when no slot of the batch has one. */
    std::vector<glm::vec4> colors;
};
LILA_SCENE_API std::vector<DrawBatch> drawBatches(scene::Graph& g, NodeId set);

// Report

/**
 * Counts per type and per rule, orphaned overrides (ids no slot has), pin conflicts, duplicate ids, unregistered types
 * and visuals that couldn't load. Also what the Validation panel shows. Main thread.
 */
LILA_SCENE_API json report(scene::Graph& g);

/** Types, the slotSet node, the Slots geometry node, providers, commands and the validator. */
LILA_SCENE_API void registerAll();
} // namespace lila::slots
