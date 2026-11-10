#pragma once

#include "maya/MayaFile.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/** The Maya importer's view of one file: node lookup, hierarchy, connections and attribute reading. Internal to src/maya. */
namespace lila::maya
{
using MNode = maya::Node;
using Names = std::initializer_list<std::string_view>;

inline bool oneOf(std::string_view s, Names names)
{
    return std::find(names.begin(), names.end(), s) != names.end();
}

inline std::string shortName(std::string_view path)
{
    if (auto bar = path.rfind('|'); bar != std::string_view::npos)
        path = path.substr(bar + 1);
    return std::string(path);
}

/** A valid Lila name: namespace segments kept, anything else Lila doesn't allow becomes '_'. */
inline std::string lilaName(std::string_view raw, std::string_view ns, const char* fallback)
{
    std::string s;
    bool segStart = true;
    for (char c : shortName(raw))
    {
        if (c == ':')
        {
            if (!segStart)
                s.push_back(':');
            segStart = true;
            continue;
        }
        bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        bool digit = c >= '0' && c <= '9';
        if (segStart && digit)
            s.push_back('_');
        s.push_back(alpha || digit ? c : '_');
        segStart = false;
    }
    while (!s.empty() && s.back() == ':')
        s.pop_back();
    if (s.empty())
        s = fallback;
    return ns.empty() ? s : std::string(ns) + ":" + s;
}

/** Maya matrices are row-vector, rows written in order; read straight into glm's columns that is the column form. */
inline glm::dmat4 matrixOf(const std::vector<double>& v)
{
    glm::dmat4 m(1.0);
    if (v.size() >= 16)
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                m[c][r] = v[size_t(c * 4 + r)];
    return m;
}

struct Plug
{
    int node = -1;
    std::string attr;
};

/** Node lookup by name or DAG path, hierarchy, and connections of one file. */
class FileIndex
{
public:
    explicit FileIndex(const File& f) : file(f)
    {
        const size_t n = f.nodes.size();
        parent.assign(n, -1);
        children.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            const MNode& node = f.nodes[i];
            if (!node.parent.empty())
            {
                int p = resolve(node.parent, int(i));
                parent[i] = p;
                if (p >= 0)
                    children[size_t(p)].push_back(int(i));
                else
                    unresolvedParents.push_back(node.name);
            }
            byName[shortName(node.name)].push_back(int(i));
        }
        for (const Connection& c : f.connections)
        {
            Plug s = split(c.src), d = split(c.dst);
            if (s.node < 0 || d.node < 0)
                continue;
            size_t k = conns.size();
            conns.push_back({s, d});
            in.emplace(d.node, k);
            out.emplace(s.node, k);
        }
    }

    const File& file;
    std::vector<int> parent;
    std::vector<std::vector<int>> children;
    std::vector<std::string> unresolvedParents;
    struct Conn
    {
        Plug src, dst;
    };
    std::vector<Conn> conns;
    std::multimap<int, size_t> in, out;

    const MNode& node(int i) const { return file.nodes[size_t(i)]; }

    /** "|grp|box|boxShape" */
    std::string fullPath(int i) const
    {
        std::string p;
        for (int cur = i; cur >= 0; cur = parent[size_t(cur)])
            p = "|" + shortName(node(cur).name) + p;
        return p;
    }

    /** Index of a name or DAG path; `before` limits the search to nodes created earlier (-p while reading). */
    int resolve(std::string_view name, int before = -1) const
    {
        while (!name.empty() && name[0] == ':')
            name.remove_prefix(1);
        const bool absolute = !name.empty() && name[0] == '|';
        std::vector<std::string_view> segs;
        for (size_t i = 0; i <= name.size();)
        {
            size_t j = name.find('|', i);
            if (j == std::string_view::npos)
                j = name.size();
            if (j > i)
                segs.push_back(name.substr(i, j - i));
            i = j + 1;
        }
        if (segs.empty())
            return -1;
        auto it = byName.find(std::string(segs.back()));
        // byName is filled as nodes are read, so a lookup while building only sees earlier nodes.
        std::vector<int> cands;
        if (it != byName.end())
            cands = it->second;
        if (before >= 0)
            std::erase_if(cands, [&](int c) { return c >= before; });
        int best = -1;
        for (int c : cands)
        {
            int cur = c;
            bool ok = true;
            for (size_t s = segs.size() - 1; s-- > 0;)
            {
                cur = parent[size_t(cur)];
                if (cur < 0 || shortName(node(cur).name) != segs[s])
                {
                    ok = false;
                    break;
                }
            }
            if (ok && absolute && parent[size_t(cur)] >= 0)
                ok = false;
            if (ok)
                best = c; // the latest match, like Maya's last created node of a name
        }
        return best;
    }

    /** "node.attr.path" to (node, attr). */
    Plug split(std::string_view s) const
    {
        size_t dot = s.find('.');
        if (dot == std::string_view::npos)
            return {};
        return {resolve(s.substr(0, dot)), std::string(s.substr(dot + 1))};
    }

    /** Source of a node's input whose attr is one of names. */
    std::optional<Plug> input(int n, Names names) const
    {
        auto [a, b] = in.equal_range(n);
        for (auto it = a; it != b; ++it)
            if (oneOf(conns[it->second].dst.attr, names))
                return conns[it->second].src;
        return std::nullopt;
    }

    std::vector<std::pair<Plug, Plug>> inputs(int n) const
    {
        std::vector<std::pair<Plug, Plug>> v;
        auto [a, b] = in.equal_range(n);
        for (auto it = a; it != b; ++it)
            v.push_back({conns[it->second].src, conns[it->second].dst});
        return v;
    }

    std::vector<std::pair<Plug, Plug>> outputs(int n) const
    {
        std::vector<std::pair<Plug, Plug>> v;
        auto [a, b] = out.equal_range(n);
        for (auto it = a; it != b; ++it)
            v.push_back({conns[it->second].src, conns[it->second].dst});
        return v;
    }

private:
    std::unordered_map<std::string, std::vector<int>> byName;
};

inline const AttrValue* lastAttr(const MNode& n, Names names)
{
    for (auto it = n.attrs.rbegin(); it != n.attrs.rend(); ++it)
        if (oneOf(it->attr, names))
            return &*it;
    return nullptr;
}

inline double num(const MNode& n, Names names, double def)
{
    const AttrValue* a = lastAttr(n, names);
    return a && !a->nums.empty() ? a->nums[0] : def;
}

inline std::string text(const MNode& n, Names names)
{
    const AttrValue* a = lastAttr(n, names);
    return a && !a->strs.empty() ? a->strs[0] : std::string();
}

/** A double3 attribute with its per-component attributes applied in file order. */
inline glm::dvec3 vec3(const MNode& n, Names whole, std::array<Names, 3> comps, glm::dvec3 def)
{
    glm::dvec3 v = def;
    for (const AttrValue& a : n.attrs)
    {
        if (oneOf(a.attr, whole) && a.nums.size() >= 3)
            v = {a.nums[0], a.nums[1], a.nums[2]};
        else
            for (int i = 0; i < 3; ++i)
                if (oneOf(a.attr, comps[size_t(i)]) && !a.nums.empty())
                    v[i] = a.nums[0];
    }
    return v;
}
} // namespace lila::maya
