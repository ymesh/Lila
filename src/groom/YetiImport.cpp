// Yeti grooms read from a Maya ASCII file without Maya or Yeti. The format reading and the node mapping follow GroomLab's
// Yeti importer (hosts/maya/scripts/groomlab/importers/yeti.py, yeti_format.py; github.com/thekesslereffect/groomlab,
// Apache-2.0, see src/groom/groomlab/LICENSE and NOTICE).
#include "groom/YetiImport.h"

#include "curve/CurveNodes.h"
#include "curve/NodeHelpers.h"
#include "groom/GroomGraph.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace lila::groom::yeti
{
namespace
{
using json = nlohmann::json;

/** Scraggle's amount and frequency in GroomLab's Noise units (yeti.py's calibration). */
constexpr double kScraggleAmplitude = 0.05;
constexpr double kScraggleFrequency = 4.0;

// ---- Maya ASCII ------------------------------------------------------------------------------------------------------

struct Token
{
    std::string text;
    bool quoted = false;
};

/** The file's statements as tokens; quoted strings joined across `+` as Maya splits long ones. */
std::vector<std::vector<Token>> statements(const std::string& s)
{
    std::vector<std::vector<Token>> out;
    std::vector<Token> cur;
    bool joinNext = false;
    size_t i = 0;
    const size_t n = s.size();
    while (i < n)
    {
        const char c = s[i];
        if (c == '/' && i + 1 < n && s[i + 1] == '/')
        {
            while (i < n && s[i] != '\n')
                ++i;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)))
        {
            ++i;
            continue;
        }
        if (c == ';')
        {
            if (!cur.empty())
                out.push_back(std::move(cur));
            cur.clear();
            ++i;
            continue;
        }
        if (c == '"')
        {
            std::string text;
            ++i;
            while (i < n && s[i] != '"')
            {
                if (s[i] == '\\' && i + 1 < n)
                {
                    const char e = s[i + 1];
                    text += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                    i += 2;
                    continue;
                }
                text += s[i++];
            }
            ++i;
            if (!cur.empty() && cur.back().quoted && joinNext)
                cur.back().text += text;
            else
                cur.push_back({std::move(text), true});
            joinNext = false;
            continue;
        }
        size_t j = i;
        while (j < n && !std::isspace(static_cast<unsigned char>(s[j])) && s[j] != ';' && s[j] != '"')
            ++j;
        std::string word = s.substr(i, j - i);
        i = j;
        if (word == "+" && !cur.empty() && cur.back().quoted)
        {
            joinNext = true;
            continue;
        }
        cur.push_back({std::move(word), false});
    }
    if (!cur.empty())
        out.push_back(std::move(cur));
    return out;
}

std::string flag(const std::vector<Token>& st, const char* name)
{
    for (size_t k = 0; k + 1 < st.size(); ++k)
        if (!st[k].quoted && st[k].text == name)
            return st[k + 1].text;
    return {};
}

struct MaNode
{
    std::string type, name, parent;
    /** setAttr values by attribute (".pds" -> its text). */
    std::map<std::string, std::string> attrs;
};

struct MaScene
{
    std::map<std::string, MaNode> nodes;
    std::vector<std::pair<std::string, std::string>> connections;
};

MaScene readMa(const std::string& text)
{
    MaScene out;
    MaNode* current = nullptr;
    for (const std::vector<Token>& st : statements(text))
    {
        const std::string& head = st[0].text;
        if (head == "createNode" && st.size() > 1)
        {
            MaNode n{st[1].text, flag(st, "-n"), flag(st, "-p"), {}};
            const std::string key = out.nodes.count(n.name) ? n.parent + "|" + n.name : n.name;
            current = &(out.nodes[key] = std::move(n));
        }
        else if (head == "setAttr" && current)
        {
            size_t k = 1;
            while (k < st.size() && !(st[k].quoted && !st[k].text.empty() && st[k].text[0] == '.'))
                ++k;
            if (k >= st.size())
                continue;
            const std::string attr = st[k].text;
            size_t v = k + 1;
            if (v + 1 < st.size() && !st[v].quoted && st[v].text == "-type")
                v += 2;
            // A string value is quoted; Yeti's strand data is bare tokens to the end of the statement.
            std::string value;
            bool found = false;
            for (size_t w = v; w < st.size() && !found; ++w)
                if (st[w].quoted)
                {
                    value = st[w].text;
                    found = true;
                }
            if (!found)
                for (size_t w = v; w < st.size(); ++w)
                    value += (value.empty() ? "" : " ") + st[w].text;
            if (!value.empty())
                current->attrs[attr] = std::move(value);
        }
        else if (head == "connectAttr" && st.size() >= 3)
            out.connections.emplace_back(st[1].text, st[2].text);
        else if (head != "setAttr")
            current = nullptr;
    }
    return out;
}

std::string nodeOf(const std::string& plug)
{
    return plug.substr(0, plug.find('.'));
}

std::string attrOf(const std::string& plug)
{
    const size_t dot = plug.find('.');
    return dot == std::string::npos ? std::string() : plug.substr(dot + 1);
}

// ---- base64 and a small XML reader -----------------------------------------------------------------------------------

std::string base64(const std::string& in)
{
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in)
    {
        if (std::isspace(c) || c == '=')
            continue;
        const size_t p = chars.find(char(c));
        if (p == std::string::npos)
            throw std::runtime_error("the Yeti graph data is not base64");
        val = (val << 6) + int(p);
        bits += 6;
        if (bits >= 0)
        {
            out.push_back(char((val >> bits) & 0xff));
            bits -= 8;
        }
    }
    return out;
}

struct Xml
{
    std::string tag;
    std::map<std::string, std::string> attrs;
    std::vector<Xml> children;
    std::string text;
};

std::string unescape(std::string s)
{
    const std::pair<const char*, const char*> entities[] = {{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
    for (const auto& [from, to] : entities)
        for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + std::strlen(to)))
            s.replace(p, std::strlen(from), to);
    return s;
}

/** Parses the element starting at s[i] ('<'); i ends past it. */
Xml element(const std::string& s, size_t& i)
{
    Xml e;
    size_t j = i + 1;
    while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j])) && s[j] != '>' && s[j] != '/')
        ++j;
    e.tag = s.substr(i + 1, j - i - 1);
    i = j;
    for (;;)
    {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        if (i >= s.size())
            throw std::runtime_error("the Yeti graph XML is cut short");
        if (s[i] == '/')
        {
            i = s.find('>', i) + 1;
            return e;
        }
        if (s[i] == '>')
        {
            ++i;
            break;
        }
        const size_t eq = s.find('=', i);
        const std::string name = s.substr(i, eq - i);
        const char q = s[eq + 1];
        const size_t end = s.find(q, eq + 2);
        e.attrs[name] = unescape(s.substr(eq + 2, end - eq - 2));
        i = end + 1;
    }
    for (;;)
    {
        const size_t lt = s.find('<', i);
        if (lt == std::string::npos)
            throw std::runtime_error("the Yeti graph XML is cut short");
        e.text += s.substr(i, lt - i);
        i = lt;
        if (s.compare(i, 4, "<!--") == 0)
        {
            i = s.find("-->", i) + 3;
            continue;
        }
        if (s.compare(i, 2, "</") == 0)
        {
            i = s.find('>', i) + 1;
            return e;
        }
        e.children.push_back(element(s, i));
    }
}

Xml parseXml(const std::string& s)
{
    size_t i = s.find('<');
    while (i != std::string::npos && (s.compare(i, 2, "<?") == 0 || s.compare(i, 4, "<!--") == 0))
        i = s.find('<', s.find('>', i));
    if (i == std::string::npos)
        throw std::runtime_error("the Yeti graph data holds no XML");
    return element(s, i);
}

// ---- Yeti's graph and grooms -----------------------------------------------------------------------------------------

struct YNode
{
    std::string id, name, type;
    std::map<std::string, std::string> params;
    std::map<std::string, std::string> attached;
    /** Each input's feeding output id ("" none), in order. */
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
};

struct YGraph
{
    std::vector<YNode> nodes;
    std::string root;

    const YNode* byId(const std::string& id) const
    {
        for (const YNode& n : nodes)
            if (n.id == id)
                return &n;
        return nullptr;
    }

    /** The node whose output feeds input `k` of n. */
    const YNode* input(const YNode& n, size_t k) const
    {
        if (k >= n.inputs.size() || n.inputs[k].empty() || n.inputs[k] == "-")
            return nullptr;
        for (const YNode& m : nodes)
            if (std::find(m.outputs.begin(), m.outputs.end(), n.inputs[k]) != m.outputs.end() || m.id == n.inputs[k])
                return &m;
        return nullptr;
    }
};

void collectNodes(const Xml& e, YGraph& g)
{
    for (const Xml& c : e.children)
    {
        if (c.tag == "node")
        {
            YNode n;
            n.id = c.attrs.count("id") ? c.attrs.at("id") : std::string();
            n.name = c.attrs.count("name") ? c.attrs.at("name") : std::string();
            n.type = c.attrs.count("type") ? c.attrs.at("type") : std::string();
            std::transform(n.type.begin(), n.type.end(), n.type.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
            for (const Xml& part : c.children)
            {
                if (part.tag == "param" && part.attrs.count("name"))
                {
                    n.params[part.attrs.at("name")] = part.attrs.count("value") ? part.attrs.at("value") : std::string();
                    if (part.attrs.count("attachment") && !part.attrs.at("attachment").empty())
                        n.attached[part.attrs.at("name")] = part.attrs.at("attachment");
                }
                else if (part.tag == "inputs")
                    for (const Xml& r : part.children)
                        n.inputs.push_back(r.attrs.count("value") ? r.attrs.at("value") : std::string());
                else if (part.tag == "outputs")
                    for (const Xml& r : part.children)
                        if (r.attrs.count("id"))
                            n.outputs.push_back(r.attrs.at("id"));
            }
            g.nodes.push_back(std::move(n));
        }
        else if (c.tag == "group" || c.tag == "items")
            collectNodes(c, g);
    }
}

void findRoot(const Xml& e, YGraph& g)
{
    for (const Xml& c : e.children)
    {
        if (c.tag == "metadata" && c.attrs.count("name") && c.attrs.at("name") == "root")
            g.root = c.attrs.count("value") ? c.attrs.at("value") : std::string();
        findRoot(c, g);
    }
}

YGraph parseGraph(const std::string& pds)
{
    const Xml root = parseXml(base64(pds));
    YGraph g;
    collectNodes(root, g);
    findRoot(root, g);
    return g;
}

/** A Yeti groom's strands: points per strand and every painted per-strand attribute. */
struct YGroom
{
    curve::Splines strands;
    std::map<std::string, std::vector<double>> attributes;
};

YGroom parseGroom(const std::string& pds)
{
    std::istringstream in(pds);
    std::string magic, name;
    size_t count = 0;
    in >> magic >> name >> count;
    if (magic.rfind("PGYS", 0) != 0)
        throw std::runtime_error("not Yeti strand data");
    std::map<std::string, std::vector<double>> raw;
    std::map<std::string, int> widths;
    for (size_t a = 0; a < count && in; ++a)
    {
        std::string attr;
        int type = 0, flagValue = 0;
        size_t n = 0;
        in >> attr >> type >> flagValue >> n;
        const int width = type == 2 ? 3 : 1;
        std::vector<double>& v = raw[attr];
        v.resize(n * size_t(width));
        for (double& x : v)
            in >> x;
        widths[attr] = width;
    }
    if (!in && !in.eof())
        throw std::runtime_error("the Yeti strand data is cut short");
    YGroom out;
    const std::vector<double>& p = raw["P"];
    const std::vector<double>& counts = raw["numVertices"];
    size_t at = 0;
    for (double c : counts)
    {
        curve::Spline s;
        s.kind = curve::Kind::Poly;
        for (size_t k = 0; k < size_t(c) && (at + 1) * 3 <= p.size(); ++k, ++at)
            s.points.emplace_back(p[at * 3], p[at * 3 + 1], p[at * 3 + 2]);
        out.strands.push_back(std::move(s));
    }
    for (const auto& [attr, values] : raw)
        if (widths[attr] == 1 && values.size() == counts.size() && attr != "numVertices" && attr != "s" && attr != "t" && attr != "id")
            out.attributes[attr] = values;
    return out;
}

double num(const YNode& n, const char* name, double fallback)
{
    const auto it = n.params.find(name);
    if (it == n.params.end())
        return fallback;
    try
    {
        size_t used = 0;
        const double v = std::stod(it->second, &used);
        return used ? v : fallback;
    }
    catch (...)
    {
        return fallback;
    }
}

json linearRamp(const std::vector<std::pair<double, double>>& keys)
{
    json k = json::array();
    for (const auto& [t, v] : keys)
        k.push_back({t, v});
    return {{"keys", k}, {"linear", true}};
}

/** The import's account of a groom: what became what. */
struct Report
{
    json mapped = json::array(), approximated = json::array(), skipped = json::array();
};

/** The fibre chain ending at the root: from the root back along each node's first input to the Scatter. */
std::vector<const YNode*> chainOf(const YGraph& g)
{
    std::vector<const YNode*> chain;
    const YNode* n = g.byId(g.root);
    if (!n && !g.nodes.empty())
        n = &g.nodes.back();
    for (int guard = 0; n && guard < 256; ++guard)
    {
        chain.push_back(n);
        if (n->type == "scatter")
            break;
        n = g.input(*n, 0);
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
}

/** The default groom graph with the chain's Scatter and look nodes: Clump, Noise and Width after Interpolate. */
json graphFor(const std::vector<const YNode*>& chain, double guideLength, Report& r)
{
    json g = json::parse(groomGraph::defaultGraph());
    json* scatter = nullptr;
    for (json& n : g["nodes"])
        if (n["name"] == "scatter")
            scatter = &n;
    std::vector<json> ops;
    for (const YNode* n : chain)
    {
        const std::string what = n->name + " (" + n->type + ")";
        if (n->type == "scatter")
        {
            (*scatter)["params"]["density"] = num(*n, "density", 20.0) * num(*n, "densityMultiplier", 1.0);
            (*scatter)["params"]["seed"] = int64_t(num(*n, "seed", 0.0));
            r.mapped.push_back(what + ": Scatter density x multiplier, seed");
            if (n->attached.count("density"))
                r.skipped.push_back(what + ": the density's map");
        }
        else if (n->type == "grow" || n->type == "comb" || n->type == "guide")
            r.mapped.push_back(what + ": Interpolate on the groom's strands as guides");
        else if (n->type == "clumping")
        {
            const double base = num(*n, "attractionBase", 1.0), tip = num(*n, "attractionTip", 1.0), bias = num(*n, "attractionBias", 0.0);
            const double exponent = std::pow(2.0, -2.0 * std::clamp(bias, -1.0, 1.0));
            std::vector<std::pair<double, double>> keys;
            for (int i = 0; i < 6; ++i)
            {
                const double t = i / 5.0;
                keys.emplace_back(t, base + (tip - base) * std::pow(t, exponent));
            }
            json params = {{"strength", std::clamp(num(*n, "weight", 1.0), -1.0, 1.0)}, {"profile", linearRamp(keys)}};
            if (num(*n, "clumpWeightRandom", 0.0) > 0.0)
                params["strengthRandom"] = std::clamp(num(*n, "clumpWeightRandom", 0.0), 0.0, 1.0);
            if (num(*n, "weightRandom", 0.0) > 0.0)
                params["strandStrengthRandom"] = std::clamp(num(*n, "weightRandom", 0.0), 0.0, 1.0);
            ops.push_back({{"type", "Clump"}, {"params", params}});
            r.mapped.push_back(what + ": Clump, weight as strength, attraction base, tip and bias as the profile");
            r.approximated.push_back(what + ": clumps picked among the strands, not Yeti's clump curves");
        }
        else if (n->type == "scraggle")
        {
            const double amount = num(*n, "scraggle", 1.0) * num(*n, "scraggleMultiplier", 1.0);
            json params = {{"space", "strand"},
                           {"octaves", 2},
                           {"frequency", std::max(0.01, num(*n, "scraggleFrequency", 1.0)) * kScraggleFrequency},
                           {"amplitude", amount * kScraggleAmplitude * guideLength},
                           {"profile", linearRamp({{0.0, num(*n, "rootScraggle", 0.0)}, {1.0, num(*n, "tipScraggle", 1.0)}})},
                           {"seed", int64_t(ops.size() + 1)}};
            if (num(*n, "percent", 1.0) < 1.0)
                params["fraction"] = std::max(0.0, num(*n, "percent", 1.0));
            ops.push_back({{"type", "Noise"}, {"params", params}});
            r.approximated.push_back(what + ": Noise, scraggle x multiplier as a share of the guides' length");
        }
        else if (n->type == "width")
        {
            const double w0 = num(*n, "baseWidth", 0.01), w1 = num(*n, "tipWidth", 0.01);
            const double p0 = num(*n, "baseParameter", 0.0), p1 = std::clamp(num(*n, "tipParameter", 1.0), p0, 1.0);
            const double tipShare = w0 > 0.0 ? w1 / w0 : 1.0;
            std::vector<std::pair<double, double>> keys = {{0.0, 1.0}};
            if (p0 > 0.0)
                keys.emplace_back(p0, 1.0);
            keys.emplace_back(p1, tipShare);
            if (p1 < 1.0)
                keys.emplace_back(1.0, tipShare);
            json params = {{"width", w0 * num(*n, "widthMultiplier", 1.0)}, {"profile", linearRamp(keys)}};
            if (num(*n, "widthRandom", 0.0) > 0.0)
                params["randomness"] = std::clamp(num(*n, "widthRandom", 0.0), 0.0, 1.0);
            ops.push_back({{"type", "Width"}, {"params", params}});
            r.mapped.push_back(what + ": Width, base and tip widths as the profile");
        }
        else if (n->type == "attribute" || n->type == "import" || n->type == "convert")
            continue;
        else
            r.skipped.push_back(what + ": no GroomLab counterpart taken over");
    }
    // hair -> op1 -> ... -> output.
    std::string from = "hair";
    json& connections = g["connections"];
    connections.erase(std::remove_if(connections.begin(), connections.end(), [](const json& c) { return c["to"] == "output"; }), connections.end());
    for (size_t k = 0; k < ops.size(); ++k)
    {
        const std::string name = "yeti" + std::to_string(k + 1);
        ops[k]["name"] = name;
        g["nodes"].push_back(ops[k]);
        connections.push_back({{"from", from}, {"to", name}, {"input", "strands"}});
        from = name;
    }
    connections.push_back({{"from", from}, {"to", "output"}, {"input", "strands"}});
    return g;
}

double medianLength(const curve::Splines& strands)
{
    std::vector<double> lengths;
    for (const curve::Spline& s : strands)
    {
        double l = 0.0;
        for (size_t k = 1; k < s.points.size(); ++k)
            l += glm::distance(s.points[k - 1], s.points[k]);
        if (l > 0.0)
            lengths.push_back(l);
    }
    if (lengths.empty())
        return 1.0;
    std::nth_element(lengths.begin(), lengths.begin() + ptrdiff_t(lengths.size() / 2), lengths.end());
    return lengths[lengths.size() / 2];
}
} // namespace

std::vector<Description> describe(const std::string& maText)
{
    const MaScene scene = readMa(maText);
    std::vector<Description> out;
    for (const auto& [key, n] : scene.nodes)
    {
        if (n.type != "pgYetiMaya")
            continue;
        Description d;
        d.node = n.name;
        d.transform = n.parent;
        for (const auto& [src, dst] : scene.connections)
        {
            if (nodeOf(dst) != n.name)
                continue;
            const std::string attr = attrOf(dst);
            const auto it = scene.nodes.find(nodeOf(src));
            if (attr.rfind("ing", 0) == 0 && it != scene.nodes.end())
                d.scalp = it->second.parent.empty() ? it->second.name : it->second.parent;
            if (attr.rfind("icd", 0) == 0 && it != scene.nodes.end() && it->second.type == "pgYetiGroom" && !it->second.attrs.count(".cgt"))
                d.grooms.push_back(it->second.name);
        }
        out.push_back(std::move(d));
    }
    return out;
}

Imported readDescription(const std::string& maText, const std::string& yetiNode)
{
    const MaScene scene = readMa(maText);
    const auto it = std::find_if(scene.nodes.begin(), scene.nodes.end(), [&](const auto& kv) {
        return kv.second.type == "pgYetiMaya" && (kv.second.name == yetiNode || kv.second.parent == yetiNode);
    });
    if (it == scene.nodes.end())
        throw std::runtime_error("no Yeti node " + yetiNode);
    const auto pds = it->second.attrs.find(".pds");
    if (pds == it->second.attrs.end())
        throw std::runtime_error(yetiNode + " holds no graph (saved without Yeti as binary?)");
    const YGraph graph = parseGraph(pds->second);
    Imported out;
    for (const Description& d : describe(maText))
        if (d.node == it->second.name)
            out.description = d;
    for (const std::string& groom : out.description.grooms)
    {
        const auto g = scene.nodes.find(groom);
        if (g == scene.nodes.end() || !g->second.attrs.count(".pds"))
            continue;
        YGroom yg = parseGroom(g->second.attrs.at(".pds"));
        for (curve::Spline& s : yg.strands)
            if (s.points.size() >= 2)
                out.guides.push_back(std::move(s));
    }
    Report r;
    if (out.guides.empty())
        r.approximated.push_back("no groom feeds " + yetiNode + ": the strands stand straight from the scalp");
    else
        r.mapped.push_back(std::to_string(out.guides.size()) + " groom strands as guides");
    const std::vector<const YNode*> chain = chainOf(graph);
    out.graph = graphFor(chain, medianLength(out.guides), r).dump();
    out.report = {{"mapped", r.mapped}, {"approximated", r.approximated}, {"skipped", r.skipped}};
    return out;
}

void registerCommands()
{
    ops::registerCommand({
        .name = "importYeti",
        .doc = "Reads the Yeti grooms of a Maya ASCII (.ma) file into Lila grooms, without Maya or Yeti: each pgYetiMaya node's "
               "groom strands become guides, its Scatter, Clumping, Scraggle and Width nodes the groom graph's Scatter, Clump, "
               "Noise and Width. The scalp is the mesh given, else the scene node named as the Yeti node's mesh in the file "
               "(import the .ma first). Returns [{yeti, groom, guides, report: {mapped, approximated, skipped}}].",
        .positionalDoc = "file",
        .flags = {{"mesh", "m", ArgType::String, nullptr, "The scalp mesh for every groom."},
                  {"nodes", "", ArgType::List, nullptr, "The pgYetiMaya nodes to read; every one when left out."}},
        .run = [](const Args& a) -> json {
            if (a.positional.empty())
                throw CommandError("importYeti: give a .ma file");
            const std::string path = a.positional[0].get<std::string>();
            std::ifstream in(path, std::ios::binary);
            if (!in)
                throw CommandError("importYeti: can't read " + path);
            std::stringstream ss;
            ss << in.rdbuf();
            const std::string text = ss.str();
            scene::Graph& g = scene::Graph::current();
            json out = json::array();
            std::vector<std::string> wanted;
            if (a.has("nodes"))
                for (const json& n : a.flags.at("nodes"))
                    wanted.push_back(n.get<std::string>());
            for (const Description& d : describe(text))
            {
                if (!wanted.empty() && std::find(wanted.begin(), wanted.end(), d.node) == wanted.end() &&
                    std::find(wanted.begin(), wanted.end(), d.transform) == wanted.end())
                    continue;
                Imported im;
                try
                {
                    im = readDescription(text, d.node);
                }
                catch (const std::exception& e)
                {
                    throw CommandError("importYeti: " + d.node + ": " + e.what());
                }
                const std::string meshName = a.has("mesh") ? a.get<std::string>("mesh") : d.scalp;
                scene::Node* mesh = meshName.empty() ? nullptr : g.find(meshName);
                if (!mesh || !mesh->type().isA("mesh"))
                    throw CommandError("importYeti: no mesh " + (meshName.empty() ? std::string("for ") + d.node : meshName) +
                                            " in the scene; import the .ma first or give mesh");
                const std::string base = d.transform.empty() ? d.node : d.transform;
                json call = {{"args", {mesh->path()}}, {"strands", true}, {"name", base}, {"symmetry", "off"}};
                if (!im.guides.empty())
                {
                    scene::Node* curve = curve::helpers::createCurve(g, im.guides, base + "YetiGuides");
                    call["guides"] = curve->path();
                }
                const json made = ops::execute("createGroom", call);
                scene::Node* gr = g.find(made.value("groom", std::string()));
                if (!gr)
                    throw CommandError("importYeti: the groom for " + d.node + " was not made");
                g.setValue({gr->id(), gr->type().attrIndex("graph"), -1}, im.graph);
                out.push_back({{"yeti", d.node}, {"groom", gr->path()}, {"guides", im.guides.size()}, {"report", im.report}});
            }
            if (out.empty())
                throw CommandError("importYeti: no Yeti node in " + path);
            return out;
        },
    });
}
} // namespace lila::groom::yeti
