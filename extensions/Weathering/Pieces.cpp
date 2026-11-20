#include "Pieces.h"
#include "Placeholders.h"

#include <algorithm>

using namespace lila;

namespace weathering
{
namespace
{
void meshesBelow(Graph& g, NodeId id, std::vector<NodeId>& out)
{
    for (NodeId c : g.children(id))
    {
        if (const scene::Node* n = g.node(c); n && n->type().isA("mesh"))
            out.push_back(c);
        meshesBelow(g, c, out);
    }
}

void addLibraryAsset(std::vector<Piece>& out, const std::string& id, double weight, bool placeholder, Graph& g, const char* cmd)
{
    const json m = ops::execute("libraryMaster", {{"args", {id}}});
    const scene::Node* root = g.find(m.value("root", std::string()));
    const json meshes = m.value("meshes", json::array());
    const scene::Node* mesh = meshes.empty() ? nullptr : g.find(meshes[0].get<std::string>());
    if (!root || !mesh)
        throw lila::CommandError(std::string(cmd) + ": library asset '" + id + "' has no mesh");
    out.push_back({mesh->id(), root->id(), id, weight, placeholder});
}

json searchTags(const std::vector<std::string>& tags)
{
    try
    {
        return ops::execute("librarySearch", {{"tags", tags}});
    }
    catch (const std::exception&)
    {
        return json::array();
    }
}

void addEntry(std::vector<Piece>& out, Graph& g, const json& entry, double weight, const char* cmd)
{
    if (entry.is_object())
    {
        if (!entry.contains("piece"))
            throw lila::CommandError(std::string(cmd) + ": a weighted piece is {\"piece\": name, \"weight\": w}");
        addEntry(out, g, entry["piece"], entry.value("weight", 1.0), cmd);
        return;
    }
    if (entry.is_array())
    {
        for (const json& e : entry)
            addEntry(out, g, e, weight, cmd);
        return;
    }
    if (!entry.is_string())
        throw lila::CommandError(std::string(cmd) + ": pieces are names, library ids or tag:<word>");
    const std::string s = entry.get<std::string>();
    if (s.rfind("tag:", 0) == 0)
    {
        const json found = searchTags({s.substr(4)});
        if (found.empty())
            throw lila::CommandError(std::string(cmd) + ": no library asset is tagged '" + s.substr(4) + "'");
        for (const json& x : found)
            addLibraryAsset(out, x["id"].get<std::string>(), weight, x.value("tier", std::string()) == "placeholder", g, cmd);
        return;
    }
    if (scene::Node* n = g.find(s))
    {
        if (n->type().isA("objectSet"))
        {
            for (const json& x : ops::execute("setMembers", {{"args", {n->path()}}, {"nodes", true}}))
                addEntry(out, g, x, weight, cmd);
            return;
        }
        if (n->type().isA("mesh"))
        {
            out.push_back({n->id(), n->id(), s, weight, false});
            return;
        }
        std::vector<NodeId> meshes;
        meshesBelow(g, n->id(), meshes);
        if (meshes.empty())
            throw lila::CommandError(std::string(cmd) + ": '" + s + "' has no mesh");
        for (NodeId m : meshes)
            out.push_back({m, m, g.node(m)->path(), weight, false});
        return;
    }
    addLibraryAsset(out, s, weight, false, g, cmd);
}

struct RoleChoice
{
    std::vector<std::string> ids;
    bool placeholder = false;
};

RoleChoice chooseRole(const std::string& role, Style style)
{
    const json found = searchTags({"weathering", role});
    const std::string want = styleName(style);
    RoleChoice user, placeholder, userStyled, placeholderStyled;
    placeholder.placeholder = placeholderStyled.placeholder = true;
    for (const json& x : found)
    {
        const bool ph = x.value("tier", std::string()) == "placeholder";
        const json tags = x.value("tags", json::array());
        const bool styled = std::ranges::find(tags, json(want)) != tags.end();
        const std::string id = x["id"].get<std::string>();
        (ph ? placeholder : user).ids.push_back(id);
        if (styled)
            (ph ? placeholderStyled : userStyled).ids.push_back(id);
    }
    if (!user.ids.empty())
        return userStyled.ids.empty() ? user : userStyled;
    return placeholderStyled.ids.empty() ? placeholder : placeholderStyled;
}
} // namespace

std::vector<Piece> resolvePieces(Graph& g, const json& value, const char* cmd)
{
    std::vector<Piece> out;
    addEntry(out, g, value, 1.0, cmd);
    if (out.empty())
        throw lila::CommandError(std::string(cmd) + ": no pieces given");
    return out;
}

std::vector<std::string> roleAssets(const std::string& role, Style style) { return chooseRole(role, style).ids; }

std::vector<Piece> piecesOrDefault(Graph& g, const lila::Args& a, const char* flag, const std::string& role, Style style, const char* cmd)
{
    if (a.has(flag))
        return resolvePieces(g, a.flags[std::string(flag)], cmd);
    RoleChoice c = chooseRole(role, style);
    // Placeholders are written on first need, so a fresh install's presets work without a setup step.
    if (c.ids.empty())
    {
        buildPlaceholders(false);
        c = chooseRole(role, style);
    }
    if (c.ids.empty())
        throw lila::CommandError(std::string(cmd) + ": no " + role + " pieces: give " + flag +
                                "=[...] (meshes, groups, sets or library ids), tag library assets 'weathering' and '" + role +
                                "', or run weatheringPlaceholders()");
    std::vector<Piece> out;
    for (const std::string& id : c.ids)
        addLibraryAsset(out, id, 1.0, c.placeholder, g, cmd);
    return out;
}

size_t pickPiece(std::span<const Piece> pieces, double u)
{
    double total = 0.0;
    for (const Piece& p : pieces)
        total += std::max(0.0, p.weight);
    if (pieces.empty() || total <= 0.0)
        return 0;
    double t = std::clamp(u, 0.0, 0.999999) * total;
    for (size_t k = 0; k < pieces.size(); ++k)
    {
        t -= std::max(0.0, pieces[k].weight);
        if (t < 0.0)
            return k;
    }
    return pieces.size() - 1;
}

geo::Mesh pieceMesh(Graph& g, const Piece& p)
{
    scene::Node* n = g.node(p.mesh);
    if (!n)
        return {};
    const auto m = n->as<scene::MeshPtr>(n->type().attrIndex("outMesh"));
    return m ? *m : geo::Mesh();
}

lila::FlagDef styleFlag()
{
    return {"style", "sty", lila::ArgType::String, "realistic",
            "realistic or toon: picks the matching placeholder pieces and the look of anything generated (noise, curves, edges).",
            {"realistic", "toon"}};
}

Style styleArg(const lila::Args& a) { return a.get<std::string>("style", "realistic") == "toon" ? Style::Toon : Style::Realistic; }

const char* styleName(Style s) { return s == Style::Toon ? "toon" : "realistic"; }

lila::FlagDef piecesFlag(const char* name, const char* shortName, const char* doc) { return {name, shortName, lila::ArgType::Any, nullptr, doc}; }

double unitHash(uint32_t seed, uint32_t a, uint32_t b)
{
    uint32_t h = seed * 0x9E3779B9u ^ (a + 0x7F4A7C15u) * 0x85EBCA6Bu ^ (b + 0x165667B1u) * 0xC2B2AE35u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return double(h) / 4294967296.0;
}
} // namespace weathering
