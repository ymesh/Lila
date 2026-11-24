#pragma once

#include "core/Export.h"

#include "tex/Graph.h"

#include <string>

namespace lila::tex
{
struct SetSettings;

/**
 * Evaluates a set's channels on the CPU at size x size and times every pass, compiling left out. Returns {nodes: {node: ms},
 * passes: [{node, ms}], total, backend, size}: nodes sums the passes by the node of the graph on show they run through (the
 * set's own graph when group is empty, else the node group of that name). A fused pass counts toward the node whose output
 * it computes. Needs no scene, so it runs on any thread. Throws std::runtime_error.
 */
LILA_SCENE_API json profileGraph(const Graph& setGraph, const SetSettings& settings, const std::string& group, int size);

/** texProfileGraph. */
LILA_SCENE_API void registerProfileCommands();
} // namespace lila::tex
