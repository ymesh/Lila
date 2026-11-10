#include "scene/Constraint.h"

#include "core/Profile.h"
#include "scene/AnimLayer.h"
#include "scene/AttrBuilders.h"
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/TransformOps.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <optional>

namespace lila::scene
{
using namespace rigutil;
using namespace attrdefs;

namespace
{
const char* kConstraintTypes[] = {"parentConstraint", "pointConstraint", "orientConstraint", "aimConstraint"};

/** Weighted quaternion average in one hemisphere; fine for the small spreads constraint targets have. */
struct QuatBlend
{
    glm::dquat sum{0, 0, 0, 0};
    glm::dquat first{1, 0, 0, 0};
    bool any = false;

    void add(const glm::dquat& q, double w)
    {
        if (!any)
            first = q;
        any = true;
        sum += (glm::dot(first, q) < 0.0 ? -q : q) * w;
    }
    glm::dmat3 result() const { return glm::length(sum) > 1e-12 ? glm::mat3_cast(glm::normalize(sum)) : glm::dmat3(1.0); }
};
} // namespace

void ConstraintNode::compute(const EvalContext&)
{
    LILA_ZONE();

    glm::dmat4 parent = as<glm::dmat4>(kParentMatrix);
    glm::dmat4 parentInv = glm::inverse(parent);
    glm::dmat3 parentRot = orthonormal(glm::dmat3(parent));
    glm::dvec3 rp = as<glm::dvec3>(kRotatePivot), rpt = as<glm::dvec3>(kRotatePivotTranslate);
    glm::dvec3 restT = as<glm::dvec3>(kRestTranslate), restR = as<glm::dvec3>(kRestRotate);
    auto order = RotateOrder(as<int64_t>(kRotateOrder));
    glm::dmat3 jo(eulerToMatrix(as<glm::dvec3>(kJointOrient), RotateOrder::XYZ));
    glm::dvec3 offset = as<glm::dvec3>(kOffset);

    glm::dvec3 outT = restT, outR = restR;
    // World rotation to local rotate values, through the parent and jointOrient, staying near the rest pose.
    auto toRotate = [&](const glm::dmat3& world) {
        glm::dmat3 r = glm::transpose(jo) * glm::transpose(parentRot) * world;
        return xformops::closestEuler(r, order, restR);
    };

    const bool pivotOffsets = as<bool>(kPivotOffsets);
    double wsum = 0.0;
    glm::dvec3 pos(0.0);
    QuatBlend rot;
    for (const auto& [i, target] : elements(kTargetMatrix))
    {
        double w = elementOr<double>(kTargetWeight, i);
        if (w <= 0.0)
            continue;
        glm::dmat4 m = std::get<glm::dmat4>(target);
        glm::dmat3 r = orthonormal(glm::dmat3(m));
        if (kind == Kind::Parent)
        {
            // The offset rides the target's scale for its position but not its turn, so a non-uniform scale doesn't skew it.
            const glm::dmat4& targetOffset = elementOr<glm::dmat4>(kTargetOffset, i);
            m = m * targetOffset;
            r = r * orthonormal(glm::dmat3(targetOffset));
            // Targets pull the rotate pivot, as Maya blends them, so several targets average where it goes.
            if (!pivotOffsets)
                m[3] += glm::dvec4(r * rp, 0.0);
        }
        pos += glm::dvec3(m[3]) * w;
        rot.add(glm::quat_cast(r), w);
        wsum += w;
    }

    if (wsum > 0.0)
    {
        pos /= wsum;
        switch (kind)
        {
        case Kind::Parent:
        {
            glm::dmat4 world(rot.result());
            world[3] = glm::dvec4(pos - glm::dmat3(world) * rp, 1.0);
            glm::dmat4 local = parentInv * world;
            glm::dmat3 lr = orthonormal(glm::dmat3(local));
            outR = xformops::closestEuler(glm::transpose(jo) * lr, order, restR);
            // The local matrix places the origin at T + rpt + rp - (JO R) rp; solve that for T.
            outT = glm::dvec3(local[3]) - rpt - rp + lr * rp;
            break;
        }
        case Kind::Point:
            outT = glm::dvec3(parentInv * glm::dvec4(pos, 1.0)) - rp - rpt + offset;
            break;
        case Kind::Orient:
            outR = toRotate(rot.result() * glm::dmat3(eulerToMatrix(offset, RotateOrder::XYZ)));
            break;
        case Kind::Aim:
        {
            glm::dvec3 origin(parent * glm::dvec4(restT + rpt + rp, 1.0));
            glm::dvec3 aim = pos - origin;
            if (glm::length(aim) < 1e-9)
                break;
            glm::dvec3 aimVec = as<glm::dvec3>(kAimVector), upVec = as<glm::dvec3>(kUpVector);
            glm::dvec3 worldUp = as<glm::dvec3>(kWorldUpVector);
            glm::dmat4 upMatrix = as<glm::dmat4>(kWorldUpMatrix);
            glm::dmat3 frame;
            switch (as<int64_t>(kWorldUpType))
            {
            case kUpScene: frame = rig::aimFrame(aimVec, upVec, aim, {0, 1, 0}); break;
            case kUpObject: frame = rig::aimFrame(aimVec, upVec, aim, glm::dvec3(upMatrix[3]) - origin); break;
            case kUpObjectRotation: frame = rig::aimFrame(aimVec, upVec, aim, glm::dmat3(upMatrix) * worldUp); break;
            case kUpVectorOnly: frame = rig::aimFrame(aimVec, upVec, aim, worldUp); break;
            default: frame = glm::mat3_cast(glm::rotation(glm::normalize(aimVec), glm::normalize(aim))); break;
            }
            outR = toRotate(frame * glm::dmat3(eulerToMatrix(offset, RotateOrder::XYZ)));
            break;
        }
        }
    }

    for (int k = 0; k < 3; ++k)
    {
        if (as<bool>(kSkipTranslateX + k))
            outT[k] = restT[k];
        if (as<bool>(kSkipRotateX + k))
            outR[k] = restR[k];
    }
    setOutput(kConstraintTranslate, outT);
    setOutput(kConstraintRotate, outR);
}

namespace
{
using Kind = ConstraintNode::Kind;

bool drivesTranslate(Kind k) { return k == Kind::Parent || k == Kind::Point; }
bool drivesRotate(Kind k) { return k != Kind::Point; }

/** The constraint driving obj.attr: through its blend, or connected straight to the plug (older scenes). */
ConstraintNode* constraintOn(Graph& g, NodeId obj, int attr)
{
    if (auto src = g.source({obj, attr, -1}))
        return dynamic_cast<ConstraintNode*>(g.node(src->node));
    for (NodeId b : anim::blendChain(g, {obj, attr, -1}))
        if (!anim::layerOf(g, b))
            if (auto src = g.source({b, anim::BlendNode::kInputB, -1}))
                if (auto* c = dynamic_cast<ConstraintNode*>(g.node(src->node)))
                    return c;
    return nullptr;
}

ConstraintNode* existingConstraint(Graph& g, NodeId obj, Kind kind)
{
    for (int attr : {int(Transform::kTranslate), int(Transform::kRotate)})
        if (auto* c = constraintOn(g, obj, attr); c && c->kind == kind)
            return c;
    return nullptr;
}

void setSkips(Graph& g, NodeId c, const json& axes, int first, const char* cmd)
{
    if (axes.is_null())
        return;
    for (const json& ax : axes)
    {
        std::string s = ax.get<std::string>();
        if (s.size() != 1 || s[0] < 'x' || s[0] > 'z')
            throw CommandError(std::string(cmd) + ": skip axes are x, y or z");
        g.setValue({c, first + (s[0] - 'x')}, true);
    }
}

json makeConstraint(Kind kind, const Args& a)
{
    LILA_ZONE();

    const char* cmd = kConstraintTypes[int(kind)];
    Graph& g = graph();
    auto objs = a.objects();
    if (objs.size() < 2)
        throw CommandError(std::string(cmd) + ": give one or more targets, then the object to constrain");
    Node& obj = nodeNamed(objs.back(), cmd);
    if (!obj.type().isA("transform"))
        throw CommandError(std::string(cmd) + ": " + obj.path() + " is not a transform");
    std::vector<NodeId> targets;
    for (size_t i = 0; i + 1 < objs.size(); ++i)
    {
        Node& t = nodeNamed(objs[i], cmd);
        if (!t.type().isA("transform"))
            throw CommandError(std::string(cmd) + ": " + t.path() + " is not a transform");
        targets.push_back(t.id());
    }

    g.evaluate();
    NodeId objId = obj.id();
    glm::dvec3 curT = obj.as<glm::dvec3>(Transform::kTranslate), curR = obj.as<glm::dvec3>(Transform::kRotate);
    glm::dmat4 objWorld = worldOf(obj);
    bool joint = rig::isJoint(obj);

    ConstraintNode* c = existingConstraint(g, objId, kind);
    bool created = !c;
    if (created)
        for (int attr : {int(Transform::kTranslate), int(Transform::kRotate)})
            if ((attr == Transform::kTranslate ? drivesTranslate(kind) : drivesRotate(kind)) && constraintOn(g, objId, attr))
                throw CommandError(std::string(cmd) + ": " + g.plugName({objId, attr, -1}) + " is already constrained");
    if (!c)
    {
        std::string name = a.has("name") ? a.get<std::string>("name") : obj.name() + "_" + cmd;
        c = static_cast<ConstraintNode*>(guarded([&] { return g.createNode(*types::find(cmd), name, kNoNode, true); }));
        NodeId id = c->id();
        g.setValue({id, ConstraintNode::kRotateOrder}, obj.value(Transform::kRotateOrder));
        if (joint)
            g.setValue({id, ConstraintNode::kJointOrient}, obj.value(Joint::kJointOrient));
        g.setValue({id, ConstraintNode::kRotatePivot}, obj.value(Transform::kRotatePivot));
        g.setValue({id, ConstraintNode::kRotatePivotTranslate}, obj.value(Transform::kRotatePivotTranslate));
        g.setValue({id, ConstraintNode::kRestTranslate}, curT);
        g.setValue({id, ConstraintNode::kRestRotate}, curR);
        if (NodeId p = g.parent(objId); p != kNoNode)
            guarded([&] { g.connect({p, Transform::kWorldMatrix, -1}, {id, ConstraintNode::kParentMatrix, -1}); return 0; });
        if (kind == Kind::Parent)
        {
            setSkips(g, id, a.flags.value("skipTranslate", json()), ConstraintNode::kSkipTranslateX, cmd);
            setSkips(g, id, a.flags.value("skipRotate", json()), ConstraintNode::kSkipRotateX, cmd);
        }
        else
            setSkips(g, id, a.flags.value("skip", json()), kind == Kind::Point ? ConstraintNode::kSkipTranslateX : ConstraintNode::kSkipRotateX, cmd);
        if (kind == Kind::Aim)
        {
            g.setValue({id, ConstraintNode::kAimVector}, vec3Of(a.flags["aimVector"]));
            g.setValue({id, ConstraintNode::kUpVector}, vec3Of(a.flags["upVector"]));
            g.setValue({id, ConstraintNode::kWorldUpVector}, vec3Of(a.flags["worldUpVector"]));
            std::string upType = a.get<std::string>("worldUpType");
            const char* upNames[] = {"scene", "object", "objectRotation", "vector", "none"};
            int64_t type = -1;
            for (int k = 0; k < 5; ++k)
                if (upType == upNames[k])
                    type = k;
            if (type < 0)
                throw CommandError("aimConstraint: worldUpType is scene, object, objectRotation, vector or none");
            g.setValue({id, ConstraintNode::kWorldUpType}, type);
            if (type == ConstraintNode::kUpObject || type == ConstraintNode::kUpObjectRotation)
            {
                if (!a.has("worldUpObject"))
                    throw CommandError("aimConstraint: worldUpType " + upType + " needs worldUpObject");
                NodeId up = nodeNamed(a.get<std::string>("worldUpObject"), cmd).id();
                guarded([&] { g.connect({up, Transform::kWorldMatrix, -1}, {id, ConstraintNode::kWorldUpMatrix, -1}); return 0; });
            }
        }
    }
    NodeId id = c->id();

    double weight = a.get<double>("weight", 1.0);
    std::vector<int> added;
    for (NodeId t : targets)
    {
        bool already = false;
        for (const PlugRef& e : g.elements({id, ConstraintNode::kTargetMatrix}))
            if (auto src = g.source(e); src && src->node == t)
            {
                g.setValue({id, ConstraintNode::kTargetWeight, -1, e.element}, weight);
                already = true;
            }
        if (already)
            continue;
        std::vector<int> used = g.groupIndices(id, "targets");
        const int slot = used.empty() ? 0 : used.back() + 1;
        guarded([&] { g.connect({t, Transform::kWorldMatrix, -1}, {id, ConstraintNode::kTargetMatrix, -1, slot}); return 0; });
        g.setValue({id, ConstraintNode::kTargetWeight, -1, slot}, weight);
        added.push_back(slot);
    }

    if (a.get<bool>("maintainOffset"))
    {
        g.evaluate();
        if (kind == Kind::Parent)
            for (int s : added)
                g.setValue({id, ConstraintNode::kTargetOffset, -1, s},
                           glm::inverse(c->elementOr<glm::dmat4>(ConstraintNode::kTargetMatrix, s)) * objWorld);
        else
        {
            g.setValue({id, ConstraintNode::kOffset}, glm::dvec3(0.0));
            g.evaluate();
            if (kind == Kind::Point)
                g.setValue({id, ConstraintNode::kOffset}, clean(curT - c->as<glm::dvec3>(ConstraintNode::kConstraintTranslate)));
            else
            {
                // offset = what turns the zero-offset result into the current orientation.
                glm::dmat3 parentRot = orthonormal(glm::dmat3(c->as<glm::dmat4>(ConstraintNode::kParentMatrix)));
                glm::dmat3 jo(eulerToMatrix(c->as<glm::dvec3>(ConstraintNode::kJointOrient), RotateOrder::XYZ));
                auto order = RotateOrder(c->as<int64_t>(ConstraintNode::kRotateOrder));
                glm::dmat3 zero = parentRot * jo * glm::dmat3(eulerToMatrix(c->as<glm::dvec3>(ConstraintNode::kConstraintRotate), order));
                glm::dmat3 now = orthonormal(glm::dmat3(objWorld));
                g.setValue({id, ConstraintNode::kOffset}, clean(matrixToEuler(glm::transpose(zero) * now, RotateOrder::XYZ)));
            }
        }
    }
    else if (a.has("offset") && kind != Kind::Parent)
        g.setValue({id, ConstraintNode::kOffset}, vec3Of(a.flags["offset"]));

    // Through a blend at the bottom of the channel's chain, so the channel stays keyable and layerable.
    if (created)
        guarded([&] {
            if (drivesTranslate(kind))
                anim::insertConstraintBlend(g, {objId, Transform::kTranslate, -1}, {id, ConstraintNode::kConstraintTranslate, -1});
            if (drivesRotate(kind))
                anim::insertConstraintBlend(g, {objId, Transform::kRotate, -1}, {id, ConstraintNode::kConstraintRotate, -1});
            return 0;
        });
    return g.node(id)->path();
}

void registerConstraintCommandsImpl()
{
    using T = ArgType;
    std::vector<FlagDef> common = {
        {"maintainOffset", "mo", T::Bool, true, "Keep the object where it is now relative to the targets."},
        {"weight", "w", T::Float, 1.0, "Weight of the given targets.", {}, {}, 0.0},
        {"name", "n", T::String, nullptr, "Name for a new constraint."},
    };
    auto with = [&](std::vector<FlagDef> extra) {
        std::vector<FlagDef> f = common;
        f.insert(f.end(), extra.begin(), extra.end());
        return f;
    };
    const char* targetsDoc = "targets..., object";

    ops::registerCommand({
        .name = "parentConstraint",
        .doc = "Make the object follow the weighted blend of its targets' translation and rotation, as if parented. "
               "Running it again on a constrained object adds targets (up to 8). Returns the constraint.",
        .positionalDoc = targetsDoc,
        .flags = with({{"skipTranslate", "st", T::List, nullptr, "Axes left alone, e.g. [\"y\"]."},
                       {"skipRotate", "sr", T::List, nullptr, "Axes left alone."}}),
        .run = [](const Args& a) -> json { return makeConstraint(Kind::Parent, a); },
    });
    ops::registerCommand({
        .name = "pointConstraint",
        .doc = "Make the object's pivot follow the weighted average of its targets' positions. Returns the constraint.",
        .positionalDoc = targetsDoc,
        .flags = with({{"offset", "o", T::Vec3, nullptr, "Translate offset (when not maintaining the current one)."},
                       {"skip", "sk", T::List, nullptr, "Axes left alone."}}),
        .run = [](const Args& a) -> json { return makeConstraint(Kind::Point, a); },
    });
    ops::registerCommand({
        .name = "orientConstraint",
        .doc = "Make the object's orientation follow the weighted blend of its targets'. Returns the constraint.",
        .positionalDoc = targetsDoc,
        .flags = with({{"offset", "o", T::Vec3, nullptr, "Rotation offset, degrees xyz."}, {"skip", "sk", T::List, nullptr, "Axes left alone."}}),
        .run = [](const Args& a) -> json { return makeConstraint(Kind::Orient, a); },
    });
    ops::registerCommand({
        .name = "aimConstraint",
        .doc = "Turn the object so aimVector points at its targets and upVector leans toward the world up. The aim "
               "starts from the object's position when constrained (restTranslate). Returns the constraint.",
        .positionalDoc = targetsDoc,
        .flags = with({
            {"aimVector", "aim", T::Vec3, json::array({1.0, 0.0, 0.0}), "Local axis that points at the target."},
            {"upVector", "u", T::Vec3, json::array({0.0, 1.0, 0.0}), "Local axis that leans toward the world up."},
            {"worldUpType", "wut", T::String, "vector", "scene | object | objectRotation | vector | none"},
            {"worldUpVector", "wu", T::Vec3, json::array({0.0, 1.0, 0.0}), "World up (or up in worldUpObject's space)."},
            nodeFlag({"worldUpObject", "wuo", T::String, nullptr, "Object for worldUpType object / objectRotation."}, "transform"),
            {"offset", "o", T::Vec3, nullptr, "Rotation offset, degrees xyz."},
            {"skip", "sk", T::List, nullptr, "Axes left alone."},
        }),
        .run = [](const Args& a) -> json { return makeConstraint(Kind::Aim, a); },
    });

    ops::registerCommand({
        .name = "removeConstraint",
        .doc = "Delete constraints (give the constraints or the constrained objects). The objects keep their current pose.",
        .positionalDoc = "constraints or objects",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> list;
            for (const std::string& name : a.objects())
            {
                Node& n = nodeNamed(name, "removeConstraint");
                if (dynamic_cast<ConstraintNode*>(&n))
                    list.push_back(n.id());
                else
                    for (int attr : {int(Transform::kTranslate), int(Transform::kRotate)})
                        if (ConstraintNode* c = constraintOn(g, n.id(), attr))
                            list.push_back(c->id());
            }
            std::sort(list.begin(), list.end());
            list.erase(std::unique(list.begin(), list.end()), list.end());
            if (list.empty())
                throw CommandError("removeConstraint: nothing constrained");
            for (NodeId id : list)
            {
                g.evaluate();
                for (int out : {int(ConstraintNode::kConstraintTranslate), int(ConstraintNode::kConstraintRotate)})
                {
                    PlugValue v = g.node(id)->value(out);
                    for (const PlugRef& d : g.destinations({id, out, -1}))
                    {
                        Node* dn = g.node(d.node);
                        if (dn && anim::isBlend(*dn) && d.attr == anim::BlendNode::kInputB)
                        {
                            // Unanimated channels keep the constrained pose; animated ones go back to their keys.
                            g.disconnect(d);
                            PlugRef base{d.node, anim::BlendNode::kInputA, -1};
                            for (int c = 0; c < 3; ++c)
                                if (!g.source(base) && !g.source({d.node, base.attr, c}))
                                    g.setValue({d.node, base.attr, c}, std::get<glm::dvec3>(v)[c]);
                            anim::removeBlend(g, d.node, false);
                            continue;
                        }
                        g.disconnect(d);
                        g.setValue(d, v);
                    }
                }
                g.deleteNode(id);
            }
            return nullptr;
        },
    });
}
} // namespace

void registerConstraintTypes()
{
    if (types::find("constraint"))
        return;
    const uint32_t stored = AttrInput | AttrStorable;
    NodeType base;
    base.name = "constraint";
    base.icon = "link";
    base.ownAttrs = {
        matrixAttr("constraintParentMatrix", AttrInput | AttrHidden, "World matrix of the constrained object's parent."),
        {.name = "constraintRotateOrder", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = stored,
         .enumNames = {"xyz", "yzx", "zxy", "xzy", "yxz", "zyx"}, .doc = "The constrained object's rotate order."},
        vec3Attr("constraintJointOrient", glm::dvec3(0.0), stored, "The constrained joint's jointOrient."),
        vec3Attr("constraintRotatePivot", glm::dvec3(0.0), stored, "The constrained object's rotatePivot."),
        vec3Attr("constraintRotateTranslate", glm::dvec3(0.0), stored, "The constrained object's rotatePivotTranslate."),
        vec3Attr("restTranslate", glm::dvec3(0.0), stored, "Translate for skipped axes; the aim origin."),
        vec3Attr("restRotate", glm::dvec3(0.0), stored, "Rotate for skipped axes."),
        boolAttr("skipTranslateX", false, "Leave translateX alone."),
        boolAttr("skipTranslateY", false, "Leave translateY alone."),
        boolAttr("skipTranslateZ", false, "Leave translateZ alone."),
        boolAttr("skipRotateX", false, "Leave rotateX alone."),
        boolAttr("skipRotateY", false, "Leave rotateY alone."),
        boolAttr("skipRotateZ", false, "Leave rotateZ alone."),
        vec3Attr("offset", glm::dvec3(0.0), stored | AttrKeyable, "Translate (point) or rotation (orient, aim) offset."),
    };
    // One element per target, matched by index; the numbered target0Matrix ... attrs of older scenes load into them.
    base.ownAttrs.push_back({.name = "targets.matrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0),
                             .flags = AttrInput | AttrHidden | AttrArray, .doc = "Target world matrix.", .fileAlias = "target#Matrix"});
    base.ownAttrs.push_back({.name = "targets.weight", .type = PlugType::Float, .defaultValue = 1.0, .flags = stored | AttrKeyable | AttrArray,
                             .doc = "Target weight.", .fileAlias = "target#Weight", .minValue = 0.0});
    base.ownAttrs.push_back({.name = "targets.offset", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0),
                             .flags = stored | AttrHidden | AttrArray, .doc = "Parent constraint: the object relative to the target.",
                             .fileAlias = "target#Offset"});
    base.retiredAttrs = {"targetCount"};
    base.ownAttrs.push_back(vec3Attr("constraintTranslate", glm::dvec3(0.0), AttrOutput, "Drives the object's translate."));
    base.ownAttrs.push_back(vec3Attr("constraintRotate", glm::dvec3(0.0), AttrOutput, "Drives the object's rotate."));
    base.ownAttrs.push_back(boolAttr("pivotOffsets", false, "Parent constraint: each target's offset places the rotate pivot, as Maya's do, rather than the origin."));
    const NodeType& b = types::registerType(std::move(base));

    for (int k = 0; k < 4; ++k)
    {
        NodeType t;
        t.name = kConstraintTypes[k];
        t.base = &b;
        if (Kind(k) == Kind::Aim)
            t.ownAttrs = {
                vec3Attr("aimVector", glm::dvec3(1, 0, 0), stored, "Local axis that points at the target."),
                vec3Attr("upVector", glm::dvec3(0, 1, 0), stored, "Local axis that leans toward the world up."),
                {.name = "worldUpType", .type = PlugType::Enum, .defaultValue = int64_t(ConstraintNode::kUpVectorOnly), .flags = stored,
                 .enumNames = {"scene", "object", "objectRotation", "vector", "none"}, .doc = "Where the up direction comes from."},
                vec3Attr("worldUpVector", glm::dvec3(0, 1, 0), stored, "World up, or up in the up object's space."),
                matrixAttr("worldUpMatrix", AttrInput | AttrHidden, "World matrix of the up object."),
            };
        t.create = [k] {
            auto n = std::make_unique<ConstraintNode>();
            n->kind = Kind(k);
            return n;
        };
        types::registerType(std::move(t));
    }
}

void registerConstraintCommands()
{
    registerConstraintCommandsImpl();
}
} // namespace lila::scene
