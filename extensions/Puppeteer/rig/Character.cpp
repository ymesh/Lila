// The character node: one per animated character, holding its rig elements as children and naming its skeleton root,
// meshes, template and pose library. Takes belong to it through their root.

#include "Puppeteer.h"
#include "rig/Rig.h"

#include <lila/Poses.h>
#include <lila/Relocate.h>

#include <algorithm>
#include <set>
#include <sstream>

using namespace lila;
using namespace lila::scene;

namespace puppeteer
{
namespace
{
std::vector<std::string> splitNames(const std::string& s)
{
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string w; in >> w;)
        out.push_back(w);
    return out;
}

std::string joinNames(const std::vector<std::string>& names)
{
    std::string out;
    for (const std::string& n : names)
        out += (out.empty() ? "" : " ") + n;
    return out;
}

/** Meshes whose skinCluster has an influence at or below root. */
std::vector<std::string> skinnedMeshes(Graph& g, NodeId root)
{
    std::vector<std::string> out;
    const CommandDef* info = ops::find("skinInfo");
    if (!info)
        return out;
    for (NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n || !n->type().isA("mesh"))
            continue;
        json r;
        try
        {
            Args a;
            a.positional = json::array({n->path()});
            r = info->run(a);
        }
        catch (const std::exception&)
        {
            continue;
        }
        for (const json& inf : r.value("influences", json::array()))
            if (Node* j = inf.is_string() ? named(g, inf.get<std::string>()) : nullptr; j && isUnder(g, j->id(), root))
            {
                out.push_back(n->path());
                break;
            }
    }
    return out;
}

NodeId topJoint(Graph& g, NodeId id)
{
    NodeId top = id;
    for (NodeId cur = g.parent(id); cur; cur = g.parent(cur))
        if (Node* n = g.node(cur); n && rig::isJoint(*n))
            top = cur;
    return top;
}

/** Moves the skeleton's rig elements (built before the character, by a template say) under it, keeping their place. */
void adoptElements(Graph& g, NodeId character, NodeId root)
{
    namespace rigel = lila::scene::rigel;
    auto joint = [&](const Node& n) -> Node* {
        for (const char* attr : {"startJoint", "ankle", "joint"})
            if (const int a = attrOf(n, attr); a >= 0)
                return named(g, n.as<std::string>(a));
        return nullptr;
    };
    std::vector<NodeId> moving;
    for (NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n || !n->type().dag || !rigel::isElement(*n) || characterFor(g, id) == character)
            continue;
        if (Node* j = joint(*n); j && isUnder(g, j->id(), root))
            moving.push_back(id);
    }
    for (NodeId id : moving)
        g.setParent(id, character, true);
}

NodeId characterArg(Graph& g, const Args& a, const char* command)
{
    std::vector<std::string> objs = objectArgs(a);
    if (a.has("character"))
        objs = {a.get<std::string>("character")};
    for (const std::string& s : objs)
        if (NodeId c = characterFor(g, nodeNamed(g, s, command).id()))
            return c;
    const std::vector<NodeId> all = characters(g);
    if (objs.empty() && all.size() == 1)
        return all.front();
    throw CommandError(std::string(command) + ": give a character, or a node in one");
}

json characterJson(Graph& g, NodeId c)
{
    const Node& n = *g.node(c);
    json elements = json::array();
    for (NodeId e : g.descendants(c))
        if (Node* en = g.node(e); en && (lila::scene::rigel::isElement(*en) || en->type().name.rfind("puppeteer", 0) == 0))
            elements.push_back({{"name", en->path()}, {"type", en->type().name}});
    json takes = json::array();
    const Node* rootNode = named(g, textOf(n, "root"));
    const std::string root = rootNode ? rootNode->path() : textOf(n, "root");
    if (ops::find("listTakes"))
        for (const json& t : ops::execute("listTakes", json::object()))
            if (const std::string tr = t.value("root", std::string()); tr.empty() || tr == root || (rootNode && named(g, tr) == rootNode))
                takes.push_back(t.value("name", std::string()));
    json poses = json::array();
    if (ops::find("listPoses"))
        try
        {
            poses = ops::execute("listPoses", {{"character", poseLibraryOf(g, c)}});
        }
        catch (const std::exception&)
        {
        }
    json templ = nullptr;
    // Rig Templates is optional: its command is there only while it is loaded.
    if (const std::string id = textOf(n, "templateId"); !id.empty() && ops::find("rigTemplateInfo"))
        try
        {
            json t = ops::execute("rigTemplateInfo", {{"args", {id}}});
            templ = {{"id", id}, {"name", t.value("name", id)}};
        }
        catch (const std::exception&)
        {
            templ = {{"id", id}, {"loaded", false}};
        }
    else if (!id.empty())
        templ = {{"id", id}, {"loaded", false}};
    json meshes = json::array();
    for (const std::string& m : splitNames(textOf(n, "meshes")))
        meshes.push_back(named(g, m) ? named(g, m)->path() : m);
    return {{"name", n.path()},         {"root", root},           {"meshes", meshes},
            {"template", templ},        {"poseLibrary", poseLibraryOf(g, c)}, {"elements", elements},
            {"takes", takes},           {"poses", poses}};
}

bool registerTypes(ext::Context& ctx)
{
    const NodeType* transform = types::find("transform");
    if (!transform)
        return false;
    NodeType t{.name = kCharacterType,
               .base = transform,
               .dag = true,
               .doc = "A Puppeteer character: names its skeleton root, meshes, rig template and pose library, and holds its "
                      "rig elements (IK chains, reverse feet, lookers, spaces) as children.",
               .icon = "user",
               .create = transform->create};
    t.ownAttrs = {
        text("root", "The skeleton's root joint."),
        text("meshes", "The character's meshes, space separated."),
        text("templateId", "The rig template it was built from, when Rig Templates is loaded; empty for none."),
        text("poseLibrary", "Pose library group. Empty: the template id, else the root joint's name."),
        text("pins", "Joints full-body moves hold where they are, space separated."),
        text("strings", "Strings rules set on this character (JSON: contacts, chains); what it leaves out comes from its template."),
    };
    lila::scene::relocate::registerNeutral(t.name);
    return types::registerType(ctx, std::move(t)) != nullptr;
}

void registerCharacterCommands(ext::Context& ctx)
{
    using T = ArgType;
    ctx.registerCommand({
        .name = "puppeteerCharacter",
        .doc = "Make a Puppeteer character for a skeleton: it finds the meshes skinned to it and the rig template it came "
               "from, and groups its rig elements, takes and poses. Returns its name.",
        .positionalDoc = "root joint (default the selection's top joint)",
        .flags = {
            {"name", "n", T::String, nullptr, "Character name; defaults to the root's name plus _character."},
            nodeFlag({"meshes", "m", T::List, nullptr, "Meshes, instead of those skinned to the skeleton."}, "mesh"),
            {"template", "tp", T::String, nullptr, "Rig template id, instead of the one the skeleton came from."},
            {"poseLibrary", "pl", T::String, nullptr, "Pose library group instead of the template or root name."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const std::vector<std::string> objs = objectArgs(a);
            if (objs.empty())
                throw CommandError("puppeteerCharacter: give the skeleton's root joint");
            const NodeId root = topJoint(g, jointNamed(g, objs.front(), "puppeteerCharacter").id());
            for (NodeId c : characters(g))
                if (named(g, textOf(*g.node(c), "root")) == g.node(root))
                    throw CommandError("puppeteerCharacter: " + g.path(root) + " already has " + g.path(c));
            const std::string rootName = g.node(root)->name();
            std::vector<std::string> meshes;
            if (a.has("meshes"))
                for (const json& m : a.flags["meshes"])
                    meshes.push_back(nodeNamed(g, m.get<std::string>(), "puppeteerCharacter").path());
            else
                meshes = skinnedMeshes(g, root);
            std::string templ = a.get<std::string>("template", "");
            if (!a.has("template"))
                templ = poses::characterOf(g, root).templateId;
            Node* c = g.createNode(*types::find(kCharacterType), a.get<std::string>("name", rootName + "_character"));
            const NodeId id = c->id();
            g.setValue(plugOf(*c, "root"), g.path(root));
            g.setValue(plugOf(*c, "meshes"), joinNames(meshes));
            g.setValue(plugOf(*c, "templateId"), templ);
            g.setValue(plugOf(*c, "poseLibrary"), a.get<std::string>("poseLibrary", ""));
            adoptElements(g, id, root);
            setupHands(g, id);
            g.select({id});
            return g.node(id)->path();
        },
    });

    ctx.registerCommand({
        .name = "puppeteerCharacterInfo",
        .doc = "A character's root, meshes, template, pose library, rig elements, takes and poses.",
        .positionalDoc = "character or a node in it",
        .flags = {{"character", "ch", T::String, nullptr, "The character."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            return characterJson(g, characterArg(g, a, "puppeteerCharacterInfo"));
        },
    });

    ctx.registerCommand({
        .name = "puppeteerListCharacters",
        .doc = "Every Puppeteer character with its root and pose library.",
        .undoable = false,
        .run = [](const Args&) -> json {
            Graph& g = Graph::current();
            json out = json::array();
            for (NodeId c : characters(g))
            {
                const Node* root = named(g, textOf(*g.node(c), "root"));
                out.push_back({{"name", g.node(c)->path()}, {"root", root ? root->path() : textOf(*g.node(c), "root")}, {"poseLibrary", poseLibraryOf(g, c)}});
            }
            return out;
        },
    });
}
} // namespace

std::vector<NodeId> characters(Graph& g)
{
    std::vector<NodeId> out;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().isA(kCharacterType))
            out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> skeletonMeshes(Graph& g, NodeId root) { return skinnedMeshes(g, root); }

NodeId characterFor(Graph& g, NodeId node)
{
    if (!node)
        return kNoNode;
    for (NodeId cur = node; cur; cur = g.parent(cur))
        if (Node* n = g.node(cur); n && n->type().isA(kCharacterType))
            return cur;
    for (NodeId c : characters(g))
        if (Node* root = named(g, textOf(*g.node(c), "root")); root && isUnder(g, node, root->id()))
            return c;
    return kNoNode;
}

std::string poseLibraryOf(Graph& g, NodeId character)
{
    const Node& n = *g.node(character);
    if (const std::string& lib = textOf(n, "poseLibrary"); !lib.empty())
        return lib;
    if (const std::string& t = textOf(n, "templateId"); !t.empty())
        return t;
    if (const Node* root = named(g, textOf(n, "root")))
        return poses::stripNamespace(root->name());
    const std::string& root = textOf(n, "root");
    return poses::stripNamespace(root.substr(root.rfind('|') + 1));
}

NodeId elementParentFor(Graph& g, NodeId joint) { return characterFor(g, joint); }

std::vector<NodeId> elementsNaming(Graph& g, const char* type, const char* attr, const std::string& joint)
{
    std::vector<NodeId> out;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().isA(type))
            if (const int a = attrOf(*n, attr); a >= 0 && n->as<std::string>(a) == joint)
                out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
}

bool registerCharacter(ext::Context& ctx)
{
    if (!registerTypes(ctx))
        return false;
    registerCharacterCommands(ctx);
    return true;
}
} // namespace puppeteer
