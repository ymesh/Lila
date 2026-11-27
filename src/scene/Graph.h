#pragma once

#include "core/Export.h"

#include "scene/Naming.h"
#include "scene/Node.h"

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <source_location>
#include <unordered_map>
#include <unordered_set>

namespace lila::scene
{
/** General prefs: cached playback on or off, and the memory it may use in MB (0: half the machine's). */
inline constexpr const char* kCachedPlaybackPref = "/cachedPlayback";
inline constexpr const char* kCachedPlaybackMemoryPref = "/cachedPlaybackMemoryMB";

enum class Change : uint8_t
{
    Created,
    Deleted,
    Renamed,
    Reparented,
    Value,
    Connection,
    Selection,
    Cleared,
    /** Many changes at once (a Graph::Batch ended): anything may have changed. Sent only to batched listeners. */
    Reset,
    /** A node gained or lost a user attr. */
    Attrs,
};

enum class SelectMode : uint8_t
{
    Replace,
    Add,
    Remove,
    Toggle,
};

/**
 * The scene's dependency graph plus its transform hierarchy. Main thread only, except evaluate() which fans
 * compute out over the TBB pool. Every mutation records undo.
 */
class LILA_SCENE_API Graph
{
public:
    static Graph& current();

    Graph();
    ~Graph();

    /** Not undoable. */
    void clear();

    // Nodes
    /**
     * applyNaming=false keeps the name as given (file loads); it is still made unique among its siblings, or across
     * the scene with NamingPolicy::uniqueNames. Default names (empty) are always made unique across the scene.
     */
    Node* createNode(const NodeType& type, std::string_view name = {}, NodeId parent = kNoNode, bool applyNaming = true);
    void deleteNode(NodeId id);
    Node* node(NodeId id) const;
    /**
     * Resolves a short name ("Arm"), a partial path ("L|Arm", any unique run of parents ending at the node) or a full
     * path ("|Body|L|Arm"). Null when nothing matches; throws CommandError listing the full paths when it is ambiguous.
     * Nodes outside the hierarchy count as roots.
     */
    Node* find(std::string_view path) const;
    /** Every node `path` matches, by id. */
    std::vector<NodeId> findAll(std::string_view path) const;
    /** The shortest path that resolves back to this node alone: its name when that is unique. "" for no node. */
    std::string path(NodeId id) const;
    /** "|Body|L|Arm". */
    std::string fullPath(NodeId id) const;
    /** Returns the name actually given, which is made unique (see createNode). */
    std::string rename(NodeId id, std::string_view name);
    /** What `wanted` would become for a node under `parent` (kNoNode for a root or a non-DAG node). */
    std::string availableName(std::string_view wanted, NodeId parent = kNoNode) const;
    /** NamingPolicy::uniqueNames: names unique across the scene rather than among siblings. */
    bool uniqueNames() const { return mNaming.uniqueNames; }
    std::vector<NodeId> nodes() const;
    /** Marks a node as loaded from a file reference (0 = owned by the scene). Not undoable; references manage it. */
    void setReference(NodeId id, uint32_t reference);

    // Hierarchy
    /**
     * index is the position the child takes among the parent's children (SIZE_MAX appends). With the same parent and
     * an index it only reorders.
     */
    void setParent(NodeId child, NodeId parent, bool keepWorld = true, size_t index = SIZE_MAX);
    NodeId parent(NodeId id) const;
    const std::vector<NodeId>& children(NodeId id) const;
    const std::vector<NodeId>& roots() const { return mRoots; }
    std::vector<NodeId> descendants(NodeId id) const;

    // Plugs
    /** "node.attr" or "node.tx", the node part resolved as find() does. Throws std::invalid_argument. */
    PlugRef plug(std::string_view path) const;
    /** The node's shortest unique path, a dot and the attr. */
    std::string plugName(const PlugRef& p) const;
    /** plugName with the node's full path: stable while other nodes come and go, for files and baselines. */
    std::string fullPlugName(const PlugRef& p) const;
    const AttrDef& attr(const PlugRef& p) const;

    /**
     * Throws std::invalid_argument when the plug is driven by a connection, unless the driven-set hook takes the
     * edit. Setting a whole Vec3 whose components are driven stores it and offers each changed driven component to
     * the hook.
     */
    void setValue(const PlugRef& p, PlugValue v);

    /**
     * Maya's attribute lock: a locked plug rejects setValue (the channel box, manipulators and setAttr all go through
     * it). A whole Vec3 counts as locked when all three components are; setting it fails only when a locked
     * component would change. Undoable.
     */
    void setLocked(const PlugRef& p, bool locked);
    bool isLocked(const PlugRef& p) const;
    /** True when the plug or any of its components is locked. */
    bool anyLocked(const PlugRef& p) const;
    /** v with the plug's locked parts put back to their stored values, for tools that move only what they may. */
    PlugValue keepLocked(const PlugRef& p, PlugValue v) const;

    enum class DrivenEdit : uint8_t
    {
        Reject, // the connection wins
        Handled, // the hook changed the source (set a key, say)
        Hold, // keep the given value until the time or the source changes
    };
    /** Decides what a set on a driven scalar plug does. src is the plug driving dst. */
    using DrivenSetHook = std::function<DrivenEdit(Graph& g, const PlugRef& dst, const PlugRef& src, double value)>;
    void setDrivenSetHook(DrivenSetHook fn) { mDrivenSet = std::move(fn); }
    /**
     * The same for a whole Vec3 driven by a Vec3 source (a rotation curve). Setting one component of such a plug
     * offers the whole value with that component replaced.
     */
    using DrivenVecSetHook = std::function<DrivenEdit(Graph& g, const PlugRef& dst, const PlugRef& src, const glm::dvec3& value)>;
    void setDrivenVecSetHook(DrivenVecSetHook fn) { mDrivenVecSet = std::move(fn); }
    /**
     * Called by the outermost deleteNode with the node and its descendants before anything is disconnected, so data
     * other nodes depend on can move (an instance source handing its mesh to an instance). Edits it makes record undo.
     */
    using DeleteHook = std::function<void(Graph& g, const std::vector<NodeId>& doomed)>;
    void setDeleteHook(DeleteHook fn) { mDeleteHook = std::move(fn); }
    /** True while a driven plug shows a held value instead of its source's. */
    bool isHeld(const PlugRef& dst) const { return mHeld.contains(dst); }
    /** Raw stored value, no evaluation. */
    const PlugValue& storedValue(const PlugRef& p) const { return node(p.node)->value(p.attr); }
    std::vector<std::pair<PlugRef, PlugRef>> connections() const { return {mInputs.begin(), mInputs.end()}; }
    /** Evaluates first if the node is dirty. Component plugs return a double. */
    PlugValue value(const PlugRef& p);

    /**
     * Either end may be one component of a Vec3 (node.tx), which carries a float. A component destination and its
     * whole Vec3 can't both be driven; force replaces whichever is there. A destination that is a whole array input
     * gets a new element after its last one. Returns the destination connected.
     */
    PlugRef connect(const PlugRef& src, const PlugRef& dst, bool force = false);
    void disconnect(const PlugRef& dst);
    std::optional<PlugRef> source(const PlugRef& dst) const;
    std::vector<PlugRef> destinations(const PlugRef& src) const;

    // Arrays
    /** An array's elements, connected or holding a value, by ascending index. */
    std::vector<PlugRef> elements(const PlugRef& array) const;
    /** The element indices any child of a grouped array uses, ascending. */
    std::vector<int> groupIndices(NodeId node, std::string_view group) const;
    /** Makes a storable array's elements values[0..n-1], dropping its other ones. Throws when one of them is connected. */
    void setElements(const PlugRef& array, const std::vector<PlugValue>& values);
    /** Removes an element: its connection and its value. */
    void removeElement(const PlugRef& element);
    /**
     * Gives node `to` the elements node `from` has on the array attr (both of one type): the values the unconnected ones
     * hold, and with shareSources the connected ones' sources too.
     */
    void copyElements(NodeId from, NodeId to, int attr, bool shareSources);

    // User attrs
    /**
     * Adds an attr to one node, after its type's, flagged AttrUser (plus AttrInput and AttrStorable). A Vec3 without child
     * names gets <name>X/Y/Z. Throws std::invalid_argument when the name or short name is taken. Returns its index.
     */
    int addAttr(NodeId node, AttrDef def);
    /** Removes a user attr with its connections and value. Later user attrs move down one index. */
    void deleteAttr(const PlugRef& attr);
    /**
     * Replaces a user attr's definition: name, type, hints, flags. Connections, keys and drivers stay on it (they hold its
     * index, not its name) unless the type changes, which drops them; the value converts to the new type where it can and
     * takes the new default where it can't. Throws std::invalid_argument for a name taken by another attr. Undoable.
     */
    void editAttr(const PlugRef& attr, AttrDef def);
    /** Moves a user attr to index `to`, another user attr's place, shifting the ones between. Undoable. */
    void moveAttr(NodeId node, int from, int to);

    // Evaluation
    void evaluate();
    /**
     * While one is alive on this thread, evaluate() leaves meshes dirty, with whatever only they need (skins, deformers),
     * computing the transforms around them. For interactive edits that read joints many times per mouse move; the next
     * evaluate() outside it (the viewport's, once per frame) brings the meshes up to date.
     */
    struct LILA_SCENE_API DeferMeshes
    {
        DeferMeshes();
        ~DeferMeshes();
        DeferMeshes(const DeferMeshes&) = delete;
        DeferMeshes& operator=(const DeferMeshes&) = delete;
    };
    /**
     * Computes only what the given nodes depend on (themselves, their parents and their inputs, transitively), leaving
     * the rest dirty for the next evaluate(), without the evaluate hooks. For reading a few values at another time, as
     * simulations sample their inputs, without recomputing meshes nothing reads.
     */
    void evaluateUpstream(const std::vector<NodeId>& nodes);
    /**
     * View-only turns (turntables): each node's parentMatrix is premultiplied by its matrix as it evaluates, so it and
     * everything under it draws, picks and edits turned. Not undoable, not saved. Nodes whose turn changes are dirtied.
     * Main thread, outside evaluate().
     */
    void setDisplayTurns(std::unordered_map<NodeId, glm::dmat4> turns);
    const std::unordered_map<NodeId, glm::dmat4>& displayTurns() const { return mDisplayTurns; }
    /** Marks a node for recompute on the next evaluate(), like an input change would. */
    void touch(NodeId id) { markDirty(id); }
    /**
     * Called on the main thread by evaluate(): before computing anything dirty (after = false) and once done (after =
     * true). Evaluations the hook makes itself don't call it again. Simulations use it to step on the main thread.
     */
    using EvaluateHook = std::function<void(Graph& g, bool after)>;
    static void setEvaluateHook(EvaluateHook hook);
    /** Further hooks, called after the one set above in the order added. */
    static void addEvaluateHook(EvaluateHook hook);
    // Cached playback
    /**
     * While on, what every node computes is kept per time, and a full evaluate() at a kept time restores it instead of
     * computing. Any change to the scene but a selection drops what is kept.
     */
    void setCachedPlayback(bool on);
    bool cachedPlayback() const { return mPlayback != nullptr; }
    void clearPlaybackCache();
    /** Every kept time in seconds, ascending. */
    std::vector<double> playbackCacheTimes() const;
    /** Roughly what the kept results hold in memory. */
    size_t playbackCacheBytes() const;

    double time() const { return mCtx.time; }
    /** notify false skips the TimeChanged event, for moving time briefly and back again (sampling at other frames). */
    void setTime(double seconds, bool notify = true);
    /** Counts evaluations that computed something; each node records the pass it last computed in (Node::evaluatedPass). */
    uint64_t evaluationPass() const { return mEvaluationPass; }
    /** Bumped on every change; the renderer uses it to skip unchanged frames. */
    uint64_t version() const { return mVersion.load(std::memory_order_relaxed); }
    /**
     * Changes whenever a node is created or deleted or the graph is cleared, to a value no other graph has had, so caches
     * keyed on it never mistake one graph for another.
     */
    uint64_t structure() const { return mStructure; }
    /**
     * Bumped by every change that can alter which nodes are hidden: hierarchy edits, visibility and display layer plugs
     * on DAG nodes, and any edit to a non-DAG node (layers among them). Moving a camera or an object leaves it alone.
     */
    uint64_t visibilityVersion() const { return mVisibilityVersion; }

    // Selection
    const std::vector<NodeId>& selection() const { return mSelection; }
    void select(const std::vector<NodeId>& ids, SelectMode mode = SelectMode::Replace);
    bool isSelected(NodeId id) const;
    /** Bumped whenever the selection changes, including when a deleted node leaves it. */
    uint64_t selectionSerial() const { return mSelectionSerial; }

    using Listener = std::function<void(Change, NodeId)>;
    /**
     * immediate: called for every change even inside a Batch (model bookkeeping that later edits depend on). Others,
     * typically UI, get one Change::Reset when the outermost Batch ends instead.
     */
    int addListener(Listener fn, bool immediate = false, std::source_location where = std::source_location::current());
    void removeListener(int id);
    /** Inside a listener's Change::Value or Change::Connection call: the plug that changed, when one did. Null otherwise. */
    const PlugRef* changedPlug() const { return mChangedPlug; }

    /** Scene loads and imports: batched listeners hear nothing until the end, then one Reset. Nests. Main thread. */
    class LILA_SCENE_API Batch
    {
    public:
        explicit Batch(Graph& g = Graph::current());
        ~Batch();
        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;

    private:
        Graph& mGraph;
    };
    bool batching() const { return mBatchDepth > 0; }

    static bool validName(std::string_view name);

    void setNamingPolicy(NamingPolicy policy) { mNaming = std::move(policy); }
    const NamingPolicy& namingPolicy() const { return mNaming; }

private:
    friend struct GraphAccess;

    struct Relations
    {
        NodeId parent = kNoNode;
        std::vector<NodeId> children;
    };

    /** global: unique across the scene; otherwise among the children of parent. */
    std::string uniqueName(std::string_view wanted, NodeId parent, NodeId self, bool global) const;
    /** id's ancestors' names match segs (leaf last, already known to match); absolute also requires a root. */
    bool matchesPath(NodeId id, const std::vector<std::string_view>& segs, bool absolute) const;
    void eraseName(const std::string& name, NodeId id);
    /** Applies the naming policy; throws in block mode when a user-given name doesn't conform. */
    std::string conformName(const NodeType& type, std::string_view wanted, bool userGiven) const;
    void attach(std::unique_ptr<Node> node, NodeId parent, size_t index);
    std::unique_ptr<Node> detach(NodeId id, size_t* indexOut);
    void reparentRaw(NodeId child, NodeId parent, size_t index);
    void renameRaw(NodeId id, const std::string& name);
    void setRaw(const PlugRef& p, const PlugValue& v);
    void setLockRaw(NodeId id, int attr, uint8_t mask);
    void connectRaw(const PlugRef& src, const PlugRef& dst);
    /** The element's stored slot, made (holding the attr's default) when missing. */
    PlugValue& elementRaw(const PlugRef& p);
    void eraseElementRaw(const PlugRef& p);
    void setElementsRaw(const PlugRef& array, const std::vector<std::pair<int, PlugValue>>& elements);
    struct AttrState
    {
        AttrDef def;
        PlugValue value;
        std::vector<std::pair<int, PlugValue>> elements;
        uint8_t lock = 0;
    };
    void insertAttrRaw(NodeId id, int index, const AttrState& state);
    AttrState removeAttrRaw(NodeId id, int index);
    /** Moves every connection and held plug on id's attrs from `from` up by delta. */
    void shiftAttrRefs(NodeId id, int from, int delta);
    /** Every connection and held plug on id's attrs renumbered by map(old index). */
    void remapAttrRefs(NodeId id, const std::function<int(int)>& map);
    void replaceAttrRaw(NodeId id, int index, const AttrState& state);
    void moveAttrRaw(NodeId id, int from, int to);
    void disconnectRaw(const PlugRef& dst);
    void selectRaw(std::vector<NodeId> ids);
    void holdRaw(const PlugRef& dst, const PlugValue& v);
    bool setDrivenVec(const PlugRef& dst, const PlugRef& src, const glm::dvec3& v);
    void releaseRaw(const PlugRef& dst);
    /** Routes a set on a driven scalar through the hook. Returns false when it is rejected. */
    bool setDriven(const PlugRef& dst, const PlugRef& src, double v);
    void markDirty(NodeId id);
    /** evaluate(), computing only what `only` depends on when given. */
    void evaluateWith(const std::vector<NodeId>* only);
    void evaluateDirty(const std::vector<NodeId>* only = nullptr);
    bool dependsOn(NodeId node, NodeId upstream) const;
    /** Puts back what the node computed at the current time, if kept. Thread-safe. */
    bool restoreCached(Node& n);
    /** Returns whether it was kept. */
    bool keepCached(Node& n);
    /** The most a node's parent and sources computed this pass (Node::mPlayback): any recompute upstream invalidates a restore. */
    uint8_t upstreamPlayback(const Node& n, uint64_t pass) const;
    void changed(Change c, NodeId id, const PlugRef* plug = nullptr);
    bool changesVisibility(Change c, NodeId id, const PlugRef* plug) const;
    /** Node and selection changes of the current graph, to the event bus. */
    void forwardEvent(Change c, NodeId id);
    std::vector<NodeId>& siblings(NodeId parent);

    std::unordered_map<NodeId, std::unique_ptr<Node>> mNodes;
    std::unordered_multimap<std::string, NodeId> mNames; // short name -> nodes
    std::unordered_map<NodeId, Relations> mRel;
    std::vector<NodeId> mRoots;
    std::map<PlugRef, PlugRef> mInputs; // dst -> src
    std::unordered_map<NodeId, std::vector<PlugRef>> mOutgoingDst; // src node -> dst plugs it drives
    std::vector<NodeId> mSelection;
    uint64_t mSelectionSerial = 0;
    std::set<PlugRef> mHeld; // driven plugs keeping a user-set value
    DrivenSetHook mDrivenSet;
    DrivenVecSetHook mDrivenVecSet;
    DeleteHook mDeleteHook;
    int mDeleteDepth = 0;
    struct ListenerEntry
    {
        Listener fn;
        bool immediate = false;
        std::source_location where; // who listens, for the profiler's slow-listener messages
    };
    std::map<int, ListenerEntry> mListeners;
    int mNextListener = 1;
    const PlugRef* mChangedPlug = nullptr;
    int mBatchDepth = 0;
    bool mBatchPending = false;
    NodeId mNextId = 1;
    EvalContext mCtx;
    NamingPolicy mNaming;
    std::atomic<uint64_t> mVersion{1};
    uint64_t mStructure = 0;
    uint64_t mVisibilityVersion = 0;
    void restructured();
    /** nodes() sorted, kept until the next structure change; filled by whichever thread asks first. */
    mutable std::mutex mSortedMutex;
    mutable std::vector<NodeId> mSorted;
    mutable uint64_t mSortedStructure = 0;
    bool mAnyDirty = false;
    uint64_t mEvaluationPass = 0;
    struct PlaybackCache;
    std::unique_ptr<PlaybackCache> mPlayback;
    std::unordered_map<NodeId, glm::dmat4> mDisplayTurns;
    std::unordered_set<NodeId> mTurned; // the turned nodes and their descendants, kept out of the playback cache
};
} // namespace lila::scene
