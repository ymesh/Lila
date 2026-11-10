#include "groom/GroomGraph.h"

#include "groom/LockOps.h"

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace lila::groom::groomGraph
{
using json = nlohmann::json;

namespace
{
json parse(const std::string& graph)
{
    json j = json::parse(graph.empty() ? defaultGraph() : graph, nullptr, false);
    if (!j.is_object())
        throw std::runtime_error("the groom graph isn't JSON");
    if (!j.contains("nodes"))
        j["nodes"] = json::array();
    if (!j.contains("connections"))
        j["connections"] = json::array();
    return j;
}

json* nodeNamed(json& g, const std::string& name)
{
    for (json& n : g["nodes"])
        if (n.value("name", std::string()) == name)
            return &n;
    return nullptr;
}

std::optional<Style> styleOfType(const std::string& type)
{
    for (int k = 0; k < int(Style::Count); ++k)
        if (lockStyleType(Style(k)) == type)
            return Style(k);
    return std::nullopt;
}

/** What feeds a node's input, or "". */
std::string sourceOf(const json& g, const std::string& node, const std::string& input)
{
    for (const json& c : g["connections"])
        if (c.value("to", std::string()) == node && c.value("input", std::string()) == input)
            return c.value("from", std::string());
    return {};
}

void unlink(json& g, const std::string& node, const std::string& input)
{
    json kept = json::array();
    for (const json& c : g["connections"])
        if (!(c.value("to", std::string()) == node && c.value("input", std::string()) == input))
            kept.push_back(c);
    g["connections"] = std::move(kept);
}

void link(json& g, const std::string& from, const std::string& to, const std::string& input)
{
    unlink(g, to, input);
    if (!from.empty())
        g["connections"].push_back({{"from", from}, {"to", to}, {"input", input}});
}

/** The chain's style node names, first applied first, and the node that feeds the first. */
std::vector<std::string> chain(const json& g, std::string* head)
{
    std::vector<std::string> out;
    std::string at = sourceOf(g, kLocks, "guides");
    for (int guard = 0; guard < 256 && !at.empty(); ++guard)
    {
        const json* n = nullptr;
        for (const json& m : g["nodes"])
            if (m.value("name", std::string()) == at)
                n = &m;
        if (!n || !styleOfType(n->value("type", std::string())))
            break;
        out.insert(out.begin(), at);
        at = sourceOf(g, at, "guides");
    }
    if (head)
        *head = at;
    return out;
}

/** The node of type Scalp, for the styles that orient by it. */
std::string scalpNode(const json& g)
{
    for (const json& n : g["nodes"])
        if (n.value("type", std::string()) == "Scalp")
            return n.value("name", std::string());
    return {};
}

void relink(json& g, const std::string& head, const std::vector<std::string>& nodes)
{
    std::string prev = head;
    for (const std::string& n : nodes)
    {
        link(g, prev, n, "guides");
        prev = n;
    }
    link(g, prev, kLocks, "guides");
}
} // namespace

std::string defaultGraph()
{
    return R"({"version": 1, "output": "output",
  "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "guides", "type": "Guides"},
    {"name": "locks", "type": "Locks"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 20.0}},
    {"name": "hair", "type": "Interpolate"},
    {"name": "output", "type": "Output"}],
  "connections": [
    {"from": "guides", "to": "locks", "input": "guides"},
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "hair", "input": "strands"},
    {"from": "locks", "to": "hair", "input": "guides"},
    {"from": "scalp", "to": "hair", "input": "scalp"},
    {"from": "hair", "to": "output", "input": "strands"}]})";
}

std::vector<StyleEntry> styles(const std::string& graph)
{
    json g = parse(graph);
    std::vector<StyleEntry> out;
    for (const std::string& name : chain(g, nullptr))
    {
        const json& n = *nodeNamed(g, name);
        out.push_back({name, *styleOfType(n.value("type", std::string())), !n.value("bypass", false)});
    }
    return out;
}

std::string addStyle(std::string& graph, Style kind, int position, const std::string& params)
{
    json g = parse(graph);
    if (!nodeNamed(g, kLocks))
        throw std::runtime_error("the groom graph has no Locks node");
    std::string head;
    std::vector<std::string> nodes = chain(g, &head);
    std::string base = styleName(kind), name = base;
    for (int i = 1; nodeNamed(g, name); ++i)
        name = base + std::to_string(i);
    json n = {{"name", name}, {"type", lockStyleType(kind)}};
    const json p = json::parse(params.empty() ? "{}" : params, nullptr, false);
    if (p.is_object() && !p.empty())
        n["params"] = p;
    g["nodes"].push_back(n);
    const size_t at = position < 0 ? nodes.size() : std::min<size_t>(size_t(position), nodes.size());
    nodes.insert(nodes.begin() + std::ptrdiff_t(at), name);
    relink(g, head, nodes);
    if (const std::string scalp = scalpNode(g); !scalp.empty())
        link(g, scalp, name, "scalp");
    graph = g.dump();
    return name;
}

void removeStyle(std::string& graph, const std::string& node)
{
    json g = parse(graph);
    std::string head;
    std::vector<std::string> nodes = chain(g, &head);
    std::erase(nodes, node);
    json kept = json::array();
    for (const json& n : g["nodes"])
        if (n.value("name", std::string()) != node)
            kept.push_back(n);
    g["nodes"] = std::move(kept);
    json links = json::array();
    for (const json& c : g["connections"])
        if (c.value("from", std::string()) != node && c.value("to", std::string()) != node)
            links.push_back(c);
    g["connections"] = std::move(links);
    relink(g, head, nodes);
    graph = g.dump();
}

void moveStyle(std::string& graph, const std::string& node, int position)
{
    json g = parse(graph);
    std::string head;
    std::vector<std::string> nodes = chain(g, &head);
    if (std::find(nodes.begin(), nodes.end(), node) == nodes.end())
        throw std::runtime_error("no lock style named '" + node + "'");
    std::erase(nodes, node);
    const size_t at = position < 0 ? nodes.size() : std::min<size_t>(size_t(position), nodes.size());
    nodes.insert(nodes.begin() + std::ptrdiff_t(at), node);
    relink(g, head, nodes);
    graph = g.dump();
}

void setParams(std::string& graph, const std::string& node, const std::string& params)
{
    json g = parse(graph);
    json* n = nodeNamed(g, node);
    if (!n)
        throw std::runtime_error("the groom graph has no node named '" + node + "'");
    const json p = json::parse(params, nullptr, false);
    if (!p.is_object())
        throw std::runtime_error("params aren't a JSON object");
    if (!n->contains("params") || !(*n)["params"].is_object())
        (*n)["params"] = json::object();
    for (const auto& [k, v] : p.items())
        (*n)["params"][k] = v;
    graph = g.dump();
}

void setEnabled(std::string& graph, const std::string& node, bool enabled)
{
    json g = parse(graph);
    json* n = nodeNamed(g, node);
    if (!n)
        throw std::runtime_error("the groom graph has no node named '" + node + "'");
    if (enabled)
        n->erase("bypass");
    else
        (*n)["bypass"] = true;
    graph = g.dump();
}

std::string params(const std::string& graph, const std::string& node)
{
    json g = parse(graph);
    json* n = nodeNamed(g, node);
    return n && n->contains("params") ? (*n)["params"].dump() : std::string("{}");
}
} // namespace lila::groom::groomGraph
