#include "gn/Exec.h"
#include "gn/NodesCommon.h"
#include "gn/Zones.h"
#include "gn/GeometryIO.h"
#include "gn/Stateful.h"

#include "core/Profile.h"
#include "geo/Ops.h"

#include <tbb/parallel_for.h>

namespace lila::gn::nodes::zones
{
const json* definitionOf(const Group& outer, const GNode& n)
{
    auto it = n.values.find("group");
    if (it == n.values.end() || !it->is_string())
        return nullptr;
    auto def = outer.subgroups.find(it->get<std::string>());
    return def == outer.subgroups.end() ? nullptr : &def->second;
}

/** The inner group's inputs and outputs, plus the zone's own inputs ahead of them. Inputs the zone drives itself are left off. */
Sockets zoneSockets(const Group& outer, const GNode& n, std::vector<SocketDecl> own, const std::vector<std::string>& driven)
{
    Sockets s;
    const json* def = definitionOf(outer, n);
    if (!def)
        return s;
    Group inner;
    try
    {
        inner = Group::fromJson(*def);
    }
    catch (const std::exception&)
    {
        return s;
    }
    s.inputs.push_back(geoIn("geometry", "Geometry handed to the inner group's Group Input."));
    for (SocketDecl& d : own)
        s.inputs.push_back(std::move(d));
    for (const GInput& in : inner.inputs)
        if (std::find(driven.begin(), driven.end(), in.name) == driven.end() && s.inputIndex(in.name) < 0)
            s.inputs.push_back(fin(in.name, in.type, in.defaultValue, Implicit::None, in.description));
    s.outputs.push_back(geoOut("geometry", "The inner group's output geometry."));
    for (const GOutput& o : inner.outputs)
        s.outputs.push_back(fout(o.name, o.type, o.description));
    return s;
}

/** Parameters for the inner group from this node's inputs, linked or set. */
std::map<std::string, Value> innerParams(const ExecContext& c, const Plan& plan)
{
    std::map<std::string, Value> params;
    for (const GInput& in : plan.group.inputs)
    {
        int k = c.sockets.inputIndex(in.name);
        if (k < 0)
            continue;
        bool linked = !c.group.linksInto(c.node.name, in.name).empty();
        if (linked && !c.inputs[k].empty())
            params[in.name] = c.inputs[k][0];
        else
        {
            auto it = c.node.values.find(in.name);
            params[in.name] = paramValue(in, it != c.node.values.end() ? *it : in.defaultValue);
        }
    }
    return params;
}

std::vector<Value> runInner(const ExecContext& c, const Plan& plan, GeometryPtr input, std::map<std::string, Value> params)
{
    ExecEnv env;
    env.time = c.env.time;
    env.tool = c.env.tool;
    env.input = std::move(input);
    env.params = std::move(params);
    env.objects = c.env.objects;
    env.ownerMatrix = c.env.ownerMatrix;
    try
    {
        return executeOutputs(plan, env);
    }
    catch (const std::exception& e)
    {
        throw ExecError(c.node.name + " (" + plan.group.name + "): " + e.what());
    }
}

/** Sets an Int inner input by name when the group has one. */
void setIndex(std::map<std::string, Value>& params, const Plan& plan, const char* name, int64_t value)
{
    if (const GInput* in = plan.group.input(name))
        params[name] = convertValue(Value{.field = constantInt(value)}, SocketType::Int, in->type);
}

void writeOutputs(ExecContext& c, const Plan& plan, const std::vector<Value>& out)
{
    c.out("geometry", !out.empty() && out[0].geometry ? out[0].geometry : makeGeometry({}));
    for (size_t k = 0; k < plan.group.outputs.size() && k + 1 < out.size(); ++k)
    {
        int o = c.sockets.outputIndex(plan.group.outputs[k].name);
        if (o >= 0)
            c.outputs[o] = convertValue(out[k + 1], plan.group.outputs[k].type, c.sockets.outputs[o].type);
    }
}

PlanPtr planFor(const ExecContext& c)
{
    const json* def = definitionOf(c.group, c.node);
    if (!def)
        throw ExecError(c.node.name + ": no group named '" + c.prop("group") + "'");
    return cachedPlan(*def);
}

/** One geometry per element: mesh parts or faces, curves, instances (each realized in place). */
std::vector<GeometryPtr> splitElements(const Geometry& g, const std::string& by)
{
    std::vector<GeometryPtr> parts;
    if (by == "instance")
    {
        if (!g.instances)
            return parts;
        const Instances& inst = *g.instances;
        for (size_t k = 0; k < inst.size(); ++k)
        {
            int ref = inst.reference[k];
            if (ref < 0 || size_t(ref) >= inst.references.size() || !inst.references[size_t(ref)])
                continue;
            parts.push_back(makeGeometry(realize(transformed(*inst.references[size_t(ref)], inst.transforms[k]))));
        }
        return parts;
    }
    if (by == "curve")
    {
        if (!g.curves)
            return parts;
        for (size_t k = 0; k < g.curves->numCurves(); ++k)
        {
            CurveSet one;
            one.addSpline(g.curves->spline(k));
            parts.push_back(curveGeometry(std::move(one)));
        }
        return parts;
    }
    if (!g.mesh)
        return parts;
    if (by == "face")
    {
        const geo::Mesh& m = *g.mesh;
        for (geo::Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
            {
                geo::Index one[] = {f};
                parts.push_back(meshGeometry(geo::extractFaces(m, one)));
            }
        return parts;
    }
    for (geo::Mesh& part : geo::separate(*g.mesh))
        parts.push_back(meshGeometry(std::move(part)));
    return parts;
}

void repeatNode()
{
    static const std::vector<std::string> driven = {"iteration"};
    registerDecl({
        .type = "RepeatGroup",
        .label = "Repeat",
        .category = "Group",
        .doc = "Runs a node group again and again, each pass taking the last one's results: its output geometry becomes the next "
               "pass's geometry, and each output named like an input feeds that input. An Int input named iteration reads the pass "
               "number, from 0.",
        .props = {prop("group", {}, "", "Name of the node group to repeat.")},
        .dynamicSockets =
            [](const Group& g, const GNode& n) {
                return zoneSockets(g, n, {bounded(in("iterations", T::Int, 1, "Number of passes; 0 passes the input through."), 0, 100000)},
                                   driven);
            },
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                PlanPtr plan = planFor(c);
                std::map<std::string, Value> params = innerParams(c, *plan);
                GeometryPtr geometry = c.geometry("geometry");
                int64_t passes = std::max<int64_t>(0, c.i("iterations"));
                std::vector<Value> out;
                for (int64_t k = 0; k < passes; ++k)
                {
                    setIndex(params, *plan, "iteration", k);
                    out = runInner(c, *plan, geometry, params);
                    geometry = !out.empty() && out[0].geometry ? out[0].geometry : makeGeometry({});
                    for (size_t o = 0; o < plan->group.outputs.size() && o + 1 < out.size(); ++o)
                        if (const GInput* fed = plan->group.input(plan->group.outputs[o].name))
                            params[fed->name] = convertValue(out[o + 1], plan->group.outputs[o].type, fed->type);
                }
                if (out.empty())
                {
                    // No passes: outputs fed by inputs show those inputs.
                    out.resize(1);
                    for (const GOutput& o : plan->group.outputs)
                    {
                        auto it = params.find(o.name);
                        const GInput* fed = plan->group.input(o.name);
                        out.push_back(it != params.end() && fed ? convertValue(it->second, fed->type, o.type) : defaultValue(o.type));
                    }
                }
                out[0].geometry = geometry;
                writeOutputs(c, *plan, out);
            },
    });
}

void forEachNode()
{
    static const std::vector<std::string> driven = {"index", "count"};
    registerDecl({
        .type = "ForEachElement",
        .label = "For Each Element",
        .category = "Group",
        .doc = "Runs a node group once per element of the geometry, each run seeing only that element, and joins what they return. "
               "Int inputs named index and count read the element's number and how many there are. Runs are evaluated in parallel; "
               "data outputs come from the last element.",
        .props = {prop("group", {}, "", "Name of the node group to run per element."),
                  prop("element", {"part", "face", "curve", "instance"}, "part",
                       "part: each connected mesh part; face: each mesh face; curve: each curve; instance: each instance, realized.")},
        .dynamicSockets = [](const Group& g, const GNode& n) { return zoneSockets(g, n, {}, driven); },
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                PlanPtr plan = planFor(c);
                std::map<std::string, Value> params = innerParams(c, *plan);
                std::vector<GeometryPtr> parts = splitElements(*c.geometry("geometry"), c.prop("element"));
                std::vector<std::vector<Value>> results(parts.size());
                tbb::parallel_for(size_t(0), parts.size(), [&](size_t k) {
                    std::map<std::string, Value> p = params;
                    setIndex(p, *plan, "index", int64_t(k));
                    setIndex(p, *plan, "count", int64_t(parts.size()));
                    results[k] = runInner(c, *plan, parts[k], std::move(p));
                });
                std::vector<GeometryPtr> outs;
                outs.reserve(results.size());
                for (const std::vector<Value>& r : results)
                    if (!r.empty() && r[0].geometry)
                        outs.push_back(r[0].geometry);
                std::vector<Value> out = results.empty() || results.back().empty() ? std::vector<Value>(1) : results.back();
                out[0].geometry = makeGeometry(join(outs));
                writeOutputs(c, *plan, out);
            },
    });
}
/** The simulation zone's subgroup, from the definition lowering stored on the node. */
PlanPtr simulationPlan(const GNode& n)
{
    auto it = n.values.find("definition");
    if (it == n.values.end() || !it->is_object())
        throw ExecError(n.name + ": the simulation zone has no body");
    return cachedPlan(*it);
}

bool isCapture(const std::string& name) { return name.starts_with("__capture"); }

/** State between steps: the geometry and each item (the subgroup's outputs). */
json stateOf(const Plan& plan, GeometryPtr geometry, const std::function<Value(const GOutput&, size_t)>& item)
{
    json items = json::object();
    for (size_t k = 0; k < plan.group.outputs.size(); ++k)
        items[plan.group.outputs[k].name] = valueToJson(plan.group.outputs[k].type, item(plan.group.outputs[k], k));
    return {{"geometry", geometryToJson(*geometry)}, {"items", items}};
}

class ZoneSim final : public NodeSim
{
public:
    Setup configure(const SimInputs& latest) override
    {
        Setup s;
        s.startFrame = latest.i("startFrame");
        s.substeps = int(std::clamp<int64_t>(latest.i("substeps"), 1, 64));
        s.paramsHash = std::hash<std::string>()(latest.node().values.value("definition", json()).dump());
        return s;
    }

    void reset(const SimInputs& in, std::vector<uint8_t>& state) override
    {
        PlanPtr plan = simulationPlan(in.node());
        state = toBytes(stateOf(*plan, in.geometry("geometry"), [&](const GOutput& o, size_t) { return in.value(o.name); }));
    }

    void step(const SimInputs& in, std::vector<uint8_t>& state, const scene::sim::StepInfo& at) override
    {
        PlanPtr plan = simulationPlan(in.node());
        json last = fromBytes(state);
        ExecEnv env;
        env.time = at.seconds;
        env.input = geometryFromJson(last.at("geometry"));
        for (const GInput& i : plan->group.inputs)
        {
            if (i.name == "deltaTime")
                env.params[i.name] = Value{.field = constantFloat(at.dt)};
            else if (isCapture(i.name))
                env.params[i.name] = in.value(i.name);
            else if (last["items"].contains(i.name))
                env.params[i.name] = valueFromStored(i.type, last["items"][i.name]);
        }
        std::vector<Value> out;
        try
        {
            out = executeOutputs(*plan, env);
        }
        catch (const std::exception& e)
        {
            throw std::runtime_error(in.node().name + ": " + e.what());
        }
        GeometryPtr geometry = !out.empty() && out[0].geometry ? out[0].geometry : makeGeometry({});
        state = toBytes(stateOf(*plan, geometry, [&](const GOutput& o, size_t k) { return k + 1 < out.size() ? out[k + 1] : defaultValue(o.type); }));
    }
};

void simulationRunner()
{
    static const std::vector<std::string> driven = {"deltaTime"};
    auto stateful = std::make_shared<StatefulDecl>();
    stateful->create = [] { return std::make_unique<ZoneSim>(); };
    stateful->output = [](ExecContext& c, const std::vector<uint8_t>* state) {
        PlanPtr plan = simulationPlan(c.node);
        if (!state)
        {
            c.out("geometry", c.geometry("geometry"));
            for (const GOutput& o : plan->group.outputs)
                if (int k = c.sockets.outputIndex(o.name), i = c.sockets.inputIndex(o.name); k >= 0 && i >= 0 && !c.inputs[size_t(i)].empty())
                    c.outputs[size_t(k)] = c.inputs[size_t(i)][0];
            return;
        }
        json s = fromBytes(*state);
        c.out("geometry", geometryFromJson(s.at("geometry")));
        for (const GOutput& o : plan->group.outputs)
            if (int k = c.sockets.outputIndex(o.name); k >= 0 && s["items"].contains(o.name))
                c.outputs[size_t(k)] = valueFromStored(o.type, s["items"][o.name]);
    };
    registerDecl({
        .type = "SimulationZone",
        .label = "Simulation",
        .category = "Zones",
        .doc = "A simulation zone after evaluation lowered it: steps its subgroup frame by frame.",
        .dynamicSockets =
            [](const Group& g, const GNode& n) {
                return zoneSockets(g, n, {in("startFrame", T::Int, 1), bounded(in("substeps", T::Int, 1), 1, 64)}, driven);
            },
        .stateful = stateful,
        .hidden = true,
    });
}

json itemDefault(SocketType t)
{
    if (t == SocketType::String)
        return "";
    if (!isDataType(t))
        return nullptr;
    return zeroJson(t);
}

void addItemSockets(std::vector<SocketDecl>& list, const std::vector<ZoneItem>& items, bool inputs)
{
    for (const ZoneItem& i : items)
    {
        bool data = isDataType(i.type);
        if (inputs)
            list.push_back(data ? fin(i.name, i.type, itemDefault(i.type)) : in(i.name, i.type, itemDefault(i.type)));
        else
            list.push_back(data ? fout(i.name, i.type) : out(i.name, i.type));
    }
}

const char* lanesDoc(const ZoneKind& k)
{
    return std::string_view(k.input) == "ClosureInput" ? " Its inputs and outputs are set in the Node Editor's side panel."
                                                       : " Values carried through besides the geometry are items, added in the Node Editor's side panel.";
}

/** A zone's input node. own: its inputs ahead of the items; drivenDocs: one per ZoneKind::driven value. */
void zoneInputNode(const ZoneKind& k, std::string doc, std::vector<SocketDecl> own, std::vector<std::string> drivenDocs, std::vector<PropDecl> props = {})
{
    registerDecl({
        .type = k.input,
        .label = std::string(k.label) + " Input",
        .category = "Zones",
        .doc = std::move(doc) + lanesDoc(k),
        .props = std::move(props),
        .dynamicSockets =
            [&k, own, drivenDocs](const Group& g, const GNode& n) {
                Sockets s;
                const GNode* outNode = zoneOutputOf(g, n);
                std::vector<ZoneItem> items = outNode ? itemsOf(*outNode, k.inputItems) : std::vector<ZoneItem>{};
                if (std::string_view(k.input) != "ClosureInput")
                {
                    s.inputs = own;
                    if (k.geometry)
                        s.inputs.push_back(geoIn("geometry", "Geometry going into the zone."));
                    addItemSockets(s.inputs, items, true);
                }
                for (size_t d = 0; d < k.driven.size(); ++d)
                    s.outputs.push_back(fout(k.driven[d].first, k.driven[d].second, d < drivenDocs.size() ? drivenDocs[d] : ""));
                if (k.geometry)
                    s.outputs.push_back(geoOut("geometry"));
                addItemSockets(s.outputs, items, false);
                return s;
            },
    });
}

void zoneOutputNode(const ZoneKind& k, std::string doc)
{
    registerDecl({
        .type = k.output,
        .label = std::string(k.label) + " Output",
        .category = "Zones",
        .doc = std::move(doc),
        .dynamicSockets =
            [&k](const Group&, const GNode& n) {
                Sockets s;
                if (k.geometry)
                    s.inputs.push_back(geoIn("geometry"));
                addItemSockets(s.inputs, itemsOf(n, k.outputItems), true);
                if (std::string_view(k.output) == "ClosureOutput")
                    s.outputs.push_back(out("closure", T::Closure, "The zone as a closure, with the values it reads from outside captured."));
                else
                {
                    s.outputs.push_back(geoOut("geometry"));
                    addItemSockets(s.outputs, itemsOf(n, k.outputItems), false);
                }
                return s;
            },
    });
}

void zoneNodes()
{
    const std::vector<ZoneKind>& kinds = zoneKinds();
    zoneInputNode(kinds[0],
                  "Start of a repeat zone: the nodes between it and Repeat Output run Iterations times, each pass taking the last one's "
                  "geometry and items.",
                  {bounded(in("iterations", T::Int, 1, "Number of passes; 0 passes the inputs through."), 0, 100000)}, {"The pass number, from 0."});
    zoneOutputNode(kinds[0], "End of a repeat zone: what reaches it feeds the next pass, and after the last pass comes out here.");
    zoneInputNode(kinds[1],
                  "Start of a for-each zone: the nodes between it and For Each Output run once per element (each mesh part, face, curve or "
                  "instance), in parallel, each seeing only its element as the geometry.",
                  {}, {"The element's number.", "How many elements there are."},
                  {prop("element", {"part", "face", "curve", "instance"}, "part",
                        "part: each connected mesh part; face: each mesh face; curve: each curve; instance: each instance, realized.")});
    zoneOutputNode(kinds[1], "End of a for-each zone: the elements' geometry joined; items come from the last element.");
    zoneInputNode(kinds[2],
                  "Start of a simulation zone: the nodes between it and Simulation Output step the geometry and items forward one frame at a "
                  "time from Start Frame, each step starting from the last. Items keep single values; store per-element state as attributes.",
                  {in("startFrame", T::Int, 1, "Frame the simulation starts on; before it, the inputs pass through."),
                   bounded(in("substeps", T::Int, 1, "Steps per frame."), 1, 64)},
                  {"Seconds the step covers."});
    zoneOutputNode(kinds[2], "End of a simulation zone: the state at the current frame, cached frame by frame.");
    zoneInputNode(kinds[3], "Start of a closure zone: its outputs are the closure's inputs, given by whichever Evaluate Closure runs it.", {}, {});
    zoneOutputNode(kinds[3],
                   "End of a closure zone: the nodes between Closure Input and here become a closure, a value that Evaluate Closure runs later "
                   "with its own inputs. Values the zone reads from outside are captured where the closure is made.");
}
} // namespace lila::gn::nodes::zones

namespace lila::gn::nodes
{
void registerZoneNodes()
{
    zones::repeatNode();
    zones::forEachNode();
    zones::zoneNodes();
    zones::simulationRunner();
}
} // namespace lila::gn::nodes
