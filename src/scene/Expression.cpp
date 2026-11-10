#include "scene/Expression.h"

#include "core/Profile.h"
#include "scene/Graph.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <map>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace lila::scene::expression
{
namespace
{
// ------------------------------------------------------------------------------------------------ syntax tree

enum class Op : uint8_t
{
    Number,
    Local,
    Input,
    Output,
    Time,
    Frame,
    Neg,
    Not,
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    And,
    Or,
    Call,
};

enum class Fn : uint8_t
{
    Sin,
    Cos,
    Tan,
    Asin,
    Acos,
    Atan,
    Atan2,
    Abs,
    Min,
    Max,
    Clamp,
    Pow,
    Sqrt,
    Exp,
    Log,
    Floor,
    Ceil,
    Sign,
    DegToRad,
    RadToDeg,
};

struct Expr
{
    Op op = Op::Number;
    double value = 0.0;
    int index = 0; // local slot, input or output index
    Fn fn = Fn::Sin;
    std::vector<Expr> args;
};

struct Stmt
{
    enum Kind : uint8_t
    {
        AssignLocal,
        AssignOutput,
        If,
        Block,
    } kind = Block;
    int index = 0;
    Expr value; // the assigned value, or an if's condition
    std::vector<Stmt> body, otherwise;
};

const std::map<std::string, std::pair<Fn, int>, std::less<>>& functions()
{
    static const std::map<std::string, std::pair<Fn, int>, std::less<>> k = {
        {"sin", {Fn::Sin, 1}},     {"cos", {Fn::Cos, 1}},     {"tan", {Fn::Tan, 1}},     {"asin", {Fn::Asin, 1}},
        {"acos", {Fn::Acos, 1}},   {"atan", {Fn::Atan, 1}},   {"atan2", {Fn::Atan2, 2}}, {"abs", {Fn::Abs, 1}},
        {"min", {Fn::Min, 2}},     {"max", {Fn::Max, 2}},     {"clamp", {Fn::Clamp, 3}}, {"pow", {Fn::Pow, 2}},
        {"sqrt", {Fn::Sqrt, 1}},   {"exp", {Fn::Exp, 1}},     {"log", {Fn::Log, 1}},     {"floor", {Fn::Floor, 1}},
        {"ceil", {Fn::Ceil, 1}},   {"sign", {Fn::Sign, 1}},   {"deg_to_rad", {Fn::DegToRad, 1}},
        {"rad_to_deg", {Fn::RadToDeg, 1}}};
    return k;
}

// ------------------------------------------------------------------------------------------------ parser

class Parser
{
public:
    explicit Parser(std::string_view text) : mText(text) {}

    std::vector<Stmt> program()
    {
        std::vector<Stmt> out;
        skip();
        while (mPos < mText.size())
        {
            statement(out);
            skip();
        }
        return out;
    }

    int locals() const { return int(mLocals.size()); }

private:
    [[noreturn]] void fail(const std::string& what) const
    {
        throw std::invalid_argument(std::format("{} at character {}", what, mPos + 1));
    }

    void skip()
    {
        for (;;)
        {
            while (mPos < mText.size() && std::isspace(static_cast<unsigned char>(mText[mPos])))
                ++mPos;
            if (mText.substr(mPos, 2) == "//")
            {
                while (mPos < mText.size() && mText[mPos] != '\n')
                    ++mPos;
                continue;
            }
            if (mText.substr(mPos, 2) == "/*")
            {
                size_t end = mText.find("*/", mPos + 2);
                mPos = end == std::string_view::npos ? mText.size() : end + 2;
                continue;
            }
            return;
        }
    }

    bool accept(std::string_view token)
    {
        skip();
        if (mText.substr(mPos, token.size()) != token)
            return false;
        // A word must not run on into a longer one.
        const bool word = std::isalpha(static_cast<unsigned char>(token.back())) != 0;
        if (word && mPos + token.size() < mText.size() && (std::isalnum(static_cast<unsigned char>(mText[mPos + token.size()])) ||
                                                          mText[mPos + token.size()] == '_'))
            return false;
        mPos += token.size();
        return true;
    }

    void expect(std::string_view token)
    {
        if (!accept(token))
            fail(std::format("expected '{}'", token));
    }

    std::string identifier()
    {
        skip();
        size_t start = mPos;
        if (mPos < mText.size() && mText[mPos] == '$')
            ++mPos;
        if (mPos >= mText.size() || !(std::isalpha(static_cast<unsigned char>(mText[mPos])) || mText[mPos] == '_'))
            fail("expected a name");
        while (mPos < mText.size() && (std::isalnum(static_cast<unsigned char>(mText[mPos])) || mText[mPos] == '_'))
            ++mPos;
        std::string name(mText.substr(start, mPos - start));
        if (name.starts_with('$'))
            name.erase(0, 1);
        return name;
    }

    /** in[n], out[n], .I[n], .O[n]: which (true for an input) and n; false when the text isn't one. */
    bool slot(bool* input, int* index)
    {
        skip();
        const size_t save = mPos;
        if (accept(".I"))
            *input = true;
        else if (accept(".O"))
            *input = false;
        else if (accept("in"))
            *input = true;
        else if (accept("out"))
            *input = false;
        else
            return false;
        if (!accept("["))
        {
            mPos = save;
            return false;
        }
        skip();
        int n = 0;
        auto [ptr, ec] = std::from_chars(mText.data() + mPos, mText.data() + mText.size(), n);
        if (ec != std::errc() || n < 0)
            fail("expected an index");
        mPos = size_t(ptr - mText.data());
        expect("]");
        if (!*input && n >= kOutputs)
            fail(std::format("only {} outputs", kOutputs));
        *index = n;
        return true;
    }

    void statement(std::vector<Stmt>& out)
    {
        if (accept(";"))
            return;
        if (accept("{"))
        {
            Stmt b;
            b.kind = Stmt::Block;
            while (!accept("}"))
            {
                if (skip(), mPos >= mText.size())
                    fail("expected '}'");
                statement(b.body);
            }
            out.push_back(std::move(b));
            return;
        }
        if (accept("if"))
        {
            Stmt s;
            s.kind = Stmt::If;
            expect("(");
            s.value = expression();
            expect(")");
            statement(s.body);
            if (accept("else"))
                statement(s.otherwise);
            out.push_back(std::move(s));
            return;
        }
        if (accept("float"))
        {
            do
            {
                const std::string name = identifier();
                const int index = localSlot(name, true);
                Stmt s;
                s.kind = Stmt::AssignLocal;
                s.index = index;
                if (accept("="))
                    s.value = expression();
                out.push_back(std::move(s));
            } while (accept(","));
            expect(";");
            return;
        }
        bool input = false;
        int index = 0;
        Stmt s;
        if (slot(&input, &index))
        {
            if (input)
                fail("inputs can't be assigned");
            s.kind = Stmt::AssignOutput;
        }
        else
        {
            s.kind = Stmt::AssignLocal;
            index = localSlot(identifier(), false);
        }
        s.index = index;
        expect("=");
        s.value = expression();
        expect(";");
        out.push_back(std::move(s));
    }

    int localSlot(const std::string& name, bool declare)
    {
        if (name == "time" || name == "frame" || functions().contains(name))
            fail(std::format("'{}' is reserved", name));
        auto it = mLocals.find(name);
        if (it != mLocals.end())
            return it->second;
        if (!declare)
            fail(std::format("'{}' is not declared", name));
        const int slot = int(mLocals.size());
        mLocals[name] = slot;
        return slot;
    }

    Expr binary(Op op, Expr a, Expr b)
    {
        Expr e;
        e.op = op;
        e.args.push_back(std::move(a));
        e.args.push_back(std::move(b));
        return e;
    }

    Expr expression() { return orExpr(); }

    Expr orExpr()
    {
        Expr e = andExpr();
        while (accept("||"))
            e = binary(Op::Or, std::move(e), andExpr());
        return e;
    }

    Expr andExpr()
    {
        Expr e = compare();
        while (accept("&&"))
            e = binary(Op::And, std::move(e), compare());
        return e;
    }

    Expr compare()
    {
        Expr e = sum();
        for (;;)
        {
            if (accept("=="))
                e = binary(Op::Eq, std::move(e), sum());
            else if (accept("!="))
                e = binary(Op::Ne, std::move(e), sum());
            else if (accept("<="))
                e = binary(Op::Le, std::move(e), sum());
            else if (accept(">="))
                e = binary(Op::Ge, std::move(e), sum());
            else if (accept("<"))
                e = binary(Op::Lt, std::move(e), sum());
            else if (accept(">"))
                e = binary(Op::Gt, std::move(e), sum());
            else
                return e;
        }
    }

    Expr sum()
    {
        Expr e = product();
        for (;;)
        {
            if (accept("+"))
                e = binary(Op::Add, std::move(e), product());
            else if (accept("-"))
                e = binary(Op::Sub, std::move(e), product());
            else
                return e;
        }
    }

    Expr product()
    {
        Expr e = unary();
        for (;;)
        {
            if (accept("*"))
                e = binary(Op::Mul, std::move(e), unary());
            else if (accept("/"))
                e = binary(Op::Div, std::move(e), unary());
            else if (accept("%"))
                e = binary(Op::Mod, std::move(e), unary());
            else
                return e;
        }
    }

    Expr unary()
    {
        if (accept("-"))
        {
            Expr e;
            e.op = Op::Neg;
            e.args.push_back(unary());
            return e;
        }
        if (accept("+"))
            return unary();
        if (accept("!"))
        {
            Expr e;
            e.op = Op::Not;
            e.args.push_back(unary());
            return e;
        }
        return primary();
    }

    Expr primary()
    {
        skip();
        if (accept("("))
        {
            Expr e = expression();
            expect(")");
            return e;
        }
        Expr e;
        bool input = false;
        if (slot(&input, &e.index))
        {
            e.op = input ? Op::Input : Op::Output;
            return e;
        }
        if (mPos < mText.size() && (std::isdigit(static_cast<unsigned char>(mText[mPos])) || mText[mPos] == '.'))
        {
            auto [ptr, ec] = std::from_chars(mText.data() + mPos, mText.data() + mText.size(), e.value);
            if (ec != std::errc())
                fail("expected a number");
            mPos = size_t(ptr - mText.data());
            e.op = Op::Number;
            return e;
        }
        const std::string name = identifier();
        if (name == "time")
        {
            e.op = Op::Time;
            return e;
        }
        if (name == "frame")
        {
            e.op = Op::Frame;
            return e;
        }
        if (auto f = functions().find(name); f != functions().end())
        {
            e.op = Op::Call;
            e.fn = f->second.first;
            expect("(");
            if (!accept(")"))
            {
                do
                    e.args.push_back(expression());
                while (accept(","));
                expect(")");
            }
            if (int(e.args.size()) != f->second.second)
                fail(std::format("{} takes {} argument(s)", name, f->second.second));
            return e;
        }
        e.op = Op::Local;
        e.index = localSlot(name, false);
        return e;
    }

    std::string_view mText;
    size_t mPos = 0;
    std::map<std::string, int, std::less<>> mLocals;
};

// ------------------------------------------------------------------------------------------------ evaluation

struct State
{
    Program::Env& env;
    std::vector<double> locals;
};

double eval(const Expr& e, State& st)
{
    auto a = [&](size_t i) { return eval(e.args[i], st); };
    switch (e.op)
    {
    case Op::Number: return e.value;
    case Op::Local: return st.locals[size_t(e.index)];
    case Op::Input: return size_t(e.index) < st.env.inputCount ? st.env.inputs[e.index] : 0.0;
    case Op::Output: return st.env.outputs[e.index];
    case Op::Time: return st.env.time;
    case Op::Frame: return st.env.frame;
    case Op::Neg: return -a(0);
    case Op::Not: return a(0) == 0.0 ? 1.0 : 0.0;
    case Op::Add: return a(0) + a(1);
    case Op::Sub: return a(0) - a(1);
    case Op::Mul: return a(0) * a(1);
    case Op::Div:
    {
        const double d = a(1);
        return d == 0.0 ? 0.0 : a(0) / d;
    }
    case Op::Mod:
    {
        const double d = a(1);
        return d == 0.0 ? 0.0 : std::fmod(a(0), d);
    }
    case Op::Eq: return a(0) == a(1) ? 1.0 : 0.0;
    case Op::Ne: return a(0) != a(1) ? 1.0 : 0.0;
    case Op::Lt: return a(0) < a(1) ? 1.0 : 0.0;
    case Op::Le: return a(0) <= a(1) ? 1.0 : 0.0;
    case Op::Gt: return a(0) > a(1) ? 1.0 : 0.0;
    case Op::Ge: return a(0) >= a(1) ? 1.0 : 0.0;
    case Op::And: return a(0) != 0.0 && a(1) != 0.0 ? 1.0 : 0.0;
    case Op::Or: return a(0) != 0.0 || a(1) != 0.0 ? 1.0 : 0.0;
    case Op::Call:
        switch (e.fn)
        {
        case Fn::Sin: return std::sin(a(0));
        case Fn::Cos: return std::cos(a(0));
        case Fn::Tan: return std::tan(a(0));
        case Fn::Asin: return std::asin(std::clamp(a(0), -1.0, 1.0));
        case Fn::Acos: return std::acos(std::clamp(a(0), -1.0, 1.0));
        case Fn::Atan: return std::atan(a(0));
        case Fn::Atan2: return std::atan2(a(0), a(1));
        case Fn::Abs: return std::abs(a(0));
        case Fn::Min: return std::min(a(0), a(1));
        case Fn::Max: return std::max(a(0), a(1));
        // Maya's argument order: clamp(min, max, value).
        case Fn::Clamp: return std::min(std::max(a(2), a(0)), a(1));
        case Fn::Pow: return std::pow(a(0), a(1));
        case Fn::Sqrt: return std::sqrt(std::max(a(0), 0.0));
        case Fn::Exp: return std::exp(a(0));
        case Fn::Log: return std::log(a(0));
        case Fn::Floor: return std::floor(a(0));
        case Fn::Ceil: return std::ceil(a(0));
        case Fn::Sign:
        {
            const double v = a(0);
            return v > 0.0 ? 1.0 : v < 0.0 ? -1.0 : 0.0;
        }
        case Fn::DegToRad: return a(0) * std::numbers::pi / 180.0;
        case Fn::RadToDeg: return a(0) * 180.0 / std::numbers::pi;
        }
    }
    return 0.0;
}

void exec(const std::vector<Stmt>& body, State& st)
{
    for (const Stmt& s : body)
        switch (s.kind)
        {
        case Stmt::AssignLocal: st.locals[size_t(s.index)] = eval(s.value, st); break;
        case Stmt::AssignOutput: st.env.outputs[s.index] = eval(s.value, st); break;
        case Stmt::If: exec(eval(s.value, st) != 0.0 ? s.body : s.otherwise, st); break;
        case Stmt::Block: exec(s.body, st); break;
        }
}

enum Attr
{
    kExpression,
    kInput,
    kFps,
    kError,
    kFirstOutput,
};

class ExpressionNode : public Node
{
public:
    void compute(const EvalContext& ctx) override
    {
        LILA_ZONE();

        const std::string& text = as<std::string>(kExpression);
        if (text != mText || !mParsed)
        {
            mText = text;
            mParsed = true;
            mError.clear();
            try
            {
                mProgram = Program::parse(text);
            }
            catch (const std::exception& e)
            {
                mProgram.reset();
                mError = e.what();
            }
        }
        setOutput(kError, mError);
        double out[kOutputs];
        for (int k = 0; k < kOutputs; ++k)
            out[k] = as<double>(kFirstOutput + k);
        if (mProgram)
        {
            std::vector<double> in;
            for (const auto& [i, v] : elements(kInput))
            {
                if (in.size() <= size_t(i))
                    in.resize(size_t(i) + 1, 0.0);
                in[size_t(i)] = std::holds_alternative<double>(v) ? std::get<double>(v) : 0.0;
            }
            Program::Env env{.inputs = in.data(), .inputCount = in.size(), .outputs = out, .time = ctx.time, .frame = ctx.time * as<double>(kFps)};
            mProgram->run(env);
        }
        for (int k = 0; k < kOutputs; ++k)
            setOutput(kFirstOutput + k, out[k]);
    }

private:
    std::string mText;
    bool mParsed = false;
    std::string mError;
    std::shared_ptr<const Program> mProgram;
};
} // namespace

struct Program::Impl
{
    std::vector<Stmt> body;
    int locals = 0;
};

std::shared_ptr<const Program> Program::parse(std::string_view text)
{
    Parser p(text);
    auto impl = std::make_shared<Impl>();
    impl->body = p.program();
    impl->locals = p.locals();
    auto program = std::make_shared<Program>();
    program->impl = std::move(impl);
    return program;
}

void Program::run(Env& env) const
{
    State st{env, std::vector<double>(size_t(impl->locals), 0.0)};
    exec(impl->body, st);
}

bool valid(std::string_view text, std::string* why)
{
    try
    {
        Program::parse(text);
        return true;
    }
    catch (const std::exception& e)
    {
        if (why)
            *why = e.what();
        return false;
    }
}

void registerTypes()
{
    if (types::find("expression"))
        return;
    NodeType t;
    t.name = "expression";
    t.icon = "math-function";
    t.timeDependent = true;
    t.doc = "Computes its outputs from its inputs and the time with a short script of arithmetic, functions, local variables "
            "and if / else: Maya's expressions, scalar part.";
    t.ownAttrs = {
        {.name = "expression", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable,
         .doc = "The script: statements like out[0] = sin(time) * in[0]; (see the expression node's reference)."},
        {.name = "input", .shortName = "in", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrInput | AttrStorable | AttrKeyable | AttrArray,
         .doc = "Values the script reads as in[0], in[1] ..."},
        {.name = "fps", .type = PlugType::Float, .defaultValue = 24.0, .flags = AttrInput | AttrStorable, .doc = "Frames per second, for frame."},
        {.name = "error", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput,
         .doc = "Why the script doesn't run; empty when it does."},
    };
    for (int k = 0; k < kOutputs; ++k)
        t.ownAttrs.push_back({.name = "output" + std::to_string(k), .shortName = "out" + std::to_string(k), .type = PlugType::Float,
                              .defaultValue = 0.0, .flags = AttrOutput, .doc = "What the script sets as out[" + std::to_string(k) + "]."});
    t.create = [] { return std::make_unique<ExpressionNode>(); };
    types::registerType(std::move(t));
}
} // namespace lila::scene::expression
