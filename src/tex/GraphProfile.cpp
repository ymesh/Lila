#include "tex/GraphProfile.h"

#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "tex/Evaluate.h"
#include "tex/Graph.h"
#include "tex/Plan.h"
#include "tex/TextureSet.h"

#include <map>
#include <string>
#include <vector>

namespace lila::tex
{
namespace
{
std::vector<std::string> pathSegments(const std::string& path)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (true)
    {
        const size_t slash = path.find('/', start);
        out.push_back(path.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
        if (slash == std::string::npos)
            return out;
        start = slash + 1;
    }
}

/**
 * The node of the graph on show that a pass's node path runs through: its first step on the set's own graph, or the step
 * after a Group node running the group. Empty when the pass lies outside the group.
 */
std::string shownNodeOf(const Graph& top, const std::string& group, const std::string& passNode)
{
    const std::vector<std::string> steps = pathSegments(passNode);
    if (group.empty())
        return steps.front();
    const Graph* at = &top;
    Graph inner;
    for (size_t i = 0; i + 1 < steps.size(); ++i)
    {
        const GNode* n = at->node(steps[i]);
        if (!n || n->type != kGroupType)
            return {};
        const std::string runs = n->values.value("group", std::string());
        if (runs == group)
            return steps[i + 1];
        auto it = top.subgroups.find(runs);
        if (it == top.subgroups.end())
            return {};
        try
        {
            inner = Graph::fromJson(it->second);
        }
        catch (const std::invalid_argument&)
        {
            return {};
        }
        at = &inner;
    }
    return {};
}
} // namespace

json profileGraph(const Graph& setGraph, const SetSettings& settings, const std::string& group, int size)
{
    PlanRequest req;
    req.width = req.height = std::max(size, 8);
    req.outputs = channelRequests(setGraph, settings);
    const Plan plan = buildPlan(setGraph, req);
    if (!plan.error.empty())
        throw std::runtime_error(plan.error);
    const EvalResult r = evaluateCpu(plan);
    json nodes = json::object(), passes = json::array();
    double total = 0.0;
    for (size_t i = 0; i < plan.passes.size() && i < r.passMilliseconds.size(); ++i)
    {
        const double ms = r.passMilliseconds[i];
        total += ms;
        passes.push_back({{"node", plan.passes[i].node}, {"ms", ms}});
        if (const std::string shown = shownNodeOf(setGraph, group, plan.passes[i].node); !shown.empty())
            nodes[shown] = nodes.value(shown, 0.0) + ms;
    }
    return {{"nodes", nodes}, {"passes", passes}, {"total", total}, {"backend", "cpu"}, {"size", req.width}};
}

void registerProfileCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "texProfileGraph",
        .doc = "Time a texture set's passes on the CPU at a square size: {nodes: {node: ms}, passes: [{node, ms}], total, backend, size}. "
               "nodes sums the passes by the node of the graph given (\"Set\" or \"Set/Group\") they run through; a pass fusing "
               "several nodes counts toward the one whose output it computes. Compile time is left out.",
        .positionalDoc = "graph",
        .flags = {{"size", "", T::Int, 256, "Width and height to evaluate at."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("texProfileGraph: graph required");
            const GraphAddress at = splitAddress(a.positional[0].get<std::string>());
            scene::Node* n = findSet(scene::Graph::current(), at.set);
            if (!n)
                throw CommandError("texProfileGraph: no texture set named '" + at.set + "'");
            try
            {
                return profileGraph(readGraph(*n), readSettings(*n), at.group, a.get<int>("size", 256));
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("texProfileGraph: ") + e.what());
            }
        },
    });
}
} // namespace lila::tex
