// fillShelf, scatterProps, pileProps and stackProps: arrangements from one call. Live (default), each builds a
// geometry-nodes group anyone can open (the arrangement node, Object Info per item, then Variant Pool and Settle
// Instances), so changing a seed or a gap re-arranges it. realize=True instead places scene instances of the items where
// the same arrangement puts them, ready for the Push tool, dressCheck and hand edits. Both paths run the same code, so
// they agree.

#include "Arrange.h"
#include "Library.h"
#include "Stagehand.h"

#include <lila/Command.h>
#include <lila/Curves.h>
#include <lila/Extension.h>
#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/Transform.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <map>

using namespace lila;

namespace stagehand
{
using T = ArgType;

namespace
{
std::string uniqueGroupName(const std::string& base)
{
    const json existing = ops::execute("gnGroups", json::object());
    auto taken = [&](const std::string& n) {
        for (const json& e : existing)
            if ((e.is_string() && e.get<std::string>() == n) || (e.is_object() && e.value("name", std::string()) == n))
                return true;
        return false;
    };
    std::string name = base;
    for (int k = 2; taken(name); ++k)
        name = base + std::to_string(k);
    return name;
}

/** One kind of thing an arrangement places: its mesh (the shape placed) and what a realized copy duplicates. */
struct ItemSource
{
    NodeId mesh = scene::kNoNode;
    NodeId root = scene::kNoNode;
    std::string name;
};

std::vector<ItemSource> itemsArg(Graph& g, const Args& a, const char* cmd)
{
    if (!a.has("items"))
        throw CommandError(std::string(cmd) + ": give items=[...] (library asset ids or meshes)");
    std::vector<ItemSource> out;
    const json& list = a.flags["items"];
    for (const json& j : list.is_array() ? list : json::array({list}))
    {
        if (!j.is_string())
            throw CommandError(std::string(cmd) + ": items are names");
        const std::string s = j.get<std::string>();
        ItemSource it;
        it.name = s;
        if (Node* n = g.find(s))
            it.root = n->id();
        else if (library::find(s))
            it.root = libraryMaster(g, s);
        else
            throw CommandError(std::string(cmd) + ": '" + s + "' is neither a scene object nor a library asset");
        const std::vector<NodeId> meshes = meshesOf(g, it.root);
        if (meshes.empty())
            throw CommandError(std::string(cmd) + ": '" + s + "' has no mesh");
        it.mesh = meshes.front();
        out.push_back(it);
    }
    return out;
}

/** A mesh node's output in its own space, or in the world. */
geo::Mesh meshOf(Graph& g, NodeId id, bool world)
{
    Node* n = g.node(id);
    const auto m = n->as<scene::MeshPtr>(n->type().attrIndex("outMesh"));
    if (!m)
        return {};
    geo::Mesh out = *m;
    if (world)
    {
        const glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        for (geo::Index p = 0; p < out.pointCapacity(); ++p)
            if (out.pointValid(p))
                out.setPosition(p, glm::dvec3(w * glm::dvec4(out.position(p), 1.0)));
    }
    return out;
}

NodeId meshArg(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": '" + name + "' is not a mesh");
    return n->id();
}

std::vector<NodeId> meshListArg(Graph& g, const Args& a, const char* flag, const char* cmd)
{
    std::vector<NodeId> out;
    if (!a.has(flag))
        return out;
    const json& j = a.flags[flag];
    for (const json& n : j.is_array() ? j : json::array({j}))
        out.push_back(meshArg(g, n.get<std::string>(), cmd));
    return out;
}

/** The arrangement worked out here, for the stats and for realize. */
struct Result
{
    std::vector<arrange::Placed> placed;
    std::optional<arrange::Settled> settled;
};

Result settleIf(std::vector<arrange::Placed> placed, Graph& g, const std::vector<ItemSource>& items, const std::vector<NodeId>& colliders,
                const arrange::SettleOptions& so, bool doSettle)
{
    Result r;
    r.placed = std::move(placed);
    if (!doSettle || r.placed.empty())
        return r;
    std::vector<geo::Mesh> itemMeshes, colliderMeshes;
    for (const ItemSource& it : items)
        itemMeshes.push_back(meshOf(g, it.mesh, false));
    for (NodeId c : colliders)
        colliderMeshes.push_back(meshOf(g, c, true));
    std::vector<const geo::Mesh*> ip, cp;
    for (const geo::Mesh& m : itemMeshes)
        ip.push_back(&m);
    for (const geo::Mesh& m : colliderMeshes)
        cp.push_back(&m);
    std::vector<glm::dmat4> xf;
    std::vector<int> idx;
    for (const arrange::Placed& p : r.placed)
    {
        xf.push_back(p.transform);
        idx.push_back(p.item);
    }
    r.settled = arrange::settle(xf, idx, ip, cp, so);
    return r;
}

glm::dmat4 finalTransform(const Result& r, size_t k) { return r.settled ? r.settled->transforms[k] : r.placed[k].transform; }

json statsOf(const Result& r, const std::vector<ItemSource>& items)
{
    json perItem = json::object();
    for (const ItemSource& it : items)
        perItem[it.name] = 0;
    int poses[3] = {0, 0, 0};
    for (const arrange::Placed& p : r.placed)
    {
        perItem[items[size_t(p.item)].name] = perItem[items[size_t(p.item)].name].get<int64_t>() + 1;
        ++poses[int(p.pose)];
    }
    json out = {{"count", int64_t(r.placed.size())}, {"perItem", perItem}, {"upright", poses[0]}, {"leaning", poses[1]}, {"lying", poses[2]}};
    if (r.settled)
    {
        int64_t resting = 0;
        for (uint8_t s : r.settled->atRest)
            resting += s;
        out["atRest"] = resting;
        out["seconds"] = r.settled->seconds;
    }
    return out;
}

/** Scene copies of the items where the arrangement put them. */
json realizeAll(Graph& g, const Result& r, const std::vector<ItemSource>& items, const std::string& mode, const std::string& groupName)
{
    LILA_ZONE();

    g.evaluate();
    // A copy of root goes where root would be if its item mesh stood at the placement.
    std::vector<glm::dmat4> meshInRoot;
    for (const ItemSource& it : items)
    {
        const glm::dmat4 root = g.node(it.root)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        const glm::dmat4 mesh = g.node(it.mesh)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        meshInRoot.push_back(glm::inverse(root) * mesh);
    }
    const json group = ops::execute("group", {{"empty", true}, {"name", groupName}});
    const std::string groupPath = group.is_array() ? group[0].get<std::string>() : group.get<std::string>();
    json out = json::array();
    for (size_t k = 0; k < r.placed.size(); ++k)
    {
        const ItemSource& it = items[size_t(r.placed[k].item)];
        const json made = ops::execute(mode == "copy" ? "duplicate" : "instance", {{"args", {g.node(it.root)->path()}}});
        Node* n = g.find(made.is_array() ? made[0].get<std::string>() : made.get<std::string>());
        if (!n)
            continue;
        // Parenting renames copies that clash under the group, so the node is kept by id.
        ops::execute("parent", {{"args", {n->path(), groupPath}}});
        n = g.node(n->id());
        ops::execute("setAttr", {{"args", {n->path() + ".visibility", true}}});
        setWorld(g, n->id(), finalTransform(r, k) * glm::inverse(meshInRoot[size_t(r.placed[k].item)]));
        out.push_back(n->path());
    }
    return {{"group", groupPath}, {"nodes", out}};
}

/** The parts of a live group the commands share: one Object Info per item into a node's items socket. */
void addItemNodes(json& nodes, json& links, Graph& g, const std::vector<ItemSource>& items, const std::string& into)
{
    for (size_t k = 0; k < items.size(); ++k)
    {
        const std::string info = "Item" + std::to_string(k + 1);
        nodes.push_back({{"name", info}, {"type", "ObjectInfo"}, {"position", json::array({0, 200 + 140 * double(k)})},
                         {"props", {{"object", g.node(items[k].mesh)->path()}, {"transformSpace", "original"}}}});
        links.push_back(info + ".geometry -> " + into + ".items");
    }
}

void addColliderNodes(json& nodes, json& links, Graph& g, const std::vector<NodeId>& colliders, const std::string& into)
{
    for (size_t k = 0; k < colliders.size(); ++k)
    {
        const std::string info = "Collider" + std::to_string(k + 1);
        nodes.push_back({{"name", info}, {"type", "ObjectInfo"}, {"position", json::array({600, -200 - 140 * double(k)})},
                         {"props", {{"object", g.node(colliders[k])->path()}, {"transformSpace", "world"}}}});
        links.push_back(info + ".geometry -> " + into + ".colliders");
    }
}

/**
 * Ends a live group: [Variant Pool] -> [Settle Instances] -> output, from `from` (a node.socket of instances), and builds
 * and applies it to a new host mesh holding hostMesh. Returns host, modifier and group.
 */
json finishLive(Graph& g, const Args& a, json nodes, json links, json inputs, const std::string& from, const std::vector<NodeId>& colliders,
                const arrange::SettleOptions& so, bool doSettle, const std::string& hostName, geo::Mesh hostMesh, const std::string& what)
{
    std::string end = from;
    const std::string variants = a.get<std::string>("variants", "pool");
    if (variants == "pool" || variants == "unique")
    {
        nodes.push_back({{"name", "Variants"}, {"type", "VariantPool"}, {"position", json::array({700, 0})}, {"props", {{"mode", variants}}}});
        links.push_back(end + " -> Variants.instances");
        end = "Variants.instances";
    }
    else if (variants != "none")
        throw CommandError("variants is pool, unique or none");
    if (doSettle)
    {
        nodes.push_back({{"name", "Settle"},
                         {"type", "SettleInstances"},
                         {"position", json::array({950, 0})},
                         {"values",
                          {{"seconds", so.seconds}, {"ground", so.ground}, {"groundHeight", so.groundHeight}, {"density", so.density},
                           {"friction", so.friction}}}});
        links.push_back(end + " -> Settle.instances");
        addColliderNodes(nodes, links, g, colliders, "Settle");
        end = "Settle.instances";
    }
    links.push_back(end + " -> GroupOutput.geometry");

    const json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", hostName}});
    const std::string host = made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
    g.setValue({g.find(host)->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(hostMesh))));
    const std::string group = uniqueGroupName(identifier(host, "Set") + "_Arrange");
    const json spec = {{"name", group}, {"description", what}, {"inputs", inputs}, {"nodes", nodes}, {"links", links}};
    ops::execute("gnBuildGroup", {{"args", {spec}}, {"replace", false}});
    const json mod = ops::execute("gnApply", {{"args", {host, group}}});
    return {{"host", host}, {"modifier", mod}, {"group", group}};
}

json input(const char* name, const char* type, json def, const char* doc, std::optional<double> min = std::nullopt, std::optional<double> softMax = std::nullopt)
{
    json in = {{"name", name}, {"type", type}, {"default", std::move(def)}, {"description", doc}};
    if (min)
        in["min"] = *min;
    if (softMax)
        in["softMax"] = *softMax;
    return in;
}

arrange::SettleOptions settleOptions(const Args& a)
{
    arrange::SettleOptions so;
    so.seconds = a.get<double>("seconds", 10.0);
    so.density = a.get<double>("density", 600.0);
    so.friction = a.get<double>("friction", 0.6);
    return so;
}

/** The flags every arrangement command takes; the items' density is massDensity where density means something else. */
std::vector<FlagDef> commonFlags(bool settle, const char* densityFlag = "density")
{
    return {
        {"items", "it", T::List, nullptr, "What to place: library asset ids and/or scene meshes; each is picked at random."},
        {"realize", "rz", T::Bool, false, "Place scene instances of the items instead of a live geometry-nodes arrangement."},
        {"mode", "m", T::String, "instance", "With realize: instance (shared meshes) or copy.", {"instance", "copy"}},
        {"variants", "va", T::String, "pool", "Live only: pool (a few bowed variants, shared), unique (one each) or none.", {"pool", "unique", "none"}},
        {"settle", "st", T::Bool, settle, "Let them come to rest with physics."},
        {"seconds", "", T::Float, 10.0, "Longest the settle runs; it stops sooner once everything rests.", {}, {}, 0.0},
        {densityFlag, "", T::Float, 600.0, "kg/m3 of the items, for the settle.", {}, {}, 1.0},
        {"friction", "", T::Float, 0.6, "Friction of the items, for the settle.", {}, {}, 0.0},
        {"seed", "s", T::Int, int64_t(1), "Another seed gives another arrangement."},
        {"name", "n", T::String, nullptr, "Name of the mesh (live) or group (realize) made."},
    };
}

json finishRealized(Graph& g, const Args& a, const Result& r, const std::vector<ItemSource>& items, const std::string& name)
{
    json out = statsOf(r, items);
    const json made = realizeAll(g, r, items, a.get<std::string>("mode", "instance"), name);
    out["group"] = made["group"];
    out["nodes"] = made["nodes"];
    return out;
}

/** World-space line along the top of a board: its longer horizontal side, through its middle (or depth cm toward its front). */
std::vector<glm::dvec3> shelfLine(Graph& g, NodeId board, double depth, double inset)
{
    const geo::Mesh m = meshOf(g, board, false);
    const arrange::Item b = arrange::itemOf(m);
    const glm::dmat4 w = g.node(board)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    const int axis = b.hi.x - b.lo.x >= b.hi.z - b.lo.z ? 0 : 2;
    const int across = axis == 0 ? 2 : 0;
    glm::dvec3 a((b.lo.x + b.hi.x) * 0.5, b.hi.y, (b.lo.z + b.hi.z) * 0.5), e = a;
    a[axis] = b.lo[axis] + inset;
    e[axis] = b.hi[axis] - inset;
    a[across] += depth;
    e[across] += depth;
    return {glm::dvec3(w * glm::dvec4(a, 1.0)), glm::dvec3(w * glm::dvec4(e, 1.0))};
}
} // namespace

void registerArrangeCommands(ext::Context& ctx)
{
    std::vector<FlagDef> shelfFlags = commonFlags(true);
    const std::vector<FlagDef> shelfFlagsExtra = {
             {"gap", "gp", T::Float, 0.3, "Space between items, cm.", {}, {}, 0.0},
             {"leanChance", "lc", T::Float, 0.08, "0 to 1: chance an item leans on its neighbour.", {}, {}, 0.0, 1.0},
             {"lyingChance", "yc", T::Float, 0.04, "0 to 1: chance a small stack lies flat.", {}, {}, 0.0, 1.0},
             {"fill", "fl", T::Float, 1.0, "0 to 1: how much of each board is filled, from its start.", {}, {}, 0.0, 1.0},
             {"inset", "in", T::Float, 1.0, "Kept clear at both ends of each board, cm.", {}, {}, 0.0},
             {"depth", "dp", T::Float, 0.0, "Line offset from the board's middle across its depth, cm."},
             {"scaleMin", "smn", T::Float, 0.9, "Smallest scale.", {}, {}, 0.0},
             {"scaleMax", "smx", T::Float, 1.1, "Largest scale.", {}, {}, 0.0},
             {"sag", "sg", T::Bool, false, "Bend each board under what rests on it (a Sag Under Load modifier on the board)."},
             {"sagAmount", "sa", T::Float, 0.3, "Sag at a board's middle for 10 kg there, cm.", {}, {}, 0.0}};
    shelfFlags.insert(shelfFlags.end(), shelfFlagsExtra.begin(), shelfFlagsExtra.end());
    ctx.registerCommand({
        .name = "fillShelf",
        .doc = "Fill shelves with a row of items (books, jars, boxes): packed side by side along each board's top, some leaning, a "
               "few lying in small stacks, varied in size, settled with physics.\n\n"
               "Each positional is a board (a mesh): the row runs along its longer horizontal side. Live by default: a mesh "
               "holding the row as a geometry-nodes arrangement, whose inputs (seed, gap, leanChance, ...) re-arrange it. "
               "realize=True places scene instances instead. sag=True bends each board under its load. Returns count, perItem, "
               "upright/leaning/lying, atRest, then host/modifier/group (live) or group/nodes (realize).",
        .positionalDoc = "boards (default the selection)",
        .flags = shelfFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            std::vector<NodeId> boards;
            for (const std::string& s : a.objects())
                boards.push_back(meshArg(g, s, "fillShelf"));
            if (boards.empty())
                throw CommandError("fillShelf: give the shelf boards");
            const std::vector<ItemSource> items = itemsArg(g, a, "fillShelf");
            g.evaluate();

            arrange::RowOptions o;
            o.gap = a.get<double>("gap", 0.3);
            o.leanChance = a.get<double>("leanChance", 0.08);
            o.lyingChance = a.get<double>("lyingChance", 0.04);
            o.fill = a.get<double>("fill", 1.0);
            o.scaleMin = a.get<double>("scaleMin", 0.9);
            o.scaleMax = a.get<double>("scaleMax", 1.1);
            o.seed = uint32_t(a.get<int64_t>("seed", 1));
            const double inset = a.get<double>("inset", 1.0), depth = a.get<double>("depth", 0.0);
            const bool doSettle = a.get<bool>("settle", true);
            arrange::SettleOptions so = settleOptions(a);

            std::vector<arrange::Item> bounds;
            for (const ItemSource& it : items)
                bounds.push_back(arrange::itemOf(meshOf(g, it.mesh, false)));
            std::vector<arrange::Placed> placed;
            geo::Mesh lines;
            arrange::RowOptions each = o;
            for (NodeId b : boards)
            {
                const std::vector<glm::dvec3> line = shelfLine(g, b, depth, inset);
                std::vector<arrange::Placed> part = arrange::row(line, bounds, each);
                placed.insert(placed.end(), part.begin(), part.end());
                lines.addEdge(lines.addPoint(line[0]), lines.addPoint(line[1]));
                ++each.seed;
            }
            const Result r = settleIf(std::move(placed), g, items, boards, so, doSettle);
            const std::string name = a.get<std::string>("name", identifier(g.node(boards[0])->name(), "Set") + "_Items");

            json out;
            if (a.get<bool>("realize", false))
                out = finishRealized(g, a, r, items, name);
            else
            {
                json nodes = json::array({{{"name", "Row"}, {"type", "RowAlongLine"}, {"position", json::array({300, 0})}}});
                json links = json::array({"GroupInput.geometry -> Row.line"});
                json inputs = json::array();
                const std::pair<const char*, double> exposed[] = {{"gap", o.gap}, {"leanChance", o.leanChance}, {"lyingChance", o.lyingChance},
                                                                   {"fill", o.fill}, {"scaleMin", o.scaleMin}, {"scaleMax", o.scaleMax}};
                for (const auto& [n, v] : exposed)
                {
                    inputs.push_back(input(n, "float", v, "See Row Along Line.", 0.0));
                    links.push_back(std::string("GroupInput.") + n + " -> Row." + n);
                }
                inputs.push_back(input("seed", "int", int64_t(o.seed), "Another seed gives another row."));
                links.push_back("GroupInput.seed -> Row.seed");
                addItemNodes(nodes, links, g, items, "Row");
                out = statsOf(r, items);
                // The row's own lines already account for inset and depth, so the node's stay at 0.
                const json live = finishLive(g, a, nodes, links, inputs, "Row.instances", boards, so, doSettle, name, std::move(lines),
                                             "Rows of items along the shelf boards, settled.");
                out.update(live);
            }

            if (a.get<bool>("sag", false))
            {
                // Loads come from the arrangement: the live host's instances, or a point per realized item weighing its mass.
                std::string loads;
                if (out.contains("host"))
                    loads = out["host"].get<std::string>();
                else
                {
                    geo::Mesh pts;
                    for (size_t k = 0; k < r.placed.size(); ++k)
                        pts.addPoint(glm::dvec3(finalTransform(r, k)[3]));
                    auto load = pts.pointAttrs().getOrAdd<float>("load");
                    auto resting = pts.pointAttrs().getOrAdd<float>("resting");
                    for (size_t k = 0; k < r.placed.size(); ++k)
                    {
                        load[k] = r.settled ? float(r.settled->masses[k]) : 1.0f;
                        resting[k] = 1.0f;
                    }
                    const json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", name + "_Loads"}});
                    loads = made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
                    g.setValue({g.find(loads)->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(pts))));
                    ops::execute("setAttr", {{"args", {loads + ".visibility", false}}});
                }
                json sags = json::array();
                for (NodeId b : boards)
                {
                    const glm::dmat4 toLocal = glm::inverse(g.node(b)->as<glm::dmat4>(scene::Transform::kWorldMatrix));
                    const std::string group = uniqueGroupName(identifier(g.node(b)->name(), "Set") + "_Sag");
                    const json spec = {
                        {"name", group},
                        {"description", "Bends the board under what rests on it."},
                        {"inputs", json::array({input("amount", "float", a.get<double>("sagAmount", 0.3), "Sag at the middle for 10 kg, cm.", 0.0, 3.0)})},
                        {"nodes", json::array({{{"name", "Loads"}, {"type", "ObjectInfo"}, {"position", json::array({0, 200})},
                                                {"props", {{"object", loads}, {"transformSpace", "world"}}}},
                                               {{"name", "Sag"}, {"type", "SagUnderLoad"}, {"position", json::array({300, 0})},
                                                {"values",
                                                 {{"toLocalX", v3(glm::dvec3(toLocal[0]))},
                                                  {"toLocalY", v3(glm::dvec3(toLocal[1]))},
                                                  {"toLocalZ", v3(glm::dvec3(toLocal[2]))},
                                                  {"toLocalOrigin", v3(glm::dvec3(toLocal[3]))}}}}})},
                        {"links", json::array({"GroupInput.geometry -> Sag.geometry", "Loads.geometry -> Sag.contacts", "GroupInput.amount -> Sag.amount",
                                               "Sag.geometry -> GroupOutput.geometry"})}};
                    try
                    {
                        ops::execute("gnBuildGroup", {{"args", {spec}}, {"replace", false}});
                        sags.push_back(ops::execute("gnApply", {{"args", {g.node(b)->path(), group}}}));
                    }
                    catch (const std::exception& e)
                    {
                        sags.push_back({{"board", g.node(b)->path()}, {"skipped", e.what()}});
                    }
                }
                out["sag"] = sags;
            }
            return out;
        },
        .examples = {"fillShelf(['Shelf_1', 'Shelf_2', 'Shelf_3'], items=['Books/Hardback', 'Books/Paperback', 'Books/Tall'], sag=True)",
                     "r = fillShelf('Shelf_1', items=['Books/Hardback'], realize=True, leanChance=0.2)\npush(r['nodes'][0], direction=[1, 0, 0])"},
    });

    std::vector<FlagDef> scatterFlags = commonFlags(false, "massDensity");
    const std::vector<FlagDef> scatterFlagsExtra = {
             {"density", "dn", T::Float, 4.0, "Items per square metre.", {}, {}, 0.0},
             {"maxCount", "mc", T::Int, int64_t(200), "At most this many.", {}, {}, 0.0},
             {"spacing", "spc", T::Float, 1.0, "Footprints kept clear, times each item's own.", {}, {}, 0.0},
             {"slopeMax", "sl", T::Float, 35.0, "Steepest surface used, degrees.", {}, {}, 0.0, 90.0},
             {"align", "al", T::Float, 0.0, "0 upright, 1 along the surface.", {}, {}, 0.0, 1.0},
             {"turn", "tu", T::Float, 180.0, "Random turn about up, degrees either way.", {}, {}, 0.0, 180.0},
             {"scaleMin", "smn", T::Float, 0.8, "Smallest scale.", {}, {}, 0.0},
             {"scaleMax", "smx", T::Float, 1.2, "Largest scale.", {}, {}, 0.0},
             {"mask", "mk", T::String, "", "Point attribute (0 to 1) on the surface that thins the scatter."},
             {"avoid", "av", T::Bool, true, "Keep out of and from under every visible mesh standing on the surface."}};
    scatterFlags.insert(scatterFlags.end(), scatterFlagsExtra.begin(), scatterFlagsExtra.end());
    ctx.registerCommand({
        .name = "scatterProps",
        .doc = "Scatter items over a surface (debris, rocks, litter): no two closer than their footprints, on gentle enough "
               "slopes, thinned by a mask attribute.\n\nLive by default (a geometry-nodes arrangement on a new mesh); realize=True "
               "places scene instances. settle=True drops them with physics. Returns the same as fillShelf.",
        .positionalDoc = "surface mesh",
        .flags = scatterFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("scatterProps: give one surface mesh");
            const NodeId surface = meshArg(g, names[0], "scatterProps");
            const std::vector<ItemSource> items = itemsArg(g, a, "scatterProps");
            g.evaluate();
            arrange::ScatterOptions o;
            o.density = a.get<double>("density", 4.0);
            o.maxCount = int(a.get<int64_t>("maxCount", 200));
            o.spacing = a.get<double>("spacing", 1.0);
            o.slopeMax = a.get<double>("slopeMax", 35.0);
            o.align = a.get<double>("align", 0.0);
            o.turn = a.get<double>("turn", 180.0);
            o.scaleMin = a.get<double>("scaleMin", 0.8);
            o.scaleMax = a.get<double>("scaleMax", 1.2);
            o.mask = a.get<std::string>("mask", "");
            o.seed = uint32_t(a.get<int64_t>("seed", 1));
            std::vector<arrange::Item> bounds;
            for (const ItemSource& it : items)
                bounds.push_back(arrange::itemOf(meshOf(g, it.mesh, false)));
            // Everything visible standing on the surface is in the way, unless avoid=False.
            std::vector<NodeId> obstacles;
            if (a.get<bool>("avoid", true))
            {
                const geo::Mesh surfaceMesh = meshOf(g, surface, true);
                const arrange::Item sb = arrange::itemOf(surfaceMesh);
                for (NodeId id : g.nodes())
                {
                    if (id == surface || !physical(g, id))
                        continue;
                    const arrange::Item b = arrange::itemOf(meshOf(g, id, true));
                    if (b.hi.x < sb.lo.x || b.lo.x > sb.hi.x || b.hi.z < sb.lo.z || b.lo.z > sb.hi.z || b.hi.y < sb.lo.y)
                        continue;
                    obstacles.push_back(id);
                    o.avoid.emplace_back(b.lo, b.hi);
                }
            }
            const bool doSettle = a.get<bool>("settle", false);
            arrange::SettleOptions so = settleOptions(a);
            so.density = a.get<double>("massDensity", 600.0);
            const Result r = settleIf(arrange::scatter(meshOf(g, surface, true), bounds, o), g, items, {surface}, so, doSettle);
            const std::string name = a.get<std::string>("name", identifier(g.node(surface)->name(), "Set") + "_Scatter");
            if (a.get<bool>("realize", false))
                return finishRealized(g, a, r, items, name);

            json nodes = json::array({{{"name", "Surface"}, {"type", "ObjectInfo"}, {"position", json::array({0, 0})},
                                       {"props", {{"object", g.node(surface)->path()}, {"transformSpace", "world"}}}},
                                      {{"name", "Scatter"}, {"type", "ScatterProps"}, {"position", json::array({300, 0})},
                                       {"values", {{"maxCount", o.maxCount}, {"slopeMax", o.slopeMax}, {"align", o.align}, {"turn", o.turn},
                                                   {"scaleMin", o.scaleMin}, {"scaleMax", o.scaleMax}, {"mask", o.mask}}}}});
            json links = json::array({"Surface.geometry -> Scatter.surface", "GroupInput.density -> Scatter.density", "GroupInput.spacing -> Scatter.spacing",
                                      "GroupInput.seed -> Scatter.seed"});
            json inputs = json::array({input("density", "float", o.density, "Items per square metre.", 0.0, 50.0),
                                       input("spacing", "float", o.spacing, "Footprints kept clear.", 0.0, 4.0),
                                       input("seed", "int", int64_t(o.seed), "Another seed gives another scatter.")});
            addItemNodes(nodes, links, g, items, "Scatter");
            for (size_t k = 0; k < obstacles.size(); ++k)
            {
                const std::string info = "Avoid" + std::to_string(k + 1);
                nodes.push_back({{"name", info}, {"type", "ObjectInfo"}, {"position", json::array({0, -200 - 120 * double(k)})},
                                 {"props", {{"object", g.node(obstacles[k])->path()}, {"transformSpace", "world"}}}});
                links.push_back(info + ".geometry -> Scatter.avoid");
            }
            json out = statsOf(r, items);
            out.update(finishLive(g, a, nodes, links, inputs, "Scatter.instances", {surface}, so, doSettle, name, geo::Mesh(),
                                  "Items scattered over " + g.node(surface)->name() + "."));
            return out;
        },
        .examples = {"scatterProps('Floor', items=['Debris/Brick', 'Debris/Chunk'], density=12, settle=True)"},
    });

    std::vector<FlagDef> pileFlags = commonFlags(true);
    const std::vector<FlagDef> pileFlagsExtra = {{"at", "", T::List, json::array({0.0, 0.0, 0.0}), "Centre of the pile's footprint, at its floor."},
                                                    {"count", "c", T::Int, int64_t(30), "How many.", {}, {}, 0.0},
                                                    {"radius", "r", T::Float, 40.0, "Footprint radius, cm.", {}, {}, 0.0},
                                                    nodeFlag({"onto", "on", T::List, nullptr, "Meshes it lands on (a crate, the floor); default a floor plane at at's height."}, "mesh")};
    pileFlags.insert(pileFlags.end(), pileFlagsExtra.begin(), pileFlagsExtra.end());
    ctx.registerCommand({
        .name = "pileProps",
        .doc = "Heap items up: dropped tumbling into a footprint (a crate of junk, a rubble heap, fruit in a bowl) and settled "
               "with physics. Returns the same as fillShelf.",
        .flags = pileFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const std::vector<ItemSource> items = itemsArg(g, a, "pileProps");
            g.evaluate();
            const json at = a.flags["at"];
            const glm::dvec3 center(at[0].get<double>(), at[1].get<double>(), at[2].get<double>());
            arrange::PileOptions o;
            o.count = int(a.get<int64_t>("count", 30));
            o.radius = a.get<double>("radius", 40.0);
            o.seed = uint32_t(a.get<int64_t>("seed", 1));
            std::vector<arrange::Item> bounds;
            for (const ItemSource& it : items)
                bounds.push_back(arrange::itemOf(meshOf(g, it.mesh, false)));
            const std::vector<NodeId> onto = meshListArg(g, a, "onto", "pileProps");
            arrange::SettleOptions so = settleOptions(a);
            so.ground = onto.empty();
            so.groundHeight = center.y;
            const bool doSettle = a.get<bool>("settle", true);
            const Result r = settleIf(arrange::pile(center, bounds, o), g, items, onto, so, doSettle);
            const std::string name = a.get<std::string>("name", "Pile");
            if (a.get<bool>("realize", false))
                return finishRealized(g, a, r, items, name);
            json nodes = json::array({{{"name", "Pile"}, {"type", "PileItems"}, {"position", json::array({300, 0})},
                                       {"values", {{"center", v3(center)}, {"radius", o.radius}}}}});
            json links = json::array({"GroupInput.count -> Pile.count", "GroupInput.seed -> Pile.seed"});
            json inputs = json::array({input("count", "int", int64_t(o.count), "How many.", 0.0, 500.0),
                                       input("seed", "int", int64_t(o.seed), "Another seed gives another heap.")});
            addItemNodes(nodes, links, g, items, "Pile");
            json out = statsOf(r, items);
            out.update(finishLive(g, a, nodes, links, inputs, "Pile.instances", onto, so, doSettle, name, geo::Mesh(), "A heap of items."));
            return out;
        },
        .examples = {"pileProps(items=['Debris/Brick', 'Debris/Chunk'], at=[120, 0, 40], count=60, radius=60)",
                     "pileProps(items=['Food/Apple'], at=[0, 82, 0], onto=['Bowl'], count=8, radius=12)"},
    });

    std::vector<FlagDef> stackFlags = commonFlags(false);
    const std::vector<FlagDef> stackFlagsExtra = {{"at", "", T::List, json::array({0.0, 0.0, 0.0}), "Where the stack stands."},
                                                    {"count", "c", T::Int, int64_t(6), "How many.", {}, {}, 0.0},
                                                    {"offset", "of", T::Float, 1.0, "Sideways wander per level, cm.", {}, {}, 0.0},
                                                    {"turn", "tu", T::Float, 6.0, "Turn per level, degrees either way.", {}, {}, 0.0, 180.0},
                                                    nodeFlag({"onto", "on", T::List, nullptr, "Meshes it stands on; default a floor plane at at's height."}, "mesh")};
    stackFlags.insert(stackFlags.end(), stackFlagsExtra.begin(), stackFlagsExtra.end());
    ctx.registerCommand({
        .name = "stackProps",
        .doc = "Stack items one on another (plates, crates, books lying flat), each turned and nudged a little. settle=True lets an "
               "unsteady stack slump. Returns the same as fillShelf.",
        .flags = stackFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const std::vector<ItemSource> items = itemsArg(g, a, "stackProps");
            g.evaluate();
            const json at = a.flags["at"];
            const glm::dvec3 base(at[0].get<double>(), at[1].get<double>(), at[2].get<double>());
            arrange::StackOptions o;
            o.count = int(a.get<int64_t>("count", 6));
            o.offset = a.get<double>("offset", 1.0);
            o.turn = a.get<double>("turn", 6.0);
            o.seed = uint32_t(a.get<int64_t>("seed", 1));
            std::vector<arrange::Item> bounds;
            for (const ItemSource& it : items)
                bounds.push_back(arrange::itemOf(meshOf(g, it.mesh, false)));
            const std::vector<NodeId> onto = meshListArg(g, a, "onto", "stackProps");
            arrange::SettleOptions so = settleOptions(a);
            so.ground = onto.empty();
            so.groundHeight = base.y;
            const bool doSettle = a.get<bool>("settle", false);
            const Result r = settleIf(arrange::stack(base, bounds, o), g, items, onto, so, doSettle);
            const std::string name = a.get<std::string>("name", "Stack");
            if (a.get<bool>("realize", false))
                return finishRealized(g, a, r, items, name);
            json nodes = json::array({{{"name", "Stack"}, {"type", "StackItems"}, {"position", json::array({300, 0})},
                                       {"values", {{"base", v3(base)}, {"offset", o.offset}, {"turn", o.turn}}}}});
            json links = json::array({"GroupInput.count -> Stack.count", "GroupInput.seed -> Stack.seed"});
            json inputs = json::array({input("count", "int", int64_t(o.count), "How many.", 0.0, 50.0),
                                       input("seed", "int", int64_t(o.seed), "Another seed gives another stack.")});
            addItemNodes(nodes, links, g, items, "Stack");
            json out = statsOf(r, items);
            out.update(finishLive(g, a, nodes, links, inputs, "Stack.instances", onto, so, doSettle, name, geo::Mesh(), "A stack of items."));
            return out;
        },
        .examples = {"stackProps(items=['Kitchen/Plate'], at=[30, 90, 0], onto=['Counter'], count=8)"},
    });

    std::vector<FlagDef> pathFlags;
    for (FlagDef& f : commonFlags(false))
        if (f.name != "realize" && f.name != "variants")
            pathFlags.push_back(std::move(f));
    const std::vector<FlagDef> pathFlagsExtra = {
        {"spacing", "sp", T::Float, 100.0, "Distance between items along the curve, cm.", {}, {}, 1.0},
        {"spacingJitter", "sj", T::Float, 0.3, "0 to 1: how much each gap varies.", {}, {}, 0.0, 0.95},
        {"offset", "of", T::Float, 0.0, "Distance to the curve's right, cm (negative: its left)."},
        {"spread", "sd", T::Float, 0.0, "Random sideways wander either way, cm.", {}, {}, 0.0},
        {"turn", "tu", T::Float, 15.0, "Random turn about up either way, degrees.", {}, {}, 0.0, 180.0},
        {"along", "al", T::Bool, true, "Items face along the curve; off turns each at random."},
        {"scaleMin", "smn", T::Float, 0.9, "Smallest scale.", {}, {}, 0.0},
        {"scaleMax", "smx", T::Float, 1.1, "Largest scale.", {}, {}, 0.0},
        {"snap", "sn", T::Bool, true, "Stand each item on the surface below the curve (looked for from 50 cm above it)."},
        nodeFlag({"onto", "on", T::List, nullptr, "Meshes they settle on; default a floor plane at the curve's lowest point."}, "mesh")};
    pathFlags.insert(pathFlags.end(), pathFlagsExtra.begin(), pathFlagsExtra.end());
    ctx.registerCommand({
        .name = "pathProps",
        .doc = "Place items along curves: litter along a kerb, cones down a road, posts along a fence, cable drums by a "
               "track. Spaced along each curve, offset or wandering to the side, turned a little, stood on the surface below "
               "and optionally settled. Places scene instances (mode='copy' for copies) in one group. Returns the same as "
               "fillShelf with realize=True.",
        .positionalDoc = "curves (default the selection)",
        .flags = pathFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const std::vector<ItemSource> items = itemsArg(g, a, "pathProps");
            std::vector<std::string> curves = a.objects();
            if (curves.empty())
                throw CommandError("pathProps: give curves");
            g.evaluate();
            arrange::PathOptions o;
            o.spacing = a.get<double>("spacing", 100.0);
            o.spacingJitter = a.get<double>("spacingJitter", 0.3);
            o.offset = a.get<double>("offset", 0.0);
            o.spread = a.get<double>("spread", 0.0);
            o.turn = a.get<double>("turn", 15.0);
            o.along = a.get<bool>("along", true);
            o.scaleMin = a.get<double>("scaleMin", 0.9);
            o.scaleMax = a.get<double>("scaleMax", 1.1);
            std::vector<arrange::Item> bounds;
            for (const ItemSource& it : items)
                bounds.push_back(arrange::itemOf(meshOf(g, it.mesh, false)));
            std::vector<arrange::Placed> placed;
            double lowest = 1e300;
            uint32_t seed = uint32_t(a.get<int64_t>("seed", 1));
            for (const std::string& c : curves)
            {
                json data;
                try
                {
                    data = ops::execute("curveData", {{"args", {c}}, {"worldSpace", true}});
                }
                catch (const std::exception&)
                {
                    throw CommandError("pathProps: " + c + " is not a curve");
                }
                for (const json& sp : data.is_array() ? data : json::array())
                {
                    curve::Polyline pl;
                    try
                    {
                        pl = curve::tessellate(curve::splineFromJson(sp));
                    }
                    catch (const std::exception& e)
                    {
                        throw CommandError("pathProps: " + c + ": " + e.what());
                    }
                    std::vector<glm::dvec3> line = pl.points;
                    if (pl.closed && !line.empty())
                        line.push_back(line.front());
                    for (const glm::dvec3& p : line)
                        lowest = std::min(lowest, p.y);
                    // Each spline gets its own seed so two curves don't repeat the same pattern.
                    o.seed = seed++;
                    for (arrange::Placed& x : arrange::path(line, bounds, o))
                        placed.push_back(x);
                }
            }
            if (placed.empty())
                throw CommandError("pathProps: nothing placed: the curves have no length");
            if (a.get<bool>("snap", true))
                for (arrange::Placed& x : placed)
                {
                    const glm::dvec3 at(x.transform[3]);
                    if (const auto hit = surfaceUnder(g, at, {}, 50.0))
                        x.transform = glm::translate(glm::dmat4(1.0), glm::dvec3(0.0, hit->point.y - at.y, 0.0)) * x.transform;
                }
            const std::vector<NodeId> onto = meshListArg(g, a, "onto", "pathProps");
            arrange::SettleOptions so = settleOptions(a);
            so.ground = onto.empty();
            so.groundHeight = lowest;
            const Result r = settleIf(std::move(placed), g, items, onto, so, a.get<bool>("settle", false));
            return finishRealized(g, a, r, items, a.get<std::string>("name", "PathProps"));
        },
        .examples = {"pathProps('Kerb', items=['Debris/Rebar', 'Debris/RebarBent'], spacing=60, offset=40, spread=20, along=False)",
                     "pathProps('Road_Edge', items=['Road/Delineator'], spacing=500, offset=-150, turn=0)"},
    });

    std::vector<FlagDef> debrisFlags;
    for (FlagDef& f : commonFlags(true))
        if (f.name != "realize" && f.name != "variants" && f.name != "items" && f.name != "density")
            debrisFlags.push_back(std::move(f));
    const std::vector<FlagDef> debrisFlagsExtra = {
        {"items", "it", T::List, nullptr, "Chips: library asset ids and/or scene meshes (default the starter library's stone chunks)."},
        {"amount", "a", T::Float, 6.0, "Chips per piece of median size, on average; bigger pieces shed more.", {}, {}, 0.0, 1000.0},
        {"size", "sz", T::Float, 0.12, "Chip size as a fraction of the piece it comes off.", {}, {}, 0.001, 1.0},
        {"minSize", "mns", T::Float, 2.0, "Smallest chip, cm.", {}, {}, 0.1},
        {"maxSize", "mxs", T::Float, 40.0, "Largest chip, cm.", {}, {}, 0.1},
        {"spread", "sd", T::Float, 1.0, "How far out they land, in piece radii beyond each piece.", {}, {}, 0.0},
        {"density", "", T::Float, 2400.0, "kg/m3 of the chips, for the settle.", {}, {}, 1.0},
        nodeFlag({"onto", "on", T::List, nullptr, "Meshes they land on besides the pieces; default a floor plane at the pieces' lowest point."}, "mesh")};
    debrisFlags.insert(debrisFlags.end(), debrisFlagsExtra.begin(), debrisFlagsExtra.end());
    ctx.registerCommand({
        .name = "debrisProps",
        .doc = "Strew debris around broken things: chips sized from each piece of a fracture, ruin or wall (bigger pieces shed "
               "more and bigger chips), dropped beside it and settled against the pieces and the ground. Places scene "
               "instances (mode='copy' for copies) in one group. Returns the same as fillShelf with realize=True.",
        .positionalDoc = "broken objects: their meshes are the pieces (default the selection)",
        .flags = debrisFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            std::vector<ItemSource> items;
            if (a.has("items"))
                items = itemsArg(g, a, "debrisProps");
            else
            {
                library::Query q;
                q.tags = {"debris", "stone"};
                json ids = json::array();
                for (const library::Asset& x : library::search(q))
                    if (x.id.find("StoneChunk") != std::string::npos)
                        ids.push_back(x.id);
                if (ids.empty())
                    throw CommandError("debrisProps: no stone chunks in the library (libraryStarter builds them); give items=[...]");
                Args withItems = a;
                withItems.flags["items"] = ids;
                items = itemsArg(g, withItems, "debrisProps");
            }
            const std::vector<std::string> sources = a.objects();
            if (sources.empty())
                throw CommandError("debrisProps: give the broken objects");
            g.evaluate();
            std::vector<NodeId> pieceMeshes;
            std::vector<std::pair<glm::dvec3, glm::dvec3>> pieces;
            double lowest = 1e300;
            for (const std::string& name : sources)
            {
                Node* n = g.find(name);
                if (!n)
                    throw CommandError("debrisProps: no node named '" + name + "'");
                for (NodeId m : meshesOf(g, n->id()))
                {
                    const geo::Mesh w = meshOf(g, m, true);
                    glm::dvec3 lo(1e300), hi(-1e300);
                    for (geo::Index p = 0; p < w.pointCapacity(); ++p)
                        if (w.pointValid(p))
                        {
                            lo = glm::min(lo, w.position(p));
                            hi = glm::max(hi, w.position(p));
                        }
                    if (lo.x > hi.x)
                        continue;
                    pieceMeshes.push_back(m);
                    pieces.emplace_back(lo, hi);
                    lowest = std::min(lowest, lo.y);
                }
            }
            if (pieces.empty())
                throw CommandError("debrisProps: the objects have no meshes");
            arrange::DebrisOptions o;
            o.amount = a.get<double>("amount", 6.0);
            o.size = a.get<double>("size", 0.12);
            o.minSize = a.get<double>("minSize", 2.0);
            o.maxSize = std::max(o.minSize, a.get<double>("maxSize", 40.0));
            o.spread = a.get<double>("spread", 1.0);
            o.seed = uint32_t(a.get<int64_t>("seed", 1));
            std::vector<arrange::Item> bounds;
            for (const ItemSource& it : items)
                bounds.push_back(arrange::itemOf(meshOf(g, it.mesh, false)));
            std::vector<NodeId> colliders = meshListArg(g, a, "onto", "debrisProps");
            const bool floor = colliders.empty();
            colliders.insert(colliders.end(), pieceMeshes.begin(), pieceMeshes.end());
            arrange::SettleOptions so = settleOptions(a);
            so.density = a.get<double>("density", 2400.0);
            so.ground = floor;
            so.groundHeight = lowest;
            const Result r = settleIf(arrange::debris(pieces, bounds, o), g, items, colliders, so, a.get<bool>("settle", true));
            if (r.placed.empty())
                throw CommandError("debrisProps: nothing placed (amount is 0?)");
            return finishRealized(g, a, r, items, a.get<std::string>("name", "Debris"));
        },
        .examples = {"debrisProps('Wall_fractured')", "debrisProps('RuinedArch', amount=12, size=0.08, onto=['Terrain'])"},
    });
}
} // namespace stagehand
