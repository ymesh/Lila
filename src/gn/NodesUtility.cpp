#include "gn/NodesCommon.h"
#include "gn/Texture.h"

#include <map>

namespace lila::gn::nodes
{
namespace
{
struct MathOp
{
    int arity;
    double (*fn)(double, double, double);
};

double safeDiv(double a, double b) { return b == 0.0 ? 0.0 : a / b; }
double flooredMod(double a, double b) { return b == 0.0 ? 0.0 : a - std::floor(a / b) * b; }
double wrapf(double v, double hi, double lo)
{
    double r = hi - lo;
    return r == 0.0 ? lo : v - r * std::floor((v - lo) / r);
}
double pingpong(double a, double s) { return s == 0.0 ? 0.0 : std::abs(flooredMod(a - s, 2.0 * s) - s); }
double smoothStep(double x, double lo, double hi)
{
    if (lo == hi)
        return x < lo ? 0.0 : 1.0;
    double t = std::clamp((x - lo) / (hi - lo), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
double smoothMin(double a, double b, double k)
{
    if (k <= 0.0)
        return std::min(a, b);
    double h = std::max(k - std::abs(a - b), 0.0) / k;
    return std::min(a, b) - h * h * h * k / 6.0;
}

const std::map<std::string, MathOp>& mathOps()
{
    static const std::map<std::string, MathOp> ops = {
        {"add", {2, [](double a, double b, double) { return a + b; }}},
        {"subtract", {2, [](double a, double b, double) { return a - b; }}},
        {"multiply", {2, [](double a, double b, double) { return a * b; }}},
        {"divide", {2, [](double a, double b, double) { return safeDiv(a, b); }}},
        {"multiplyAdd", {3, [](double a, double b, double c) { return a * b + c; }}},
        {"power", {2, [](double a, double b, double) { return a < 0.0 && b != std::floor(b) ? 0.0 : std::pow(a, b); }}},
        {"logarithm", {2, [](double a, double b, double) { return a > 0.0 && b > 0.0 && b != 1.0 ? std::log(a) / std::log(b) : 0.0; }}},
        {"sqrt", {1, [](double a, double, double) { return a > 0.0 ? std::sqrt(a) : 0.0; }}},
        {"inverseSqrt", {1, [](double a, double, double) { return a > 0.0 ? 1.0 / std::sqrt(a) : 0.0; }}},
        {"absolute", {1, [](double a, double, double) { return std::abs(a); }}},
        {"exponent", {1, [](double a, double, double) { return std::exp(a); }}},
        {"minimum", {2, [](double a, double b, double) { return std::min(a, b); }}},
        {"maximum", {2, [](double a, double b, double) { return std::max(a, b); }}},
        {"lessThan", {2, [](double a, double b, double) { return a < b ? 1.0 : 0.0; }}},
        {"greaterThan", {2, [](double a, double b, double) { return a > b ? 1.0 : 0.0; }}},
        // a against the edge b; smoothStep eases a from b to c.
        {"step", {2, [](double a, double b, double) { return a >= b ? 1.0 : 0.0; }}},
        {"smoothStep", {3, [](double a, double b, double c) { return smoothStep(a, b, c); }}},
        {"sign", {1, [](double a, double, double) { return a > 0.0 ? 1.0 : a < 0.0 ? -1.0 : 0.0; }}},
        {"compare", {3, [](double a, double b, double c) { return std::abs(a - b) <= std::max(c, 1e-5) ? 1.0 : 0.0; }}},
        {"smoothMin", {3, [](double a, double b, double c) { return smoothMin(a, b, c); }}},
        {"smoothMax", {3, [](double a, double b, double c) { return -smoothMin(-a, -b, c); }}},
        {"round", {1, [](double a, double, double) { return std::floor(a + 0.5); }}},
        {"floor", {1, [](double a, double, double) { return std::floor(a); }}},
        {"ceil", {1, [](double a, double, double) { return std::ceil(a); }}},
        {"truncate", {1, [](double a, double, double) { return std::trunc(a); }}},
        {"fraction", {1, [](double a, double, double) { return a - std::floor(a); }}},
        {"modulo", {2, [](double a, double b, double) { return b == 0.0 ? 0.0 : std::fmod(a, b); }}},
        {"flooredModulo", {2, [](double a, double b, double) { return flooredMod(a, b); }}},
        {"wrap", {3, [](double a, double b, double c) { return wrapf(a, b, c); }}},
        {"snap", {2, [](double a, double b, double) { return b == 0.0 ? 0.0 : std::floor(a / b) * b; }}},
        {"pingpong", {2, [](double a, double b, double) { return pingpong(a, b); }}},
        {"sine", {1, [](double a, double, double) { return std::sin(a); }}},
        {"cosine", {1, [](double a, double, double) { return std::cos(a); }}},
        {"tangent", {1, [](double a, double, double) { return std::tan(a); }}},
        {"arcsine", {1, [](double a, double, double) { return std::asin(std::clamp(a, -1.0, 1.0)); }}},
        {"arccosine", {1, [](double a, double, double) { return std::acos(std::clamp(a, -1.0, 1.0)); }}},
        {"arctangent", {1, [](double a, double, double) { return std::atan(a); }}},
        {"arctan2", {2, [](double a, double b, double) { return std::atan2(a, b); }}},
        {"sinh", {1, [](double a, double, double) { return std::sinh(a); }}},
        {"cosh", {1, [](double a, double, double) { return std::cosh(a); }}},
        {"tanh", {1, [](double a, double, double) { return std::tanh(a); }}},
        {"radians", {1, [](double a, double, double) { return glm::radians(a); }}},
        {"degrees", {1, [](double a, double, double) { return glm::degrees(a); }}},
    };
    return ops;
}

std::vector<std::string> keysOf(const std::map<std::string, MathOp>& m)
{
    std::vector<std::string> out;
    for (auto& [k, v] : m)
        out.push_back(k);
    return out;
}

void math()
{
    registerDecl({
        .type = "Math",
        .label = "Math",
        .category = "Utilities",
        .doc = "Float math on values or fields.",
        .props = {prop("operation", keysOf(mathOps()), "add"), prop("clamp", {"false", "true"}, "false", "Clamp the result to 0..1.")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                auto it = mathOps().find(findDecl("Math")->propValue(n, "operation"));
                int arity = it == mathOps().end() ? 2 : it->second.arity;
                Sockets s;
                s.inputs = {fin("a", T::Float, 0.5)};
                if (arity > 1)
                    s.inputs.push_back(fin("b", T::Float, 0.5));
                if (arity > 2)
                    s.inputs.push_back(fin("c", T::Float, 0.5));
                s.outputs = {fout("value", T::Float)};
                return s;
            },
        .exec =
            [](ExecContext& c) {
                auto it = mathOps().find(c.prop("operation"));
                if (it == mathOps().end())
                    throw ExecError("unknown math operation " + c.prop("operation"));
                auto fn = it->second.fn;
                bool clamp = c.prop("clamp") == "true";
                FieldPtr b = c.sockets.inputIndex("b") >= 0 ? c.field("b") : constantFloat(0.0);
                FieldPtr cc = c.sockets.inputIndex("c") >= 0 ? c.field("c") : constantFloat(0.0);
                c.out("value", map<double, double, double, double>(
                                   T::Float,
                                   [fn, clamp](double a, double b, double x) {
                                       double r = fn(a, b, x);
                                       return clamp ? std::clamp(r, 0.0, 1.0) : r;
                                   },
                                   {c.field("a"), b, cc}));
            },
    });
}

struct VecOp
{
    int arity; // vectors used; 4 means a and scale
    bool scalar; // result goes to the value output
    D3 (*fn)(D3, D3, D3, double);
    double (*value)(D3, D3);
};

D3 safeDivV(D3 a, D3 b) { return {safeDiv(a.x, b.x), safeDiv(a.y, b.y), safeDiv(a.z, b.z)}; }
D3 normalizeSafe(D3 a) { return glm::length(a) > 1e-12 ? glm::normalize(a) : D3(0.0); }

const std::map<std::string, VecOp>& vecOps()
{
    static const std::map<std::string, VecOp> ops = {
        {"add", {2, false, [](D3 a, D3 b, D3, double) { return a + b; }, nullptr}},
        {"subtract", {2, false, [](D3 a, D3 b, D3, double) { return a - b; }, nullptr}},
        {"multiply", {2, false, [](D3 a, D3 b, D3, double) { return a * b; }, nullptr}},
        {"divide", {2, false, [](D3 a, D3 b, D3, double) { return safeDivV(a, b); }, nullptr}},
        {"multiplyAdd", {3, false, [](D3 a, D3 b, D3 c, double) { return a * b + c; }, nullptr}},
        {"cross", {2, false, [](D3 a, D3 b, D3, double) { return glm::cross(a, b); }, nullptr}},
        {"project", {2, false, [](D3 a, D3 b, D3, double) { double l = glm::dot(b, b); return l > 0.0 ? b * (glm::dot(a, b) / l) : D3(0.0); }, nullptr}},
        {"reflect", {2, false, [](D3 a, D3 b, D3, double) { D3 n = normalizeSafe(b); return a - 2.0 * glm::dot(a, n) * n; }, nullptr}},
        {"refract", {5, false, [](D3 a, D3 b, D3, double eta) { return glm::refract(a, normalizeSafe(b), eta); }, nullptr}},
        {"faceforward", {3, false, [](D3 a, D3 b, D3 c, double) { return glm::dot(c, b) < 0.0 ? a : -a; }, nullptr}},
        {"dot", {2, true, nullptr, [](D3 a, D3 b) { return glm::dot(a, b); }}},
        {"distance", {2, true, nullptr, [](D3 a, D3 b) { return glm::distance(a, b); }}},
        {"length", {1, true, nullptr, [](D3 a, D3) { return glm::length(a); }}},
        {"scale", {4, false, [](D3 a, D3, D3, double s) { return a * s; }, nullptr}},
        {"normalize", {1, false, [](D3 a, D3, D3, double) { return normalizeSafe(a); }, nullptr}},
        {"absolute", {1, false, [](D3 a, D3, D3, double) { return glm::abs(a); }, nullptr}},
        {"minimum", {2, false, [](D3 a, D3 b, D3, double) { return glm::min(a, b); }, nullptr}},
        {"maximum", {2, false, [](D3 a, D3 b, D3, double) { return glm::max(a, b); }, nullptr}},
        {"floor", {1, false, [](D3 a, D3, D3, double) { return glm::floor(a); }, nullptr}},
        {"ceil", {1, false, [](D3 a, D3, D3, double) { return glm::ceil(a); }, nullptr}},
        {"fraction", {1, false, [](D3 a, D3, D3, double) { return a - glm::floor(a); }, nullptr}},
        {"modulo", {2, false, [](D3 a, D3 b, D3, double) { return D3(flooredMod(a.x, b.x), flooredMod(a.y, b.y), flooredMod(a.z, b.z)); }, nullptr}},
        {"wrap", {3, false, [](D3 a, D3 b, D3 c, double) { return D3(wrapf(a.x, b.x, c.x), wrapf(a.y, b.y, c.y), wrapf(a.z, b.z, c.z)); }, nullptr}},
        {"snap", {2, false, [](D3 a, D3 b, D3, double) { return D3(b.x == 0 ? 0 : std::floor(a.x / b.x) * b.x, b.y == 0 ? 0 : std::floor(a.y / b.y) * b.y, b.z == 0 ? 0 : std::floor(a.z / b.z) * b.z); }, nullptr}},
        {"sine", {1, false, [](D3 a, D3, D3, double) { return glm::sin(a); }, nullptr}},
        {"cosine", {1, false, [](D3 a, D3, D3, double) { return glm::cos(a); }, nullptr}},
        {"tangent", {1, false, [](D3 a, D3, D3, double) { return glm::tan(a); }, nullptr}},
    };
    return ops;
}

void vectorMath()
{
    std::vector<std::string> names;
    for (auto& [k, v] : vecOps())
        names.push_back(k);
    registerDecl({
        .type = "VectorMath",
        .label = "Vector Math",
        .category = "Utilities",
        .doc = "Vector math on values or fields. dot, distance and length give the value output.",
        .props = {prop("operation", names, "add")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                auto it = vecOps().find(findDecl("VectorMath")->propValue(n, "operation"));
                VecOp op = it == vecOps().end() ? vecOps().at("add") : it->second;
                Sockets s;
                s.inputs = {fin("a", T::Vector, vec(0, 0, 0))};
                if (op.arity >= 2 && op.arity != 4)
                    s.inputs.push_back(fin("b", T::Vector, vec(0, 0, 0)));
                if (op.arity == 3)
                    s.inputs.push_back(fin("c", T::Vector, vec(0, 0, 0)));
                if (op.arity == 4)
                    s.inputs.push_back(fin("scale", T::Float, 1.0));
                if (op.arity == 5)
                    s.inputs.push_back(fin("scale", T::Float, 1.0, Implicit::None, "Index of refraction ratio."));
                if (op.scalar)
                    s.outputs = {fout("value", T::Float)};
                else
                    s.outputs = {fout("vector", T::Vector)};
                return s;
            },
        .exec =
            [](ExecContext& c) {
                auto it = vecOps().find(c.prop("operation"));
                if (it == vecOps().end())
                    throw ExecError("unknown vector operation " + c.prop("operation"));
                VecOp op = it->second;
                auto opt = [&](const char* id, FieldPtr fallback) { return c.sockets.inputIndex(id) >= 0 ? c.field(id) : fallback; };
                FieldPtr a = c.field("a"), b = opt("b", constantVector(D3(0.0))), cc = opt("c", constantVector(D3(0.0)));
                FieldPtr s = opt("scale", constantFloat(1.0));
                if (op.scalar)
                {
                    auto fn = op.value;
                    c.out("value", map<double, D3, D3>(T::Float, [fn](D3 x, D3 y) { return fn(x, y); }, {a, b}));
                }
                else
                {
                    auto fn = op.fn;
                    c.out("vector", map<D3, D3, D3, D3, double>(T::Vector, [fn](D3 x, D3 y, D3 z, double k) { return fn(x, y, z, k); }, {a, b, cc, s}));
                }
            },
    });
}

void compare()
{
    registerDecl({
        .type = "Compare",
        .label = "Compare",
        .category = "Utilities",
        .doc = "Compares two values. Vectors compare by length for the ordering tests and element-wise for equality.",
        .props = {prop("dataType", {"float", "int", "vector"}, "float"),
                  prop("operation", {"lessThan", "lessEqual", "greaterThan", "greaterEqual", "equal", "notEqual"}, "greaterThan")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                SocketType t = dataTypeOf(findDecl("Compare")->propValue(n, "dataType"));
                Sockets s;
                json def = t == T::Vector ? vec(0, 0, 0) : json(0);
                s.inputs = {fin("a", t, def), fin("b", t, def)};
                if (t != T::Int)
                    s.inputs.push_back(bounded(fin("epsilon", T::Float, 0.001, Implicit::None, "Tolerance for equal and notEqual."), 0.0));
                s.outputs = {fout("result", T::Bool)};
                return s;
            },
        .exec =
            [](ExecContext& c) {
                std::string op = c.prop("operation");
                SocketType t = dataTypeOf(c.prop("dataType"));
                FieldPtr eps = c.sockets.inputIndex("epsilon") >= 0 ? c.field("epsilon") : constantFloat(0.0);
                auto test = [op](double a, double b, double e) {
                    if (op == "lessThan")
                        return a < b;
                    if (op == "lessEqual")
                        return a <= b;
                    if (op == "greaterThan")
                        return a > b;
                    if (op == "greaterEqual")
                        return a >= b;
                    if (op == "equal")
                        return std::abs(a - b) <= e;
                    return std::abs(a - b) > e;
                };
                if (t == T::Vector)
                {
                    bool eq = op == "equal" || op == "notEqual";
                    c.out("result", map<uint8_t, D3, D3, double>(
                                        T::Bool,
                                        [test, eq, op](D3 a, D3 b, double e) {
                                            if (eq)
                                                return test(glm::distance(a, b), 0.0, e);
                                            return test(glm::length(a), glm::length(b), e);
                                        },
                                        {c.field("a"), c.field("b"), eps}));
                }
                else
                    c.out("result", map<uint8_t, double, double, double>(T::Bool, test, {c.field("a"), c.field("b"), eps}));
            },
    });
}

void booleanMath()
{
    registerDecl({
        .type = "BooleanMath",
        .label = "Boolean Math",
        .category = "Utilities",
        .doc = "Logic on booleans.",
        .props = {prop("operation", {"and", "or", "not", "nand", "nor", "xnor", "xor", "imply", "nimply"}, "and")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                Sockets s;
                s.inputs = {fin("a", T::Bool, false)};
                if (findDecl("BooleanMath")->propValue(n, "operation") != "not")
                    s.inputs.push_back(fin("b", T::Bool, false));
                s.outputs = {fout("result", T::Bool)};
                return s;
            },
        .exec =
            [](ExecContext& c) {
                std::string op = c.prop("operation");
                FieldPtr b = c.sockets.inputIndex("b") >= 0 ? c.field("b") : constantBool(false);
                c.out("result", map<uint8_t, uint8_t, uint8_t>(
                                    T::Bool,
                                    [op](uint8_t x, uint8_t y) {
                                        bool a = x != 0, bb = y != 0;
                                        if (op == "and")
                                            return a && bb;
                                        if (op == "or")
                                            return a || bb;
                                        if (op == "not")
                                            return !a;
                                        if (op == "nand")
                                            return !(a && bb);
                                        if (op == "nor")
                                            return !(a || bb);
                                        if (op == "xnor")
                                            return a == bb;
                                        if (op == "xor")
                                            return a != bb;
                                        if (op == "imply")
                                            return !a || bb;
                                        return a && !bb;
                                    },
                                    {c.field("a"), b}));
            },
    });
}

void mapRange()
{
    registerDecl({
        .type = "MapRange",
        .label = "Map Range",
        .category = "Utilities",
        .doc = "Remaps a value from one range to another.",
        .props = {prop("interpolation", {"linear", "stepped", "smoothstep", "smootherstep"}, "linear"),
                  prop("clamp", {"true", "false"}, "true")},
        .sockets = {.inputs = {fin("value", T::Float, 1.0), fin("fromMin", T::Float, 0.0), fin("fromMax", T::Float, 1.0), fin("toMin", T::Float, 0.0),
                               fin("toMax", T::Float, 1.0), bounded(fin("steps", T::Float, 4.0, Implicit::None, "For stepped."), 0.0)},
                    .outputs = {fout("result", T::Float)}},
        .exec =
            [](ExecContext& c) {
                std::string interp = c.prop("interpolation");
                bool clamp = c.prop("clamp") != "false";
                auto fn = [interp, clamp](double v, double f0, double f1, double t0, double t1, double steps) {
                    double t = f1 == f0 ? 0.0 : (v - f0) / (f1 - f0);
                    if (interp == "stepped")
                        t = steps > 0.0 ? std::floor(t * (steps + 1.0)) / steps : t;
                    else if (interp == "smoothstep")
                    {
                        t = std::clamp(t, 0.0, 1.0);
                        t = t * t * (3.0 - 2.0 * t);
                    }
                    else if (interp == "smootherstep")
                    {
                        t = std::clamp(t, 0.0, 1.0);
                        t = t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
                    }
                    if (clamp)
                        t = std::clamp(t, 0.0, 1.0);
                    return t0 + (t1 - t0) * t;
                };
                c.out("result", std::make_shared<FunctionField>(
                                    T::Float,
                                    std::vector<FieldPtr>{c.field("value"), c.field("fromMin"), c.field("fromMax"), c.field("toMin"), c.field("toMax"),
                                                          c.field("steps")},
                                    std::vector<SocketType>(6, T::Float), [fn](const std::vector<Column>& in, size_t n) {
                                        bool single = true;
                                        for (const Column& x : in)
                                            single = single && x.single;
                                        size_t cnt = single ? 1 : n;
                                        std::vector<double> out(cnt);
                                        tbb::parallel_for(size_t(0), cnt, [&](size_t k) {
                                            out[k] = fn(in[0].at<double>(k), in[1].at<double>(k), in[2].at<double>(k), in[3].at<double>(k),
                                                        in[4].at<double>(k), in[5].at<double>(k));
                                        });
                                        return Column{std::move(out), single};
                                    }));
            },
    });
}

void clampNode()
{
    registerDecl({
        .type = "Clamp",
        .label = "Clamp",
        .category = "Utilities",
        .doc = "Limits a value to a range. range mode also works when min is above max.",
        .props = {prop("clampType", {"minMax", "range"}, "minMax")},
        .sockets = {.inputs = {fin("value", T::Float, 1.0), fin("min", T::Float, 0.0), fin("max", T::Float, 1.0)}, .outputs = {fout("result", T::Float)}},
        .exec =
            [](ExecContext& c) {
                bool range = c.prop("clampType") == "range";
                c.out("result", map<double, double, double, double>(
                                    T::Float,
                                    [range](double v, double lo, double hi) {
                                        if (range && lo > hi)
                                            std::swap(lo, hi);
                                        return std::min(std::max(v, lo), hi);
                                    },
                                    {c.field("value"), c.field("min"), c.field("max")}));
            },
    });
}

void mix()
{
    registerDecl({
        .type = "Mix",
        .label = "Mix",
        .category = "Utilities",
        .doc = "Blends a and b by factor.",
        .props = {prop("dataType", {"float", "vector", "rotation", "color"}, "float"), prop("clampFactor", {"true", "false"}, "true")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                SocketType t = dataTypeOf(findDecl("Mix")->propValue(n, "dataType"));
                json def = t == T::Float ? json(0.0) : zeroJson(t);
                return Sockets{{fin("factor", T::Float, 0.5), fin("a", t, def), fin("b", t, def)}, {fout("result", t)}};
            },
        .exec =
            [](ExecContext& c) {
                SocketType t = dataTypeOf(c.prop("dataType"));
                bool clamp = c.prop("clampFactor") != "false";
                if (t == T::Float)
                    c.out("result", map<double, double, double, double>(
                                        T::Float, [clamp](double f, double a, double b) { f = clamp ? std::clamp(f, 0.0, 1.0) : f; return a + (b - a) * f; },
                                        {c.field("factor"), c.field("a"), c.field("b")}));
                else if (t == T::Color)
                    c.out("result", map<glm::dvec4, double, glm::dvec4, glm::dvec4>(
                                        t, [clamp](double f, glm::dvec4 a, glm::dvec4 b) { f = clamp ? std::clamp(f, 0.0, 1.0) : f; return a + (b - a) * f; },
                                        {c.field("factor"), c.field("a"), c.field("b")}));
                else
                    c.out("result", map<D3, double, D3, D3>(
                                        t, [clamp](double f, D3 a, D3 b) { f = clamp ? std::clamp(f, 0.0, 1.0) : f; return a + (b - a) * f; },
                                        {c.field("factor"), c.field("a"), c.field("b")}));
            },
    });
}

void switchNode()
{
    registerDecl({
        .type = "Switch",
        .label = "Switch",
        .category = "Utilities",
        .doc = "Picks false or true by the switch. For data types the switch can be a field.",
        .props = {prop("type", {"geometry", "float", "int", "bool", "vector", "rotation", "string", "color"}, "geometry")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                SocketType t = dataTypeOf(findDecl("Switch")->propValue(n, "type"));
                parseSocketType(findDecl("Switch")->propValue(n, "type"), &t);
                bool data = isDataType(t);
                Sockets s;
                s.inputs = {data ? fin("switch", T::Bool, false) : in("switch", T::Bool, false)};
                if (t == T::Geometry)
                {
                    s.inputs.push_back(geoIn("false"));
                    s.inputs.push_back(geoIn("true"));
                    s.outputs = {geoOut("output")};
                }
                else
                {
                    json def = t == T::String ? json("") : zeroJson(t);
                    s.inputs.push_back(data ? fin("false", t, def) : in("false", t, def));
                    s.inputs.push_back(data ? fin("true", t, def) : in("true", t, def));
                    s.outputs = {data ? fout("output", t) : out("output", t)};
                }
                return s;
            },
        .exec =
            [](ExecContext& c) {
                SocketType t = SocketType::Geometry;
                parseSocketType(c.prop("type"), &t);
                if (t == T::Geometry)
                {
                    c.out("output", c.geometry(c.b("switch") ? "true" : "false"));
                    return;
                }
                if (t == T::String)
                {
                    c.outString("output", c.s(c.b("switch") ? "true" : "false"));
                    return;
                }
                FieldPtr sw = c.field("switch");
                if (sw->constant())
                {
                    c.out("output", c.field(c.b("switch") ? "true" : "false"));
                    return;
                }
                FieldPtr f = c.field("false"), tr = c.field("true");
                c.out("output", std::make_shared<FunctionField>(t, std::vector<FieldPtr>{sw, f, tr}, std::vector<SocketType>{T::Bool, t, t},
                                                                [](const std::vector<Column>& in, size_t n) {
                                                                    return std::visit(
                                                                        [&](const auto& fv) -> Column {
                                                                            using V = typename std::decay_t<decltype(fv)>::value_type;
                                                                            std::vector<V> out(n);
                                                                            for (size_t k = 0; k < n; ++k)
                                                                                out[k] = in[0].at<uint8_t>(k) ? in[2].at<V>(k) : in[1].at<V>(k);
                                                                            return Column::of(std::move(out));
                                                                        },
                                                                        in[1].data);
                                                                }));
            },
    });
}

void xyz()
{
    registerDecl({
        .type = "CombineXYZ",
        .label = "Combine XYZ",
        .category = "Utilities",
        .doc = "Builds a vector from three floats.",
        .sockets = {.inputs = {fin("x", T::Float, 0.0), fin("y", T::Float, 0.0), fin("z", T::Float, 0.0)}, .outputs = {fout("vector", T::Vector)}},
        .exec =
            [](ExecContext& c) {
                c.out("vector", map<D3, double, double, double>(T::Vector, [](double x, double y, double z) { return D3(x, y, z); },
                                                                 {c.field("x"), c.field("y"), c.field("z")}));
            },
    });
    registerDecl({
        .type = "SeparateXYZ",
        .label = "Separate XYZ",
        .category = "Utilities",
        .doc = "Splits a vector into three floats.",
        .sockets = {.inputs = {fin("vector", T::Vector, vec(0, 0, 0))}, .outputs = {fout("x", T::Float), fout("y", T::Float), fout("z", T::Float)}},
        .exec =
            [](ExecContext& c) {
                FieldPtr v = c.field("vector");
                c.out("x", map<double, D3>(T::Float, [](D3 a) { return a.x; }, {v}));
                c.out("y", map<double, D3>(T::Float, [](D3 a) { return a.y; }, {v}));
                c.out("z", map<double, D3>(T::Float, [](D3 a) { return a.z; }, {v}));
            },
    });
}

void rotateVector()
{
    registerDecl({
        .type = "VectorRotate",
        .label = "Vector Rotate",
        .category = "Utilities",
        .doc = "Rotates a vector around a center, by axis and angle, a fixed axis, or an Euler rotation.",
        .props = {prop("rotationType", {"axisAngle", "x", "y", "z", "euler"}, "axisAngle"), prop("invert", {"false", "true"}, "false")},
        .sockets = {.inputs = {fin("vector", T::Vector, vec(0, 0, 0)), fin("center", T::Vector, vec(0, 0, 0)), fin("axis", T::Vector, vec(0, 1, 0)),
                               fin("angle", T::Float, 0.0, Implicit::None, "Radians."), fin("rotation", T::Rotation, vec(0, 0, 0))},
                    .outputs = {fout("vector", T::Vector)}},
        .exec =
            [](ExecContext& c) {
                std::string type = c.prop("rotationType");
                double sign = c.prop("invert") == "true" ? -1.0 : 1.0;
                c.out("vector", map<D3, D3, D3, D3, double, D3>(
                                    T::Vector,
                                    [type, sign](D3 v, D3 center, D3 axis, double angle, D3 euler) {
                                        glm::dmat3 r(1.0);
                                        if (type == "euler")
                                        {
                                            r = eulerToMatrix(euler);
                                            if (sign < 0.0)
                                                r = glm::transpose(r);
                                        }
                                        else
                                        {
                                            D3 ax = type == "x" ? D3(1, 0, 0) : type == "y" ? D3(0, 1, 0) : type == "z" ? D3(0, 0, 1) : axis;
                                            r = axisAngle(ax, angle * sign);
                                        }
                                        return center + r * (v - center);
                                    },
                                    {c.field("vector"), c.field("center"), c.field("axis"), c.field("angle"), c.field("rotation")}));
            },
    });
}

void alignEuler()
{
    registerDecl({
        .type = "AlignEulerToVector",
        .label = "Align Euler to Vector",
        .category = "Utilities",
        .doc = "Turns a rotation so one of its axes points along a vector.",
        .props = {prop("axis", {"x", "y", "z"}, "y"), prop("pivotAxis", {"auto", "x", "y", "z"}, "auto")},
        .sockets = {.inputs = {fin("rotation", T::Rotation, vec(0, 0, 0)), fin("factor", T::Float, 1.0), fin("vector", T::Vector, vec(0, 1, 0))},
                    .outputs = {fout("rotation", T::Rotation)}},
        .exec =
            [](ExecContext& c) {
                auto axisVec = [](const std::string& a) { return a == "x" ? D3(1, 0, 0) : a == "y" ? D3(0, 1, 0) : D3(0, 0, 1); };
                D3 local = axisVec(c.prop("axis"));
                std::string pivotName = c.prop("pivotAxis");
                D3 pivot = axisVec(pivotName);
                bool autoPivot = pivotName == "auto";
                c.out("rotation", map<D3, D3, double, D3>(
                                      T::Rotation,
                                      [=](D3 euler, double factor, D3 target) {
                                          if (glm::length(target) < 1e-12)
                                              return euler;
                                          glm::dmat3 r = eulerToMatrix(euler);
                                          D3 cur = glm::normalize(r * local);
                                          D3 t = glm::normalize(target);
                                          glm::dmat3 turn;
                                          if (autoPivot)
                                          {
                                              D3 ax = glm::cross(cur, t);
                                              double ang = std::atan2(glm::length(ax), glm::dot(cur, t));
                                              turn = glm::length(ax) < 1e-12 ? (glm::dot(cur, t) < 0.0 ? rotationBetween(cur, t) : glm::dmat3(1.0))
                                                                             : axisAngle(ax, ang * factor);
                                          }
                                          else
                                          {
                                              D3 p = glm::normalize(r * pivot);
                                              D3 a = cur - p * glm::dot(cur, p), b = t - p * glm::dot(t, p);
                                              if (glm::length(a) < 1e-12 || glm::length(b) < 1e-12)
                                                  return euler;
                                              a = glm::normalize(a);
                                              b = glm::normalize(b);
                                              double ang = std::atan2(glm::dot(glm::cross(a, b), p), glm::dot(a, b));
                                              turn = axisAngle(p, ang * factor);
                                          }
                                          return matrixToEuler(turn * r);
                                      },
                                      {c.field("rotation"), c.field("factor"), c.field("vector")}));
            },
    });
}

/** Gradient noise in -1..1 with hashed gradients at lattice points. */
double gradientNoise(D3 p)
{
    static const D3 grads[12] = {{1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1}, {-1, 0, 1},
                                 {1, 0, -1}, {-1, 0, -1}, {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}};
    D3 i = glm::floor(p), f = p - i;
    auto fade = [](double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); };
    D3 u(fade(f.x), fade(f.y), fade(f.z));
    auto corner = [&](int dx, int dy, int dz) {
        uint32_t h = hash32(uint32_t(int64_t(i.x) + dx), uint32_t(int64_t(i.y) + dy), uint32_t(int64_t(i.z) + dz));
        return glm::dot(grads[h % 12], f - D3(dx, dy, dz));
    };
    auto lerp = [](double a, double b, double t) { return a + (b - a) * t; };
    double x00 = lerp(corner(0, 0, 0), corner(1, 0, 0), u.x), x10 = lerp(corner(0, 1, 0), corner(1, 1, 0), u.x);
    double x01 = lerp(corner(0, 0, 1), corner(1, 0, 1), u.x), x11 = lerp(corner(0, 1, 1), corner(1, 1, 1), u.x);
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z);
}

/** fBm over a gradient noise, 0..1. Fractional detail blends in the last octave. */
template <class P, class Noise> double fbm(P p, double detail, double roughness, double lacunarity, Noise noiseAt)
{
    detail = std::clamp(detail, 0.0, 15.0);
    double amp = 1.0, sum = 0.0, norm = 0.0, freq = 1.0;
    int octaves = int(detail);
    for (int o = 0; o <= octaves; ++o)
    {
        sum += noiseAt(p * freq) * amp;
        norm += amp;
        amp *= roughness;
        freq *= lacunarity;
    }
    double rest = detail - double(octaves);
    if (rest > 0.0)
    {
        double extra = noiseAt(p * freq) * amp;
        double withExtra = (sum + extra) / (norm + amp);
        double without = sum / norm;
        return 0.5 + 0.5 * (without + (withExtra - without) * rest);
    }
    return 0.5 + 0.5 * sum / norm;
}

Sockets noiseSockets(const Group&, const GNode& n)
{
    std::string dims = findDecl("NoiseTexture")->propValue(n, "dimensions");
    Sockets s;
    if (dims != "1D")
        s.inputs.push_back(fin("vector", T::Vector, nullptr, Implicit::Position));
    if (dims == "1D" || dims == "4D")
        s.inputs.push_back(fin("w", T::Float, 0.0, Implicit::None, "The fourth coordinate (the only one in 1D). Animate it to evolve the noise."));
    for (SocketDecl d : {fin("scale", T::Float, 0.05), bounded(fin("detail", T::Float, 2.0), 0.0, 15.0), bounded(fin("roughness", T::Float, 0.5), 0.0, 1.0),
                         bounded(fin("lacunarity", T::Float, 2.0), 0.0),
                         fin("distortion", T::Float, 0.0)})
        s.inputs.push_back(std::move(d));
    s.outputs = {fout("fac", T::Float), fout("color", T::Color)};
    return s;
}

void noise()
{
    registerDecl({
        .type = "NoiseTexture",
        .label = "Noise Texture",
        .category = "Texture",
        .doc = "Gradient noise with fBm octaves in 1D to 4D (W is the extra coordinate). Positions are in cm, so the default scale of 0.05 "
               "gives about five bumps a metre.",
        .props = {prop("dimensions", {"1D", "2D", "3D", "4D"}, "3D")},
        .dynamicSockets = noiseSockets,
        .exec =
            [](ExecContext& c) {
                std::string dimsName = c.prop("dimensions");
                int dims = dimsName == "1D" ? 1 : dimsName == "2D" ? 2 : dimsName == "4D" ? 4 : 3;
                FieldPtr vector = c.sockets.inputIndex("vector") >= 0 ? c.field("vector") : constantVector(D3(0.0));
                FieldPtr w = c.sockets.inputIndex("w") >= 0 ? c.field("w") : constantFloat(0.0);
                std::vector<FieldPtr> ins{vector, c.field("scale"), c.field("detail"), c.field("roughness"), c.field("lacunarity"), c.field("distortion"), w};
                std::vector<SocketType> types{T::Vector, T::Float, T::Float, T::Float, T::Float, T::Float, T::Float};
                auto make = [&](bool color) {
                    return std::make_shared<FunctionField>(color ? T::Color : T::Float, ins, types, [color, dims](const std::vector<Column>& in, size_t n) {
                        bool single = true;
                        for (const Column& x : in)
                            single = single && x.single;
                        size_t cnt = single ? 1 : n;
                        std::vector<double> fac(color ? 0 : cnt);
                        std::vector<glm::dvec4> col(color ? cnt : 0);
                        tbb::parallel_for(size_t(0), cnt, [&](size_t k) {
                            double scale = in[1].at<double>(k);
                            double det = in[2].at<double>(k), rough = in[3].at<double>(k), lac = in[4].at<double>(k), dist = in[5].at<double>(k);
                            if (dims == 4)
                            {
                                glm::dvec4 p(in[0].at<D3>(k) * scale, in[6].at<double>(k) * scale);
                                if (dist != 0.0)
                                    p += glm::dvec4(tex::gradientNoise4(p + glm::dvec4(13.5, 0, 0, 0)), tex::gradientNoise4(p + glm::dvec4(0, 47.2, 0, 0)),
                                                    tex::gradientNoise4(p + glm::dvec4(0, 0, 91.7, 0)), tex::gradientNoise4(p + glm::dvec4(0, 0, 0, 33.1))) *
                                         dist;
                                auto nz = [](const glm::dvec4& q) { return tex::gradientNoise4(q); };
                                if (color)
                                    col[k] = glm::dvec4(fbm(p, det, rough, lac, nz), fbm(p + glm::dvec4(31.4, 17.3, 5.9, 2.2), det, rough, lac, nz),
                                                        fbm(p + glm::dvec4(7.7, 53.1, 29.3, 11.5), det, rough, lac, nz), 1.0);
                                else
                                    fac[k] = fbm(p, det, rough, lac, nz);
                                return;
                            }
                            D3 p = dims == 1 ? D3(in[6].at<double>(k) * scale, 0.0, 0.0) : in[0].at<D3>(k) * scale;
                            if (dims == 2)
                                p.z = 0.0;
                            if (dist != 0.0)
                                p += D3(gradientNoise(p + D3(13.5, 0, 0)), gradientNoise(p + D3(0, 47.2, 0)), gradientNoise(p + D3(0, 0, 91.7))) * dist;
                            if (dims == 1)
                                p.y = p.z = 0.0;
                            else if (dims == 2)
                                p.z = 0.0;
                            if (color)
                                col[k] = glm::dvec4(fbm(p, det, rough, lac, gradientNoise), fbm(p + D3(31.4, 17.3, 5.9), det, rough, lac, gradientNoise),
                                                    fbm(p + D3(7.7, 53.1, 29.3), det, rough, lac, gradientNoise), 1.0);
                            else
                                fac[k] = fbm(p, det, rough, lac, gradientNoise);
                        });
                        return color ? Column{std::move(col), single} : Column{std::move(fac), single};
                    });
                };
                c.out("fac", make(false));
                c.out("color", make(true));
            },
    });
}

void floatToInt()
{
    registerDecl({
        .type = "FloatToInteger",
        .label = "Float to Integer",
        .category = "Utilities",
        .doc = "Rounds a float to an integer.",
        .props = {prop("roundingMode", {"round", "floor", "ceil", "truncate"}, "round")},
        .sockets = {.inputs = {fin("float", T::Float, 0.0)}, .outputs = {fout("integer", T::Int)}},
        .exec =
            [](ExecContext& c) {
                std::string m = c.prop("roundingMode");
                c.out("integer", map<int64_t, double>(
                                     T::Int,
                                     [m](double v) {
                                         double r = m == "floor" ? std::floor(v) : m == "ceil" ? std::ceil(v) : m == "truncate" ? std::trunc(v) : std::round(v);
                                         return int64_t(r);
                                     },
                                     {c.field("float")}));
            },
    });
}
} // namespace

void registerUtilityNodes()
{
    math();
    vectorMath();
    compare();
    booleanMath();
    mapRange();
    clampNode();
    mix();
    switchNode();
    xyz();
    rotateVector();
    alignEuler();
    noise();
    floatToInt();
}
} // namespace lila::gn::nodes
