#include "slots/Slots.h"

#include "core/Log.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "gn/Geometry.h"
#include "gn/Gn.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Node.h"
#include "scene/Transform.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

namespace lila::slots
{
using scene::Graph;
using scene::Node;
using scene::PlugRef;
using scene::Transform;

namespace detail
{
std::vector<NodeId> publishedNodes();
std::vector<Slot> publishedBy(NodeId n);
void evaluatePublishers(Graph& g);
std::vector<Slot> withPins(std::vector<Slot> slots, const State& st, std::vector<std::string>* conflicts);
std::vector<const PinnedSlot*> allPinned(const State& st);
Resolved resolveImpl(const Slot& s, const State& st, bool mainThread);
} // namespace detail

namespace
{
glm::dmat4 worldOf(Graph& g, NodeId id)
{
    const Node* n = g.node(id);
    if (!n || !n->type().isA("transform"))
        return glm::dmat4(1.0);
    return std::get<glm::dmat4>(g.value(PlugRef{id, Transform::kWorldMatrix}));
}

int slotsAttr(const Node& n) { return n.type().attrIndex("slots"); }

/** Parsed slotSet contents, local space, kept while the stored text is unchanged. */
struct SetCache
{
    size_t hash = 0;
    std::vector<Slot> local;
};
std::unordered_map<NodeId, SetCache>& setCache()
{
    static std::unordered_map<NodeId, SetCache> c;
    return c;
}

const std::vector<Slot>& localSlots(const Node& n)
{
    const int a = slotsAttr(n);
    static const std::vector<Slot> none;
    if (a < 0)
        return none;
    const std::string& text = n.as<std::string>(a);
    const size_t h = std::hash<std::string>{}(text);
    SetCache& c = setCache()[n.id()];
    if (c.hash == h && !(text.empty() && !c.local.empty()))
        return c.local;
    c.hash = h;
    c.local.clear();
    if (!text.empty())
    {
        const json j = json::parse(text, nullptr, false);
        if (j.is_array())
            for (const json& s : j)
                c.local.push_back(Slot::fromJson(s));
    }
    return c.local;
}

/** The node a publisher's slots are local to: a modifier's mesh, else the node itself. */
NodeId frameNodeOf(Graph& g, NodeId publisher)
{
    const Node* n = g.node(publisher);
    if (!n)
        return scene::kNoNode;
    if (scene::modifiers::isModifier(*n))
        return scene::modifiers::meshOf(g, publisher);
    return publisher;
}

std::mutex& visualMutex()
{
    static std::mutex m;
    return m;
}
struct CachedVisual
{
    std::shared_ptr<const geo::Mesh> mesh;
    std::string missing;
};
std::map<std::string, CachedVisual>& visualCache()
{
    static std::map<std::string, CachedVisual> c;
    return c;
}

std::string footprintText(const glm::dvec3& f)
{
    return std::to_string(int(std::lround(f.x))) + "x" + std::to_string(int(std::lround(f.y))) + "x" + std::to_string(int(std::lround(f.z)));
}

glm::dvec3 footprintOf(const std::string& type)
{
    const SlotType* t = resolveType(type);
    return t ? t->footprint : glm::dvec3(100.0);
}

std::string placeholderOf(const Resolved& r, const std::string& type)
{
    if (!r.payload.placeholder.empty())
        return r.payload.placeholder;
    const SlotType* t = resolveType(type);
    return t ? t->placeholder : std::string("box");
}

std::string placeholderKey(const Resolved& r, const std::string& type)
{
    return "placeholder:" + placeholderOf(r, type) + "/" + footprintText(footprintOf(type));
}
} // namespace

namespace detail
{
void touchPublishers(const std::set<NodeId>* only)
{
    Graph& g = Graph::current();
    // A generator node that publishes its own slots (site kits, buildings) keeps its step result while its inputs stay the
    // same, so a rule or override would not reach its instances until the modifier runs again from scratch.
    for (NodeId n : publishedNodes())
        if ((!only || only->contains(n)) && g.node(n) && !gn::rerunModifier(g, n))
            g.touch(n);
    for (NodeId id : g.nodes())
        if (const Node* n = g.node(id); n && n->type().isA(kSetType))
            g.touch(id);
}

void forgetSet(NodeId id) { setCache().erase(id); }

void evaluatePublishers(Graph& g)
{
    // Reading a frame's world evaluates it; all of them in one pass runs dirty publishers in parallel, not one by one.
    std::vector<NodeId> frames;
    for (NodeId pub : publishedNodes())
        if (const NodeId frame = frameNodeOf(g, pub); frame != scene::kNoNode)
            frames.push_back(frame);
    g.evaluateUpstream(frames);
}
} // namespace detail

std::vector<Slot> readSet(Graph& g, NodeId set)
{
    const Node* n = g.node(set);
    if (!n || !n->type().isA(kSetType))
        return {};
    const glm::dmat4 world = worldOf(g, set);
    std::vector<Slot> out = localSlots(*n);
    for (Slot& s : out)
        s.transform = world * s.transform;
    return out;
}

std::vector<Slot> collect(Graph& g) { return collect(g, nullptr); }

std::vector<Slot> collect(Graph& g, const std::function<bool(const Slot&)>& keep)
{
    StatePtr st = state();
    std::vector<Slot> out;
    auto filtered = [&](std::vector<Slot> slots) {
        if (keep)
            std::erase_if(slots, [&](const Slot& s) { return !keep(s); });
        return slots;
    };
    for (NodeId id : g.nodes())
        if (const Node* n = g.node(id); n && n->type().isA(kSetType))
            for (Slot& s : detail::withPins(filtered(readSet(g, id)), *st, nullptr))
                out.push_back(std::move(s));
    detail::evaluatePublishers(g);
    for (NodeId pub : detail::publishedNodes())
    {
        const NodeId frame = frameNodeOf(g, pub);
        if (frame == scene::kNoNode)
            continue;
        std::vector<Slot> slots = filtered(detail::publishedBy(pub));
        if (slots.empty())
            continue;
        const glm::dmat4 world = worldOf(g, frame);
        for (Slot& s : slots)
            s.transform = world * s.transform;
        for (Slot& s : detail::withPins(std::move(slots), *st, nullptr))
            out.push_back(std::move(s));
    }
    if (!st->pins.empty())
    {
        std::set<std::string> seen;
        for (const Slot& s : out)
            seen.insert(s.id);
        for (const PinnedSlot* p : detail::allPinned(*st))
            if ((!keep || keep(p->slot)) && seen.insert(p->slot.id).second)
                out.push_back(p->slot);
    }
    return out;
}

std::vector<Slot> slotsOf(Graph& g, NodeId node)
{
    StatePtr st = state();
    std::vector<Slot> slots;
    std::string ownerScope;
    if (const Node* n = g.node(node); n && n->type().isA(kSetType))
        slots = readSet(g, node);
    else
    {
        slots = publishedSlots(node);
        const glm::dmat4 world = worldOf(g, frameNodeOf(g, node));
        for (Slot& s : slots)
            s.transform = world * s.transform;
    }
    std::set<std::string> owners, seen;
    for (const Slot& s : slots)
        owners.insert(s.owner);
    std::vector<Slot> out = detail::withPins(std::move(slots), *st, nullptr);
    for (const Slot& s : out)
        seen.insert(s.id);
    for (const PinnedSlot* p : detail::allPinned(*st))
        if (owners.contains(p->slot.owner) && seen.insert(p->slot.id).second)
            out.push_back(p->slot);
    return out;
}

std::optional<Slot> findSlot(Graph& g, std::string_view id)
{
    for (Slot& s : collect(g))
        if (s.id == id)
            return std::move(s);
    return std::nullopt;
}

NodeId publisherOf(Graph& g, std::string_view id)
{
    for (NodeId nid : g.nodes())
        if (const Node* n = g.node(nid); n && n->type().isA(kSetType))
            for (const Slot& s : localSlots(*n))
                if (s.id == id)
                    return nid;
    for (NodeId pub : detail::publishedNodes())
        for (const Slot& s : detail::publishedBy(pub))
            if (s.id == id)
                return frameNodeOf(g, pub);
    return scene::kNoNode;
}

json writeSet(Graph& g, const std::string& name, NodeId parent, const std::vector<Slot>& slots, const std::string& publisherDoc)
{
    const scene::NodeType* type = scene::types::find(kSetType);
    if (!type)
        throw std::runtime_error("slots: the slotSet node type is not registered");
    NodeId set = scene::kNoNode;
    if (parent != scene::kNoNode)
    {
        for (NodeId c : g.children(parent))
            if (const Node* n = g.node(c); n && n->name() == name && n->type().isA(kSetType))
                set = c;
    }
    else if (const Node* n = g.find(name); n && n->type().isA(kSetType))
        set = n->id();
    if (set == scene::kNoNode)
        set = g.createNode(*type, name, parent)->id();
    const glm::dmat4 toLocal = glm::inverse(worldOf(g, set));

    StatePtr st = state();
    std::vector<std::string> conflicts;
    detail::withPins(slots, *st, &conflicts);
    // Stored as generated: pins apply on reading, so unpinning shows what the generator makes now.
    json arr = json::array();
    size_t pinned = 0;
    for (Slot s : slots)
    {
        if (isPinned(s.id))
            ++pinned;
        s.transform = toLocal * s.transform;
        arr.push_back(s.toJson());
    }
    const Node* n = g.node(set);
    g.setValue(PlugRef{set, slotsAttr(*n)}, arr.dump());
    if (!publisherDoc.empty())
        g.setValue(PlugRef{set, n->type().attrIndex("source")}, publisherDoc);
    return {{"set", g.path(set)}, {"written", arr.size()}, {"pinned", pinned}, {"conflicts", conflicts}};
}

// Visuals

Registry<MeshProvider>& meshProviders()
{
    static Registry<MeshProvider> r;
    return r;
}

std::string visualKey(const Resolved& r, const std::string& type)
{
    if (r.payload.hidden && *r.payload.hidden)
        return {};
    if (!r.payload.item.empty())
        return "item:" + r.payload.item;
    if (!r.payload.mesh.empty())
        return "mesh:" + r.payload.mesh;
    return placeholderKey(r, type);
}

std::shared_ptr<const geo::Mesh> cachedVisual(const std::string& key)
{
    std::lock_guard lock(visualMutex());
    auto it = visualCache().find(key);
    return it == visualCache().end() ? nullptr : it->second.mesh;
}

void clearVisuals()
{
    std::lock_guard lock(visualMutex());
    visualCache().clear();
}

std::shared_ptr<const geo::Mesh> placeholderMesh(const std::string& shape, const glm::dvec3& f)
{
    const glm::dvec3 s = glm::max(f, glm::dvec3(1.0));
    geo::Mesh m;
    auto box = [](double w, double h, double d, const glm::dvec3& at) {
        geo::Mesh b = geo::makeCube({.width = w, .height = h, .depth = d, .baseAtOrigin = true});
        gn::transformMesh(b, glm::translate(glm::dmat4(1.0), at));
        return b;
    };
    if (shape == "panel")
        m = box(s.x, s.y, std::min(s.z, 8.0), {0, 0, 0});
    else if (shape == "door")
    {
        m = box(s.x, s.y, std::min(s.z, 10.0), {0, 0, 0});
        m.append(box(s.x * 0.08, s.x * 0.08, 6.0, {s.x * 0.35, s.y * 0.45, std::min(s.z, 10.0) * 0.5 + 3.0}));
    }
    else if (shape == "window")
    {
        const double t = std::min(s.x, s.y) * 0.08;
        const double d = std::min(s.z, 12.0);
        m = box(s.x, t, d, {0, 0, 0});
        m.append(box(s.x, t, d, {0, s.y - t, 0}));
        m.append(box(t, s.y, d, {-(s.x - t) * 0.5, 0, 0}));
        m.append(box(t, s.y, d, {(s.x - t) * 0.5, 0, 0}));
        m.append(box(s.x - 2 * t, s.y - 2 * t, 1.0, {0, t, -d * 0.25}));
    }
    else if (shape == "sign")
    {
        m = box(s.x, s.y, std::min(s.z, 6.0), {0, 0, 0});
        m.append(box(s.x * 0.9, s.y * 0.8, 1.0, {0, s.y * 0.1, std::min(s.z, 6.0) * 0.5 + 0.5}));
    }
    else if (shape == "awning")
    {
        geo::Mesh slab = box(s.x, std::min(s.y, 6.0), s.z, {0, 0, 0});
        gn::transformMesh(slab, glm::rotate(glm::dmat4(1.0), glm::radians(-20.0), glm::dvec3(1, 0, 0)));
        m = std::move(slab);
    }
    else if (shape == "pole" || shape == "lamp")
    {
        const double r = std::max(5.0, std::min(s.x, s.z) * 0.12);
        m = geo::makeCylinder({.radius = r, .height = s.y, .subdivAxis = 8, .baseAtOrigin = true});
        if (shape == "lamp")
        {
            m.append(box(s.x * 0.5, r * 2.0, r * 2.0, {s.x * 0.25, s.y - r * 2.0, 0}));
            m.append(box(r * 5.0, r * 2.0, r * 4.0, {s.x * 0.5 - r * 2.5, s.y - r * 4.0, 0}));
        }
    }
    else if (shape == "tree" || shape == "palm")
    {
        const double trunk = shape == "palm" ? s.y * 0.85 : s.y * 0.4;
        const double r = std::max(6.0, std::min(s.x, s.z) * 0.05);
        m = geo::makeCylinder({.radius = r, .height = trunk, .subdivAxis = 8, .baseAtOrigin = true});
        geo::Mesh crown = geo::makeSphere({.radius = 0.5, .subdivAxis = 10, .subdivHeight = 6, .baseAtOrigin = true});
        // A palm's crown is a wide flat spray at the top of a tall trunk; a tree's a full canopy.
        const glm::dvec3 size = shape == "palm" ? glm::dvec3(s.x, s.y * 0.15, s.z) : glm::dvec3(s.x, s.y - trunk * 0.9, s.z);
        const double base = shape == "palm" ? trunk - size.y * 0.3 : trunk * 0.9;
        gn::transformMesh(crown, glm::translate(glm::dmat4(1.0), glm::dvec3(0, base, 0)) * glm::scale(glm::dmat4(1.0), size));
        m.append(crown);
    }
    else if (shape == "bush" || shape == "plant" || shape == "cylinder" || shape == "cone")
    {
        if (shape == "cylinder")
            m = geo::makeCylinder({.radius = 0.5, .height = 1.0, .subdivAxis = 12, .baseAtOrigin = true});
        else if (shape == "cone")
            m = geo::makeCone({.radius = 0.5, .height = 1.0, .subdivAxis = 10, .baseAtOrigin = true});
        else
            m = geo::makeSphere({.radius = 0.5, .subdivAxis = 8, .subdivHeight = 5, .baseAtOrigin = true});
        gn::transformMesh(m, glm::scale(glm::dmat4(1.0), s));
    }
    else if (shape == "row")
    {
        const int n = std::max(1, int(s.z / 60.0));
        m = box(s.x * 0.6, s.y * 0.15, s.z, {0, 0, 0});
        for (int i = 0; i < n; ++i)
        {
            geo::Mesh p = geo::makeSphere({.radius = 0.5, .subdivAxis = 6, .subdivHeight = 4, .baseAtOrigin = true});
            gn::transformMesh(p, glm::translate(glm::dmat4(1.0), glm::dvec3(0, s.y * 0.1, -s.z * 0.5 + (i + 0.5) * s.z / n)) *
                                     glm::scale(glm::dmat4(1.0), glm::dvec3(s.x * 0.5, s.y * 0.9, std::min(s.z / n, s.x * 0.5))));
            m.append(p);
        }
    }
    else
        m = box(s.x, s.y, s.z, {0, 0, 0});
    return std::make_shared<const geo::Mesh>(std::move(m));
}

const std::vector<std::string>& placeholderShapes()
{
    static const std::vector<std::string> s = {"box", "panel", "door", "window", "sign", "awning", "pole", "lamp",
                                               "tree", "palm", "bush", "plant", "cylinder", "cone", "row"};
    return s;
}

Visual visualFor(Graph& g, const Resolved& r, const std::string& type, bool load)
{
    Visual v;
    v.key = visualKey(r, type);
    if (v.key.empty())
        return v;
    {
        std::lock_guard lock(visualMutex());
        if (auto it = visualCache().find(v.key); it != visualCache().end() && (it->second.missing.empty() || !load))
        {
            v.mesh = it->second.mesh;
            v.missing = it->second.missing;
            return v;
        }
    }
    CachedVisual made;
    const bool provided = !r.payload.item.empty() || !r.payload.mesh.empty();
    if (provided)
    {
        const std::string scheme = !r.payload.item.empty() ? "item" : "mesh";
        const std::string ref = !r.payload.item.empty() ? r.payload.item : r.payload.mesh;
        if (MeshProvider* p = meshProviders().find(scheme))
        {
            std::string why;
            try
            {
                made.mesh = (*p)(g, ref, load, why);
            }
            catch (const std::exception& e)
            {
                why = e.what();
            }
            if (!made.mesh)
                made.missing = why.empty() ? scheme + " " + ref + " gave no mesh" : why;
        }
        else
            made.missing = "nothing provides " + scheme + " payloads (load the Set Dressing extension for library items)";
    }
    if (!made.mesh)
        made.mesh = placeholderMesh(placeholderOf(r, type), footprintOf(type));
    {
        std::lock_guard lock(visualMutex());
        visualCache()[v.key] = made;
    }
    if (!made.missing.empty())
        log::warn("Slots: {}: {}; drawn as its placeholder", v.key, made.missing);
    v.mesh = made.mesh;
    v.missing = made.missing;
    return v;
}

std::vector<DrawBatch> drawBatches(Graph& g, NodeId set)
{
    std::vector<DrawBatch> out;
    std::map<std::string, size_t> at;
    StatePtr st = state();
    for (const Slot& s : slotsOf(g, set))
    {
        const Resolved r = detail::resolveImpl(s, *st, true);
        const Visual v = visualFor(g, r, s.type, false);
        if (!v.mesh)
            continue;
        auto [it, added] = at.try_emplace(v.key, out.size());
        if (added)
            out.push_back({v.mesh, v.key, {}, {}});
        DrawBatch& b = out[it->second];
        const std::optional<glm::vec4> color = dataColor(r.payload.data);
        if (color && b.colors.empty())
            b.colors.assign(b.transforms.size(), glm::vec4(0.0f));
        b.transforms.push_back(glm::mat4(s.transform));
        if (!b.colors.empty())
            b.colors.push_back(color.value_or(glm::vec4(0.0f)));
    }
    return out;
}

/** A node's meshes (its own and its descendants'), in its space, as one mesh. Null when it has none. */
std::shared_ptr<const geo::Mesh> combinedMesh(Graph& g, NodeId root)
{
    const glm::dmat4 toLocal = glm::inverse(worldOf(g, root));
    geo::Mesh all;
    bool any = false;
    std::vector<NodeId> nodes{root};
    for (NodeId d : g.descendants(root))
        nodes.push_back(d);
    for (NodeId id : nodes)
    {
        const Node* n = g.node(id);
        if (!n || !n->type().isA("mesh"))
            continue;
        const scene::MeshPtr m = std::get<scene::MeshPtr>(g.value(PlugRef{id, scene::MeshNode::kOutMesh}));
        if (!m)
            continue;
        if (nodes.size() == 1)
            return m;
        geo::Mesh copy = *m;
        gn::transformMesh(copy, toLocal * worldOf(g, id));
        all.append(copy);
        any = true;
    }
    return any ? std::make_shared<const geo::Mesh>(std::move(all)) : nullptr;
}
} // namespace lila::slots
