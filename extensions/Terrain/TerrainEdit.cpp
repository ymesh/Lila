#include "TerrainEdit.h"

#include <lila/Deformer.h>
#include <lila/Math.h>
#include <lila/Node.h>
#include <lila/Transform.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

using namespace lila;

namespace terrain::edit
{
using scene::Graph;
using scene::Node;

bool zUp() { return math::upAxis() == math::UpAxis::Z; }
glm::dvec3 upVector() { return zUp() ? glm::dvec3(0.0, 0.0, 1.0) : glm::dvec3(0.0, 1.0, 0.0); }
double upOf(const glm::dvec3& w) { return zUp() ? w.z : w.y; }
glm::dvec3 fromGround(glm::dvec2 g, double up) { return zUp() ? glm::dvec3(g.x, g.y, up) : glm::dvec3(g.x, up, g.y); }

glm::dvec3 worldPoint(const json& p, const std::string& cmd)
{
    if (!p.is_array() || (p.size() != 2 && p.size() != 3))
        throw CommandError(cmd + ": points are [x, " + std::string(zUp() ? "y" : "z") + "] or [x, y, z]");
    return p.size() == 2 ? fromGround({p[0].get<double>(), p[1].get<double>()}) : glm::dvec3(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
}

json vec3(const glm::dvec3& p) { return json::array({p.x, p.y, p.z}); }

double TerrainTarget::localHeight(double up, glm::dvec2 xz) const
{
    const double k = glm::dot(glm::dvec3(m[1]), upVector());
    const double base = upOf(world(xz.x, 0.0, xz.y));
    return std::abs(k) > 1e-12 ? (up - base) / k : up;
}

namespace
{
struct BatchState
{
    int depth = 0;
    std::map<scene::NodeId, gn::Group> groups;
    // Lists edited in place, by group node, node name and property.
    std::map<std::tuple<scene::NodeId, std::string, std::string>, json> lists;
};

json parseList(const gn::GNode& n, const char* prop)
{
    auto it = n.values.find(prop);
    if (it == n.values.end() || !it->is_string() || it->get<std::string>().empty())
        return json::array();
    json j = json::parse(it->get<std::string>(), nullptr, false);
    return j.is_array() ? j : json::array();
}

BatchState& batchState()
{
    thread_local BatchState s;
    return s;
}
} // namespace

EditBatch::EditBatch() { ++batchState().depth; }

EditBatch::~EditBatch()
{
    try
    {
        commit("terrainBatch");
    }
    catch (...)
    {
    }
    --batchState().depth;
}

bool EditBatch::active() { return batchState().depth > 0; }

void EditBatch::commit(const std::string& cmd)
{
    Graph& g = Graph::current();
    std::map<scene::NodeId, gn::Group> groups = std::move(batchState().groups);
    auto lists = std::move(batchState().lists);
    batchState().groups.clear();
    batchState().lists.clear();
    for (auto& [key, list] : lists)
    {
        const auto& [id, nodeName, prop] = key;
        Node* node = g.node(id);
        if (!node)
            continue;
        if (!groups.contains(id))
            groups[id] = gn::readGroup(*node);
        if (gn::GNode* n = nodeNamed(groups[id], nodeName))
            n->values[prop] = list.dump();
    }
    for (auto& [id, group] : groups)
        if (Node* node = g.node(id))
        {
            GroupEdit e{node, std::move(group)};
            const int depth = std::exchange(batchState().depth, 0);
            try
            {
                commitGroup(e, cmd);
            }
            catch (...)
            {
                batchState().depth = depth;
                throw;
            }
            batchState().depth = depth;
        }
}

bool batchedGroup(const Node& groupNode, gn::Group& out)
{
    BatchState& s = batchState();
    if (s.depth == 0)
        return false;
    auto it = s.groups.find(groupNode.id());
    if (it == s.groups.end())
        return false;
    out = it->second;
    return true;
}

bool batchGroup(const Node& groupNode, gn::Group& group)
{
    BatchState& s = batchState();
    if (s.depth == 0)
        return false;
    s.groups[groupNode.id()] = std::move(group);
    return true;
}

ListEdit::ListEdit(const Node& groupNode, const gn::GNode& n, const char* prop) : mProp(prop), mList(&mOwn)
{
    BatchState& s = batchState();
    if (s.depth == 0)
    {
        mOwn = parseList(n, prop);
        return;
    }
    const auto key = std::make_tuple(groupNode.id(), n.name, std::string(prop));
    auto it = s.lists.find(key);
    if (it == s.lists.end())
        it = s.lists.emplace(key, parseList(n, prop)).first;
    mList = &it->second;
}

void ListEdit::store(gn::GNode& n)
{
    if (mList == &mOwn)
        n.values[mProp] = mOwn.dump();
}

TerrainTarget findTerrain(Graph& g, const std::string& name, const std::string& cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(cmd + ": no node named '" + name + "'");
    TerrainTarget t;
    for (bool evaluated = !EditBatch::active(); ; evaluated = true)
    {
        if (evaluated)
            g.evaluate();
        t = {};
        if (gn::isGnModifier(*n))
        {
            t.modifier = n;
            t.local = gn::modifierHeightfield(n->id());
            const scene::NodeId meshId = scene::modifiers::meshOf(g, n->id());
            t.mesh = meshId != scene::kNoNode ? g.node(meshId) : nullptr;
        }
        else
        {
            t.mesh = n;
            const std::vector<scene::NodeId> stack = scene::modifiers::stack(g, n->id());
            for (auto it = stack.rbegin(); it != stack.rend() && !t.local; ++it)
                if (Node* m = g.node(*it); m && gn::isGnModifier(*m))
                    if (auto h = gn::modifierHeightfield(m->id()))
                    {
                        t.modifier = m;
                        t.local = std::move(h);
                    }
        }
        // In a batch the heightfield from the last evaluation does; a terrain never evaluated yet is evaluated now.
        if (evaluated || (t.local && !t.local->empty()))
            break;
    }
    if (!t.local || t.local->empty())
        throw CommandError(cmd + ": " + n->path() + " makes no heightfield (make a terrain with terrainCreate)");
    t.m = t.mesh ? t.mesh->as<glm::dmat4>(scene::Transform::kWorldMatrix) : glm::dmat4(1.0);
    t.inv = glm::inverse(t.m);
    return t;
}

TerrainTarget oneTerrain(const Args& a, const std::string& cmd)
{
    const std::vector<std::string> names = a.objects();
    if (names.size() != 1)
        throw CommandError(cmd + ": give one terrain");
    return findTerrain(Graph::current(), names[0], cmd);
}

GroupEdit readGroup(Graph& g, const TerrainTarget& t, const std::string& cmd)
{
    Node* node = t.modifier ? gn::groupOfModifier(g, *t.modifier) : nullptr;
    if (!node)
        throw CommandError(cmd + ": " + (t.modifier ? t.modifier->path() : std::string("the terrain")) + " has no node group");
    try
    {
        GroupEdit e{node, {}};
        if (!batchedGroup(*node, e.group))
            e.group = gn::readGroup(*node);
        return e;
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": " + e.what());
    }
}

void commitGroup(GroupEdit& e, const std::string& cmd)
{
    if (batchGroup(*e.node, e.group))
        return;
    try
    {
        gn::commitGroup(Graph::current(), *e.node, std::move(e.group));
    }
    catch (const std::exception& ex)
    {
        throw CommandError(cmd + ": " + ex.what());
    }
}

gn::GNode* nodeNamed(gn::Group& grp, const std::string& name)
{
    for (gn::GNode& n : grp.nodes)
        if (n.name == name)
            return &n;
    return nullptr;
}

gn::GLink* linkInto(gn::Group& grp, const std::string& node, const std::string& socket)
{
    for (gn::GLink& l : grp.links)
        if (l.toNode == node && l.toSocket == socket)
            return &l;
    return nullptr;
}

gn::GNode& chainNode(gn::Group& grp, const std::string& name, const std::string& type, const std::set<std::string>& after, double yOffset, const std::string& cmd)
{
    if (gn::GNode* n = nodeNamed(grp, name))
    {
        if (n->type != type)
            throw CommandError(cmd + ": the terrain's group has a " + n->type + " node named " + name + " already");
        return *n;
    }
    std::string to = "GroupOutput", socket = "geometry";
    for (gn::GLink* l = linkInto(grp, to, socket); l; l = linkInto(grp, to, socket))
    {
        const gn::GNode* from = nodeNamed(grp, l->fromNode);
        if (!from || !after.count(from->type))
            break;
        to = from->name;
        socket = "heightfield";
    }
    gn::GLink* l = linkInto(grp, to, socket);
    if (!l)
        throw CommandError(cmd + ": the terrain's group has no heightfield running into " + to);
    const gn::GNode* toNode = nodeNamed(grp, to);
    const gn::GNode* fromNode = nodeNamed(grp, l->fromNode);
    const glm::dvec2 at = toNode && fromNode ? (toNode->position + fromNode->position) * 0.5 + glm::dvec2(0.0, yOffset) : glm::dvec2(0.0);
    const std::string fromName = l->fromNode, fromSocket = l->fromSocket;
    l->fromNode = name;
    l->fromSocket = "heightfield";
    grp.links.push_back({fromName, fromSocket, name, "heightfield"});
    gn::GNode n;
    n.name = name;
    n.type = type;
    n.position = at;
    grp.nodes.push_back(std::move(n));
    return grp.nodes.back();
}

json jsonProp(const gn::GNode& n, const char* prop, json fallback)
{
    auto it = n.values.find(prop);
    if (it == n.values.end() || !it->is_string() || it->get<std::string>().empty())
        return fallback;
    json j = json::parse(it->get<std::string>(), nullptr, false);
    return j.is_discarded() || j.type() != fallback.type() ? fallback : j;
}

std::string sourceNode(gn::Group& grp, const std::string& prefix, const std::string& object)
{
    for (const gn::GNode& n : grp.nodes)
        if (n.type == "ObjectInfo" && n.name.starts_with(prefix) && n.values.value("object", std::string()) == object)
            return n.name;
    gn::GNode n;
    n.name = grp.uniqueNodeName(prefix);
    n.type = "ObjectInfo";
    n.position = {-600.0, -400.0 - 120.0 * double(grp.nodes.size())};
    n.values["object"] = object;
    grp.nodes.push_back(n);
    return n.name;
}

void dropUnusedSources(gn::Group& grp, const std::string& prefix, const std::string& type, std::initializer_list<const char*> props)
{
    std::set<std::string> used;
    for (const gn::GNode& n : grp.nodes)
        if (n.type == type)
            for (const char* prop : props)
                for (const json& e : jsonProp(n, prop, json::array()))
                    if (e.contains("object"))
                        used.insert(e["object"].get<std::string>());
    std::erase_if(grp.nodes, [&](const gn::GNode& n) { return n.type == "ObjectInfo" && n.name.starts_with(prefix) && !used.count(n.name); });
}
} // namespace terrain::edit
