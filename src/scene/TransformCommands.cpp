#include "ops/Command.h"
#include "scene/CenterInMesh.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/OrientPivot.h"
#include "scene/Rig.h"
#include "scene/RigElements.h"
#include "scene/TransformOps.h"

namespace lila::scene
{
namespace
{
std::vector<NodeId> transformsOf(const Args& a, const char* cmd)
{
    std::vector<NodeId> ids;
    for (const std::string& name : a.objects())
    {
        Node* n = Graph::current().find(name);
        if (!n)
            throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
        if (n->type().isA("transform"))
            ids.push_back(n->id());
    }
    if (ids.empty())
        throw CommandError(std::string(cmd) + ": nothing to transform");
    return ids;
}

glm::dvec3 vec3(const json& j)
{
    return {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()};
}

constexpr const char* kAxisFlags[] = {"x", "y", "z"};

/** x=, y=, z=: the axes named, with NaN for the others; nullopt when none is. */
std::optional<glm::dvec3> axisArgs(const Args& a)
{
    glm::dvec3 v(std::numeric_limits<double>::quiet_NaN());
    bool any = false;
    for (int i = 0; i < 3; ++i)
        if (const char* f = kAxisFlags[i]; a.has(f))
        {
            v[i] = a.get<double>(f);
            any = true;
        }
    return any ? std::optional(v) : std::nullopt;
}

/** The axes named in v replace those of current. */
glm::dvec3 withAxes(glm::dvec3 current, const glm::dvec3& v)
{
    for (int i = 0; i < 3; ++i)
        if (!std::isnan(v[i]))
            current[i] = v[i];
    return current;
}

/** Per-axis flags act on the component selection when there is one and no objects were named. */
bool onComponents(const Args& a)
{
    const ComponentMode m = components::mode();
    return a.positional.empty() && m != ComponentMode::Object && !components::selected(m).empty();
}

json vecJson(const glm::dvec3& v)
{
    return json::array({v.x, v.y, v.z});
}

Space spaceArg(const Args& a)
{
    std::string s = a.get<std::string>("space", "world");
    if (s == "world")
        return Space::World;
    if (s == "object")
        return Space::Object;
    if (s == "parent")
        return Space::Parent;
    throw CommandError("space must be 'world', 'object' or 'parent'");
}

glm::dquat eulerQuat(const glm::dvec3& degrees)
{
    return glm::quat_cast(glm::dmat3(eulerToMatrix(degrees, RotateOrder::XYZ)));
}
} // namespace

void registerTransformCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "move",
        .doc = "Move objects. move(by=(x, y, z)) moves relative in `space`; move(to=(x, y, z)) puts each pivot at a world "
               "position. A selected parent and child move once.\n\n"
               "move(x=0) sets one world axis and leaves the others: each selected object's pivot, or with components selected "
               "every selected point, so move(x=0) puts an edge loop on the centre line.",
        .positionalDoc = "objects",
        .flags = {
            {"by", "", T::Vec3, nullptr, "Relative offset, cm."},
            {"to", "", T::Vec3, nullptr, "Absolute world position of the pivot, cm."},
            {"x", "", T::Float, nullptr, "World X for the pivot, or for every selected point, cm."},
            {"y", "", T::Float, nullptr, "World Y, cm."},
            {"z", "", T::Float, nullptr, "World Z, cm."},
            {"space", "", T::String, "world", "'world', 'object' or 'parent' for `by`."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (std::optional<glm::dvec3> axes = axisArgs(a))
            {
                if (onComponents(a))
                {
                    // Squashing the named axes to nothing about a pivot on the target plane lands every point on it.
                    glm::dvec3 factor(1.0), pivot(0.0);
                    for (int i = 0; i < 3; ++i)
                        if (!std::isnan((*axes)[i]))
                        {
                            factor[i] = 0.0;
                            pivot[i] = (*axes)[i];
                        }
                    ops::execute("scaleComponents", {{"by", vecJson(factor)}, {"pivot", vecJson(pivot)}});
                    return nullptr;
                }
                auto ids = transformsOf(a, "move");
                rigel::poseControllers(g, ids, [&] {
                    for (NodeId id : xformops::topLevel(g, ids))
                    {
                        const glm::dvec3 pivot = xformops::worldPivot(g, id);
                        xformops::moveBy(g, {id}, withAxes(pivot, *axes) - pivot);
                    }
                });
                return nullptr;
            }
            auto ids = transformsOf(a, "move");
            if (!a.has("to") && !a.has("by"))
                throw CommandError("move: give by=, to= or x=, y=, z=");
            // An IK controller poses its chain's joints.
            rigel::poseControllers(g, ids, [&] {
                if (a.has("to"))
                {
                    glm::dvec3 to = vec3(a.flags["to"]);
                    for (NodeId id : xformops::topLevel(g, ids))
                        xformops::moveBy(g, {id}, to - xformops::worldPivot(g, id));
                }
                else
                    xformops::moveBy(g, ids, vec3(a.flags["by"]), spaceArg(a));
            });
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "rotate",
        .doc = "Rotate objects about their pivots. rotate(by=(x, y, z)) adds a rotation (degrees, XYZ order) in `space`; "
               "rotate(to=(x, y, z)) sets the local Euler angles.\n\n"
               "rotate(x=90) sets one local angle and leaves the others. With components selected it turns them that far about "
               "the world axis through their centre instead.",
        .positionalDoc = "objects",
        .flags = {
            {"by", "", T::Vec3, nullptr, "Relative rotation, degrees."},
            {"to", "", T::Vec3, nullptr, "Absolute local rotation, degrees."},
            {"x", "", T::Float, nullptr, "Local X rotation, degrees; on components, a turn about world X."},
            {"y", "", T::Float, nullptr, "Local Y rotation, degrees."},
            {"z", "", T::Float, nullptr, "Local Z rotation, degrees."},
            {"space", "", T::String, "world", "'world', 'object' or 'parent' for `by`, or 'gimbal' to add `by` to the rotate channels."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (std::optional<glm::dvec3> axes = axisArgs(a))
            {
                if (onComponents(a))
                {
                    ops::execute("rotateComponents", {{"by", vecJson(withAxes(glm::dvec3(0.0), *axes))}});
                    return nullptr;
                }
                auto ids = transformsOf(a, "rotate");
                rigel::poseControllers(g, ids, [&] {
                    for (NodeId id : ids)
                    {
                        const PlugRef plug{id, Transform::kRotate};
                        g.setValue(plug, g.keepLocked(plug, withAxes(g.node(id)->as<glm::dvec3>(Transform::kRotate), *axes)));
                    }
                });
                return nullptr;
            }
            auto ids = transformsOf(a, "rotate");
            if (!a.has("to") && !a.has("by"))
                throw CommandError("rotate: give by=, to= or x=, y=, z=");
            rigel::poseControllers(g, ids, [&] {
                if (a.has("to"))
                {
                    for (NodeId id : ids)
                        g.setValue({id, Transform::kRotate}, g.keepLocked({id, Transform::kRotate}, vec3(a.flags["to"])));
                }
                else if (a.get<std::string>("space", "world") == "gimbal")
                    xformops::rotateChannelsBy(g, ids, vec3(a.flags["by"]));
                else
                    xformops::rotateBy(g, ids, eulerQuat(vec3(a.flags["by"])), spaceArg(a));
            });
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "scale",
        .doc = "Scale objects along their own axes. scale(by=(x, y, z)) multiplies; scale(to=(x, y, z)) sets.\n\n"
               "scale(x=2) sets one local scale and leaves the others. With components selected it scales them by that factor "
               "along the world axis about their centre instead, so scale(y=0) flattens them.",
        .positionalDoc = "objects",
        .flags = {
            {"by", "", T::Vec3, nullptr, "Factor per axis."},
            {"to", "", T::Vec3, nullptr, "Absolute scale."},
            {"x", "", T::Float, nullptr, "Local X scale; on components, a factor along world X."},
            {"y", "", T::Float, nullptr, "Local Y scale."},
            {"z", "", T::Float, nullptr, "Local Z scale."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (std::optional<glm::dvec3> axes = axisArgs(a))
            {
                if (onComponents(a))
                {
                    ops::execute("scaleComponents", {{"by", vecJson(withAxes(glm::dvec3(1.0), *axes))}});
                    return nullptr;
                }
                for (NodeId id : transformsOf(a, "scale"))
                {
                    const PlugRef plug{id, Transform::kScale};
                    g.setValue(plug, g.keepLocked(plug, withAxes(g.node(id)->as<glm::dvec3>(Transform::kScale), *axes)));
                }
                return nullptr;
            }
            auto ids = transformsOf(a, "scale");
            if (a.has("to"))
            {
                for (NodeId id : ids)
                    g.setValue({id, Transform::kScale}, g.keepLocked({id, Transform::kScale}, vec3(a.flags["to"])));
            }
            else if (a.has("by"))
                xformops::scaleBy(g, ids, vec3(a.flags["by"]));
            else
                throw CommandError("scale: give by=, to= or x=, y=, z=");
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "setPivot",
        .doc = "Move the rotate and scale pivots to a world position, or orient the pivot, without moving the objects. "
               "A joint's pivot is the joint itself: position= moves the joint and orient= sets its jointOrient, leaving its "
               "children where they are.",
        .positionalDoc = "objects",
        .flags = {
            {"position", "", T::Vec3, nullptr, "World position, cm."},
            {"orient", "", T::Vec3, nullptr, "Pivot orientation relative to the object's axes, Euler XYZ degrees (pivotOrient); "
                                             "on a joint, its jointOrient."},
        },
        .run = [](const Args& a) -> json {
            if (!a.has("position") && !a.has("orient"))
                throw CommandError("setPivot: give position= or orient=");
            Graph& g = Graph::current();
            for (NodeId id : transformsOf(a, "setPivot"))
            {
                const bool joint = rig::isJoint(*g.node(id));
                if (a.has("position") && joint)
                    rig::moveNodeTo(g, id, vec3(a.flags["position"]), true);
                else if (a.has("position"))
                    xformops::setWorldPivot(g, id, vec3(a.flags["position"]));
                if (a.has("orient") && joint)
                    rig::setJointOrient(g, id, vec3(a.flags["orient"]));
                else if (a.has("orient"))
                    g.setValue({id, Transform::kPivotOrient}, vec3(a.flags["orient"]));
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "resetPivot",
        .doc = "Put the pivots back at the object's origin, keeping the object where it is.",
        .positionalDoc = "objects",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            for (NodeId id : transformsOf(a, "resetPivot"))
                xformops::resetPivot(g, id);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "zeroPivotRotation",
        .doc = "Line the pivots' axes up with the world (or with the object's own axes), leaving their positions and the "
               "objects alone.",
        .positionalDoc = "objects",
        .flags = {{"space", "", T::String, "world", "Axes to line up with.", {"world", "object"}}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const bool world = a.get<std::string>("space", "world") == "world";
            for (NodeId id : transformsOf(a, "zeroPivotRotation"))
            {
                if (world)
                    xformops::setWorldPivotFrame(g, id, glm::dmat3(1.0));
                else
                    g.setValue({id, Transform::kPivotOrient}, glm::dvec3(0.0));
            }
            return nullptr;
        },
        .adjustable = true,
    });

    ops::registerCommand({
        .name = "zeroPivot",
        .doc = "Move the pivots to the world origin, keeping the object where it is.",
        .positionalDoc = "objects",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            for (NodeId id : transformsOf(a, "zeroPivot"))
                xformops::setWorldPivot(g, id, glm::dvec3(0.0));
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "matchPivot",
        .doc = "Copy the first object's pivot onto the others, keeping them where they are. position=False keeps their pivot "
               "positions and orient=False their pivot orientations.",
        .positionalDoc = "objects (the first is the source)",
        .flags = {
            {"position", "p", T::Bool, true, "Copy the pivot's world position."},
            {"orient", "o", T::Bool, true, "Copy the pivot's world axes."},
        },
        .run = [](const Args& a) -> json {
            const bool position = a.get<bool>("position"), orient = a.get<bool>("orient");
            if (!position && !orient)
                throw CommandError("matchPivot: position and orient are both off");
            auto ids = transformsOf(a, "matchPivot");
            if (ids.size() < 2)
                throw CommandError("matchPivot: select the object to copy from, then the ones to change");
            Graph& g = Graph::current();
            const NodeId source = ids.front();
            ids.erase(ids.begin());
            g.evaluate();
            const glm::dvec3 at = xformops::worldPivot(g, source);
            const glm::dmat3 axes = xformops::pivotFrame(g, source);
            for (NodeId id : ids)
            {
                if (orient)
                    xformops::setWorldPivotFrame(g, id, axes);
                if (position)
                    xformops::setWorldPivot(g, id, at);
            }
            return nullptr;
        },
        .adjustable = true,
    });

    ops::registerCommand({
        .name = "pivot",
        .doc = "World position of an object's rotate pivot.",
        .positionalDoc = "object",
        .undoable = false,
        .run = [](const Args& a) -> json {
            auto ids = transformsOf(a, "pivot");
            glm::dvec3 p = xformops::worldPivot(Graph::current(), ids.front());
            return json::array({p.x, p.y, p.z});
        },
    });

    orientpivot::registerCommand();
}
} // namespace lila::scene
