#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

class QAction;
class QWidget;

/** Side-panel editors for texture-graph node parameters and unlinked inputs. */
namespace lila::ui::texParams
{
using json = nlohmann::json;

/** changed gets the parameter's id and new value. */
using SetFn = std::function<void(const std::string& id, const json& value)>;
/** Makes the input with this id an input of the node group the node is in. */
using ExposeFn = std::function<void(const std::string& id)>;
/** An action for a parameter's right-click menu (owned by owner, the row's label), or null for none. */
using ParamActionFn = std::function<QAction*(const std::string& id, QWidget* owner)>;

/**
 * Editors for one node as texGraphInfo lists it ("nodes" entry): its parameters (float and int sliders, enums, bools,
 * colour swatches, gradient and curve editors, seeds with a randomise button, image pickers, code editors, scene node pickers),
 * then its unlinked inputs, then its "settings" (output size and bit depth) under Output. A graph node's parameters are its
 * exposed ones. Every edit goes to set. node's "set", when given, names the texture set, for editors that read its bake (a
 * camera picker filling position-map bounds). An empty title (no title, label or type) leaves the heading out.
 */
LILA_UI_API QWidget* nodePanel(QWidget* parent, const json& node, SetFn set, ExposeFn expose = {}, ParamActionFn paramAction = {});

/** Colour stops on a bar: click the bar to add one, drag to move, double-click to pick its colour, right-click to remove. */
LILA_UI_API QWidget* gradientEditor(QWidget* parent, const json& value, std::function<void(const json&)> changed);
/** Points on a 0 to 1 graph: click to add, drag to move, right-click to remove; smooth or linear like the evaluator. */
LILA_UI_API QWidget* curveEditor(QWidget* parent, const json& value, std::function<void(const json&)> changed);

/** Stops as [[t, [r, g, b, a]], ...] (linear) from any value the evaluator accepts. */
LILA_UI_API json gradientStops(const json& value);
/** Points as [[x, y], ...], sorted by x. */
LILA_UI_API json curvePoints(const json& value);
/** The curve's value at x, matching the evaluator (Hermite through the points when smooth). */
LILA_UI_API double evalCurve(const json& points, bool smooth, double x);
} // namespace lila::ui::texParams
