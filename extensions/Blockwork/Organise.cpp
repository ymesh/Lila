#include "Entities.h"
#include "Tool.h"

#include <lila/Layouts.h>
#include <lila/Log.h>
#include <lila/MeshOps.h>
#include <lila/Paths.h>
#include <lila/Prefs.h>
#include <lila/Transform.h>

#include <QUuid>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace blockwork
{
using namespace lila;
using namespace lila::scene;
using geo::Index;

namespace
{
constexpr const char* kVisgroupAttr = "blockworkVisgroup";
constexpr const char* kHiddenAttr = "blockworkHidden";
constexpr const char* kPrefabAttr = "blockworkPrefab";
constexpr const char* kPlacementAttr = "blockworkPlacement";
constexpr const char* kPrefabFolderPref = "/blockwork/prefabFolder";

/** Tool materials: what each keeps of a face. */
struct ToolMaterial
{
    const char* name;
    glm::dvec3 color;
    bool render;
    bool collide;
    const char* doc;
};
const ToolMaterial kToolMaterials[] = {
    {"ToolNodraw", {0.85, 0.75, 0.2}, false, true, "Never drawn; still collides. For faces players never see."},
    {"ToolClip", {0.6, 0.3, 0.8}, false, true, "Invisible collision: player clips, smoothing over stairs."},
    {"ToolSkip", {0.4, 0.45, 0.55}, false, false, "Neither drawn nor colliding: faces kept only while building."},
};

const ToolMaterial* toolMaterial(const std::string& name)
{
    for (const ToolMaterial& t : kToolMaterials)
        if (name == t.name)
            return &t;
    return nullptr;
}

std::string faceMaterial(const geo::Mesh& m, Index f)
{
    std::span<const int32_t> ids = m.faceAttrs().get<int32_t>(geo::attr::kMaterial);
    if (f < ids.size() && ids[f] >= 0 && size_t(ids[f]) < m.materialNames().size())
        return m.materialNames()[size_t(ids[f])];
    return {};
}

/** The block's evaluated mesh (uvs laid out) in world space. */
geo::Mesh evaluatedWorld(Graph& g, NodeId id)
{
    Node* n = g.node(id);
    const MeshPtr& m = n->as<MeshPtr>(MeshNode::kOutMesh);
    geo::Mesh w = m ? *m : geo::Mesh();
    const glm::dmat4 mat = n->as<glm::dmat4>(Transform::kWorldMatrix);
    for (Index p = 0; p < w.pointCapacity(); ++p)
        if (w.pointValid(p))
            w.setPosition(p, glm::dvec3(mat * glm::dvec4(w.position(p), 1.0)));
    return w;
}

bool convex(const geo::Mesh& m)
{
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const glm::dvec3 n = m.faceNormal(f), c = m.faceCentroid(f);
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p) && glm::dot(m.position(p) - c, n) > 0.05)
                return false;
    }
    return true;
}

std::vector<NodeId> blocksUnder(Graph& g, NodeId root)
{
    std::vector<NodeId> out;
    std::vector<NodeId> stack = {root};
    while (!stack.empty())
    {
        const NodeId id = stack.back();
        stack.pop_back();
        for (NodeId c : g.children(id))
        {
            stack.push_back(c);
            if (Node* n = g.node(c); n && isBlock(*n))
                out.push_back(c);
        }
    }
    return out;
}

bool boolAttr(const Node& n, const char* name)
{
    const int i = n.type().attrIndex(name);
    return i >= 0 && std::holds_alternative<bool>(n.value(i)) && n.as<bool>(i);
}

std::vector<NodeId> visgroups(Graph& g)
{
    std::vector<NodeId> out;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && boolAttr(*n, kVisgroupAttr))
            out.push_back(id);
    return out;
}

std::vector<std::string> membersOf(Graph& g, NodeId set)
{
    return ops::execute("setMembers", {{"args", {g.fullPath(set)}}, {"nodes", true}}).get<std::vector<std::string>>();
}

/** Every visgroup member's visibility: shown unless a hidden visgroup holds it. */
void applyVisgroups(Graph& g)
{
    std::map<std::string, bool> shown;
    for (NodeId v : visgroups(g))
    {
        const bool hidden = boolAttr(*g.node(v), kHiddenAttr);
        for (const std::string& m : membersOf(g, v))
            shown[m] = shown.count(m) ? shown[m] && !hidden : !hidden;
    }
    for (const auto& [m, on] : shown)
        if (Node* n = g.find(m))
            if (n->as<bool>(Transform::kVisibility) != on)
                g.setValue({n->id(), Transform::kVisibility, -1}, on);
}

std::filesystem::path prefabFolder()
{
    const std::string f = PrefStore::get().value<std::string>("general", kPrefabFolderPref, "");
    return f.empty() ? paths::userConfigDir() / "prefabs" : std::filesystem::path(f);
}

json meshJson(const geo::Mesh& src, const glm::dmat4& toLocal)
{
    geo::Mesh m = src;
    m.compact();
    json pts = json::array(), sizes = json::array(), idx = json::array(), faces = json::array();
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        const glm::dvec3 q = glm::dvec3(toLocal * glm::dvec4(m.position(p), 1.0));
        pts.push_back({q.x, q.y, q.z});
    }
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        std::vector<Index> fp = m.facePoints(f);
        sizes.push_back(fp.size());
        for (Index p : fp)
            idx.push_back(p);
        const FaceMapping fm = faceMapping(m, f);
        faces.push_back({{"material", faceMaterial(m, f)}, {"mode", fm.mode}, {"offset", {fm.offset.x, fm.offset.y}}, {"scale", {fm.scale.x, fm.scale.y}},
                         {"rotate", fm.rotate}, {"u", {fm.uAxis.x, fm.uAxis.y, fm.uAxis.z}}, {"v", {fm.vAxis.x, fm.vAxis.y, fm.vAxis.z}}});
    }
    return {{"points", pts}, {"sizes", sizes}, {"indices", idx}, {"faces", faces}};
}

geo::Mesh meshFromJson(const json& j, const glm::dmat4& toWorld)
{
    std::vector<glm::dvec3> pts;
    for (const json& p : j["points"])
        pts.push_back(glm::dvec3(toWorld * glm::dvec4(p[0].get<double>(), p[1].get<double>(), p[2].get<double>(), 1.0)));
    const std::vector<Index> sizes = j["sizes"].get<std::vector<Index>>(), idx = j["indices"].get<std::vector<Index>>();
    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    const json& faces = j["faces"];
    std::map<std::string, std::vector<uint32_t>> byMat;
    for (Index f = 0; f < m.faceCapacity() && f < faces.size(); ++f)
    {
        const json& fj = faces[f];
        FaceMapping fm;
        fm.mode = fj.value("mode", 0);
        fm.offset = {fj["offset"][0].get<float>(), fj["offset"][1].get<float>()};
        fm.scale = {fj["scale"][0].get<float>(), fj["scale"][1].get<float>()};
        fm.rotate = fj.value("rotate", 0.0f);
        // Explicit axes turn with the placement.
        const glm::dmat3 r(toWorld);
        fm.uAxis = glm::vec3(glm::normalize(r * glm::dvec3(fj["u"][0].get<double>(), fj["u"][1].get<double>(), fj["u"][2].get<double>())));
        fm.vAxis = glm::vec3(glm::normalize(r * glm::dvec3(fj["v"][0].get<double>(), fj["v"][1].get<double>(), fj["v"][2].get<double>())));
        setFaceMapping(m, f, fm);
        if (!fj.value("material", std::string()).empty())
            byMat[fj["material"].get<std::string>()].push_back(f);
    }
    for (const auto& [mat, list] : byMat)
        if (materials::find(graph(), mat))
            materials::assign(m, mat, list);
    hardenEdges(m);
    return m;
}

glm::dmat4 placement(const glm::dvec3& at, const glm::dvec3& normal, double turn)
{
    glm::dmat4 r = glm::rotate(glm::dmat4(1.0), glm::radians(turn), glm::dvec3(0.0, 1.0, 0.0));
    const glm::dvec3 n = glm::normalize(normal);
    if (glm::dot(n, glm::dvec3(0.0, 1.0, 0.0)) < 0.9999)
    {
        const glm::dvec3 axis = glm::cross(glm::dvec3(0.0, 1.0, 0.0), n);
        const double angle = std::acos(std::clamp(n.y, -1.0, 1.0));
        r = glm::rotate(glm::dmat4(1.0), angle, glm::length(axis) > 1e-9 ? glm::normalize(axis) : glm::dvec3(1.0, 0.0, 0.0)) * r;
    }
    return glm::translate(glm::dmat4(1.0), at) * r;
}

/** The placement a prefab group was put down with, in its own space. */
glm::dmat4 groupPlacement(const Node& group)
{
    glm::dmat4 m(1.0);
    const int i = group.type().attrIndex(kPlacementAttr);
    if (i < 0)
        return m;
    try
    {
        const json j = json::parse(group.as<std::string>(i));
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                m[c][r] = j.at(size_t(c * 4 + r)).get<double>();
    }
    catch (const std::exception&)
    {
    }
    return m;
}

void setGroupPlacement(Graph& g, NodeId group, const glm::dmat4& m)
{
    json j = json::array();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            j.push_back(m[c][r]);
    const std::string path = g.fullPath(group);
    if (g.node(group)->type().attrIndex(kPlacementAttr) < 0)
        ops::execute("addAttr", {{"args", {path}}, {"longName", kPlacementAttr}, {"dataType", "string"}, {"hidden", true}});
    ops::execute("setAttr", {{"args", {path + "." + kPlacementAttr, j.dump()}}});
}

/** Builds a prefab's contents under group: placement puts it in the group's space. */
void buildPrefab(Graph& g, const json& doc, NodeId group, const glm::dmat4& placement)
{
    g.evaluate();
    const glm::dmat4 toWorld = g.node(group)->as<glm::dmat4>(Transform::kWorldMatrix) * placement;
    for (const json& b : doc.value("blocks", json::array()))
        createBlock(g, meshFromJson(b["mesh"], placement), b.value("name", std::string("block1")), group, {});
    for (const json& e : doc.value("entities", json::array()))
    {
        const EntityClass* c = findEntityClass(e.value("class", std::string()));
        if (!c)
        {
            log::warn("Blockwork: prefab entity class '{}' is not defined; skipped", e.value("class", std::string()));
            continue;
        }
        const glm::dvec3 p(e["position"][0].get<double>(), e["position"][1].get<double>(), e["position"][2].get<double>());
        const glm::dvec3 w = glm::dvec3(toWorld * glm::dvec4(p, 1.0));
        const std::string path = ops::execute("blockworkEntity", {{"class", c->name}, {"position", {w.x, w.y, w.z}}, {"turn", e.value("turn", 0.0)},
                                                                  {"name", e.value("name", c->name + "1")}, {"parent", ""}})
                                     .get<std::string>();
        g.setParent(g.find(path)->id(), group, true);
        for (const auto& [k, v] : e.value("properties", json::object()).items())
            if (!v.is_null())
                ops::execute("setAttr", {{"args", {g.fullPath(g.find(path)->id()) + "." + k, v}}});
    }
}

json readPrefab(const std::string& file)
{
    std::ifstream in{std::filesystem::path(file)};
    if (!in)
        throw CommandError("no prefab at " + file);
    return json::parse(in);
}
} // namespace

void registerOrganise(ext::Context& ctx)
{
    using T = ArgType;

    ctx.registerCommand({
        .name = "blockworkToolMaterials",
        .doc = "Make the tool materials: ToolNodraw (never drawn, still collides), ToolClip (invisible collision) and ToolSkip (dropped). "
               "blockworkMerge leaves their faces out of the render mesh. Returns their names.",
        .run = [](const Args&) -> json {
            Graph& g = graph();
            json out = json::array();
            for (const ToolMaterial& t : kToolMaterials)
            {
                if (!materials::find(g, t.name))
                {
                    Node* m = materials::create(g, t.name);
                    ops::execute("setAttr", {{"args", {g.fullPath(m->id()) + ".baseColor", {t.color.x, t.color.y, t.color.z}}}});
                }
                out.push_back(t.name);
            }
            return out;
        },
    });

    ctx.registerCommand({
        .name = "blockworkMerge",
        .doc = "Merge a group's blocks into one mesh for the engine: faces with tool materials left out, entities and volumes skipped, "
               "and a UCX_ convex collision hull per block. Concave blocks can't be hulls: with collision auto, any concave block "
               "makes the mesh collide as itself instead, and the result says so. The blocks stay; merging again replaces the mesh. "
               "Returns {mesh, collision, hulls, concave}.",
        .positionalDoc = "group (default Level)",
        .flags = {{"name", "n", T::String, nullptr, "The merged mesh's name; default the group's name with Mesh after it."},
                  {"collision", "c", T::String, "auto", "auto, hulls (UCX_ per convex block), mesh (the render mesh) or none.",
                   {"auto", "hulls", "mesh", "none"}}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const std::string groupName = a.positional.empty() ? std::string(kDefaultGroup) : a.positional[0].get<std::string>();
            Node* group = g.find(groupName);
            if (!group)
                throw CommandError("blockworkMerge: no group '" + groupName + "'");
            g.evaluate();
            const std::string name = a.get<std::string>("name", group->name() + "Mesh");
            std::vector<std::string> globalMats;
            std::vector<geo::Mesh> parts, hulls;
            std::vector<std::string> concave;
            for (NodeId id : blocksUnder(g, group->id()))
            {
                if (isEntity(*g.node(id)) || g.node(id)->name() == name)
                    continue;
                geo::Mesh m = evaluatedWorld(g, id);
                bool collides = false;
                std::vector<Index> drop;
                for (Index f = 0; f < m.faceCapacity(); ++f)
                {
                    if (!m.faceValid(f))
                        continue;
                    const ToolMaterial* t = toolMaterial(faceMaterial(m, f));
                    collides = collides || !t || t->collide;
                    if (t && !t->render)
                        drop.push_back(f);
                }
                if (collides)
                {
                    std::vector<glm::dvec3> pts;
                    for (Index p = 0; p < m.pointCapacity(); ++p)
                        if (m.pointValid(p))
                            pts.push_back(m.position(p));
                    if (convex(m))
                        hulls.push_back(geo::convexHullMesh(pts, 256));
                    else
                        concave.push_back(g.node(id)->name());
                }
                geo::deleteFaces(m, drop);
                m.removeAllLoose();
                m.compact();
                if (m.numFaces() == 0)
                    continue;
                // Material ids into one shared list.
                std::span<int32_t> ids = m.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
                for (int32_t& id2 : ids)
                {
                    const std::string mat = id2 >= 0 && size_t(id2) < m.materialNames().size() ? m.materialNames()[size_t(id2)] : materials::kDefaultSlot;
                    auto it = std::find(globalMats.begin(), globalMats.end(), mat);
                    if (it == globalMats.end())
                        it = globalMats.insert(globalMats.end(), mat);
                    id2 = int32_t(it - globalMats.begin());
                }
                m.materialNames().clear();
                parts.push_back(std::move(m));
            }
            if (parts.empty())
                throw CommandError("blockworkMerge: " + groupName + " has no blocks to merge");
            geo::Mesh merged;
            for (geo::Mesh& p : parts)
            {
                p.materialNames() = globalMats;
                merged.append(p);
            }
            merged.materialNames() = globalMats;

            std::string mode = a.get<std::string>("collision", "auto");
            if (mode == "auto")
                mode = concave.empty() ? "hulls" : "mesh";
            if (mode == "hulls" && !concave.empty())
                log::warn("blockworkMerge: {} concave block(s) have no hull: {}", concave.size(), concave.front());
            if (mode == "mesh" && a.get<std::string>("collision", "auto") == "auto" && !concave.empty())
                log::warn("blockworkMerge: {} is concave, so {} collides as its own mesh instead of hulls; split it with Clip to use hulls",
                          concave.front(), name);

            if (Node* old = g.find(name))
                ops::execute("delete", {{"args", {g.fullPath(old->id())}}});
            Node* out = g.createNode(*types::find("mesh"), name);
            g.setValue({out->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(merged))));
            int made = 0;
            if (mode == "hulls")
                for (geo::Mesh& h : hulls)
                {
                    Node* u = g.createNode(*types::find("mesh"), "UCX_" + name + "_" + std::to_string(++made), out->id(), false);
                    g.setValue({u->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(h))));
                }
            return {{"mesh", g.fullPath(out->id())}, {"collision", mode}, {"hulls", made}, {"concave", concave}};
        },
        .examples = {"blockworkMerge('Level')", "blockworkMerge('Level', collision='hulls')"},
    });

    ctx.registerCommand({
        .name = "blockworkVisgroup",
        .doc = "Visgroups: named sets of objects to hide and show together; an object can be in several and stays hidden while any "
               "of its hidden groups is. With a name and no other flag, makes the group from the selection. Returns every visgroup "
               "as {name, hidden, members}.",
        .positionalDoc = "visgroup name",
        .flags = {{"add", "a", T::List, nullptr, "Objects to add; empty list adds the selection."},
                  {"remove", "r", T::List, nullptr, "Objects to take out; empty list takes the selection out."},
                  {"hidden", "h", T::Bool, nullptr, "Hide or show the group."},
                  {"delete", "", T::Bool, false, "Delete the group (its objects stay)."},
                  {"select", "s", T::Bool, false, "Select its objects."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            auto sel = [&]() {
                std::vector<std::string> s;
                for (NodeId id : g.selection())
                    s.push_back(g.fullPath(id));
                return s;
            };
            auto listOf = [&](const char* flag) {
                std::vector<std::string> l = a.flags[flag].get<std::vector<std::string>>();
                return l.empty() ? sel() : l;
            };
            if (!a.positional.empty())
            {
                const std::string name = a.positional[0].get<std::string>();
                Node* set = g.find(name);
                if (set && !boolAttr(*set, kVisgroupAttr))
                    throw CommandError("blockworkVisgroup: " + name + " is not a visgroup");
                if (!set)
                {
                    if (a.get<bool>("delete", false))
                        throw CommandError("blockworkVisgroup: no visgroup " + name);
                    const std::vector<std::string> members = a.has("add") ? listOf("add") : sel();
                    json call = {{"name", name}, {"empty", members.empty()}};
                    if (!members.empty())
                        call["args"] = members;
                    set = g.find(ops::execute("createSet", call).get<std::string>());
                    const std::string p = g.fullPath(set->id());
                    ops::execute("addAttr", {{"args", {p}}, {"longName", kVisgroupAttr}, {"attributeType", "bool"}, {"defaultValue", true}, {"hidden", true}});
                    ops::execute("addAttr", {{"args", {p}}, {"longName", kHiddenAttr}, {"attributeType", "bool"}, {"defaultValue", false},
                                             {"doc", "The group's objects are hidden."}});
                    ops::execute("setAttr", {{"args", {p + "." + kVisgroupAttr, true}}});
                }
                else
                {
                    const std::string p = g.fullPath(set->id());
                    if (a.get<bool>("delete", false))
                    {
                        ops::execute("setAttr", {{"args", {p + "." + kHiddenAttr, false}}});
                        applyVisgroups(g);
                        ops::execute("delete", {{"args", {p}}});
                    }
                    else
                    {
                        auto withSet = [&](std::vector<std::string> l) {
                            json args = json::array({p});
                            for (const std::string& m : l)
                                args.push_back(m);
                            return args;
                        };
                        if (a.has("add"))
                            ops::execute("addToSet", {{"args", withSet(listOf("add"))}});
                        if (a.has("remove"))
                            ops::execute("removeFromSet", {{"args", withSet(listOf("remove"))}});
                        if (a.has("hidden"))
                            ops::execute("setAttr", {{"args", {p + "." + kHiddenAttr, a.get<bool>("hidden")}}});
                        if (a.get<bool>("select", false))
                            ops::execute("selectSetMembers", {{"args", {p}}});
                    }
                }
                applyVisgroups(g);
            }
            json out = json::array();
            for (NodeId v : visgroups(g))
                out.push_back({{"name", g.node(v)->name()}, {"hidden", boolAttr(*g.node(v), kHiddenAttr)}, {"members", membersOf(g, v)}});
            return out;
        },
        .examples = {"blockworkVisgroup('Upstairs')", "blockworkVisgroup('Upstairs', hidden=True)", "blockworkVisgroup('Upstairs', add=['stairs1'])"},
    });

    ctx.registerCommand({
        .name = "blockworkPrefabSave",
        .doc = "Save blocks and entities as a prefab file (.lilaprefab) other scenes can place. Positions are kept relative to the "
               "first object's pivot, or the group's when a placed prefab group is given. Returns the file.",
        .positionalDoc = "objects or a placed prefab group (default the selection)",
        .flags = {{"name", "n", T::String, nullptr, "File name in the prefab folder; default the first object's name."},
                  {"path", "p", T::String, nullptr, "Write here instead of the prefab folder."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            g.evaluate();
            std::vector<NodeId> roots;
            for (const std::string& n : a.objects())
                if (Node* k = g.find(n))
                    roots.push_back(k->id());
            if (roots.empty())
                throw CommandError("blockworkPrefabSave: select the blocks and entities to save");
            // A placed prefab group saves its contents, relative to itself.
            glm::dmat4 toLocal(1.0);
            std::vector<NodeId> items;
            Node* first = g.node(roots.front());
            if (roots.size() == 1 && first->type().attrIndex(kPrefabAttr) >= 0)
            {
                toLocal = glm::inverse(first->as<glm::dmat4>(Transform::kWorldMatrix) * groupPlacement(*first));
                for (NodeId c : g.children(first->id()))
                    items.push_back(c);
            }
            else
            {
                const glm::dvec3 pivot = glm::dvec3(first->as<glm::dmat4>(Transform::kWorldMatrix) * glm::dvec4(first->as<glm::dvec3>(Transform::kRotatePivot), 1.0));
                toLocal = glm::translate(glm::dmat4(1.0), glm::dvec3(-pivot.x, -pivot.y, -pivot.z));
                items = roots;
            }
            json doc = {{"format", "lilaprefab"}, {"version", 1}, {"blocks", json::array()}, {"entities", json::array()}};
            for (NodeId id : items)
            {
                Node* n = g.node(id);
                if (isEntity(*n) && !isBlock(*n))
                {
                    const EntityClass* c = findEntityClass(n->as<std::string>(n->type().attrIndex("entityClass")));
                    const glm::dmat4 w = n->as<glm::dmat4>(Transform::kWorldMatrix);
                    const glm::dvec3 p = glm::dvec3(toLocal * w[3]);
                    json props = json::object();
                    if (c)
                        for (const EntityProperty& pr : c->properties)
                            if (int i = n->type().attrIndex(pr.name); i >= 0)
                                std::visit([&](const auto& v) {
                                    using V = std::decay_t<decltype(v)>;
                                    if constexpr (std::is_same_v<V, double> || std::is_same_v<V, int64_t> || std::is_same_v<V, bool> || std::is_same_v<V, std::string>)
                                        props[pr.name] = v;
                                    else if constexpr (std::is_same_v<V, glm::dvec3>)
                                        props[pr.name] = {v.x, v.y, v.z};
                                },
                                           n->value(i));
                    doc["entities"].push_back({{"name", n->name()}, {"class", c ? c->name : std::string()}, {"position", {p.x, p.y, p.z}},
                                               {"turn", n->as<glm::dvec3>(Transform::kRotate).y}, {"properties", props}});
                }
                else if (isBlock(*n))
                    doc["blocks"].push_back({{"name", n->name()}, {"mesh", meshJson(worldMesh(g, id), toLocal)}});
            }
            if (doc["blocks"].empty() && doc["entities"].empty())
                throw CommandError("blockworkPrefabSave: nothing to save; select blocks or entities");
            std::filesystem::path file = a.has("path") ? std::filesystem::path(a.get<std::string>("path"))
                                                       : prefabFolder() / (a.get<std::string>("name", first->name()) + ".lilaprefab");
            std::filesystem::create_directories(file.parent_path());
            std::ofstream(file) << doc.dump(1);
            return file.generic_string();
        },
        .examples = {"blockworkPrefabSave(['doorway', 'torch1'], name='Doorway')"},
    });

    ctx.registerCommand({
        .name = "blockworkPrefabPlace",
        .doc = "Place a prefab: its blocks and entities in a group that remembers the file, stood on the point and turned to the "
               "normal. Returns the group.",
        .positionalDoc = "prefab file, or a name in the prefab folder",
        .flags = {{"position", "p", T::Vec3, json::array({0, 0, 0}), "Where it stands, world cm."},
                  {"normal", "n", T::Vec3, json::array({0, 1, 0}), "The surface's normal; the prefab's up turns to it."},
                  {"turn", "t", T::Float, 0.0, "Turn around its up, degrees."},
                  {"parent", "", T::String, kDefaultGroup, "Group it goes under, made when missing; empty for the world."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            if (a.positional.empty())
                throw CommandError("blockworkPrefabPlace: name the prefab");
            std::string file = a.positional[0].get<std::string>();
            if (!std::filesystem::exists(std::filesystem::path(file)))
                file = (prefabFolder() / (file + ".lilaprefab")).generic_string();
            const json doc = readPrefab(file);
            const json p = a.flags["position"], n = a.flags["normal"];
            const glm::dmat4 at = placement({p[0].get<double>(), p[1].get<double>(), p[2].get<double>()},
                                            {n[0].get<double>(), n[1].get<double>(), n[2].get<double>()}, a.get<double>("turn", 0.0));
            const std::string parentName = a.get<std::string>("parent", kDefaultGroup);
            const NodeId parent = parentName.empty() ? kNoNode : levelGroup(g, parentName);
            Node* group = g.createNode(*types::find("transform"), std::filesystem::path(file).stem().string() + "1", parent);
            const std::string gp = g.fullPath(group->id());
            ops::execute("addAttr", {{"args", {gp}}, {"longName", kPrefabAttr}, {"dataType", "string"}, {"usedAsFilename", true},
                                     {"doc", "The prefab file this group was placed from."}});
            ops::execute("setAttr", {{"args", {gp + "." + kPrefabAttr, file}}});
            // The group stays untransformed, so its blocks' points sit in world space and share the level's texture space.
            setGroupPlacement(g, group->id(), at);
            buildPrefab(g, doc, group->id(), at);
            g.select({group->id()});
            return gp;
        },
        .examples = {"blockworkPrefabPlace('Doorway', position=[512, 0, 0])"},
    });

    ctx.registerCommand({
        .name = "blockworkPrefabUpdate",
        .doc = "Save a placed prefab group's contents back to its file, then rebuild every other group placed from that file in "
               "place. Returns the groups rebuilt.",
        .positionalDoc = "placed prefab group (default the selection)",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<std::string> objs = a.objects();
            Node* src = objs.empty() ? nullptr : g.find(objs.front());
            while (src && src->type().attrIndex(kPrefabAttr) < 0)
                src = g.node(g.parent(src->id()));
            if (!src)
                throw CommandError("blockworkPrefabUpdate: select a placed prefab group or something in one");
            const std::string file = src->as<std::string>(src->type().attrIndex(kPrefabAttr));
            ops::execute("blockworkPrefabSave", {{"args", {g.fullPath(src->id())}}, {"path", file}});
            const json doc = readPrefab(file);
            json out = json::array();
            for (NodeId id : g.nodes())
            {
                Node* n = g.node(id);
                if (!n || id == src->id() || n->type().attrIndex(kPrefabAttr) < 0 || n->as<std::string>(n->type().attrIndex(kPrefabAttr)) != file)
                    continue;
                std::vector<std::string> old;
                for (NodeId c : g.children(id))
                    old.push_back(g.fullPath(c));
                if (!old.empty())
                    ops::execute("delete", {{"args", old}});
                buildPrefab(g, doc, id, groupPlacement(*n));
                out.push_back(g.fullPath(id));
            }
            return out;
        },
    });

    ctx.registerCommand({
        .name = "blockworkPrefabs",
        .doc = "The prefab files in the prefab folder, or set the folder. Returns {folder, prefabs}.",
        .flags = {{"folder", "f", T::String, nullptr, "Use this folder; empty goes back to the default."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.has("folder"))
                PrefStore::get().set("general", kPrefabFolderPref, a.get<std::string>("folder"));
            json list = json::array();
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(prefabFolder(), ec))
                if (e.path().extension() == ".lilaprefab")
                    list.push_back(e.path().generic_string());
            return {{"folder", prefabFolder().generic_string()}, {"prefabs", list}};
        },
    });

    // Four views over the level, with the Blockwork panels beside them.
    ui::layouts::add(ctx, "Blockwork",
                     {{"version", 1},
                      {"preset",
                       {{"open", {"outliner", "channelBox", "attributeEditor", "levelToolkit", kTexturePanelId, kVisgroupPanelId, kPrefabPanelId}},
                        {"raise", {"levelToolkit", kTexturePanelId}},
                        {"size", json::object()}}},
                      {"menuSet", "Level"},
                      {"viewport", {{"quad", true}, {"camera", "persp"}}}});
}

void visgroupsChanged() { applyVisgroups(graph()); }
} // namespace blockwork
