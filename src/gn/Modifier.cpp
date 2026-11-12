#include "gn/Exec.h"
#include "gn/Gn.h"
#include "gn/InstanceView.h"
#include "gn/NodesCommon.h"
#include "gn/Stateful.h"
#include "gn/Tool.h"
#include "gn/Viewer.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "curve/CurveNodes.h"
#include "geo/Mesh.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Simulation.h"
#include "scene/Transform.h"
#include "scene/ViewState.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <set>

namespace lila::gn
{
using scene::AttrDef;
using scene::Graph;
using scene::Node;
using scene::NodeId;
using scene::PlugRef;
using scene::PlugType;

namespace
{
constexpr int kDefinitionAttr = 0;

/** Where a gn modifier type keeps each exposed parameter and Object Info input. */
struct TypeLayout
{
    struct Param
    {
        GInput input;
        int attr;
    };
    struct Object
    {
        std::string node;
        int meshAttr;
        int matrixAttr;
        int curveAttr;
    };
    struct Bone
    {
        std::string node;
        int worldAttr;
        int parentAttr;
    };
    /** A Collection Info input: arrays with one element per member object, its parent's matrix and the members' names. */
    struct Collection
    {
        std::string node;
        int meshesAttr;
        int curvesAttr;
        int matricesAttr;
        int parentAttr;
        int namesAttr;
    };
    int defAttr = 2;
    std::vector<Param> params;
    std::vector<Object> objects;
    std::vector<Bone> bones;
    std::vector<Collection> collections;
    int ownerAttr = -1;
};

/** Each modifier's last result whole, before it became the output mesh: heightfields, volumes, the spreadsheet read it. */
struct Outputs
{
    std::mutex mutex;
    std::map<NodeId, GeometryPtr> byModifier;
};

Outputs& outputs()
{
    // Never destroyed: the scene graph's static nodes clear their entries during exit, after this would be gone.
    static Outputs* o = new Outputs;
    return *o;
}

void setOutputGeometry(NodeId modifier, GeometryPtr g)
{
    std::lock_guard lock(outputs().mutex);
    if (g)
        outputs().byModifier[modifier] = std::move(g);
    else
        outputs().byModifier.erase(modifier);
}

/** Modifiers whose group reads Is Viewport or the active camera, re-evaluated when either changes. */
struct ViewModifiers
{
    std::mutex mutex;
    std::set<NodeId> ids;
};

ViewModifiers& viewModifiers()
{
    static ViewModifiers* v = new ViewModifiers;
    return *v;
}

void noteViewModifier(NodeId modifier, bool readsView)
{
    std::lock_guard lock(viewModifiers().mutex);
    if (readsView)
        viewModifiers().ids.insert(modifier);
    else
        viewModifiers().ids.erase(modifier);
}

void touchViewModifiers()
{
    std::set<NodeId> ids;
    {
        std::lock_guard lock(viewModifiers().mutex);
        ids = viewModifiers().ids;
    }
    Graph& g = Graph::current();
    for (NodeId id : ids)
        g.touch(id);
}

std::string objectMeshAttr(const std::string& node) { return "obj_" + node + "_mesh"; }
std::string objectMatrixAttr(const std::string& node) { return "obj_" + node + "_matrix"; }
std::string objectCurveAttr(const std::string& node) { return "obj_" + node + "_curve"; }
std::string boneWorldAttr(const std::string& node) { return "bone_" + node + "_world"; }
std::string boneParentAttr(const std::string& node) { return "bone_" + node + "_parent"; }
std::string collectionAttr(const std::string& node, const char* part) { return "col_" + node + "_" + part; }

/** A curve object's splines as a geometry curve component; null when the value holds none. */
std::shared_ptr<const CurveSet> curvesOf(const scene::PlugValue& v)
{
    const curve::Splines& splines = curve::splinesOf(v);
    if (splines.empty())
        return nullptr;
    auto set = std::make_shared<CurveSet>();
    for (const curve::Spline& s : splines)
        set->addSpline(s);
    return set;
}

/** A modifier running a node group. compute() reads only its own plugs: the definition arrives by connection. */
class GnModifierNode final : public scene::ModifierNode
{
public:
    explicit GnModifierNode(std::shared_ptr<const TypeLayout> layout) : mLayout(std::move(layout)) {}

    void compute(const scene::EvalContext& ctx) override
    {
        LILA_ZONE();

        const scene::MeshPtr& in = as<scene::MeshPtr>(inAttr);
        if (!as<bool>(enabledAttr) || scene::modifiers::bypassed(in))
        {
            noteSimulationModifier(id(), false, false);
            noteViewModifier(id(), false);
            clearInstanceView(id());
            setOutputGeometry(id(), nullptr);
            setModifierGizmos(id(), nullptr);
            setOutput(outAttr, in);
            return;
        }
        const std::string& def = as<std::string>(mLayout->defAttr);
        if (!mPlan || def != mCachedDef)
        {
            mCachedDef = def;
            PlanPtr oldPlan = std::move(mPlan);
            StepCache oldCache = std::move(mCache);
            mPlan.reset();
            mCache = StepCache{};
            mCarry = StepCache{};
            try
            {
                if (!def.empty())
                    mPlan = compile(Group::fromJson(json::parse(def)));
            }
            catch (const std::exception& e)
            {
                log::warn("{}: {}", name(), e.what());
            }
            if (oldPlan && mPlan && oldCache.plan == oldPlan.get())
                mCarry = carryOver(oldCache, *oldPlan, *mPlan);
        }
        noteSimulationModifier(id(), mPlan && mPlan->statefulStep >= 0, mPlan && mPlan->statefulInputsUseTime);
        noteViewModifier(id(), mPlan && mPlan->usesView);
        if (!mPlan)
        {
            clearInstanceView(id());
            setOutputGeometry(id(), nullptr);
            setModifierGizmos(id(), nullptr);
            setOutput(outAttr, in);
            return;
        }
        try
        {
            ExecEnv env;
            env.time = ctx.time;
            env.owner = id();
            env.sampleOnly = mPlan->statefulStep >= 0 && scene::sim::sampling();
            if (in)
            {
                if (in->hasDeleted())
                    env.input = meshGeometry(*in);
                else
                {
                    Geometry g;
                    g.mesh = in;
                    env.input = makeGeometry(std::move(g));
                }
            }
            for (const TypeLayout::Param& p : mLayout->params)
            {
                const scene::PlugValue& v = value(p.attr);
                json j = scene::toJson(v);
                env.params[p.input.name] = paramValue(p.input, j);
            }
            for (const TypeLayout::Object& o : mLayout->objects)
                env.objects[o.node] = {as<scene::MeshPtr>(o.meshAttr), curvesOf(value(o.curveAttr)), as<glm::dmat4>(o.matrixAttr)};
            for (const TypeLayout::Bone& b : mLayout->bones)
                env.objects[b.node] = {nullptr, nullptr, as<glm::dmat4>(b.worldAttr), as<glm::dmat4>(b.parentAttr)};
            for (const TypeLayout::Collection& col : mLayout->collections)
                env.collections[col.node] = collectionInput(col);
            if (mLayout->ownerAttr >= 0)
                env.ownerMatrix = as<glm::dmat4>(mLayout->ownerAttr);
            prepareCache(env.input);
            env.cache = &mCache;
            if (env.sampleOnly)
            {
                execute(*mPlan, env);
                setOutput(outAttr, in);
                return;
            }
            ViewerSink sink;
            if (mPlan->viewerStep >= 0)
                env.viewer = &sink;
            auto gizmos = std::make_shared<GizmoList>();
            if (!mPlan->gizmoSteps.empty())
                env.gizmos = gizmos.get();
            GeometryPtr result = execute(*mPlan, env);
            setModifierGizmos(id(), std::move(gizmos));
            setOutputGeometry(id(), result);
            scene::MeshPtr out = std::make_shared<geo::Mesh>(toOutputMesh(*result));
            if (InstanceViewPtr view = makeInstanceView(*result, out))
                setInstanceView(id(), std::move(view));
            else
                clearInstanceView(id());
            setOutput(outAttr, out);
            if (sink.captured)
                publishViewer(sink);
            else
                clearViewerData(id());
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", name(), e.what());
            setOutput(outAttr, in);
            clearViewerData(id());
            clearInstanceView(id());
            setOutputGeometry(id(), nullptr);
            setModifierGizmos(id(), nullptr);
        }
    }

    PlaybackExtra playbackExtra(const scene::EvalContext& ctx) const override
    {
        // Until the simulation reaches this frame the output shows the start; it is recomputed once the state arrives.
        if (mPlan && mPlan->statefulStep >= 0 && !simulationStateReady(id(), ctx.time))
            return {.keep = false};
        auto p = std::make_shared<Published>(
            Published{instanceView(id()), modifierGeometry(id()), viewerData(id()), mPlan && mPlan->statefulStep >= 0 ? simulationInputs(id()) : nullptr});
        size_t bytes = sizeof(Published);
        if (p->view)
            bytes += p->view->instanceCount * sizeof(glm::mat4);
        return {std::move(p), bytes};
    }

    void restorePlaybackExtra(const std::shared_ptr<const void>& data) override
    {
        const auto* p = static_cast<const Published*>(data.get());
        if (p && p->view)
            setInstanceView(id(), p->view);
        else
            clearInstanceView(id());
        setOutputGeometry(id(), p ? p->geometry : nullptr);
        if (p && p->viewer)
            setViewerData(id(), p->viewer);
        else
            clearViewerData(id());
        if (p && p->simInputs)
            restoreSimulationInputs(id(), p->simInputs);
    }

    ~GnModifierNode() override
    {
        clearViewerData(id());
        forgetSimulationModifier(id());
        noteViewModifier(id(), false);
        clearInstanceView(id());
        setOutputGeometry(id(), nullptr);
        setModifierGizmos(id(), nullptr);
    }

private:
    /** What compute() publishes beside its output mesh. */
    struct Published
    {
        InstanceViewPtr view;
        GeometryPtr geometry;
        ViewerPtr viewer;
        std::shared_ptr<const SimInputs> simInputs;
    };

    /**
     * Keeps the cached step results only while nothing a time-independent step reads has changed. The input mesh counts
     * by content: upstream nodes can hand over a new copy of the same mesh on every evaluation.
     */
    void prepareCache(const GeometryPtr& in)
    {
        uint64_t key = 0x6a09e667f3bcc908ull;
        key = in ? hashGeometry(*in, key) : key;
        for (const TypeLayout::Param& p : mLayout->params)
        {
            std::string j = scene::toJson(value(p.attr)).dump();
            key = scene::sim::hashBytes(j.data(), j.size(), key);
        }
        for (const TypeLayout::Object& o : mLayout->objects)
        {
            Geometry object;
            object.mesh = as<scene::MeshPtr>(o.meshAttr);
            object.curves = curvesOf(value(o.curveAttr));
            key = hashGeometry(object, key);
            key = scene::sim::hashBytes(&as<glm::dmat4>(o.matrixAttr), sizeof(glm::dmat4), key);
        }
        for (const TypeLayout::Bone& b : mLayout->bones)
        {
            key = scene::sim::hashBytes(&as<glm::dmat4>(b.worldAttr), sizeof(glm::dmat4), key);
            key = scene::sim::hashBytes(&as<glm::dmat4>(b.parentAttr), sizeof(glm::dmat4), key);
        }
        for (const TypeLayout::Collection& col : mLayout->collections)
        {
            CollectionInput collection = collectionInput(col);
            key = scene::sim::hashBytes(&collection.parent, sizeof(glm::dmat4), key);
            for (size_t k = 0; k < collection.members.size(); ++k)
            {
                Geometry object;
                object.mesh = collection.members[k].mesh;
                object.curves = collection.members[k].curves;
                key = hashGeometry(object, key);
                key = scene::sim::hashBytes(&collection.members[k].matrix, sizeof(glm::dmat4), key);
                key = scene::sim::hashBytes(collection.names[k].data(), collection.names[k].size(), key);
            }
        }
        if (mCache.plan != mPlan.get() || mCache.key != key)
        {
            // A new plan keeps the results of steps an edit left alone, while nothing from outside changed.
            if (mCarry.plan == mPlan.get() && mCarry.key == key)
                mCache = std::move(mCarry);
            else
            {
                mCache = StepCache{};
                mCache.plan = mPlan.get();
                mCache.key = key;
            }
            mCarry = StepCache{};
        }
    }

    /**
     * Each step's identity: its node's type, values and mute, and the identities of what feeds it. Equal identities in two
     * plans compute the same thing from the same outside inputs.
     */
    static std::vector<uint64_t> stepSignatures(const Plan& p)
    {
        std::vector<uint64_t> sig(p.steps.size(), 0);
        for (size_t s = 0; s < p.steps.size(); ++s)
        {
            const Plan::Step& step = p.steps[s];
            const GNode& n = p.group.nodes[step.node];
            std::string text = n.type + "|" + n.values.dump() + (n.muted ? "|m" : "|");
            // A nested group's step runs the definition embedded for it, which can change while the node doesn't.
            if (auto g = n.values.find("group"); g != n.values.end() && g->is_string())
                if (auto def = p.group.subgroups.find(g->get<std::string>()); def != p.group.subgroups.end())
                    text += def->second.dump();
            uint64_t h = scene::sim::hashBytes(text.data(), text.size(), 0x9e3779b97f4a7c15ull);
            for (const auto& links : step.inputs)
            {
                for (const Plan::Source& src : links)
                {
                    const uint64_t part[3] = {src.step >= 0 ? sig[size_t(src.step)] : 0, uint64_t(src.output), uint64_t(src.type)};
                    h = scene::sim::hashBytes(part, sizeof part, h);
                }
                h = (h ^ 0xff51afd7ed558ccdull) * 0x100000001b3ull;
            }
            sig[s] = h;
        }
        return sig;
    }

    /** The old cache's results for the new plan's steps that compute the same thing; a cache for the new plan. */
    static StepCache carryOver(const StepCache& old, const Plan& oldPlan, const Plan& next)
    {
        StepCache c;
        c.plan = &next;
        c.key = old.key;
        c.valid.assign(next.steps.size(), 0);
        c.results.assign(next.steps.size(), {});
        if (old.valid.size() != oldPlan.steps.size())
            return c;
        std::map<uint64_t, size_t> was;
        const std::vector<uint64_t> oldSig = stepSignatures(oldPlan), newSig = stepSignatures(next);
        for (size_t s = 0; s < oldPlan.steps.size(); ++s)
            if (old.valid[s])
                was[oldSig[s]] = s;
        for (size_t s = 0; s < next.steps.size(); ++s)
        {
            const Plan::Step& step = next.steps[s];
            // Nested groups and simulation nodes read more than their node shows; they always run again.
            if (step.dynamic || step.decl->stateful || step.decl->dynamicSockets)
                continue;
            if (auto it = was.find(newSig[s]); it != was.end())
            {
                c.results[s] = old.results[it->second];
                c.valid[s] = 1;
            }
        }
        return c;
    }

    void publishViewer(const ViewerSink& sink)
    {
        LILA_ZONE();

        auto d = std::make_shared<ViewerData>();
        d->group = mPlan->group.name;
        d->node = sink.node;
        d->type = sink.type;
        d->domain = sink.domain;
        Geometry r = realize(sink.geometry ? *sink.geometry : Geometry{});
        geo::Mesh m = r.mesh ? *r.mesh : geo::Mesh();
        if (r.points)
            m.append(*r.points);
        if (r.curves)
            for (const glm::dvec3& p : r.curves->positions)
                m.addPoint(p);
        stripAnonymous(m);
        m.compact();
        if (sink.field)
        {
            FieldContext ctx = nodes::contextFor(m, sink.domain);
            d->domain = ctx.domain;
            bool vector = sink.type == SocketType::Vector || sink.type == SocketType::Rotation;
            std::vector<glm::dvec4> v(ctx.size);
            if (sink.type == SocketType::Color)
                v = expand<glm::dvec4>(evaluate(sink.field, ctx, SocketType::Color), ctx.size);
            else if (vector)
            {
                std::vector<glm::dvec3> raw = expand<glm::dvec3>(evaluate(sink.field, ctx, SocketType::Vector), ctx.size);
                for (size_t k = 0; k < ctx.size; ++k)
                    v[k] = glm::dvec4(raw[k], 0.0);
            }
            else
            {
                std::vector<double> raw = expand<double>(evaluate(sink.field, ctx, SocketType::Float), ctx.size);
                for (size_t k = 0; k < ctx.size; ++k)
                    v[k] = glm::dvec4(raw[k], 0.0, 0.0, 0.0);
            }
            d->values = std::move(v);
            d->hasValues = true;
            if (!d->values.empty())
            {
                d->minValue = d->maxValue = d->values[0].x;
                for (const glm::dvec4& x : d->values)
                {
                    d->minValue = std::min(d->minValue, x.x);
                    d->maxValue = std::max(d->maxValue, x.x);
                }
            }
        }
        d->mesh = std::make_shared<const geo::Mesh>(std::move(m));
        static std::atomic<uint64_t> serial{0};
        d->serial = ++serial;
        setViewerData(id(), std::move(d));
    }

    CollectionInput collectionInput(const TypeLayout::Collection& col) const
    {
        CollectionInput in;
        in.parent = as<glm::dmat4>(col.parentAttr);
        json names = json::parse(as<std::string>(col.namesAttr), nullptr, false);
        for (const auto& [k, v] : elements(col.matricesAttr))
        {
            ObjectInput o;
            o.matrix = std::get<glm::dmat4>(v);
            if (const scene::PlugValue* m = element(col.meshesAttr, k))
                o.mesh = std::get<scene::MeshPtr>(*m);
            if (const scene::PlugValue* cv = element(col.curvesAttr, k))
                o.curves = curvesOf(*cv);
            in.members.push_back(std::move(o));
            const bool named = names.is_array() && size_t(k) < names.size() && names[size_t(k)].is_string();
            in.names.push_back(named ? names[size_t(k)].get<std::string>() : std::string());
        }
        return in;
    }

    std::shared_ptr<const TypeLayout> mLayout;
    std::string mCachedDef;
    PlanPtr mPlan;
    StepCache mCache;
    /** Results a definition edit carried over, taken up by prepareCache when the outside inputs are unchanged. */
    StepCache mCarry;
};

/** Node holding a group definition. Nothing to compute: modifiers read the definition through connections. */
class GroupNode final : public Node
{
};

struct Interface
{
    std::vector<GInput> inputs;
    std::vector<std::string> objects;
    std::vector<std::string> bones;
    std::vector<std::string> collections;
    bool usesTime = false;

    std::string signature() const
    {
        json j = json::array();
        for (const GInput& in : inputs)
            j.push_back(std::string(socketTypeName(in.type)) + ":" + in.name + (in.subtype == "angle" ? ":angle" : ""));
        json o = objects;
        return json{{"in", j}, {"obj", o}, {"bones", bones}, {"cols", collections}, {"time", usesTime}}.dump();
    }
};

Interface interfaceOf(const Group& g)
{
    Interface i;
    i.inputs = g.inputs;
    for (const GNode& n : g.nodes)
        if (n.type == "ObjectInfo")
            i.objects.push_back(n.name);
        else if (n.type == "BoneInfo")
            i.bones.push_back(n.name);
        else if (n.type == "CollectionInfo")
            i.collections.push_back(n.name);
    i.usesTime = usesSceneTime(g) || hasStateful(g);
    return i;
}

/** Registered gn types: name -> interface signature, and the highest version seen per group. */
struct Registry
{
    std::map<std::string, std::string> signatures;
    std::map<std::string, int> maxVersion;
};

Registry& registry()
{
    static Registry r;
    return r;
}

/** Label, tooltip, limits and panel: what editors show, none of which changes the attr's layout. */
void presentParam(AttrDef& a, const GInput& in)
{
    a.niceName = in.displayName();
    a.doc = !in.description.empty() ? in.description
            : in.type == SocketType::Rotation ? "Exposed geometry-nodes rotation, degrees."
                                               : "Group input of the geometry-nodes group, fed to its Group Input node.";
    a.minValue = in.min;
    a.maxValue = in.max;
    a.softMin = in.softMin;
    a.softMax = in.softMax;
    a.step = in.step;
    if (in.subtype == "factor" && !a.softMin && !a.softMax)
    {
        a.softMin = 0.0;
        a.softMax = 1.0;
    }
    else if (in.subtype == "percentage" && !a.softMin && !a.softMax)
    {
        a.softMin = 0.0;
        a.softMax = 100.0;
    }
    a.category = in.panel;
    if (in.type == SocketType::Color || (in.type == SocketType::Vector && in.subtype == "color"))
        a.flags |= scene::AttrColor;
    else
        a.flags &= ~scene::AttrColor;
}

AttrDef paramAttr(const GInput& in)
{
    AttrDef a;
    a.name = in.name;
    a.flags = scene::AttrInput | scene::AttrStorable | scene::AttrKeyable;
    const json& d = in.defaultValue;
    auto num = [&](double fallback) { return d.is_number() ? d.get<double>() : d.is_boolean() ? double(d.get<bool>()) : fallback; };
    switch (in.type)
    {
    case SocketType::Int:
        a.type = PlugType::Int;
        a.defaultValue = int64_t(std::llround(num(0.0)));
        break;
    case SocketType::Bool:
        a.type = PlugType::Bool;
        a.defaultValue = num(0.0) != 0.0;
        break;
    case SocketType::Vector:
    case SocketType::Rotation:
    {
        a.type = PlugType::Vec3;
        glm::dvec3 v(0.0);
        if (d.is_array() && d.size() == 3)
            v = {d[0].get<double>(), d[1].get<double>(), d[2].get<double>()};
        a.defaultValue = v;
        for (int k = 0; k < 3; ++k)
            a.childNames[k] = in.name + char('X' + k);
        break;
    }
    case SocketType::Color:
    {
        a.type = PlugType::Vec3;
        glm::dvec3 v(0.0);
        if (d.is_array() && d.size() >= 3)
            v = {d[0].get<double>(), d[1].get<double>(), d[2].get<double>()};
        else if (d.is_number())
            v = glm::dvec3(d.get<double>());
        a.defaultValue = v;
        for (int k = 0; k < 3; ++k)
            a.childNames[k] = in.name + "RGB"[k];
        break;
    }
    case SocketType::String:
        a.type = PlugType::String;
        a.defaultValue = d.is_string() ? d.get<std::string>() : std::string();
        a.flags &= ~scene::AttrKeyable;
        break;
    case SocketType::Matrix:
    {
        a.type = PlugType::Matrix;
        glm::dmat4 m(1.0);
        if (d.is_array() && d.size() == 16)
            for (int k = 0; k < 16; ++k)
                m[k / 4][k % 4] = d[size_t(k)].get<double>();
        a.defaultValue = m;
        break;
    }
    case SocketType::Geometry:
    case SocketType::Bundle:
    case SocketType::Closure:
        // Only a link inside a parent group can feed these; on the modifier they read as empty.
        a.type = PlugType::String;
        a.defaultValue = std::string();
        a.flags = (a.flags & ~scene::AttrKeyable) | scene::AttrHidden;
        break;
    default:
        a.type = PlugType::Float;
        a.defaultValue = num(0.0);
        break;
    }
    presentParam(a, in);
    return a;
}

/**
 * Refreshes an already registered type's params in place, so a tooltip edit needs no new type. Defaults are refreshed too:
 * a group rebuilt under the same name (a new scene) must not hand new modifiers the old group's defaults.
 */
void representParams(const scene::NodeType& t, const Group& g)
{
    auto& type = const_cast<scene::NodeType&>(t);
    for (const GInput& in : g.inputs)
        for (std::vector<AttrDef>* list : {&type.ownAttrs, &type.attrs})
            for (AttrDef& a : *list)
                if (a.name == in.name)
                {
                    presentParam(a, in);
                    a.defaultValue = paramAttr(in).defaultValue;
                }
}

const scene::NodeType& ensureType(const Group& g)
{
    LILA_ZONE();

    std::string name = typeNameFor(g);
    Interface iface = interfaceOf(g);
    std::string sig = iface.signature();
    Registry& r = registry();
    r.maxVersion[g.name] = std::max(r.maxVersion[g.name], g.typeVersion);
    if (const scene::NodeType* t = scene::types::find(name))
    {
        auto it = r.signatures.find(name);
        if (it != r.signatures.end() && it->second == sig)
        {
            representParams(*t, g);
            return *t;
        }
        // Same name, other interface (another scene loaded): nodes still holding the old type keep it alive.
        scene::types::unregisterType(name);
    }

    scene::NodeType t;
    t.name = name;
    t.icon = "topology-complex";
    t.doc = "Runs the " + g.name + " geometry-nodes group on the mesh. Edit the group in the Node Editor; its group inputs "
            "show here as parameters.";
    t.timeDependent = iface.usesTime;
    auto layout = std::make_shared<TypeLayout>();
    t.ownAttrs.push_back({.name = "inMesh", .type = PlugType::Mesh, .defaultValue = scene::MeshPtr(), .flags = scene::AttrInput | scene::AttrHidden});
    t.ownAttrs.push_back({.name = "enabled", .type = PlugType::Bool, .defaultValue = true,
                          .flags = scene::AttrInput | scene::AttrStorable | scene::AttrKeyable, .doc = "Turn the modifier off without removing it."});
    t.ownAttrs.push_back({.name = "groupDefinition", .type = PlugType::String, .defaultValue = std::string(),
                          .flags = scene::AttrInput | scene::AttrHidden, .doc = "Connected from the group node."});
    layout->defAttr = 2;
    for (const GInput& in : iface.inputs)
    {
        layout->params.push_back({in, int(t.ownAttrs.size())});
        t.ownAttrs.push_back(paramAttr(in));
    }
    for (const std::string& obj : iface.objects)
    {
        int meshAttr = int(t.ownAttrs.size());
        t.ownAttrs.push_back({.name = objectMeshAttr(obj), .type = PlugType::Mesh, .defaultValue = scene::MeshPtr(),
                              .flags = scene::AttrInput | scene::AttrHidden, .doc = "Object Info mesh."});
        t.ownAttrs.push_back({.name = objectMatrixAttr(obj), .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0),
                              .flags = scene::AttrInput | scene::AttrHidden, .doc = "Object Info world matrix."});
        t.ownAttrs.push_back({.name = objectCurveAttr(obj), .type = PlugType::Data, .defaultValue = scene::DataPtr(),
                              .flags = scene::AttrInput | scene::AttrHidden, .doc = "Object Info curve."});
        layout->objects.push_back({obj, meshAttr, meshAttr + 1, meshAttr + 2});
    }
    for (const std::string& b : iface.bones)
    {
        int world = int(t.ownAttrs.size());
        t.ownAttrs.push_back({.name = boneWorldAttr(b), .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0),
                              .flags = scene::AttrInput | scene::AttrHidden, .doc = "Bone Info world matrix."});
        t.ownAttrs.push_back({.name = boneParentAttr(b), .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0),
                              .flags = scene::AttrInput | scene::AttrHidden, .doc = "Bone Info parent world matrix."});
        layout->bones.push_back({b, world, world + 1});
    }
    for (const std::string& col : iface.collections)
    {
        int first = int(t.ownAttrs.size());
        t.ownAttrs.push_back({.name = collectionAttr(col, "meshes"), .type = PlugType::Mesh, .defaultValue = scene::MeshPtr(),
                              .flags = scene::AttrInput | scene::AttrHidden | scene::AttrArray, .doc = "Collection Info member meshes."});
        t.ownAttrs.push_back({.name = collectionAttr(col, "curves"), .type = PlugType::Data, .defaultValue = scene::DataPtr(),
                              .flags = scene::AttrInput | scene::AttrHidden | scene::AttrArray, .doc = "Collection Info member curves."});
        t.ownAttrs.push_back({.name = collectionAttr(col, "matrices"), .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0),
                              .flags = scene::AttrInput | scene::AttrHidden | scene::AttrArray, .doc = "Collection Info member world matrices."});
        t.ownAttrs.push_back({.name = collectionAttr(col, "parent"), .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0),
                              .flags = scene::AttrInput | scene::AttrHidden, .doc = "Collection Info parent world matrix."});
        t.ownAttrs.push_back({.name = collectionAttr(col, "names"), .type = PlugType::String, .defaultValue = std::string(),
                              .flags = scene::AttrInput | scene::AttrStorable | scene::AttrHidden, .doc = "Collection Info member names, JSON."});
        layout->collections.push_back({col, first, first + 1, first + 2, first + 3, first + 4});
    }
    if (!iface.objects.empty() || !iface.bones.empty() || !iface.collections.empty())
    {
        layout->ownerAttr = int(t.ownAttrs.size());
        t.ownAttrs.push_back({.name = "ownerMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0),
                              .flags = scene::AttrInput | scene::AttrStorable | scene::AttrHidden,
                              .doc = "The mesh's world matrix when the Object Info inputs were connected: Object Info nodes set "
                                     "Relative To owner read the other object against it."});
    }
    t.ownAttrs.push_back({.name = "outMesh", .type = PlugType::Mesh, .defaultValue = scene::MeshPtr(), .flags = scene::AttrOutput | scene::AttrHidden});
    int out = int(t.ownAttrs.size()) - 1;
    std::shared_ptr<const TypeLayout> frozen = layout;
    t.create = [frozen, out] {
        auto n = std::make_unique<GnModifierNode>(frozen);
        n->outAttr = out;
        return n;
    };
    r.signatures[name] = sig;
    return scene::types::registerType(std::move(t));
}

/** Registers the type for a group node's current definition, if it has a readable one. */
void syncTypeFor(const Node& n)
{
    const std::string& def = n.as<std::string>(kDefinitionAttr);
    if (def.empty())
        return;
    try
    {
        ensureType(Group::fromJson(json::parse(def)));
    }
    catch (const std::exception& e)
    {
        log::warn("{}: {}", n.path(), e.what());
    }
}

void setGroupDefinition(Graph& g, Node& n, const Group& grp)
{
    g.setValue({n.id(), kDefinitionAttr, -1}, grp.toJson().dump());
}

/** Drops links whose sockets no longer exist (a property change removed them) or whose nodes are gone. */
void pruneLinks(Group& grp)
{
    registerNodeLibrary();
    std::erase_if(grp.links, [&](const GLink& l) {
        const GNode* from = grp.node(l.fromNode);
        const GNode* to = grp.node(l.toNode);
        if (!from || !to)
            return true;
        const NodeDecl* fd = findDecl(from->type);
        const NodeDecl* td = findDecl(to->type);
        if (!fd || !td)
            return false;
        return fd->socketsFor(grp, *from).outputIndex(l.fromSocket) < 0 || td->socketsFor(grp, *to).inputIndex(l.toSocket) < 0;
    });
}

/** Points a Bone Info input at the world and parent matrices of the joint (or any transform) its node names. */
void connectBone(Graph& g, Node& mod, const GNode& n)
{
    int worldAttr = mod.type().attrIndex(boneWorldAttr(n.name)), parentAttr = mod.type().attrIndex(boneParentAttr(n.name));
    if (worldAttr < 0 || parentAttr < 0)
        return;
    std::string joint = findDecl("BoneInfo")->propValue(n, "joint");
    Node* j = joint.empty() ? nullptr : g.find(joint);
    if (j && !j->type().isA("transform"))
        throw std::invalid_argument(joint + " is not a joint");
    PlugRef worldDst{mod.id(), worldAttr, -1}, parentDst{mod.id(), parentAttr, -1};
    PlugRef worldSrc = j ? PlugRef{j->id(), scene::Transform::kWorldMatrix, -1} : PlugRef{};
    PlugRef parentSrc = j ? PlugRef{j->id(), scene::Transform::kParentMatrix, -1} : PlugRef{};
    if (j && g.source(worldDst) == worldSrc && g.source(parentDst) == parentSrc)
        return;
    g.disconnect(worldDst);
    g.disconnect(parentDst);
    if (!j)
        return;
    try
    {
        g.connect(worldSrc, worldDst);
        g.connect(parentSrc, parentDst);
    }
    catch (const std::exception&)
    {
        throw std::invalid_argument("Bone Info " + n.name + ": " + joint + " depends on " + mod.path() + " (that would be a cycle)");
    }
}

/**
 * Points a Collection Info input at every mesh and curve object under the transform its node names, depth first in
 * outliner order, leaving out the mesh the modifier is on.
 */
void connectCollection(Graph& g, Node& mod, const GNode& n)
{
    const scene::NodeType& type = mod.type();
    int meshes = type.attrIndex(collectionAttr(n.name, "meshes")), curves = type.attrIndex(collectionAttr(n.name, "curves"));
    int matrices = type.attrIndex(collectionAttr(n.name, "matrices")), parent = type.attrIndex(collectionAttr(n.name, "parent"));
    int names = type.attrIndex(collectionAttr(n.name, "names"));
    if (meshes < 0 || curves < 0 || matrices < 0 || parent < 0 || names < 0)
        return;
    std::string parentName = findDecl("CollectionInfo")->propValue(n, "parent");
    Node* top = parentName.empty() ? nullptr : g.find(parentName);
    if (top && !top->type().isA("transform"))
        throw std::invalid_argument(parentName + " is not a transform");
    const NodeId self = scene::modifiers::meshOf(g, mod.id());
    std::vector<std::pair<PlugRef, PlugRef>> want; // source, destination
    json memberNames = json::array();
    if (top)
    {
        want.push_back({{top->id(), scene::Transform::kWorldMatrix, -1}, {mod.id(), parent, -1}});
        std::function<void(NodeId)> visit = [&](NodeId id) {
            for (NodeId c : g.children(id))
            {
                Node* child = g.node(c);
                if (!child)
                    continue;
                const bool isCurve = child->type().isA("curve");
                if (c != self && (isCurve || child->type().isA("mesh")))
                {
                    int k = int(memberNames.size());
                    PlugRef shape = isCurve ? PlugRef{c, child->type().attrIndex("local"), -1} : PlugRef{c, scene::MeshNode::kOutMesh, -1};
                    want.push_back({shape, {.node = mod.id(), .attr = isCurve ? curves : meshes, .element = k}});
                    want.push_back({{c, scene::Transform::kWorldMatrix, -1}, {.node = mod.id(), .attr = matrices, .element = k}});
                    memberNames.push_back(child->name());
                }
                visit(c);
            }
        };
        visit(top->id());
    }
    std::vector<std::pair<PlugRef, PlugRef>> have;
    for (const auto& [dst, src] : g.connections())
        if (dst.node == mod.id() && (dst.attr == meshes || dst.attr == curves || dst.attr == matrices || dst.attr == parent))
            have.push_back({src, dst});
    std::sort(want.begin(), want.end());
    std::sort(have.begin(), have.end());
    if (want != have)
    {
        for (const auto& [src, dst] : have)
            g.disconnect(dst);
        try
        {
            for (const auto& [src, dst] : want)
                g.connect(src, dst);
        }
        catch (const std::exception&)
        {
            throw std::invalid_argument("Collection Info " + n.name + ": something under " + parentName + " depends on " + mod.path() +
                                        " (that would be a cycle)");
        }
    }
    std::string list = memberNames.dump();
    if (mod.as<std::string>(names) != list)
        g.setValue({mod.id(), names, -1}, list);
}

/**
 * Points each Object Info input of a modifier at the object its node names, and takes the owner mesh's world matrix for
 * the ones relative to it. Throws on cycles or non-meshes.
 */
void connectObjects(Graph& g, Node& mod, const Group& grp)
{
    if (int owner = mod.type().attrIndex("ownerMatrix"); owner >= 0)
        if (NodeId mesh = scene::modifiers::meshOf(g, mod.id()))
        {
            const glm::dmat4 world = std::get<glm::dmat4>(g.value({mesh, scene::Transform::kWorldMatrix, -1}));
            if (mod.as<glm::dmat4>(owner) != world)
                g.setValue({mod.id(), owner, -1}, world);
        }
    for (const GNode& n : grp.nodes)
    {
        if (n.type == "BoneInfo")
            connectBone(g, mod, n);
        if (n.type == "CollectionInfo")
            connectCollection(g, mod, n);
        if (n.type != "ObjectInfo")
            continue;
        int meshAttr = mod.type().attrIndex(objectMeshAttr(n.name));
        int matAttr = mod.type().attrIndex(objectMatrixAttr(n.name));
        int curveAttr = mod.type().attrIndex(objectCurveAttr(n.name));
        if (meshAttr < 0 || matAttr < 0 || curveAttr < 0)
            continue;
        std::string object = findDecl("ObjectInfo")->propValue(n, "object");
        PlugRef meshDst{mod.id(), meshAttr, -1}, matDst{mod.id(), matAttr, -1}, curveDst{mod.id(), curveAttr, -1};
        Node* obj = object.empty() ? nullptr : g.find(object);
        const bool isCurve = obj && obj->type().isA("curve");
        if (obj && !isCurve && !obj->type().isA("mesh"))
            throw std::invalid_argument(object + " is not a mesh or a curve");
        PlugRef shapeSrc;
        if (obj)
            shapeSrc = isCurve ? PlugRef{obj->id(), obj->type().attrIndex("local"), -1} : PlugRef{obj->id(), scene::MeshNode::kOutMesh, -1};
        PlugRef shapeDst = isCurve ? curveDst : meshDst;
        PlugRef matSrc = obj ? PlugRef{obj->id(), scene::Transform::kWorldMatrix, -1} : PlugRef{};
        auto cur = g.source(shapeDst);
        if (cur && obj && *cur == shapeSrc && g.source(matDst) == matSrc && !g.source(isCurve ? meshDst : curveDst))
            continue;
        g.disconnect(meshDst);
        g.disconnect(curveDst);
        g.disconnect(matDst);
        if (!obj)
            continue;
        try
        {
            g.connect(shapeSrc, shapeDst);
            g.connect(matSrc, matDst);
        }
        catch (const std::exception&)
        {
            throw std::invalid_argument("Object Info " + n.name + ": " + object + " depends on " + mod.path() + " (that would be a cycle)");
        }
    }
}

using Renames = std::map<std::string, std::string>;

/**
 * Replaces a modifier with one of the group's current type, keeping its name, connections and parameter values.
 * renames maps old parameter names to new ones, so a renamed input keeps its value and animation.
 */
Node* migrate(Graph& g, Node& old, const scene::NodeType& type, const Renames& renames)
{
    LILA_ZONE();

    std::string name = old.name();
    NodeId oldId = old.id();
    std::vector<std::pair<PlugRef, PlugRef>> in, out;
    for (auto& [dst, src] : g.connections())
    {
        if (dst.node == oldId)
            in.push_back({src, dst});
        else if (src.node == oldId)
            out.push_back({src, dst});
    }
    auto newName = [&](const std::string& n) {
        auto it = renames.find(n);
        return it == renames.end() ? n : it->second;
    };
    std::vector<std::pair<std::string, scene::PlugValue>> values;
    const auto& defs = old.type().attrs;
    for (int i = 0; i < int(defs.size()); ++i)
        if ((defs[i].flags & scene::AttrStorable) && (defs[i].flags & scene::AttrInput))
            values.push_back({defs[i].name, old.value(i)});

    Node* fresh = g.createNode(type, {}, scene::kNoNode, false);
    for (auto& [attr, v] : values)
    {
        int idx = type.attrIndex(newName(attr));
        if (idx >= 0 && type.attrs[idx].type == defs[old.type().attrIndex(attr)].type)
            g.setValue({fresh->id(), idx, -1}, v);
    }
    g.deleteNode(oldId);
    g.rename(fresh->id(), name);
    for (auto& [src, dst] : in)
    {
        int idx = type.attrIndex(newName(defs[dst.attr].name));
        if (idx < 0)
            continue;
        try
        {
            g.connect(src, {fresh->id(), idx, dst.component});
        }
        catch (const std::exception& e)
        {
            log::warn("{}: couldn't reconnect {}: {}", name, defs[dst.attr].name, e.what());
        }
    }
    for (auto& [src, dst] : out)
    {
        int idx = type.attrIndex(defs[src.attr].name);
        if (idx >= 0)
            g.connect({fresh->id(), idx, src.component}, dst);
    }
    return fresh;
}

void syncInstances(Graph& g, Node& groupNode, const Group& grp, const Renames& renames)
{
    const scene::NodeType& type = ensureType(grp);
    for (NodeId id : instancesOf(g, groupNode.id()))
    {
        Node* mod = g.node(id);
        if (!mod)
            continue;
        if (&mod->type().registered() != &type)
            mod = migrate(g, *mod, type, renames);
        connectObjects(g, *mod, grp);
    }
}

/**
 * Copies the current definition of every group its Group nodes use into grp.subgroups, dropping unused ones. Groups
 * missing from the scene keep the copy they had. Throws when a group would end up containing itself.
 */
void embedSubgroups(Graph& g, Group& grp)
{
    std::map<std::string, json> next;
    for (const std::string& name : referencedGroups(grp))
    {
        if (name == grp.name)
            throw std::invalid_argument("group " + grp.name + " can't contain itself");
        json def;
        if (Node* n = findGroupNode(g, name))
            def = readGroup(*n).toJson();
        else if (auto it = grp.subgroups.find(name); it != grp.subgroups.end())
            def = it->second;
        else
            continue;
        if (dependsOn(Group::fromJson(def), grp.name))
            throw std::invalid_argument("group " + name + " uses " + grp.name + ", so it can't go inside it (that would be a cycle)");
        next[name] = std::move(def);
    }
    grp.subgroups = std::move(next);
}

/** Re-embeds a changed group into every group that uses it, which updates their modifiers in turn. */
void propagateToUsers(Graph& g, const std::string& changed)
{
    thread_local std::set<std::string> active;
    if (!active.insert(changed).second)
        return;
    for (const std::string& name : groupNames(g))
    {
        if (name == changed || active.contains(name))
            continue;
        Node* n = findGroupNode(g, name);
        Group user;
        try
        {
            user = readGroup(*n);
        }
        catch (const std::exception&)
        {
            continue;
        }
        std::vector<std::string> refs = referencedGroups(user);
        if (std::find(refs.begin(), refs.end(), changed) == refs.end())
            continue;
        try
        {
            commitGroup(g, *n, std::move(user));
        }
        catch (const std::exception& e)
        {
            log::warn("{}: couldn't update its use of {}: {}", name, changed, e.what());
        }
    }
    active.erase(changed);
}
} // namespace

std::string typeNameFor(const Group& g)
{
    return "gn_" + g.name + (g.typeVersion > 1 ? "_v" + std::to_string(g.typeVersion) : std::string());
}

scene::Node* findGroupNode(Graph& g, std::string_view group)
{
    // A hierarchy node elsewhere may share the group's name, so look for the group node among every match.
    for (NodeId id : g.findAll(group))
        if (Node* n = g.node(id); n->type().name == kGroupNodeType)
            return n;
    return nullptr;
}

std::vector<std::string> groupNames(Graph& g)
{
    std::vector<std::string> out;
    for (NodeId id : g.nodes())
        if (g.node(id)->type().name == kGroupNodeType)
            out.push_back(g.node(id)->name());
    return out;
}

Group readGroup(const Node& groupNode)
{
    const std::string& def = groupNode.as<std::string>(kDefinitionAttr);
    if (def.empty())
        throw std::invalid_argument(groupNode.path() + " has no definition");
    try
    {
        return Group::fromJson(json::parse(def));
    }
    catch (const json::exception& e)
    {
        throw std::invalid_argument(groupNode.path() + ": " + e.what());
    }
}

std::vector<NodeId> instancesOf(Graph& g, NodeId groupNode)
{
    std::vector<NodeId> out;
    for (const PlugRef& d : g.destinations({groupNode, kDefinitionAttr, -1}))
        if (Node* n = g.node(d.node); n && isGnModifier(*n))
            out.push_back(d.node);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool isGnModifier(const Node& n) { return dynamic_cast<const GnModifierNode*>(&n) != nullptr; }

std::shared_ptr<const geo::Heightfield> modifierHeightfield(NodeId modifier)
{
    GeometryPtr g = modifierGeometry(modifier);
    return g ? g->heightfield : nullptr;
}

GeometryPtr modifierGeometry(NodeId modifier)
{
    std::lock_guard lock(outputs().mutex);
    auto it = outputs().byModifier.find(modifier);
    return it == outputs().byModifier.end() ? nullptr : it->second;
}

std::vector<std::pair<NodeId, GeometryPtr>> modifierGeometries()
{
    std::lock_guard lock(outputs().mutex);
    return {outputs().byModifier.begin(), outputs().byModifier.end()};
}

Node* groupOfModifier(Graph& g, const Node& modifier)
{
    if (!isGnModifier(modifier))
        return nullptr;
    int def = modifier.type().attrIndex("groupDefinition");
    auto src = g.source({modifier.id(), def, -1});
    return src ? g.node(src->node) : nullptr;
}

void commitGroup(Graph& g, Node& groupNode, Group next) { commitGroup(g, groupNode, std::move(next), {}); }

void commitGroup(Graph& g, Node& groupNode, Group next, const std::map<std::string, std::string>& renames)
{
    LILA_ZONE();

    Group prev = readGroup(groupNode);
    next.name = prev.name;
    embedSubgroups(g, next);
    pruneLinks(next);
    pruneTies(next.ties, next.links);
    std::vector<std::string> names;
    for (const GNode& n : next.nodes)
        names.push_back(n.name);
    pruneFrames(next.frames, names);
    if (interfaceOf(next).signature() != interfaceOf(prev).signature())
        next.typeVersion = std::max(registry().maxVersion[prev.name], prev.typeVersion) + 1;
    else
        next.typeVersion = prev.typeVersion;
    bool changed = next.toJson() != prev.toJson();
    setGroupDefinition(g, groupNode, next);
    syncInstances(g, groupNode, next, renames);
    if (changed)
        propagateToUsers(g, next.name);
}

namespace detail
{
/** Used by the commands: a new modifier of the group's type, inserted at the top of a mesh's stack. */
Node* applyGroup(Graph& g, Node& groupNode, Node& mesh)
{
    Group grp = readGroup(groupNode);
    const scene::NodeType& type = ensureType(grp);
    PlugRef top{mesh.id(), scene::MeshNode::kInMesh, -1};
    auto src = g.source(top);
    Node* mod = g.createNode(type, grp.name + "Modifier", scene::kNoNode, false);
    auto* mn = static_cast<scene::ModifierNode*>(mod);
    if (src)
    {
        g.disconnect(top);
        g.connect(*src, {mod->id(), mn->inAttr, -1});
    }
    else
    {
        Node* source = g.createNode(*scene::types::find("meshSource"), {}, scene::kNoNode, true);
        g.setValue({source->id(), scene::MeshSourceNode::kMesh, -1}, g.storedValue(top));
        g.setValue(top, scene::MeshPtr());
        g.connect({source->id(), scene::MeshSourceNode::kOutMesh, -1}, {mod->id(), mn->inAttr, -1});
    }
    // Its owner and objects first: joining the mesh's stack can evaluate it straight away.
    if (int owner = mod->type().attrIndex("ownerMatrix"); owner >= 0)
        g.setValue({mod->id(), owner, -1}, std::get<glm::dmat4>(g.value({mesh.id(), scene::Transform::kWorldMatrix, -1})));
    connectObjects(g, *mod, grp);
    g.connect({mod->id(), mn->outAttr, -1}, top);
    g.connect({groupNode.id(), kDefinitionAttr, -1}, {mod->id(), mod->type().attrIndex("groupDefinition"), -1});
    connectObjects(g, *mod, grp);
    return mod;
}

void createGroupNode(Graph& g, const Group& given)
{
    Group grp = given;
    embedSubgroups(g, grp);
    Node* n = g.createNode(*scene::types::find(kGroupNodeType), grp.name, scene::kNoNode, false);
    if (n->name() != grp.name)
    {
        g.deleteNode(n->id());
        throw std::invalid_argument("a node named " + grp.name + " already exists");
    }
    ensureType(grp);
    setGroupDefinition(g, *n, grp);
    propagateToUsers(g, grp.name);
}
} // namespace detail

void registerTypes()
{
    registerNodeLibrary();
    registerSimulationSupport();
    static const bool watchingView = [] {
        scene::viewState::onChange(touchViewModifiers);
        scene::viewState::installCommandScopes();
        return true;
    }();
    (void)watchingView;
    if (scene::types::find(kGroupNodeType))
        return;
    scene::NodeType t;
    t.name = kGroupNodeType;
    t.icon = "topology-complex";
    t.doc = "Stores a geometry-nodes group in the scene. The modifiers that run it read their definition from here.";
    t.ownAttrs = {{.name = "definition", .type = PlugType::String, .defaultValue = std::string(),
                   .flags = scene::AttrInput | scene::AttrStorable | scene::AttrHidden, .doc = "Geometry-nodes group, JSON."}};
    t.create = [] { return std::make_unique<GroupNode>(); };
    scene::types::registerType(std::move(t));

    // Loading a scene or undoing an edit sets the definition first; the modifier type has to exist before
    // nodes of it are created, so register it right then.
    Graph::current().addListener([](scene::Change c, NodeId id) {
        if (c != scene::Change::Value && c != scene::Change::Created)
            return;
        Node* n = Graph::current().node(id);
        if (n && n->type().name == kGroupNodeType)
            syncTypeFor(*n);
    }, true);
}
} // namespace lila::gn
