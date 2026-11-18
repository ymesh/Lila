#pragma once

#include "core/Export.h"
#include "core/Registry.h"
#include "ui/Owner.h"

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QString>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class QBoxLayout;
class QWidget;

namespace lila::scene
{
class Graph;
class Node;
enum class Change : uint8_t;
} // namespace lila::scene

namespace lila::ui
{
class NodeEditor;

/** The backend Lila's own node editor dock opens on. */
inline constexpr const char* kGeometryNodesBackend = "gn";

/**
 * What a node editor edits: one graph domain (geometry nodes, texture graphs, an extension's graphs). The canvas,
 * side panel, clipboard and keys talk only to this.
 *
 * Every edit is a command named command(verb), "gn" + "AddNode" for geometry nodes, taking the same arguments as the
 * geometry-nodes command of that verb: GroupInfo, NodeTypes, AddNode, DeleteNode, Connect, Disconnect, MoveNodes,
 * SetValue, CopyNodes, PasteNodes, DuplicateNodes, GroupNodes, Ungroup, ExportGroup, ImportGroup, SaveGroup,
 * SetActiveViewer, AddInput, RemoveInput, RenameInput, MoveInput, SetInputInfo, ExposeInput, RemoveOutput. The editor
 * hides what a verb drives when supports() says the backend lacks it. GroupInfo lists the group's owners under "owners"
 * (geometry nodes says "modifiers").
 *
 * Open an editor on a backend with NodeEditor(parent, id), for instance from a panel an extension registers.
 */
class LILA_UI_API GraphBackend
{
public:
    virtual ~GraphBackend();

    /** Registry key, "gn". */
    virtual std::string id() const = 0;
    /** Shown in the editor's backend picker and tooltips: "Geometry Nodes". */
    virtual QString title() const = 0;

    /** Prepended to every verb: "gn". */
    virtual std::string commandPrefix() const = 0;
    /** The command a verb runs: the prefix and the verb, unless the backend names it otherwise. */
    virtual std::string command(std::string_view verb) const { return commandPrefix() + std::string(verb); }
    virtual bool supports(std::string_view verb) const;
    /** Verbs that only read, run without echoing to the script editor. */
    virtual bool isQuery(std::string_view verb) const;
    /** Fits the editor's call of a verb to the backend's command, e.g. dropping flags it doesn't take. */
    virtual void adaptCall(std::string_view /*verb*/, nlohmann::json& /*call*/) const {}

    /** The groups in the scene, in the order the group picker lists them. */
    virtual std::vector<std::string> groupNames(scene::Graph& g) const = 0;
    virtual bool hasGroup(scene::Graph& g, const std::string& group) const;
    /** True for the scene node that stores a group, whose value changes rebuild the canvas. */
    virtual bool isGroupNode(const scene::Node& n) const = 0;
    /** The action the New button runs; empty hides the button. */
    virtual std::string newGroupAction() const { return {}; }
    /** The node type that runs another group (its "group" prop names it); empty when groups don't nest. */
    virtual std::string subgroupNodeType() const { return {}; }
    /** Any node type that runs the group its "group" prop names (the subgroup node, plus loops over one). */
    virtual bool runsSubgroup(const std::string& type) const { return !type.empty() && type == subgroupNodeType(); }
    /** File suffixes the editor imports and exports, the first used for export: {"lilanodes", "json"}. */
    virtual std::vector<std::string> fileSuffixes() const { return {}; }

    /** The scene node that runs a group (a modifier for geometry nodes) and the group it runs. */
    struct Owner
    {
        std::string path;
        std::string group;
    };
    /** What the selection leads to: the editor opens its group and edits that owner's values. Empty for none. */
    virtual Owner followSelection(scene::Graph& g) const;
    /**
     * Whether a scene change rebuilds the canvas, for a backend whose groups show the scene's own nodes (a value change on
     * a group node always does). n is the node changed; null for a selection change.
     */
    virtual bool rebuildsOn(scene::Change /*change*/, const scene::Node* /*n*/) const { return false; }
    /** Whether undo and redo rebuild the canvas, for a backend keeping view state (positions, flags) outside its group nodes. */
    virtual bool rebuildsOnUndo() const { return false; }
    /**
     * A cheap fingerprint of everything the group's canvas and panel show, or nullopt when the backend has none. A refresh
     * whose stamp matches the last one skips the rebuild, so commands that leave the group alone cost nothing here.
     */
    virtual std::optional<size_t> groupStamp(scene::Graph& /*g*/, const std::string& /*group*/) const { return std::nullopt; }
    /** The group the node at path runs, empty when it isn't an owner. */
    virtual std::string groupOfOwner(scene::Graph& g, const std::string& path) const;
    /** "modifier": what an owner is called in the side panel. */
    virtual QString ownerNoun() const { return "owner"; }
    /** What the toolbar's group picker is labelled. */
    virtual QString groupNoun() const { return "Group"; }
    /** Whether the editor offers Rename for its groups. */
    virtual bool canRenameGroup() const { return false; }
    /** Renames a group as an undoable UI command; returns the name it ended up with, empty when it failed (already logged). */
    virtual std::string renameGroup(const std::string& /*group*/, const std::string& /*to*/) const { return {}; }
    /** Whether the editor offers Delete for its groups. */
    virtual bool canDeleteGroup() const { return false; }
    /** Deletes a group as an undoable UI command; false when it failed (already logged). */
    virtual bool deleteGroup(const std::string& /*group*/) const { return false; }

    /** The node type that previews in the viewport (an eye on its header toggles it); empty for none. */
    virtual std::string viewerType() const { return {}; }
    /** A node to preview: what Ctrl+Shift+click asks the backend to show. */
    struct ViewRequest
    {
        std::string group;
        std::string node;
        std::string nodeType;
        std::vector<std::pair<std::string, std::string>> outputs; // (id, type)
        QPointF position;
        double nodeWidth = 0.0;
    };
    virtual void viewNode(const ViewRequest&) const {}
    /** Whether Ctrl+Shift+click previews nodes; by default when the backend has a viewer node. */
    virtual bool canViewNodes() const { return !viewerType().empty(); }

    /** Side of the square preview a node of the type draws under its header; 0 for none. */
    virtual int thumbnailSize(const std::string& /*nodeType*/) const { return 0; }
    /**
     * The node's preview, given the group's GroupInfo. Null while it renders (or an older one meanwhile); ready runs on
     * the GUI thread when a newer one is in, and the editor asks again.
     */
    virtual QImage thumbnail(const std::string& /*group*/, const nlohmann::json& /*info*/, const std::string& /*node*/,
                             std::function<void()> /*ready*/) const
    {
        return {};
    }

    /** The side panel's editor for the selected node (an entry of GroupInfo's "nodes"); null uses the editor's own. */
    virtual QWidget* nodePanel(QWidget* /*parent*/, NodeEditor& /*editor*/, const nlohmann::json& /*node*/) const { return nullptr; }

    /** Whether the toolbar shows the Send button. */
    virtual bool canSend() const { return false; }
    /** Scene paths the Send button acts on. ownerInView is the followed owner when it runs group, else empty. */
    virtual std::vector<std::string> sendTargets(scene::Graph& g, const std::string& group, const std::string& ownerInView) const;

    enum class SocketShape : uint8_t
    {
        Circle,
        Square,
        Pill,
    };
    enum class SocketMark : uint8_t
    {
        None,
        Dot,  // a small dark dot
        Hole, // a dark disc of half the radius
        Ring,
    };
    struct SocketStyle
    {
        QColor color;
        SocketShape shape = SocketShape::Circle;
        SocketMark mark = SocketMark::None;
        double radius = 5.0;
    };
    /**
     * The node that merges values of a socket type with an operation ("add", "subtract", "multiply", "divide"): Join
     * Geometry for geometry, Math for numbers. multi: every source goes into inA; otherwise nodes chain two at a time.
     */
    struct MergeNode
    {
        std::string type;
        nlohmann::json values = nlohmann::json::object();
        std::string inA, inB;
        std::string out;
        bool multi = false;
    };
    virtual std::optional<MergeNode> mergeNode(const std::string& /*socketType*/, const std::string& /*op*/) const { return std::nullopt; }
    /** Wire thickness for a socket type; the backend's main data (geometry, materials) draws thicker. */
    virtual double wireWidth(const std::string& /*type*/) const { return 2.0; }
    /** Fields draw as diamonds in the style's colour whatever the shape. */
    virtual SocketStyle socketStyle(const std::string& type) const;
    /** Whether an output of one socket type may feed an input of another; the canvas uses it to pick sockets to wire. */
    virtual bool canLink(const std::string& from, const std::string& to) const { return from == to; }
    /**
     * A line about the value on a wire out of node.socket, for its tooltip, when it is cheap to tell (info is the
     * group's GroupInfo). By default: the values of a node with nothing linked into it.
     */
    virtual QString wireSummary(const nlohmann::json& info, const std::string& node, const std::string& socket) const;
    /** Sockets that carry data but no editable value (geometry): no value text, no side-panel field. */
    virtual bool valueless(const std::string&) const { return false; }
    virtual QColor headerColor(const std::string& type, const std::string& category) const;
    /** Types offered for a new exposed input. */
    virtual std::vector<std::string> inputTypes() const { return {"float", "int", "bool", "vector", "color", "string"}; }
    /** Shown under the side panel when no node is selected. */
    virtual QString canvasHint() const;

    /** Adds backend-specific buttons to the editor's toolbar, after Add Node. */
    virtual void addToolbarButtons(QBoxLayout*, NodeEditor&) const {}
};

using GraphBackendPtr = std::shared_ptr<GraphBackend>;

/** Registered graph backends. A node editor holds the one it shows and drops it when it is removed. */
namespace graphBackends
{
LILA_UI_API Registry<GraphBackendPtr>& registry();
/** Adds or replaces, under the backend's id. */
LILA_UI_API void add(GraphBackendPtr backend, std::string owner = kLilaOwner);
/** Owned by the extension and removed when it unloads. 0, with a log message, when the id is someone else's. */
LILA_UI_API uint64_t add(ext::Context& ctx, GraphBackendPtr backend);
LILA_UI_API bool remove(const std::string& id);
/** Null when id is not registered. */
LILA_UI_API GraphBackendPtr find(const std::string& id);
/** Registered ids, oldest first. */
LILA_UI_API std::vector<std::string> ids();
} // namespace graphBackends

/** Registers the geometry-nodes backend (kGeometryNodesBackend). */
LILA_UI_API void registerGnGraphBackend();

/** The backend texture graphs open on. */
inline constexpr const char* kTextureGraphBackend = "tex";
/** Registers the texture-graph backend (kTextureGraphBackend). */
LILA_UI_API void registerTexGraphBackend();

/** The backend showing the scene's own dependency graph: views of real nodes and plugs around the selection. */
inline constexpr const char* kSceneGraphBackend = "scene";
/** Registers the scene backend (kSceneGraphBackend). */
LILA_UI_API void registerSceneGraphBackend();
} // namespace lila::ui
