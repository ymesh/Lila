#include "tex/GraphFiles.h"

#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "tex/Graph.h"
#include "tex/NodeDef.h"
#include "tex/Ports.h"
#include "tex/TextureSet.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <set>

namespace lila::tex
{
namespace
{
std::string paramArg(const Args& a, size_t i, const char* cmd, const char* what)
{
    if (a.positional.size() <= i || !a.positional[i].is_string())
        throw CommandError(std::string(cmd) + ": " + what + " required");
    return a.positional[i].get<std::string>();
}

std::pair<std::string, std::string> splitParamTarget(const std::string& plug)
{
    const size_t dot = plug.find('.');
    if (dot == std::string::npos)
        return {plug, {}};
    return {plug.substr(0, dot), plug.substr(dot + 1)};
}

Graph readParamGraph(const std::string& address, const char* cmd)
{
    try
    {
        return readGraphAt(scene::Graph::current(), address);
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
}

bool isSetGraph(const std::string& address) { return splitAddress(address).group.empty(); }

GraphInput& paramNamed(Graph& g, const std::string& name, const char* cmd)
{
    auto it = std::find_if(g.inputs.begin(), g.inputs.end(), [&](const GraphInput& in) { return in.name == name; });
    if (it == g.inputs.end())
        throw CommandError(std::string(cmd) + ": " + g.name + " has no parameter '" + name + "'");
    return *it;
}

json* findPreset(Graph& g, const std::string& name)
{
    for (json& p : g.presets)
        if (p.value("name", "") == name)
            return &p;
    return nullptr;
}

json& presetOrThrow(Graph& g, const std::string& name, const char* cmd)
{
    json* p = findPreset(g, name);
    if (!p)
        throw CommandError(std::string(cmd) + ": " + g.name + " has no preset '" + name + "'");
    return *p;
}

/**
 * A parameter's value. On a set's own graph a parameter drives its target nodes directly, so the first target's value is
 * the one in use; elsewhere (a node group) the input's default is.
 */
json paramValueOf(const Graph& g, const GraphInput& in, bool top)
{
    if (top && in.param && !in.targets.empty())
    {
        const auto [node, id] = splitParamTarget(in.targets.front());
        if (const GNode* n = g.node(node))
        {
            if (auto it = n->values.find(id); it != n->values.end())
                return *it;
            if (const ParamDecl* p = portsOf(g, *n).param(id))
                return p->defaultValue;
        }
    }
    return in.defaultValue;
}

void setParamValue(Graph& g, GraphInput& in, const json& value, bool top, const char* cmd)
{
    if (in.param && !paramValueValid(in.paramType, value, in.options))
        throw CommandError(std::string(cmd) + ": " + in.name + " takes a " + paramTypeName(in.paramType) + ", got " + value.dump());
    in.defaultValue = value;
    if (!top || !in.param)
        return;
    for (const std::string& t : in.targets)
    {
        const auto [node, id] = splitParamTarget(t);
        if (GNode* n = g.node(node))
            n->values[id] = value;
    }
}

json graphParamJson(const Graph& g, const GraphInput& in, bool top)
{
    json o = {{"name", in.name},
              {"label", in.label},
              {"kind", in.param ? "param" : "port"},
              {"type", in.param ? paramTypeName(in.paramType) : portTypeName(in.port)},
              {"value", paramValueOf(g, in, top)},
              {"targets", in.targets}};
    if (!in.description.empty())
        o["description"] = in.description;
    if (!in.options.empty())
        o["options"] = in.options;
    for (auto [key, v] : {std::pair{"min", &in.min}, {"max", &in.max}, {"softMin", &in.softMin}, {"softMax", &in.softMax}, {"step", &in.step}})
        if (*v)
            o[key] = **v;
    return o;
}

json presetSnapshot(const Graph& g, bool top)
{
    json values = json::object();
    for (const GraphInput& in : g.inputs)
        values[in.name] = paramValueOf(g, in, top);
    return values;
}

std::string uniqueParamName(const Graph& g, const std::string& base)
{
    if (!g.input(base))
        return base;
    for (int i = 1;; ++i)
        if (std::string c = base + std::to_string(i); !g.input(c))
            return c;
}

bool validParamName(const std::string& n)
{
    return !n.empty() && n.front() != '@' && n.find('/') == std::string::npos && n.find('.') == std::string::npos;
}
} // namespace

void registerParamCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "texExposeParam",
        .doc = "Make a node's parameter a parameter of its graph: of the texture set (\"Set\"), edited in the graph's Parameters panel "
               "and stored in its presets, or of a node group (\"Set/Group\"), set on each Group node running it. Returns the "
               "parameter's name, made unique.",
        .positionalDoc = "graph node.param",
        .flags = {{"name", "", T::String, nullptr, "Wanted name; the parameter's id when left out."},
                  {"label", "", T::String, nullptr, "Shown instead of the name; the node parameter's label when left out."}},
        .run = [](const Args& a) -> json {
            const std::string address = paramArg(a, 0, "texExposeParam", "graph");
            const std::string plug = paramArg(a, 1, "texExposeParam", "node.param");
            const auto [nn, id] = splitParamTarget(plug);
            if (id.empty())
                throw CommandError("texExposeParam: expected node.param, got '" + plug + "'");
            Graph g = readParamGraph(address, "texExposeParam");
            const GNode* n = g.node(nn);
            if (!n)
                throw CommandError("texExposeParam: no node '" + nn + "' in " + address);
            const NodePorts ports = portsOf(g, *n);
            const ParamDecl* decl = ports.param(id);
            if (!decl || decl->hidden)
                throw CommandError("texExposeParam: " + nn + " has no parameter '" + id + "'");
            for (const GraphInput& in : g.inputs)
                if (std::ranges::find(in.targets, plug) != in.targets.end())
                    throw CommandError("texExposeParam: " + plug + " is already the parameter '" + in.name + "'");
            const std::string base = a.get<std::string>("name", id);
            if (!validParamName(base))
                throw CommandError("texExposeParam: '" + base + "' can't name a parameter (no / or ., no leading @)");
            GraphInput in;
            in.name = uniqueParamName(g, base);
            in.param = true;
            in.paramType = decl->type;
            auto v = n->values.find(id);
            in.defaultValue = v != n->values.end() ? *v : decl->defaultValue;
            in.label = a.get<std::string>("label", decl->label);
            in.description = decl->doc;
            in.options = decl->options;
            in.min = decl->min;
            in.max = decl->max;
            in.softMin = decl->softMin;
            in.softMax = decl->softMax;
            in.step = decl->step;
            in.targets = {plug};
            g.inputs.push_back(in);
            writeGraphAt(scene::Graph::current(), address, g);
            return in.name;
        },
        .examples = {"texExposeParam(\"RockTex\", \"Noise.scale\", label=\"Grain\")"},
    });

    ops::registerCommand({
        .name = "texUnexposeParam",
        .doc = "Remove a graph parameter. The nodes it drove keep its current value, and presets forget it.",
        .positionalDoc = "graph name",
        .run = [](const Args& a) -> json {
            const std::string address = paramArg(a, 0, "texUnexposeParam", "graph");
            const std::string name = paramArg(a, 1, "texUnexposeParam", "name");
            Graph g = readParamGraph(address, "texUnexposeParam");
            if (!paramNamed(g, name, "texUnexposeParam").param)
                throw CommandError("texUnexposeParam: '" + name + "' is a socket input; remove it with texRemoveInput");
            std::erase_if(g.inputs, [&](const GraphInput& in) { return in.name == name; });
            for (json& p : g.presets)
                if (p.contains("values") && p["values"].is_object())
                    p["values"].erase(name);
            writeGraphAt(scene::Graph::current(), address, g);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "texSetGraphParam",
        .doc = "Set a graph parameter's value. On a texture set's graph it sets every node parameter it drives; on a node group it "
               "sets the default its Group nodes use.",
        .positionalDoc = "graph name value",
        .run = [](const Args& a) -> json {
            const std::string address = paramArg(a, 0, "texSetGraphParam", "graph");
            const std::string name = paramArg(a, 1, "texSetGraphParam", "name");
            if (a.positional.size() < 3)
                throw CommandError("texSetGraphParam: value required");
            Graph g = readParamGraph(address, "texSetGraphParam");
            setParamValue(g, paramNamed(g, name, "texSetGraphParam"), a.positional[2], isSetGraph(address), "texSetGraphParam");
            writeGraphAt(scene::Graph::current(), address, g);
            return nullptr;
        },
        .examples = {"texSetGraphParam(\"RockTex\", \"scale\", 12.0)"},
    });

    ops::registerCommand({
        .name = "texGraphParams",
        .doc = "A graph's parameters and presets: {params: [{name, label, kind, type, value, targets, options, min, max, softMin, "
               "softMax, step, description}], presets: [names], active, seed}. active names the first preset whose values all match "
               "the parameters now; empty when none does. seed: a set's graph seed (texSetGraphSeed).",
        .positionalDoc = "graph",
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::string address = paramArg(a, 0, "texGraphParams", "graph");
            const Graph g = readParamGraph(address, "texGraphParams");
            json params = json::array(), presets = json::array();
            for (const GraphInput& in : g.inputs)
                params.push_back(graphParamJson(g, in, isSetGraph(address)));
            const json now = presetSnapshot(g, isSetGraph(address));
            std::string active;
            for (const json& p : g.presets)
            {
                presets.push_back(p.value("name", ""));
                const json values = p.value("values", json::object());
                bool matches = !values.empty() || p.contains("seed");
                for (const auto& [key, value] : values.items())
                    matches = matches && (!now.contains(key) || now[key] == value);
                matches = matches && (!p.contains("seed") || p["seed"] == g.seed);
                if (active.empty() && matches)
                    active = p.value("name", "");
            }
            json out = {{"params", params}, {"presets", presets}, {"active", active}};
            if (isSetGraph(address))
                out["seed"] = g.seed;
            return out;
        },
    });

    ops::registerCommand({
        .name = "texSavePreset",
        .doc = "Store every graph parameter's current value (and a set's graph seed) under a preset name, replacing a preset of that "
               "name. Returns the name.",
        .positionalDoc = "graph name",
        .run = [](const Args& a) -> json {
            const std::string address = paramArg(a, 0, "texSavePreset", "graph");
            const std::string name = paramArg(a, 1, "texSavePreset", "name");
            if (name.empty())
                throw CommandError("texSavePreset: name required");
            Graph g = readParamGraph(address, "texSavePreset");
            json entry = {{"name", name}, {"values", presetSnapshot(g, isSetGraph(address))}};
            if (isSetGraph(address))
                entry["seed"] = g.seed;
            if (json* p = findPreset(g, name))
                *p = std::move(entry);
            else
                g.presets.push_back(std::move(entry));
            writeGraphAt(scene::Graph::current(), address, g);
            return name;
        },
        .examples = {"texSavePreset(\"RockTex\", \"Mossy\")"},
    });

    ops::registerCommand({
        .name = "texApplyPreset",
        .doc = "Set the graph parameters (and a set's graph seed) to a preset's values. Parameters added since it was saved keep "
               "theirs. Returns the names set.",
        .positionalDoc = "graph name",
        .run = [](const Args& a) -> json {
            const std::string address = paramArg(a, 0, "texApplyPreset", "graph");
            const std::string name = paramArg(a, 1, "texApplyPreset", "name");
            Graph g = readParamGraph(address, "texApplyPreset");
            const json& preset = presetOrThrow(g, name, "texApplyPreset");
            const json values = preset.value("values", json::object());
            if (isSetGraph(address) && preset.contains("seed") && preset["seed"].is_number_integer())
                g.seed = preset["seed"].get<int>();
            json set = json::array();
            for (GraphInput& in : g.inputs)
                if (auto it = values.find(in.name); it != values.end())
                {
                    setParamValue(g, in, *it, isSetGraph(address), "texApplyPreset");
                    set.push_back(in.name);
                }
            writeGraphAt(scene::Graph::current(), address, g);
            return set;
        },
    });

    ops::registerCommand({
        .name = "texRenamePreset",
        .doc = "Rename a graph preset. Returns the new name.",
        .positionalDoc = "graph name newName",
        .run = [](const Args& a) -> json {
            const std::string address = paramArg(a, 0, "texRenamePreset", "graph");
            const std::string from = paramArg(a, 1, "texRenamePreset", "name"), to = paramArg(a, 2, "texRenamePreset", "new name");
            if (to.empty())
                throw CommandError("texRenamePreset: new name required");
            Graph g = readParamGraph(address, "texRenamePreset");
            json& p = presetOrThrow(g, from, "texRenamePreset");
            if (from == to)
                return to;
            if (findPreset(g, to))
                throw CommandError("texRenamePreset: " + g.name + " already has a preset '" + to + "'");
            p["name"] = to;
            writeGraphAt(scene::Graph::current(), address, g);
            return to;
        },
    });

    ops::registerCommand({
        .name = "texDeletePreset",
        .doc = "Delete a graph preset. The parameters keep their values.",
        .positionalDoc = "graph name",
        .run = [](const Args& a) -> json {
            const std::string address = paramArg(a, 0, "texDeletePreset", "graph");
            const std::string name = paramArg(a, 1, "texDeletePreset", "name");
            Graph g = readParamGraph(address, "texDeletePreset");
            presetOrThrow(g, name, "texDeletePreset");
            g.presets.erase(std::remove_if(g.presets.begin(), g.presets.end(), [&](const json& p) { return p.value("name", "") == name; }),
                            g.presets.end());
            writeGraphAt(scene::Graph::current(), address, g);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "texSetGraphSeed",
        .doc = "Set a texture set's graph seed: added to every seed parameter in the set, node groups included, so one number "
               "re-rolls the whole material. Presets store it. Returns the seed.",
        .positionalDoc = "set [seed]",
        .flags = {{"random", "", T::Bool, false, "Pick a new seed at random instead."}},
        .run = [](const Args& a) -> json {
            const std::string address = paramArg(a, 0, "texSetGraphSeed", "texture set");
            if (!isSetGraph(address))
                throw CommandError("texSetGraphSeed: the seed belongs to the set; give \"" + splitAddress(address).set + "\"");
            Graph g = readParamGraph(address, "texSetGraphSeed");
            if (a.get<bool>("random", false))
            {
                static std::mt19937 rng{std::random_device{}()};
                g.seed = int(std::uniform_int_distribution<int>(1, 99999)(rng));
            }
            else if (a.positional.size() > 1 && a.positional[1].is_number())
                g.seed = int(std::llround(a.positional[1].get<double>()));
            else
                throw CommandError("texSetGraphSeed: seed or random=True required");
            writeGraphAt(scene::Graph::current(), address, g);
            return g.seed;
        },
        .examples = {"texSetGraphSeed(\"RockTex\", 42)", "texSetGraphSeed(\"RockTex\", random=True)"},
    });
}
} // namespace lila::tex
