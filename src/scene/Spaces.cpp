#include "scene/Spaces.h"

#include "ops/Command.h"
#include "core/Profile.h"
#include "scene/Anim.h"
#include "scene/AnimLayer.h"
#include "scene/Graph.h"
#include "scene/RotationCurve.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <sstream>

namespace lila::scene::spaces
{
namespace
{
constexpr double kEps = 1e-6;

std::vector<std::string> words(const std::string& s)
{
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string w; in >> w;)
        out.push_back(w);
    return out;
}

std::string joined(const std::vector<std::string>& w)
{
    std::string out;
    for (const std::string& s : w)
        out += (out.empty() ? "" : " ") + s;
    return out;
}

Kind kindOf(const std::string& w) { return w == "world" ? Kind::World : w == "node" ? Kind::Node : Kind::Parent; }

std::vector<std::string> kindsOf(const Graph& g, NodeId sw) { return words(g.node(sw)->as<std::string>(SwitchNode::kKinds)); }

double frameStep()
{
    const double fps = anim::playback().fps;
    return 1.0 / (fps > 0.0 ? fps : 30.0);
}

/** Evaluates at other times, then puts the graph's time back. */
struct TimeScope
{
    Graph& g;
    double saved;
    explicit TimeScope(Graph& graph) : g(graph), saved(graph.time()) {}
    ~TimeScope()
    {
        g.setTime(saved);
        g.evaluate();
    }
    void at(double t)
    {
        g.setTime(t);
        g.evaluate();
    }
};

glm::dmat4 worldOf(Graph& g, NodeId id)
{
    g.evaluate();
    return g.node(id)->as<glm::dmat4>(Transform::kWorldMatrix);
}

glm::dmat4 parentWorldOf(Graph& g, NodeId id)
{
    NodeId p = g.parent(id);
    return p == kNoNode ? glm::dmat4(1.0) : worldOf(g, p);
}

glm::dvec3 vecOf(Graph& g, NodeId id, int attr) { return std::get<glm::dvec3>(g.value({id, attr, -1})); }

int spaceValue(Graph& g, NodeId id)
{
    g.evaluate();
    return int(std::get<int64_t>(g.value({id, Transform::kSpace, -1})));
}

struct Trs
{
    glm::dvec3 t{0.0}, r{0.0}, s{1.0};
};

/** translate/rotate/scale whose local matrix (with the node's pivots, orient and axis) is local; rotate nearest ref. */
Trs valuesFor(const Node& n, const glm::dmat4& local, const glm::dvec3& ref)
{
    glm::dvec3 scale, translation, skew;
    glm::dquat q;
    glm::dvec4 perspective;
    glm::decompose(local, scale, q, translation, skew, perspective);
    const auto& xf = static_cast<const Transform&>(n);
    const auto order = RotateOrder(n.as<int64_t>(Transform::kRotateOrder));
    const glm::dmat4 pre = xf.preRotation();
    const glm::dmat4 axis = xf.rotateAxisMatrix();
    Trs v;
    v.r = eulerNear(glm::transpose(glm::dmat3(pre)) * glm::mat3_cast(q) * glm::transpose(glm::dmat3(axis)), order, ref);
    v.s = scale;
    auto tr = [](const glm::dvec3& p) { return glm::translate(glm::dmat4(1.0), p); };
    auto at = [&](int a) { return n.as<glm::dvec3>(a); };
    const glm::dvec3 rp = at(Transform::kRotatePivot), sp = at(Transform::kScalePivot);
    // Everything after translate in the pivot model; translate is what puts its origin on local's.
    const glm::dmat4 rest = tr(at(Transform::kRotatePivotTranslate)) * tr(rp) * pre * eulerToMatrix(v.r, order) * axis * tr(-rp) *
                            tr(at(Transform::kScalePivotTranslate)) * tr(sp) * glm::scale(glm::dmat4(1.0), v.s) * tr(-sp);
    v.t = glm::dvec3(local[3]) - glm::dvec3(rest[3]);
    return v;
}

Trs currentValues(Graph& g, NodeId id)
{
    g.evaluate();
    return {vecOf(g, id, Transform::kTranslate), vecOf(g, id, Transform::kRotate), vecOf(g, id, Transform::kScale)};
}

bool driven(const Graph& g, NodeId id, int attr)
{
    if (g.source({id, attr, -1}))
        return true;
    if (g.node(id)->type().attrs[size_t(attr)].type != PlugType::Vec3)
        return false;
    for (int c = 0; c < 3; ++c)
        if (g.source({id, attr, c}))
            return true;
    return false;
}

bool animated(const Graph& g, NodeId id)
{
    for (int a : {int(Transform::kTranslate), int(Transform::kRotate), int(Transform::kScale), int(Transform::kSpace)})
        if (driven(g, id, a))
            return true;
    return false;
}

bool sameRotation(const glm::dvec3& a, const glm::dvec3& b, RotateOrder order)
{
    const glm::dmat3 ma(eulerToMatrix(a, order)), mb(eulerToMatrix(b, order));
    double err = 0.0;
    for (int c = 0; c < 3; ++c)
        err += glm::length(ma[c] - mb[c]);
    return err < 1e-9;
}

/** Keys a whole Vec3 channel at the graph's current time so it evaluates to v, through layers and rotation curves. */
void keyVec(Graph& g, NodeId id, int attr, const glm::dvec3& v)
{
    const double t = g.time();
    const PlugRef whole{id, attr, -1};
    const auto order = RotateOrder(g.node(id)->as<int64_t>(Transform::kRotateOrder));
    if (attr == Transform::kRotate && anim::keysAsRotation(g, whole) && anim::blendChain(g, whole).empty() && !anim::keyLayer(g, whole))
    {
        anim::setRotationKey(g, whole, t, v);
        return;
    }
    // Layer conversions of one component read the others' current values, so a pass or two more settles a rotation.
    for (int pass = 0; pass < 4; ++pass)
    {
        for (int c = 0; c < 3; ++c)
            anim::setKey(g, {id, attr, c}, t, v[c]);
        g.evaluate();
        const glm::dvec3 now = std::get<glm::dvec3>(g.value(whole));
        if (attr == Transform::kRotate ? sameRotation(now, v, order) : glm::length(now - v) < 1e-9)
            return;
    }
}

void keySpace(Graph& g, NodeId id, int index)
{
    anim::setKey(g, {id, Transform::kSpace, -1}, g.time(), double(index), anim::Tangent::Step, anim::Tangent::Step);
}

void setVec(Graph& g, NodeId id, int attr, const glm::dvec3& v)
{
    const PlugRef p{id, attr, -1};
    g.setValue(p, g.keepLocked(p, v));
}

/** Keys (or sets) translate, rotate and, when it moved or is animated, scale. */
void apply(Graph& g, NodeId id, const Trs& v, bool key)
{
    const Trs now = currentValues(g, id);
    const bool scale = glm::length(now.s - v.s) > 1e-9 || driven(g, id, Transform::kScale);
    if (!key)
    {
        setVec(g, id, Transform::kTranslate, v.t);
        setVec(g, id, Transform::kRotate, v.r);
        if (scale)
            setVec(g, id, Transform::kScale, v.s);
        return;
    }
    keyVec(g, id, Transform::kTranslate, v.t);
    keyVec(g, id, Transform::kRotate, v.r);
    if (scale)
        keyVec(g, id, Transform::kScale, v.s);
}

/** The values that put the node's world matrix at world in its current frame. */
Trs valuesAt(Graph& g, NodeId id, const glm::dmat4& world)
{
    g.evaluate();
    const Node& n = *g.node(id);
    return valuesFor(n, glm::inverse(n.as<glm::dmat4>(Transform::kParentMatrix)) * world, n.as<glm::dvec3>(Transform::kRotate));
}

/** The curve the space channel's keys go to (the active layer's, else the base). */
NodeId spaceCurve(Graph& g, NodeId id) { return anim::curveFor(g, {id, Transform::kSpace, -1}); }

/** Times of the node's transform and space keys in [t0, t1]. */
std::vector<double> keyTimesIn(Graph& g, NodeId id, double t0, double t1)
{
    std::vector<double> out;
    for (double t : anim::keyTimes(g, {id}))
        if (t >= t0 - kEps && t <= t1 + kEps)
            out.push_back(t);
    return out;
}

/** True when evaluating `from` reads `on`: through parents, connections or another space. */
bool dependsOn(const Graph& g, NodeId from, NodeId on)
{
    std::multimap<NodeId, NodeId> sources;
    for (const auto& [dst, src] : g.connections())
        sources.emplace(dst.node, src.node);
    std::vector<NodeId> stack{from};
    std::vector<NodeId> seen;
    while (!stack.empty())
    {
        NodeId cur = stack.back();
        stack.pop_back();
        if (cur == on)
            return true;
        if (cur == kNoNode || std::find(seen.begin(), seen.end(), cur) != seen.end())
            continue;
        seen.push_back(cur);
        stack.push_back(g.parent(cur));
        auto [b, e] = sources.equal_range(cur);
        for (auto it = b; it != e; ++it)
            stack.push_back(it->second);
    }
    return false;
}

NodeId createSwitch(Graph& g, NodeId id)
{
    Node* sw = g.createNode(*types::find(kSwitchType), g.node(id)->name() + "_spaceSwitch", kNoNode, false);
    g.setValue({sw->id(), SwitchNode::kKinds, -1}, std::string("parent"));
    g.connect({sw->id(), SwitchNode::kFrames, -1}, {id, Transform::kSpaceFrames, -1});
    return sw->id();
}

/** Adds shift to the space curve's keys whose value is above `above`. */
void remapKeys(Graph& g, NodeId curve, int above, int shift)
{
    if (!curve || !anim::isCurve(*g.node(curve)))
        return;
    anim::Curve c = anim::curveOf(*g.node(curve));
    bool changed = false;
    for (anim::Key& k : c.keys)
        if (int(std::lround(k.value)) > above)
        {
            k.value += shift;
            changed = true;
        }
    if (changed)
        anim::setCurve(g, curve, c);
}
} // namespace

json Frames::toJson() const
{
    json f = json::array();
    for (size_t i = 0; i < frames.size(); ++i)
        f.push_back(parent[i] ? json("parent") : scene::toJson(PlugValue(frames[i])));
    return {{"kind", kFramesKind}, {"frames", f}};
}

void SwitchNode::compute(const EvalContext&)
{
    auto f = std::make_shared<Frames>();
    const std::vector<std::string> kinds = words(as<std::string>(kKinds));
    for (int i = 0; i < int(kinds.size()); ++i)
    {
        const Kind k = kindOf(kinds[size_t(i)]);
        const glm::dmat4& offset = elementOr<glm::dmat4>(kOffset, i);
        f->parent.push_back(k == Kind::Parent);
        f->frames.push_back(k == Kind::Node ? elementOr<glm::dmat4>(kTarget, i) * offset : k == Kind::World ? offset : glm::dmat4(1.0));
    }
    setOutput(kFrames, DataPtr(std::move(f)));
}

const glm::dmat4* activeFrame(const Node& n)
{
    if (n.type().attrs.size() <= size_t(Transform::kSpaceFrames))
        return nullptr;
    const DataPtr* d = std::get_if<DataPtr>(&n.value(Transform::kSpaceFrames));
    if (!d || !*d || std::strcmp((*d)->kind(), kFramesKind) != 0)
        return nullptr;
    const auto* f = static_cast<const Frames*>(d->get());
    const int64_t* i = std::get_if<int64_t>(&n.value(Transform::kSpace));
    if (!i || *i <= 0 || *i >= int64_t(f->frames.size()) || f->parent[size_t(*i)])
        return nullptr;
    return &f->frames[size_t(*i)];
}

bool isSwitch(const Node& n) { return n.type().isA(kSwitchType); }

NodeId switchOf(const Graph& g, NodeId node)
{
    const Node* n = g.node(node);
    if (!n || !n->type().isA("transform"))
        return kNoNode;
    auto s = g.source({node, Transform::kSpaceFrames, -1});
    return s && g.node(s->node) && isSwitch(*g.node(s->node)) ? s->node : kNoNode;
}

std::vector<Space> list(const Graph& g, NodeId node)
{
    std::vector<Space> out{{0, "parent", Kind::Parent, g.parent(node)}};
    const NodeId sw = switchOf(g, node);
    if (!sw)
        return out;
    const std::vector<std::string> kinds = kindsOf(g, sw);
    for (int i = 1; i < int(kinds.size()); ++i)
    {
        Space s{i, kinds[size_t(i)], kindOf(kinds[size_t(i)]), kNoNode};
        if (s.kind == Kind::Node)
        {
            auto src = g.source({sw, SwitchNode::kTarget, -1, i});
            s.target = src ? src->node : kNoNode;
            s.name = s.target ? g.node(s.target)->name() : std::string();
        }
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<std::string> names(const Graph& g, NodeId node)
{
    std::vector<std::string> out;
    for (const Space& s : list(g, node))
        out.push_back(s.name.empty() ? "(missing)" : s.name);
    return out;
}

int active(Graph& g, NodeId node)
{
    const int n = int(list(g, node).size());
    return std::clamp(spaceValue(g, node), 0, n - 1);
}

int find(const Graph& g, NodeId node, const std::string& space)
{
    const std::vector<Space> all = list(g, node);
    for (const Space& s : all)
        if (!s.name.empty() && s.name == space)
            return s.index;
    const std::vector<NodeId> want = g.findAll(space);
    for (const Space& s : all)
        if (want.size() == 1 && s.target == want[0] && s.kind == Kind::Node)
            return s.index;
    if (space == "local")
        return 0;
    if (!space.empty() && std::all_of(space.begin(), space.end(), [](char c) { return c >= '0' && c <= '9'; }))
    {
        const int i = std::stoi(space);
        return i < int(all.size()) ? i : -1;
    }
    return -1;
}

int addSpace(Graph& g, NodeId id, const std::string& space, bool maintainOffset)
{
    LILA_ZONE();

    Node* n = g.node(id);
    if (!n || !n->type().isA("transform"))
        throw std::invalid_argument("spaces go on transforms and joints");
    if (space == "parent" || space == "local")
        return 0;
    NodeId target = kNoNode;
    if (space != "world")
    {
        Node* t = g.find(space);
        if (!t || !t->type().isA("transform"))
            throw std::invalid_argument("no transform named '" + space + "'");
        target = t->id();
        if (target == g.parent(id))
            return 0;
        if (target == id || dependsOn(g, target, id))
            throw std::invalid_argument(t->path() + " depends on " + n->path() + ", so it can't be its space");
    }
    if (int existing = find(g, id, space); existing >= 0 && (target || list(g, id)[size_t(existing)].kind == Kind::World))
        return existing;

    NodeId sw = switchOf(g, id);
    if (!sw)
        sw = createSwitch(g, id);
    std::vector<std::string> kinds = kindsOf(g, sw);
    const int slot = int(kinds.size());

    const glm::dmat4 parentWorld = parentWorldOf(g, id);
    glm::dmat4 offset(1.0);
    if (maintainOffset)
        offset = target ? glm::inverse(worldOf(g, target)) * parentWorld : parentWorld;
    g.setValue({sw, SwitchNode::kOffset, -1, slot}, offset);
    if (target)
        g.connect({target, Transform::kWorldMatrix, -1}, {sw, SwitchNode::kTarget, -1, slot});
    kinds.push_back(target ? "node" : "world");
    g.setValue({sw, SwitchNode::kKinds, -1}, joined(kinds));
    return slot;
}

void removeSpace(Graph& g, NodeId id, int index)
{
    LILA_ZONE();

    const NodeId sw = switchOf(g, id);
    std::vector<std::string> kinds = sw ? kindsOf(g, sw) : std::vector<std::string>{};
    if (!sw || index <= 0 || index >= int(kinds.size()))
        throw std::invalid_argument("no space " + std::to_string(index) + " to remove (the parent stays)");

    // Every stretch spent in the space goes back to the parent, keeping the world pose.
    if (NodeId curve = spaceCurve(g, id); curve && anim::isCurve(*g.node(curve)))
    {
        for (int guard = 0; guard < 10000; ++guard)
        {
            curve = spaceCurve(g, id);
            if (!curve)
                break;
            const anim::Curve c = anim::curveOf(*g.node(curve));
            auto it = std::find_if(c.keys.begin(), c.keys.end(), [&](const anim::Key& k) { return int(std::lround(k.value)) == index; });
            if (it == c.keys.end())
                break;
            const double start = it->time;
            double end = std::max(anim::playback().maxTime, start);
            for (double t : anim::keyTimes(g, {id}))
                end = std::max(end, t);
            if (auto next = std::next(it); next != c.keys.end())
                end = next->time - frameStep();
            switchSpace(g, id, 0, {.range = std::make_pair(start, std::max(start, end)), .hold = false});
        }
        remapKeys(g, spaceCurve(g, id), index, -1);
    }
    else if (!driven(g, id, Transform::kSpace))
    {
        const int now = spaceValue(g, id);
        if (now == index)
            switchSpace(g, id, 0, {.key = false});
        else if (now > index)
            g.setValue({id, Transform::kSpace, -1}, int64_t(now - 1));
    }

    const int count = int(kinds.size());
    std::vector<std::optional<PlugRef>> targets;
    std::vector<glm::dmat4> offsets;
    for (int i = 0; i < count; ++i)
    {
        targets.push_back(g.source({sw, SwitchNode::kTarget, -1, i}));
        offsets.push_back(g.node(sw)->elementOr<glm::dmat4>(SwitchNode::kOffset, i));
    }
    for (int i = index; i < count; ++i)
        if (targets[size_t(i)])
            g.disconnect({sw, SwitchNode::kTarget, -1, i});
    for (int i = index; i + 1 < count; ++i)
    {
        g.setValue({sw, SwitchNode::kOffset, -1, i}, offsets[size_t(i + 1)]);
        if (targets[size_t(i + 1)])
            g.connect(*targets[size_t(i + 1)], {sw, SwitchNode::kTarget, -1, i});
    }
    g.removeElement({sw, SwitchNode::kOffset, -1, count - 1});
    kinds.erase(kinds.begin() + index);
    g.setValue({sw, SwitchNode::kKinds, -1}, joined(kinds));

    if (kinds.size() <= 1)
    {
        if (NodeId curve = spaceCurve(g, id); curve && anim::blendChain(g, {id, Transform::kSpace, -1}).empty())
            g.deleteNode(curve);
        g.deleteNode(sw);
        if (!driven(g, id, Transform::kSpace))
            g.setValue({id, Transform::kSpace, -1}, int64_t(0));
    }
}

void switchSpace(Graph& g, NodeId id, int index, const SwitchOptions& opts)
{
    LILA_ZONE();

    Node* n = g.node(id);
    const int count = int(list(g, id).size());
    if (!n || index < 0 || index >= count)
        throw std::invalid_argument("no space " + std::to_string(index));
    const bool key = opts.key.value_or(animated(g, id) || opts.range.has_value());

    TimeScope scope(g);
    if (!key)
    {
        const glm::dmat4 world = worldOf(g, id);
        g.setValue({id, Transform::kSpace, -1}, int64_t(index));
        if (opts.preserve)
            apply(g, id, valuesAt(g, id, world), false);
        return;
    }

    const double step = frameStep();
    const double a = opts.range ? std::min(opts.range->first, opts.range->second) : opts.time.value_or(g.time());
    const double b = opts.range ? std::max(opts.range->first, opts.range->second) : a;

    std::vector<double> times;
    if (opts.range && opts.bake)
        for (double t = a; t <= b + kEps; t += step)
            times.push_back(t);
    else if (opts.range)
    {
        times = keyTimesIn(g, id, a, b);
        times.push_back(a);
        times.push_back(b);
    }
    else
        times.push_back(a);
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end(), [](double x, double y) { return std::abs(x - y) <= kEps; }), times.end());

    // Everything the switch must keep is read before the first key changes it.
    std::vector<glm::dmat4> worlds;
    for (double t : times)
    {
        scope.at(t);
        worlds.push_back(worldOf(g, id));
    }
    struct Hold
    {
        double time;
        Trs values;
        int space;
    };
    std::vector<Hold> holds;
    if (opts.hold)
    {
        scope.at(a - step);
        if (spaceValue(g, id) != index)
            holds.push_back({a - step, currentValues(g, id), spaceValue(g, id)});
        if (opts.range)
        {
            scope.at(b + step);
            if (spaceValue(g, id) != index)
                holds.push_back({b + step, currentValues(g, id), spaceValue(g, id)});
        }
    }

    for (const Hold& h : holds)
    {
        scope.at(h.time);
        keySpace(g, id, h.space);
        apply(g, id, h.values, true);
    }
    if (NodeId curve = spaceCurve(g, id); curve && b > a)
        anim::removeKeys(g, curve, a + kEps, b + kEps);
    scope.at(a);
    keySpace(g, id, index);
    if (!opts.preserve)
        return;
    for (size_t i = 0; i < times.size(); ++i)
    {
        scope.at(times[i]);
        apply(g, id, valuesAt(g, id, worlds[i]), true);
    }
}

void registerTypes()
{
    if (types::find(kSwitchType))
        return;
    NodeType t;
    t.name = kSwitchType;
    t.icon = "world";
    t.doc = "A transform's spaces (addSpace): one world frame per space for the transform's space channel to pick from.";
    t.ownAttrs = {
        {.name = "kinds", .type = PlugType::String, .defaultValue = std::string("parent"), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "One word per space: parent, world or node (the target connected to spaces[i].target)."},
        {.name = "frames", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrOutput, .doc = "Each space's world frame."},
    };
    // One element per space, matched by index; older scenes' target0 ... and offset0 ... load into them.
    t.ownAttrs.push_back({.name = "spaces.target", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0),
                          .flags = AttrInput | AttrHidden | AttrArray, .doc = "World matrix of the space's target node.", .fileAlias = "target#"});
    t.ownAttrs.push_back({.name = "spaces.offset", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0),
                          .flags = AttrInput | AttrStorable | AttrHidden | AttrArray,
                          .doc = "The space's frame relative to its target (to the world for a world space).", .fileAlias = "offset#"});
    t.create = [] { return std::make_unique<SwitchNode>(); };
    types::registerType(std::move(t));
}

namespace
{
Graph& graph() { return Graph::current(); }

template <class F> auto guarded(F&& f)
{
    try
    {
        return f();
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(e.what());
    }
}

NodeId transformNamed(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    if (!n->type().isA("transform"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a transform");
    return n->id();
}

std::string spaceText(const json& j)
{
    return j.is_number() ? std::to_string(j.get<int>()) : j.get<std::string>();
}

int spaceArg(Graph& g, NodeId id, const json& j, const char* cmd)
{
    if (j.is_null())
        throw CommandError(std::string(cmd) + ": give space=");
    const int i = find(g, id, spaceText(j));
    if (i < 0)
    {
        std::string all;
        for (const std::string& s : names(g, id))
            all += (all.empty() ? "" : ", ") + s;
        throw CommandError(std::string(cmd) + ": " + g.node(id)->path() + " has no space '" + spaceText(j) + "' (it has " + all + ")");
    }
    return i;
}

json listJson(Graph& g, NodeId id)
{
    json spaces = json::array();
    for (const Space& s : list(g, id))
        spaces.push_back({{"index", s.index},
                          {"name", s.name},
                          {"kind", s.kind == Kind::Parent ? "parent" : s.kind == Kind::World ? "world" : "node"}});
    return {{"node", g.node(id)->path()}, {"active", active(g, id)}, {"spaces", spaces}};
}
} // namespace

void registerCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "addSpace",
        .doc = "Give a transform or joint spaces to animate in: 'world' or other nodes (its parent is always space 0). Its "
               "space channel (keyable, in the channel box) then picks the frame translate, rotate and scale are read in, "
               "and switchSpace changes it keeping the pose. With no space flag, the last selected node gets the others "
               "as spaces. Returns the node's spaces.",
        .positionalDoc = "nodes",
        .flags = {{"space", "sp", T::Any, nullptr, "'world', a node name, or a list of them."},
                  {"maintainOffset", "mo", T::Bool, true,
                   "Start each space where the parent is now, so adding and switching there change nothing; off uses the "
                   "target's own frame (the origin for world)."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<std::string> objs = a.objects();
            std::vector<std::string> spaces;
            if (a.has("space"))
            {
                const json& s = a.flags["space"];
                if (s.is_array())
                    for (const json& e : s)
                        spaces.push_back(e.get<std::string>());
                else
                    spaces.push_back(s.get<std::string>());
            }
            else if (objs.size() >= 2)
            {
                spaces.assign(objs.begin(), objs.end() - 1);
                objs = {objs.back()};
            }
            if (objs.empty() || spaces.empty())
                throw CommandError("addSpace: give the node and space= (or select the spaces, then the node)");
            json out;
            for (const std::string& o : objs)
            {
                const NodeId id = transformNamed(g, o, "addSpace");
                for (const std::string& s : spaces)
                    guarded([&] { return addSpace(g, id, s, a.get<bool>("maintainOffset", true)); });
                out = listJson(g, id);
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "removeSpace",
        .doc = "Remove one of a node's spaces. Frames spent in it switch to the parent keeping the world pose; the last "
               "space removed takes the spaceSwitch and the space channel with it. Returns the node's spaces.",
        .positionalDoc = "nodes",
        .flags = {{"space", "sp", T::Any, nullptr, "Space name or index (not 0, the parent)."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            json out;
            for (const std::string& o : a.objects())
            {
                const NodeId id = transformNamed(g, o, "removeSpace");
                const int i = spaceArg(g, id, a.flags.value("space", json()), "removeSpace");
                guarded([&] {
                    removeSpace(g, id, i);
                    return 0;
                });
                out = listJson(g, id);
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "listSpaces",
        .doc = "A node's spaces: {node, active, spaces: [{index, name, kind}]}, kind parent, world or node.",
        .positionalDoc = "node",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<std::string> objs = a.objects();
            if (objs.empty())
                throw CommandError("listSpaces: give a node");
            return listJson(g, transformNamed(g, objs.front(), "listSpaces"));
        },
    });

    ops::registerCommand({
        .name = "switchSpace",
        .doc = "Switch nodes to another space keeping their world pose: keys the space channel (stepped) and re-keys "
               "translate, rotate and scale in the new frame at the time, on the active layer. With range, every key in "
               "it (every frame with bake) keeps its pose, and the frames either side keep theirs. An unanimated node "
               "just changes. Returns the space index.",
        .positionalDoc = "nodes",
        .flags = {{"space", "sp", T::Any, nullptr, "'parent', 'world', a target's name, or the index."},
                  {"time", "t", T::Float, nullptr, "Seconds. Default: the current time."},
                  {"range", "r", T::List, nullptr, "(start, end) seconds to switch over."},
                  {"bake", "b", T::Bool, false, "With range: key every frame, so even the motion between keys is unchanged."},
                  {"preserve", "pr", T::String, "world", "world: keep the world pose; none: only change the space.", {"world", "none"}},
                  {"key", "k", T::Bool, nullptr, "Key rather than set. Default: when the node is animated."},
                  {"hold", "h", T::Bool, true, "Key the old space and pose a frame before (and after a range) so the rest of the animation keeps its pose."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            SwitchOptions o;
            if (a.has("time"))
                o.time = a.get<double>("time");
            if (a.has("range"))
            {
                const json& r = a.flags["range"];
                if (!r.is_array() || r.size() != 2)
                    throw CommandError("switchSpace: range is (start, end)");
                o.range = std::make_pair(r[0].get<double>(), r[1].get<double>());
            }
            o.bake = a.get<bool>("bake", false);
            o.preserve = a.get<std::string>("preserve", "world") != "none";
            if (a.has("key"))
                o.key = a.get<bool>("key");
            o.hold = a.get<bool>("hold", true);
            int index = 0;
            for (const std::string& name : a.objects())
            {
                const NodeId id = transformNamed(g, name, "switchSpace");
                index = spaceArg(g, id, a.flags.value("space", json()), "switchSpace");
                guarded([&] {
                    switchSpace(g, id, index, o);
                    return 0;
                });
            }
            return index;
        },
    });
}
} // namespace lila::scene::spaces
