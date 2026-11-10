#include "layout/SoftNetBind.h"

#include "layout/LayoutNode.h"
#include "layout/Resolve.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/RigUtil.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <map>
#include <optional>
#include <string>

namespace lila::layout
{
namespace
{
using namespace lila::scene;
using namespace lila::scene::rigutil;

/** A net's knots and handles as bound: which node is which, their rest frames, and the mirror plane. */
struct NetView
{
    NodeId hub = kNoNode;
    NetBind bind;
    std::vector<NodeId> knots; // by index
    std::map<NodeId, int> indexOf;
    /** The layout's mirror axis, -1 for none. */
    int axis = -1;

    glm::dmat4 restFrame(int k) const
    {
        const deform::softnet::KnotRig& r = bind.knots[size_t(k)];
        glm::dmat4 f(r.rest);
        f[3] = glm::dvec4(r.origin, 1.0);
        return f;
    }
};

NetView viewOf(Graph& g, NodeId hub)
{
    NetView v;
    v.hub = hub;
    const Node& h = *g.node(hub);
    v.bind = NetBind::fromJson(h.as<std::string>(h.type().attrIndex("binding")));
    v.knots.assign(v.bind.knots.size(), kNoNode);
    const NodeType* knotType = types::find(kSoftNetKnotType);
    for (NodeId c : g.children(hub))
        if (const Node* n = g.node(c); n && n->type().name == kSoftNetKnotType)
            if (const int64_t i = n->as<int64_t>(knotType->attrIndex("index")); i >= 0 && size_t(i) < v.knots.size())
            {
                v.knots[size_t(i)] = c;
                v.indexOf[c] = int(i);
            }
    if (Node* layoutNode = g.find(h.as<std::string>(h.type().attrIndex("layout"))))
        if (Node* ln = findLayoutNode(g, layoutNode->id()))
            if (const auto r = resolvedLayout(g, ln->id()); r && r->layout().mirrorAxis >= 0)
                v.axis = r->layout().mirrorAxis;
    return v;
}

int64_t junctionOf(Graph& g, NodeId knot) { return g.node(knot)->as<int64_t>(types::find(kSoftNetKnotType)->attrIndex("junction")); }

/** The knot across the mirror: the one on the junction's mirror image, else the one standing at its reflection. -1 for none or itself. */
int partnerOf(Graph& g, const NetView& v, int k)
{
    const int64_t j = junctionOf(g, v.knots[size_t(k)]);
    for (size_t o = 0; o < v.knots.size(); ++o)
        if (v.knots[o] != kNoNode && int(o) != k && junctionOf(g, v.knots[o]) == (j ^ int64_t(kMirrored)))
            return int(o);
    if (v.axis < 0)
        return -1;
    glm::dvec3 m = v.bind.knots[size_t(k)].origin;
    m[v.axis] = -m[v.axis];
    int best = -1;
    double bestD = 1e300, spread = 0.0;
    for (size_t o = 0; o < v.knots.size(); ++o)
    {
        spread = std::max(spread, glm::length(v.bind.knots[o].origin - v.bind.knots[size_t(k)].origin));
        if (const double d = glm::length(v.bind.knots[o].origin - m); d < bestD)
        {
            bestD = d;
            best = int(o);
        }
    }
    return best != k && bestD < std::max(spread * 0.02, 1e-6) ? best : -1;
}

glm::dmat4 reflection(int axis)
{
    glm::dmat4 m(1.0);
    m[axis][axis] = -1.0;
    return m;
}

std::vector<NodeId> tangentsOf(Graph& g, NodeId knot)
{
    std::vector<NodeId> out;
    for (NodeId c : g.children(knot))
        if (const Node* n = g.node(c); n && n->type().name == kSoftNetTangentType)
            out.push_back(c);
    return out;
}

/**
 * Copies knot k's pose to its partner p, mirrored in their rest frames, so a pose reads the same on both sides whatever the
 * body does; its tangent handles go to the partner's matching handles.
 */
void mirrorKnot(Graph& g, const NetView& v, int k, int p)
{
    const glm::dmat4 m = reflection(std::max(v.axis, 0));
    // Partner frame from source frame through the mirror: A takes the source's frame coordinates to the partner's.
    const glm::dmat4 a = glm::inverse(v.restFrame(p)) * m * v.restFrame(k);
    const glm::dmat4 ai = glm::inverse(a);
    const NodeId src = v.knots[size_t(k)], dst = v.knots[size_t(p)];
    g.evaluate();
    const glm::dmat4 c = g.node(src)->as<glm::dmat4>(Transform::kMatrix);
    Transform::setLocalMatrix(g, dst, a * c * ai);

    const NodeType* tt = types::find(kSoftNetTangentType);
    const int runAttr = tt->attrIndex("run"), endAttr = tt->attrIndex("end");
    const glm::dmat3 lin(a);
    for (NodeId t : tangentsOf(g, src))
    {
        const Node& tn = *g.node(t);
        const int64_t run = tn.as<int64_t>(runAttr), end = tn.as<int64_t>(endAttr);
        const glm::dvec3 restS = glm::dvec3(tn.as<glm::dmat4>(Transform::kOffsetParentMatrix)[3]);
        const glm::dvec3 want = glm::dvec3(a * glm::dvec4(restS, 1.0));
        // The partner's handle on the mirrored run end, else the one whose rest sits nearest the mirrored rest.
        NodeId match = kNoNode;
        double bestD = 1e300;
        for (NodeId pt : tangentsOf(g, dst))
        {
            const Node& pn = *g.node(pt);
            if (pn.as<int64_t>(runAttr) == (run ^ int64_t(kMirrored)) && pn.as<int64_t>(endAttr) == end)
            {
                match = pt;
                break;
            }
            if (const double d = glm::length(glm::dvec3(pn.as<glm::dmat4>(Transform::kOffsetParentMatrix)[3]) - want); d < bestD)
            {
                bestD = d;
                match = pt;
            }
        }
        if (match != kNoNode)
            g.setValue({match, Transform::kTranslate, -1}, lin * tn.as<glm::dvec3>(Transform::kTranslate));
    }
}

glm::dvec3 vec3Of(const Args& a, const char* flag)
{
    const json& j = a.flags[flag];
    if (!j.is_array() || j.size() != 3)
        throw CommandError(std::string("softNetPose: ") + flag + " takes [x, y, z]");
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

NodeId hubArg(Graph& g, const std::string& name, const char* cmd)
{
    const NodeId hub = netOf(g, nodeNamed(name, cmd).id());
    if (hub == kNoNode)
        throw CommandError(std::string(cmd) + ": " + name + " is not part of a Soft Net");
    return hub;
}

json mirrorKnots(Graph& g, const std::vector<NodeId>& knots)
{
    json out = json::array();
    std::map<NodeId, NetView> views;
    for (NodeId k : knots)
    {
        const NodeId hub = netOf(g, k);
        if (hub == kNoNode)
            continue;
        auto it = views.find(hub);
        if (it == views.end())
            it = views.emplace(hub, viewOf(g, hub)).first;
        const NetView& v = it->second;
        const auto idx = v.indexOf.find(k);
        if (idx == v.indexOf.end())
            continue;
        const int p = partnerOf(g, v, idx->second);
        if (p < 0)
            continue;
        mirrorKnot(g, v, idx->second, p);
        out.push_back(g.node(v.knots[size_t(p)])->path());
    }
    return out;
}

/** The knots a list of names means: knots themselves, a handle's knot, or every knot of a net named by its group or deformer. */
std::vector<NodeId> knotsFrom(Graph& g, const std::vector<std::string>& names, const char* cmd, int side = 0)
{
    std::vector<NodeId> out;
    for (const std::string& name : names)
    {
        Node& n = nodeNamed(name, cmd);
        if (n.type().name == kSoftNetKnotType)
            out.push_back(n.id());
        else if (n.type().name == kSoftNetTangentType)
            out.push_back(g.parent(n.id()));
        else if (const NodeId hub = netOf(g, n.id()); hub != kNoNode)
        {
            const NetView v = viewOf(g, hub);
            for (size_t k = 0; k < v.knots.size(); ++k)
                if (v.knots[k] != kNoNode && (side == 0 || v.axis < 0 || v.bind.knots[k].origin[v.axis] * side > 0.0))
                    out.push_back(v.knots[k]);
        }
        else
            throw CommandError(std::string(cmd) + ": " + name + " is not part of a Soft Net");
    }
    return out;
}
} // namespace

void registerSoftNetPoseCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "softNetKnots",
        .doc = "A Soft Net's knots: each with its junction, how many lines meet there, whether it shows by default (junctions and "
               "line ends), its partner across the mirror and its tangent handles. showAll shows or hides the knots inside lines. "
               "Returns [{knot, junction, degree, exposed, partner, tangents}].",
        .positionalDoc = "net (its knots group, a knot, the deformer or a mesh it binds)",
        .flags = {{"showAll", "sa", T::Bool, nullptr, "Show every knot (true) or junctions and line ends only (false)."},
                  {"toggleShowAll", "ts", T::Bool, false, "Flip showAll."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.objects().empty())
                throw CommandError("softNetKnots: give a Soft Net");
            const NodeId hub = hubArg(g, a.objects().front(), "softNetKnots");
            const int showAll = g.node(hub)->type().attrIndex("showAllKnots");
            if (a.has("showAll"))
                g.setValue({hub, showAll, -1}, a.get<bool>("showAll"));
            else if (a.get<bool>("toggleShowAll"))
                g.setValue({hub, showAll, -1}, !g.node(hub)->as<bool>(showAll));
            const NetView v = viewOf(g, hub);
            json out = json::array();
            for (size_t k = 0; k < v.knots.size(); ++k)
            {
                if (v.knots[k] == kNoNode)
                    continue;
                json tangents = json::array();
                for (NodeId t : tangentsOf(g, v.knots[k]))
                    tangents.push_back(g.node(t)->path());
                const int p = partnerOf(g, v, int(k));
                const int degree = v.bind.knots[k].degree;
                out.push_back({{"knot", g.node(v.knots[k])->path()},
                               {"junction", junctionOf(g, v.knots[k])},
                               {"degree", degree},
                               {"exposed", !g.source({v.knots[k], Transform::kVisibility, -1})},
                               {"partner", p >= 0 ? json(g.node(v.knots[size_t(p)])->path()) : json(nullptr)},
                               {"tangents", tangents}});
            }
            return out;
        },
        .examples = {"softNetKnots(\"SoftNet_knots\", showAll=True)"},
    });

    ops::registerCommand({
        .name = "mirrorSoftNetKnots",
        .doc = "Copy Soft Net knots' poses to their partners across the mirror (a mirrored layout's pairs, else the knot at the "
               "reflected place), mirrored in their frames, tangent handles included. Given the net, mirrors every knot on the side "
               "fromSide picks. Returns the knots moved.",
        .positionalDoc = "knots, or a net",
        .flags = {{"fromSide", "fs", T::Int, 1, "With a net: copy from the + (1) or - (-1) side of the mirror plane.", {}, {}, -1.0, 1.0}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.objects().empty())
                throw CommandError("mirrorSoftNetKnots: give knots or a net");
            const int side = a.get<int>("fromSide") < 0 ? -1 : 1;
            return mirrorKnots(g, knotsFrom(g, a.objects(), "mirrorSoftNetKnots", side));
        },
        .examples = {"mirrorSoftNetKnots(\"SoftNet_k12_l\")", "mirrorSoftNetKnots(\"SoftNet_knots\", fromSide=-1)"},
    });

    ops::registerCommand({
        .name = "softNetPose",
        .doc = "Pose Soft Net knots in their frames on the surface (z the normal): translate, rotate and scale set on each; "
               "mirror=True also gives each knot's partner the mirrored pose. Returns the knots posed.",
        .positionalDoc = "knots",
        .flags = {{"translate", "t", T::Vec3, nullptr, "Move in the knot's frame, cm."},
                  {"rotate", "ro", T::Vec3, nullptr, "Turn in the knot's frame, degrees."},
                  {"scale", "s", T::Vec3, nullptr, "Scale in the knot's frame."},
                  {"mirror", "m", T::Bool, false, "Pose the partners across the mirror too."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const std::vector<NodeId> knots = knotsFrom(g, a.objects(), "softNetPose");
            if (knots.empty())
                throw CommandError("softNetPose: give knots");
            json out = json::array();
            for (NodeId k : knots)
            {
                if (a.has("translate"))
                    g.setValue({k, Transform::kTranslate, -1}, vec3Of(a, "translate"));
                if (a.has("rotate"))
                    g.setValue({k, Transform::kRotate, -1}, vec3Of(a, "rotate"));
                if (a.has("scale"))
                    g.setValue({k, Transform::kScale, -1}, vec3Of(a, "scale"));
                out.push_back(g.node(k)->path());
            }
            if (a.get<bool>("mirror"))
                for (const json& p : mirrorKnots(g, knots))
                    out.push_back(p);
            return out;
        },
        .examples = {"softNetPose([\"SoftNet_k3_l\"], translate=[0, 0, 0.5], mirror=True)"},
    });
}
} // namespace lila::layout
