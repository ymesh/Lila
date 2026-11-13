#include "scene/AnimLayer.h"

#include "ops/Command.h"
#include "core/Profile.h"
#include "scene/Anim.h"
#include "scene/AnimCurve.h"
#include "scene/Graph.h"
#include "scene/Offsets.h"
#include "scene/RigUtil.h"
#include "scene/RotationCurve.h"
#include "scene/TransformOps.h"

#include <glm/ext/quaternion_exponential.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace lila::scene::anim
{
namespace
{
constexpr double kEps = 1e-9;
constexpr const char* kModeNames[] = {"additive", "override"};
constexpr const char* kAccumulationNames[] = {"quaternion", "euler"};

PlugRef whole(PlugRef p)
{
    p.component = -1;
    return p;
}

bool isVec(Graph& g, const PlugRef& p)
{
    return g.attr(whole(p)).type == PlugType::Vec3;
}

bool rotationAttr(const std::string& n)
{
    return n.starts_with("rotate") && n.find("Pivot") == std::string::npos;
}

bool numeric(PlugType t)
{
    return t == PlugType::Float || t == PlugType::Int || t == PlugType::Bool || t == PlugType::Enum || t == PlugType::Vec3;
}

glm::dvec3 vecOf(const PlugValue& v)
{
    if (const auto* x = std::get_if<glm::dvec3>(&v))
        return *x;
    return std::visit(
        [](const auto& s) -> glm::dvec3 {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_arithmetic_v<T>)
                return {double(s), 0.0, 0.0};
            else
                return glm::dvec3(0.0);
        },
        v);
}

PlugValue scalarAs(PlugType t, double v)
{
    switch (t)
    {
    case PlugType::Bool: return v != 0.0;
    case PlugType::Int:
    case PlugType::Enum: return int64_t(std::llround(v));
    default: return v;
    }
}

glm::dquat quatOf(const glm::dvec3& euler, RotateOrder order)
{
    return glm::normalize(glm::quat_cast(glm::dmat3(eulerToMatrix(euler, order))));
}

glm::dquat powQ(glm::dquat q, double e)
{
    if (q.w < 0.0)
        q = -q;
    return glm::normalize(glm::pow(q, e));
}

struct BlendInfo
{
    glm::dvec3 a{0.0}, b{0.0};
    double w = 1.0;
    LayerMode mode = LayerMode::Additive;
    Accumulation acc = Accumulation::Quaternion;
    bool rotation = false;
    RotateOrder order = RotateOrder::XYZ;
};

BlendInfo infoOf(const Node& n)
{
    BlendInfo i;
    i.a = vecOf(n.value(BlendNode::kInputA));
    i.b = vecOf(n.value(BlendNode::kInputB));
    i.w = n.as<double>(BlendNode::kWeight);
    i.mode = LayerMode(std::clamp<int64_t>(n.as<int64_t>(BlendNode::kMode), 0, 1));
    i.acc = Accumulation(std::clamp<int64_t>(n.as<int64_t>(BlendNode::kAccumulation), 0, 1));
    if (const auto* bn = dynamic_cast<const BlendNode*>(&n); bn && bn->rotation)
    {
        i.rotation = true;
        i.order = RotateOrder(std::clamp<int64_t>(n.as<int64_t>(BlendNode::kRotateOrder), 0, 5));
    }
    return i;
}

bool quaternionSpace(const BlendInfo& i)
{
    return i.rotation && i.acc == Accumulation::Quaternion;
}

/** inputB that makes the blend output o, given its inputA. */
glm::dvec3 solveB(const BlendInfo& i, const glm::dvec3& o)
{
    if (!quaternionSpace(i))
    {
        if (i.mode == LayerMode::Additive)
            return std::abs(i.w) > kEps ? (o - i.a) / i.w : o - i.a;
        return i.w > kEps ? (o - i.a * (1.0 - i.w)) / i.w : o;
    }
    glm::dquat qa = quatOf(i.a, i.order), qo = quatOf(o, i.order), qb;
    glm::dquat d = glm::inverse(qa) * qo;
    double e = std::abs(i.w) > kEps ? 1.0 / i.w : 1.0;
    qb = i.mode == LayerMode::Additive ? powQ(d, e) : qa * powQ(d, e);
    return xformops::closestEuler(glm::mat3_cast(qb), i.order, i.b);
}

/** inputA that makes the blend output o, given its inputB. A full override hides inputA, which then just takes o. */
glm::dvec3 solveA(const BlendInfo& i, const glm::dvec3& o)
{
    if (i.mode == LayerMode::Override && i.w >= 1.0 - kEps)
        return o;
    if (!quaternionSpace(i) || i.mode == LayerMode::Override)
        return i.mode == LayerMode::Additive ? o - i.b * i.w : (o - i.b * i.w) / (1.0 - i.w);
    glm::dquat layer = glm::slerp(glm::dquat(1, 0, 0, 0), quatOf(i.b, i.order), i.w);
    glm::dquat qa = quatOf(o, i.order) * glm::inverse(layer);
    return xformops::closestEuler(glm::mat3_cast(qa), i.order, i.a);
}

std::string safeName(std::string s)
{
    std::replace(s.begin(), s.end(), ':', '_');
    return s;
}

std::optional<PlugRef> linkSource(Graph& g, const PlugRef& p, bool vec)
{
    return g.source(vec ? PlugRef{p.node, p.attr, 0} : whole(p));
}

void disconnectAll(Graph& g, const PlugRef& p, bool vec)
{
    g.disconnect(whole(p));
    if (vec)
        for (int c = 0; c < 3; ++c)
            g.disconnect({p.node, p.attr, c});
}

void link(Graph& g, NodeId blend, const PlugRef& dst, bool vec)
{
    if (vec)
        for (int c = 0; c < 3; ++c)
            g.connect({blend, BlendNode::kOutput, c}, {dst.node, dst.attr, c});
    else
        g.connect({blend, BlendNode::kOutput, -1}, whole(dst));
}

/** Drivers of a plug: its whole source and, for a Vec3, each component's. */
struct Drivers
{
    std::optional<PlugRef> whole;
    std::array<std::optional<PlugRef>, 3> comp;

    bool driven(int c) const { return whole || (c >= 0 && comp[size_t(c)]); }
};

Drivers driversOf(Graph& g, const PlugRef& p, bool vec)
{
    Drivers d;
    d.whole = g.source(whole(p));
    if (vec)
        for (int c = 0; c < 3; ++c)
            d.comp[size_t(c)] = g.source({p.node, p.attr, c});
    return d;
}

void connectDrivers(Graph& g, const Drivers& d, const PlugRef& p, bool vec)
{
    if (d.whole)
        g.connect(*d.whole, whole(p));
    if (vec)
        for (int c = 0; c < 3; ++c)
            if (d.comp[size_t(c)])
                g.connect(*d.comp[size_t(c)], {p.node, p.attr, c});
}

/** Stores value on the parts of p nothing drives. Locked plugs keep theirs. */
void setStatic(Graph& g, const PlugRef& p, const glm::dvec3& value, const Drivers& d, bool vec)
{
    if (d.whole)
        return;
    try
    {
        if (vec)
        {
            for (int c = 0; c < 3; ++c)
                if (!d.comp[size_t(c)])
                    g.setValue({p.node, p.attr, c}, value[c]);
        }
        else
            g.setValue(whole(p), scalarAs(g.attr(p).type, value.x));
    }
    catch (const std::invalid_argument&)
    {
    }
}

double layerOrder(Graph& g, NodeId blend)
{
    NodeId l = layerOf(g, blend);
    return l ? double(g.node(l)->as<int64_t>(LayerNode::kOrder)) : -1e18;
}

/** Rewires member's chain to exactly bottomUp, keeping the base drivers and static value under it. */
void rebuildChain(Graph& g, const PlugRef& member, const std::vector<NodeId>& bottomUp)
{
    LILA_ZONE();

    const bool vec = isVec(g, member);
    std::vector<NodeId> old = blendChain(g, member);
    PlugRef base = old.empty() ? whole(member) : PlugRef{old.back(), BlendNode::kInputA, -1};
    Drivers drivers = driversOf(g, base, vec);
    glm::dvec3 baseStatic = vecOf(g.storedValue(base));

    disconnectAll(g, member, vec);
    for (NodeId b : old)
        disconnectAll(g, {b, BlendNode::kInputA, -1}, vec);

    PlugRef newBase = bottomUp.empty() ? whole(member) : PlugRef{bottomUp.front(), BlendNode::kInputA, -1};
    setStatic(g, newBase, baseStatic, drivers, vec);
    connectDrivers(g, drivers, newBase, vec);
    for (size_t i = 0; i + 1 < bottomUp.size(); ++i)
        link(g, bottomUp[i], {bottomUp[i + 1], BlendNode::kInputA, -1}, vec);
    if (!bottomUp.empty())
        link(g, bottomUp.back(), member, vec);
}

void sortBottomUp(Graph& g, std::vector<NodeId>& blends)
{
    std::stable_sort(blends.begin(), blends.end(), [&](NodeId a, NodeId b) { return layerOrder(g, a) < layerOrder(g, b); });
}

void resortChain(Graph& g, const PlugRef& member)
{
    std::vector<NodeId> chain = blendChain(g, member);
    std::reverse(chain.begin(), chain.end());
    std::vector<NodeId> sorted = chain;
    sortBottomUp(g, sorted);
    if (sorted != chain)
        rebuildChain(g, member, sorted);
}

NodeId createBlend(Graph& g, const PlugRef& member, NodeId layer, const std::string& suffix, const std::string& blendType = {})
{
    const AttrDef& a = g.attr(whole(member));
    const char* type = a.type == PlugType::Vec3 ? (rotationAttr(a.name) ? "animBlendRotation" : "animBlendVector") : "animBlendLinear";
    const NodeType* t = types::find(blendType.empty() ? std::string(type) : blendType);
    if (!t || !t->isA(type))
        throw std::invalid_argument(g.plugName(whole(member)) + " can't take a " + blendType + " blend");
    Node* owner = g.node(member.node);
    NodeId b = g.createNode(*t, safeName(owner->name() + "_" + a.name + "_" + suffix), kNoNode, false)->id();
    if (owner->type().isA("transform") && type == std::string("animBlendRotation"))
        g.setValue({b, BlendNode::kRotateOrder}, owner->value(Transform::kRotateOrder));
    if (layer)
    {
        g.connect({layer, LayerNode::kOutWeight, -1}, {b, BlendNode::kWeight, -1});
        g.connect({layer, LayerNode::kMode, -1}, {b, BlendNode::kMode, -1});
        g.connect({layer, LayerNode::kAccumulation, -1}, {b, BlendNode::kAccumulation, -1});
    }
    return b;
}

void deleteCurvesFeeding(Graph& g, const PlugRef& input, bool vec)
{
    Drivers d = driversOf(g, input, vec);
    std::set<NodeId> curves;
    auto consider = [&](const std::optional<PlugRef>& s) {
        if (s && g.node(s->node) && isAnyCurve(*g.node(s->node)))
            curves.insert(s->node);
    };
    consider(d.whole);
    for (const auto& c : d.comp)
        consider(c);
    const auto conns = g.connections();
    for (NodeId c : curves)
    {
        bool shared = std::any_of(conns.begin(), conns.end(), [&](const auto& e) { return e.second.node == c && e.first.node != input.node; });
        if (!shared)
            g.deleteNode(c);
    }
}

bool isTransformRotate(Graph& g, const PlugRef& member)
{
    Node* n = g.node(member.node);
    return n && n->type().isA("transform") && member.attr == Transform::kRotate;
}

/**
 * Whether keys on an input of member's chain go to a rotation curve: one drives it already, or nothing does and the
 * member's rotationInterpolation isn't Euler.
 */
bool takesRotationKeys(Graph& g, const PlugRef& member, const Drivers& d)
{
    if (!isTransformRotate(g, member))
        return false;
    if (d.whole)
        return isRotationCurve(*g.node(d.whole->node));
    if (std::any_of(d.comp.begin(), d.comp.end(), [](const auto& c) { return c.has_value(); }))
        return false;
    return RotationInterpolation(g.node(member.node)->as<int64_t>(Transform::kRotationInterpolation)) != RotationInterpolation::Euler;
}

/** The input's rotation curve, made and connected when it has none, edited by fn. Returns it. */
NodeId keyRotation(Graph& g, const PlugRef& input, const PlugRef& member, const Drivers& d, const std::string& name,
                   const std::function<void(RotationCurve&)>& fn)
{
    Node& m = *g.node(member.node);
    auto order = RotateOrder(m.as<int64_t>(Transform::kRotateOrder));
    NodeId curve = d.whole ? d.whole->node : kNoNode;
    RotationCurve rc;
    if (curve)
        rc = rotationCurveOf(*g.node(curve));
    else
    {
        curve = g.createNode(*types::find("animCurveRQ"), name, kNoNode, false)->id();
        auto mode = RotationInterpolation(m.as<int64_t>(Transform::kRotationInterpolation));
        rc.interp = mode == RotationInterpolation::Slerp ? RotInterp::Slerp : RotInterp::Smooth;
        rc.order = order;
    }
    rc.reorder(order);
    fn(rc);
    setRotationCurve(g, curve, rc);
    if (!d.whole)
        g.connect({curve, RotationCurveNode::kOutput, -1}, whole(input));
    return curve;
}

/** An unconnected copy of a curve node (scalar or rotation), every stored setting included. */
NodeId copyCurve(Graph& g, NodeId id, const std::string& name)
{
    Node& n = *g.node(id);
    NodeId c = g.createNode(n.type(), name, kNoNode, false)->id();
    const auto& attrs = n.type().attrs;
    for (int i = 0; i < int(attrs.size()); ++i)
        if ((attrs[i].flags & AttrInput) && (attrs[i].flags & AttrStorable) && !(g.node(c)->value(i) == n.value(i)))
            g.setValue({c, i}, n.value(i));
    return c;
}
} // namespace

void removeBlend(Graph& g, NodeId blend, bool deleteInputs)
{
    const bool vec = g.node(blend)->type().attrs[BlendNode::kOutput].type == PlugType::Vec3;
    PlugRef member = memberOf(g, blend);
    if (member.valid())
    {
        std::vector<NodeId> chain = blendChain(g, member);
        std::reverse(chain.begin(), chain.end());
        std::erase(chain, blend);
        rebuildChain(g, member, chain);
    }
    if (deleteInputs)
        deleteCurvesFeeding(g, {blend, BlendNode::kInputB, -1}, vec);
    g.deleteNode(blend);
}

namespace
{
void moveInputs(Graph& g, const PlugRef& from, const PlugRef& to, bool vec)
{
    Drivers d = driversOf(g, from, vec);
    glm::dvec3 v = vecOf(g.storedValue(whole(from)));
    disconnectAll(g, from, vec);
    setStatic(g, to, v, {}, vec);
    connectDrivers(g, d, to, vec);
}

/** Every curve feeding the plug (whole or per component). */
std::vector<NodeId> curvesFeeding(Graph& g, const PlugRef& p, bool vec)
{
    std::vector<NodeId> out;
    Drivers d = driversOf(g, p, vec);
    auto add = [&](const std::optional<PlugRef>& s) {
        if (s && isAnyCurve(*g.node(s->node)) && std::find(out.begin(), out.end(), s->node) == out.end())
            out.push_back(s->node);
    };
    add(d.whole);
    for (const auto& c : d.comp)
        add(c);
    return out;
}

void updateSolo(Graph& g)
{
    std::vector<NodeId> all = layers(g);
    bool any = std::any_of(all.begin(), all.end(), [&](NodeId l) { return g.node(l)->as<bool>(LayerNode::kSolo); });
    for (NodeId l : all)
    {
        bool want = any && !g.node(l)->as<bool>(LayerNode::kSolo);
        if (g.node(l)->as<bool>(LayerNode::kSoloMuted) != want)
            g.setValue({l, LayerNode::kSoloMuted}, want);
    }
}

void renumber(Graph& g, const std::vector<NodeId>& bottomUp)
{
    for (size_t i = 0; i < bottomUp.size(); ++i)
        if (g.node(bottomUp[i])->as<int64_t>(LayerNode::kOrder) != int64_t(i))
            g.setValue({bottomUp[i], LayerNode::kOrder}, int64_t(i));
}

/** Members of every layer, for re-sorting chains after the stack order changes. */
std::vector<PlugRef> allMembers(Graph& g)
{
    std::vector<PlugRef> out;
    for (NodeId l : layers(g))
        for (const PlugRef& m : layerMembers(g, l))
            if (std::find(out.begin(), out.end(), m) == out.end())
                out.push_back(m);
    return out;
}

void placeLayer(Graph& g, NodeId layer, size_t index)
{
    std::vector<NodeId> stack = layers(g);
    std::erase(stack, layer);
    stack.insert(stack.begin() + std::ptrdiff_t(std::min(index, stack.size())), layer);
    renumber(g, stack);
    for (const PlugRef& m : allMembers(g))
        resortChain(g, m);
}

size_t indexOf(Graph& g, NodeId layer)
{
    std::vector<NodeId> stack = layers(g);
    return size_t(std::find(stack.begin(), stack.end(), layer) - stack.begin());
}
} // namespace

void BlendNode::compute(const EvalContext&)
{
    LILA_ZONE();

    BlendInfo i = infoOf(*this);
    glm::dvec3 out;
    if (quaternionSpace(i))
        out = blendRotation(i.a, i.b, i.w, i.mode, i.acc, i.order);
    else
        out = i.mode == LayerMode::Additive ? i.a + i.b * i.w : i.a + (i.b - i.a) * i.w;
    if (type().attrs[kOutput].type == PlugType::Vec3)
        setOutput(kOutput, out);
    else
        setOutput(kOutput, out.x);
}

void LayerNode::compute(const EvalContext&)
{
    bool off = as<bool>(kMute) || as<bool>(kSoloMuted);
    setOutput(kOutWeight, off ? 0.0 : as<double>(kWeight));
}

bool isBlend(const Node& n)
{
    return n.type().isA("animBlend");
}

bool isLayer(const Node& n)
{
    return n.type().isA("animLayer");
}

glm::dvec3 blendRotation(const glm::dvec3& a, const glm::dvec3& b, double w, LayerMode mode, Accumulation acc, RotateOrder order)
{
    const glm::dvec3 linear = mode == LayerMode::Additive ? a + b * w : a + (b - a) * w;
    if (acc == Accumulation::Euler || w == 0.0)
        return w == 0.0 ? a : linear;
    if (mode == LayerMode::Override && w == 1.0)
        return b;
    glm::dquat qa = quatOf(a, order), qb = quatOf(b, order);
    glm::dquat q = mode == LayerMode::Additive ? qa * glm::slerp(glm::dquat(1, 0, 0, 0), qb, w) : glm::slerp(qa, qb, w);
    return xformops::closestEuler(glm::mat3_cast(q), order, linear);
}

std::vector<NodeId> layers(Graph& g)
{
    std::vector<NodeId> out;
    for (NodeId id : g.nodes())
        if (isLayer(*g.node(id)))
            out.push_back(id);
    std::stable_sort(out.begin(), out.end(), [&](NodeId a, NodeId b) {
        int64_t oa = g.node(a)->as<int64_t>(LayerNode::kOrder), ob = g.node(b)->as<int64_t>(LayerNode::kOrder);
        return oa != ob ? oa < ob : a < b;
    });
    return out;
}

NodeId activeLayer(Graph& g)
{
    // Asked for on every curve lookup of a layered channel; the scan is cached until the graph changes.
    static const Graph* cachedGraph = nullptr;
    static uint64_t cachedVersion = 0;
    static NodeId cached = kNoNode;
    if (cachedGraph == &g && cachedVersion == g.version() && (!cached || g.node(cached)))
        return cached;
    cached = kNoNode;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); isLayer(*n) && n->as<bool>(LayerNode::kActive))
        {
            cached = id;
            break;
        }
    cachedGraph = &g;
    cachedVersion = g.version();
    return cached;
}

NodeId layerOf(Graph& g, NodeId blend)
{
    auto src = g.source({blend, BlendNode::kWeight, -1});
    return src && isLayer(*g.node(src->node)) ? src->node : kNoNode;
}

std::vector<NodeId> blendChain(Graph& g, const PlugRef& member)
{
    std::vector<NodeId> out;
    Node* n = g.node(member.node);
    if (!n || member.attr < 0)
        return out;
    const bool vec = isVec(g, member);
    PlugRef cur = whole(member);
    while (out.size() < 256)
    {
        auto src = linkSource(g, cur, vec);
        if (!src || src->attr != BlendNode::kOutput || !isBlend(*g.node(src->node)))
            break;
        out.push_back(src->node);
        cur = {src->node, BlendNode::kInputA, -1};
    }
    return out;
}

PlugRef memberOf(Graph& g, NodeId blend)
{
    NodeId cur = blend;
    for (int guard = 0; guard < 256; ++guard)
    {
        const bool vec = g.node(cur)->type().attrs[BlendNode::kOutput].type == PlugType::Vec3;
        std::vector<PlugRef> dsts = g.destinations({cur, BlendNode::kOutput, vec ? 0 : -1});
        if (dsts.empty())
            return {};
        const PlugRef& d = dsts.front();
        if (isBlend(*g.node(d.node)) && d.attr == BlendNode::kInputA)
        {
            cur = d.node;
            continue;
        }
        return whole(d);
    }
    return {};
}

std::vector<PlugRef> layerMembers(Graph& g, NodeId layer)
{
    std::vector<PlugRef> out;
    for (const PlugRef& d : g.destinations({layer, LayerNode::kOutWeight, -1}))
        if (isBlend(*g.node(d.node)))
            if (PlugRef m = memberOf(g, d.node); m.valid() && std::find(out.begin(), out.end(), m) == out.end())
                out.push_back(m);
    return out;
}

NodeId blendFor(Graph& g, const PlugRef& member, NodeId layer)
{
    for (NodeId b : blendChain(g, member))
        if (layerOf(g, b) == layer)
            return b;
    return kNoNode;
}

NodeId createLayer(Graph& g, const std::string& name, LayerMode mode)
{
    LILA_ZONE();

    std::vector<NodeId> stack = layers(g);
    Node* n = g.createNode(*types::find("animLayer"), name.empty() ? "AnimLayer1" : name, kNoNode, true);
    g.setValue({n->id(), LayerNode::kMode}, int64_t(mode));
    g.setValue({n->id(), LayerNode::kOrder}, int64_t(stack.size()));
    updateSolo(g);
    return n->id();
}

NodeId addMember(Graph& g, NodeId layer, const PlugRef& memberIn) { return addMember(g, layer, memberIn, std::string()); }

NodeId addMember(Graph& g, NodeId layer, const PlugRef& memberIn, const std::string& blendType)
{
    LILA_ZONE();

    PlugRef member = whole(memberIn);
    if (NodeId b = blendFor(g, member, layer))
        return b;
    const AttrDef& a = g.attr(member);
    if (!numeric(a.type) || !(a.flags & AttrInput))
        throw std::invalid_argument(g.plugName(member) + " can't be layered");
    const bool vec = a.type == PlugType::Vec3;
    g.evaluate();
    glm::dvec3 now = vecOf(g.storedValue(member));
    NodeId b = createBlend(g, member, layer, g.node(layer)->name(), blendType);
    auto mode = LayerMode(g.node(layer)->as<int64_t>(LayerNode::kMode));
    // An empty layer changes nothing: additive starts at zero, override at the current value.
    setStatic(g, {b, BlendNode::kInputB, -1}, mode == LayerMode::Additive ? glm::dvec3(0.0) : now, {}, vec);
    std::vector<NodeId> chain = blendChain(g, member);
    std::reverse(chain.begin(), chain.end());
    std::vector<NodeId> next = chain;
    next.push_back(b);
    sortBottomUp(g, next);
    rebuildChain(g, member, next);
    return b;
}

void removeMember(Graph& g, NodeId layer, const PlugRef& member)
{
    if (NodeId b = blendFor(g, whole(member), layer))
        removeBlend(g, b, true);
}

NodeId insertConstraintBlend(Graph& g, const PlugRef& memberIn, const PlugRef& driver)
{
    LILA_ZONE();

    PlugRef member = whole(memberIn);
    const bool vec = isVec(g, member);
    NodeId b = createBlend(g, member, kNoNode, "constraintBlend");
    g.setValue({b, BlendNode::kMode}, int64_t(LayerMode::Override));
    std::vector<NodeId> chain = blendChain(g, member);
    std::reverse(chain.begin(), chain.end());
    chain.insert(chain.begin(), b);
    rebuildChain(g, member, chain);
    disconnectAll(g, {b, BlendNode::kInputB, -1}, vec);
    g.connect(driver, {b, BlendNode::kInputB, -1});
    return b;
}

namespace
{
KeyRouter& router()
{
    static KeyRouter r;
    return r;
}
AutoKeyListener& autoKeyListener()
{
    static AutoKeyListener l;
    return l;
}

/** The router's route for member, else the active layer's. */
KeyRoute routeFor(Graph& g, const PlugRef& member, bool write)
{
    if (router())
        if (std::optional<KeyRoute> r = router()(g, member, write))
            return *r;
    return {activeLayer(g), false};
}
} // namespace

void setKeyRouter(KeyRouter r) { router() = std::move(r); }

NodeId keyLayer(Graph& g, const PlugRef& member) { return routeFor(g, whole(member), false).layer; }

NodeId routeKey(Graph& g, const PlugRef& plug) { return routeFor(g, whole(plug), true).layer; }

bool editSetsStatic(Graph& g, const PlugRef& plug)
{
    if (!router())
        return false;
    const PlugRef member = whole(plug);
    const KeyRoute r = routeFor(g, member, false);
    if (!r.staticValue)
        return false;
    // No layer or blend yet: the write makes them, with a static input.
    if (!r.layer)
        return true;
    const NodeId b = blendFor(g, member, r.layer);
    return !b || curvesFeeding(g, {b, BlendNode::kInputB, -1}, isVec(g, member)).empty();
}

void setAutoKeyListener(AutoKeyListener l) { autoKeyListener() = std::move(l); }

void notifyAutoKey(Graph& g, const PlugRef& plug, double time)
{
    if (autoKeyListener())
        autoKeyListener()(g, plug, time);
}

PlugRef keyInput(Graph& g, const PlugRef& plug)
{
    Node* n = g.node(plug.node);
    if (!n || plug.attr < 0)
        return plug;
    const bool vec = isVec(g, plug);
    // Cheap exit for the common case: nothing blends this plug.
    auto src = linkSource(g, plug, vec);
    if (!src || src->attr != BlendNode::kOutput || !isBlend(*g.node(src->node)))
        return plug;
    std::vector<NodeId> chain = blendChain(g, plug);
    if (NodeId layer = routeFor(g, whole(plug), false).layer)
        if (NodeId b = blendFor(g, whole(plug), layer))
            return {b, BlendNode::kInputB, plug.component};
    return {chain.back(), BlendNode::kInputA, plug.component};
}

namespace
{
/** The open KeyBatch's members and the values the keys so far give them. */
struct KeyBatchState
{
    int depth = 0;
    std::vector<std::pair<PlugRef, glm::dvec3>> wanted;
};
thread_local KeyBatchState tBatch;
} // namespace

KeyBatch::KeyBatch() { ++tBatch.depth; }
KeyBatch::~KeyBatch()
{
    if (--tBatch.depth == 0)
        tBatch.wanted.clear();
}

std::optional<KeyRedirect> redirectKey(Graph& g, const PlugRef& plug, double value)
{
    LILA_ZONE();

    PlugRef member = whole(plug);
    std::vector<NodeId> chain = blendChain(g, member);
    const KeyRoute route = routeFor(g, member, true);
    NodeId layer = route.layer;
    if (chain.empty() && !layer)
        return std::nullopt;

    NodeId target = kNoNode;
    int input = BlendNode::kInputA;
    std::string suffix;
    bool added = false;
    if (layer)
    {
        Node& l = *g.node(layer);
        if (l.as<bool>(LayerNode::kLock))
            throw std::invalid_argument("animation layer " + l.path() + " is locked");
        target = blendFor(g, member, layer);
        if (!target)
        {
            target = addMember(g, layer, member);
            chain = blendChain(g, member);
            added = true;
        }
        input = BlendNode::kInputB;
        suffix = "_" + l.name();
    }
    else
        target = chain.back();

    auto pending = std::find_if(tBatch.wanted.begin(), tBatch.wanted.end(), [&](const auto& w) { return w.first == member; });
    // The member's own chain is all a key reads; meshes skinned to it needn't be recomputed for every key.
    if (tBatch.depth == 0 || added)
        g.evaluateUpstream({member.node});
    const int comp = plug.component >= 0 ? plug.component : 0;
    glm::dvec3 o = tBatch.depth > 0 && pending != tBatch.wanted.end() ? pending->second : vecOf(g.storedValue(member));
    o[comp] = value;
    if (tBatch.depth > 0)
    {
        if (pending != tBatch.wanted.end())
            pending->second = o;
        else
            tBatch.wanted.push_back({member, o});
    }
    glm::dvec3 result = o;
    for (NodeId b : chain)
    {
        BlendInfo info = infoOf(*g.node(b));
        const auto* bn = dynamic_cast<const BlendNode*>(g.node(b));
        if (b == target)
        {
            std::optional<glm::dvec3> own = !bn ? std::nullopt : input == BlendNode::kInputB ? bn->solveInputB(o) : bn->solveInputA(o);
            result = own ? *own : input == BlendNode::kInputB ? solveB(info, o) : solveA(info, o);
            break;
        }
        std::optional<glm::dvec3> own = bn ? bn->solveInputA(o) : std::nullopt;
        o = own ? *own : solveA(info, o);
    }
    KeyRedirect r;
    r.plug = {target, input, plug.component};
    r.value = result[comp];
    r.whole = result;
    r.curveName = safeName(g.node(member.node)->name() + "_" + channelName(g, plug) + suffix);
    r.staticValue = route.staticValue && layer && curvesFeeding(g, whole(r.plug), isVec(g, member)).empty();
    return r;
}

std::optional<NodeId> setLayeredRotationKey(Graph& g, const PlugRef& plug, double time, double value, std::optional<Tangent> in,
                                            std::optional<Tangent> out)
{
    LILA_ZONE();

    PlugRef member = whole(plug);
    if (plug.component < 0 || !isTransformRotate(g, member) || (blendChain(g, member).empty() && !keyLayer(g, member)))
        return std::nullopt;
    auto r = redirectKey(g, plug, value);
    if (!r)
        return std::nullopt;
    PlugRef input = whole(r->plug);
    const auto* bn = dynamic_cast<const BlendNode*>(g.node(input.node));
    if ((bn && bn->staticInputB() && input.attr == BlendNode::kInputB) || r->staticValue)
    {
        g.setValue(input, r->whole);
        return input.node;
    }
    Drivers d = driversOf(g, input, true);
    if (!takesRotationKeys(g, member, d))
        return std::nullopt;
    NodeId layer = input.attr == BlendNode::kInputB ? layerOf(g, input.node) : kNoNode;
    std::string name = safeName(g.node(member.node)->name() + "_" + g.attr(member).name + (layer ? "_" + g.node(layer)->name() : ""));
    return keyRotation(g, input, member, d, name, [&](RotationCurve& rc) {
        size_t i = rc.insert(rc.makeKey(time, r->whole));
        if (in)
            rc.keys[i].in = *in;
        if (out)
            rc.keys[i].out = *out;
    });
}

namespace
{
using rigutil::guarded;

NodeId layerNamed(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n || !isLayer(*n))
        throw CommandError(std::string(cmd) + ": no animation layer named '" + name + "'");
    return n->id();
}

/** The layer flag, else the active layer. kNoNode means the base animation. */
NodeId layerArg(Graph& g, const Args& a, const char* cmd)
{
    if (!a.has("layer"))
        return activeLayer(g);
    std::string name = a.get<std::string>("layer");
    return name == kBaseLayerName ? kNoNode : layerNamed(g, name, cmd);
}

/** Whole attributes from nodes (every keyable one) or plugs. */
std::vector<PlugRef> memberArgs(Graph& g, const std::vector<std::string>& objs, const char* cmd)
{
    std::vector<PlugRef> out;
    auto add = [&](PlugRef p) {
        p = whole(p);
        if (std::find(out.begin(), out.end(), p) == out.end())
            out.push_back(p);
    };
    for (const std::string& o : objs)
    {
        if (o.find('.') != std::string::npos)
        {
            add(guarded([&] { return g.plug(o); }));
            continue;
        }
        Node* n = g.find(o);
        if (!n)
            throw CommandError(std::string(cmd) + ": no node named '" + o + "'");
        if (isLayer(*n) || isBlend(*n) || isAnyCurve(*n))
            continue;
        for (const PlugRef& p : keyablePlugs(g, n->id()))
            add(p);
    }
    return out;
}

json layerJson(Graph& g, NodeId l)
{
    Node& n = *g.node(l);
    json members = json::array();
    for (const PlugRef& m : layerMembers(g, l))
        members.push_back(g.plugName(m));
    return {{"name", n.path()},
            {"weight", n.as<double>(LayerNode::kWeight)},
            {"mute", n.as<bool>(LayerNode::kMute)},
            {"solo", n.as<bool>(LayerNode::kSolo)},
            {"lock", n.as<bool>(LayerNode::kLock)},
            {"mode", kModeNames[std::clamp<int64_t>(n.as<int64_t>(LayerNode::kMode), 0, 1)]},
            {"accumulation", kAccumulationNames[std::clamp<int64_t>(n.as<int64_t>(LayerNode::kAccumulation), 0, 1)]},
            {"active", n.as<bool>(LayerNode::kActive)},
            {"members", members}};
}

int64_t modeArg(const std::string& s, const char* cmd)
{
    if (s == "additive")
        return 0;
    if (s == "override")
        return 1;
    throw CommandError(std::string(cmd) + ": mode is additive or override");
}

int64_t accumulationArg(const std::string& s, const char* cmd)
{
    if (s == "quaternion")
        return 0;
    if (s == "euler")
        return 1;
    throw CommandError(std::string(cmd) + ": accumulation is quaternion or euler");
}

void setActive(Graph& g, NodeId layer)
{
    for (NodeId l : layers(g))
        if (g.node(l)->as<bool>(LayerNode::kActive) != (l == layer))
            g.setValue({l, LayerNode::kActive}, l == layer);
}

void deleteLayer(Graph& g, NodeId l)
{
    for (const PlugRef& m : layerMembers(g, l))
        removeMember(g, l, m);
    // Blends of parked takes still hang off the layer; they go with it.
    for (const PlugRef& d : g.destinations({l, LayerNode::kOutWeight, -1}))
        if (g.node(d.node) && isBlend(*g.node(d.node)))
        {
            deleteCurvesFeeding(g, {d.node, BlendNode::kInputB, -1}, isVec(g, {d.node, BlendNode::kInputB, -1}));
            g.deleteNode(d.node);
        }
    deleteCurvesFeeding(g, {l, LayerNode::kWeight, -1}, false);
    g.deleteNode(l);
    updateSolo(g);
}

/** Key times of the curves feeding p, appended to out. */
void keyTimesOf(Graph& g, const PlugRef& p, bool vec, std::set<double>& out)
{
    for (NodeId c : curvesFeeding(g, p, vec))
        for (double t : curveKeyTimes(*g.node(c)))
            out.insert(t);
}

/**
 * Writes samples onto input (a whole plug) as curves, one per component, replacing their keys; a single sample with
 * nothing animated becomes a static value.
 */
void writeSamples(Graph& g, const PlugRef& input, const PlugRef& member, const std::vector<double>& times,
                  const std::vector<glm::dvec3>& values, const std::string& suffix)
{
    const bool vec = isVec(g, member);
    Drivers d = driversOf(g, input, vec);
    bool animated = times.size() > 1;
    if (animated && takesRotationKeys(g, member, d))
    {
        std::string name = safeName(g.node(member.node)->name() + "_" + g.attr(member).name + suffix);
        keyRotation(g, input, member, d, name, [&](RotationCurve& rc) {
            rc.keys.clear();
            for (size_t i = 0; i < times.size(); ++i)
                rc.insert(rc.makeKey(times[i], values[i]));
        });
        return;
    }
    // A whole-Vec3 driver that isn't a rotation curve can't take per-component keys; component curves replace it.
    if (d.whole && vec)
    {
        g.disconnect(whole(input));
        d.whole.reset();
    }
    const AttrDef& ma = g.attr(member);
    for (int c = vec ? 0 : -1; c < (vec ? 3 : 0); ++c)
    {
        PlugRef p{input.node, input.attr, c};
        std::optional<PlugRef> src = vec ? d.comp[size_t(c)] : d.whole;
        NodeId curve = src && isCurve(*g.node(src->node)) ? src->node : kNoNode;
        const int k = std::max(c, 0);
        if (!curve && !animated)
        {
            if (!src)
                g.setValue(p, vec ? PlugValue(values[0][k]) : scalarAs(g.attr(p).type, values[0][k]));
            continue;
        }
        if (!curve)
        {
            if (src)
                g.disconnect(p);
            std::string name = safeName(g.node(member.node)->name() + "_" + (vec ? ma.childNames[size_t(k)] : ma.name) + suffix);
            curve = g.createNode(*types::find(curveTypeFor(ma)), name, kNoNode, false)->id();
            g.connect({curve, CurveNode::kOutput, -1}, p);
        }
        Curve cv = curveOf(*g.node(curve));
        cv.keys.clear();
        for (size_t i = 0; i < times.size(); ++i)
            cv.insert({.time = times[i], .value = values[i][k]});
        setCurve(g, curve, cv);
    }
}

json mergeLayers(Graph& g, const std::vector<NodeId>& named)
{
    LILA_ZONE();

    std::vector<NodeId> stack = layers(g);
    size_t lo = stack.size(), hi = 0;
    for (NodeId l : named)
    {
        size_t i = indexOf(g, l);
        lo = std::min(lo, i);
        hi = std::max(hi, i);
    }
    std::vector<NodeId> merged(stack.begin() + std::ptrdiff_t(lo), stack.begin() + std::ptrdiff_t(hi + 1));
    const NodeId target = lo > 0 ? stack[lo - 1] : kNoNode;
    auto isMerged = [&](NodeId b) { return std::find(merged.begin(), merged.end(), layerOf(g, b)) != merged.end(); };

    struct Job
    {
        PlugRef member;
        NodeId top = kNoNode; // highest merged blend
        NodeId lowest = kNoNode; // lowest merged blend
        NodeId below = kNoNode; // the unmerged blend under lowest, if any
        std::vector<glm::dvec3> values;
    };
    std::vector<Job> jobs;
    std::set<double> times;
    for (NodeId l : merged)
    {
        keyTimesOf(g, {l, LayerNode::kWeight, -1}, false, times);
        for (const PlugRef& m : layerMembers(g, l))
        {
            if (std::any_of(jobs.begin(), jobs.end(), [&](const Job& j) { return j.member == m; }))
                continue;
            Job job{m};
            const bool vec = isVec(g, m);
            std::vector<NodeId> chain = blendChain(g, m);
            for (size_t i = 0; i < chain.size(); ++i)
            {
                if (!isMerged(chain[i]))
                    continue;
                if (!job.top)
                    job.top = chain[i];
                job.lowest = chain[i];
                job.below = i + 1 < chain.size() ? chain[i + 1] : kNoNode;
                keyTimesOf(g, {chain[i], BlendNode::kInputB, -1}, vec, times);
                if (int env = g.node(chain[i])->type().attrIndex("envelope"); env >= 0)
                    keyTimesOf(g, {chain[i], env, -1}, false, times);
            }
            keyTimesOf(g, {chain.back(), BlendNode::kInputA, -1}, vec, times);
            if (job.below)
                keyTimesOf(g, {job.below, BlendNode::kInputB, -1}, vec, times);
            jobs.push_back(std::move(job));
        }
    }

    // Every frame across the keys, so the merged curves match the layered result frame for frame.
    std::vector<double> samples;
    const double saved = g.time();
    if (times.empty())
        samples.push_back(saved);
    else
    {
        double fps = playback().fps;
        long long f0 = std::llround(std::ceil(*times.begin() * fps - 1e-6)), f1 = std::llround(std::floor(*times.rbegin() * fps + 1e-6));
        for (long long f = f0; f <= f1; ++f)
            times.insert(double(f) / fps);
        std::vector<double> sorted(times.begin(), times.end());
        for (double t : sorted)
            if (samples.empty() || t - samples.back() > 1e-6)
                samples.push_back(t);
    }

    const bool targetIsLayer = target != kNoNode;
    for (double t : samples)
    {
        g.setTime(t);
        g.evaluate();
        for (Job& job : jobs)
        {
            glm::dvec3 o = vecOf(g.node(job.top)->value(BlendNode::kOutput));
            BlendInfo low = infoOf(*g.node(job.lowest));
            glm::dvec3 v;
            if (targetIsLayer)
            {
                BlendInfo ti;
                if (job.below && layerOf(g, job.below) == target)
                    ti = infoOf(*g.node(job.below));
                else
                {
                    // No blend on the target yet: one is made with the target's settings over what is below now.
                    ti = low;
                    Node& tn = *g.node(target);
                    ti.w = tn.as<double>(LayerNode::kOutWeight);
                    ti.mode = LayerMode(tn.as<int64_t>(LayerNode::kMode));
                    ti.acc = Accumulation(tn.as<int64_t>(LayerNode::kAccumulation));
                    ti.b = glm::dvec3(0.0);
                }
                v = solveB(ti, o);
            }
            else if (job.below)
                v = solveA(infoOf(*g.node(job.below)), o);
            else
                v = o;
            job.values.push_back(v);
        }
    }
    g.setTime(saved);

    for (Job& job : jobs)
    {
        for (NodeId b : blendChain(g, job.member))
            if (isMerged(b))
                removeBlend(g, b, true);
        PlugRef input;
        std::string suffix;
        if (targetIsLayer)
        {
            input = {addMember(g, target, job.member), BlendNode::kInputB, -1};
            suffix = "_" + g.node(target)->name();
        }
        else
        {
            std::vector<NodeId> chain = blendChain(g, job.member);
            input = chain.empty() ? job.member : PlugRef{chain.back(), BlendNode::kInputA, -1};
        }
        writeSamples(g, input, job.member, samples, job.values, suffix);
    }
    const bool activeMerged = std::find(merged.begin(), merged.end(), activeLayer(g)) != merged.end();
    for (NodeId l : merged)
        deleteLayer(g, l);
    if (activeMerged)
        setActive(g, target);
    g.evaluate();
    return targetIsLayer ? json(g.node(target)->path()) : json(kBaseLayerName);
}

NodeId duplicateLayer(Graph& g, NodeId src, const std::string& name)
{
    Node& s = *g.node(src);
    NodeId l = createLayer(g, name.empty() ? s.name() + "Copy" : name, LayerMode(s.as<int64_t>(LayerNode::kMode)));
    for (int attr : {int(LayerNode::kWeight), int(LayerNode::kAccumulation), int(LayerNode::kMute)})
        g.setValue({l, attr}, g.node(src)->value(attr));
    placeLayer(g, l, indexOf(g, src) + 1);
    for (const PlugRef& m : layerMembers(g, src))
    {
        NodeId from = blendFor(g, m, src);
        const bool vec = isVec(g, m);
        NodeId to = addMember(g, l, m);
        PlugRef in{from, BlendNode::kInputB, -1}, out{to, BlendNode::kInputB, -1};
        Drivers d = driversOf(g, in, vec);
        setStatic(g, out, vecOf(g.storedValue(in)), {}, vec);
        auto copy = [&](const std::optional<PlugRef>& s, const PlugRef& dst) {
            if (!s)
                return;
            PlugRef srcPlug = *s;
            if (Node* c = g.node(s->node); c && isAnyCurve(*c))
                srcPlug.node = copyCurve(g, s->node, c->name() + "_" + g.node(l)->name());
            g.connect(srcPlug, dst);
        };
        copy(d.whole, out);
        if (vec)
            for (int c = 0; c < 3; ++c)
                copy(d.comp[size_t(c)], {to, BlendNode::kInputB, c});
    }
    return l;
}

json extractLayer(Graph& g, NodeId src, const std::vector<PlugRef>& members, const std::string& name)
{
    NodeId l;
    if (src)
    {
        Node& s = *g.node(src);
        l = createLayer(g, name.empty() ? s.name() + "Extract" : name, LayerMode(s.as<int64_t>(LayerNode::kMode)));
        for (int attr : {int(LayerNode::kWeight), int(LayerNode::kAccumulation)})
            g.setValue({l, attr}, g.node(src)->value(attr));
        placeLayer(g, l, indexOf(g, src) + 1);
    }
    else
    {
        l = createLayer(g, name.empty() ? "BaseExtract" : name, LayerMode::Override);
        placeLayer(g, l, 0);
    }
    int moved = 0;
    for (const PlugRef& m : members)
    {
        const bool vec = isVec(g, m);
        if (src)
        {
            NodeId from = blendFor(g, m, src);
            if (!from)
                continue;
            NodeId to = addMember(g, l, m);
            moveInputs(g, {from, BlendNode::kInputB, -1}, {to, BlendNode::kInputB, -1}, vec);
            removeBlend(g, from, false);
            ++moved;
            continue;
        }
        std::vector<NodeId> chain = blendChain(g, m);
        if (!chain.empty() && !layerOf(g, chain.back()))
            continue; // constrained: an override layer under it would hide the constraint
        PlugRef base = chain.empty() ? m : PlugRef{chain.back(), BlendNode::kInputA, -1};
        if (curvesFeeding(g, base, vec).empty())
            continue;
        NodeId to = addMember(g, l, m);
        moveInputs(g, {to, BlendNode::kInputA, -1}, {to, BlendNode::kInputB, -1}, vec);
        ++moved;
    }
    if (moved == 0)
    {
        deleteLayer(g, l);
        throw CommandError("extractAnimLayer: none of those channels are animated on that layer");
    }
    return g.node(l)->path();
}
} // namespace

void registerLayerTypes()
{
    if (types::find("animBlend"))
        return;
    std::vector<std::string> modes(std::begin(kModeNames), std::end(kModeNames));
    std::vector<std::string> accs(std::begin(kAccumulationNames), std::end(kAccumulationNames));
    const uint32_t stored = AttrInput | AttrStorable;
    auto vec3 = [](std::string name, uint32_t flags, std::string doc) {
        AttrDef a{.name = name, .type = PlugType::Vec3, .defaultValue = glm::dvec3(0.0), .flags = flags, .doc = std::move(doc)};
        for (int i = 0; i < 3; ++i)
            a.childNames[i] = name + char('X' + i);
        return a;
    };

    NodeType base;
    base.name = "animBlend";
    base.icon = "stack-2";
    base.doc = "Blends one animation layer (inputB) over everything below it (inputA) for one channel.";
    base.ownAttrs = {
        {.name = "weight", .shortName = "w", .type = PlugType::Float, .defaultValue = 1.0, .flags = stored | AttrKeyable,
         .doc = "How much of inputB; driven by the layer, or keyable on a constraint blend.", .minValue = 0.0, .maxValue = 1.0},
        {.name = "mode", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = stored, .enumNames = modes,
         .doc = "additive: A + B * weight. override: A blended toward B by weight."},
        {.name = "accumulation", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = stored, .enumNames = accs,
         .doc = "Rotation blends: quaternion (no gimbal flips) or per-axis Euler."},
    };
    const NodeType& b = types::registerType(std::move(base));
    for (int kind = 0; kind < 3; ++kind)
    {
        NodeType t;
        t.name = kind == 0 ? "animBlendLinear" : kind == 1 ? "animBlendVector" : "animBlendRotation";
        t.base = &b;
        if (kind == 0)
            t.ownAttrs = {
                {.name = "inputA", .type = PlugType::Float, .defaultValue = 0.0, .flags = stored, .doc = "Everything below this layer."},
                {.name = "inputB", .type = PlugType::Float, .defaultValue = 0.0, .flags = stored, .doc = "This layer's value."},
                {.name = "output", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrOutput, .doc = "The blended value."},
            };
        else
            t.ownAttrs = {
                vec3("inputA", stored, "Everything below this layer."),
                vec3("inputB", stored, "This layer's value."),
                vec3("output", AttrOutput, "The blended value."),
            };
        if (kind == 2)
            t.ownAttrs.push_back({.name = "rotateOrder", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = stored,
                                  .enumNames = {"xyz", "yzx", "zxy", "xzy", "yxz", "zyx"}, .doc = "The member's rotate order."});
        t.create = [kind] {
            auto n = std::make_unique<BlendNode>();
            n->rotation = kind == 2;
            return n;
        };
        types::registerType(std::move(t));
    }

    NodeType layer;
    layer.name = "animLayer";
    layer.icon = "stack-2";
    layer.doc = "An animation layer: keys on its members blend over the layers below, by weight.";
    layer.ownAttrs = {
        {.name = "weight", .shortName = "w", .type = PlugType::Float, .defaultValue = 1.0, .flags = stored | AttrKeyable,
         .doc = "How much of the layer applies.", .minValue = 0.0, .maxValue = 1.0},
        {.name = "mute", .type = PlugType::Bool, .defaultValue = false, .flags = stored, .doc = "Turn the layer off."},
        {.name = "solo", .type = PlugType::Bool, .defaultValue = false, .flags = stored, .doc = "Mute every layer that isn't soloed."},
        {.name = "lock", .type = PlugType::Bool, .defaultValue = false, .flags = stored, .doc = "Refuse keys and membership changes."},
        {.name = "mode", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = stored, .enumNames = modes,
         .doc = "additive adds on top of the layers below; override replaces them by weight."},
        {.name = "rotationAccumulation", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = stored, .enumNames = accs,
         .doc = "Blend rotations as quaternions or per-axis Euler."},
        {.name = "order", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = stored | AttrHidden, .doc = "Position in the stack, 0 at the bottom."},
        {.name = "active", .type = PlugType::Bool, .defaultValue = false, .flags = stored | AttrHidden, .doc = "Keys go to this layer."},
        {.name = "soloMuted", .type = PlugType::Bool, .defaultValue = false, .flags = stored | AttrHidden,
         .doc = "Another layer is soloed."},
        {.name = "outWeight", .type = PlugType::Float, .defaultValue = 1.0, .flags = AttrOutput,
         .doc = "Weight after mute and solo; drives the layer's blends."},
    };
    layer.create = [] { return std::make_unique<LayerNode>(); };
    types::registerType(std::move(layer));
    offsets::registerTypes();
}

void registerLayerCommands()
{
    offsets::registerCommands();
    using T = ArgType;
    const FlagDef layerFlag{"layer", "l", T::String, nullptr, "Animation layer. Default: the active one."};

    ops::registerCommand({
        .name = "createAnimLayer",
        .doc = "Make an animation layer at the top of the stack, with the given objects or plugs (default the selection) "
               "as members, and make it active. Returns its name.",
        .positionalDoc = "members",
        .flags = {
            {"name", "n", T::String, nullptr, "Layer name."},
            {"mode", "m", T::String, "additive", "additive | override", {"additive", "override"}},
            {"accumulation", "acc", T::String, "quaternion", "Rotation blending: quaternion | euler", {"quaternion", "euler"}},
            {"empty", "e", T::Bool, false, "Don't add the selection."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            NodeId l = createLayer(g, a.get<std::string>("name", "AnimLayer1"), LayerMode(modeArg(a.get<std::string>("mode"), "createAnimLayer")));
            g.setValue({l, LayerNode::kAccumulation}, accumulationArg(a.get<std::string>("accumulation"), "createAnimLayer"));
            if (!a.get<bool>("empty"))
                for (const PlugRef& m : memberArgs(g, a.objects(), "createAnimLayer"))
                    guarded([&] { return addMember(g, l, m); });
            setActive(g, l);
            return g.node(l)->path();
        },
    });

    ops::registerCommand({
        .name = "deleteAnimLayer",
        .doc = "Delete animation layers with their curves; their members lose what the layers added.",
        .positionalDoc = "layers",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.empty())
                throw CommandError("deleteAnimLayer: name the layers");
            for (const std::string& name : a.objects())
                deleteLayer(g, layerNamed(g, name, "deleteAnimLayer"));
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "addAnimLayerMembers",
        .doc = "Add objects (every keyable attribute) or plugs to a layer. Returns how many attributes were added.",
        .positionalDoc = "objects or plugs",
        .flags = {layerFlag},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            NodeId l = layerArg(g, a, "addAnimLayerMembers");
            if (!l)
                throw CommandError("addAnimLayerMembers: the base animation has no members; name a layer");
            if (g.node(l)->as<bool>(LayerNode::kLock))
                throw CommandError("addAnimLayerMembers: " + g.node(l)->path() + " is locked");
            int added = 0;
            for (const PlugRef& m : memberArgs(g, a.objects(), "addAnimLayerMembers"))
                if (!blendFor(g, m, l))
                {
                    guarded([&] { return addMember(g, l, m); });
                    ++added;
                }
            return added;
        },
    });

    ops::registerCommand({
        .name = "removeAnimLayerMembers",
        .doc = "Take objects or plugs off a layer, deleting what the layer keyed on them. Returns how many went.",
        .positionalDoc = "objects or plugs",
        .flags = {layerFlag},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            NodeId l = layerArg(g, a, "removeAnimLayerMembers");
            if (!l)
                throw CommandError("removeAnimLayerMembers: name a layer");
            if (g.node(l)->as<bool>(LayerNode::kLock))
                throw CommandError("removeAnimLayerMembers: " + g.node(l)->path() + " is locked");
            int removed = 0;
            for (const PlugRef& m : memberArgs(g, a.objects(), "removeAnimLayerMembers"))
                if (blendFor(g, m, l))
                {
                    removeMember(g, l, m);
                    ++removed;
                }
            return removed;
        },
    });

    ops::registerCommand({
        .name = "mergeAnimLayers",
        .doc = "Bake layers (and any between them) down into the layer under the lowest, or the base animation: the "
               "result is sampled every frame across their keys, so it evaluates the same. Returns the layer merged into.",
        .positionalDoc = "layers",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.empty())
                throw CommandError("mergeAnimLayers: name the layers");
            std::vector<NodeId> named;
            for (const std::string& n : a.objects())
                named.push_back(layerNamed(g, n, "mergeAnimLayers"));
            return guarded([&] { return mergeLayers(g, named); });
        },
    });

    ops::registerCommand({
        .name = "extractAnimLayer",
        .doc = "Move the animation of objects or plugs from a layer (or the base animation) to a new layer just above "
               "it, leaving the result unchanged. Returns the new layer.",
        .positionalDoc = "objects or plugs",
        .flags = {layerFlag, {"name", "n", T::String, nullptr, "Name of the new layer."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            NodeId src = layerArg(g, a, "extractAnimLayer");
            if (src && g.node(src)->as<bool>(LayerNode::kLock))
                throw CommandError("extractAnimLayer: " + g.node(src)->path() + " is locked");
            return guarded([&] { return extractLayer(g, src, memberArgs(g, a.objects(), "extractAnimLayer"), a.get<std::string>("name")); });
        },
    });

    ops::registerCommand({
        .name = "duplicateAnimLayer",
        .doc = "Copy a layer, its settings and its curves, just above it. Returns the copy.",
        .positionalDoc = "layer",
        .flags = {{"name", "n", T::String, nullptr, "Name of the copy."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.size() != 1)
                throw CommandError("duplicateAnimLayer: name one layer");
            NodeId src = layerNamed(g, a.positional[0].get<std::string>(), "duplicateAnimLayer");
            return g.node(guarded([&] { return duplicateLayer(g, src, a.get<std::string>("name")); }))->path();
        },
    });

    ops::registerCommand({
        .name = "reorderAnimLayer",
        .doc = "Move a layer in the stack: index (0 is just above the base animation) or by (+1 up, -1 down). Returns its new index.",
        .positionalDoc = "layer",
        .flags = {{"index", "i", T::Int, nullptr, "New position.", {}, {}, 0.0}, {"by", "b", T::Int, nullptr, "Steps up (positive) or down."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.size() != 1)
                throw CommandError("reorderAnimLayer: name one layer");
            NodeId l = layerNamed(g, a.positional[0].get<std::string>(), "reorderAnimLayer");
            int64_t now = int64_t(indexOf(g, l));
            int64_t to = a.has("index") ? a.get<int64_t>("index") : now + a.get<int64_t>("by", 0);
            to = std::clamp<int64_t>(to, 0, int64_t(layers(g).size()) - 1);
            if (to != now)
                guarded([&] { placeLayer(g, l, size_t(to)); return 0; });
            return to;
        },
    });

    ops::registerCommand({
        .name = "setActiveAnimLayer",
        .doc = "Choose the layer keys go to; BaseAnimation (or no argument) for the base animation.",
        .positionalDoc = "layer",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::string name = a.positional.empty() ? kBaseLayerName : a.positional[0].get<std::string>();
            setActive(g, name == kBaseLayerName ? kNoNode : layerNamed(g, name, "setActiveAnimLayer"));
            return name;
        },
    });

    ops::registerCommand({
        .name = "editAnimLayer",
        .doc = "Set a layer's weight, mute, solo, lock, mode or rotation accumulation. Returns its settings.",
        .positionalDoc = "layer",
        .flags = {
            {"weight", "w", T::Float, nullptr, "", {}, {}, 0.0, 1.0},
            {"mute", "mu", T::Bool, nullptr, ""},
            {"solo", "so", T::Bool, nullptr, "Mutes every other layer that isn't soloed."},
            {"lock", "lk", T::Bool, nullptr, "No keys or membership changes."},
            {"mode", "m", T::String, nullptr, "additive | override", {"additive", "override"}},
            {"accumulation", "acc", T::String, nullptr, "quaternion | euler", {"quaternion", "euler"}},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.size() != 1)
                throw CommandError("editAnimLayer: name one layer");
            NodeId l = layerNamed(g, a.positional[0].get<std::string>(), "editAnimLayer");
            guarded([&] {
                if (a.has("weight"))
                    g.setValue({l, LayerNode::kWeight}, a.get<double>("weight"));
                for (auto [flag, attr] : {std::pair{"mute", LayerNode::kMute}, std::pair{"solo", LayerNode::kSolo}, std::pair{"lock", LayerNode::kLock}})
                    if (a.has(flag))
                        g.setValue({l, attr}, a.get<bool>(flag));
                if (a.has("mode"))
                    g.setValue({l, LayerNode::kMode}, modeArg(a.get<std::string>("mode"), "editAnimLayer"));
                if (a.has("accumulation"))
                    g.setValue({l, LayerNode::kAccumulation}, accumulationArg(a.get<std::string>("accumulation"), "editAnimLayer"));
                return 0;
            });
            if (a.has("solo"))
                updateSolo(g);
            return layerJson(g, l);
        },
    });

    ops::registerCommand({
        .name = "listAnimLayers",
        .doc = "The layer stack from the bottom: BaseAnimation first, then each layer's settings and members.",
        .undoable = false,
        .run = [](const Args&) -> json {
            Graph& g = Graph::current();
            json out = json::array();
            out.push_back({{"name", kBaseLayerName}, {"base", true}, {"active", activeLayer(g) == kNoNode}});
            for (NodeId l : layers(g))
                out.push_back(layerJson(g, l));
            return out;
        },
    });
}
} // namespace lila::scene::anim
