#include "scene/Components.h"

#include "geo/ComponentPaths.h"
#include "geo/Mesh.h"
#include "geo/Topology.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/Isolation.h"
#include "scene/Modifiers.h"

#include <string>

namespace lila::scene::components
{
namespace
{
struct Target
{
    NodeId node = kNoNode;
    MeshPtr mesh; // the cage, which component ids index
};

Target meshArg(const Args& a, const char* cmd)
{
    auto objs = a.objects();
    if (objs.empty())
        throw CommandError(std::string(cmd) + ": no mesh");
    Graph& g = Graph::current();
    Node* n = g.find(objs.back());
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + objs.back() + "'");
    if (!n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + objs.back() + " is not a mesh");
    g.evaluate();
    Target t{n->id(), modifiers::cage(g, n->id())};
    if (!t.mesh)
        throw CommandError(std::string(cmd) + ": " + objs.back() + " has no mesh");
    return t;
}

Op opArg(const Args& a, Op fallback)
{
    if (a.get<bool>("add"))
        return Op::Add;
    if (a.get<bool>("deselect"))
        return Op::Remove;
    if (a.get<bool>("toggle"))
        return Op::Toggle;
    return fallback;
}

void selectAndShow(NodeId node, ComponentMode kind, const Ids& ids, Op op, const Args& a)
{
    select(node, kind, a.get<bool>("symmetric") ? withSymmetry(node, kind, ids) : ids, op);
    setMode(kind);
}

/**
 * The loop or ring through two neighbouring components: vertices sharing an edge, edges meeting along a loop or facing
 * each other across a quad, faces sharing an edge. Empty when they are not neighbours.
 */
Ids neighbourLoop(const geo::Mesh& m, const isolation::Visibility& vis, ComponentMode kind, geo::Index a, geo::Index b)
{
    if (kind == ComponentMode::Vertex)
    {
        if (!m.pointValid(a) || !m.pointValid(b))
            return {};
        geo::Index e = m.findEdge(a, b);
        if (e == geo::kInvalid)
            return {};
        return convert(m, ComponentMode::Edge, isolation::clipLoop(m, vis, geo::edgeLoop(m, e), e, ComponentMode::Edge, false), ComponentMode::Vertex);
    }
    if (kind == ComponentMode::Edge)
    {
        if (!m.edgeValid(a) || !m.edgeValid(b))
            return {};
        for (int i = 0; i < 2; ++i)
            if (geo::edgeLoopNext(m, a, m.edgePoint(a, i)) == b)
                return isolation::clipLoop(m, vis, geo::edgeLoop(m, a), a, ComponentMode::Edge, false);
        bool facing = false;
        m.forEachEdgeCorner(a, [&](geo::Index c) { facing = facing || geo::quadOppositeEdge(m, a, m.cornerFace(c)) == b; });
        return facing ? isolation::clipLoop(m, vis, geo::edgeRing(m, a), a, ComponentMode::Edge, true) : Ids{};
    }
    if (!m.faceValid(a) || !m.faceValid(b))
        return {};
    geo::Index shared = geo::kInvalid;
    m.forEachFaceCorner(a, [&](geo::Index c) {
        geo::Index e = m.cornerEdge(c);
        m.forEachEdgeCorner(e, [&](geo::Index d) {
            if (m.cornerFace(d) == b)
                shared = e;
        });
    });
    return shared == geo::kInvalid ? Ids{} : isolation::clipLoop(m, vis, geo::faceLoop(m, shared), shared, ComponentMode::Face, false);
}
} // namespace

void registerPathCommands()
{
    using T = ArgType;
    const FlagDef opFlags[] = {
        {"add", "", T::Bool, false, "Add to the selection."},
        {"deselect", "", T::Bool, false, "Remove from the selection."},
        {"toggle", "", T::Bool, false, "Toggle."},
        {"symmetric", "", T::Bool, false, "Also the mirror images, when the mesh's symmetry is on."},
    };

    std::vector<FlagDef> loopFlags = {
        {"edge", "", T::Int, nullptr, "The edge the loop runs through.", {}, {}, 0.0},
        {"ring", "", T::Bool, false, "The edge ring instead of the loop."},
        {"faces", "", T::Bool, false, "The faces of the edge's ring: the face loop crossing the edge."},
    };
    loopFlags.insert(loopFlags.end(), std::begin(opFlags), std::end(opFlags));
    ops::registerCommand({
        .name = "selectLoopThrough",
        .doc = "Select the edge loop (or ring) through one edge of a mesh, or with faces=True the face loop crossing it. "
               "Without add/deselect/toggle it replaces the selection of that kind. Returns the ids.",
        .positionalDoc = "mesh",
        .flags = loopFlags,
        .run = [](const Args& a) -> json {
            Target t = meshArg(a, "selectLoopThrough");
            if (!a.has("edge"))
                throw CommandError("selectLoopThrough: give edge=");
            geo::Index e = geo::Index(a.get<int64_t>("edge"));
            if (!t.mesh->edgeValid(e))
                throw CommandError("selectLoopThrough: no edge " + std::to_string(e));
            bool faces = a.get<bool>("faces");
            const bool ring = !faces && a.get<bool>("ring");
            Ids ids = faces ? geo::faceLoop(*t.mesh, e) : ring ? geo::edgeRing(*t.mesh, e) : geo::edgeLoop(*t.mesh, e);
            ids = isolation::clipLoop(*t.mesh, isolation::activeVisibility(t.node, *t.mesh), ids, e, faces ? ComponentMode::Face : ComponentMode::Edge, ring);
            UndoChunk chunk("Select Loop");
            selectAndShow(t.node, faces ? ComponentMode::Face : ComponentMode::Edge, ids, opArg(a, Op::Replace), a);
            return ids;
        },
    });

    std::vector<FlagDef> connectedFlags = {
        {"kind", "", T::String, "face", "vertex | edge | face", {"vertex", "edge", "face"}},
        {"id", "", T::Int, nullptr, "A component of the piece to select.", {}, {}, 0.0},
    };
    connectedFlags.insert(connectedFlags.end(), std::begin(opFlags), std::end(opFlags));
    ops::registerCommand({
        .name = "selectConnected",
        .doc = "Select every component of a kind on the connected piece (faces joined through shared edges) that one component "
               "belongs to. Without add/deselect/toggle it replaces the selection of that kind. Returns the ids.",
        .positionalDoc = "mesh",
        .flags = connectedFlags,
        .run = [](const Args& a) -> json {
            Target t = meshArg(a, "selectConnected");
            if (!a.has("id"))
                throw CommandError("selectConnected: give id=");
            ComponentMode kind = ComponentMode::Face;
            try
            {
                kind = modeFromName(a.get<std::string>("kind", "face"));
            }
            catch (const std::invalid_argument&)
            {
                throw CommandError("selectConnected: kind must be vertex, edge or face");
            }
            if (kind == ComponentMode::Object)
                throw CommandError("selectConnected: kind must be vertex, edge or face");
            const geo::Mesh& m = *t.mesh;
            const geo::Index id = geo::Index(a.get<int64_t>("id"));
            const bool valid = kind == ComponentMode::Vertex ? m.pointValid(id) : kind == ComponentMode::Edge ? m.edgeValid(id) : m.faceValid(id);
            if (!valid)
                throw CommandError(std::string("selectConnected: no ") + modeName(kind) + " " + std::to_string(id));
            std::vector<geo::Index> shell;
            geo::faceComponents(m, shell);
            std::vector<uint8_t> want(shell.size() + 1, 0);
            auto seed = [&](geo::Index f) {
                if (f < shell.size() && shell[f] != geo::kInvalid)
                    want[shell[f]] = 1;
            };
            if (kind == ComponentMode::Face)
                seed(id);
            else if (kind == ComponentMode::Edge)
                m.forEachEdgeCorner(id, [&](geo::Index c) { seed(m.cornerFace(c)); });
            else
                for (geo::Index f : m.pointFaces(id))
                    seed(f);
            Ids faces;
            for (geo::Index f = 0; f < shell.size(); ++f)
                if (shell[f] != geo::kInvalid && want[shell[f]])
                    faces.push_back(f);
            // A loose edge or point has no faces to spread through, so it is its own piece.
            Ids ids = faces.empty() ? Ids{id} : convert(m, ComponentMode::Face, faces, kind);
            ids = isolation::visibleOnly(isolation::activeVisibility(t.node, m), kind, std::move(ids));
            UndoChunk chunk("Select Connected");
            selectAndShow(t.node, kind, ids, opArg(a, Op::Replace), a);
            return ids;
        },
    });

    ops::registerCommand({
        .name = "selectPathBetween",
        .doc = "Select the components between two of a mesh's vertices, edges or faces: the shorter way along a loop or ring "
               "they share, else the shortest path across the surface. Neighbours (vertices on one edge, edges along a loop or across "
               "a quad, faces sharing an edge) select the whole loop or ring through both instead, unless deselecting. Adds to the selection unless replace or deselect. Returns the ids in order.",
        .positionalDoc = "mesh",
        .flags = {
            {"kind", "", T::String, "edge", "vertex | edge | face", {"vertex", "edge", "face"}},
            {"from", "", T::Int, nullptr, "Id the path starts at.", {}, {}, 0.0},
            {"to", "", T::Int, nullptr, "Id the path ends at.", {}, {}, 0.0},
            {"replace", "", T::Bool, false, "Replace the selection of that kind."},
            {"deselect", "", T::Bool, false, "Remove the path from the selection."},
            {"symmetric", "", T::Bool, false, "Also the mirror images, when the mesh's symmetry is on."},
        },
        .run = [](const Args& a) -> json {
            Target t = meshArg(a, "selectPathBetween");
            if (!a.has("from") || !a.has("to"))
                throw CommandError("selectPathBetween: give from= and to=");
            ComponentMode kind = ComponentMode::Edge;
            try
            {
                kind = modeFromName(a.get<std::string>("kind", "edge"));
            }
            catch (const std::invalid_argument&)
            {
                throw CommandError("selectPathBetween: kind must be vertex, edge or face");
            }
            geo::Index from = geo::Index(a.get<int64_t>("from")), to = geo::Index(a.get<int64_t>("to"));
            if (kind == ComponentMode::Object)
                throw CommandError("selectPathBetween: kind must be vertex, edge or face");
            const bool deselect = a.get<bool>("deselect");
            Ids path = deselect ? Ids{} : neighbourLoop(*t.mesh, isolation::activeVisibility(t.node, *t.mesh), kind, from, to);
            if (path.empty())
                path = kind == ComponentMode::Vertex ? geo::pointPathBetween(*t.mesh, from, to)
                       : kind == ComponentMode::Edge ? geo::edgePathBetween(*t.mesh, from, to)
                                                     : geo::facePathBetween(*t.mesh, from, to);
            if (path.empty())
                throw CommandError("selectPathBetween: no path between " + std::to_string(from) + " and " + std::to_string(to));
            Op op = deselect ? Op::Remove : a.get<bool>("replace") ? Op::Replace : Op::Add;
            UndoChunk chunk("Select Path");
            selectAndShow(t.node, kind, path, op, a);
            return path;
        },
    });

    ops::registerCommand({
        .name = "selectContainedFaces",
        .doc = "Select the faces enclosed by the selected edges: a flood fill that stops at selected edges and mesh borders. "
               "On each piece of a mesh the smaller regions are kept (every one but the largest by area), or with larger=True "
               "only the largest. Switches to face mode.",
        .flags = {{"larger", "", T::Bool, false, "Keep the largest region instead of the smaller ones."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            g.evaluate();
            bool larger = a.get<bool>("larger");
            std::map<NodeId, Ids> faces;
            json out = json::object();
            for (const auto& [node, edges] : selected(ComponentMode::Edge))
            {
                MeshPtr m = modifiers::cage(g, node);
                Node* n = g.node(node);
                if (!m || !n || edges.empty())
                    continue;
                if (Ids ids = geo::containedFaces(*m, edges, !larger); !ids.empty())
                {
                    out[n->path()] = ids;
                    faces[node] = std::move(ids);
                }
            }
            if (faces.empty())
                throw CommandError("selectContainedFaces: select edges that close off a region");
            setAll({}, selected(ComponentMode::Edge), std::move(faces));
            setMode(ComponentMode::Face);
            return out;
        },
    });
}
} // namespace lila::scene::components
