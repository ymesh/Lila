#include "Blockwork.h"

#include <lila/Boolean.h>
#include <lila/Components.h>
#include <lila/Deformer.h>
#include <lila/MeshOps.h>
#include <lila/Prefs.h>
#include <lila/Transform.h>

#include <algorithm>
#include <cmath>

namespace blockwork
{
using namespace lila;
using namespace lila::scene;
using geo::Index;

namespace
{
glm::dvec3 vec3Flag(const Args& a, const char* name, const glm::dvec3& fallback)
{
    if (!a.has(name))
        return fallback;
    const json& v = a.flags[name];
    if (!v.is_array() || v.size() != 3)
        throw CommandError(std::string(name) + ": give three numbers");
    return {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
}

/** The mesh's faces: the faces flag, or its selected faces. */
std::vector<Index> faceArg(const Args& a, NodeId mesh)
{
    std::vector<Index> out;
    if (a.has("faces"))
        for (const json& f : a.flags["faces"])
            out.push_back(f.get<Index>());
    else
        for (uint32_t f : components::selected(mesh, ComponentMode::Face))
            out.push_back(f);
    return out;
}

std::string graphPathOf(Graph& g, NodeId id) { return g.fullPath(id); }

/** Meshes in the scene other than skip whose world bounds overlap box (grown by pad). */
std::vector<NodeId> overlapping(Graph& g, const geo::Bounds& box, const std::vector<NodeId>& skip, double pad, bool blocksOnly)
{
    std::vector<NodeId> out;
    g.evaluate();
    for (NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n || !n->type().isA("mesh") || std::find(skip.begin(), skip.end(), id) != skip.end() || (blocksOnly && !isBlock(*n)))
            continue;
        const geo::Bounds b = worldMesh(g, id).bounds();
        if (!b.valid())
            continue;
        if (b.min.x <= box.max.x + pad && b.max.x >= box.min.x - pad && b.min.y <= box.max.y + pad && b.max.y >= box.min.y - pad &&
            b.min.z <= box.max.z + pad && b.max.z >= box.min.z - pad)
            out.push_back(id);
    }
    return out;
}

/** The mesh in the space of `into` (both its world placement taken out). */
geo::Mesh meshIn(Graph& g, NodeId mesh, NodeId into)
{
    geo::Mesh m = worldMesh(g, mesh);
    const glm::dmat4 inv = glm::inverse(g.node(into)->as<glm::dmat4>(Transform::kWorldMatrix));
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            m.setPosition(p, glm::dvec3(inv * glm::dvec4(m.position(p), 1.0)));
    return m;
}

/** Runs a boolean of base against the others in base's own space; the result keeps base's face mapping layers where it can. */
geo::Mesh booleanInto(Graph& g, NodeId base, const std::vector<geo::Mesh>& others, BooleanOp op, const char* cmd)
{
    const geo::Mesh own = baseMesh(g, base);
    std::vector<BooleanOperand> ops = {{&own, glm::dmat4(1.0)}};
    for (const geo::Mesh& m : others)
        ops.push_back({&m, glm::dmat4(1.0)});
    BooleanOptions opts;
    opts.op = op;
    opts.splitNgons = false;
    opts.hardAngle = 40.0;
    BooleanResult r = meshBoolean(ops, glm::dmat4(1.0), opts);
    if (!r.ok())
        throw CommandError(std::string(cmd) + ": " + r.error);
    return std::move(r.mesh);
}
} // namespace

void registerCommands(ext::Context& ctx)
{
    using T = ArgType;

    ctx.registerCommand({
        .name = "blockworkBlock",
        .doc = "Build a block: a box, wedge, cylinder, arch, stairs or prism standing on a rectangle, as a mesh whose points are in world "
               "space with a Texture Lock modifier. Returns the block's path.\n\n"
               "The rectangle runs from origin along uAxis by size[0] and across by size[1]; the block rises along normal by size[2]. "
               "A zero height makes a floor face; a footprint with one zero side makes a wall face (box only).",
        .flags = {{"shape", "s", T::String, "box", "box, wedge, cylinder, arch, stairs or prism.", shapeNames()},
                  {"origin", "o", T::Vec3, json::array({0, 0, 0}), "A corner of the footprint, world cm."},
                  {"size", "sz", T::Vec3, json::array({128, 128, 128}), "Extent along uAxis, across it, and up the normal, cm; negative goes the other way."},
                  {"normal", "n", T::Vec3, json::array({0, 1, 0}), "The direction the block rises: the surface it stands on faces this way."},
                  {"uAxis", "u", T::Vec3, nullptr, "The footprint's first side direction; default the world axis that lies in the plane."},
                  {"sides", "", T::Int, 12, "Cylinder and arch sides.", {}, {}, 3, 256},
                  {"radiusToCorners", "", T::Bool, true, "Cylinder: corners on the drawn radii; off puts side centres there."},
                  {"skipCaps", "", T::Bool, false, "Cylinder: leave the ends open."},
                  {"ngonCaps", "", T::Bool, true, "Cylinder: each end as one face; off fans triangles to a centre point."},
                  {"steps", "", T::Int, 8, "Stairs: number of steps; 0 takes it from stepHeight.", {}, {}, 0, 512},
                  {"stepHeight", "", T::Float, 0.0, "Stairs: rise per step, cm, when steps is 0.", {}, {}, 0.0},
                  {"thickness", "", T::Float, 25.0, "Arch: frame width around the opening, cm.", {}, {}, 0.0},
                  {"opening", "", T::Bool, false, "Arch: the opening instead of the frame, round at the far end of the footprint, run through the height; for carving doorways."},
                  {"direction", "d", T::Int, 0, "Quarter turns of the stairs' climb, the wedge's slope or the arch's opening.", {}, {}, 0, 3},
                  {"profile", "p", T::List, nullptr, "Prism: the outline as [[u, v], ...] from origin, cm; concave works."},
                  {"name", "", T::String, "block1", "Node name."},
                  {"parent", "", T::String, kDefaultGroup, "Group to put it under, made when missing; empty for the world."},
                  {"material", "m", T::String, "", "Material slot for every face; empty keeps the default."},
                  {"suffix", "", T::String, "", "Appended to the name, such as _col for engines that take collision by name."},
                  {"flip", "", T::Bool, false, "Faces point inward, for a room you stand inside."},
                  {"removeOverlap", "", T::Bool, false, "Remove faces pressed against other blocks' faces, from both, so no hidden faces remain."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const auto shape = shapeFromName(a.get<std::string>("shape", "box"));
            if (!shape)
                throw CommandError("blockworkBlock: unknown shape '" + a.get<std::string>("shape") + "'");
            Frame f;
            f.origin = vec3Flag(a, "origin", glm::dvec3(0.0));
            f.size = vec3Flag(a, "size", glm::dvec3(128.0));
            f.normal = glm::normalize(vec3Flag(a, "normal", glm::dvec3(0.0, 1.0, 0.0)));
            planeBasis(f.normal, f.u, f.v);
            if (a.has("uAxis"))
            {
                glm::dvec3 u = vec3Flag(a, "uAxis", f.u);
                u -= f.normal * glm::dot(u, f.normal);
                if (glm::length(u) < 1e-9)
                    throw CommandError("blockworkBlock: uAxis runs along the normal");
                f.u = glm::normalize(u);
                f.v = acrossAxis(f.normal, f.u);
            }
            ShapeOptions o;
            o.sides = a.get<int>("sides", 12);
            o.radiusToCorners = a.get<bool>("radiusToCorners", true);
            o.skipCaps = a.get<bool>("skipCaps", false);
            o.ngonCaps = a.get<bool>("ngonCaps", true);
            o.steps = a.get<int>("steps", 8);
            o.stepHeight = a.get<double>("stepHeight", 0.0);
            o.thickness = a.get<double>("thickness", 25.0);
            o.direction = a.get<int>("direction", 0);
            o.opening = a.get<bool>("opening", false);
            if (a.has("profile"))
                for (const json& p : a.flags["profile"])
                    o.profile.push_back({p.at(0).get<double>(), p.at(1).get<double>()});
            geo::Mesh m = buildShape(*shape, f, o);
            if (m.empty())
                throw CommandError("blockworkBlock: the " + a.get<std::string>("shape", "box") + " has no size; give it a footprint and a height");
            if (a.get<bool>("flip", false))
            {
                std::vector<Index> all;
                for (Index i = 0; i < m.faceCapacity(); ++i)
                    if (m.faceValid(i))
                        all.push_back(i);
                geo::flipNormals(m, all);
            }
            if (a.get<bool>("removeOverlap", false))
            {
                std::vector<Index> own;
                for (NodeId other : overlapping(g, m.bounds(), {}, 0.01, false))
                {
                    geo::Mesh theirs = worldMesh(g, other);
                    for (Index f2 : coveredFaces(m, theirs))
                        own.push_back(f2);
                    std::vector<Index> gone = coveredFaces(theirs, m);
                    if (!gone.empty())
                    {
                        geo::Mesh base = baseMesh(g, other);
                        geo::deleteFaces(base, gone);
                        base.removeAllLoose();
                        setBaseMesh(g, other, std::move(base));
                    }
                }
                std::sort(own.begin(), own.end());
                own.erase(std::unique(own.begin(), own.end()), own.end());
                geo::deleteFaces(m, own);
                m.removeAllLoose();
                m.compact();
                if (m.empty())
                    throw CommandError("blockworkBlock: every face is pressed against another block");
            }
            const std::string parentName = a.get<std::string>("parent", kDefaultGroup);
            const NodeId parent = parentName.empty() ? kNoNode : levelGroup(g, parentName);
            const NodeId id = createBlock(g, std::move(m), a.get<std::string>("name", "block1") + a.get<std::string>("suffix", ""), parent,
                                          a.get<std::string>("material", ""));
            g.select({id});
            return graphPathOf(g, id);
        },
        .adjustable = true,
        .examples = {"blockworkBlock(origin=[0, 0, 0], size=[512, 384, 256])", "blockworkBlock(shape='stairs', size=[128, 256, 128], steps=8)",
                     "blockworkBlock(shape='prism', profile=[[0, 0], [256, 0], [256, 128], [128, 128], [128, 256], [0, 256]], size=[0, 0, 64])"},
    });

    ctx.registerCommand({
        .name = "blockworkBevel",
        .doc = "Bevel blocks' exposed edges with a live Block Bevel modifier: only convex edges, and none lying on another "
               "block's face (resting on it or butting it), so no groove opens where blocks meet. It sits under a Hotspot "
               "modifier so the bevels are trimmed too. Run it again after moving blocks to find the touching edges afresh. "
               "Returns the modifiers.",
        .positionalDoc = "blocks (default the selection, or every block)",
        .flags = {{"width", "w", T::Float, 2.0, "cm from each edge to the bevel's edges.", {}, {}, 0.0},
                  {"segments", "sg", T::Int, 1, "1 is a chamfer; more round it.", {}, {}, 1, 8},
                  {"angle", "an", T::Float, 30.0, "Only edges whose faces meet sharper than this, degrees.", {}, {}, 0.0, 180.0},
                  {"remove", "rm", T::Bool, false, "Take the bevel off instead."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const std::vector<NodeId> ids = blockArgs(a, "blockworkBevel", true);
            if (ids.empty())
                throw CommandError("blockworkBevel: no blocks");
            json out = json::array();
            for (NodeId id : ids)
            {
                NodeId bevel = kNoNode, hotspot = kNoNode;
                for (NodeId m : modifiers::stack(g, id))
                    if (const Node* n = g.node(m); n && n->type().name == kBevelType)
                        bevel = m;
                    else if (n && n->type().name == "trimHotspotModifier")
                        hotspot = m;
                if (a.get<bool>("remove", false))
                {
                    if (bevel != kNoNode)
                        ops::execute("removeModifier", {{"args", json::array({g.fullPath(bevel)})}});
                    continue;
                }
                geo::Mesh mine = worldMesh(g, id);
                std::vector<geo::Mesh> others;
                for (NodeId other : overlapping(g, mine.bounds(), {id}, 0.01, false))
                    others.push_back(worldMesh(g, other));
                geo::Mesh base = baseMesh(g, id);
                const glm::dmat4 w = g.node(id)->as<glm::dmat4>(Transform::kWorldMatrix), toLocal = glm::inverse(w);
                // Split off the stretch of an edge another block covers only part of, so that stretch stays square.
                for (int guard = 0; guard < 64; ++guard)
                {
                    geo::Mesh plain = mine;
                    plain.edgeAttrs().remove(kContactAttr);
                    const std::vector<Index> rounded = bevelableEdges(plain, a.get<double>("angle", 30.0));
                    std::optional<ContactCut> cut;
                    for (const geo::Mesh& o : others)
                        if ((cut = partialContactCut(mine, o, rounded)))
                            break;
                    if (!cut)
                        break;
                    geo::cutAlongPlane(base, glm::dvec3(toLocal * glm::dvec4(cut->point, 1.0)),
                                       glm::normalize(glm::transpose(glm::dmat3(w)) * cut->normal), 1e-4, {}, cut->faces);
                    mine = base;
                    for (Index p = 0; p < mine.pointCapacity(); ++p)
                        if (mine.pointValid(p))
                            mine.setPosition(p, glm::dvec3(w * glm::dvec4(mine.position(p), 1.0)));
                }
                std::vector<Index> touching;
                for (const geo::Mesh& o : others)
                {
                    const std::vector<Index> f = touchingEdges(mine, o);
                    touching.insert(touching.end(), f.begin(), f.end());
                }
                markContact(base, touching);
                setBaseMesh(g, id, std::move(base));
                if (bevel == kNoNode)
                {
                    Node* mod = g.createNode(*types::find(kBevelType), {}, kNoNode, true);
                    // Under the hotspot, so the bevel faces are trimmed with the rest.
                    const auto* hot = hotspot == kNoNode ? nullptr : dynamic_cast<const ModifierNode*>(g.node(hotspot));
                    modifiers::insert(g, *mod, id, hot ? PlugRef{hotspot, hot->inAttr, -1} : PlugRef{id, MeshNode::kInMesh, -1});
                    bevel = mod->id();
                }
                const std::string path = g.fullPath(bevel);
                ops::execute("setAttr", {{"args", {path + ".width", a.get<double>("width", 2.0)}}});
                ops::execute("setAttr", {{"args", {path + ".segments", a.get<int>("segments", 1)}}});
                ops::execute("setAttr", {{"args", {path + ".angle", a.get<double>("angle", 30.0)}}});
                out.push_back(path);
            }
            return out;
        },
        .examples = {"blockworkBevel(width=2)", "blockworkBevel('arch1', width=4, segments=3)", "blockworkBevel(remove=True)"},
    });

    ctx.registerCommand({
        .name = "blockworkHollow",
        .doc = "Turn each block into walls of the given thickness around its old volume (a room), or outside it. The block's selected faces, "
               "or the faces flag, are left open with their rims closed. Returns the blocks.",
        .positionalDoc = "blocks (default the selection)",
        .flags = {{"thickness", "t", T::Float, 16.0, "Wall thickness, cm.", {}, {}, 0.01},
                  {"inward", "i", T::Bool, true, "Walls inside the block, so it becomes the room's outside; off builds them outside it."},
                  {"faces", "f", T::List, nullptr, "Faces to leave open; default the selected faces."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            json out = json::array();
            for (NodeId id : blockArgs(a, "blockworkHollow"))
            {
                geo::Mesh m = hollow(baseMesh(g, id), a.get<double>("thickness", 16.0), a.get<bool>("inward", true), faceArg(a, id));
                if (m.empty())
                    throw CommandError("blockworkHollow: " + g.node(id)->name() + " has no faces to wall");
                setBaseMesh(g, id, std::move(m));
                out.push_back(graphPathOf(g, id));
            }
            if (out.empty())
                throw CommandError("blockworkHollow: select a block");
            components::clear();
            return out;
        },
        .adjustable = true,
        .examples = {"blockworkHollow('block1', thickness=16)"},
    });

    ctx.registerCommand({
        .name = "blockworkClip",
        .doc = "Cut blocks along a plane and close the cut. keep picks the side the normal points to (front), the other (back), or both, "
               "which splits each block in two. Returns the blocks, new ones last.",
        .positionalDoc = "blocks (default the selection)",
        .flags = {{"point", "p", T::Vec3, nullptr, "A point on the plane, world cm."},
                  {"normal", "n", T::Vec3, nullptr, "The plane's normal, pointing to the front side."},
                  {"keep", "k", T::String, "front", "front, back or both.", {"front", "back", "both"}}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            if (!a.has("point") || !a.has("normal"))
                throw CommandError("blockworkClip: give the plane's point and normal");
            const glm::dvec3 point = vec3Flag(a, "point", {}), normal = vec3Flag(a, "normal", {});
            if (glm::length(normal) < 1e-9)
                throw CommandError("blockworkClip: the normal has no length");
            const std::string keep = a.get<std::string>("keep", "front");
            json out = json::array(), made = json::array();
            for (NodeId id : blockArgs(a, "blockworkClip"))
            {
                g.evaluate();
                const glm::dmat4 inv = glm::inverse(g.node(id)->as<glm::dmat4>(Transform::kWorldMatrix));
                const glm::dvec3 p = glm::dvec3(inv * glm::dvec4(point, 1.0));
                const glm::dvec3 n = glm::normalize(glm::dvec3(glm::transpose(glm::inverse(glm::dmat3(inv))) * normal));
                const geo::Mesh base = baseMesh(g, id);
                geo::Mesh front = clip(base, p, n, keep == "back" ? -1 : 1);
                if (keep == "both")
                {
                    geo::Mesh back = clip(base, p, n, -1);
                    if (!back.empty() && back.numFaces() > 0 && front.numFaces() > 0)
                    {
                        const json dup = ops::execute("duplicate", {{"args", {graphPathOf(g, id)}}});
                        const std::string copy = dup.is_array() ? dup.at(0).get<std::string>() : dup.get<std::string>();
                        setBaseMesh(g, g.find(copy)->id(), std::move(back));
                        made.push_back(copy);
                    }
                }
                if (front.numFaces() == 0)
                    ops::execute("delete", {{"args", {graphPathOf(g, id)}}});
                else
                {
                    setBaseMesh(g, id, std::move(front));
                    out.push_back(graphPathOf(g, id));
                }
            }
            if (out.empty() && made.empty())
                throw CommandError("blockworkClip: select a block");
            for (const json& m : made)
                out.push_back(m);
            return out;
        },
        .adjustable = true,
        .examples = {"blockworkClip('block1', point=[0, 64, 0], normal=[1, 1, 0], keep='front')"},
    });

    ctx.registerCommand({
        .name = "blockworkCarve",
        .doc = "Cut the cutter's shape out of every block it overlaps, then delete the cutter. Returns the blocks that were cut.",
        .positionalDoc = "cutter (default the selection's last mesh)",
        .flags = {{"keep", "", T::Bool, false, "Keep the cutter."}, {"blocks", "b", T::List, nullptr, "Only these blocks; default every block it overlaps."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> sel = blockArgs(a, "blockworkCarve");
            if (sel.empty())
                throw CommandError("blockworkCarve: select the cutter");
            const NodeId cutter = sel.back();
            const geo::Mesh cut = worldMesh(g, cutter);
            std::vector<NodeId> targets;
            if (a.has("blocks"))
                for (const json& n : a.flags["blocks"])
                    if (Node* t = g.find(n.get<std::string>()))
                        targets.push_back(t->id());
            if (!a.has("blocks"))
                targets = overlapping(g, cut.bounds(), {cutter}, 0.0, true);
            json out = json::array();
            for (NodeId t : targets)
            {
                const geo::Mesh inT = meshIn(g, cutter, t);
                geo::Mesh r = booleanInto(g, t, {inT}, BooleanOp::Difference, "blockworkCarve");
                if (r.numFaces() == 0)
                {
                    ops::execute("delete", {{"args", {graphPathOf(g, t)}}});
                    continue;
                }
                setBaseMesh(g, t, std::move(r));
                out.push_back(graphPathOf(g, t));
            }
            if (!a.get<bool>("keep", false))
                ops::execute("delete", {{"args", {graphPathOf(g, cutter)}}});
            return out;
        },
        .examples = {"blockworkCarve('doorCutter')"},
    });

    ctx.registerCommand({
        .name = "blockworkJoin",
        .doc = "Merge blocks into one: the first keeps its name and takes in the rest, with faces where they meet removed. With one block, "
               "it joins every block touching it. Returns the joined block.",
        .positionalDoc = "blocks (default the selection)",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> ids = blockArgs(a, "blockworkJoin");
            if (ids.empty())
                throw CommandError("blockworkJoin: select the blocks to join");
            if (ids.size() == 1)
                for (NodeId t : overlapping(g, worldMesh(g, ids[0]).bounds(), {ids[0]}, 0.01, true))
                    ids.push_back(t);
            if (ids.size() < 2)
                throw CommandError("blockworkJoin: nothing touches " + g.node(ids[0])->name());
            std::vector<geo::Mesh> others;
            for (size_t i = 1; i < ids.size(); ++i)
                others.push_back(meshIn(g, ids[i], ids[0]));
            setBaseMesh(g, ids[0], booleanInto(g, ids[0], others, BooleanOp::Union, "blockworkJoin"));
            for (size_t i = 1; i < ids.size(); ++i)
                ops::execute("delete", {{"args", {graphPathOf(g, ids[i])}}});
            g.select({ids[0]});
            return graphPathOf(g, ids[0]);
        },
        .examples = {"blockworkJoin(['room1', 'corridor1'])"},
    });

    ctx.registerCommand({
        .name = "blockworkExtend",
        .doc = "Push faces along their normals: move resizes the block, extrude grows new geometry from the faces, copy builds a new block "
               "from them. Returns the block (or the new one).",
        .positionalDoc = "block (default the selection)",
        .flags = {{"distance", "d", T::Float, 16.0, "How far, cm; negative pulls in."},
                  {"mode", "m", T::String, "move", "move, extrude or copy.", {"move", "extrude", "copy"}},
                  {"faces", "f", T::List, nullptr, "Faces to push; default the selected faces."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> ids = blockArgs(a, "blockworkExtend");
            if (ids.empty())
                throw CommandError("blockworkExtend: select a block's faces");
            const NodeId id = ids.front();
            const std::vector<Index> faces = faceArg(a, id);
            if (faces.empty())
                throw CommandError("blockworkExtend: select faces to push");
            const double d = a.get<double>("distance", 16.0);
            const std::string mode = a.get<std::string>("mode", "move");
            geo::Mesh m = baseMesh(g, id);
            if (mode == "move")
            {
                // Each point moves by the sum of its pushed faces' moves, so a corner shared by two pushed faces goes diagonally.
                std::vector<glm::dvec3> delta(m.pointCapacity(), glm::dvec3(0.0));
                std::vector<std::vector<glm::dvec3>> seen(m.pointCapacity());
                for (Index f : faces)
                {
                    if (!m.faceValid(f))
                        continue;
                    const glm::dvec3 n = m.faceNormal(f);
                    for (Index p : m.facePoints(f))
                        if (std::none_of(seen[p].begin(), seen[p].end(), [&](const glm::dvec3& k) { return glm::dot(k, n) > 0.9999; }))
                        {
                            seen[p].push_back(n);
                            delta[p] += n * d;
                        }
                }
                for (Index p = 0; p < m.pointCapacity(); ++p)
                    if (m.pointValid(p))
                        m.setPosition(p, m.position(p) + delta[p]);
                setBaseMesh(g, id, std::move(m));
                return graphPathOf(g, id);
            }
            geo::ExtrudeOptions opts;
            opts.offset = d;
            if (mode == "extrude")
            {
                geo::extrudeFaces(m, faces, opts);
                hardenEdges(m);
                setBaseMesh(g, id, std::move(m));
                return graphPathOf(g, id);
            }
            geo::ExtrudeCopy c = geo::extrudeCopy(m, faces, opts, true);
            // Close the copy where it met the source, then build it as its own block.
            std::vector<Index> rim;
            for (Index e = 0; e < c.mesh.edgeCapacity(); ++e)
                if (c.mesh.edgeValid(e) && c.mesh.isBoundaryEdge(e))
                    rim.push_back(e);
            geo::fillHoles(c.mesh, rim);
            geo::conformNormals(c.mesh);
            c.mesh.compact();
            g.evaluate();
            const glm::dmat4 w = g.node(id)->as<glm::dmat4>(Transform::kWorldMatrix);
            for (Index p = 0; p < c.mesh.pointCapacity(); ++p)
                if (c.mesh.pointValid(p))
                    c.mesh.setPosition(p, glm::dvec3(w * glm::dvec4(c.mesh.position(p), 1.0)));
            hardenEdges(c.mesh);
            const NodeId made = createBlock(g, std::move(c.mesh), g.node(id)->name(), g.parent(id), {});
            g.select({made});
            return graphPathOf(g, made);
        },
        .adjustable = true,
        .examples = {"blockworkExtend('block1', faces=[1], distance=64)", "blockworkExtend('block1', faces=[1], distance=128, mode='copy')"},
    });

    ctx.registerCommand({
        .name = "blockworkFlip",
        .doc = "Turn blocks inside out (or the given faces), for rooms seen from inside. Returns the blocks.",
        .positionalDoc = "blocks (default the selection)",
        .flags = {{"faces", "f", T::List, nullptr, "Only these faces; default every face, or the selected faces when there are some."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            json out = json::array();
            for (NodeId id : blockArgs(a, "blockworkFlip"))
            {
                geo::Mesh m = baseMesh(g, id);
                std::vector<Index> faces = faceArg(a, id);
                if (faces.empty())
                    for (Index f = 0; f < m.faceCapacity(); ++f)
                        if (m.faceValid(f))
                            faces.push_back(f);
                geo::flipNormals(m, faces);
                setBaseMesh(g, id, std::move(m));
                out.push_back(graphPathOf(g, id));
            }
            return out;
        },
    });

    ctx.registerCommand({
        .name = "blockworkGrid",
        .doc = "Set or read the Blockwork grid: the step every Blockwork tool and Lila's move snapping use. Returns {step, absolute}.",
        .flags = {{"step", "s", T::Float, nullptr, "Grid step, cm (1/8 to 4096).", {}, {}, 0.125, 4096.0},
                  {"finer", "", T::Bool, false, "Halve the step."},
                  {"coarser", "", T::Bool, false, "Double the step."},
                  {"absolute", "a", T::Bool, nullptr, "Snap moves onto grid lines; off snaps them by whole steps from where they start."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.has("step"))
                setGridStep(a.get<double>("step"));
            if (a.get<bool>("finer", false))
                setGridStep(gridStep() * 0.5);
            if (a.get<bool>("coarser", false))
                setGridStep(gridStep() * 2.0);
            if (a.has("absolute"))
                PrefStore::get().set("general", "/manipulator/snap/move/absolute", a.get<bool>("absolute"));
            return {{"step", gridStep()}, {"absolute", gridAbsolute()}};
        },
        .examples = {"blockworkGrid(step=32)", "blockworkGrid(finer=True)"},
    });
}
} // namespace blockwork
