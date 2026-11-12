#include "gn/Exec.h"

#include "gn/Bake.h"

#include "core/Profile.h"
#include "gn/Routing.h"
#include "gn/Stateful.h"
#include "gn/Tool.h"
#include "gn/Zones.h"

#include <functional>
#include <map>
#include <mutex>

namespace lila::gn
{
namespace
{
/** Fills Step::dynamic (forward from Scene Time and simulation nodes) and Step::feedsStateful (back from the latter). */
void markDependencies(Plan& plan)
{
    plan.statefulStep = -1;
    for (size_t s = 0; s < plan.steps.size(); ++s)
    {
        Plan::Step& step = plan.steps[s];
        const bool stateful = step.decl->stateful != nullptr;
        if (stateful && plan.statefulStep < 0)
            plan.statefulStep = int(s);
        step.dynamic = stateful || step.decl->type == "SceneTime" || step.decl->type == "IsViewport" || step.decl->type == "ActiveCamera" ||
                       // Nodes with side-panel actions record their inputs for them (gn/Bake.h), so they must run each time.
                       !step.decl->actions.empty() ||
                       (runsGroup(step.decl->type) && (plan.usesTime || plan.usesView)) ||
                       step.decl->type == "Viewer" || isGizmoType(step.decl->type) || int(s) == plan.outputStep;
        for (const std::vector<Plan::Source>& links : step.inputs)
            for (const Plan::Source& src : links)
                step.dynamic = step.dynamic || plan.steps[size_t(src.step)].dynamic;
        step.feedsStateful = false;
    }
    if (plan.statefulStep < 0)
        return;
    plan.statefulInputsUseTime = false;
    for (const std::vector<Plan::Source>& links : plan.steps[size_t(plan.statefulStep)].inputs)
        for (const Plan::Source& src : links)
            plan.statefulInputsUseTime = plan.statefulInputsUseTime || plan.steps[size_t(src.step)].dynamic;
    plan.steps[size_t(plan.statefulStep)].feedsStateful = true;
    for (size_t s = plan.steps.size(); s-- > 0;)
        if (plan.steps[s].feedsStateful)
            for (const std::vector<Plan::Source>& links : plan.steps[s].inputs)
                for (const Plan::Source& src : links)
                    plan.steps[size_t(src.step)].feedsStateful = true;
}
/** Plan::needed: back from the output and viewer, stopping at baked Bake nodes. */
void markNeeded(Plan& plan)
{
    plan.needed.clear();
    bool anyBaked = false;
    for (const Plan::Step& s : plan.steps)
        anyBaked = anyBaked || isBaked(plan.group.nodes[s.node]);
    if (!anyBaked)
        return;
    plan.needed.assign(plan.steps.size(), 0);
    std::vector<int> stack;
    for (int root : {plan.outputStep, plan.viewerStep, plan.statefulStep})
        if (root >= 0)
            stack.push_back(root);
    stack.insert(stack.end(), plan.gizmoSteps.begin(), plan.gizmoSteps.end());
    while (!stack.empty())
    {
        int s = stack.back();
        stack.pop_back();
        if (plan.needed[size_t(s)])
            continue;
        plan.needed[size_t(s)] = 1;
        const Plan::Step& step = plan.steps[size_t(s)];
        if (isBaked(plan.group.nodes[step.node]))
            continue;
        for (const std::vector<Plan::Source>& links : step.inputs)
            for (const Plan::Source& src : links)
                stack.push_back(src.step);
    }
}
} // namespace

/** The group's socket types for muted nodes, read from the definition as it is before routing changes it. */
static RoutingHooks routingHooks(const Group& g)
{
    RoutingHooks h;
    h.ports = [&g](const GNode& n) {
        PortTypes t;
        if (const NodeDecl* d = findDecl(n.type))
        {
            Sockets s = d->socketsFor(g, n);
            for (const SocketDecl& i : s.inputs)
                t.inputs.push_back({i.id, socketTypeName(i.type)});
            for (const SocketDecl& o : s.outputs)
                t.outputs.push_back({o.id, socketTypeName(o.type)});
        }
        return t;
    };
    h.linkable = [](const std::string& from, const std::string& to) {
        SocketType a, b;
        return parseSocketType(from, &a) && parseSocketType(to, &b) && linkable(a, b);
    };
    return h;
}

PlanPtr compile(const Group& g)
{
    LILA_ZONE();

    registerNodeLibrary();
    auto plan = std::make_shared<Plan>();
    plan->group = g;
    resolveRouting(plan->group.nodes, plan->group.links, routingHooks(g));
    if (std::string error = lowerZones(plan->group); !error.empty())
    {
        plan->error = error;
        return plan;
    }
    const Group& grp = plan->group;

    std::map<std::string, size_t, std::less<>> byName;
    for (size_t i = 0; i < grp.nodes.size(); ++i)
        byName[grp.nodes[i].name] = i;

    const GNode* outNode = grp.firstOfType("GroupOutput");
    if (!outNode)
    {
        plan->error = "the group has no Group Output node";
        return plan;
    }

    std::vector<int> stepOf(grp.nodes.size(), -1);
    std::vector<uint8_t> state(grp.nodes.size(), 0); // 1 visiting, 2 done
    std::vector<Sockets> sockets(grp.nodes.size());
    std::vector<const NodeDecl*> decls(grp.nodes.size(), nullptr);
    for (size_t i = 0; i < grp.nodes.size(); ++i)
    {
        decls[i] = findDecl(grp.nodes[i].type);
        if (decls[i])
            sockets[i] = decls[i]->socketsFor(grp, grp.nodes[i]);
    }

    std::function<bool(size_t)> visit = [&](size_t n) -> bool {
        if (state[n] == 2)
            return true;
        if (state[n] == 1)
        {
            plan->error = "the group has a cycle through " + grp.nodes[n].name;
            return false;
        }
        if (!decls[n])
        {
            plan->error = grp.nodes[n].name + ": unknown node type " + grp.nodes[n].type;
            return false;
        }
        state[n] = 1;
        Plan::Step step;
        step.node = n;
        step.decl = decls[n];
        step.sockets = sockets[n];
        step.inputs.resize(step.sockets.inputs.size());
        for (size_t k = 0; k < step.sockets.inputs.size(); ++k)
            for (const GLink& l : grp.links)
            {
                if (l.toNode != grp.nodes[n].name || l.toSocket != step.sockets.inputs[k].id)
                    continue;
                auto it = byName.find(l.fromNode);
                if (it == byName.end() || !decls[it->second])
                    continue;
                int out = sockets[it->second].outputIndex(l.fromSocket);
                if (out < 0)
                    continue;
                if (!visit(it->second))
                    return false;
                step.inputs[k].push_back({stepOf[it->second], out, sockets[it->second].outputs[out].type});
                if (!step.sockets.inputs[k].multi)
                    break;
            }
        stepOf[n] = int(plan->steps.size());
        plan->steps.push_back(std::move(step));
        state[n] = 2;
        return true;
    };
    plan->usesTime = usesSceneTime(grp);
    plan->usesView = usesViewState(grp);
    size_t outIndex = size_t(outNode - grp.nodes.data());
    if (!visit(outIndex))
    {
        plan->steps.clear();
        return plan;
    }
    plan->outputStep = stepOf[outIndex];
    markDependencies(*plan);
    markNeeded(*plan);
    if (const GNode* viewer = activeViewer(grp))
    {
        std::string error = plan->error;
        size_t vi = size_t(viewer - grp.nodes.data());
        // A broken viewer branch only loses the preview, never the result.
        if (visit(vi))
        {
            plan->viewerStep = stepOf[vi];
            markDependencies(*plan);
            markNeeded(*plan);
        }
        plan->error = error;
    }
    {
        const std::string error = plan->error;
        bool added = false;
        for (size_t n = 0; n < grp.nodes.size(); ++n)
            if (isGizmoType(grp.nodes[n].type) && !grp.nodes[n].muted && visit(n))
            {
                plan->gizmoSteps.push_back(stepOf[n]);
                added = true;
            }
        plan->error = error;
        if (added)
        {
            markDependencies(*plan);
            markNeeded(*plan);
        }
    }
    return plan;
}

const GNode* activeViewer(const Group& g)
{
    for (const GNode& n : g.nodes)
        if (n.type == "Viewer")
            if (auto it = n.values.find("active"); it != n.values.end() && it->is_boolean() && it->get<bool>())
                return &n;
    return nullptr;
}

PlanPtr cachedPlan(const json& definition)
{
    static std::mutex mutex;
    static std::map<std::string, PlanPtr> cache;
    std::string key = definition.dump();
    {
        std::lock_guard lock(mutex);
        if (auto it = cache.find(key); it != cache.end())
            return it->second;
    }
    PlanPtr plan = compile(Group::fromJson(definition));
    std::lock_guard lock(mutex);
    if (cache.size() > 128)
        cache.clear();
    cache[key] = plan;
    return plan;
}

std::vector<Value> executeOutputs(const Plan& plan, const ExecEnv& env)
{
    LILA_ZONE();

    if (!plan.error.empty() || plan.outputStep < 0)
        throw ExecError(plan.error.empty() ? "nothing to evaluate" : plan.error);

    std::vector<std::vector<Value>> results(plan.steps.size());
    std::vector<Value> outputs;
    StepCache* cache = env.cache && env.cache->plan == &plan ? env.cache : nullptr;
    if (cache && cache->valid.size() != plan.steps.size())
    {
        cache->valid.assign(plan.steps.size(), 0);
        cache->results.assign(plan.steps.size(), {});
    }
    const bool skipUnneeded = !plan.needed.empty() && !bakeCaptureActive();
    for (size_t s = 0; s < plan.steps.size(); ++s)
    {
        const Plan::Step& step = plan.steps[s];
        const GNode& node = plan.group.nodes[step.node];
        if (env.sampleOnly && !step.feedsStateful)
            continue;
        if (skipUnneeded && !plan.needed[s])
            continue;
        if (cache && !step.dynamic && cache->valid[s])
        {
            results[s] = cache->results[s];
            continue;
        }
        ExecContext ctx(plan.group, node, *step.decl, step.sockets, env);
        ctx.inputs.resize(step.sockets.inputs.size());
        ctx.outputs.resize(step.sockets.outputs.size());
        for (size_t k = 0; k < step.sockets.inputs.size(); ++k)
        {
            const SocketDecl& sd = step.sockets.inputs[k];
            for (const Plan::Source& src : step.inputs[k])
                if (!results[src.step].empty())
                    ctx.inputs[k].push_back(convertValue(results[src.step][src.output], src.type, sd.type));
            if (!ctx.inputs[k].empty() || sd.multi)
                continue;
            auto it = node.values.find(sd.id);
            if (it == node.values.end() && sd.implicit != Implicit::None)
            {
                Value v;
                switch (sd.implicit)
                {
                case Implicit::Position: v.field = positionField(); break;
                case Implicit::Normal: v.field = normalField(); break;
                case Implicit::Index: v.field = indexField(); break;
                default: v.field = idField(); break;
                }
                v.field = convertField(v.field, sd.type);
                ctx.inputs[k].push_back(std::move(v));
                continue;
            }
            ctx.inputs[k].push_back(valueFromJson(sd.type, it != node.values.end() ? *it : sd.defaultValue));
        }
        for (size_t k = 0; k < step.sockets.outputs.size(); ++k)
            ctx.outputs[k] = defaultValue(step.sockets.outputs[k].type);
        bool isViewer = int(s) == plan.viewerStep;
        const bool isGizmo = isGizmoType(step.decl->type);
        if (isGizmo && !env.gizmos)
            continue;
        try
        {
            if (step.decl->stateful)
                execStateful(ctx);
            else if (step.decl->exec)
                step.decl->exec(ctx);
        }
        catch (const std::exception& e)
        {
            if (isViewer || isGizmo)
                continue;
            if (dynamic_cast<const ExecError*>(&e))
                throw;
            throw ExecError(node.name + ": " + e.what());
        }
        if (int(s) == plan.outputStep)
            for (size_t k = 0; k < ctx.inputs.size(); ++k)
                outputs.push_back(ctx.inputs[k].empty() ? defaultValue(step.sockets.inputs[k].type) : ctx.inputs[k][0]);
        if (cache && !step.dynamic)
        {
            cache->results[s] = ctx.outputs;
            cache->valid[s] = 1;
        }
        results[s] = std::move(ctx.outputs);
        if (env.sampleOnly && int(s) == plan.statefulStep)
            break;
    }
    return outputs;
}

GeometryPtr execute(const Plan& plan, const ExecEnv& env)
{
    std::vector<Value> out = executeOutputs(plan, env);
    if (!out.empty() && out[0].geometry)
        return out[0].geometry;
    return makeGeometry({});
}
} // namespace lila::gn
