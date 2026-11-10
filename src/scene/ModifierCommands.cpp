#include "scene/Modifiers.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "geo/Ops.h"
#include "scene/DeformEval.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/Instancing.h"
#include "scene/MeshNodes.h"

#include <algorithm>

namespace lila::scene::modifiers
{
namespace
{
constexpr std::string_view kSuffix = "Modifier";

Graph& graph() { return Graph::current(); }

ModifierNode& modifierNamed(const std::string& name, const char* cmd)
{
    auto* n = asModifier(graph().find(name));
    if (!n)
        throw CommandError(std::string(cmd) + ": " + name + " is not a modifier");
    return *n;
}

Node& meshNamed(const std::string& name, const char* cmd)
{
    Node* n = graph().find(name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return *n;
}

int outAttrOf(const Node& n)
{
    if (auto* m = asModifier(&n))
        return m->outAttr;
    if (auto* gen = dynamic_cast<const GeneratorNode*>(&n))
        return gen->outAttr;
    return n.type().attrIndex("outMesh");
}

/** Deletes a node nothing reads any more, unless it lives in the hierarchy. */
void dropIfOrphan(Graph& g, NodeId id)
{
    Node* n = g.node(id);
    if (!n || n->type().dag)
        return;
    int out = outAttrOf(*n);
    if (out < 0 || g.destinations({id, out, -1}).empty())
        g.deleteNode(id);
}

/**
 * A shape fed through a "create" data plug (a curve with history): stores what the history makes and drops the nodes
 * that fed only it.
 */
void bakeCreateHistory(Graph& g, Node& n)
{
    int create = n.type().attrIndex("create");
    if (create < 0 || n.type().attrs[size_t(create)].type != PlugType::Data)
        return;
    PlugRef in{n.id(), create, -1};
    std::optional<PlugRef> src = g.source(in);
    if (!src)
        return;
    g.evaluate();
    PlugValue baked = g.value(in);
    g.disconnect(in);
    g.setValue(in, baked);
    // Upstream history nodes nothing else reads go too.
    std::vector<NodeId> stackIds{src->node};
    while (!stackIds.empty())
    {
        NodeId id = stackIds.back();
        stackIds.pop_back();
        Node* up = g.node(id);
        if (!up || up->type().dag)
            continue;
        bool used = false;
        for (size_t a = 0; a < up->type().attrs.size(); ++a)
            if (!g.destinations({id, int(a), -1}).empty())
                used = true;
        if (used)
            continue;
        for (const auto& [dst, from] : g.connections())
            if (dst.node == id)
                stackIds.push_back(from.node);
        g.deleteNode(id);
    }
}

json describe(Graph& g, NodeId id)
{
    Node* n = g.node(id);
    json j = {{"name", n->path()}, {"type", n->type().name}};
    if (auto* m = asModifier(n))
    {
        j["enabled"] = n->as<bool>(m->enabledAttr);
        j["exportEnabled"] = exportEnabled(*n);
        j["error"] = m->error();
    }
    return j;
}
} // namespace

std::vector<std::string> kinds()
{
    std::vector<std::string> out;
    for (const NodeType* t : types::all())
    {
        const std::string& name = t->name;
        if (!t->create || name.size() <= kSuffix.size() || !name.ends_with(kSuffix))
            continue;
        // Geometry-node modifiers need their group wired in, so they are added from the node editor instead.
        if (t->attrIndex("groupDefinition") >= 0)
            continue;
        if (!asModifier(t->create().get()))
            continue;
        out.push_back(name.substr(0, name.size() - kSuffix.size()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string kindDoc(std::string_view kind)
{
    const NodeType* t = types::find(std::string(kind) + std::string(kSuffix));
    return t ? t->doc : std::string();
}

NodeId stackSource(Graph& g, NodeId mesh)
{
    std::vector<NodeId> mods = stack(g, mesh);
    PlugRef bottom{mesh, MeshNode::kInMesh, -1};
    if (!mods.empty())
        bottom = {mods.front(), static_cast<ModifierNode*>(g.node(mods.front()))->inAttr, -1};
    auto src = g.source(bottom);
    return src ? src->node : kNoNode;
}

NodeId meshOf(Graph& g, NodeId modifier)
{
    NodeId cur = modifier;
    // A chain can't loop, but a malformed graph shouldn't hang the UI either.
    for (int guard = 0; guard < 4096; ++guard)
    {
        Node* n = g.node(cur);
        if (!n)
            return kNoNode;
        if (n->type().isA("mesh"))
            return cur;
        int out = outAttrOf(*n);
        if (out < 0)
            return kNoNode;
        auto dsts = g.destinations({cur, out, -1});
        if (dsts.empty())
            return kNoNode;
        cur = dsts.front().node;
    }
    return kNoNode;
}

bool exportEnabled(const Node& modifier)
{
    int a = modifier.type().attrIndex("exportEnabled");
    return a < 0 || modifier.as<bool>(a);
}

MeshPtr exportMesh(Graph& g, NodeId mesh)
{
    LILA_ZONE();

    g.evaluate();
    Node* m = g.node(mesh);
    if (!m)
        return nullptr;
    if (NodeId root = instancing::root(g, mesh); root != mesh)
        return exportMesh(g, root);
    std::vector<NodeId> mods = stack(g, mesh);
    auto ranInViewport = [](const ModifierNode& mod) { return mod.as<bool>(mod.enabledAttr) && !bypassed(mod.as<MeshPtr>(mod.inAttr)); };
    auto ranOnGpu = [](const ModifierNode& mod) { return (mod.traits() & Node::kDeformer) && static_cast<const DeformerNode&>(mod).ranOnGpu(); };
    // Evaluated results stay valid up to the first modifier whose export state differs from what the viewport ran, or
    // that ran on the GPU in float: exports get the CPU reference.
    auto split = std::find_if(mods.begin(), mods.end(), [&](NodeId id) {
        auto* mod = static_cast<ModifierNode*>(g.node(id));
        return ranInViewport(*mod) != exportEnabled(*mod) || ranOnGpu(*mod);
    });
    if (split == mods.end())
        return m->as<MeshPtr>(MeshNode::kOutMesh);

    auto* first = static_cast<ModifierNode*>(g.node(*split));
    MeshPtr cur = first->as<MeshPtr>(first->inAttr);
    deformEval::CpuOnly cpu;
    for (auto it = split; it != mods.end(); ++it)
    {
        auto* mod = static_cast<ModifierNode*>(g.node(*it));
        bool exported = exportEnabled(*mod);
        if (!exported || !cur)
            continue;
        if (!mod->apply)
        {
            log::warn("{}: can't re-run for export after an earlier modifier changed; exported without it", mod->path());
            continue;
        }
        try
        {
            cur = std::make_shared<geo::Mesh>(mod->apply(*stackInput(*mod, cur), *mod));
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", mod->path(), e.what());
        }
    }
    if (cur && geo::hasFitSpacing(*cur))
    {
        auto bare = std::make_shared<geo::Mesh>(*cur);
        geo::removeFitSpacing(*bare);
        cur = bare;
    }
    return cur;
}

void registerStackCommands()
{
    ops::registerCommand({
        .name = "modifierStack",
        .doc = "A mesh's stack as data: its source (generator, meshSource or null) and each modifier, applied first first, with "
               "name, type, enabled, exportEnabled and its last evaluation error.",
        .positionalDoc = "mesh",
        .undoable = false,
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("modifierStack: no mesh");
            Graph& g = graph();
            NodeId mesh = meshNamed(objs.back(), "modifierStack").id();
            g.evaluate();
            json mods = json::array();
            for (NodeId id : stack(g, mesh))
                mods.push_back(describe(g, id));
            NodeId src = stackSource(g, mesh);
            return {{"mesh", g.node(mesh)->path()}, {"source", src ? describe(g, src) : json(nullptr)}, {"modifiers", mods}};
        },
    });

    ops::registerCommand({
        .name = "modifierKinds",
        .doc = "Every modifier kind addModifier accepts, including ones extensions registered. docs=True returns {kind: description} "
               "instead.",
        .flags = {{"docs", "", ArgType::Bool, false, "Return each kind's description too."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (!a.get<bool>("docs"))
                return kinds();
            json out = json::object();
            for (const std::string& kind : kinds())
                out[kind] = kindDoc(kind);
            return out;
        },
    });

    ops::registerCommand({
        .name = "duplicateModifier",
        .doc = "Copy a modifier with its settings and insert the copy right after it in the stack. Returns the copy's name.",
        .positionalDoc = "modifier",
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("duplicateModifier: no modifier");
            Graph& g = graph();
            ModifierNode& orig = modifierNamed(objs.back(), "duplicateModifier");
            const NodeType& type = orig.type();
            auto* copy = static_cast<ModifierNode*>(g.createNode(type, {}, kNoNode, true));
            for (int i = 0; i < int(type.attrs.size()); ++i)
            {
                const AttrDef& def = type.attrs[i];
                if (i == orig.inAttr || !(def.flags & AttrInput))
                    continue;
                // Driven inputs (keys, geometry-node groups, object info) share the same driver.
                if (def.flags & AttrArray)
                    g.copyElements(orig.id(), copy->id(), i, true);
                else if (auto src = g.source({orig.id(), i, -1}))
                    g.connect(*src, {copy->id(), i, -1});
                else if (def.type != PlugType::Mesh)
                    g.setValue({copy->id(), i, -1}, orig.value(i));
            }
            PlugRef out{orig.id(), orig.outAttr, -1};
            std::vector<PlugRef> dsts = g.destinations(out);
            for (const PlugRef& d : dsts)
                g.disconnect(d);
            g.connect(out, {copy->id(), copy->inAttr, -1});
            for (const PlugRef& d : dsts)
                g.connect({copy->id(), copy->outAttr, -1}, d);
            return copy->path();
        },
    });

    ops::registerCommand({
        .name = "applyAllModifiers",
        .doc = "Bake every modifier of a mesh into its base mesh, leaving an empty stack over an editable source.",
        .positionalDoc = "meshes",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            for (const std::string& name : a.objects())
            {
                NodeId mesh = meshNamed(name, "applyAllModifiers").id();
                std::vector<NodeId> mods = stack(g, mesh);
                if (mods.empty())
                    continue;
                NodeId src = stackSource(g, mesh);
                ops::execute("applyModifier", {{"args", {g.node(mods.back())->path()}}});
                if (src && !dynamic_cast<MeshSourceNode*>(g.node(src)))
                    dropIfOrphan(g, src);
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "bakeModifierStack",
        .doc = "Bake meshes to their current result, deleting the generator and modifiers feeding them. Curves likewise keep what their history makes and lose the history; other objects are skipped.",
        .positionalDoc = "objects",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            for (const std::string& name : a.objects())
            {
                if (Node* n = g.find(name); n && !n->type().isA("mesh"))
                {
                    bakeCreateHistory(g, *n);
                    continue;
                }
                Node& mesh = meshNamed(name, "bakeModifierStack");
                std::vector<NodeId> mods = stack(g, mesh.id());
                for (NodeId id : mods)
                    if (static_cast<ModifierNode*>(g.node(id))->editingInput())
                        throw CommandError("bakeModifierStack: " + g.node(id)->path() + " is editing its edges: apply or cancel that first");
                PlugRef in{mesh.id(), MeshNode::kInMesh, -1};
                if (!g.source(in))
                    continue;
                g.evaluate();
                MeshPtr baked = mesh.as<MeshPtr>(MeshNode::kOutMesh);
                NodeId src = stackSource(g, mesh.id());
                g.disconnect(in);
                g.setValue(in, baked);
                for (auto it = mods.rbegin(); it != mods.rend(); ++it)
                    g.deleteNode(*it);
                if (src)
                    dropIfOrphan(g, src);
            }
            return nullptr;
        },
    });
}
} // namespace lila::scene::modifiers
