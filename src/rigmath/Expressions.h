#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;

/**
 * Expressions on plugs, in Python syntax: `ctrl.ry * 0.5 + clamp(foot.roll, 0, 90)`. The subset of numbers, other plugs
 * by name, time and frame, arithmetic, comparisons, and/or/not, `a if c else b` and the math functions below compiles to
 * rig math nodes, so it evaluates as fast as nodes, shows in the node editor and goes to Unreal as a program; anything else
 * runs as Python in a pythonExpression node (slower, Lila only). Every node an expression makes carries a hidden
 * lilaExpression attr naming its target.
 */
namespace expressions
{
/** A parsed expression. */
struct Ast
{
    enum Kind : uint8_t
    {
        Number,
        Plug,
        Unary,  // op: "-", "not"
        Binary, // op: + - * / % ** and or
        Compare, // op: < <= > >= == !=
        Ternary, // args: then, condition, else
        Call,    // op: function name
        Time,    // op: "time" (seconds) or "frame"
    };
    Kind kind = Number;
    double number = 0.0;
    std::string op;   // operator, function name or plug path
    std::vector<std::unique_ptr<Ast>> args;
};

/** Parses the Python subset; throws std::invalid_argument naming what it can't read. */
LILA_SCENE_API std::unique_ptr<Ast> parse(const std::string& text);
/** The functions the subset compiles: name to the rig math function it becomes. */
LILA_SCENE_API const std::vector<std::pair<std::string, std::string>>& functions();
/** Whether text compiles to rig math nodes; else why not, in why. */
LILA_SCENE_API bool compiles(const std::string& text, std::string* why = nullptr);

/**
 * The value of a parsed expression with no scene nodes behind it, by the same ops the rig math nodes run (ExpressionVm.h):
 * plug reads a plug's value by its name as written. Throws std::invalid_argument for an unknown function or a wrong
 * argument count, and lets plug's own exceptions through.
 */
LILA_SCENE_API double evaluate(const Ast& a, const std::function<double(const std::string& plug)>& plug, double time, double frame);

/** Python for the expressions that don't compile: code with plugs as v0, v1 ... and their values, plus time and frame. */
using PythonEvaluator =
    std::function<std::optional<double>(const std::string& code, const std::vector<double>& values, double time, double frame, std::string* error)>;
LILA_SCENE_API void setPythonEvaluator(PythonEvaluator fn);

/**
 * A native expression as a program for lila_expr (ExpressionVm.h): its text form, and the plugs its inputs i0, i1 ... read.
 * Empty text with why set when the plug has no expression, or one that runs as Python or was hand-edited past what a
 * program holds.
 */
struct ProgramText
{
    std::string text;
    std::vector<PlugRef> inputs;
    bool usesTime = false;
    std::string why;
};
LILA_SCENE_API ProgramText program(Graph& g, const PlugRef& target);
/** Every plug that has an expression, by full name. */
LILA_SCENE_API std::vector<PlugRef> targets(Graph& g);

/** The pythonExpression and sceneTime node types. */
LILA_SCENE_API void registerTypes();
/** expression: set, query or delete a plug's expression. */
LILA_SCENE_API void registerCommands();
} // namespace expressions
} // namespace lila::scene
