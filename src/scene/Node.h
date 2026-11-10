#pragma once

#include "core/Export.h"
#include "core/Registry.h"

#include "scene/Plug.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lila::ext
{
class Context;
}

namespace lila::scene
{
class Graph;

struct EvalContext
{
    double time = 0.0; // seconds
};

class Node;

struct LILA_SCENE_API NodeType
{
    std::string name;
    const NodeType* base = nullptr;
    bool dag = false; // lives in the transform hierarchy
    bool timeDependent = false; // recomputed whenever the graph's time changes
    std::string owner; // extension that registered it; empty = built in
    /** What the node does and when to use it, one or two sentences. Tooltips, menus and the API reference show it. */
    std::string doc;
    /** Tabler icon name for the outliner and other node lists; empty inherits the base type's. */
    std::string icon;
    std::vector<AttrDef> ownAttrs;
    std::function<std::unique_ptr<Node>()> create;

    /** Base attrs first, then own. Filled by registerType. */
    std::vector<AttrDef> attrs;
    /** Set on a node's private copy of its type, which adds its user attrs (AttrUser) after the type's own: the type it copies. */
    const NodeType* userOf = nullptr;

    int attrIndex(std::string_view name) const;
    /**
     * attrIndex, falling back to the attrs' fileAlias: for reading scene files. With element, also reads "<alias><n>", an
     * array's numbered predecessor, setting *element to n (else -1).
     */
    int fileAttrIndex(std::string_view name, int* element = nullptr) const;
    bool isA(std::string_view typeName) const;
    /** The registered type: this one, or the one a node's private copy (see userOf) was made from. */
    const NodeType& registered() const { return userOf ? *userOf : *this; }
    /** The children of a grouped array ("group.child" attrs), in declaration order. */
    std::vector<int> groupChildren(std::string_view group) const;
    /** Attr names older scene files may hold that no longer mean anything; loading skips them without a warning. */
    std::vector<std::string> retiredAttrs;
    /** True when this type or a base retired the attr name. */
    bool isRetired(std::string_view name) const;
};

namespace types
{
/** Every node type, owner-tagged. */
LILA_SCENE_API Registry<NodeType>& registry();
LILA_SCENE_API const NodeType& registerType(NodeType type);
/** For extensions: owned by ctx and removed when it unloads. Null, with a log message, if the name is taken. */
LILA_SCENE_API const NodeType* registerType(ext::Context& ctx, NodeType type);
/** Drops a type from lookup. It stays allocated for anything still pointing at it, but can no longer create nodes. */
LILA_SCENE_API void unregisterType(std::string_view name);
LILA_SCENE_API const NodeType* find(std::string_view name);
/** Makes find(oldName) return the type named `type`, for scene files that store a type under an older name. */
LILA_SCENE_API void addFileAlias(std::string_view oldName, std::string_view type);
LILA_SCENE_API std::vector<const NodeType*> all();
/** The first icon set on the type or one of its bases, else fallback. */
LILA_SCENE_API const std::string& iconFor(const NodeType& type, const std::string& fallback);
LILA_SCENE_API void registerBuiltins();
} // namespace types

/**
 * A DG node. Values live here; connections and hierarchy live in Graph. compute() runs on a worker thread and may
 * only read this node's values (inputs are already pulled from their sources) and write its outputs.
 */
class Node
{
public:
    virtual ~Node() = default;

    NodeId id() const { return mId; }
    /** The graph the node is in, null before it is added. */
    const Graph* graph() const { return mGraph; }
    const NodeType& type() const { return *mType; }
    /** The short name, unique among siblings. Use path() to address the node. */
    const std::string& name() const { return mName; }
    /** Graph::path: the shortest path that resolves back to this node. */
    LILA_SCENE_API std::string path() const;
    /** Graph::fullPath. */
    LILA_SCENE_API std::string fullPath() const;

    /** "" for the root namespace. */
    const std::string& nameSpace() const { return mNamespace; }

    /** Reference this node was loaded from; 0 for nodes owned by the scene. */
    uint32_t referenceId() const { return mReference; }

    const PlugValue& value(int attr) const { return mValues[attr]; }
    /** An array's elements (index, value), by ascending index: connected ones carry what was pulled. Empty for other attrs. */
    LILA_SCENE_API const std::vector<std::pair<int, PlugValue>>& elements(int attr) const;
    /** One element's value, or null when the array has no such element. */
    LILA_SCENE_API const PlugValue* element(int attr, int index) const;
    /** An element's value, or the attr's default when it has no such element. */
    template <class T> const T& elementOr(int attr, int index) const
    {
        const PlugValue* v = element(attr, index);
        return std::get<T>(v ? *v : mType->attrs[size_t(attr)].defaultValue);
    }
    /** Locked parts of an attr: bit 0 for a scalar, bits 0-2 for a Vec3's components. See Graph::setLocked. */
    uint8_t lockMask(int attr) const { return size_t(attr) < mLocks.size() ? mLocks[size_t(attr)] : uint8_t(0); }
    template <class T> const T& as(int attr) const { return std::get<T>(mValues[attr]); }
    /** Graph::evaluationPass() when this node last computed; 0 before it ever has. */
    uint64_t evaluatedPass() const { return mEvaluatedPass; }

    virtual void compute(const EvalContext& ctx) { (void)ctx; }

    /** Side data compute() publishes outside the node's plugs, kept by cached playback along with its values. */
    struct PlaybackExtra
    {
        std::shared_ptr<const void> data;
        size_t bytes = 0;
        /** False when what compute() just produced is a placeholder that will be recomputed, never to be kept. */
        bool keep = true;
    };
    /** Called on a worker thread right after compute(). */
    virtual PlaybackExtra playbackExtra(const EvalContext& ctx) const
    {
        (void)ctx;
        return {};
    }
    /** Republishes what playbackExtra returned, in place of a compute(). Called on a worker thread. */
    virtual void restorePlaybackExtra(const std::shared_ptr<const void>& data) { (void)data; }

    enum Trait : uint32_t
    {
        kModifier = 1, // a ModifierNode
        kDeformer = 2, // a DeformerNode
    };
    /** Trait bits, so code can tell a node's C++ base without dynamic_cast, which fails across libraries on some platforms. */
    virtual uint32_t traits() const { return 0; }

protected:
    template <class T> void setOutput(int attr, T v) { mValues[attr] = std::move(v); }

private:
    friend class Graph;

    NodeId mId = kNoNode;
    const Graph* mGraph = nullptr;
    const NodeType* mType = nullptr;
    std::string mName;
    std::string mNamespace;
    uint32_t mReference = 0;
    std::vector<PlugValue> mValues;
    std::vector<std::vector<std::pair<int, PlugValue>>> mElements; // per attr, empty until an array gets an element
    std::shared_ptr<NodeType> mOwnType; // the private type copy holding user attrs, once one is added
    std::vector<uint8_t> mLocks; // per attr, empty until something is locked
    bool mDirty = true;
    /** In the pass it last evaluated in, with cached playback on: 0 restored, 1 computed and kept, 2 computed, not kept. */
    uint8_t mPlayback = 0;
    uint64_t mEvaluatedPass = 0;
};
} // namespace lila::scene
