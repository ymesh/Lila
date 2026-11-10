#include "scene/Graph.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Achievements.h"
#include "core/Events.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/SystemMemory.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/Simulation.h"
#include "scene/Spaces.h"
#include "scene/Transform.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace lila::scene
{
namespace
{
const std::vector<NodeId> kNoChildren;

bool isArray(const AttrDef& a) { return (a.flags & AttrArray) != 0; }

/** The attr part of a plug's name: attr, attr[3], or group[3].child for a grouped array's child. */
std::string attrPart(const AttrDef& a, const PlugRef& p)
{
    if (p.component >= 0)
        return a.childNames[size_t(p.component)];
    if (p.element < 0)
        return a.name;
    const std::string index = "[" + std::to_string(p.element) + "]";
    size_t dot = a.name.find('.');
    return dot == std::string::npos ? a.name + index : a.name.substr(0, dot) + index + a.name.substr(dot);
}

/** A whole non-negative number, or -1. */
int parseIndex(std::string_view digits)
{
    int v = -1;
    if (digits.empty() || std::from_chars(digits.data(), digits.data() + digits.size(), v).ptr != digits.data() + digits.size())
        return -1;
    return v;
}

PlugValue* elementSlot(std::vector<std::vector<std::pair<int, PlugValue>>>& all, const PlugRef& p)
{
    if (size_t(p.attr) >= all.size())
        return nullptr;
    for (auto& [i, v] : all[size_t(p.attr)])
        if (i == p.element)
            return &v;
    return nullptr;
}

bool scalarOf(const PlugValue& v, double& out)
{
    return std::visit(
        [&](const auto& x) {
            using V = std::decay_t<decltype(x)>;
            if constexpr (std::is_arithmetic_v<V>)
            {
                out = double(x);
                return true;
            }
            else
                return false;
        },
        v);
}

/** Holds a value inside the attr's hard limits; each component of a Vec3. */
void clampToLimits(const AttrDef& a, PlugValue& v)
{
    if (!a.minValue && !a.maxValue)
        return;
    const double lo = a.minValue.value_or(-HUGE_VAL), hi = a.maxValue.value_or(HUGE_VAL);
    if (double* d = std::get_if<double>(&v))
        *d = std::clamp(*d, lo, hi);
    else if (int64_t* i = std::get_if<int64_t>(&v))
        *i = int64_t(std::clamp(double(*i), std::ceil(lo), std::floor(hi)));
    else if (glm::dvec3* x = std::get_if<glm::dvec3>(&v))
        *x = glm::clamp(*x, glm::dvec3(lo), glm::dvec3(hi));
}

void assignScalar(PlugValue& slot, double s)
{
    std::visit(
        [&](auto& d) {
            using D = std::decay_t<decltype(d)>;
            if constexpr (std::is_same_v<D, bool>)
                d = s != 0.0;
            else if constexpr (std::is_arithmetic_v<D>)
                d = D(s);
        },
        slot);
}

void measureValue(const PlugValue& v, UndoShared& shared, size_t& owned)
{
    if (const MeshPtr* m = std::get_if<MeshPtr>(&v); m && *m)
        shared.add(m->get(), (*m)->memoryBytes());
    else if (const DataPtr* d = std::get_if<DataPtr>(&v); d && *d)
        shared.add(d->get(), (*d)->bytes());
    else if (const std::string* s = std::get_if<std::string>(&v))
        owned += s->capacity();
}

template <class Undo, class Redo> struct LambdaAction final : UndoAction
{
    Undo u;
    Redo r;
    bool selection;
    /** Copies of the plug values the lambdas capture, so the undo budget can see them. */
    std::vector<PlugValue> held;
    LambdaAction(Undo u, Redo r, bool selection, std::vector<PlugValue> held)
        : u(std::move(u)), r(std::move(r)), selection(selection), held(std::move(held)) {}
    void undo() override { u(); }
    void redo() override { r(); }
    bool selectionOnly() const override { return selection; }
    size_t bytes() const override
    {
        size_t n = sizeof(*this);
        UndoShared ignored;
        for (const PlugValue& v : held)
            measureValue(v, ignored, n);
        return n;
    }
    void sharedBytes(UndoShared& shared) const override
    {
        size_t ignored = 0;
        for (const PlugValue& v : held)
            measureValue(v, shared, ignored);
    }
};

template <class Undo, class Redo> void recordUndo(Undo u, Redo r, bool selection = false, std::vector<PlugValue> held = {})
{
    UndoStack::get().record(
        std::make_unique<LambdaAction<Undo, Redo>>(std::move(u), std::move(r), selection, std::move(held)));
}

/** Holds a deleted node so undo can put it back. */
struct DeleteAction final : UndoAction
{
    Graph& g;
    NodeId id;
    NodeId parent;
    size_t index;
    std::unique_ptr<Node> held;
    DeleteAction(Graph& g, NodeId id, NodeId parent, size_t index, std::unique_ptr<Node> held)
        : g(g), id(id), parent(parent), index(index), held(std::move(held)) {}
    void undo() override;
    void redo() override;
    size_t bytes() const override
    {
        size_t n = sizeof(*this);
        if (held)
        {
            UndoShared ignored;
            n += sizeof(Node) + held->name().capacity();
            for (size_t i = 0; i < held->type().attrs.size(); ++i)
                measureValue(held->value(int(i)), ignored, n);
        }
        return n;
    }
    void sharedBytes(UndoShared& shared) const override
    {
        if (!held)
            return;
        size_t ignored = 0;
        for (size_t i = 0; i < held->type().attrs.size(); ++i)
            measureValue(held->value(int(i)), shared, ignored);
    }
};
} // namespace

struct GraphAccess
{
    static void attach(Graph& g, std::unique_ptr<Node> n, NodeId parent, size_t index) { g.attach(std::move(n), parent, index); }
    static std::unique_ptr<Node> detach(Graph& g, NodeId id) { return g.detach(id, nullptr); }
};

void DeleteAction::undo() { GraphAccess::attach(g, std::move(held), parent, index); }
void DeleteAction::redo() { held = GraphAccess::detach(g, id); }

Graph& Graph::current()
{
    static Graph g;
    return g;
}

struct Graph::PlaybackCache
{
    struct Entry
    {
        std::vector<std::pair<int, PlugValue>> values;
        std::vector<std::vector<std::pair<int, PlugValue>>> elements;
        std::shared_ptr<const void> extra;
    };
    std::mutex mutex;
    std::map<int64_t, std::unordered_map<NodeId, Entry>> times;
    /** Meshes already counted in bytes; the cache keeps them alive, so their addresses stay unique. */
    std::unordered_set<const void*> counted;
    size_t bytes = 0;
    size_t limit = 0;

    static int64_t keyOf(double seconds) { return std::llround(seconds * 1e6); }
};

Graph::Graph() { restructured(); }
Graph::~Graph() = default;

void Graph::restructured()
{
    static std::atomic<uint64_t> counter{0};
    mStructure = ++counter;
}

void Graph::setCachedPlayback(bool on)
{
    if (on == cachedPlayback())
        return;
    mPlayback = on ? std::make_unique<PlaybackCache>() : nullptr;
    clearPlaybackCache();
}

void Graph::clearPlaybackCache()
{
    if (!mPlayback)
        return;
    const int64_t mb = PrefStore::get().value<int64_t>("general", kCachedPlaybackMemoryPref, 0);
    std::lock_guard lock(mPlayback->mutex);
    mPlayback->times.clear();
    mPlayback->counted.clear();
    mPlayback->bytes = 0;
    mPlayback->limit = mb > 0 ? size_t(mb) << 20 : physicalMemoryBytes() / 2;
}

std::vector<double> Graph::playbackCacheTimes() const
{
    std::vector<double> out;
    if (!mPlayback)
        return out;
    std::lock_guard lock(mPlayback->mutex);
    for (const auto& [key, nodes] : mPlayback->times)
        out.push_back(double(key) / 1e6);
    return out;
}

size_t Graph::playbackCacheBytes() const
{
    if (!mPlayback)
        return 0;
    std::lock_guard lock(mPlayback->mutex);
    return mPlayback->bytes;
}

bool Graph::restoreCached(Node& n)
{
    PlaybackCache& c = *mPlayback;
    std::shared_ptr<const void> extra;
    {
        std::lock_guard lock(c.mutex);
        auto t = c.times.find(PlaybackCache::keyOf(mCtx.time));
        if (t == c.times.end())
            return false;
        auto e = t->second.find(n.id());
        if (e == t->second.end())
            return false;
        for (const auto& [attr, v] : e->second.values)
            n.mValues[size_t(attr)] = v;
        if (!e->second.elements.empty())
            n.mElements = e->second.elements;
        extra = e->second.extra;
    }
    n.restorePlaybackExtra(extra);
    return true;
}

uint8_t Graph::upstreamPlayback(const Node& n, uint64_t pass) const
{
    uint8_t most = 0;
    auto visit = [&](NodeId up) {
        if (const Node* u = node(up); u && u->mEvaluatedPass == pass)
            most = std::max(most, u->mPlayback);
    };
    if (NodeId p = parent(n.id()); p != kNoNode)
        visit(p);
    for (auto it = mInputs.lower_bound({n.id(), 0, -1}); it != mInputs.end() && it->first.node == n.id(); ++it)
        visit(it->second.node);
    return most;
}

bool Graph::keepCached(Node& n)
{
    PlaybackCache& c = *mPlayback;
    // Outputs and whatever arrives by connection; unconnected inputs only change by an edit, which drops the cache.
    std::vector<bool> keep(n.mValues.size(), false);
    const std::vector<AttrDef>& attrs = n.type().attrs;
    for (size_t i = 0; i < keep.size() && i < attrs.size(); ++i)
        keep[i] = !(attrs[i].flags & AttrInput);
    bool elements = false;
    for (auto it = mInputs.lower_bound({n.id(), 0, -1}); it != mInputs.end() && it->first.node == n.id(); ++it)
    {
        if (it->first.element >= 0)
            elements = true;
        else if (size_t(it->first.attr) < keep.size())
            keep[size_t(it->first.attr)] = true;
    }
    if (n.type().dag)
        keep[Transform::kParentMatrix] = true;

    PlaybackCache::Entry e;
    for (size_t i = 0; i < keep.size(); ++i)
        if (keep[i])
            e.values.emplace_back(int(i), n.mValues[i]);
    if (elements)
        e.elements = n.mElements;
    Node::PlaybackExtra extra = n.playbackExtra(mCtx);
    if (!extra.keep)
        return false;
    e.extra = std::move(extra.data);

    std::lock_guard lock(c.mutex);
    const int64_t key = PlaybackCache::keyOf(mCtx.time);
    auto t = c.times.find(key);
    if (c.bytes >= c.limit && (t == c.times.end() || !t->second.contains(n.id())))
        return false;
    size_t bytes = extra.bytes + e.values.size() * sizeof(PlugValue);
    for (const auto& [attr, v] : e.values)
        if (const MeshPtr* m = std::get_if<MeshPtr>(&v); m && *m && c.counted.insert(m->get()).second)
            bytes += (*m)->memoryBytes();
    c.bytes += bytes;
    c.times[key][n.id()] = std::move(e);
    return true;
}

void Graph::clear()
{
    restructured();
    mNodes.clear();
    mNames.clear();
    mRel.clear();
    mRoots.clear();
    mInputs.clear();
    mOutgoingDst.clear();
    mSelection.clear();
    ++mSelectionSerial;
    mHeld.clear();
    mDisplayTurns.clear();
    mTurned.clear();
    mCtx = {};
    UndoStack::get().clear();
    changed(Change::Cleared, kNoNode);
}

bool Graph::validName(std::string_view name)
{
    if (name.empty())
        return false;
    bool start = true;
    for (char c : name)
    {
        if (c == ':')
        {
            start = true;
            continue;
        }
        bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        bool digit = c >= '0' && c <= '9';
        if (!(alpha || (digit && !start)))
            return false;
        start = false;
    }
    return true;
}

std::string Graph::uniqueName(std::string_view wanted, NodeId parent, NodeId self, bool global) const
{
    auto taken = [&](const std::string& n) {
        auto [b, e] = mNames.equal_range(n);
        for (auto it = b; it != e; ++it)
            if (it->second != self && (global || this->parent(it->second) == parent))
                return true;
        return false;
    };
    std::string base(wanted);
    if (!taken(base))
        return base;

    size_t digits = base.size();
    while (digits > 0 && base[digits - 1] >= '0' && base[digits - 1] <= '9')
        --digits;
    std::string stem = base.substr(0, digits);
    int n = 1;
    if (digits < base.size())
        std::from_chars(base.data() + digits, base.data() + base.size(), n);
    for (;; ++n)
    {
        std::string candidate = stem + std::to_string(n);
        if (!taken(candidate))
            return candidate;
    }
}

std::string Graph::conformName(const NodeType& type, std::string_view wanted, bool userGiven) const
{
    std::string fixed = mNaming.conform(type, wanted);
    if (!userGiven || fixed == wanted)
        return fixed;
    switch (mNaming.mode)
    {
    case NamingPolicy::Mode::Fix:
        if (!ops::agentCalling())
            achievements::unlock("NAME_AND_SHAME");
        return fixed;
    case NamingPolicy::Mode::Warn:
        log::warn("'{}' breaks the naming convention (expected '{}')", wanted, fixed);
        return std::string(wanted);
    case NamingPolicy::Mode::Block:
        throw std::invalid_argument("'" + std::string(wanted) + "' breaks the naming convention; use '" + fixed + "'");
    }
    return fixed;
}

namespace
{
/**
 * A name nobody chose (empty, or a command's own default like "Cube" when its call has no name flag) is made unique
 * across the scene, so new nodes don't make existing names ambiguous.
 */
bool defaultName(std::string_view name, bool applyNaming)
{
    if (name.empty())
        return true;
    const Args* running = ops::currentArgs();
    return applyNaming && running && !running->has("name");
}
} // namespace

std::string Graph::availableName(std::string_view wanted, NodeId parent) const
{
    return uniqueName(wanted, parent, kNoNode, mNaming.uniqueNames);
}

void Graph::eraseName(const std::string& name, NodeId id)
{
    auto [b, e] = mNames.equal_range(name);
    for (auto it = b; it != e; ++it)
        if (it->second == id)
        {
            mNames.erase(it);
            return;
        }
}

std::vector<NodeId>& Graph::siblings(NodeId parent)
{
    return parent == kNoNode ? mRoots : mRel[parent].children;
}

void Graph::attach(std::unique_ptr<Node> n, NodeId parent, size_t index)
{
    restructured();
    NodeId id = n->id();
    mNames.emplace(n->mName, id);
    n->mGraph = this;
    if (n->type().dag)
    {
        mRel[id].parent = parent;
        auto& sib = siblings(parent);
        sib.insert(sib.begin() + std::min(index, sib.size()), id);
    }
    n->mDirty = true;
    mAnyDirty = true;
    mNodes.emplace(id, std::move(n));
    changed(Change::Created, id);
}

std::unique_ptr<Node> Graph::detach(NodeId id, size_t* indexOut)
{
    auto it = mNodes.find(id);
    if (it == mNodes.end())
        return nullptr;
    restructured();
    std::unique_ptr<Node> n = std::move(it->second);
    mNodes.erase(it);
    eraseName(n->mName, id);
    if (std::erase(mSelection, id))
        ++mSelectionSerial;
    if (auto rel = mRel.find(id); rel != mRel.end())
    {
        auto& sib = siblings(rel->second.parent);
        auto pos = std::find(sib.begin(), sib.end(), id);
        if (indexOut)
            *indexOut = size_t(pos - sib.begin());
        if (pos != sib.end())
            sib.erase(pos);
        mRel.erase(rel);
    }
    changed(Change::Deleted, id);
    return n;
}

Node* Graph::createNode(const NodeType& type, std::string_view name, NodeId parent, bool applyNaming)
{
    LILA_ZONE();

    if (!type.create)
        throw std::invalid_argument("node type '" + type.name + "' is abstract");
    if (parent != kNoNode && (!node(parent) || !node(parent)->type().dag))
        throw std::invalid_argument("parent must be a transform");
    if (!name.empty() && !validName(name))
        throw std::invalid_argument("invalid name '" + std::string(name) + "'");

    std::unique_ptr<Node> n = type.create();
    n->mId = mNextId++;
    n->mType = &type.registered();
    // Default names follow the convention's style even when enforcement is off.
    std::string fallback = applyCase(type.name, mNaming.styleFor(type));
    NodeId p = type.dag ? parent : kNoNode;
    n->mName = uniqueName(name.empty() ? fallback : applyNaming ? conformName(type, name, true) : std::string(name), p,
                          kNoNode, mNaming.uniqueNames || defaultName(name, applyNaming));
    if (auto colon = n->mName.rfind(':'); colon != std::string::npos)
        n->mNamespace = n->mName.substr(0, colon);
    n->mValues.reserve(type.attrs.size());
    for (const AttrDef& a : type.registered().attrs)
        n->mValues.push_back(a.defaultValue);
    // A copy of a node with user attrs (type is its private type) gets them too, as part of being created.
    if (type.userOf)
    {
        n->mOwnType = std::make_shared<NodeType>(type);
        n->mType = n->mOwnType.get();
        for (size_t i = n->mValues.size(); i < type.attrs.size(); ++i)
            n->mValues.push_back(type.attrs[i].defaultValue);
    }

    NodeId id = n->id();
    attach(std::move(n), p, SIZE_MAX);

    auto action = std::make_unique<DeleteAction>(*this, id, p, siblings(p).size() - 1, nullptr);
    // Recorded as an inverted delete: undo removes, redo restores.
    struct CreateAction final : UndoAction
    {
        std::unique_ptr<DeleteAction> del;
        void undo() override { del->redo(); }
        void redo() override { del->undo(); }
    };
    auto create = std::make_unique<CreateAction>();
    create->del = std::move(action);
    UndoStack::get().record(std::move(create));
    return node(id);
}

void Graph::setReference(NodeId id, uint32_t reference)
{
    if (Node* n = node(id))
        n->mReference = reference;
}

void Graph::deleteNode(NodeId id)
{
    LILA_ZONE();

    if (!node(id))
        return;
    struct Depth
    {
        int& d;
        explicit Depth(int& v) : d(v) { ++d; }
        ~Depth() { --d; }
    };
    if (mDeleteHook && mDeleteDepth == 0)
    {
        std::vector<NodeId> doomed = descendants(id);
        doomed.insert(doomed.begin(), id);
        Depth hook(mDeleteDepth);
        mDeleteHook(*this, doomed);
    }
    Depth inner(mDeleteDepth);
    for (NodeId child : std::vector<NodeId>(children(id)))
        deleteNode(child);

    std::vector<PlugRef> inbound;
    for (auto& [dst, src] : mInputs)
        if (dst.node == id || src.node == id)
            inbound.push_back(dst);
    for (const PlugRef& dst : inbound)
        disconnect(dst);

    if (isSelected(id))
    {
        std::vector<NodeId> sel = mSelection;
        std::erase(sel, id);
        select(sel, SelectMode::Replace);
    }

    NodeId p = parent(id);
    size_t index = 0;
    std::unique_ptr<Node> held = detach(id, &index);
    UndoStack::get().record(std::make_unique<DeleteAction>(*this, id, p, index, std::move(held)));
}

Node* Graph::node(NodeId id) const
{
    auto it = mNodes.find(id);
    return it == mNodes.end() ? nullptr : it->second.get();
}

bool Graph::matchesPath(NodeId id, const std::vector<std::string_view>& segs, bool absolute) const
{
    NodeId cur = id;
    for (size_t i = segs.size() - 1; i-- > 0;)
    {
        cur = parent(cur);
        if (cur == kNoNode || node(cur)->mName != segs[i])
            return false;
    }
    return !absolute || parent(cur) == kNoNode;
}

std::vector<NodeId> Graph::findAll(std::string_view path) const
{
    bool absolute = !path.empty() && path[0] == '|';
    if (absolute)
        path.remove_prefix(1);
    std::vector<std::string_view> segs;
    for (size_t start = 0;;)
    {
        size_t bar = path.find('|', start);
        segs.push_back(path.substr(start, bar == std::string_view::npos ? std::string_view::npos : bar - start));
        if (segs.back().empty())
            return {};
        if (bar == std::string_view::npos)
            break;
        start = bar + 1;
    }
    std::vector<NodeId> out;
    auto [b, e] = mNames.equal_range(std::string(segs.back()));
    for (auto it = b; it != e; ++it)
        if ((segs.size() == 1 && !absolute) || matchesPath(it->second, segs, absolute))
            out.push_back(it->second);
    std::sort(out.begin(), out.end());
    return out;
}

Node* Graph::find(std::string_view path) const
{
    if (path.find('|') == std::string_view::npos)
    {
        auto [b, e] = mNames.equal_range(std::string(path));
        if (b == e)
            return nullptr;
        if (std::next(b) == e)
            return node(b->second);
    }
    std::vector<NodeId> all = findAll(path);
    if (all.empty())
        return nullptr;
    if (all.size() == 1)
        return node(all[0]);
    std::string list;
    for (NodeId id : all)
        list += (list.empty() ? "" : ", ") + fullPath(id);
    throw CommandError("'" + std::string(path) + "' is ambiguous: it matches " + list + "; use a path");
}

std::string Graph::fullPath(NodeId id) const
{
    std::vector<const std::string*> chain;
    for (NodeId cur = id; cur != kNoNode; cur = parent(cur))
        if (const Node* n = node(cur))
            chain.push_back(&n->mName);
        else
            break;
    std::string out;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        out += "|" + **it;
    return out;
}

std::string Graph::path(NodeId id) const
{
    const Node* n = node(id);
    if (!n)
        return {};
    auto [b, e] = mNames.equal_range(n->mName);
    if (b == e || std::next(b) == e)
        return n->mName;
    std::vector<std::string_view> chain; // leaf first
    for (NodeId cur = id; cur != kNoNode; cur = parent(cur))
        chain.push_back(node(cur)->mName);
    std::vector<std::string_view> segs;
    for (size_t k = 1; k <= chain.size(); ++k)
    {
        segs.assign(chain.begin(), chain.begin() + std::ptrdiff_t(k));
        std::reverse(segs.begin(), segs.end());
        int matches = 0;
        for (auto it = b; it != e && matches < 2; ++it)
            matches += matchesPath(it->second, segs, false) ? 1 : 0;
        if (matches == 1)
        {
            std::string out;
            for (std::string_view seg : segs)
                out += (out.empty() ? "" : "|") + std::string(seg);
            return out;
        }
    }
    return fullPath(id);
}

std::vector<NodeId> Graph::nodes() const
{
    std::scoped_lock lock(mSortedMutex);
    if (mSortedStructure != mStructure)
    {
        mSorted.clear();
        mSorted.reserve(mNodes.size());
        for (auto& [id, n] : mNodes)
            mSorted.push_back(id);
        std::sort(mSorted.begin(), mSorted.end());
        mSortedStructure = mStructure;
    }
    return mSorted;
}

void Graph::renameRaw(NodeId id, const std::string& name)
{
    Node* n = node(id);
    eraseName(n->mName, id);
    n->mName = name;
    auto colon = name.rfind(':');
    n->mNamespace = colon == std::string::npos ? std::string() : name.substr(0, colon);
    mNames.emplace(name, id);
    changed(Change::Renamed, id);
}

std::string Graph::rename(NodeId id, std::string_view name)
{
    Node* n = node(id);
    if (!n)
        throw std::invalid_argument("no such node");
    if (!validName(name))
        throw std::invalid_argument("invalid name '" + std::string(name) + "'");
    std::string oldName = n->mName;
    std::string newName = uniqueName(conformName(n->type(), name, true), parent(id), id, mNaming.uniqueNames);
    if (newName == oldName)
        return newName;
    renameRaw(id, newName);
    recordUndo([this, id, oldName] { renameRaw(id, oldName); }, [this, id, newName] { renameRaw(id, newName); });
    return newName;
}

NodeId Graph::parent(NodeId id) const
{
    auto it = mRel.find(id);
    return it == mRel.end() ? kNoNode : it->second.parent;
}

const std::vector<NodeId>& Graph::children(NodeId id) const
{
    auto it = mRel.find(id);
    return it == mRel.end() ? kNoChildren : it->second.children;
}

std::vector<NodeId> Graph::descendants(NodeId id) const
{
    std::vector<NodeId> out;
    std::vector<NodeId> stack(children(id).rbegin(), children(id).rend());
    while (!stack.empty())
    {
        NodeId n = stack.back();
        stack.pop_back();
        out.push_back(n);
        const auto& c = children(n);
        stack.insert(stack.end(), c.rbegin(), c.rend());
    }
    return out;
}

void Graph::reparentRaw(NodeId child, NodeId newParent, size_t index)
{
    auto& rel = mRel[child];
    auto& oldSib = siblings(rel.parent);
    std::erase(oldSib, child);
    rel.parent = newParent;
    auto& sib = siblings(newParent);
    sib.insert(sib.begin() + std::min(index, sib.size()), child);
    markDirty(child);
    changed(Change::Reparented, child);
}

void Graph::setParent(NodeId child, NodeId newParent, bool keepWorld, size_t index)
{
    LILA_ZONE();

    Node* c = node(child);
    if (!c || !c->type().dag)
        throw std::invalid_argument("only transforms can be parented");
    if (newParent != kNoNode)
    {
        Node* p = node(newParent);
        if (!p || !p->type().dag)
            throw std::invalid_argument("parent must be a transform");
        for (NodeId a = newParent; a != kNoNode; a = parent(a))
            if (a == child)
                throw std::invalid_argument("cannot parent a node under itself");
    }
    NodeId oldParent = parent(child);
    auto& oldSib = siblings(oldParent);
    size_t oldIndex = size_t(std::find(oldSib.begin(), oldSib.end(), child) - oldSib.begin());
    if (oldParent == newParent)
    {
        size_t newIndex = std::min(index, oldSib.size() - 1);
        if (index == SIZE_MAX || newIndex == oldIndex)
            return;
        reparentRaw(child, newParent, newIndex);
        recordUndo([this, child, oldParent, oldIndex] { reparentRaw(child, oldParent, oldIndex); },
                   [this, child, newParent, newIndex] { reparentRaw(child, newParent, newIndex); });
        return;
    }

    glm::dmat4 world(1.0);
    if (keepWorld)
    {
        evaluate();
        world = c->as<glm::dmat4>(Transform::kWorldMatrix);
    }

    reparentRaw(child, newParent, index);
    const auto& newSib = siblings(newParent);
    size_t newIndex = size_t(std::find(newSib.begin(), newSib.end(), child) - newSib.begin());
    recordUndo([this, child, oldParent, oldIndex] { reparentRaw(child, oldParent, oldIndex); },
               [this, child, newParent, newIndex] { reparentRaw(child, newParent, newIndex); });
    // Sibling names stay unique, so only a clash under the new parent renames.
    if (std::string oldName = c->mName, fixed = uniqueName(oldName, newParent, child, mNaming.uniqueNames); fixed != oldName)
    {
        renameRaw(child, fixed);
        recordUndo([this, child, oldName] { renameRaw(child, oldName); }, [this, child, fixed] { renameRaw(child, fixed); });
    }

    if (keepWorld)
    {
        glm::dmat4 parentWorld(1.0);
        if (newParent != kNoNode)
            parentWorld = node(newParent)->as<glm::dmat4>(Transform::kWorldMatrix);
        Transform::setLocalMatrix(*this, child, glm::inverse(parentWorld) * world);
    }
}

PlugRef Graph::plug(std::string_view path) const
{
    auto dot = path.find('.');
    if (dot == std::string_view::npos)
        throw std::invalid_argument("expected node.attr, got '" + std::string(path) + "'");
    Node* n = find(path.substr(0, dot));
    if (!n)
        throw std::invalid_argument("no node named '" + std::string(path.substr(0, dot)) + "'");
    std::string_view attrName = path.substr(dot + 1);
    int element = -1;
    std::string grouped;
    if (size_t close = attrName.find("]."); close != std::string_view::npos)
    {
        // group[3].child names the child attr "group.child", element 3.
        size_t open = attrName.rfind('[', close);
        element = open == std::string_view::npos ? -1 : parseIndex(attrName.substr(open + 1, close - open - 1));
        if (element < 0)
            throw std::invalid_argument("bad element index in '" + std::string(path) + "'");
        grouped = std::string(attrName.substr(0, open)) + "." + std::string(attrName.substr(close + 2));
        attrName = grouped;
    }
    else if (attrName.ends_with(']'))
    {
        size_t open = attrName.rfind('[');
        element = open == std::string_view::npos ? -1 : parseIndex(attrName.substr(open + 1, attrName.size() - open - 2));
        if (element < 0)
            throw std::invalid_argument("bad element index in '" + std::string(path) + "'");
        attrName = attrName.substr(0, open);
    }
    const auto& attrs = n->type().attrs;
    for (int i = 0; i < int(attrs.size()); ++i)
    {
        const AttrDef& a = attrs[i];
        if (a.name == attrName || a.shortName == attrName)
        {
            if (element >= 0 && !isArray(a))
                throw std::invalid_argument("'" + n->name() + "." + a.name + "' is not an array");
            return {n->id(), i, -1, element};
        }
        if (a.type == PlugType::Vec3 && element < 0)
            for (int c = 0; c < 3; ++c)
                if (a.childNames[c] == attrName || a.childShort[c] == attrName)
                    return {n->id(), i, c};
    }
    // Numbered attrs an array replaced, as older scene files and scripts name them.
    if (element < 0)
        if (int k = -1, i = n->type().fileAttrIndex(attrName, &k); i >= 0 && k >= 0)
            return {n->id(), i, -1, k};
    throw std::invalid_argument("'" + n->name() + "' has no attribute '" + std::string(attrName) + "'");
}

std::string Graph::plugName(const PlugRef& p) const
{
    Node* n = node(p.node);
    if (!n)
        return "?";
    const AttrDef& a = n->type().attrs[p.attr];
    return path(p.node) + "." + attrPart(a, p);
}

std::string Graph::fullPlugName(const PlugRef& p) const
{
    Node* n = node(p.node);
    if (!n)
        return "?";
    const AttrDef& a = n->type().attrs[p.attr];
    return fullPath(p.node) + "." + attrPart(a, p);
}

const AttrDef& Graph::attr(const PlugRef& p) const
{
    return node(p.node)->type().attrs[p.attr];
}

void Graph::setRaw(const PlugRef& p, const PlugValue& v)
{
    Node* n = node(p.node);
    if (p.element >= 0)
    {
        elementRaw(p) = v;
        markDirty(p.node);
        changed(Change::Value, p.node, &p);
        return;
    }
    PlugValue& slot = n->mValues[p.attr];
    if (p.component >= 0)
        std::get<glm::dvec3>(slot)[p.component] = std::get<double>(v);
    else
        slot = v;
    markDirty(p.node);
    changed(Change::Value, p.node, &p);
}

namespace
{
uint8_t lockBits(const AttrDef& a, int component)
{
    if (a.type != PlugType::Vec3)
        return 1;
    return component >= 0 ? uint8_t(1u << component) : uint8_t(7);
}
} // namespace

bool Graph::isLocked(const PlugRef& p) const
{
    const Node* n = node(p.node);
    if (!n || p.attr < 0)
        return false;
    uint8_t bits = lockBits(n->type().attrs[p.attr], p.component);
    return (n->lockMask(p.attr) & bits) == bits;
}

bool Graph::anyLocked(const PlugRef& p) const
{
    const Node* n = node(p.node);
    return n && p.attr >= 0 && (n->lockMask(p.attr) & lockBits(n->type().attrs[p.attr], p.component)) != 0;
}

void Graph::setLocked(const PlugRef& p, bool locked)
{
    Node* n = node(p.node);
    if (!n || p.attr < 0)
        throw std::invalid_argument("no such plug");
    uint8_t old = n->lockMask(p.attr), bits = lockBits(n->type().attrs[p.attr], p.component);
    uint8_t now = locked ? uint8_t(old | bits) : uint8_t(old & ~bits);
    if (now == old)
        return;
    NodeId id = p.node;
    int attr = p.attr;
    setLockRaw(id, attr, now);
    recordUndo([this, id, attr, old] { setLockRaw(id, attr, old); }, [this, id, attr, now] { setLockRaw(id, attr, now); });
}

void Graph::setLockRaw(NodeId id, int attr, uint8_t mask)
{
    Node* n = node(id);
    if (!n)
        return;
    if (n->mLocks.size() <= size_t(attr))
        n->mLocks.resize(n->type().attrs.size());
    n->mLocks[size_t(attr)] = mask;
    const PlugRef plug{id, attr, -1};
    changed(Change::Value, id, &plug);
}

PlugValue Graph::keepLocked(const PlugRef& p, PlugValue v) const
{
    const Node* n = node(p.node);
    if (!n || p.attr < 0)
        return v;
    uint8_t mask = n->lockMask(p.attr);
    if (!mask)
        return v;
    if (isLocked(p))
        return p.component >= 0 ? PlugValue(std::get<glm::dvec3>(n->value(p.attr))[p.component]) : n->value(p.attr);
    if (glm::dvec3* x = std::get_if<glm::dvec3>(&v); x && p.component < 0)
        for (int c = 0; c < 3; ++c)
            if (mask & (1u << c))
                (*x)[c] = std::get<glm::dvec3>(n->value(p.attr))[c];
    return v;
}

void Graph::setValue(const PlugRef& p, PlugValue v)
{
    Node* n = node(p.node);
    if (!n)
        throw std::invalid_argument("no such node");
    const AttrDef& a = n->type().attrs[p.attr];
    if (!(a.flags & AttrInput))
        throw std::invalid_argument(plugName(p) + " is an output and can't be set");
    if (isArray(a) && !(a.flags & AttrStorable))
        throw std::invalid_argument(plugName(p) + " is an array input and takes connections, not values");
    if (isArray(a) && (p.element < 0 || p.component >= 0))
        throw std::invalid_argument(plugName(p) + " is an array: set one element (" + plugName(p) + "[0]), or all with setElements");
    if (a.type == PlugType::Int || a.type == PlugType::Float || a.type == PlugType::Vec3)
        clampToLimits(a, v);
    if (uint8_t mask = n->lockMask(p.attr))
    {
        const glm::dvec3* x = std::get_if<glm::dvec3>(&v);
        if (p.component >= 0 || !x)
        {
            if (isLocked(p))
                throw std::invalid_argument(plugName(p) + " is locked and can't be changed");
        }
        else
            for (int c = 0; c < 3; ++c)
                if ((mask & (1u << c)) && (*x)[c] != std::get<glm::dvec3>(n->mValues[p.attr])[c])
                    throw std::invalid_argument(plugName({p.node, p.attr, c}) + " is locked and can't be changed");
    }
    if (auto src = source(p))
    {
        double d = 0.0;
        if (const glm::dvec3* vec = std::get_if<glm::dvec3>(&v); vec && p.component < 0)
        {
            if (!setDrivenVec(p, *src, *vec))
                throw std::invalid_argument(plugName(p) + " is driven by a connection");
            return;
        }
        if (!scalarOf(v, d) || !setDriven(p, *src, d))
            throw std::invalid_argument(plugName(p) + " is driven by a connection");
        return;
    }
    if (p.component >= 0)
        if (auto src = source({p.node, p.attr, -1}))
        {
            double d = 0.0;
            glm::dvec3 whole = std::get<glm::dvec3>(value({p.node, p.attr, -1}));
            if (!scalarOf(v, d))
                throw std::invalid_argument(plugName(p) + " is driven by a connection");
            whole[p.component] = d;
            if (!setDrivenVec({p.node, p.attr, -1}, *src, whole))
                throw std::invalid_argument(plugName(p) + " is driven by a connection");
            return;
        }

    if (p.element >= 0)
    {
        const PlugValue* was = nullptr;
        for (const auto& [i, ev] : n->elements(p.attr))
            if (i == p.element)
                was = &ev;
        if (was && *was == v)
            return;
        std::optional<PlugValue> old = was ? std::optional(*was) : std::nullopt;
        setRaw(p, v);
        recordUndo(
            [this, p, old] {
                if (old)
                    setRaw(p, *old);
                else
                    eraseElementRaw(p);
            },
            [this, p, v] { setRaw(p, v); });
        return;
    }
    PlugValue old = p.component >= 0 ? PlugValue(std::get<glm::dvec3>(n->mValues[p.attr])[p.component]) : n->mValues[p.attr];
    if (old == v)
        return;
    setRaw(p, v);
    std::vector<PlugValue> held;
    if (std::holds_alternative<MeshPtr>(v) || std::holds_alternative<DataPtr>(v) || std::holds_alternative<std::string>(v))
        held = {old, v};
    recordUndo([this, p, old] { setRaw(p, old); }, [this, p, v] { setRaw(p, v); }, false, std::move(held));

    if (p.component < 0 && a.type == PlugType::Vec3)
        for (int c = 0; c < 3; ++c)
        {
            PlugRef comp{p.node, p.attr, c};
            double now = std::get<glm::dvec3>(v)[c];
            if (auto src = source(comp); src && now != std::get<glm::dvec3>(old)[c])
                setDriven(comp, *src, now);
        }
}

bool Graph::setDriven(const PlugRef& dst, const PlugRef& src, double v)
{
    DrivenEdit edit = mDrivenSet ? mDrivenSet(*this, dst, src, v) : DrivenEdit::Reject;
    if (edit == DrivenEdit::Reject)
        return false;
    if (edit == DrivenEdit::Hold)
    {
        bool was = mHeld.contains(dst);
        double old = 0.0;
        const PlugValue& slot = node(dst.node)->mValues[dst.attr];
        if (dst.component >= 0)
            old = std::get<glm::dvec3>(slot)[dst.component];
        else
            scalarOf(slot, old);
        holdRaw(dst, v);
        recordUndo([this, dst, was, old] { was ? holdRaw(dst, old) : releaseRaw(dst); }, [this, dst, v] { holdRaw(dst, v); });
    }
    return true;
}

bool Graph::setDrivenVec(const PlugRef& dst, const PlugRef& src, const glm::dvec3& v)
{
    if (src.component >= 0 || attr(src).type != PlugType::Vec3)
        return false;
    DrivenEdit edit = mDrivenVecSet ? mDrivenVecSet(*this, dst, src, v) : DrivenEdit::Reject;
    if (edit == DrivenEdit::Reject)
        return false;
    if (edit == DrivenEdit::Hold)
    {
        bool was = mHeld.contains(dst);
        glm::dvec3 old = std::get<glm::dvec3>(node(dst.node)->mValues[dst.attr]);
        holdRaw(dst, v);
        recordUndo([this, dst, was, old] { was ? holdRaw(dst, old) : releaseRaw(dst); }, [this, dst, v] { holdRaw(dst, v); });
    }
    return true;
}

void Graph::holdRaw(const PlugRef& dst, const PlugValue& v)
{
    PlugValue& slot = node(dst.node)->mValues[dst.attr];
    if (const glm::dvec3* vec = std::get_if<glm::dvec3>(&v))
        slot = *vec;
    else if (dst.component >= 0)
        std::get<glm::dvec3>(slot)[dst.component] = std::get<double>(v);
    else
        assignScalar(slot, std::get<double>(v));
    markDirty(dst.node);
    mHeld.insert(dst);
    changed(Change::Value, dst.node, &dst);
}

void Graph::releaseRaw(const PlugRef& dst)
{
    mHeld.erase(dst);
    markDirty(dst.node);
    changed(Change::Value, dst.node, &dst);
}

PlugValue Graph::value(const PlugRef& p)
{
    Node* n = node(p.node);
    if (!n)
        throw std::invalid_argument("no such node");
    // Only what this plug depends on: reading a joint mid-drag must not recompute the skinned meshes downstream of it.
    if (mAnyDirty && n->mDirty)
    {
        const std::vector<NodeId> one{p.node};
        evaluateWith(&one);
    }
    if (p.element >= 0)
    {
        for (const auto& [i, v] : n->elements(p.attr))
            if (i == p.element)
                return v;
        return n->type().attrs[p.attr].defaultValue;
    }
    const PlugValue& v = n->mValues[p.attr];
    if (p.component >= 0)
        return std::get<glm::dvec3>(v)[p.component];
    return v;
}

bool Graph::dependsOn(NodeId n, NodeId upstream) const
{
    std::vector<NodeId> stack{n};
    std::unordered_set<NodeId> seen;
    while (!stack.empty())
    {
        NodeId cur = stack.back();
        stack.pop_back();
        if (cur == upstream)
            return true;
        if (!seen.insert(cur).second)
            continue;
        for (auto& [dst, src] : mInputs)
            if (dst.node == cur)
                stack.push_back(src.node);
        if (NodeId p = parent(cur); p != kNoNode)
            stack.push_back(p);
    }
    return false;
}

void Graph::connectRaw(const PlugRef& src, const PlugRef& dst)
{
    if (dst.element >= 0 && node(dst.node))
        elementRaw(dst);
    mHeld.erase(dst);
    mInputs[dst] = src;
    mOutgoingDst[src.node].push_back(dst);
    markDirty(dst.node);
    changed(Change::Connection, dst.node, &dst);
}

void Graph::disconnectRaw(const PlugRef& dst)
{
    auto it = mInputs.find(dst);
    if (it == mInputs.end())
        return;
    std::erase(mOutgoingDst[it->second.node], dst);
    mInputs.erase(it);
    mHeld.erase(dst);
    if (Node* n = node(dst.node))
    {
        // A storable array keeps the element, holding the last value pulled through; an input-only one drops it.
        if (dst.element >= 0)
        {
            if (!(n->type().attrs[size_t(dst.attr)].flags & AttrStorable))
                eraseElementRaw(dst);
        }
        else if (const AttrDef& a = n->type().attrs[size_t(dst.attr)]; a.flags & AttrResetOnDisconnect)
            n->mValues[size_t(dst.attr)] = a.defaultValue;
    }
    markDirty(dst.node);
    changed(Change::Connection, dst.node, &dst);
}

PlugRef Graph::connect(const PlugRef& src, const PlugRef& to, bool force)
{
    const AttrDef& sa = attr(src);
    const AttrDef& da = attr(to);
    if (!(da.flags & AttrInput))
        throw std::invalid_argument(plugName(to) + " is not an input");
    if (src.element >= 0)
        throw std::invalid_argument("array elements are inputs and can't drive " + plugName(to));
    if (to.element >= 0 && !isArray(da))
        throw std::invalid_argument(plugName({to.node, to.attr}) + " is not an array");
    if (isArray(da) && to.component >= 0)
        throw std::invalid_argument("array inputs have no components");
    PlugRef dst = to;
    if (isArray(da) && dst.element < 0)
    {
        std::vector<PlugRef> used = elements(dst);
        dst.element = used.empty() ? 0 : used.back().element + 1;
    }
    if ((src.component >= 0 && sa.type != PlugType::Vec3) || (dst.component >= 0 && da.type != PlugType::Vec3))
        throw std::invalid_argument("only vec3 plugs have components");
    PlugType st = src.component >= 0 ? PlugType::Float : sa.type;
    PlugType dt = dst.component >= 0 ? PlugType::Float : da.type;
    auto numeric = [](PlugType t) { return t == PlugType::Float || t == PlugType::Int || t == PlugType::Bool || t == PlugType::Enum; };
    if (st != dt && !(numeric(st) && numeric(dt)))
        throw std::invalid_argument(std::string("type mismatch: ") + typeName(st) + " -> " + typeName(dt));
    if (src.node == dst.node || dependsOn(src.node, dst.node))
        throw std::invalid_argument("connection would create a cycle");

    std::vector<PlugRef> clashing;
    if (dst.component >= 0)
    {
        if (source({dst.node, dst.attr, -1}))
            clashing.push_back({dst.node, dst.attr, -1});
    }
    else if (da.type == PlugType::Vec3)
        for (int c = 0; c < 3; ++c)
            if (source({dst.node, dst.attr, c}))
                clashing.push_back({dst.node, dst.attr, c});
    if (!clashing.empty())
    {
        if (!force)
            throw std::invalid_argument(plugName(dst) + " overlaps a driven plug; use force");
        for (const PlugRef& c : clashing)
            disconnect(c);
    }

    if (auto existing = source(dst))
    {
        if (*existing == src)
            return dst;
        if (!force)
            throw std::invalid_argument(plugName(dst) + " is already connected; use force");
        disconnect(dst);
    }
    // Undo puts back the value the plug held before it was driven, not the last one pulled through the connection.
    std::optional<PlugValue> before;
    bool newElement = false;
    if (dst.element >= 0)
    {
        const auto& els = node(dst.node)->elements(dst.attr);
        auto it = std::ranges::find(els, dst.element, [](const auto& e) { return e.first; });
        newElement = it == els.end();
        if (!newElement && da.type != PlugType::Mesh && da.type != PlugType::Data)
            before = it->second;
    }
    else if (da.type != PlugType::Mesh && da.type != PlugType::Data)
    {
        const PlugValue& v = node(dst.node)->value(dst.attr);
        before = dst.component >= 0 ? PlugValue(std::get<glm::dvec3>(v)[dst.component]) : v;
    }
    connectRaw(src, dst);
    recordUndo(
        [this, dst, before, newElement] {
            disconnectRaw(dst);
            if (newElement)
                eraseElementRaw(dst);
            else if (before && node(dst.node))
                setRaw(dst, *before);
        },
        [this, src, dst] { connectRaw(src, dst); });
    return dst;
}

void Graph::disconnect(const PlugRef& dst)
{
    auto src = source(dst);
    if (!src)
        return;
    PlugRef s = *src;
    disconnectRaw(dst);
    recordUndo([this, s, dst] { connectRaw(s, dst); }, [this, dst] { disconnectRaw(dst); });
}

std::optional<PlugRef> Graph::source(const PlugRef& dst) const
{
    auto it = mInputs.find(dst);
    return it == mInputs.end() ? std::nullopt : std::optional(it->second);
}

std::vector<PlugRef> Graph::elements(const PlugRef& array) const
{
    std::vector<PlugRef> out;
    if (Node* n = node(array.node))
        for (const auto& [i, v] : n->elements(array.attr))
            out.push_back({array.node, array.attr, -1, i});
    return out;
}

std::vector<int> Graph::groupIndices(NodeId id, std::string_view group) const
{
    std::vector<int> out;
    if (Node* n = node(id))
        for (int a : n->type().groupChildren(group))
            for (const auto& [i, v] : n->elements(a))
                out.push_back(i);
    std::ranges::sort(out);
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

PlugValue& Graph::elementRaw(const PlugRef& p)
{
    Node* n = node(p.node);
    if (n->mElements.size() < n->type().attrs.size())
        n->mElements.resize(n->type().attrs.size());
    auto& els = n->mElements[size_t(p.attr)];
    auto at = std::lower_bound(els.begin(), els.end(), p.element, [](const auto& e, int i) { return e.first < i; });
    if (at == els.end() || at->first != p.element)
        at = els.insert(at, {p.element, n->type().attrs[size_t(p.attr)].defaultValue});
    return at->second;
}

void Graph::eraseElementRaw(const PlugRef& p)
{
    Node* n = node(p.node);
    if (!n || size_t(p.attr) >= n->mElements.size())
        return;
    std::erase_if(n->mElements[size_t(p.attr)], [&](const auto& e) { return e.first == p.element; });
    markDirty(p.node);
    changed(Change::Value, p.node, &p);
}

void Graph::setElementsRaw(const PlugRef& array, const std::vector<std::pair<int, PlugValue>>& elements)
{
    Node* n = node(array.node);
    if (n->mElements.size() < n->type().attrs.size())
        n->mElements.resize(n->type().attrs.size());
    n->mElements[size_t(array.attr)] = elements;
    markDirty(array.node);
    changed(Change::Value, array.node, &array);
}

void Graph::setElements(const PlugRef& array, const std::vector<PlugValue>& values)
{
    Node* n = node(array.node);
    if (!n)
        throw std::invalid_argument("no such node");
    const AttrDef& a = n->type().attrs[size_t(array.attr)];
    if (!isArray(a) || !(a.flags & AttrStorable) || !(a.flags & AttrInput))
        throw std::invalid_argument(plugName({array.node, array.attr}) + " is not an array that holds values");
    if (isLocked({array.node, array.attr}))
        throw std::invalid_argument(plugName({array.node, array.attr}) + " is locked and can't be changed");
    for (const PlugRef& e : elements({array.node, array.attr}))
        if (source(e))
            throw std::invalid_argument(plugName(e) + " is driven by a connection");
    std::vector<std::pair<int, PlugValue>> before = n->elements(array.attr), after;
    for (size_t i = 0; i < values.size(); ++i)
        after.emplace_back(int(i), values[i]);
    if (before == after)
        return;
    PlugRef whole{array.node, array.attr};
    setElementsRaw(whole, after);
    recordUndo([this, whole, before] { setElementsRaw(whole, before); }, [this, whole, after] { setElementsRaw(whole, after); });
}

void Graph::removeElement(const PlugRef& e)
{
    Node* n = node(e.node);
    if (!n || e.element < 0)
        throw std::invalid_argument("removeElement: expected an element plug (node.attr[n])");
    disconnect(e);
    const auto& els = n->elements(e.attr);
    auto it = std::ranges::find(els, e.element, [](const auto& x) { return x.first; });
    if (it == els.end())
        return;
    PlugValue was = it->second;
    eraseElementRaw(e);
    recordUndo([this, e, was] { setRaw(e, was); }, [this, e] { eraseElementRaw(e); });
}

void Graph::copyElements(NodeId from, NodeId to, int attr, bool shareSources)
{
    const bool stores = (node(from)->type().attrs[size_t(attr)].flags & AttrStorable) != 0;
    for (const PlugRef& e : elements({from, attr}))
    {
        PlugRef dst{to, attr, -1, e.element};
        if (auto src = source(e))
        {
            if (shareSources)
                connect(*src, dst, true);
        }
        else if (stores)
            setValue(dst, value(e));
    }
}

void Graph::shiftAttrRefs(NodeId id, int from, int delta)
{
    remapAttrRefs(id, [&](int attr) { return attr >= from ? attr + delta : attr; });
}

void Graph::remapAttrRefs(NodeId id, const std::function<int(int)>& map)
{
    auto moved = [&](PlugRef p) {
        if (p.node == id)
            p.attr = map(p.attr);
        return p;
    };
    std::map<PlugRef, PlugRef> inputs;
    for (const auto& [dst, src] : mInputs)
        inputs.emplace(moved(dst), moved(src));
    mInputs = std::move(inputs);
    for (auto& [srcNode, dsts] : mOutgoingDst)
        for (PlugRef& d : dsts)
            d = moved(d);
    std::set<PlugRef> held;
    for (const PlugRef& h : mHeld)
        held.insert(moved(h));
    mHeld = std::move(held);
}

void Graph::insertAttrRaw(NodeId id, int index, const AttrState& s)
{
    Node* n = node(id);
    if (!n->mOwnType)
    {
        n->mOwnType = std::make_shared<NodeType>(*n->mType);
        n->mOwnType->userOf = &n->mType->registered();
        n->mType = n->mOwnType.get();
    }
    shiftAttrRefs(id, index, 1);
    n->mOwnType->attrs.insert(n->mOwnType->attrs.begin() + index, s.def);
    n->mValues.insert(n->mValues.begin() + index, s.value);
    if (!n->mLocks.empty() || s.lock)
    {
        n->mLocks.resize(std::max(n->mLocks.size(), size_t(index)));
        n->mLocks.insert(n->mLocks.begin() + index, s.lock);
    }
    if (!n->mElements.empty() || !s.elements.empty())
    {
        n->mElements.resize(std::max(n->mElements.size(), size_t(index)));
        n->mElements.insert(n->mElements.begin() + index, s.elements);
    }
    markDirty(id);
    changed(Change::Attrs, id);
}

Graph::AttrState Graph::removeAttrRaw(NodeId id, int index)
{
    Node* n = node(id);
    AttrState s{n->mOwnType->attrs[size_t(index)], n->mValues[size_t(index)], n->elements(index), n->lockMask(index)};
    n->mOwnType->attrs.erase(n->mOwnType->attrs.begin() + index);
    n->mValues.erase(n->mValues.begin() + index);
    if (size_t(index) < n->mLocks.size())
        n->mLocks.erase(n->mLocks.begin() + index);
    if (size_t(index) < n->mElements.size())
        n->mElements.erase(n->mElements.begin() + index);
    shiftAttrRefs(id, index + 1, -1);
    markDirty(id);
    changed(Change::Attrs, id);
    return s;
}

int Graph::addAttr(NodeId id, AttrDef def)
{
    Node* n = node(id);
    if (!n)
        throw std::invalid_argument("no such node");
    // group.child: one level of grouping.
    const size_t dot = def.name.find('.');
    if (!validName(def.name.substr(0, dot)) || (dot != std::string::npos && !validName(def.name.substr(dot + 1))))
        throw std::invalid_argument("invalid attribute name '" + def.name + "'");
    const NodeType& t = n->type();
    for (const std::string& want : {def.name, def.shortName})
        if (!want.empty() && t.attrIndex(want) >= 0)
            throw std::invalid_argument("'" + n->name() + "' already has an attribute '" + want + "'");
    const std::string group = dot == std::string::npos ? def.name : def.name.substr(0, dot);
    if (dot == std::string::npos ? !t.groupChildren(group).empty() : t.attrIndex(group) >= 0)
        throw std::invalid_argument("'" + n->name() + "' already has an attribute '" + group + "'");
    def.flags |= AttrUser | AttrInput | AttrStorable;
    if (def.type == PlugType::Vec3)
        for (int c = 0; c < 3; ++c)
            if (def.childNames[size_t(c)].empty())
                def.childNames[size_t(c)] = def.name + "XYZ"[c];
    const int index = int(t.attrs.size());
    AttrState s{def, def.defaultValue, {}, 0};
    insertAttrRaw(id, index, s);
    recordUndo([this, id, index] { removeAttrRaw(id, index); }, [this, id, index, s] { insertAttrRaw(id, index, s); });
    return index;
}

void Graph::deleteAttr(const PlugRef& p)
{
    Node* n = node(p.node);
    if (!n)
        throw std::invalid_argument("no such node");
    if (!(n->type().attrs[size_t(p.attr)].flags & AttrUser))
        throw std::invalid_argument(plugName({p.node, p.attr}) + " belongs to the node's type; only attributes added with addAttr can be deleted");
    std::vector<PlugRef> touching;
    for (const auto& [dst, src] : mInputs)
        if ((dst.node == p.node && dst.attr == p.attr) || (src.node == p.node && src.attr == p.attr))
            touching.push_back(dst);
    for (const PlugRef& dst : touching)
        disconnect(dst);
    const NodeId id = p.node;
    const int index = p.attr;
    AttrState s = removeAttrRaw(id, index);
    recordUndo([this, id, index, s] { insertAttrRaw(id, index, s); }, [this, id, index] { removeAttrRaw(id, index); });
}

namespace
{
bool holds(PlugType type, const PlugValue& v)
{
    switch (type)
    {
    case PlugType::Bool: return std::holds_alternative<bool>(v);
    case PlugType::Int:
    case PlugType::Enum: return std::holds_alternative<int64_t>(v);
    case PlugType::Float: return std::holds_alternative<double>(v);
    case PlugType::Vec3: return std::holds_alternative<glm::dvec3>(v);
    case PlugType::Matrix: return std::holds_alternative<glm::dmat4>(v);
    case PlugType::String: return std::holds_alternative<std::string>(v);
    case PlugType::Mesh: return std::holds_alternative<MeshPtr>(v);
    case PlugType::Data: return std::holds_alternative<DataPtr>(v);
    }
    return false;
}

/** v as def's type: through JSON where that reads, else by its first number, else def's default. */
PlugValue convertValue(const PlugValue& v, const AttrDef& def)
{
    if (holds(def.type, v))
        return v;
    json j = toJson(v);
    double number = 0.0;
    bool numeric = true;
    if (auto* s = std::get_if<std::string>(&v))
    {
        char* end = nullptr;
        number = std::strtod(s->c_str(), &end);
        numeric = end != s->c_str();
    }
    else if (j.is_array() && !j.empty() && j[0].is_number())
        number = j[0].get<double>();
    else if (j.is_number() || j.is_boolean())
        number = j.is_boolean() ? double(j.get<bool>()) : j.get<double>();
    else
        numeric = false;
    if (def.type == PlugType::String)
    {
        auto text = [](double d) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%g", d);
            return std::string(buf);
        };
        if (auto* v3 = std::get_if<glm::dvec3>(&v))
            return text(v3->x) + " " + text(v3->y) + " " + text(v3->z);
        if (auto* b = std::get_if<bool>(&v))
            return std::string(*b ? "true" : "false");
        if (numeric)
            return text(number);
    }
    std::vector<json> tries;
    if (numeric)
    {
        tries.push_back(def.type == PlugType::Int || def.type == PlugType::Enum ? json(int64_t(std::llround(number))) : json(number));
        if (def.type == PlugType::Vec3)
            tries.push_back(json::array({number, number, number}));
    }
    tries.push_back(j);
    for (const json& t : tries)
        try
        {
            PlugValue out = fromJson(def, t);
            if (auto* e = std::get_if<int64_t>(&out); e && def.type == PlugType::Enum && (*e < 0 || *e >= int64_t(def.enumNames.size())))
                continue;
            return out;
        }
        catch (const std::exception&)
        {
        }
    return def.defaultValue;
}
} // namespace

void Graph::replaceAttrRaw(NodeId id, int index, const AttrState& s)
{
    Node* n = node(id);
    n->mOwnType->attrs[size_t(index)] = s.def;
    n->mValues[size_t(index)] = s.value;
    if (!n->mElements.empty() || !s.elements.empty())
    {
        n->mElements.resize(std::max(n->mElements.size(), size_t(index) + 1));
        n->mElements[size_t(index)] = s.elements;
    }
    if (!n->mLocks.empty() || s.lock)
    {
        n->mLocks.resize(std::max(n->mLocks.size(), size_t(index) + 1));
        n->mLocks[size_t(index)] = s.lock;
    }
    markDirty(id);
    changed(Change::Attrs, id);
}

void Graph::editAttr(const PlugRef& p, AttrDef def)
{
    Node* n = node(p.node);
    if (!n)
        throw std::invalid_argument("no such node");
    const AttrDef old = n->type().attrs[size_t(p.attr)];
    if (!(old.flags & AttrUser))
        throw std::invalid_argument(plugName({p.node, p.attr}) + " belongs to the node's type; only attributes added with addAttr can be edited");
    const size_t dot = def.name.find('.');
    if (!validName(def.name.substr(0, dot)) || (dot != std::string::npos && !validName(def.name.substr(dot + 1))))
        throw std::invalid_argument("invalid attribute name '" + def.name + "'");
    const NodeType& t = n->type();
    for (const std::string& want : {def.name, def.shortName})
    {
        if (want.empty())
            continue;
        if (int at = t.attrIndex(want); at >= 0 && at != p.attr)
            throw std::invalid_argument("'" + n->name() + "' already has an attribute '" + want + "'");
        for (size_t i = 0; i < t.attrs.size(); ++i)
            if (int(i) != p.attr && t.attrs[i].type == PlugType::Vec3 &&
                (std::find(t.attrs[i].childNames.begin(), t.attrs[i].childNames.end(), want) != t.attrs[i].childNames.end() ||
                 std::find(t.attrs[i].childShort.begin(), t.attrs[i].childShort.end(), want) != t.attrs[i].childShort.end()))
                throw std::invalid_argument("'" + n->name() + "' already has an attribute '" + want + "'");
    }
    def.flags |= AttrUser | AttrInput | AttrStorable;
    if (def.type == PlugType::Vec3 && (def.childNames[0].empty() || (old.type == PlugType::Vec3 && def.childNames == old.childNames &&
                                                                     old.childNames[0] == old.name + "X")))
        for (int c = 0; c < 3; ++c)
            def.childNames[size_t(c)] = def.name + "XYZ"[c];
    if (def.type != PlugType::Vec3)
        def.childNames = {};
    if (!holds(def.type, def.defaultValue))
        def.defaultValue = convertValue(def.defaultValue, AttrDef{.type = def.type, .defaultValue = defaultFor(def.type), .enumNames = def.enumNames});
    const bool retyped = def.type != old.type || (def.flags & AttrArray) != (old.flags & AttrArray);
    if (retyped)
    {
        std::vector<PlugRef> touching;
        for (const auto& [dst, src] : mInputs)
            if ((dst.node == p.node && dst.attr == p.attr) || (src.node == p.node && src.attr == p.attr))
                touching.push_back(dst);
        for (const PlugRef& dst : touching)
            disconnect(dst);
    }
    const int index = p.attr;
    const NodeId id = p.node;
    AttrState before{old, n->mValues[size_t(index)], n->elements(index), n->lockMask(index)};
    AttrState after{def, convertValue(before.value, def), {}, retyped ? uint8_t(0) : before.lock};
    if (def.flags & AttrArray)
    {
        if (old.flags & AttrArray)
            for (const auto& [i, v] : before.elements)
                after.elements.emplace_back(i, convertValue(v, def));
        after.value = def.defaultValue;
    }
    else if (old.flags & AttrArray)
        after.value = before.elements.empty() ? def.defaultValue : convertValue(before.elements.front().second, def);
    replaceAttrRaw(id, index, after);
    recordUndo([this, id, index, before] { replaceAttrRaw(id, index, before); }, [this, id, index, after] { replaceAttrRaw(id, index, after); });
}

void Graph::moveAttrRaw(NodeId id, int from, int to)
{
    Node* n = node(id);
    auto rotate = [&](auto& v) {
        if (v.empty())
            return;
        v.resize(std::max(v.size(), n->mOwnType->attrs.size()));
        if (from < to)
            std::rotate(v.begin() + from, v.begin() + from + 1, v.begin() + to + 1);
        else
            std::rotate(v.begin() + to, v.begin() + from, v.begin() + from + 1);
    };
    rotate(n->mValues);
    rotate(n->mElements);
    rotate(n->mLocks);
    rotate(n->mOwnType->attrs);
    remapAttrRefs(id, [&](int a) {
        if (a == from)
            return to;
        if (from < to && a > from && a <= to)
            return a - 1;
        if (to < from && a >= to && a < from)
            return a + 1;
        return a;
    });
    markDirty(id);
    changed(Change::Attrs, id);
}

void Graph::moveAttr(NodeId id, int from, int to)
{
    Node* n = node(id);
    if (!n)
        throw std::invalid_argument("no such node");
    const auto& attrs = n->type().attrs;
    for (int i : {from, to})
        if (i < 0 || size_t(i) >= attrs.size() || !(attrs[size_t(i)].flags & AttrUser))
            throw std::invalid_argument("only attributes added with addAttr can be reordered, among themselves");
    if (from == to)
        return;
    moveAttrRaw(id, from, to);
    recordUndo([this, id, from, to] { moveAttrRaw(id, to, from); }, [this, id, from, to] { moveAttrRaw(id, from, to); });
}

std::vector<PlugRef> Graph::destinations(const PlugRef& src) const
{
    std::vector<PlugRef> out;
    if (auto it = mOutgoingDst.find(src.node); it != mOutgoingDst.end())
        for (const PlugRef& dst : it->second)
            if (mInputs.at(dst) == src)
                out.push_back(dst);
    return out;
}

void Graph::markDirty(NodeId id)
{
    std::vector<NodeId> stack{id};
    while (!stack.empty())
    {
        NodeId cur = stack.back();
        stack.pop_back();
        Node* n = node(cur);
        if (!n)
            continue;
        // Already dirty means everything downstream already is.
        if (n->mDirty && cur != id)
            continue;
        n->mDirty = true;
        if (auto it = mOutgoingDst.find(cur); it != mOutgoingDst.end())
            for (const PlugRef& dst : it->second)
            {
                // A held value lasts only until its source changes.
                if (cur == id)
                    mHeld.erase(dst);
                stack.push_back(dst.node);
            }
        for (NodeId c : children(cur))
            stack.push_back(c);
    }
    mAnyDirty = true;
    mVersion.fetch_add(1, std::memory_order_relaxed);
}

void Graph::setDisplayTurns(std::unordered_map<NodeId, glm::dmat4> turns)
{
    bool rootsChanged = turns.size() != mDisplayTurns.size();
    for (const auto& [id, m] : mDisplayTurns)
        if (auto it = turns.find(id); it == turns.end())
        {
            rootsChanged = true;
            markDirty(id);
        }
        else if (it->second != m)
            markDirty(id);
    for (const auto& [id, m] : turns)
        if (!mDisplayTurns.contains(id))
        {
            rootsChanged = true;
            markDirty(id);
        }
    mDisplayTurns = std::move(turns);
    if (!rootsChanged)
        return;
    mTurned.clear();
    for (const auto& [id, m] : mDisplayTurns)
    {
        mTurned.insert(id);
        for (NodeId d : descendants(id))
            mTurned.insert(d);
    }
}

void Graph::setTime(double seconds, bool notify)
{
    if (mCtx.time == seconds)
        return;
    mCtx.time = seconds;
    mHeld.clear();
    for (auto& [id, n] : mNodes)
        if (n->type().timeDependent)
            markDirty(id);
    mVersion.fetch_add(1, std::memory_order_relaxed);
    if (notify && this == &current())
        events::send({.type = events::Event::TimeChanged, .time = seconds});
}

namespace
{
Graph::EvaluateHook& evaluateHook()
{
    static Graph::EvaluateHook h;
    return h;
}
std::vector<Graph::EvaluateHook>& extraHooks()
{
    static std::vector<Graph::EvaluateHook> h;
    return h;
}
bool gInEvaluateHook = false;

struct HookGuard
{
    HookGuard() { gInEvaluateHook = true; }
    ~HookGuard() { gInEvaluateHook = false; }
};
} // namespace

void Graph::setEvaluateHook(EvaluateHook hook) { evaluateHook() = std::move(hook); }

void Graph::addEvaluateHook(EvaluateHook hook) { extraHooks().push_back(std::move(hook)); }

namespace
{
thread_local int gDeferMeshes = 0;
} // namespace

Graph::DeferMeshes::DeferMeshes() { ++gDeferMeshes; }
Graph::DeferMeshes::~DeferMeshes() { --gDeferMeshes; }

void Graph::evaluate()
{
    if (gDeferMeshes > 0 && mAnyDirty)
    {
        std::vector<NodeId> roots;
        for (auto& [id, n] : mNodes)
            if (n->mDirty && n->type().dag && !n->type().isA("mesh"))
                roots.push_back(id);
        evaluateWith(&roots);
        return;
    }
    evaluateWith(nullptr);
}

void Graph::evaluateWith(const std::vector<NodeId>* only)
{
    const EvaluateHook& hook = evaluateHook();
    if ((!hook && extraHooks().empty()) || gInEvaluateHook)
    {
        evaluateDirty(only);
        return;
    }
    auto call = [&](bool after) {
        if (hook)
            hook(*this, after);
        for (const EvaluateHook& h : extraHooks())
            h(*this, after);
    };
    if (mAnyDirty)
    {
        HookGuard guard;
        call(false);
    }
    evaluateDirty(only);
    HookGuard guard;
    call(true);
}

void Graph::evaluateUpstream(const std::vector<NodeId>& nodes) { evaluateDirty(&nodes); }

void Graph::evaluateDirty(const std::vector<NodeId>* only)
{
    LILA_ZONE();

    if (!mAnyDirty)
        return;
    // Just the dirty nodes `only` depends on. Upstream of a clean node is clean, so the walk stops at clean nodes.
    std::unordered_set<NodeId> needed;
    if (only)
    {
        std::vector<NodeId> stack(only->begin(), only->end());
        while (!stack.empty())
        {
            const NodeId id = stack.back();
            stack.pop_back();
            Node* n = node(id);
            if (!n || !n->mDirty || !needed.insert(id).second)
                continue;
            if (NodeId p = parent(id); p != kNoNode)
                stack.push_back(p);
            for (auto it = mInputs.lower_bound({id, 0, -1}); it != mInputs.end() && it->first.node == id; ++it)
                stack.push_back(it->second.node);
        }
        if (needed.empty())
            return;
    }
    mAnyDirty = false;
    const uint64_t pass = ++mEvaluationPass;
    // Partial and simulation-sampling evaluations compute only what a reader needs, not the scene at this time.
    const bool cache = mPlayback && !only && !sim::sampling();

    // Kahn levels over the dirty subgraph; each level computes in parallel.
    std::vector<Node*> dirty;
    for (auto& [id, n] : mNodes)
        if (n->mDirty)
        {
            if (only && !needed.contains(id))
                mAnyDirty = true;
            else
                dirty.push_back(n.get());
        }

    std::unordered_map<NodeId, int> pending;
    std::unordered_map<NodeId, std::vector<NodeId>> downstream;
    for (Node* n : dirty)
        pending[n->id()] = 0;
    auto addEdge = [&](NodeId up, NodeId down) {
        if (up == down || !pending.contains(up))
            return;
        downstream[up].push_back(down);
        ++pending[down];
    };
    for (Node* n : dirty)
    {
        if (NodeId p = parent(n->id()); p != kNoNode)
            addEdge(p, n->id());
    }
    for (auto& [dst, src] : mInputs)
        if (pending.contains(dst.node))
            addEdge(src.node, dst.node);

    std::vector<Node*> level;
    for (Node* n : dirty)
        if (pending[n->id()] == 0)
            level.push_back(n);

    while (!level.empty())
    {
        auto pull = [&](Node* n) {
            const bool cacheNode = cache && !mTurned.contains(n->id());
            const uint8_t upstream = cacheNode ? upstreamPlayback(*n, pass) : 0;
            if (cacheNode && upstream == 0 && restoreCached(*n))
            {
                n->mPlayback = 0;
                n->mDirty = false;
                n->mEvaluatedPass = pass;
                return;
            }
            if (n->type().dag)
            {
                glm::dmat4 pw(1.0);
                if (NodeId p = parent(n->id()); p != kNoNode)
                    pw = node(p)->as<glm::dmat4>(Transform::kWorldMatrix);
                n->mValues[Transform::kParentMatrix] = pw;
            }
            for (auto it = mInputs.lower_bound({n->id(), 0, -1}); it != mInputs.end() && it->first.node == n->id(); ++it)
            {
                const PlugRef& sp = it->second;
                const PlugRef& dp = it->first;
                if (!mHeld.empty() && mHeld.contains(dp))
                    continue;
                const PlugValue& sv = node(sp.node)->mValues[sp.attr];
                PlugValue* slot = dp.element >= 0 ? elementSlot(n->mElements, dp) : &n->mValues[dp.attr];
                if (!slot)
                    continue;
                PlugValue& dv = *slot;
                const AttrDef& da = n->type().attrs[dp.attr];
                if (sp.component >= 0 || dp.component >= 0)
                {
                    double s = 0.0;
                    if (sp.component >= 0)
                        s = std::get<glm::dvec3>(sv)[sp.component];
                    else
                        scalarOf(sv, s);
                    if (dp.component >= 0)
                        std::get<glm::dvec3>(dv)[dp.component] = s;
                    else
                        assignScalar(dv, s);
                    clampToLimits(da, dv);
                    continue;
                }
                // Numeric types convert across connections.
                std::visit(
                    [&](auto& d) {
                        using D = std::decay_t<decltype(d)>;
                        std::visit(
                            [&](const auto& s) {
                                using S = std::decay_t<decltype(s)>;
                                if constexpr (std::is_same_v<D, S>)
                                    d = s;
                                else if constexpr (std::is_arithmetic_v<D> && std::is_arithmetic_v<S>)
                                    d = D(s);
                            },
                            sv);
                    },
                    dv);
                clampToLimits(da, dv);
            }
            if (n->type().dag)
            {
                if (const glm::dmat4* frame = spaces::activeFrame(*n))
                    n->mValues[Transform::kParentMatrix] = *frame;
                else if (const auto* opm = n->mValues.size() > Transform::kOffsetParentMatrix ? std::get_if<glm::dmat4>(&n->mValues[Transform::kOffsetParentMatrix]) : nullptr;
                         opm && *opm != glm::dmat4(1.0))
                    n->mValues[Transform::kParentMatrix] = std::get<glm::dmat4>(n->mValues[Transform::kParentMatrix]) * *opm;
            }
            if (!mDisplayTurns.empty() && n->type().dag)
                if (auto turn = mDisplayTurns.find(n->id()); turn != mDisplayTurns.end())
                    n->mValues[Transform::kParentMatrix] = turn->second * n->as<glm::dmat4>(Transform::kParentMatrix);
            n->compute(mCtx);
            // Computed from a placeholder upstream means a placeholder too.
            if (cacheNode)
                n->mPlayback = upstream < 2 && keepCached(*n) ? 1 : 2;
            n->mDirty = false;
            n->mEvaluatedPass = pass;
        };

        if (level.size() < 16)
            for (Node* n : level)
                pull(n);
        else
            tbb::parallel_for(size_t(0), level.size(), [&](size_t i) { pull(level[i]); });

        std::vector<Node*> next;
        for (Node* n : level)
            for (NodeId d : downstream[n->id()])
                if (--pending[d] == 0)
                    next.push_back(node(d));
        level.swap(next);
    }
}

void Graph::selectRaw(std::vector<NodeId> ids)
{
    mSelection = std::move(ids);
    ++mSelectionSerial;
    mVersion.fetch_add(1, std::memory_order_relaxed);
    changed(Change::Selection, kNoNode);
}

void Graph::select(const std::vector<NodeId>& ids, SelectMode mode)
{
    std::vector<NodeId> next = mode == SelectMode::Replace ? std::vector<NodeId>() : mSelection;
    for (NodeId id : ids)
    {
        if (!node(id))
            continue;
        auto it = std::find(next.begin(), next.end(), id);
        bool present = it != next.end();
        switch (mode)
        {
        case SelectMode::Replace:
        case SelectMode::Add:
            if (!present)
                next.push_back(id);
            break;
        case SelectMode::Remove:
            if (present)
                next.erase(it);
            break;
        case SelectMode::Toggle:
            if (present)
                next.erase(it);
            else
                next.push_back(id);
            break;
        }
    }
    if (next == mSelection)
        return;
    std::vector<NodeId> old = mSelection;
    selectRaw(next);
    recordUndo([this, old] { selectRaw(old); }, [this, next] { selectRaw(next); }, true);
}

bool Graph::isSelected(NodeId id) const
{
    return std::find(mSelection.begin(), mSelection.end(), id) != mSelection.end();
}

int Graph::addListener(Listener fn, bool immediate)
{
    mListeners.emplace(mNextListener, ListenerEntry{std::move(fn), immediate});
    return mNextListener++;
}

void Graph::removeListener(int id)
{
    mListeners.erase(id);
}

bool Graph::changesVisibility(Change c, NodeId id, const PlugRef* plug) const
{
    if (c == Change::Selection || c == Change::Renamed)
        return false;
    if (c != Change::Value && c != Change::Connection)
        return true;
    const Node* n = node(id);
    if (!plug || !n || !n->type().dag)
        return true;
    return plug->attr == Transform::kVisibility || plug->attr == Transform::kDisplayLayer;
}

void Graph::changed(Change c, NodeId id, const PlugRef* plug)
{
    mVersion.fetch_add(1, std::memory_order_relaxed);
    if (changesVisibility(c, plug ? plug->node : id, plug))
        ++mVisibilityVersion;
    if (c != Change::Selection && !sim::sampling())
        clearPlaybackCache();
    const PlugRef* outer = std::exchange(mChangedPlug, plug);
    for (auto& [lid, l] : mListeners)
        if (mBatchDepth == 0 || l.immediate)
            l.fn(c, id);
        else
            mBatchPending = true;
    mChangedPlug = outer;
    if (mBatchDepth == 0 && this == &current())
        forwardEvent(c, id);
}

void Graph::forwardEvent(Change c, NodeId id)
{
    events::Event e;
    switch (c)
    {
    case Change::Created: e = events::Event::NodeCreated; break;
    case Change::Deleted: e = events::Event::NodeDeleted; break;
    case Change::Renamed: e = events::Event::NodeRenamed; break;
    case Change::Selection: e = events::Event::SelectionChanged; break;
    default: return;
    }
    if (!events::hasSubscribers(e))
        return;
    const Node* n = c == Change::Renamed ? node(id) : nullptr;
    events::send({.type = e, .name = n ? n->name() : std::string(), .node = id});
}

Graph::Batch::Batch(Graph& g) : mGraph(g)
{
    ++mGraph.mBatchDepth;
}

Graph::Batch::~Batch()
{
    if (--mGraph.mBatchDepth > 0 || !mGraph.mBatchPending)
        return;
    mGraph.mBatchPending = false;
    for (auto& [lid, l] : mGraph.mListeners)
        if (!l.immediate)
            l.fn(Change::Reset, kNoNode);
}
} // namespace lila::scene
