#include "gn/NodesCommon.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <regex>

namespace lila::gn::nodes::strings2
{
/** Code point index of a byte offset in UTF-8 text. */
int64_t codePointAt(const std::string& s, size_t byte)
{
    int64_t n = 0;
    for (size_t i = 0; i < byte && i < s.size(); ++i)
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80)
            ++n;
    return n;
}

/** A number formatted by a spec after the colon of a placeholder, like ".2f" or "05"; "d" and friends take it as an integer. */
std::string formatNumber(double v, const std::string& spec)
{
    std::string fmt = "{:" + spec + "}";
    bool integral = !spec.empty() && std::string_view("dxXbBoc").find(spec.back()) != std::string_view::npos;
    if (integral)
    {
        int64_t i = int64_t(std::llround(v));
        return std::vformat(fmt, std::make_format_args(i));
    }
    return std::vformat(fmt, std::make_format_args(v));
}

/** {a} to {d} numbers, {0}, {1}... linked strings, each with an optional :spec; {{ and }} are literal braces. */
std::string formatString(const std::string& format, const std::array<double, 4>& numbers, const std::vector<std::string>& strings)
{
    std::string out;
    for (size_t i = 0; i < format.size(); ++i)
    {
        char ch = format[i];
        if (ch == '}' && i + 1 < format.size() && format[i + 1] == '}')
        {
            out += '}';
            ++i;
            continue;
        }
        if (ch != '{')
        {
            out += ch;
            continue;
        }
        if (i + 1 < format.size() && format[i + 1] == '{')
        {
            out += '{';
            ++i;
            continue;
        }
        size_t close = format.find('}', i);
        if (close == std::string::npos)
            throw ExecError("Format String: a { has no closing }");
        std::string field = format.substr(i + 1, close - i - 1);
        std::string name = field, spec;
        if (size_t colon = field.find(':'); colon != std::string::npos)
        {
            name = field.substr(0, colon);
            spec = field.substr(colon + 1);
        }
        try
        {
            if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'd')
                out += formatNumber(numbers[size_t(name[0] - 'a')], spec);
            else if (!name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c); }))
            {
                size_t k = size_t(std::stoul(name));
                const std::string s = k < strings.size() ? strings[k] : std::string();
                out += spec.empty() ? s : std::vformat("{:" + spec + "}", std::make_format_args(s));
            }
            else
                throw ExecError("Format String: unknown field {" + field + "}; use {a} to {d} or {0}, {1}...");
        }
        catch (const std::format_error& e)
        {
            throw ExecError("Format String: bad format in {" + field + "}: " + e.what());
        }
        i = close;
    }
    return out;
}

/** The number at the start of s (after spaces), and how many characters it took; 0 for both when there is none. */
std::pair<double, int64_t> leadingNumber(const std::string& s, bool integer)
{
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    const char* first = s.data() + a;
    const char* last = s.data() + s.size();
    if (first < last && *first == '+')
        ++first;
    if (integer)
    {
        int64_t v = 0;
        auto [p, ec] = std::from_chars(first, last, v);
        if (ec != std::errc())
            return {0.0, 0};
        return {double(v), codePointAt(s, size_t(p - s.data()))};
    }
    double v = 0.0;
    auto [p, ec] = std::from_chars(first, last, v);
    if (ec != std::errc())
        return {0.0, 0};
    return {v, codePointAt(s, size_t(p - s.data()))};
}

void stringNodes()
{
    registerDecl({
        .type = "FindInString",
        .label = "Find in String",
        .category = "Text",
        .doc = "Where search first appears in the string (a character index, -1 when absent) and how many times it appears without overlapping.",
        .sockets = {.inputs = {in("string", T::String, ""), in("search", T::String, "")}, .outputs = {fout("firstFound", T::Int), fout("count", T::Int)}},
        .exec =
            [](ExecContext& c) {
                const std::string s = c.s("string"), key = c.s("search");
                int64_t first = -1, count = 0;
                if (!key.empty())
                    for (size_t at = s.find(key); at != std::string::npos; at = s.find(key, at + key.size()))
                    {
                        if (count == 0)
                            first = codePointAt(s, at);
                        ++count;
                    }
                c.out("firstFound", constantInt(first));
                c.out("count", constantInt(count));
            },
    });
    registerDecl({
        .type = "FormatString",
        .label = "Format String",
        .category = "Text",
        .doc = "Builds text from a pattern: {a} to {d} are the numbers, {0}, {1}... the linked strings in link order. A spec after a colon "
               "formats the value: {a:.2f} two decimals, {a:04d} a zero-padded integer, {0:>8} right-aligned. {{ and }} are braces.",
        .sockets = {.inputs = {in("format", T::String, "{a}"), in("a", T::Float, 0.0), in("b", T::Float, 0.0), in("c", T::Float, 0.0), in("d", T::Float, 0.0),
                               {.id = "strings", .type = T::String, .multi = true}},
                    .outputs = {out("string", T::String)}},
        .exec =
            [](ExecContext& c) {
                std::vector<std::string> strings;
                for (const Value& v : c.inputs[size_t(c.sockets.inputIndex("strings"))])
                    strings.push_back(v.string);
                c.outString("string", formatString(c.s("format"), {c.f("a"), c.f("b"), c.f("c"), c.f("d")}, strings));
            },
    });
    registerDecl({
        .type = "MatchString",
        .label = "Match String",
        .category = "Text",
        .doc = "Whether the string starts with, ends with or contains the key, or matches it as a regular expression (ECMAScript, anywhere in the string).",
        .props = {prop("operation", {"startsWith", "endsWith", "contains", "regex"}, "contains")},
        .sockets = {.inputs = {in("string", T::String, ""), in("key", T::String, ""), in("caseSensitive", T::Bool, true)}, .outputs = {fout("result", T::Bool)}},
        .exec =
            [](ExecContext& c) {
                std::string s = c.s("string"), key = c.s("key");
                const std::string op = c.prop("operation");
                const bool caseSensitive = c.b("caseSensitive");
                bool r = false;
                if (op == "regex")
                {
                    try
                    {
                        auto flags = std::regex::ECMAScript | (caseSensitive ? std::regex::flag_type(0) : std::regex::icase);
                        r = std::regex_search(s, std::regex(key, flags));
                    }
                    catch (const std::regex_error& e)
                    {
                        throw ExecError("Match String: bad regular expression: " + std::string(e.what()));
                    }
                }
                else
                {
                    if (!caseSensitive)
                        for (std::string* t : {&s, &key})
                            std::transform(t->begin(), t->end(), t->begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
                    if (op == "startsWith")
                        r = s.starts_with(key);
                    else if (op == "endsWith")
                        r = s.ends_with(key);
                    else
                        r = s.find(key) != std::string::npos;
                }
                c.out("result", constantBool(r));
            },
    });
    registerDecl({
        .type = "StringToValue",
        .label = "String to Value",
        .category = "Text",
        .doc = "The number at the start of the string, leading spaces skipped, and how many characters it took (0 when there is no number).",
        .props = {prop("dataType", {"float", "int"}, "float")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                bool isInt = findDecl("StringToValue")->propValue(n, "dataType") == "int";
                return Sockets{{in("string", T::String, "")}, {fout("value", isInt ? T::Int : T::Float), fout("length", T::Int)}};
            },
        .exec =
            [](ExecContext& c) {
                bool isInt = c.prop("dataType") == "int";
                auto [v, len] = leadingNumber(c.s("string"), isInt);
                c.out("value", isInt ? constantInt(int64_t(v)) : constantFloat(v));
                c.out("length", constantInt(len));
            },
    });
}
} // namespace lila::gn::nodes::strings2

namespace lila::gn::nodes
{
void registerStringNodes2() { strings2::stringNodes(); }
} // namespace lila::gn::nodes
