#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "geo/Ops.h"
#include "geo/Reduce.h"
#include "geo/Symmetry.h"
#include "geo/Topology.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Isolation.h"
#include "scene/MeshEditInternal.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <format>
#include <map>

namespace lila::scene::detail
{
void registerMeshEditMeshOpsCommands()
{
    using T = ArgType;
    using K = ComponentMode;

    ops::registerCommand({
        .name = "polyReduce",
        .doc = "Reduce the polygon count of whole meshes while keeping their shape, silhouette, UVs and normals. The result is "
               "triangles (or quads with quads=True). A mesh with modifiers gets a decimate modifier on top instead. Honours "
               "the mesh's symmetry: one half (or one radial wedge) is reduced and mirrored (or repeated).",
        .positionalDoc = "objects",
        .flags = {{"ratio", "", T::Float, 0.5, "Fraction of triangles to keep, 0 to 1.", {}, {}, 0.0, 1.0},
                  {"targetTriangles", "", T::Int, 0, "Triangles to keep; above 0 it overrides ratio.", {}, {}, 0.0},
                  {"keepBorders", "", T::Bool, true, "Open borders stay exactly where they are."},
                  {"keepHardEdges", "", T::Bool, true, "Hard edges stay intact, as UV seams and material borders always do; off lets them collapse for a lower count."},
                  {"symmetry", "", T::String, "auto", "auto (the mesh's symmetry setting), off, x, y or z.", {"auto", "off", "x", "y", "z"}},
                  {"quads", "", T::Bool, false, "Join triangle pairs back into quads where they make good ones."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> nodes;
            for (const std::string& name : meshObjects(a, "polyReduce"))
                nodes.push_back(g.find(name)->id());
            const std::string sym = a.get<std::string>("symmetry", "auto");
            if (sym != "auto" && sym != "off" && sym != "x" && sym != "y" && sym != "z")
                throw CommandError("polyReduce: symmetry must be auto, off, x, y or z");
            geo::ReduceOptions o;
            o.ratio = std::clamp(a.get<double>("ratio"), 0.0, 1.0);
            o.targetTriangles = Index(std::max(0, a.get<int>("targetTriangles")));
            o.keepBorders = a.get<bool>("keepBorders");
            o.keepHardEdges = a.get<bool>("keepHardEdges");
            o.quads = a.get<bool>("quads");
            UndoChunk chunk("Reduce");
            json out = json::object();
            for (NodeId id : nodes)
            {
                Node& n = *g.node(id);
                const int axis = sym == "auto" ? symmetryAxis(n) : sym == "off" ? -1 : sym[0] - 'x';
                if (!modifiers::stack(g, id).empty())
                {
                    json params = {{"ratio", o.ratio}, {"targetTriangles", int64_t(o.targetTriangles)}, {"keepBorders", o.keepBorders},
                                   {"keepHardEdges", o.keepHardEdges}, {"symmetry", axis + 1}, {"quads", o.quads}};
                    out[n.path()] = ops::execute("addModifier", {{"args", {n.path()}}, {"kind", "decimate"}, {"params", params}});
                    continue;
                }
                PlugRef base = basePlug(n);
                geo::Mesh mesh = baseMesh(base);
                if (const auto spec = symmetrySpec(n); sym == "auto" && spec && spec->radial)
                {
                    // Radial: reduce one wedge with its seams held, then repeat it round the axis.
                    const double tol = symmetryTolerance(mesh);
                    geo::cutToWedge(mesh, *spec->radial, 0, tol);
                    mesh.compact();
                    geo::ReduceOptions wo = o;
                    wo.keepBorders = true;
                    wo.symmetryAxis = -1;
                    wo.targetTriangles = o.targetTriangles / Index(spec->radial->order());
                    geo::ReduceResult r = geo::reduceMesh(mesh, wo);
                    geo::symmetrizeRadialTopology(r.mesh, *spec->radial, 0, tol);
                    out[n.path()] = r.mesh.numFaces();
                    g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(r.mesh))));
                    continue;
                }
                o.symmetryAxis = axis;
                o.symmetryOffset = symmetryOffset(n, axis);
                o.symmetryTolerance = symmetryTolerance(mesh);
                geo::ReduceResult r = geo::reduceMesh(mesh, o);
                if (r.symmetryIgnored)
                    log::warn("polyReduce {}: faces straddle the symmetry plane, so it was reduced without symmetry", n.path());
                if (axis >= 0 && axis == symmetryAxis(n))
                {
                    geo::removeSymmetryLayers(r.mesh);
                    geo::SymmetryMap::persistent(r.mesh, axis, symmetryTolerance(r.mesh), nullptr, symmetryOffset(n, axis));
                }
                out[n.path()] = r.mesh.numFaces();
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(r.mesh))));
            }
            components::clear();
            return out;
        },
    });

    ops::registerCommand({
        .name = "polyQuadrangulate",
        .doc = "Join neighbouring triangles into quads.",
        .flags = {{"maxAngle", "", T::Float, 40.0, "Largest angle between the two triangles, degrees.", {}, {}, 0.0, 180.0}},
        .run = [](const Args& a) -> json {
            return editSelected("Quadrangulate", K::Face, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap*) {
                geo::QuadrangulateOptions o;
                o.maxFaceAngle = a.get<double>("maxAngle");
                return faces(geo::quadrangulateFaces(m, sel, o));
            }, true);
        },
    });

    ops::registerCommand({
        .name = "polySubdivide",
        .doc = "Catmull-Clark subdivide whole meshes (crease-aware).",
        .positionalDoc = "objects",
        .flags = {{"levels", "", T::Int, 1, "Subdivision levels.", {}, {}, 1.0, 5.0},
                  {"keepHardEdges", "", T::Bool, false, "Keep normals split along hard edges; off smooths the shading across them."},
                  {"uvs", "", T::String, "fitSurface", "Island interior uvs: fitSurface spreads them evenly over the smoothed surface; blend smooths them like the points.",
                   {"fitSurface", "blend"}},
                  {"uvCorners", "", T::String, "keep", "Island corner uvs: keep or smooth.", {"keep", "smooth"}}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            UndoChunk chunk("Subdivide");
            for (const std::string& name : meshObjects(a, "polySubdivide"))
            {
                Node& n = meshArg(name, "polySubdivide");
                PlugRef base = basePlug(n);
                geo::SubdivideOptions o;
                o.levels = std::clamp(a.get<int>("levels"), 1, 5);
                o.keepHardEdges = a.get<bool>("keepHardEdges", false);
                o.uvs = a.get<std::string>("uvs", "fitSurface") == "blend" ? geo::SubdivideUvs::Blend : geo::SubdivideUvs::FitSurface;
                o.uvCorners = a.get<std::string>("uvCorners", "keep") == "smooth" ? geo::SubdivideUvCorners::Smooth : geo::SubdivideUvCorners::Keep;
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(geo::subdivide(baseMesh(base), o))));
            }
            components::clear();
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "polyMirror",
        .doc = "Mirror whole meshes across the plane through the object's or the world's origin facing an axis, welding along "
               "the seam. direction positive keeps the + side and rebuilds the - side from it, cutting faces that cross the "
               "plane; negative does the reverse; both adds a mirrored copy of the whole mesh. flip=True reflects the mesh "
               "across the plane instead, keeping nothing of the original.",
        .positionalDoc = "objects",
        .flags = {{"axis", "", T::String, "x", "x, y or z.", {"x", "y", "z"}},
                  {"direction", "", T::String, "positive", "positive (+ to -), negative (- to +) or both.", {"positive", "negative", "both"}},
                  {"space", "", T::String, "object", "object or world: whose origin and axes the plane uses.", {"object", "world"}},
                  {"weld", "", T::Bool, true, "Weld points on the plane."},
                  {"tolerance", "", T::Float, 0.001, "Points this close to the plane are cut onto it and welded, cm.", {}, {}, 0.0},
                  {"flip", "", T::Bool, false, "Reflect the mesh across the plane rather than adding a mirrored copy: the original is not kept."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::string axis = a.get<std::string>("axis", "x");
            if (axis.size() != 1 || axis[0] < 'x' || axis[0] > 'z')
                throw CommandError("polyMirror: axis must be x, y or z");
            const std::string direction = a.get<std::string>("direction", "positive");
            if (direction != "positive" && direction != "negative" && direction != "both")
                throw CommandError("polyMirror: direction must be positive, negative or both");
            const std::string space = a.get<std::string>("space", "object");
            if (space != "object" && space != "world")
                throw CommandError("polyMirror: space must be object or world");
            if (space == "world")
                g.evaluate();
            UndoChunk chunk("Mirror");
            for (const std::string& name : meshObjects(a, "polyMirror"))
            {
                Node& n = meshArg(name, "polyMirror");
                PlugRef base = basePlug(n);
                geo::Mesh m = baseMesh(base);
                if (a.get<bool>("flip"))
                {
                    glm::dmat4 reflect(1.0);
                    reflect[axis[0] - 'x'][axis[0] - 'x'] = -1.0;
                    if (space == "world")
                    {
                        const glm::dmat4 w = n.as<glm::dmat4>(Transform::kWorldMatrix);
                        reflect = glm::inverse(w) * reflect * w;
                    }
                    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
                        if (m.pointValid(p))
                            m.setPosition(p, glm::dvec3(reflect * glm::dvec4(m.position(p), 1.0)));
                    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
                        if (m.faceValid(f))
                            m.flipFace(f);
                    g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
                    continue;
                }
                geo::MirrorOptions o;
                o.plane.normal = glm::dvec3(0.0);
                o.plane.normal[axis[0] - 'x'] = 1.0;
                o.direction = direction == "positive" ? geo::MirrorDirection::Positive
                            : direction == "negative" ? geo::MirrorDirection::Negative
                                                      : geo::MirrorDirection::Both;
                if (space == "world")
                    o.space = n.as<glm::dmat4>(Transform::kWorldMatrix);
                o.weld = a.get<bool>("weld");
                o.tolerance = std::max(0.0, a.get<double>("tolerance"));
                geo::mirrorMesh(m, o);
                // The result is exactly symmetric, so its pairs are re-found by position on the next edit.
                geo::removeSymmetryLayers(m);
                m.compact();
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "polySeparate",
        .doc = "Split meshes into one object per connected part. Returns the new names.",
        .positionalDoc = "objects",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            json out = json::array();
            std::vector<NodeId> made;
            for (const std::string& name : meshObjects(a, "polySeparate"))
            {
                Node& n = meshArg(name, "polySeparate");
                g.evaluate();
                std::vector<geo::Mesh> parts = geo::separate(*n.as<MeshPtr>(MeshNode::kOutMesh));
                if (parts.size() < 2)
                    continue;
                NodeId parent = g.parent(n.id());
                for (size_t i = 0; i < parts.size(); ++i)
                {
                    Node* part = g.createNode(n.type(), n.name(), parent, false);
                    for (int attr : {Transform::kTranslate, Transform::kRotate, Transform::kScale, Transform::kRotatePivot, Transform::kScalePivot,
                                     Transform::kRotatePivotTranslate, Transform::kScalePivotTranslate, Transform::kRotateOrder})
                        g.setValue({part->id(), attr, -1}, g.storedValue({n.id(), attr, -1}));
                    g.setValue({part->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(parts[i]))));
                    out.push_back(part->path());
                    made.push_back(part->id());
                }
                isolation::inherit(std::vector<NodeId>(made.end() - std::ptrdiff_t(parts.size()), made.end()), {n.id()});
                g.deleteNode(n.id());
            }
            g.select(made);
            return out;
        },
    });

    ops::registerCommand({
        .name = "polyCombine",
        .doc = "Merge meshes into one object (world space kept). Returns its name.",
        .positionalDoc = "objects",
        .flags = {{"name", "", T::String, nullptr, "Name of the combined mesh."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const std::vector<std::string> names = meshObjects(a, "polyCombine");
            if (names.size() < 2)
                throw CommandError("polyCombine: select at least two meshes, or a group holding them");
            g.evaluate();
            std::vector<geo::Mesh> worldMeshes;
            for (const std::string& name : names)
            {
                Node& n = meshArg(name, "polyCombine");
                geo::Mesh m = *n.as<MeshPtr>(MeshNode::kOutMesh);
                glm::dmat4 w = n.as<glm::dmat4>(Transform::kWorldMatrix);
                for (Index p = 0; p < m.pointCapacity(); ++p)
                    if (m.pointValid(p))
                        m.setPosition(p, glm::dvec3(w * glm::dvec4(m.position(p), 1.0)));
                // Mirrored transforms flip winding; keep faces pointing out.
                if (glm::determinant(glm::dmat3(w)) < 0.0)
                {
                    Ids all;
                    for (Index f = 0; f < m.faceCapacity(); ++f)
                        if (m.faceValid(f))
                            all.push_back(f);
                    geo::flipNormals(m, all);
                }
                worldMeshes.push_back(std::move(m));
            }
            std::vector<const geo::Mesh*> ptrs;
            for (const geo::Mesh& m : worldMeshes)
                ptrs.push_back(&m);
            geo::Mesh combined = geo::combine(ptrs);
            std::string last = names.back();
            last = last.substr(last.rfind('|') + 1);
            Node* out = g.createNode(*types::find("mesh"), a.get<std::string>("name", last), kNoNode, true);
            g.setValue({out->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(combined))));
            std::vector<NodeId> sources;
            for (const std::string& name : names)
                if (Node* n = g.find(name))
                    sources.push_back(n->id());
            for (NodeId id : sources)
                if (g.node(id))
                    g.deleteNode(id);
            isolation::inherit({out->id()}, sources);
            g.select({out->id()});
            return out->path();
        },
    });

    ops::registerCommand({
        .name = "polyExtract",
        .doc = "Move the selected faces out into new mesh objects (one per source mesh). Returns their names.",
        .run = [](const Args&) -> json {
            Graph& g = graph();
            g.evaluate();
            ComponentMode from = components::mode();
            if (from == K::Object)
                throw CommandError("polyExtract: no faces selected");
            std::map<NodeId, Ids> targets;
            for (auto& [node, ids] : components::selected(from))
                if (Node* n = g.node(node); n && n->type().isA("mesh"))
                    if (MeshPtr cage = modifiers::cage(g, node))
                        if (Ids f = components::convert(*cage, from, ids, K::Face); !f.empty())
                            targets[node] = std::move(f);
            if (targets.empty())
                throw CommandError("polyExtract: no faces selected");
            json out = json::array();
            std::vector<NodeId> made, sources;
            components::clear();
            for (auto& [node, sel] : targets)
            {
                Node& n = *g.node(node);
                PlugRef base = basePlug(n);
                geo::Mesh m = baseMesh(base);
                geo::Mesh part = geo::extractFaces(m, sel);
                part.compact();
                geo::deleteFaces(m, sel);
                m.compact();
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
                Node* piece = g.createNode(n.type(), n.name(), g.parent(n.id()), false);
                for (int attr : {Transform::kTranslate, Transform::kRotate, Transform::kScale, Transform::kRotatePivot, Transform::kScalePivot,
                                 Transform::kRotatePivotTranslate, Transform::kScalePivotTranslate, Transform::kRotateOrder})
                    g.setValue({piece->id(), attr, -1}, g.storedValue({n.id(), attr, -1}));
                g.setValue({piece->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(part))));
                out.push_back(piece->path());
                made.push_back(piece->id());
                sources.push_back(node);
            }
            isolation::inherit(made, sources);
            components::setMode(K::Object);
            g.select(made);
            return out;
        },
    });

    ops::registerCommand({
        .name = "polySoftEdge",
        .doc = "Make the selected edges soft or hard by the angle their faces meet at: sharper than angle turns hard, the "
               "rest soft. 180 softens every edge, 0 hardens every edge. In object mode, whole selected meshes.",
        .flags = {{"angle", "", T::Float, 180.0, "Edges whose faces meet sharper than this turn hard, degrees.", {}, {}, 0.0, 180.0}},
        .run = [](const Args& a) -> json {
            const double angle = a.get<double>("angle");
            return editSelected("Soften/Harden Edges", K::Edge, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                const Ids edges = withMirror(m, s, sel, K::Edge);
                if (angle >= 180.0)
                    geo::setSoftEdges(m, edges);
                else if (angle <= 0.0)
                {
                    std::span<bool> hard = m.edgeAttrs().getOrAdd<bool>(geo::attr::kHard);
                    auto harden = [&](Index e) {
                        if (m.edgeValid(e))
                            hard[e] = true;
                    };
                    if (edges.empty())
                        for (Index e = 0; e < m.edgeCapacity(); ++e)
                            harden(e);
                    for (Index e : edges)
                        harden(e);
                }
                else
                    geo::setHardEdgesByAngle(m, angle, edges);
                // Stored split normals would hide the change.
                if (m.cornerAttrs().has(geo::attr::kNormal))
                    geo::updateCornerNormals(m);
                return EditResult{};
            }, true);
        },
    });

    auto setEdgeWeight = [](const char* label, std::string_view attr, float value) {
        return editSelected(label, K::Edge, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
            std::span<float> w = m.edgeAttrs().getOrAdd<float>(attr);
            for (Index e : withMirror(m, s, sel, K::Edge))
                if (m.edgeValid(e))
                    w[e] = value;
            return edges(sel);
        });
    };

    ops::registerCommand({
        .name = "polyCrease",
        .doc = "Set the subdivision crease of the selected edges. Each subdivision level spends 1 of it, so 1 is sharp for "
               "one level and smooth after, and 10 stays sharp. 0 removes the crease.",
        .flags = {{"value", "v", T::Float, 10.0, "Crease: levels the edge stays sharp for.", {}, {}, 0.0, 10.0}},
        .run = [setEdgeWeight](const Args& a) -> json {
            return setEdgeWeight("Crease Edges", geo::attr::kCrease, float(a.get<double>("value")));
        },
    });

    ops::registerCommand({
        .name = "polyBevelWeight",
        .doc = "Set the bevel weight of the selected edges, read by Auto Bevel in weight mode (it bevels edges above its "
               "weightThreshold).",
        .flags = {{"value", "v", T::Float, 1.0, "Bevel weight, 0 to 1.", {}, {}, 0.0, 1.0}},
        .run = [setEdgeWeight](const Args& a) -> json {
            return setEdgeWeight("Bevel Weight", geo::attr::kBevelWeight, float(a.get<double>("value")));
        },
    });

    ops::registerCommand({
        .name = "polyDuplicateFaces",
        .doc = "Copy the selected faces into a separate shell of the same mesh and select the copies.",
        .run = [](const Args&) -> json {
            return editSelected("Duplicate Faces", K::Face, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                geo::Mesh part = geo::extractFaces(m, withMirror(m, s, sel, K::Face));
                part.compact();
                const Index first = m.faceCapacity();
                m.append(part);
                Ids made;
                for (Index f = first; f < m.faceCapacity(); ++f)
                    if (m.faceValid(f))
                        made.push_back(f);
                return faces(std::move(made));
            });
        },
    });

    ops::registerCommand({
        .name = "polyConnect",
        .doc = "Connect selected vertices that share a face with new edges, or the midpoints of selected edges across their faces.",
        .run = [](const Args&) -> json {
            K kind = components::mode() == K::Vertex ? K::Vertex : K::Edge;
            return editSelected("Connect", kind, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                Ids all = withMirror(m, s, sel, kind);
                Ids pts;
                if (kind == K::Edge)
                {
                    for (Index e : all)
                        if (m.edgeValid(e))
                            pts.push_back(m.splitEdge(e, 0.5));
                }
                else
                    pts = all;
                return edges(geo::connectPoints(m, pts));
            });
        },
    });
}
} // namespace lila::scene::detail
