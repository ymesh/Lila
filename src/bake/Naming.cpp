#include "bake/Naming.h"

#include "core/ImageIO.h"

#include <algorithm>

namespace lila::bake
{
const std::vector<std::string>& nameTokens()
{
    static const std::vector<std::string> t = {"project", "group", "mesh", "material", "map", "udim", "res", "ext", "variant"};
    return t;
}

std::string fileSafe(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        if (c == ':' || c == '|' || c == '/' || c == '\\' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == ' ')
            c = '_';
    return out;
}

namespace
{
/** A token body split into its key and the [!text] exclusions written before it: "[!M_]material". */
struct TokenSpec
{
    std::string_view key;
    std::vector<std::string_view> excludes;
};

TokenSpec parseToken(std::string_view body)
{
    TokenSpec spec;
    while (body.starts_with("[!"))
    {
        const size_t close = body.find(']');
        if (close == std::string_view::npos)
            break;
        spec.excludes.push_back(body.substr(2, close - 2));
        body.remove_prefix(close + 1);
    }
    spec.key = body;
    return spec;
}

std::string exclude(std::string v, const std::vector<std::string_view>& excludes)
{
    for (std::string_view x : excludes)
    {
        if (x.empty())
            continue;
        if (v.starts_with(x))
            v.erase(0, x.size());
        else if (v.ends_with(x))
            v.erase(v.size() - x.size());
    }
    return v;
}

/** Calls f(start, end, spec) for each {token} in tmpl, end being one past the closing brace. */
template <typename F> void forEachToken(std::string_view tmpl, F&& f)
{
    for (size_t i = tmpl.find('{'); i != std::string_view::npos; i = tmpl.find('{', i + 1))
    {
        const size_t close = tmpl.find('}', i);
        if (close == std::string_view::npos)
            break;
        f(i, close + 1, parseToken(tmpl.substr(i + 1, close - i - 1)));
        i = close;
    }
}
} // namespace

bool hasToken(std::string_view tmpl, std::string_view token)
{
    bool found = false;
    forEachToken(tmpl, [&](size_t, size_t, const TokenSpec& s) { found |= s.key == token; });
    return found;
}

std::string expandName(std::string_view tmpl, const NameTokens& t, bool forceUdim)
{
    const std::string res = t.width == t.height ? std::to_string(t.width) : std::to_string(t.width) + "x" + std::to_string(t.height);
    auto value = [&](std::string_view key, bool& known) -> std::string {
        known = true;
        if (key == "project")
            return fileSafe(t.project);
        if (key == "group")
            return fileSafe(t.group);
        if (key == "mesh")
            return fileSafe(t.mesh);
        if (key == "material")
            return fileSafe(t.material);
        if (key == "map")
            return fileSafe(t.map);
        if (key == "udim")
            return image::udimLabel(t.udim, t.udimStyle);
        if (key == "res")
            return res;
        if (key == "ext")
            return t.ext;
        if (key == "variant")
            return fileSafe(t.variant.empty() ? std::string("base") : t.variant);
        known = false;
        return {};
    };
    std::string out;
    size_t at = 0;
    forEachToken(tmpl, [&](size_t start, size_t end, const TokenSpec& s) {
        out += tmpl.substr(at, start - at);
        bool known = false;
        const std::string v = value(s.key, known);
        out += known ? fileSafe(exclude(v, s.excludes)) : std::string(tmpl.substr(start, end - start));
        at = end;
    });
    out += tmpl.substr(at);
    if (!hasToken(tmpl, "ext"))
    {
        if (forceUdim && !hasToken(tmpl, "udim"))
            out += "." + image::udimLabel(t.udim, t.udimStyle);
        out += "." + t.ext;
    }
    else if (forceUdim && !hasToken(tmpl, "udim"))
    {
        const size_t dot = out.rfind('.');
        out.insert(dot == std::string::npos ? out.size() : dot, "." + image::udimLabel(t.udim, t.udimStyle));
    }
    return out;
}

std::vector<std::string> unknownTokens(std::string_view tmpl)
{
    std::vector<std::string> out;
    forEachToken(tmpl, [&](size_t, size_t, const TokenSpec& s) {
        if (std::ranges::find(nameTokens(), s.key) == nameTokens().end())
            out.push_back(std::string(s.key));
    });
    return out;
}
} // namespace lila::bake
