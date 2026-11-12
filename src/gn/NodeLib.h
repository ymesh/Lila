#pragma once

#include "core/Export.h"
#include "core/Registry.h"

#include "gn/Field.h"
#include "gn/Group.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lila::ext
{
class Context;
}

namespace lila::gn
{
/** What an unconnected field input reads instead of its value. */
enum class Implicit : uint8_t
{
    None,
    Position,
    Normal,
    Index,
    Id,
};

struct SocketDecl
{
    std::string id;
    SocketType type = SocketType::Float;
    json defaultValue;
    /** Inputs: accepts a field. Outputs: produces one (drawn as a diamond). */
    bool field = false;
    /** Takes any number of links (Join Geometry). */
    bool multi = false;
    Implicit implicit = Implicit::None;
    std::string doc;
    /** Bounds of an unlinked Int or Float input's value; editors and gnSetValue hold it inside them. */
    std::optional<double> min, max;
    /** A String's editor in the Node Editor: "multiline" (a box of several lines), "code" (monospace, applied on Ctrl+Enter) or
     * "font" (font pickers); empty is one line. */
    std::string editor;
};

/** A non-linkable choice on a node, such as the Math operation. Free text when options is empty. */
struct PropDecl
{
    std::string id;
    std::vector<std::string> options;
    std::string defaultValue;
    std::string doc;
    /** A free-text property's editor, as SocketDecl::editor. */
    std::string editor;
};

struct LILA_SCENE_API Sockets
{
    std::vector<SocketDecl> inputs;
    std::vector<SocketDecl> outputs;

    int inputIndex(std::string_view id) const;
    int outputIndex(std::string_view id) const;
};

class ExecContext;
struct StatefulDecl;

/** A button in the Node Editor's side panel: runs gn<verb>(group, node). */
struct NodeAction
{
    std::string label;
    std::string verb;
    std::string doc;
};
struct StepCache;

struct LILA_SCENE_API NodeDecl
{
    std::string type;
    std::string label;
    std::string category;
    std::string doc;
    std::vector<PropDecl> props;
    Sockets sockets;
    /** For nodes whose sockets follow a property or the group interface. */
    std::function<Sockets(const Group&, const GNode&)> dynamicSockets;
    std::function<void(ExecContext&)> exec;
    /** Set for simulation nodes (gn/Stateful.h), which run through it instead of exec. */
    std::shared_ptr<const StatefulDecl> stateful;
    /** Made by evaluation only (zones lower to these), so the node library and menus leave it out. */
    bool hidden = false;
    std::vector<NodeAction> actions;

    Sockets socketsFor(const Group& g, const GNode& n) const;
    const PropDecl* prop(std::string_view id) const;
    /** The node's value of a property, or its default. */
    std::string propValue(const GNode& n, std::string_view id) const;
};

/** Every geometry node type, owner-tagged. */
LILA_SCENE_API Registry<NodeDecl>& declRegistry();
/** Adds, or replaces the type's declaration. */
LILA_SCENE_API void registerDecl(NodeDecl d);
/** For extensions: owned by ctx and removed when it unloads. Null, with a log message, if the type is taken. */
LILA_SCENE_API const NodeDecl* registerDecl(ext::Context& ctx, NodeDecl d);
/** Groups using the type then fail to evaluate with "unknown node type". */
LILA_SCENE_API void unregisterDecl(std::string_view type);
LILA_SCENE_API const NodeDecl* findDecl(std::string_view type);
LILA_SCENE_API std::vector<const NodeDecl*> allDecls();
/** Fills the registry with the built-in node library. Safe to call twice. */
LILA_SCENE_API void registerNodeLibrary();

/** Whether an output of one type may feed an input of another. */
LILA_SCENE_API bool linkable(SocketType from, SocketType to);
/**
 * The socket on a new node of this type that a dragged socket of `type` connects to: one of its inputs when
 * wantInput (the drag came from an output), else one of its outputs. Same type first, then the first linkable one.
 * Empty when nothing fits.
 */
LILA_SCENE_API std::string compatibleSocket(const Group& g, const NodeDecl& d, SocketType type, bool wantInput);

struct Bundle;
struct Closure;
struct Plan;
struct ToolContext;
struct GizmoSpec;

/** What a socket carries during evaluation. Which member is used follows the socket type. */
struct Value
{
    GeometryPtr geometry;
    FieldPtr field;
    std::string string;
    std::shared_ptr<const Bundle> bundle;
    std::shared_ptr<const Closure> closure;
};

/** Values by name, each keeping its socket type, as Combine Bundle makes them. */
struct Bundle
{
    struct Item
    {
        std::string name;
        SocketType type = SocketType::Float;
        Value value;
    };
    std::vector<Item> items;

    const Item* find(std::string_view name) const
    {
        for (const Item& i : items)
            if (i.name == name)
                return &i;
        return nullptr;
    }
};

/**
 * A group to run later with the values it captured where it was made. Its plan's group inputs are the closure's
 * inputs followed by the captured ones; its outputs are the closure's outputs.
 */
struct Closure
{
    std::shared_ptr<const Plan> plan;
    std::map<std::string, Value> captured;
    /** Names of the inputs and outputs a caller sees, with their types (captured inputs left out). */
    std::vector<std::pair<std::string, SocketType>> inputs, outputs;
};

/** Another scene object as seen by an Object Info node. */
struct ObjectInput
{
    std::shared_ptr<const geo::Mesh> mesh;
    /** The splines of a curve object, in its own space; null for a mesh. */
    std::shared_ptr<const CurveSet> curves;
    glm::dmat4 matrix{1.0};
    /** The world matrix of its parent; Bone Info reads a joint's local transform against it. */
    glm::dmat4 parent{1.0};
};

/** The mesh and curve objects under a transform, as a Collection Info node sees them. */
struct CollectionInput
{
    glm::dmat4 parent{1.0};
    std::vector<std::string> names;
    std::vector<ObjectInput> members;
};

/** What the active Viewer node saw: its geometry and the field to colour it by (null when unlinked). */
struct ViewerSink
{
    std::string node;
    GeometryPtr geometry;
    FieldPtr field;
    SocketType type = SocketType::Float;
    Domain domain = Domain::Point;
    bool captured = false;
};

/** Everything an evaluation can read from outside the group. */
struct ExecEnv
{
    double time = 0.0; // seconds
    GeometryPtr input;
    std::map<std::string, Value> params;        // group inputs by name
    std::map<std::string, ObjectInput> objects; // by Object Info and Bone Info node name
    std::map<std::string, CollectionInput> collections; // by Collection Info node name
    /** The owner mesh's world matrix when the Object Info inputs were connected; Relative To owner reads objects against it. */
    glm::dmat4 ownerMatrix{1.0};
    /** Set by the modifier while a Viewer is active; nested groups run without one. */
    ViewerSink* viewer = nullptr;
    /** The modifier node (scene::NodeId) running the group, which simulation nodes publish under; 0 elsewhere. */
    uint64_t owner = 0;
    /** The simulation framework is sampling inputs: only the steps feeding the simulation node run. */
    bool sampleOnly = false;
    /** Results of time-independent steps, kept by the modifier between evaluations (gn/Exec.h). */
    StepCache* cache = nullptr;
    /** Set while the group runs as a tool (gn/Tool.h): the pointer, view and active element its tool nodes read. */
    const ToolContext* tool = nullptr;
    /** Where gizmo nodes record their handles; null skips them. Nested groups run without one. */
    std::vector<GizmoSpec>* gizmos = nullptr;
};

/** Turns a JSON value (node value, exposed default) into a socket value. Rotations are given in degrees. */
LILA_SCENE_API Value valueFromJson(SocketType t, const json& j);
/** An exposed input's value from JSON as the graph sees it: angle-subtype floats are given in degrees and read as radians. */
LILA_SCENE_API Value paramValue(const GInput& in, const json& j);
/** Converts a value along a link of another type; incompatible pairs give the default. */
LILA_SCENE_API Value convertValue(const Value& v, SocketType from, SocketType to);
LILA_SCENE_API Value defaultValue(SocketType t);

class LILA_SCENE_API ExecContext
{
public:
    ExecContext(const Group& g, const GNode& n, const NodeDecl& d, const Sockets& s, const ExecEnv& env)
        : group(g), node(n), decl(d), sockets(s), env(env)
    {
    }

    const Group& group;
    const GNode& node;
    const NodeDecl& decl;
    const Sockets& sockets;
    const ExecEnv& env;
    std::vector<std::vector<Value>> inputs; // per input socket; several only for multi inputs
    std::vector<Value> outputs;

    /** Never null; an empty geometry when unconnected. */
    GeometryPtr geometry(std::string_view id) const;
    std::vector<GeometryPtr> geometries(std::string_view id) const;
    FieldPtr field(std::string_view id) const;
    double f(std::string_view id) const;
    int64_t i(std::string_view id) const;
    bool b(std::string_view id) const;
    glm::dvec3 v(std::string_view id) const;
    std::string s(std::string_view id) const;
    std::string prop(std::string_view id) const { return decl.propValue(node, id); }

    void out(std::string_view id, GeometryPtr g);
    void out(std::string_view id, FieldPtr f);
    void outString(std::string_view id, std::string s);
    /** Attribute name for a field output backed by data the node stored on the geometry. */
    std::string anonymous(std::string_view id) const { return "__" + node.name + "_" + std::string(id); }

private:
    const Value& in(std::string_view id) const;
};

/** Thrown by nodes for a readable evaluation error. */
struct ExecError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};
} // namespace lila::gn
