#include "rigmath/Expressions.h"

#include "core/Profile.h"
#include "ops/Command.h"
#include "rigmath/MathNodes.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/Relocate.h"
#include "scene/Anim.h"
#include "rigmath/ExpressionVm.h"

#include <cctype>
#include <cmath>
#include <format>
#include <map>
#include <mutex>
#include <set>

namespace lila::scene::expressions
{
namespace
{
constexpr const char* kTag = "lilaExpression";

struct Token
{
    enum Kind : uint8_t
    {
        End,
        Number,
        Name, // identifiers, keywords and dotted plug paths
        Op,
    };
    Kind kind = End;
    std::string text;
    double number = 0.0;
    size_t at = 0;
};

std::vector<Token> tokenize(const std::string& s)
{
    std::vector<Token> out;
    size_t i = 0;
    auto nameChar = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':'; };
    while (i < s.size())
    {
        const char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c)))
        {
            ++i;
            continue;
        }
        Token t;
        t.at = i;
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))))
        {
            char* end = nullptr;
            t.number = std::strtod(s.c_str() + i, &end);
            const size_t n = size_t(end - (s.c_str() + i));
            t.kind = Token::Number;
            t.text = s.substr(i, n);
            i += n;
        }
        else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
        {
            size_t j = i;
            // node.attr, node.attr[2], ns:node.attr: one token, split into a plug later.
            while (j < s.size() && (nameChar(s[j]) || (s[j] == '.' && j + 1 < s.size() && (std::isalpha(static_cast<unsigned char>(s[j + 1])) || s[j + 1] == '_')) ||
                                    (s[j] == '[' && s.find(']', j) != std::string::npos)))
            {
                if (s[j] == '[')
                    j = s.find(']', j);
                ++j;
            }
            t.kind = Token::Name;
            t.text = s.substr(i, j - i);
            i = j;
        }
        else
        {
            static const char* kTwo[] = {"**", "<=", ">=", "==", "!=", "//"};
            t.kind = Token::Op;
            t.text = std::string(1, c);
            for (const char* two : kTwo)
                if (s.compare(i, 2, two) == 0)
                    t.text = two;
            if (std::string("+-*/%()<>,").find(c) == std::string::npos && t.text.size() == 1)
                throw std::invalid_argument("can't read '" + std::string(1, c) + "' at " + std::to_string(i + 1));
            i += t.text.size();
        }
        out.push_back(std::move(t));
    }
    out.push_back(Token{Token::End, "", 0.0, s.size()});
    return out;
}

class Parser
{
public:
    explicit Parser(std::vector<Token> tokens) : mTokens(std::move(tokens)) {}

    std::unique_ptr<Ast> parseAll()
    {
        auto e = ternary();
        if (peek().kind != Token::End)
            throw std::invalid_argument("unexpected '" + peek().text + "'");
        return e;
    }

private:
    const Token& peek() const { return mTokens[mAt]; }
    bool accept(const char* text)
    {
        if ((peek().kind == Token::Op || peek().kind == Token::Name) && peek().text == text)
        {
            ++mAt;
            return true;
        }
        return false;
    }
    void expect(const char* text)
    {
        if (!accept(text))
            throw std::invalid_argument(std::string("expected '") + text + "'" + (peek().kind == Token::End ? " at the end" : " before '" + peek().text + "'"));
    }
    static std::unique_ptr<Ast> node(Ast::Kind k, std::string op, std::vector<std::unique_ptr<Ast>> args = {})
    {
        auto a = std::make_unique<Ast>();
        a->kind = k;
        a->op = std::move(op);
        a->args = std::move(args);
        return a;
    }
    template <class... A> static std::vector<std::unique_ptr<Ast>> list(A&&... a)
    {
        std::vector<std::unique_ptr<Ast>> v;
        (v.push_back(std::move(a)), ...);
        return v;
    }

    std::unique_ptr<Ast> ternary()
    {
        auto then = orExpr();
        if (!accept("if"))
            return then;
        auto cond = orExpr();
        expect("else");
        auto other = ternary();
        return node(Ast::Ternary, "", list(std::move(then), std::move(cond), std::move(other)));
    }
    std::unique_ptr<Ast> orExpr()
    {
        auto l = andExpr();
        while (accept("or"))
            l = node(Ast::Binary, "or", list(std::move(l), andExpr()));
        return l;
    }
    std::unique_ptr<Ast> andExpr()
    {
        auto l = notExpr();
        while (accept("and"))
            l = node(Ast::Binary, "and", list(std::move(l), notExpr()));
        return l;
    }
    std::unique_ptr<Ast> notExpr()
    {
        if (accept("not"))
            return node(Ast::Unary, "not", list(notExpr()));
        return comparison();
    }
    std::unique_ptr<Ast> comparison()
    {
        auto l = arith();
        for (const char* op : {"<=", ">=", "==", "!=", "<", ">"})
            if (accept(op))
            {
                auto c = node(Ast::Compare, op, list(std::move(l), arith()));
                for (const char* again : {"<=", ">=", "==", "!=", "<", ">"})
                    if (peek().kind == Token::Op && peek().text == again)
                        throw std::invalid_argument("chained comparisons run as Python");
                return c;
            }
        return l;
    }
    std::unique_ptr<Ast> arith()
    {
        auto l = term();
        while (true)
        {
            if (accept("+"))
                l = node(Ast::Binary, "+", list(std::move(l), term()));
            else if (accept("-"))
                l = node(Ast::Binary, "-", list(std::move(l), term()));
            else
                return l;
        }
    }
    std::unique_ptr<Ast> term()
    {
        auto l = factor();
        while (true)
        {
            if (accept("*"))
                l = node(Ast::Binary, "*", list(std::move(l), factor()));
            else if (accept("//"))
                throw std::invalid_argument("floor division runs as Python");
            else if (accept("/"))
                l = node(Ast::Binary, "/", list(std::move(l), factor()));
            else if (accept("%"))
                l = node(Ast::Binary, "%", list(std::move(l), factor()));
            else
                return l;
        }
    }
    std::unique_ptr<Ast> factor()
    {
        if (accept("-"))
            return node(Ast::Unary, "-", list(factor()));
        if (accept("+"))
            return factor();
        return power();
    }
    std::unique_ptr<Ast> power()
    {
        auto base = atom();
        if (accept("**"))
            return node(Ast::Binary, "**", list(std::move(base), factor()));
        return base;
    }
    std::unique_ptr<Ast> atom()
    {
        const Token t = peek();
        if (t.kind == Token::Number)
        {
            ++mAt;
            auto a = node(Ast::Number, "");
            a->number = t.number;
            return a;
        }
        if (accept("("))
        {
            auto e = ternary();
            expect(")");
            return e;
        }
        if (t.kind != Token::Name)
            throw std::invalid_argument(t.kind == Token::End ? "the expression ends early" : "unexpected '" + t.text + "'");
        ++mAt;
        static const std::map<std::string, double> kConstants = {{"pi", 3.14159265358979323846}, {"tau", 6.28318530717958647692},
                                                                 {"e", 2.71828182845904523536}, {"True", 1.0}, {"False", 0.0}};
        if ((t.text == "time" || t.text == "frame") && !(peek().kind == Token::Op && peek().text == "("))
            return node(Ast::Time, t.text);
        if (auto c = kConstants.find(t.text); c != kConstants.end())
        {
            auto a = node(Ast::Number, "");
            a->number = c->second;
            return a;
        }
        if (accept("("))
        {
            std::vector<std::unique_ptr<Ast>> args;
            if (!accept(")"))
            {
                do
                    args.push_back(ternary());
                while (accept(","));
                expect(")");
            }
            return node(Ast::Call, t.text, std::move(args));
        }
        if (t.text.find('.') == std::string::npos)
            throw std::invalid_argument("'" + t.text + "' is neither a plug (node.attr) nor a constant");
        return node(Ast::Plug, t.text);
    }

    std::vector<Token> mTokens;
    size_t mAt = 0;
};

struct FnInfo
{
    std::string rigmath;
    int args;
    int output = 0;  // which output pin
    int maxArgs = 0; // the most arguments it takes when some are optional; 0 = args
    rigmath::Kind kind = rigmath::Kind::Float;
    std::vector<int> pins = {};                     // the pin each argument goes to; empty = in order
    std::vector<std::pair<int, double>> fixed = {}; // pins set to constants
};
const std::map<std::string, FnInfo>& fnTable()
{
    using K = rigmath::Kind;
    static const std::map<std::string, FnInfo> t = {
        {"abs", {"abs", 1}},
        {"min", {"min", 2}},
        {"max", {"max", 2}},
        {"clamp", {"clamp", 3}},
        {"lerp", {"lerp", 3}},
        {"sin", {"sin", 1}},
        {"cos", {"cos", 1}},
        {"tan", {"tan", 1}},
        {"asin", {"asin", 1}},
        {"acos", {"acos", 1}},
        {"atan", {"atan", 1}},
        {"atan2", {"atan2", 2}},
        {"sqrt", {"sqrt", 1}},
        {"exp", {"exp", 1}},
        {"pow", {"pow", 2}},
        {"log", {"loge", 1, 0, 2}},
        {"sign", {"sign", 1}},
        {"fmod", {"fmod", 2, 1}},
        {"hypot", {"hypotenuse", 2}},
        {"degrees", {"radiansToDegrees", 1}},
        {"radians", {"degreesToRadians", 1}},
        {"wrap", {"wrap", 3}},
        {"mapRange", {"mapRangeClamped", 5}},
        {"floor", {"floor", 1}},
        {"ceil", {"ceil", 1}},
        {"round", {"round", 1}},
        {"trunc", {"truncate", 1}},
        {"noise", {"perlinNoise1D", 1, 0, 2}},
        {"random", {"randomFloatFromStream", 1, 0, 3, K::Int}},
        // Maya's argument order: linstep(min, max, value), smoothstep(min, max, value).
        {"linstep", {"mapRangeClamped", 3, 0, 0, K::Float, {1, 2, 0}, {{3, 0.0}, {4, 1.0}}}},
        {"smoothstep", {"mapRangeClamped", 3, 0, 0, K::Float, {1, 2, 0}, {{3, 0.0}, {4, 1.0}}}},
    };
    return t;
}

/** Whether fn takes n arguments; random takes a seed, or a seed, min and max. */
bool arityOk(const std::string& fn, const FnInfo& f, int n)
{
    if (fn == "random")
        return n == 1 || n == 3;
    return n >= f.args && n <= std::max(f.args, f.maxArgs);
}

/** Whether text uses name as a bare identifier: outside strings, not after a dot. */
bool usesName(const std::string& s, std::string_view name)
{
    auto word = [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == ':'; };
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        if (quote)
        {
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '"' || c == '\'')
        {
            quote = c;
            continue;
        }
        if (s.compare(i, name.size(), name) == 0 && (i == 0 || (!word(s[i - 1]) && s[i - 1] != '.')) &&
            (i + name.size() >= s.size() || !word(s[i + name.size()])))
            return true;
    }
    return false;
}

/** The overload of a rig math function whose inputs take kind (Float, or Bool for logic). */
const rigmath::Overload* overload(const std::string& fn, rigmath::Kind kind)
{
    for (const rigmath::Function& f : rigmath::functions())
        if (f.name == fn)
            for (const rigmath::Overload& o : f.overloads)
                if (!o.inputs.empty() && o.inputs[0].kind == kind)
                    return &o;
    return nullptr;
}

/** Why an AST doesn't compile to rig math (unknown function, wrong arity, missing plug), or empty. */
std::string uncompilable(const Ast& a, Graph& g)
{
    if (a.kind == Ast::Plug)
    {
        try
        {
            g.plug(a.op);
        }
        catch (const std::exception&)
        {
            return "no plug " + a.op;
        }
    }
    if (a.kind == Ast::Call)
    {
        auto it = fnTable().find(a.op);
        if (it == fnTable().end())
            return a.op + "() isn't a rig math function";
        if (const FnInfo& f = it->second; !arityOk(a.op, f, int(a.args.size())))
        {
            const int most = std::max(f.args, f.maxArgs);
            const std::string counts = a.op == "random" ? "1 or 3" : most == f.args ? std::to_string(f.args) : std::format("{} or {}", f.args, most);
            return a.op + "() takes " + counts + " arguments";
        }
    }
    for (const auto& c : a.args)
        if (std::string why = uncompilable(*c, g); !why.empty())
            return why;
    return {};
}

/** A value an expression builds: a plug to connect, or a constant to set. */
struct Value
{
    std::optional<PlugRef> plug;
    double constant = 0.0;
};

class Compiler
{
public:
    Compiler(Graph& g, std::string target, std::string label) : g(g), mTarget(std::move(target)), mLabel(std::move(label)) {}

    Value build(const Ast& a)
    {
        switch (a.kind)
        {
        case Ast::Number: return {std::nullopt, a.number};
        case Ast::Plug: return {g.plug(a.op), 0.0};
        case Ast::Unary:
            if (a.op == "-")
                return op("negate", rigmath::Kind::Float, {build(*a.args[0])});
            return op("not", rigmath::Kind::Bool, {build(*a.args[0])});
        case Ast::Binary:
        {
            static const std::map<std::string, std::pair<const char*, rigmath::Kind>> kOps = {
                {"+", {"add", rigmath::Kind::Float}},      {"-", {"subtract", rigmath::Kind::Float}}, {"*", {"multiply", rigmath::Kind::Float}},
                {"/", {"divide", rigmath::Kind::Float}},   {"**", {"pow", rigmath::Kind::Float}},     {"and", {"and", rigmath::Kind::Bool}},
                {"or", {"or", rigmath::Kind::Bool}}};
            if (a.op == "%")
                return op("fmod", rigmath::Kind::Float, {build(*a.args[0]), build(*a.args[1])}, 1);
            const auto& [fn, kind] = kOps.at(a.op);
            return op(fn, kind, {build(*a.args[0]), build(*a.args[1])});
        }
        case Ast::Compare:
        {
            static const std::map<std::string, const char*> kCmp = {{"<", "less"},     {"<=", "lessEqual"}, {">", "greater"},
                                                                    {">=", "greaterEqual"}, {"==", "equal"}, {"!=", "notEqual"}};
            return op(kCmp.at(a.op), rigmath::Kind::Float, {build(*a.args[0]), build(*a.args[1])});
        }
        case Ast::Ternary:
            // select's pins are a, b, pickA.
            return op("select", rigmath::Kind::Float, {build(*a.args[0]), build(*a.args[2]), build(*a.args[1])});
        case Ast::Time: return time(a.op == "frame");
        case Ast::Call:
        {
            const FnInfo& f = fnTable().at(a.op);
            std::vector<Value> args;
            for (const auto& c : a.args)
                args.push_back(build(*c));
            if (a.op == "log" && args.size() == 2)
                return op("log", rigmath::Kind::Float, std::move(args));
            if (a.op == "random" && args.size() == 3)
                return op("randomFloatInRangeFromStream", rigmath::Kind::Float, {args[1], args[2], args[0]});
            Value v = op(f.rigmath, f.kind, std::move(args), f.output, f.pins, f.fixed);
            if (a.op == "smoothstep")
            {
                // t * t * (3 - 2 * t), t the clamped place in min..max.
                const Value rest = op("subtract", rigmath::Kind::Float, {{std::nullopt, 3.0}, op("multiply", rigmath::Kind::Float, {v, {std::nullopt, 2.0}})});
                return op("multiply", rigmath::Kind::Float, {op("multiply", rigmath::Kind::Float, {v, v}), rest});
            }
            return v;
        }
        }
        throw std::invalid_argument("unknown expression part");
    }

    std::vector<NodeId> nodes;

private:
    /** The scene time in seconds, or in frames: one sceneTime node per expression. */
    Value time(bool frames)
    {
        if (mTime == kNoNode)
        {
            mTime = g.createNode(*types::find("sceneTime"), mLabel + "_time")->id();
            nodes.push_back(mTime);
        }
        return {PlugRef{mTime, frames ? 1 : 0}, 0.0};
    }

    Value op(const std::string& fn, rigmath::Kind kind, std::vector<Value> args, int output = 0, const std::vector<int>& pins = {},
             const std::vector<std::pair<int, double>>& fixed = {})
    {
        const rigmath::Overload* o = overload(fn, kind);
        if (!o)
            throw std::invalid_argument("no rig math " + fn);
        const NodeType* type = types::find(o->nodeType);
        if (!type)
            throw std::invalid_argument("rig math node " + o->nodeType + " isn't registered");
        Node* n = g.createNode(*type, mLabel + "_" + fn);
        nodes.push_back(n->id());
        auto set = [&](size_t input, const Value& v) {
            const int pin = n->type().attrIndex(o->inputs[input].name);
            if (v.plug)
                g.connect(*v.plug, {n->id(), pin});
            else
            {
                const AttrDef& d = n->type().attrs[size_t(pin)];
                g.setValue({n->id(), pin}, d.type == PlugType::Bool ? PlugValue(v.constant != 0.0)
                                           : d.type == PlugType::Int ? PlugValue(int64_t(std::llround(v.constant)))
                                                                     : PlugValue(v.constant));
            }
        };
        for (size_t i = 0; i < args.size(); ++i)
            if (const size_t input = i < pins.size() ? size_t(pins[i]) : i; input < o->inputs.size())
                set(input, args[i]);
        for (const auto& [input, value] : fixed)
            if (size_t(input) < o->inputs.size())
                set(size_t(input), {std::nullopt, value});
        return {PlugRef{n->id(), n->type().attrIndex(o->outputs[size_t(output)].name)}, 0.0};
    }

    Graph& g;
    std::string mTarget;
    std::string mLabel;
    NodeId mTime = kNoNode;
};

/** Plug paths in text as (offset, length), for Python: dotted names that resolve, outside strings. */
std::vector<std::pair<size_t, size_t>> plugSpans(Graph& g, const std::string& s)
{
    std::vector<std::pair<size_t, size_t>> out;
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        if (quote)
        {
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '"' || c == '\'')
        {
            quote = c;
            continue;
        }
        if (!(std::isalpha(static_cast<unsigned char>(c)) || c == '_') || (i > 0 && (s[i - 1] == '.' || std::isalnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '_')))
            continue;
        size_t j = i;
        while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_' || s[j] == ':' || s[j] == '.' || s[j] == '[' || s[j] == ']'))
            ++j;
        const std::string name = s.substr(i, j - i);
        if (name.find('.') != std::string::npos)
            try
            {
                g.plug(name);
                out.emplace_back(i, j - i);
            }
            catch (const std::exception&)
            {
            }
        i = j - 1;
    }
    return out;
}

PythonEvaluator& evaluator()
{
    static PythonEvaluator fn;
    return fn;
}
std::mutex& evaluatorLock()
{
    static std::mutex m;
    return m;
}

class SceneTimeNode final : public Node
{
public:
    void compute(const EvalContext& ctx) override
    {
        setOutput(0, ctx.time);
        setOutput(1, anim::frameOf(ctx.time));
    }
};

class PythonNode final : public Node
{
public:
    enum Attr
    {
        kCode,
        kInputs,
        kOutput,
        kError,
        kTime,
        kFrame,
    };
    void compute(const EvalContext&) override
    {
        LILA_ZONE();
        std::vector<double> values;
        for (const auto& [index, v] : elements(kInputs))
        {
            if (size_t(index) >= values.size())
                values.resize(size_t(index) + 1, 0.0);
            values[size_t(index)] = std::get<double>(v);
        }
        PythonEvaluator fn;
        {
            std::lock_guard lock(evaluatorLock());
            fn = evaluator();
        }
        std::string error;
        std::optional<double> r = fn ? fn(as<std::string>(kCode), values, as<double>(kTime), as<double>(kFrame), &error) : std::nullopt;
        if (!fn)
            error = "Python isn't running";
        setOutput(kOutput, r.value_or(0.0));
        setOutput(kError, r ? std::string() : error);
    }
};

json tagOf(const Node& n)
{
    const int i = n.type().attrIndex(kTag);
    if (i < 0)
        return nullptr;
    json j = json::parse(n.as<std::string>(i), nullptr, false);
    return j.is_object() ? j : json();
}

void tag(Graph& g, NodeId id, const json& value)
{
    const int i = g.addAttr(id, AttrDef{.name = kTag, .type = PlugType::String, .defaultValue = std::string(), .flags = AttrHidden,
                                         .doc = "The expression this node belongs to."});
    g.setValue({id, i}, value.dump());
}

/** Nodes of the expression on target (its full plug name), the root (the one with the text) first. */
std::vector<NodeId> expressionNodes(Graph& g, const std::string& target)
{
    std::vector<NodeId> out;
    for (NodeId id : g.nodes())
        if (json t = tagOf(*g.node(id)); t.is_object() && t.value("target", "") == target)
        {
            if (t.contains("text"))
                out.insert(out.begin(), id);
            else
                out.push_back(id);
        }
    return out;
}
} // namespace

std::unique_ptr<Ast> parse(const std::string& text) { return Parser(tokenize(text)).parseAll(); }

const std::vector<std::pair<std::string, std::string>>& functions()
{
    static const std::vector<std::pair<std::string, std::string>> list = [] {
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto& [name, f] : fnTable())
            out.emplace_back(name, f.rigmath);
        return out;
    }();
    return list;
}

bool compiles(const std::string& text, std::string* why)
{
    try
    {
        std::unique_ptr<Ast> a = parse(text);
        std::string reason = uncompilable(*a, Graph::current());
        if (why)
            *why = reason;
        return reason.empty();
    }
    catch (const std::invalid_argument& e)
    {
        if (why)
            *why = e.what();
        return false;
    }
}

double evaluate(const Ast& a, const std::function<double(const std::string&)>& plug, double time, double frame)
{
    auto run = [](const char* fn, std::vector<double> in, int output = 0) {
        const lila_expr::Op op = lila_expr::opNamed(fn);
        in.resize(size_t(std::max(lila_expr::info(op).inputs, int(in.size()))), 0.0);
        double out[2];
        lila_expr::apply(op, in.data(), out);
        return out[output];
    };
    std::vector<double> args;
    for (const auto& c : a.args)
        args.push_back(evaluate(*c, plug, time, frame));
    switch (a.kind)
    {
    case Ast::Number: return a.number;
    case Ast::Plug: return plug(a.op);
    case Ast::Time: return a.op == "frame" ? frame : time;
    case Ast::Unary: return run(a.op == "-" ? "negate" : "not", args);
    case Ast::Binary:
    {
        static const std::map<std::string, const char*> kOps = {{"+", "add"},  {"-", "subtract"}, {"*", "multiply"}, {"/", "divide"},
                                                                {"**", "pow"}, {"and", "and"},     {"or", "or"}};
        if (a.op == "%")
            return run("fmod", args, 1);
        return run(kOps.at(a.op), args);
    }
    case Ast::Compare:
    {
        static const std::map<std::string, const char*> kCmp = {{"<", "less"},         {"<=", "lessEqual"}, {">", "greater"},
                                                                {">=", "greaterEqual"}, {"==", "equal"},     {"!=", "notEqual"}};
        return run(kCmp.at(a.op), args);
    }
    case Ast::Ternary: return run("select", {args[0], args[2], args[1]});
    case Ast::Call:
    {
        auto it = fnTable().find(a.op);
        if (it == fnTable().end())
            throw std::invalid_argument(a.op + "() isn't a function expressions know");
        const FnInfo& f = it->second;
        if (!arityOk(a.op, f, int(args.size())))
            throw std::invalid_argument(a.op + "() takes " + (a.op == "random" ? std::string("1 or 3") : std::to_string(f.args)) + " arguments");
        if (a.op == "log" && args.size() == 2)
            return run("log", args);
        if (a.op == "random" && args.size() == 3)
            return run("randomFloatInRangeFromStream", {args[1], args[2], args[0]});
        std::vector<double> in = args;
        if (!f.pins.empty())
        {
            in.assign(size_t(lila_expr::info(lila_expr::opNamed(f.rigmath.c_str())).inputs), 0.0);
            for (size_t i = 0; i < args.size(); ++i)
                in[size_t(f.pins[i])] = args[i];
            for (const auto& [pin, v] : f.fixed)
                in[size_t(pin)] = v;
        }
        const double v = run(f.rigmath.c_str(), in, f.output);
        return a.op == "smoothstep" ? v * v * (3.0 - 2.0 * v) : v;
    }
    }
    throw std::invalid_argument("unknown expression part");
}

std::vector<PlugRef> targets(Graph& g)
{
    std::vector<PlugRef> out;
    for (NodeId id : g.nodes())
        if (json t = tagOf(*g.node(id)); t.is_object() && t.contains("text"))
            try
            {
                out.push_back(g.plug(t.value("target", "")));
            }
            catch (const std::exception&)
            {
            }
    return out;
}

ProgramText program(Graph& g, const PlugRef& target)
{
    ProgramText out;
    const std::string key = g.fullPlugName(target);
    std::vector<NodeId> nodes = expressionNodes(g, key);
    if (nodes.empty())
    {
        out.why = "no expression";
        return out;
    }
    if (!tagOf(*g.node(nodes[0])).value("native", true))
    {
        out.why = "runs as Python";
        return out;
    }
    const std::optional<PlugRef> root = g.source(target);
    if (!root)
    {
        out.why = "the expression isn't connected to " + g.plugName(target);
        return out;
    }
    const std::set<NodeId> mine(nodes.begin(), nodes.end());
    std::map<NodeId, int> step;
    std::map<PlugRef, int> inputIndex;
    std::vector<std::string> lines;
    auto outputIndex = [](const rigmath::Overload& o, const Node& n, int attr) {
        for (size_t k = 0; k < o.outputs.size(); ++k)
            if (n.type().attrIndex(o.outputs[k].name) == attr)
                return int(k);
        return -1;
    };
    // An argument naming where a plug's value comes from; emits upstream nodes first.
    std::function<std::string(const PlugRef&)> arg;
    std::function<bool(NodeId)> emit = [&](NodeId id) -> bool {
        if (step.count(id))
            return true;
        const Node& n = *g.node(id);
        const rigmath::Overload* o = rigmath::overloadFor(n.type().name);
        const rigmath::Function* f = rigmath::functionFor(n.type().name);
        const lila_expr::Op op = f ? lila_expr::opNamed(f->name) : lila_expr::Op::Count;
        if (!o || op == lila_expr::Op::Count)
        {
            out.why = n.name() + " (" + n.type().name + ") has no Unreal equivalent";
            return false;
        }
        const rigmath::Kind expect = f->name == "not" || f->name == "and" || f->name == "or" ? rigmath::Kind::Bool
                                     : f->name == "randomFloatFromStream"                  ? rigmath::Kind::Int
                                                                                           : rigmath::Kind::Float;
        if (overload(f->name, expect) != o)
        {
            out.why = n.name() + " uses the " + rigmath::kindName(o->inputs.empty() ? expect : o->inputs[0].kind) + " form of " + f->name;
            return false;
        }
        std::string line = f->name;
        for (const rigmath::Pin& pin : o->inputs)
        {
            const PlugRef in{id, n.type().attrIndex(pin.name)};
            std::string a = arg(in);
            if (a.empty())
                return false;
            line += " " + a;
        }
        step[id] = int(lines.size());
        lines.push_back(std::move(line));
        return true;
    };
    arg = [&](const PlugRef& in) -> std::string {
        if (const std::optional<PlugRef> src = g.source(in))
        {
            const Node& s = *g.node(src->node);
            if (mine.count(src->node) && s.type().name == "sceneTime")
            {
                out.usesTime = true;
                return src->attr == 0 ? "t" : "f";
            }
            if (mine.count(src->node))
            {
                if (!emit(src->node))
                    return {};
                const int k = outputIndex(*rigmath::overloadFor(s.type().name), s, src->attr);
                return std::format("n{}.{}", step[src->node], std::max(k, 0));
            }
            auto [it, added] = inputIndex.emplace(*src, int(inputIndex.size()));
            if (added)
                out.inputs.push_back(*src);
            return "i" + std::to_string(it->second);
        }
        const PlugValue v = g.value(in);
        double d = 0.0;
        if (const double* x = std::get_if<double>(&v))
            d = *x;
        else if (const int64_t* i = std::get_if<int64_t>(&v))
            d = double(*i);
        else if (const bool* b = std::get_if<bool>(&v))
            d = *b ? 1.0 : 0.0;
        return std::format("{}", d);
    };
    std::string result;
    if (mine.count(root->node))
        result = arg(target);
    if (result.empty())
    {
        if (out.why.empty())
            out.why = "the expression isn't connected to " + g.plugName(target);
        out.inputs.clear();
        return out;
    }
    out.text = std::format("fps {}\ninputs {}\n", anim::playback().fps, out.inputs.size());
    for (const std::string& l : lines)
        out.text += l + "\n";
    out.text += "= " + result + "\n";
    return out;
}

void setPythonEvaluator(PythonEvaluator fn)
{
    std::lock_guard lock(evaluatorLock());
    evaluator() = std::move(fn);
}

void registerTypes()
{
    if (types::find("pythonExpression"))
        return;
    NodeType t;
    t.name = "pythonExpression";
    t.icon = "brand-python";
    t.doc = "An expression that runs as Python each time its inputs change: slower than rig math nodes and Lila only.";
    t.ownAttrs = {
        {.name = "code", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrCode,
         .doc = "Python returning a number; v0, v1 ... are the inputs, math's functions are in scope."},
        {.name = "inputs", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrInput | AttrStorable | AttrArray, .doc = "The plugs the code reads."},
        {.name = "output", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrOutput, .doc = "The result."},
        {.name = "error", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput, .doc = "Why the last run failed."},
        {.name = "time", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrInput, .doc = "Seconds, as time in the code."},
        {.name = "frame", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrInput, .doc = "The frame, as frame in the code."},
    };
    t.create = [] { return std::make_unique<PythonNode>(); };
    types::registerType(std::move(t));
    relocate::registerNeutral("pythonExpression");

    NodeType st;
    st.name = "sceneTime";
    st.icon = "clock";
    st.timeDependent = true;
    st.doc = "The scene's current time, in seconds and in frames: what time and frame read in an expression.";
    st.ownAttrs = {
        {.name = "time", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrOutput, .doc = "Seconds."},
        {.name = "frame", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrOutput, .doc = "Seconds times the scene's frame rate."},
    };
    st.create = [] { return std::make_unique<SceneTimeNode>(); };
    types::registerType(std::move(st));
    relocate::registerNeutral("sceneTime");
}

void registerCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "expression",
        .doc = "Drive a plug with an expression in Python syntax: expression('box.ty', string='ctrl.ry * 0.5 + clamp(foot.roll, 0, 90)'). "
               "Numbers, other plugs by name, time (seconds) and frame, + - * / % **, comparisons, and/or/not, `a if c else b` and abs, "
               "min, max, clamp, lerp, sin, cos, tan, asin, acos, atan, atan2, sqrt, exp, pow, log, sign, fmod, hypot, degrees, radians, "
               "wrap, mapRange, floor, ceil, round, trunc, noise(x, seed), random(seed) or random(seed, min, max), linstep(min, max, x) "
               "and smoothstep(min, max, x) compile to rig math nodes: as fast as nodes, shown in the node editor, and sent to Unreal "
               "Control Rig by unrealExpressionRig. Anything else runs as Python with time and frame in scope (slower, Lila only). "
               "Returns {native, nodes, reason}; query adds the Unreal program.",
        .positionalDoc = "plug",
        .flags = {{"string", "s", T::String, nullptr, "The expression; a leading = is optional."},
                  {"python", "py", T::Bool, false, "Run it as Python even when it compiles to nodes."},
                  {"query", "q", T::Bool, false, "Return {text, native, nodes, program, programInputs} for the plug's expression, or None."},
                  {"delete", "d", T::Bool, false, "Remove the plug's expression and its nodes."}},
        .run = [](const Args& a) -> json {
            if (a.positional.size() != 1)
                throw CommandError("expression: expected a plug");
            Graph& g = Graph::current();
            PlugRef target;
            try
            {
                target = g.plug(a.positional[0].get<std::string>());
            }
            catch (const std::invalid_argument& e)
            {
                throw CommandError(e.what());
            }
            const std::string key = g.fullPlugName(target);
            std::vector<NodeId> existing = expressionNodes(g, key);
            if (a.get<bool>("query"))
            {
                if (existing.empty())
                    return nullptr;
                json root = tagOf(*g.node(existing[0]));
                json nodes = json::array();
                for (NodeId id : existing)
                    nodes.push_back(g.node(id)->path());
                json out = {{"text", root.value("text", "")}, {"native", root.value("native", true)}, {"nodes", nodes}};
                if (out["native"].get<bool>())
                {
                    ProgramText p = program(g, target);
                    json inputs = json::array();
                    for (const PlugRef& in : p.inputs)
                        inputs.push_back(g.plugName(in));
                    out["program"] = p.text.empty() ? json(nullptr) : json(p.text);
                    out["programInputs"] = inputs;
                    if (!p.why.empty())
                        out["unrealReason"] = p.why;
                }
                return out;
            }
            for (NodeId id : existing)
                if (g.node(id))
                    g.deleteNode(id);
            if (a.get<bool>("delete"))
                return nullptr;
            std::string text = a.get<std::string>("string", "");
            while (!text.empty() && (text.front() == '=' || std::isspace(static_cast<unsigned char>(text.front()))))
                text.erase(text.begin());
            if (text.empty())
                throw CommandError("expression: give string, the expression");
            const AttrDef& def = g.attr(target);
            const bool scalar = target.component >= 0 || def.type == PlugType::Float || def.type == PlugType::Int || def.type == PlugType::Bool ||
                                def.type == PlugType::Enum;
            if (!scalar)
                throw CommandError("expression: " + g.plugName(target) + " isn't a number; drive one of its components");
            const std::string label = "expr_" + g.node(target.node)->name();
            std::string why;
            const bool native = !a.get<bool>("python") && compiles(text, &why);
            // Unbalanced or unfinished text is a mistake in Python too, not a reason to run it there.
            if (!native && (why.rfind("the expression ends early", 0) == 0 || why.rfind("expected '", 0) == 0))
                throw CommandError("expression: " + why);
            json nodes = json::array();
            try
            {
                if (native)
                {
                    std::unique_ptr<Ast> ast = parse(text);
                    Compiler c(g, key, label);
                    Value v = c.build(*ast);
                    if (c.nodes.empty())
                    {
                        // A constant or a lone plug: a node still holds it, so the expression has somewhere to keep its text.
                        const rigmath::Overload* o = overload("add", rigmath::Kind::Float);
                        Node* n = g.createNode(*types::find(o->nodeType), label + "_value");
                        c.nodes.push_back(n->id());
                        const int in = n->type().attrIndex(o->inputs[0].name);
                        if (v.plug)
                            g.connect(*v.plug, {n->id(), in});
                        else
                            g.setValue({n->id(), in}, v.constant);
                        g.setValue({n->id(), n->type().attrIndex(o->inputs[1].name)}, 0.0);
                        v.plug = PlugRef{n->id(), n->type().attrIndex(o->outputs[0].name)};
                    }
                    g.connect(*v.plug, target, true);
                    const NodeId root = v.plug->node;
                    for (NodeId id : c.nodes)
                    {
                        json t = {{"target", key}};
                        if (id == root)
                            t.update({{"text", text}, {"native", true}});
                        tag(g, id, t);
                        nodes.push_back(g.node(id)->path());
                    }
                }
                else
                {
                    Node* n = g.createNode(*types::find("pythonExpression"), label + "_py");
                    std::string code;
                    size_t at = 0;
                    int index = 0;
                    for (auto [start, length] : plugSpans(g, text))
                    {
                        code += text.substr(at, start - at) + "v" + std::to_string(index);
                        g.connect(g.plug(text.substr(start, length)), {n->id(), PythonNode::kInputs, -1, index});
                        at = start + length;
                        ++index;
                    }
                    code += text.substr(at);
                    g.setValue({n->id(), PythonNode::kCode}, code);
                    g.connect({n->id(), PythonNode::kOutput}, target, true);
                    tag(g, n->id(), {{"target", key}, {"text", text}, {"native", false}});
                    nodes.push_back(n->path());
                    if (usesName(code, "time") || usesName(code, "frame"))
                    {
                        Node* clock = g.createNode(*types::find("sceneTime"), label + "_time");
                        g.connect({clock->id(), 0}, {n->id(), PythonNode::kTime});
                        g.connect({clock->id(), 1}, {n->id(), PythonNode::kFrame});
                        tag(g, clock->id(), {{"target", key}});
                        nodes.push_back(clock->path());
                    }
                }
            }
            catch (const std::invalid_argument& e)
            {
                throw CommandError(std::string("expression: ") + e.what());
            }
            json out = {{"native", native}, {"nodes", nodes}};
            if (!native)
                out["reason"] = a.get<bool>("python") ? std::string("asked for Python") : why;
            return out;
        },
    });
}
} // namespace lila::scene::expressions
