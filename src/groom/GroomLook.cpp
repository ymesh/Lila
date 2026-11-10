#include "groom/GroomLook.h"

#include "groom/GroomGraph.h"
#include "groom/GroomNodes.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <groomlab/graph.h>

#include <nlohmann/json.hpp>

#include <algorithm>

namespace lila::groom
{
namespace
{
using json = nlohmann::json;
using scene::Graph;
using scene::Node;
using T = ArgType;

/** The look nodes a groom's palette offers, in menu order. */
const std::vector<std::string>& lookTypes()
{
    static const std::vector<std::string> types = {"Clump", "Noise", "Curl", "Length", "Cut", "Width"};
    return types;
}

json parseGraph(const Node& gr)
{
    const std::string& text = std::get<std::string>(gr.value(gr.type().attrIndex("graph")));
    return json::parse(text.empty() ? groomGraph::defaultGraph() : text);
}

json* nodeNamed(json& g, const std::string& name)
{
    for (json& n : g["nodes"])
        if (n.value("name", std::string()) == name)
            return &n;
    return nullptr;
}

/** The connection feeding node `to`'s strands input, or null. */
json* strandsInto(json& g, const std::string& to)
{
    for (json& c : g["connections"])
        if (c.value("to", std::string()) == to && c.value("input", std::string()) == "strands")
            return &c;
    return nullptr;
}

/** The look chain: the nodes from the hair (Interpolate) to the output along their strands inputs, in order. */
std::vector<std::string> chain(json& g)
{
    std::vector<std::string> out;
    std::string at = g.value("output", std::string("output"));
    for (int guard = 0; guard < 256; ++guard)
    {
        json* c = strandsInto(g, at);
        if (!c)
            break;
        const std::string from = (*c)["from"];
        const json* n = nodeNamed(g, from);
        if (!n || n->value("type", std::string()) == "Interpolate")
            break;
        out.push_back(from);
        at = from;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

json numberOf(const groomlab::ParamValue& v)
{
    if (const auto* b = std::get_if<bool>(&v))
        return *b;
    if (const auto* i = std::get_if<std::int64_t>(&v))
        return *i;
    if (const auto* d = std::get_if<double>(&v))
        return *d;
    return nullptr;
}

/** Each look node with its number and on/off params: name, value, default, range, kind (f float, i int, b bool), doc. */
json listing(json& g)
{
    json out = json::array();
    const groomlab::OperatorRegistry& reg = groomlab::OperatorRegistry::instance();
    for (const std::string& name : chain(g))
    {
        const json& n = *nodeNamed(g, name);
        const std::string type = n.value("type", std::string());
        json params = json::array();
        if (reg.contains(type))
            for (const groomlab::ParamSpec& p : reg.create(type)->paramSpecs())
            {
                const json def = numberOf(p.defaultValue);
                if (def.is_null() || p.advanced || !p.choices.empty())
                    continue;
                const json given = n.contains("params") && n["params"].contains(p.name) ? n["params"][p.name] : def;
                const char kind = def.is_boolean() ? 'b' : def.is_number_integer() ? 'i' : 'f';
                json e = {{"name", p.name}, {"value", given.is_number() || given.is_boolean() ? given : def}, {"default", def},
                          {"kind", std::string(1, kind)}, {"doc", p.description}};
                if (p.softMin || p.min)
                    e["min"] = p.softMin ? *p.softMin : *p.min;
                if (p.softMax || p.max)
                    e["max"] = p.softMax ? *p.softMax : *p.max;
                params.push_back(e);
            }
        out.push_back({{"node", name}, {"type", type}, {"enabled", !n.value("bypass", false)}, {"params", params}});
    }
    return out;
}

void setGraph(Graph& g, Node& gr, const json& graph)
{
    g.setValue({gr.id(), gr.type().attrIndex("graph"), -1}, graph.dump());
}
} // namespace

std::vector<std::string> groomLookTypes()
{
    return lookTypes();
}

void registerLookCommands()
{
    ops::registerCommand({
        .name = "groomLook",
        .doc = "Lists a groom's look nodes (the strands' Clump, Noise, Curl, Length, Cut and Width after they are interpolated) "
               "with their number and on/off settings, or changes them: add one at the end, remove, toggle or set its params. "
               "Returns the list.",
        .positionalDoc = "groom",
        .flags = {{"edit", "e", T::String, "list", "What to do.", {"list", "add", "remove", "toggle", "set"}},
                  {"type", "t", T::String, "Clump", "add: the node type.", lookTypes()},
                  {"node", "n", T::String, nullptr, "remove, toggle, set: the look node."},
                  {"params", "", T::String, nullptr, "set: params as a JSON object, e.g. {\"strength\": 0.5}."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node* gr = nullptr;
            for (const std::string& name : a.objects())
                if (Node* n = g.find(name); n && (gr = findGroom(g, *n)))
                    break;
            if (!gr)
                throw CommandError("groomLook: select a groom");
            json graph = parseGraph(*gr);
            const std::string edit = a.get<std::string>("edit", "list");
            if (edit == "add")
            {
                const std::string type = a.get<std::string>("type", "Clump");
                if (!groomlab::OperatorRegistry::instance().contains(type))
                    throw CommandError("groomLook: no node type " + type);
                std::string base = type;
                std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                std::string name = base;
                for (int i = 1; nodeNamed(graph, name); ++i)
                    name = base + std::to_string(i);
                json* into = strandsInto(graph, graph.value("output", std::string("output")));
                if (!into)
                    throw CommandError("groomLook: the graph's output takes no strands");
                const std::string from = (*into)["from"];
                (*into)["from"] = name;
                graph["nodes"].push_back({{"name", name}, {"type", type}});
                graph["connections"].push_back({{"from", from}, {"to", name}, {"input", "strands"}});
                setGraph(g, *gr, graph);
                json out = listing(graph);
                return {{"node", name}, {"look", out}};
            }
            if (edit != "list")
            {
                const std::string node = a.get<std::string>("node", "");
                const std::vector<std::string> c = chain(graph);
                if (std::find(c.begin(), c.end(), node) == c.end())
                    throw CommandError("groomLook: " + node + " is not one of " + gr->path() + "'s look nodes");
                json& n = *nodeNamed(graph, node);
                if (edit == "toggle")
                    n["bypass"] = !n.value("bypass", false);
                else if (edit == "set")
                {
                    const json p = json::parse(a.get<std::string>("params", "{}"), nullptr, false);
                    if (!p.is_object())
                        throw CommandError("groomLook: params aren't a JSON object");
                    for (const auto& [k, v] : p.items())
                        n["params"][k] = v;
                }
                else if (edit == "remove")
                {
                    json* into = nullptr;
                    for (json& con : graph["connections"])
                        if (con.value("from", std::string()) == node && con.value("input", std::string()) == "strands")
                            into = &con;
                    const json* feed = strandsInto(graph, node);
                    if (into && feed)
                        (*into)["from"] = (*feed)["from"];
                    auto& cons = graph["connections"];
                    cons.erase(std::remove_if(cons.begin(), cons.end(), [&](const json& con) { return con.value("to", std::string()) == node; }), cons.end());
                    auto& nodes = graph["nodes"];
                    nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const json& x) { return x.value("name", std::string()) == node; }), nodes.end());
                }
                setGraph(g, *gr, graph);
            }
            return listing(graph);
        },
    });
    ops::registerCommand({
        .name = "groomPreview",
        .doc = "Shows a groom's look nodes with these params without setting them, for a slider's drag: on the GPU from the "
               "strands as last grown when the look chain allows, else on the CPU. groomEndPreview ends it. Not undoable. Returns "
               "{gpu, why}: why tells what kept it on the CPU.",
        .positionalDoc = "groom",
        .flags = {{"node", "n", T::String, nullptr, "The look node."}, {"params", "", T::String, nullptr, "Params as a JSON object."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node* gr = nullptr;
            for (const std::string& name : a.objects())
                if (Node* n = g.find(name); n && (gr = findGroom(g, *n)))
                    break;
            if (!gr)
                throw CommandError("groomPreview: select a groom");
            const json p = json::parse(a.get<std::string>("params", "{}"), nullptr, false);
            if (!p.is_object())
                throw CommandError("groomPreview: params aren't a JSON object");
            std::map<std::string, double> values;
            for (const auto& [k, v] : p.items())
                if (v.is_number())
                    values[k] = v.get<double>();
            const PreviewResult r = previewLook(g, gr->id(), a.get<std::string>("node", ""), values);
            return {{"gpu", r.gpu}, {"why", r.why}};
        },
    });
    ops::registerCommand({
        .name = "groomEndPreview",
        .doc = "Ends a groom's look preview: it shows its own params again.",
        .positionalDoc = "groom",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            for (const std::string& name : a.objects())
                if (Node* n = g.find(name))
                    if (Node* gr = findGroom(g, *n))
                        endPreviewLook(g, gr->id());
            return true;
        },
    });
}
} // namespace lila::groom
