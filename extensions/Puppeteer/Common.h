#pragma once

// Helpers shared by Puppeteer's scene code: attr definitions, lookups by name, and joint frames.

#include "rig/RigMath.h"

#include <lila/Anim.h>
#include <lila/Command.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Node.h>
#include <lila/Rig.h>
#include <lila/Transform.h>

#include <string>
#include <vector>

namespace puppeteer
{
using lila::json;
using lila::scene::AttrDef;
using lila::scene::Graph;
using lila::scene::Node;
using lila::scene::NodeId;
using lila::scene::PlugRef;
using lila::scene::PlugType;

inline AttrDef number(const char* name, double def, const char* doc, uint32_t extra = 0)
{
    using namespace lila::scene;
    return {.name = name, .type = PlugType::Float, .defaultValue = def, .flags = AttrInput | AttrStorable | extra, .doc = doc};
}
inline AttrDef whole(const char* name, int64_t def, const char* doc, uint32_t extra = 0)
{
    using namespace lila::scene;
    return {.name = name, .type = PlugType::Int, .defaultValue = def, .flags = AttrInput | AttrStorable | extra, .doc = doc};
}
inline AttrDef text(const char* name, const char* doc, uint32_t extra = 0)
{
    using namespace lila::scene;
    return {.name = name, .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | extra, .doc = doc};
}
inline AttrDef vector3(const char* name, glm::dvec3 def, const char* doc, uint32_t extra = 0)
{
    using namespace lila::scene;
    AttrDef a{.name = name, .type = PlugType::Vec3, .defaultValue = def, .flags = AttrInput | AttrStorable | extra, .doc = doc};
    const std::string n = name;
    a.childNames = {n + "X", n + "Y", n + "Z"};
    return a;
}
inline AttrDef matrix(const char* name, const char* doc, uint32_t extra = 0)
{
    using namespace lila::scene;
    return {.name = name, .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrStorable | extra, .doc = doc};
}
inline AttrDef outFloat(const char* name, const char* doc)
{
    using namespace lila::scene;
    return {.name = name, .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrOutput | AttrHidden, .doc = doc};
}
inline AttrDef outVec(const char* name, const char* doc)
{
    using namespace lila::scene;
    AttrDef a{.name = name, .type = PlugType::Vec3, .defaultValue = glm::dvec3(0.0), .flags = AttrOutput | AttrHidden, .doc = doc};
    const std::string n = name;
    a.childNames = {n + "X", n + "Y", n + "Z"};
    return a;
}
inline AttrDef outMatrix(const char* name, const char* doc)
{
    using namespace lila::scene;
    return {.name = name, .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrOutput | AttrHidden, .doc = doc};
}

inline Graph& graph() { return Graph::current(); }

/** True when node is ancestor or below it. */
inline bool isUnder(const Graph& g, NodeId node, NodeId ancestor)
{
    for (NodeId cur = node; cur; cur = g.parent(cur))
        if (cur == ancestor)
            return true;
    return false;
}

inline int attrOf(const Node& n, const char* name) { return n.type().attrIndex(name); }
inline PlugRef plugOf(const Node& n, const char* name) { return {n.id(), attrOf(n, name), -1}; }

inline const std::string& textOf(const Node& n, const char* name) { return n.as<std::string>(attrOf(n, name)); }

/** The node a text attr names (a full path, or a name in older scenes); null when it is gone or ambiguous. */
inline Node* named(const Graph& g, const std::string& path)
{
    try
    {
        return path.empty() ? nullptr : g.find(path);
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

/** The node named, or a CommandError naming the command. */
inline Node& nodeNamed(Graph& g, const std::string& name, const char* command)
{
    Node* n = g.find(name);
    if (!n)
        throw lila::CommandError(std::string(command) + ": no node named '" + name + "'");
    return *n;
}

inline Node& jointNamed(Graph& g, const std::string& name, const char* command)
{
    Node& n = nodeNamed(g, name, command);
    if (!lila::scene::rig::isJoint(n))
        throw lila::CommandError(std::string(command) + ": " + name + " is not a joint");
    return n;
}

inline glm::dmat4 worldOf(Graph& g, NodeId id)
{
    return std::get<glm::dmat4>(g.value({id, lila::scene::Transform::kWorldMatrix, -1}));
}
inline glm::dmat4 parentWorldOf(Graph& g, NodeId id)
{
    return std::get<glm::dmat4>(g.value({id, lila::scene::Transform::kParentMatrix, -1}));
}
inline glm::dvec3 vecOf(Graph& g, NodeId id, int attr) { return std::get<glm::dvec3>(g.value({id, attr, -1})); }

/** A joint's jointOrient as a matrix; identity for other transforms. */
inline glm::dmat3 orientOf(Graph& g, NodeId id)
{
    Node* n = g.node(id);
    if (!n || !lila::scene::rig::isJoint(*n))
        return glm::dmat3(1.0);
    return glm::dmat3(lila::scene::eulerToMatrix(vecOf(g, id, lila::scene::Joint::kJointOrient), lila::scene::RotateOrder::XYZ));
}

inline lila::scene::RotateOrder rotateOrderOf(Graph& g, NodeId id)
{
    return lila::scene::RotateOrder(std::get<int64_t>(g.value({id, lila::scene::Transform::kRotateOrder, -1})));
}

/** Rotate values giving the R term r, continuous with the node's current rotate. */
inline glm::dvec3 rotateFor(Graph& g, NodeId id, const glm::dmat3& r)
{
    return lila::scene::eulerNear(r, rotateOrderOf(g, id), vecOf(g, id, lila::scene::Transform::kRotate));
}

/** Rotates the node so its world rotation becomes `world` (scale and translate untouched). Records undo. */
inline void setWorldRotation(Graph& g, NodeId id, const glm::dmat3& world)
{
    const glm::dmat3 parent = math::rotationOf(parentWorldOf(g, id));
    const glm::dmat3 axis = glm::dmat3(lila::scene::eulerToMatrix(vecOf(g, id, lila::scene::Transform::kRotateAxis), lila::scene::RotateOrder::XYZ));
    const glm::dmat3 r = math::localRotationFor(parent, orientOf(g, id), world) * glm::transpose(axis);
    g.setValue({id, lila::scene::Transform::kRotate, -1}, rotateFor(g, id, r));
}

/** Moves the node's origin to a world point through its translate. Records undo. */
inline void setWorldPosition(Graph& g, NodeId id, const glm::dvec3& world)
{
    const glm::dvec3 now(worldOf(g, id)[3]);
    const glm::dmat3 parent(parentWorldOf(g, id));
    const glm::dvec3 t = vecOf(g, id, lila::scene::Transform::kTranslate);
    g.setValue({id, lila::scene::Transform::kTranslate, -1}, t + glm::inverse(parent) * (world - now));
}

/** Runs a command from UI code; failures go to the log instead of out of a Qt slot or gizmo callback. */
inline json runCommand(const char* name, const json& call = json::object())
{
    try
    {
        return lila::ops::execute(name, call, lila::CallSource::UI);
    }
    catch (const std::exception& e)
    {
        lila::log::write(lila::LogLevel::Error, e.what());
        return nullptr;
    }
}

/** Names from a command's positional args, or the selection; empty strings dropped. */
inline std::vector<std::string> objectArgs(const lila::Args& a)
{
    std::vector<std::string> out;
    for (const std::string& s : a.objects())
        if (!s.empty())
            out.push_back(s);
    return out;
}

/** The command's own call with its objects and flags passed on as they came. */
inline json forwarded(const lila::Args& a)
{
    json call = a.flags;
    call["args"] = objectArgs(a);
    return call;
}

inline json vecJson(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }

/**
 * Keys node.attr ("translate", "rotate", "scale") to v at time, whatever drives it: a key is set, then each axis gets
 * its value, which works for three Euler curves and for one rotation curve alike, under layers and constraint blends.
 */
inline void keyVec(const std::string& node, const std::string& attr, const glm::dvec3& v, double time)
{
    const char axes[3] = {'x', 'y', 'z'};
    // On an animation layer each key takes what the layer adds to reach v, which setKeyframe's value works out from what
    // is below the layer at the scene's time: so the time steps there first.
    lila::scene::Graph& g = lila::scene::Graph::current();
    if (lila::scene::anim::keyLayer(g, g.plug(node + "." + attr)) != lila::scene::kNoNode)
    {
        const double now = g.time();
        g.setTime(time, false);
        g.evaluate();
        for (int k = 0; k < 3; ++k)
            lila::ops::execute("setKeyframe", {{"args", {node + "." + attr.substr(0, 1) + axes[k]}}, {"time", time}, {"value", v[k]}});
        g.setTime(now, false);
        g.evaluate();
        return;
    }
    lila::ops::execute("setKeyframe", {{"args", {node + "." + attr}}, {"time", time}});
    for (int k = 0; k < 3; ++k)
        lila::ops::execute("keyframe", {{"args", {node + "." + attr.substr(0, 1) + axes[k]}}, {"time", json::array({time, time})},
                                         {"valueChange", v[k]}});
}
inline glm::dvec3 jsonVec(const json& j) { return {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()}; }
} // namespace puppeteer
