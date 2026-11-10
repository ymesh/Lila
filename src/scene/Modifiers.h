#pragma once

#include "core/Export.h"

#include "scene/Node.h"

#include <functional>

namespace lila::geo
{
struct MeshRemap;
}

namespace lila::scene
{
class Graph;

/**
 * A non-destructive mesh modifier: inMesh in, outMesh out, keyable parameters. Modifiers chain between a mesh's
 * source (a generator or a stored base mesh) and the mesh node, so the stack is just DG history:
 *
 *   polyCube -> bevelModifier -> mirrorModifier -> Cube.inMesh
 */
class LILA_SCENE_API ModifierNode : public Node
{
public:
    using Apply = std::function<geo::Mesh(const geo::Mesh&, const Node&)>;

    void compute(const EvalContext& ctx) override;
    uint32_t traits() const override { return kModifier; }

    /** True while this modifier shows its input for editing: the cage becomes its input and it and later modifiers pass through. */
    virtual bool editingInput() const { return false; }

    /** Why the last evaluation failed (the input passed through), empty when it succeeded. Read after Graph::evaluate. */
    const std::string& error() const { return mError; }

    Apply apply;
    int inAttr = 0;
    int enabledAttr = 1;
    int outAttr = -1;

protected:
    std::string mError;
};

/** Holds a base mesh upstream of modifiers, so direct edits never disturb the stack. */
class LILA_SCENE_API MeshSourceNode : public Node
{
public:
    enum Attr
    {
        kMesh,
        kOutMesh,
    };
    void compute(const EvalContext& ctx) override;
};

namespace modifiers
{
/** Point layer marking a mesh that passes through the rest of its stack untouched (a modifier upstream is in edit mode). */
inline constexpr const char* kBypassLayer = "lila:editBypass";
LILA_SCENE_API bool bypassed(const MeshPtr& mesh);

LILA_SCENE_API bool isModifier(const Node& n);
/** n as a modifier, or null. */
inline ModifierNode* asModifier(Node* n) { return n && (n->traits() & Node::kModifier) ? static_cast<ModifierNode*>(n) : nullptr; }
inline const ModifierNode* asModifier(const Node* n) { return n && (n->traits() & Node::kModifier) ? static_cast<const ModifierNode*>(n) : nullptr; }

/** Modifiers feeding a mesh node, first applied first. */
LILA_SCENE_API std::vector<NodeId> stack(Graph& g, NodeId mesh);

/** True when a subdivision modifier comes after the modifier in its stack. */
LILA_SCENE_API bool feedsSubdivision(const ModifierNode& mod);

/**
 * The modifier's input as it runs: the first modifier of a stack feeding a subdivision adds geo::addFitSpacing's layers,
 * so a Fit Surface subdivision measures island borders bunched as the modifiers before it bunch the uvs.
 */
LILA_SCENE_API MeshPtr stackInput(const ModifierNode& mod, const MeshPtr& in);

/** The storable plug holding the base mesh direct edits change. Bakes a procedural source into a stored one. */
LILA_SCENE_API PlugRef editPlug(Graph& g, NodeId mesh);

/** The base mesh without modifiers (the cage), as components are picked and edited on it. Doesn't change the graph. */
LILA_SCENE_API MeshPtr cage(Graph& g, NodeId mesh);

/** The mesh's component selection, mode and object selection, for an edit mode to put back when it ends. */
LILA_SCENE_API json selectionSnapshot(Graph& g, NodeId mesh);
LILA_SCENE_API void restoreSelection(Graph& g, NodeId mesh, const json& snapshot);

/** Kinds addModifier can create ("bevel" for bevelModifier), from the type registry, so extension modifiers appear too. Sorted. */
LILA_SCENE_API std::vector<std::string> kinds();

/** What a kind ("bevel") does, from its node type's doc. Empty for an unknown or undocumented kind. */
LILA_SCENE_API std::string kindDoc(std::string_view kind);

/** What feeds the bottom of the stack: a generator, a meshSource, or kNoNode when the mesh stores its own base. */
LILA_SCENE_API NodeId stackSource(Graph& g, NodeId mesh);

/** The mesh node at the top of a modifier's chain, kNoNode if it feeds none. */
LILA_SCENE_API NodeId meshOf(Graph& g, NodeId modifier);

/** Runs as addModifier creates a modifier of the type, before it is wired in, so it can take context such as the selected components. */
LILA_SCENE_API void setCreateHook(const std::string& type, std::function<void(Graph&, Node& modifier, NodeId mesh)> hook);

/** A direct edit compacted the base mesh: modifiers holding component ids on it follow them. Records undo with the edit. */
LILA_SCENE_API void baseRemapped(Graph& g, NodeId mesh, const geo::MeshRemap& remap, const geo::Mesh& base);

/** Whether a modifier is applied on export (its exportEnabled attr; true when the type has none). */
LILA_SCENE_API bool exportEnabled(const Node& modifier);

/** The mesh as exporters should write it: outMesh, or the stack re-run without the modifiers turned off for export. */
LILA_SCENE_API MeshPtr exportMesh(Graph& g, NodeId mesh);

LILA_SCENE_API void registerTypes();
LILA_SCENE_API void registerCommands();
/** duplicateModifier, applyAllModifiers, bakeModifierStack, modifierStack (ModifierCommands.cpp). */
LILA_SCENE_API void registerStackCommands();
} // namespace modifiers
} // namespace lila::scene
