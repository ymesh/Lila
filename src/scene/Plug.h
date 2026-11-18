#pragma once

#include "core/Export.h"
#include "core/Registry.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <functional>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace lila::geo
{
class Mesh;
}

namespace lila::ext
{
class Context;
}

namespace lila::scene
{
using json = nlohmann::json;
/** Meshes flow through the graph as shared immutable snapshots; an edit makes a new one. */
using MeshPtr = std::shared_ptr<const geo::Mesh>;

/**
 * Any other immutable payload a plug carries (skin weights, influence palettes), shared like meshes. A kind registered
 * with data::registerKind can be stored; toJson must write a "kind" key holding that name.
 */
struct Data
{
    virtual ~Data() = default;
    virtual const char* kind() const = 0;
    virtual nlohmann::json toJson() const = 0;
    /** Approximate heap bytes, for the undo memory budget. */
    virtual size_t bytes() const { return 256; }
};
using DataPtr = std::shared_ptr<const Data>;
using NodeId = uint64_t;
constexpr NodeId kNoNode = 0;

enum class PlugType : uint8_t
{
    Bool,
    Int,
    Float,
    Vec3,
    Matrix,
    String,
    Enum, // stored as Int
    Mesh,
    Data,
};

using PlugValue = std::variant<bool, int64_t, double, glm::dvec3, glm::dmat4, std::string, MeshPtr, DataPtr>;

enum AttrFlags : uint32_t
{
    AttrInput = 1 << 0,
    AttrOutput = 1 << 1,
    AttrKeyable = 1 << 2,
    AttrStorable = 1 << 3,
    AttrHidden = 1 << 4,
    AttrFile = 1 << 5, // String holding a file path: editors offer a file browser
    AttrResetOnDisconnect = 1 << 6, // only meaningful while driven: losing the connection puts the default back
    AttrColor = 1 << 7,             // Vec3 holding a linear RGB colour: editors show a swatch
    AttrUnique = 1 << 8,            // an identity: duplicates don't inherit it
    AttrArray = 1 << 9,             // any number of elements (node.attr[3]); with AttrStorable they hold values, else only connections
    AttrUser = 1 << 10,             // added to one node by addAttr rather than declared by its type
    AttrChannelBox = 1 << 11,       // shown in the channel box although not keyable
    AttrMultiline = 1 << 12,        // String of several lines: editors offer a text box
    AttrFont = 1 << 13,             // String naming a font (family and style, or a file): editors offer font pickers
    AttrCode = 1 << 14,             // String of source code: editors offer a monospace box that applies on Ctrl+Enter
    AttrImage = 1 << 15,            // with AttrFile, an image: editors offer thumbnails and the image library tied to the attr
    AttrCurve = 1 << 16,            // String of curve points "x y, x y, ..." in 0..1: editors offer a curve editor
};

struct AttrDef
{
    std::string name;
    std::string shortName;
    PlugType type = PlugType::Float;
    PlugValue defaultValue = 0.0;
    uint32_t flags = AttrInput | AttrStorable;
    std::vector<std::string> enumNames;      // Enum only
    /** Enum only: each name's own doc, by index, shown when hovering that choice; a missing one comes from doc. */
    std::vector<std::string> enumDocs;
    std::array<std::string, 3> childNames;   // Vec3: long names, e.g. translateX
    std::array<std::string, 3> childShort;   // Vec3: tx, ty, tz
    std::string doc;
    /**
     * What a scene file means by leaving this attr out, when that differs from defaultValue: files omit values equal
     * to the default, so a default that changes keeps its old value here and older files load unchanged.
     */
    std::optional<PlugValue> fileDefault;
    /**
     * An older name scene files may store this attr under. Only loading reads it. On an array, the numbered attrs it
     * replaces, # standing for the number: "section#" (or just "section") reads section0, section1 ... as elements 0, 1 ...,
     * "target#Weight" reads target0Weight ...
     */
    std::string fileAlias;
    /** Editor hints. niceName replaces the label made from the name; the limits bound sliders and typed values. */
    std::string niceName;
    std::optional<double> minValue;
    std::optional<double> maxValue;
    std::optional<double> softMin;
    std::optional<double> softMax;
    std::optional<double> step;
    /** Groups attrs under a heading in editors; empty is the type's own section. */
    std::string category;

    /** The value scene files omit and assume when the attr is absent. */
    const PlugValue& storedDefault() const { return fileDefault ? *fileDefault : defaultValue; }
};

/**
 * node.attr, optionally one component of a Vec3 (node.tx) or one element of an array (node.attr[3]). An attr named
 * "group.child" is a child of a grouped array: its elements read as node.group[3].child, matched by index with the group's
 * other children.
 */
struct PlugRef
{
    NodeId node = kNoNode;
    int attr = -1;
    int component = -1;
    int element = -1;

    bool valid() const { return node != kNoNode && attr >= 0; }
    bool operator==(const PlugRef&) const = default;
    auto operator<=>(const PlugRef&) const = default;
};

LILA_SCENE_API json toJson(const PlugValue& v);
/** Converts j to the attr's type, or throws std::invalid_argument. */
LILA_SCENE_API PlugValue fromJson(const AttrDef& def, const json& j);
LILA_SCENE_API PlugValue defaultFor(PlugType type);
LILA_SCENE_API const char* typeName(PlugType type);
/** The type typeName names. Throws std::invalid_argument for anything else. */
LILA_SCENE_API PlugType typeNamed(std::string_view name);
/** A user attr's definition as scene files store it: name, type, flags and the editor hints set on it. */
LILA_SCENE_API json attrDefToJson(const AttrDef& def);
/** attrDefToJson read back. Throws std::invalid_argument for a bad type or default. */
LILA_SCENE_API AttrDef attrDefFromJson(const json& j);

/**
 * toJson of each value, in order; meshes and data convert in parallel. packed writes meshes with meshToJsonPacked, for a
 * document headed straight into a binary scene file.
 */
LILA_SCENE_API std::vector<json> toJsonAll(const std::vector<const PlugValue*>& values, bool packed = false);
struct FromJsonResult
{
    PlugValue value;
    std::string error; // fromJson's message when it threw; value is then unset
};
/** fromJson of each (attr, json), in order; meshes decode in parallel. */
LILA_SCENE_API std::vector<FromJsonResult> fromJsonAll(const std::vector<std::pair<const AttrDef*, const json*>>& items);

namespace data
{
using FromJson = std::function<DataPtr(const json&)>;
/** Every Data kind scene files can load, owner-tagged. */
LILA_SCENE_API Registry<FromJson>& kinds();
/** Lets a Data kind load from scene files. Registering a kind again replaces its loader. */
LILA_SCENE_API void registerKind(const std::string& kind, FromJson fromJson);
/**
 * For extensions: owned by ctx and removed when it unloads. False, with a log message, if the kind is taken. Values
 * already in the scene keep working while the extension is loaded; files holding the kind then fail to load it.
 */
LILA_SCENE_API bool registerKind(ext::Context& ctx, const std::string& kind, FromJson fromJson);
} // namespace data
} // namespace lila::scene
