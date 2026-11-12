#include "gn/Exec.h"
#include "gn/NodesCommon.h"

namespace lila::gn::nodes
{
namespace
{
/** The definition a Group node runs, embedded in the group that holds it. */
const json* innerDefinition(const Group& outer, const GNode& n)
{
    auto it = n.values.find("group");
    if (it == n.values.end() || !it->is_string())
        return nullptr;
    auto def = outer.subgroups.find(it->get<std::string>());
    return def == outer.subgroups.end() ? nullptr : &def->second;
}

Sockets groupSockets(const Group& outer, const GNode& n)
{
    Sockets s;
    const json* def = innerDefinition(outer, n);
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
    for (const GInput& in : inner.inputs)
    {
        SocketDecl d = fin(in.name, in.type, in.defaultValue, Implicit::None, in.description);
        s.inputs.push_back(std::move(d));
    }
    s.outputs.push_back(geoOut("geometry", "The inner group's output geometry."));
    for (const GOutput& o : inner.outputs)
        s.outputs.push_back(fout(o.name, o.type, o.description));
    return s;
}

void groupNode()
{
    registerDecl({
        .type = kGroupNodeDeclType,
        .label = "Group",
        .category = "Group",
        .doc = "Runs another node group inside this one. Its exposed inputs are this node's inputs and its outputs are this node's "
               "outputs. Editing that group updates every group using it.",
        .props = {prop("group", {}, "", "Name of the node group to run.")},
        .dynamicSockets = groupSockets,
        .exec =
            [](ExecContext& c) {
                const json* def = innerDefinition(c.group, c.node);
                if (!def)
                    throw ExecError(c.node.name + ": no group named '" + c.prop("group") + "'");
                PlanPtr plan = cachedPlan(*def);
                ExecEnv env;
                env.time = c.env.time;
                env.tool = c.env.tool;
                env.input = c.geometry("geometry");
                for (const GInput& in : plan->group.inputs)
                {
                    int k = c.sockets.inputIndex(in.name);
                    if (k < 0)
                        continue;
                    bool linked = !c.group.linksInto(c.node.name, in.name).empty();
                    if (linked && !c.inputs[k].empty())
                        env.params[in.name] = c.inputs[k][0];
                    else
                    {
                        auto it = c.node.values.find(in.name);
                        env.params[in.name] = paramValue(in, it != c.node.values.end() ? *it : in.defaultValue);
                    }
                }
                std::vector<Value> out;
                try
                {
                    out = executeOutputs(*plan, env);
                }
                catch (const std::exception& e)
                {
                    throw ExecError(c.node.name + " (" + plan->group.name + "): " + e.what());
                }
                if (!out.empty())
                    c.out("geometry", out[0].geometry ? out[0].geometry : makeGeometry({}));
                for (size_t k = 0; k < plan->group.outputs.size() && k + 1 < out.size(); ++k)
                {
                    int o = c.sockets.outputIndex(plan->group.outputs[k].name);
                    if (o >= 0)
                        c.outputs[o] = convertValue(out[k + 1], plan->group.outputs[k].type, c.sockets.outputs[o].type);
                }
            },
    });
}

void viewerNode()
{
    registerDecl({
        .type = "Viewer",
        .label = "Viewer",
        .category = "Output",
        .doc = "Previews geometry at this point of the graph in the viewport, coloured by the value field, while the viewer is active "
               "(gnSetActiveViewer, or click its eye in the Node Editor). The modifier's result is unchanged.",
        .props = {prop("dataType", {"float", "int", "vector", "color", "bool"}, "float"), prop("domain", {"auto", "point", "edge", "face", "corner", "instance"}, "auto")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                SocketType t = dataTypeOf(findDecl("Viewer")->propValue(n, "dataType"));
                return Sockets{{geoIn("geometry"), fin("value", t, zeroJson(t), Implicit::None, "Field to colour the preview by.")}, {}};
            },
        .exec =
            [](ExecContext& c) {
                ViewerSink* sink = c.env.viewer;
                if (!sink || sink->captured)
                    return;
                auto active = c.node.values.find("active");
                if (active == c.node.values.end() || !active->is_boolean() || !active->get<bool>())
                    return;
                sink->node = c.node.name;
                sink->geometry = c.geometry("geometry");
                sink->type = dataTypeOf(c.prop("dataType"));
                std::string dom = c.prop("domain");
                sink->domain = dom == "auto" ? Domain::Point : domainOf(dom);
                bool linked = !c.group.linksInto(c.node.name, "value").empty();
                sink->field = linked ? c.field("value") : nullptr;
                sink->captured = true;
            },
    });
}
} // namespace

void registerGroupNodes()
{
    groupNode();
    viewerNode();
}
} // namespace lila::gn::nodes
