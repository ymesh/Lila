#pragma once

#include "core/Export.h"

#include "tex/Graph.h"

#include <functional>
#include <string>
#include <string_view>

/**
 * Parameter expressions: a number parameter of a texture node computed from other values instead of set, in the syntax
 * of plug expressions (rigmath/Expressions.h): `Noise.scale * 2`, `input.wear * 0.5 + 0.1`, `sin(time) * 0.5 + 0.5`.
 * `Node.param` reads another node's parameter or unlinked input in the same graph, `input.name` one of the graph's exposed
 * inputs (a node group's, or the set's), and time (seconds) and frame the scene's time. The planner evaluates them, so an
 * expression changes values, never the plan's passes.
 */
namespace lila::tex
{
/** Node value {param: expression text}. */
inline constexpr const char* kExpressionsKey = "expressions";

/** A node parameter's expression; empty for none. */
LILA_SCENE_API std::string expressionOf(const GNode& node, std::string_view param);

/** Reads the value behind a name as written in the expression ("Noise.scale", "input.wear"); throws std::invalid_argument when there is none. */
using ExpressionReader = std::function<double(const std::string& name)>;

/** The expression's value at seconds. Throws std::invalid_argument naming what is wrong. */
LILA_SCENE_API double evaluateExpression(const std::string& text, const ExpressionReader& read, double seconds);

/** A JSON value as an expression reads it: numbers and bools; anything else throws std::invalid_argument naming what. */
LILA_SCENE_API double expressionNumber(const json& v, const std::string& what);

/**
 * A node's parameter or input value as the top level of g sees it at seconds: its expression's value, else what it holds,
 * else fallback. A broken expression gives what it holds and sets *error.
 */
LILA_SCENE_API json resolvedValue(const Graph& g, const GNode& n, const std::string& id, const json& fallback, double seconds,
                                  std::string* error = nullptr);

/** Whether any expression in the graph or its subgroups reads time or frame. */
LILA_SCENE_API bool expressionsUseTime(const Graph& g);
} // namespace lila::tex
