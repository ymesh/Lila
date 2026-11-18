#include "Blockwork.h"

#include <lila/Deformer.h>
#include <lila/Prefs.h>
#include <lila/Relocate.h>
#include <lila/Transform.h>

#include <algorithm>
#include <cmath>

namespace blockwork
{
using namespace lila;
using namespace lila::scene;

namespace
{
constexpr const char* kGridPref = "/blockwork/grid";
constexpr const char* kAbsolutePref = "/manipulator/snap/move/absolute";

/** Lays every face's uvs out from its mapping, so edits by any tool keep the texture in place. */
class TextureLockNode final : public ModifierNode
{
public:
    enum Attr
    {
        kInMesh,
        kEnabled,
        kExportEnabled,
        kTileSize,
        kOutMesh,
    };

    TextureLockNode()
    {
        inAttr = kInMesh;
        enabledAttr = kEnabled;
        outAttr = kOutMesh;
        apply = [](const geo::Mesh& in, const Node& n) {
            geo::Mesh m = in;
            applyMapping(m, n.as<double>(kTileSize));
            return m;
        };
    }
};
} // namespace

bool registerTypes(ext::Context& ctx)
{
    NodeType t;
    t.name = kTextureLockType;
    t.icon = "lock-square";
    t.doc = "Texture Lock: lays the mesh's uvs out from each face's texture mapping (world or face aligned, offset, scale, rotation), "
            "so the texture stays put on the surface through every edit instead of stretching. Blockwork blocks carry one.";
    t.ownAttrs = {
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden},
        {.name = "enabled", .type = PlugType::Bool, .defaultValue = true, .flags = AttrInput | AttrStorable | AttrKeyable,
         .doc = "Lay the uvs out from the mapping; off keeps the uvs the mesh stores, which then stretch with edits."},
        {.name = "exportEnabled", .type = PlugType::Bool, .defaultValue = true, .flags = AttrInput | AttrStorable, .doc = "Apply when exporting."},
        {.name = "tileSize", .type = PlugType::Float, .defaultValue = 100.0, .flags = AttrInput | AttrStorable | AttrKeyable,
         .doc = "cm per texture repeat on faces that set no scale of their own.", .minValue = 0.01},
        {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden},
    };
    t.create = [] { return std::make_unique<TextureLockNode>(); };
    relocate::registerNeutral(t.name);
    return types::registerType(ctx, std::move(t)) != nullptr;
}

double gridStep()
{
    const double s = PrefStore::get().value<double>("general", kGridPref, 16.0);
    return s > 0.0 ? s : 16.0;
}

void setGridStep(double step)
{
    step = std::clamp(step, 0.125, 4096.0);
    PrefStore& p = PrefStore::get();
    p.set("general", kGridPref, step);
    // Lila's own move snapping follows the grid, so gizmo moves land on it too.
    p.set("general", "/manipulator/snap/move/step", step);
}

bool gridAbsolute() { return PrefStore::get().value<bool>("general", kAbsolutePref, true); }

Graph& graph() { return Graph::current(); }

bool isBlock(const Node& n)
{
    if (!n.type().isA("mesh"))
        return false;
    Graph& g = graph();
    for (NodeId id : modifiers::stack(g, n.id()))
        if (const Node* m = g.node(id); m && m->type().name == kTextureLockType)
            return true;
    return false;
}

std::vector<NodeId> blockArgs(const Args& a, const char* cmd, bool allWhenEmpty)
{
    Graph& g = graph();
    std::vector<NodeId> out;
    std::vector<std::string> names = a.objects();
    for (const std::string& name : names)
    {
        Node* n = g.find(name);
        if (!n)
            throw CommandError(std::string(cmd) + ": no node '" + name + "'");
        if (n->type().isA("mesh"))
            out.push_back(n->id());
        else
            // A group stands for the meshes under it.
            for (NodeId c : g.children(n->id()))
                if (Node* k = g.node(c); k && k->type().isA("mesh"))
                    out.push_back(c);
    }
    if (out.empty() && names.empty() && allWhenEmpty)
        for (NodeId id : g.nodes())
            if (Node* n = g.node(id); n && isBlock(*n))
                out.push_back(id);
    return out;
}

geo::Mesh baseMesh(Graph& g, NodeId mesh)
{
    MeshPtr m = modifiers::cage(g, mesh);
    return m ? *m : geo::Mesh();
}

void setBaseMesh(Graph& g, NodeId mesh, geo::Mesh m)
{
    g.setValue(modifiers::editPlug(g, mesh), MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
}

geo::Mesh worldMesh(Graph& g, NodeId mesh)
{
    geo::Mesh m = baseMesh(g, mesh);
    g.evaluate();
    const glm::dmat4 w = g.node(mesh)->as<glm::dmat4>(Transform::kWorldMatrix);
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            m.setPosition(p, glm::dvec3(w * glm::dvec4(m.position(p), 1.0)));
    return m;
}

NodeId levelGroup(Graph& g, const std::string& name)
{
    if (Node* n = g.find(name))
        return n->id();
    return g.createNode(*types::find("transform"), name)->id();
}

NodeId createBlock(Graph& g, geo::Mesh mesh, const std::string& name, NodeId parent, const std::string& material)
{
    if (!material.empty())
        materials::assign(mesh, material, {});
    const glm::dvec3 centre = mesh.bounds().center();
    Node* n = g.createNode(*types::find("mesh"), name.empty() ? "block1" : name, parent);
    g.setValue({n->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
    // Points stay in world space so every block shares one texture space; the pivot sits at the centre for the gizmo.
    g.setValue({n->id(), Transform::kRotatePivot, -1}, centre);
    g.setValue({n->id(), Transform::kScalePivot, -1}, centre);
    ops::execute("addModifier", {{"args", {g.fullPath(n->id())}}, {"kind", kTextureLockKind}});
    return n->id();
}
} // namespace blockwork
