#include "tex/Expressions.h"

#include "rigmath/Expressions.h"
#include "scene/Anim.h"
#include "tex/Ports.h"

namespace lila::tex
{
namespace
{
bool mentionsTime(const json& expressions)
{
    if (!expressions.is_object())
        return false;
    for (const auto& [param, text] : expressions.items())
        if (text.is_string())
            if (const std::string s = text.get<std::string>(); s.find("time") != std::string::npos || s.find("frame") != std::string::npos)
                return true;
    return false;
}

bool graphJsonUsesTime(const json& g)
{
    for (const json& n : g.value("nodes", json::array()))
        if (n.contains("values") && n["values"].is_object() && mentionsTime(n["values"].value(kExpressionsKey, json())))
            return true;
    const json subgroups = g.value("subgroups", json::object());
    for (const auto& [name, sub] : subgroups.items())
        if (graphJsonUsesTime(sub))
            return true;
    return false;
}
} // namespace

std::string expressionOf(const GNode& node, std::string_view param)
{
    auto e = node.values.find(kExpressionsKey);
    if (e == node.values.end() || !e->is_object())
        return {};
    auto it = e->find(std::string(param));
    return it != e->end() && it->is_string() ? it->get<std::string>() : std::string();
}

double evaluateExpression(const std::string& text, const ExpressionReader& read, double seconds)
{
    std::string body = text;
    if (const size_t at = body.find_first_not_of(" \t"); at != std::string::npos && body[at] == '=')
        body.erase(0, at + 1);
    const std::unique_ptr<scene::expressions::Ast> ast = scene::expressions::parse(body);
    return scene::expressions::evaluate(*ast, read, seconds, scene::anim::frameOf(seconds));
}

double expressionNumber(const json& v, const std::string& what)
{
    if (v.is_number())
        return v.get<double>();
    if (v.is_boolean())
        return v.get<bool>() ? 1.0 : 0.0;
    throw std::invalid_argument(what + " isn't a number");
}

namespace
{
json resolvedAt(const Graph& g, const GNode& n, const std::string& id, const json& fallback, double seconds, int depth)
{
    json stored = fallback;
    if (auto it = n.values.find(id); it != n.values.end())
        stored = *it;
    const std::string text = expressionOf(n, id);
    if (text.empty())
        return stored;
    if (depth > 32)
        throw std::invalid_argument(n.name + "." + id + " reads itself through its expression");
    auto read = [&](const std::string& name) -> double {
        const size_t dot = name.find('.');
        const std::string node = name.substr(0, dot), what = name.substr(dot + 1);
        if (node == "input" && !g.node(node))
        {
            const GraphInput* in = g.input(what);
            if (!in || !in->param)
                throw std::invalid_argument("the graph has no input " + what);
            return expressionNumber(in->defaultValue, name);
        }
        const GNode* other = g.node(node);
        if (!other)
            throw std::invalid_argument("no node " + node);
        const NodePorts ports = portsOf(g, *other);
        json def;
        if (const ParamDecl* p = ports.param(what))
            def = p->defaultValue;
        else if (const PortDecl* in = ports.input(what))
            def = in->defaultValue;
        else
            throw std::invalid_argument(node + " has no parameter " + what);
        return expressionNumber(resolvedAt(g, *other, what, def, seconds, depth + 1), name);
    };
    return evaluateExpression(text, read, seconds);
}
} // namespace

json resolvedValue(const Graph& g, const GNode& n, const std::string& id, const json& fallback, double seconds, std::string* error)
{
    try
    {
        return resolvedAt(g, n, id, fallback, seconds, 0);
    }
    catch (const std::invalid_argument& e)
    {
        if (error)
            *error = e.what();
        auto it = n.values.find(id);
        return it != n.values.end() ? *it : fallback;
    }
}

bool expressionsUseTime(const Graph& g)
{
    for (const GNode& n : g.nodes)
        if (auto e = n.values.find(kExpressionsKey); e != n.values.end() && mentionsTime(*e))
            return true;
    for (const auto& [name, sub] : g.subgroups)
        if (graphJsonUsesTime(sub))
            return true;
    return false;
}
} // namespace lila::tex
