// Edge decals and edge chipping: the live edgeDecalStrips node and edgeChip modifier, their commands, the placeholder
// edge pieces and sheets, and the Level > Weathering menu rows.

#include "EdgeGeometry.h"
#include "EdgeSheets.h"
#include "Pieces.h"
#include "Placeholders.h"
#include "Weathering.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/LevelToolkit.h>
#include <lila/Log.h>
#include <lila/Menus.h>
#include <lila/MeshOps.h>
#include <lila/OptionBox.h>
#include <lila/Profile.h>
#include <lila/Relocate.h>
#include <lila/Transform.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

namespace weathering
{
using namespace lila;
using namespace lila::scene;
using T = ArgType;

namespace edgesui
{
constexpr const char* kDecalType = "edgeDecalStrips";
constexpr const char* kChipType = "edgeChipModifier";
constexpr const char* kChipKind = "edgeChip";
/** String attr on an edge decal mesh: the sheet its strips map, so trimDecalSend sends it as a mesh decal. */
constexpr const char* kMeshDecalAttr = "trimDecalMesh";

int attrOf(const Node& n, std::string_view name) { return n.type().attrIndex(name); }

AttrDef param(std::string name, PlugType type, PlugValue def, std::string doc, std::optional<double> lo = {}, std::optional<double> hi = {})
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc), .minValue = lo, .maxValue = hi};
}

AttrDef enumParam(std::string name, int64_t def, std::vector<std::string> names, std::string doc)
{
    AttrDef a = param(std::move(name), PlugType::Enum, def, std::move(doc));
    a.enumNames = std::move(names);
    return a;
}

unsigned kindsOf(int64_t edges) { return edges == 0 ? edges::kConvex : edges == 1 ? edges::kConcave : edges::kConvex | edges::kConcave; }

geo::Mesh toWorld(const geo::Mesh& m, const glm::dmat4& w)
{
    geo::Mesh out = m;
    for (geo::Index p = 0; p < out.pointCapacity(); ++p)
        if (out.pointValid(p))
            out.setPosition(p, glm::dvec3(w * glm::dvec4(out.position(p), 1.0)));
    return out;
}

std::vector<edges::DecalTrim> trimsFrom(const std::string& text)
{
    std::vector<edges::DecalTrim> out;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_array())
        return out;
    for (const json& t : j)
    {
        edges::DecalTrim d;
        d.name = t.value("name", std::string());
        d.uvMin = {t["uv"][0].get<double>(), t["uv"][1].get<double>()};
        d.uvMax = {t["uv"][2].get<double>(), t["uv"][3].get<double>()};
        d.size = {t["size"][0].get<double>(), t["size"][1].get<double>()};
        d.alongV = t.value("alongV", false);
        d.kinds = t.value("kinds", 3u);
        out.push_back(d);
    }
    return out;
}

/** Strips along a host's sharp edges in world space: decal pieces from a sheet, or user pieces swept along. */
class EdgeDecalNode final : public Node
{
public:
    void compute(const EvalContext&) override
    {
        LILA_ZONE();
        const int outAttr = attrOf(*this, "outMesh"), statsAttr = attrOf(*this, "stats"), warnAttr = attrOf(*this, "warning");
        std::string warning;
        json stats = json::object();
        auto out = std::make_shared<geo::Mesh>();
        try
        {
            const MeshPtr& in = as<MeshPtr>(attrOf(*this, "inMesh"));
            if (in)
            {
                const geo::Mesh world = toWorld(*in, as<glm::dmat4>(attrOf(*this, "inMatrix")));
                const unsigned kinds = kindsOf(as<int64_t>(attrOf(*this, "edges")));
                const std::vector<edges::SharpEdge> sharp = edges::sharpEdges(world, as<double>(attrOf(*this, "angle")), kinds);
                const std::vector<edges::Chain> chains = edges::chainEdges(world, sharp, as<double>(attrOf(*this, "splitAngle")));
                edges::StripStats s;
                const uint32_t seed = uint32_t(as<int64_t>(attrOf(*this, "seed")));
                if (as<int64_t>(attrOf(*this, "mode")) == 0)
                {
                    edges::StripOptions o;
                    o.width = as<double>(attrOf(*this, "width"));
                    o.offset = as<double>(attrOf(*this, "offset"));
                    o.segment = as<double>(attrOf(*this, "segment"));
                    o.coverage = as<double>(attrOf(*this, "coverage"));
                    o.fade = as<double>(attrOf(*this, "fade"));
                    o.jitter = as<double>(attrOf(*this, "jitter"));
                    o.seed = seed;
                    o.material = as<std::string>(attrOf(*this, "material"));
                    const std::vector<edges::DecalTrim> trims = trimsFrom(as<std::string>(attrOf(*this, "trims")));
                    if (trims.empty())
                        warning = "No decal strips: pick a sheet with edgeDecals(sheet=...)";
                    *out = edges::decalStrips(world, chains, trims, o, &s);
                }
                else
                {
                    std::vector<edges::SweepPiece> pieces;
                    std::map<int, double> weights;
                    for (const auto& [index, v] : elements(attrOf(*this, "pieceWeights")))
                        if (const double* w = std::get_if<double>(&v))
                            weights[index] = *w;
                    for (const auto& [index, v] : elements(attrOf(*this, "pieces")))
                        if (const MeshPtr* m = std::get_if<MeshPtr>(&v); m && *m)
                            pieces.push_back({**m, weights.contains(index) ? weights[index] : 1.0});
                    if (pieces.empty())
                        warning = "No pieces to sweep: connect meshes with edgeDecals(mode='mesh', pieces=[...])";
                    edges::SweepOptions o;
                    o.scale = as<double>(attrOf(*this, "pieceScale"));
                    o.offset = as<double>(attrOf(*this, "offset"));
                    o.coverage = as<double>(attrOf(*this, "coverage"));
                    o.seed = seed;
                    *out = edges::sweepPieces(world, chains, pieces, o, &s);
                }
                size_t convexEdges = 0;
                for (const edges::SharpEdge& e : sharp)
                    convexEdges += e.kind == edges::kConvex;
                stats = {{"edges", sharp.size()}, {"convexEdges", convexEdges}, {"concaveEdges", sharp.size() - convexEdges},
                         {"chains", chains.size()}, {"pieces", s.pieces}, {"convexPieces", s.convex}, {"concavePieces", s.concave},
                         {"faces", out->numFaces()}};
            }
        }
        catch (const std::exception& e)
        {
            warning = e.what();
        }
        if (!warning.empty() && warning != mLogged)
            log::write(LogLevel::Warning, std::string(name()) + ": " + warning);
        mLogged = warning;
        setOutput(outAttr, MeshPtr(out));
        setOutput(statsAttr, stats.dump());
        setOutput(warnAttr, warning);
    }

private:
    std::string mLogged;
};

/** Chips out of a mesh's convex edges, live in its modifier stack. */
class EdgeChipNode final : public ModifierNode
{
public:
    EdgeChipNode()
    {
        inAttr = 0;
        enabledAttr = 1;
        outAttr = 14;
    }

    void compute(const EvalContext&) override
    {
        LILA_ZONE();
        const int chipsAttr = attrOf(*this, "chips"), warnAttr = attrOf(*this, "warning");
        mError.clear();
        const MeshPtr& in = as<MeshPtr>(inAttr);
        if (!in || !as<bool>(enabledAttr) || modifiers::bypassed(in))
        {
            setOutput(outAttr, in);
            setOutput(chipsAttr, int64_t(0));
            setOutput(warnAttr, std::string());
            return;
        }
        edges::ChipOptions o;
        o.angle = as<double>(attrOf(*this, "angle"));
        o.size = as<double>(attrOf(*this, "size"));
        o.depth = as<double>(attrOf(*this, "depth"));
        o.amount = as<double>(attrOf(*this, "amount"));
        o.shape = as<double>(attrOf(*this, "shape"));
        o.bevel = as<double>(attrOf(*this, "bevel"));
        o.uvScale = as<double>(attrOf(*this, "uvScale"));
        o.material = as<std::string>(attrOf(*this, "material"));
        o.seed = uint32_t(as<int64_t>(attrOf(*this, "seed")));
        std::vector<geo::Mesh> cutters;
        for (const auto& [index, v] : elements(attrOf(*this, "cutters")))
            if (const MeshPtr* m = std::get_if<MeshPtr>(&v); m && *m)
                cutters.push_back(**m);
        try
        {
            edges::ChipResult r = edges::chipEdges(*in, as<glm::dmat4>(attrOf(*this, "uvSpace")), o, cutters);
            if (!r.warning.empty())
                log::write(LogLevel::Warning, std::string(name()) + ": " + r.warning);
            setOutput(outAttr, MeshPtr(std::make_shared<geo::Mesh>(std::move(r.mesh))));
            setOutput(chipsAttr, int64_t(r.chips));
            setOutput(warnAttr, r.warning);
        }
        catch (const std::exception& e)
        {
            mError = e.what();
            log::write(LogLevel::Warning, std::string(name()) + ": " + e.what());
            setOutput(outAttr, in);
            setOutput(chipsAttr, int64_t(0));
            setOutput(warnAttr, std::string(e.what()));
        }
    }
};

bool registerEdgeNodes(ext::Context& ctx)
{
    {
        NodeType t;
        t.name = kDecalType;
        t.icon = "border-corners";
        t.doc = "Edge decal strips: decal pieces (or swept user meshes) along a mesh's sharp convex and concave edges, in world space, "
                "rebuilt live as the mesh changes. Made by edgeDecals.";
        t.ownAttrs = {
            {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden, .doc = "The host's outMesh."},
            {.name = "inMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden, .doc = "The host's world matrix."},
            enumParam("mode", 0, {"decal", "mesh"}, "Decal strips from a sheet, or user meshes swept along the edges."),
            enumParam("edges", 2, {"convex", "concave", "both"}, "Which sharp edges get strips."),
            param("angle", PlugType::Float, 35.0, "Faces must meet at more than this many degrees.", 0.0, 180.0),
            param("splitAngle", PlugType::Float, 40.0, "A run of edges breaks where it turns by more than this many degrees.", 0.0, 180.0),
            param("width", PlugType::Float, 8.0, "Strip width across the edge, cm.", 0.01),
            param("offset", PlugType::Float, 0.05, "Lift off the surface, cm.", 0.0),
            param("segment", PlugType::Float, 0.0, "cm of strip per decal piece; 0 keeps the strip's own aspect.", 0.0),
            param("coverage", PlugType::Float, 0.8, "Share of the edge length covered, 0 to 1.", 0.0, 1.0),
            param("fade", PlugType::Float, 6.0, "cm over which a run narrows away at its ends.", 0.0),
            param("jitter", PlugType::Float, 0.25, "Each piece's width varies by up to this fraction.", 0.0, 0.95),
            param("pieceScale", PlugType::Float, 1.0, "Swept pieces: scale across the edge.", 0.0001),
            param("seed", PlugType::Int, int64_t(1), "Another seed gives other gaps and other strips."),
            {.name = "trims", .type = PlugType::String, .defaultValue = std::string("[]"), .flags = AttrInput | AttrStorable | AttrHidden,
             .doc = "The sheet strips the decals pick from, as JSON."},
            {.name = "material", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable, .doc = "Material slot of the decal faces."},
            {.name = "pieces", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden | AttrArray, .doc = "Meshes swept along the edges."},
            {.name = "pieceWeights", .type = PlugType::Float, .defaultValue = 1.0, .flags = AttrInput | AttrStorable | AttrHidden | AttrArray,
             .doc = "How often each piece is picked."},
            {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden},
            {.name = "stats", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput, .doc = "What the last build made, as JSON."},
            {.name = "warning", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput, .doc = "Problems with the last build."},
        };
        t.create = [] { return std::make_unique<EdgeDecalNode>(); };
        if (!types::registerType(ctx, std::move(t)))
            return false;
        relocate::registerWorldAttrs(kDecalType, {.worldOutput = true});
    }
    {
        NodeType t;
        t.name = kChipType;
        t.icon = "hammer";
        t.doc = "Edge Chip: breaks a mesh's convex edges with chips cut out by a boolean, from noisy realistic breaks to clean faceted "
                "chunks, keeping the mesh closed. Cut faces get uvs projected in world space. Live: rebuilt whenever the mesh changes.";
        t.ownAttrs = {
            {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden},
            param("enabled", PlugType::Bool, true, "Turn the chips off without removing them."),
            {.name = "exportEnabled", .type = PlugType::Bool, .defaultValue = true, .flags = AttrInput | AttrStorable, .doc = "Apply when exporting."},
            param("angle", PlugType::Float, 40.0, "Faces must meet at more than this many degrees for their edge to chip.", 0.0, 180.0),
            param("size", PlugType::Float, 8.0, "Chunk size along the edge, cm.", 0.01),
            param("depth", PlugType::Float, 2.5, "Deepest cut into the solid from the edge, cm.", 0.001),
            param("amount", PlugType::Float, 0.5, "Share of the edge chipped, 0 to 1.", 0.0, 1.0),
            param("shape", PlugType::Float, 0.0, "0 noisy realistic breaks, 1 clean faceted chunks.", 0.0, 1.0),
            param("bevel", PlugType::Float, 0.0, "Bevel the sharp edges first, cm; 0 for none.", 0.0),
            param("uvScale", PlugType::Float, 100.0, "cm per uv unit on the cut faces.", 0.001),
            {.name = "material", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable,
             .doc = "Material slot of the cut faces; empty uses the slot of the face each chip breaks."},
            param("seed", PlugType::Int, int64_t(1), "Another seed chips other places."),
            {.name = "uvSpace", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrStorable | AttrHidden,
             .doc = "Object to world, as the cut faces' uvs are projected (the host's world matrix when the chips were made)."},
            {.name = "cutters", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden | AttrArray,
             .doc = "User meshes used as chip shapes instead of generated ones."},
            {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden},
            {.name = "chips", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrOutput, .doc = "Chips cut."},
            {.name = "warning", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput, .doc = "Problems with the last build."},
        };
        t.create = [] { return std::make_unique<EdgeChipNode>(); };
        relocate::registerNeutral(t.name);
        if (!types::registerType(ctx, std::move(t)))
            return false;
    }
    return true;
}

// ---- Placeholder swept pieces -------------------------------------------------------------------------------------

/** A closed tube along +X: a ring of (y, z) points at each of segs + 1 stations, capped. shapeAt varies a ring point. */
geo::Mesh profileTube(const std::vector<glm::dvec2>& ring, double length, int segs, const std::function<glm::dvec2(int, int, glm::dvec2)>& shapeAt)
{
    std::vector<glm::dvec3> pts;
    std::vector<geo::Index> sizes, idx;
    const int n = int(ring.size());
    for (int s = 0; s <= segs; ++s)
        for (int i = 0; i < n; ++i)
        {
            const glm::dvec2 p = shapeAt ? shapeAt(s, i, ring[size_t(i)]) : ring[size_t(i)];
            pts.emplace_back(length * double(s) / segs, p.x, p.y);
        }
    for (int s = 0; s < segs; ++s)
        for (int i = 0; i < n; ++i)
        {
            const int j = (i + 1) % n;
            sizes.push_back(4);
            idx.insert(idx.end(), {geo::Index(s * n + i), geo::Index(s * n + j), geo::Index((s + 1) * n + j), geo::Index((s + 1) * n + i)});
        }
    sizes.push_back(geo::Index(n));
    for (int i = n - 1; i >= 0; --i)
        idx.push_back(geo::Index(i));
    sizes.push_back(geo::Index(n));
    for (int i = 0; i < n; ++i)
        idx.push_back(geo::Index(segs * n + i));
    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    geo::conformNormals(m);
    return m;
}

/** A lumpy moss lip: a half-round cushion that sits over an edge, 40 cm long. */
geo::Mesh mossLip()
{
    std::vector<glm::dvec2> ring;
    for (int i = 0; i < 10; ++i)
    {
        const double a = glm::pi<double>() * (0.1 + 0.8 * i / 9.0);
        ring.emplace_back(std::sin(a) * 2.2 - 0.4, std::cos(a) * 3.0);
    }
    return profileTube(ring, 40.0, 16, [](int s, int i, glm::dvec2 p) {
        const double lump = 0.75 + 0.5 * unitHash(17, uint32_t(s / 2), uint32_t(i / 3)) * (s == 0 || s == 16 ? 0.3 : 1.0);
        return glm::dvec2(p.x * lump, p.y * (0.85 + 0.3 * unitHash(29, uint32_t(s), 0)));
    });
}

/** A chunky chamfered toon trim, 40 cm long, square-ish with cut corners, for outlines and caps over edges. */
geo::Mesh toonTrim()
{
    const double h = 1.6, c = 0.6;
    const std::vector<glm::dvec2> ring = {{-h + c, -h}, {h - c, -h}, {h, -h + c}, {h, h - c}, {h - c, h}, {-h + c, h}, {-h, h - c}, {-h, -h + c}};
    return profileTube(ring, 40.0, 8, {});
}

void registerEdgePlaceholders()
{
    addPlaceholder({.collection = "Weathering/EdgeTrims",
                    .name = "MossLip",
                    .role = "edge-trim",
                    .style = "realistic",
                    .tags = {"moss", "lip", "edge"},
                    .description = "A lumpy moss cushion that runs along an edge (edgeDecals mode='mesh'). X runs along the edge.",
                    .physicsMaterial = "fabric",
                    .build = mossLip});
    addPlaceholder({.collection = "Weathering/EdgeTrims",
                    .name = "ToonTrim",
                    .role = "edge-trim",
                    .style = "toon",
                    .tags = {"trim", "outline", "edge"},
                    .description = "A chunky chamfered trim for stylized edges and outlines (edgeDecals mode='mesh'). X runs along the edge.",
                    .physicsMaterial = "wood",
                    .build = toonTrim});
}

// ---- Commands -----------------------------------------------------------------------------------------------------

std::vector<Node*> hostMeshes(const Args& a, const char* cmd)
{
    Graph& g = Graph::current();
    std::vector<Node*> out;
    std::vector<std::string> names = a.objects();
    if (names.empty())
        for (NodeId id : g.selection())
            if (Node* n = g.node(id))
                names.push_back(n->path());
    std::set<NodeId> seen;
    std::function<void(Node*)> add = [&](Node* n) {
        if (n->type().isA("mesh"))
        {
            if (seen.insert(n->id()).second)
                out.push_back(n);
            return;
        }
        for (NodeId c : g.children(n->id()))
            if (Node* k = g.node(c))
                add(k);
    };
    for (const std::string& s : names)
    {
        Node* n = g.find(s);
        if (!n)
            throw CommandError(std::string(cmd) + ": no node named '" + s + "'");
        add(n);
    }
    if (out.empty())
        throw CommandError(std::string(cmd) + ": select or name meshes (or groups of them)");
    return out;
}

std::vector<std::string> stringList(const Args& a, const char* flag, std::vector<std::string> fallback)
{
    if (!a.has(flag))
        return fallback;
    std::vector<std::string> out;
    const json v = a.flags[std::string(flag)];
    if (v.is_string())
        out.push_back(v.get<std::string>());
    else if (v.is_array())
        for (const json& x : v)
            out.push_back(x.is_string() ? x.get<std::string>() : x.dump());
    return out;
}

/** What a sheet gives the decals: its trims as DecalTrims (JSON for the node), material and maps. */
struct SheetChoice
{
    std::string file;
    std::string material;
    json trims = json::array();
    json textures = json::object();
    std::vector<std::string> notes;
};

SheetChoice loadSheet(const Args& a, Style style, const char* cmd)
{
    if (!ops::registry().find("trimSheetInfo"))
        throw CommandError(std::string(cmd) + ": decal strips map trim or decal sheets, which need the Trim Sheets extension; turn it on, "
                                              "or sweep your own meshes with mode='mesh'");
    SheetChoice c;
    std::string sheet = a.has("sheet") ? a.get<std::string>("sheet") : std::string();
    if (sheet.empty())
    {
        sheet = edges::edgeSheet(styleName(style)).generic_string();
        c.notes.push_back(std::string("No sheet given: used the placeholder ") + styleName(style) + " edge decal sheet");
    }
    json info;
    try
    {
        info = ops::execute("trimSheetInfo", {{"sheet", sheet}});
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": sheet '" + sheet + "': " + e.what());
    }
    c.file = info.value("file", sheet);
    c.material = info.value("material", std::string());
    c.textures = info.value("textures", json::object());

    const std::vector<std::string> convexTags = stringList(a, "convexTags", {"chips", "worn", "crack"});
    const std::vector<std::string> concaveTags = stringList(a, "concaveTags", {"dirt", "moss", "crack"});
    const std::vector<std::string> only = stringList(a, "trims", {});
    const json all = info.value("trims", json::array());
    auto tagged = [](const json& t, const std::vector<std::string>& tags) {
        for (const json& x : t.value("tags", json::array()))
            if (x.is_string() && std::ranges::find(tags, x.get<std::string>()) != tags.end())
                return true;
        return false;
    };
    auto named = [&](const json& t) {
        return std::ranges::find(only, t.value("name", std::string())) != only.end() ||
               std::ranges::find(only, std::to_string(t.value("index", -1))) != only.end();
    };
    auto add = [&](const json& t, unsigned kinds) {
        const json ws = t.value("worldSizeCm", json::array({100.0, 10.0}));
        const json uv = t.value("uv", json::object());
        const double wx = ws[0].get<double>(), wy = ws[1].get<double>();
        const bool alongV = wy > wx;
        c.trims.push_back({{"name", t.value("name", std::string())},
                           {"uv", {uv["min"][0], uv["min"][1], uv["max"][0], uv["max"][1]}},
                           {"size", alongV ? json::array({wy, wx}) : json::array({wx, wy})},
                           {"alongV", alongV},
                           {"kinds", kinds}});
    };
    if (!only.empty())
    {
        for (const json& t : all)
            if (named(t))
                add(t, edges::kConvex | edges::kConcave);
        if (c.trims.empty())
            throw CommandError(std::string(cmd) + ": the sheet has none of the trims " + json(only).dump());
        return c;
    }
    bool convex = false, concave = false;
    for (const json& t : all)
    {
        const unsigned k = (tagged(t, convexTags) ? edges::kConvex : 0u) | (tagged(t, concaveTags) ? edges::kConcave : 0u);
        if (k)
        {
            add(t, k);
            convex = convex || (k & edges::kConvex);
            concave = concave || (k & edges::kConcave);
        }
    }
    if (!convex || !concave)
    {
        // Kinds no tag matched take the sheet's edge-tagged strips, else every strip on it.
        const unsigned missing = (convex ? 0u : edges::kConvex) | (concave ? 0u : edges::kConcave);
        bool found = false;
        for (const json& t : all)
            if (tagged(t, {"edge"}))
            {
                add(t, missing);
                found = true;
            }
        if (!found)
            for (const json& t : all)
                add(t, missing);
        c.notes.push_back(std::string("No trims tagged for ") + (missing == 3 ? "either edge kind" : missing == 1 ? "convex edges" : "concave edges") +
                          (found ? ": used the sheet's edge-tagged trims" : ": used every trim on the sheet"));
    }
    if (c.trims.empty())
        throw CommandError(std::string(cmd) + ": the sheet has no trims");
    return c;
}

/** The decal material in Lila's viewport: the sheet's maps, translucent when it has an opacity map. */
void ensureDecalMaterial(const SheetChoice& c)
{
    Graph& g = Graph::current();
    if (c.material.empty() || materials::find(g, c.material))
        return;
    json call = {{"args", {c.material}}, {"roughness", 0.75}};
    if (c.textures.contains("baseColor"))
        call["baseColorMap"] = c.textures["baseColor"];
    if (c.textures.contains("normal"))
        call["normalMap"] = c.textures["normal"];
    if (c.textures.contains("opacity"))
        call["blendMode"] = "translucent";
    ops::execute("createMaterial", call);
}

void setString(Graph& g, Node& n, const char* attr, const std::string& v)
{
    if (attrOf(n, attr) < 0)
        g.addAttr(n.id(), {.name = attr, .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrUser | AttrHidden,
                           .doc = "Weathering."});
    g.setValue({n.id(), attrOf(n, attr), -1}, v);
}

std::string groupFor(const Args& a, const std::string& fallback)
{
    const json made = ops::execute("group", {{"empty", true}, {"name", a.get<std::string>("name", fallback)}});
    return made.is_array() ? made[0].get<std::string>() : made.get<std::string>();
}

EdgeChipNode* chipOn(Graph& g, NodeId mesh)
{
    const std::vector<NodeId> s = modifiers::stack(g, mesh);
    for (auto it = s.rbegin(); it != s.rend(); ++it)
        if (Node* n = g.node(*it); n && n->type().name == kChipType)
            return static_cast<EdgeChipNode*>(n);
    return nullptr;
}

/**
 * Where edge decals read a host: its mesh before any Edge Chip, so strips follow the clean edges instead of every chip
 * facet (and float over the chips like paint lifting off them).
 */
PlugRef decalSource(Graph& g, NodeId host)
{
    if (EdgeChipNode* chip = chipOn(g, host))
        if (const std::optional<PlugRef> src = g.source({chip->id(), chip->inAttr, -1}))
            return *src;
    return {host, MeshNode::kOutMesh, -1};
}

/** Points the host's edge decal nodes at decalSource, after its stack changed. */
void rewireDecals(Graph& g, NodeId host)
{
    const PlugRef to = decalSource(g, host);
    std::vector<PlugRef> readers = g.destinations({host, MeshNode::kOutMesh, -1});
    if (EdgeChipNode* chip = chipOn(g, host))
        for (const PlugRef& r : g.destinations({chip->id(), attrOf(*chip, "outMesh"), -1}))
            readers.push_back(r);
    for (const PlugRef& r : readers)
        if (Node* n = g.node(r.node); n && n->type().name == kDecalType && r.attr == attrOf(*n, "inMesh") && g.source(r) != to)
            g.connect(to, r, true);
}

void registerEdgeCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "edgeDecals",
        .doc = "Lay decal strips along meshes' sharp edges: chipped paint and worn corners on convex edges, dirt, cracks and moss in "
               "concave ones. Edges count when their faces meet at more than angle degrees; Blockwork tool faces (nodraw, clip, skip), "
               "hidden faces and edges where blocks touch are skipped. Runs of edges follow curved loops until they turn by more than "
               "splitAngle. Each run is cut into pieces, each mapped onto a strip picked at random from the sheet (tags chips, worn, "
               "crack for convex edges, dirt, moss, crack for concave ones), with gaps (coverage) and runs narrowing away at their "
               "ends (fade). The sheet is any trim or decal sheet, your own painted ones included; without one the placeholder sheet "
               "for the style is used. mode='mesh' sweeps your own meshes along the edges instead (stylized trims, toon outlines, "
               "moss lips): X along the edge, +Y out of the surface. Live: strips rebuild when a mesh changes (Blockwork extend, "
               "clip, carve). trimDecalSend sends the strips to Unreal as mesh decals on M_LilaDecal (MF_LilaDecal). Returns group, "
               "meshes, nodes, sheet, material, stats and notes.",
        .positionalDoc = "meshes or groups (default the selection)",
        .flags = {{"mode", "m", T::String, "decal", "decal (strips from a sheet) or mesh (your meshes swept along).", {"decal", "mesh"}},
                  {"edges", "e", T::String, "both", "Which sharp edges: convex, concave or both.", {"convex", "concave", "both"}},
                  {"angle", "a", T::Float, 35.0, "Faces must meet at more than this many degrees.", {}, {}, 0.0, 180.0},
                  {"splitAngle", "sa", T::Float, 40.0, "A run breaks where it turns by more than this many degrees.", {}, {}, 0.0, 180.0},
                  {"width", "w", T::Float, 8.0, "Strip width across the edge, cm.", {}, {}, 0.01},
                  {"offset", "of", T::Float, 0.05, "Lift off the surface, cm.", {}, {}, 0.0},
                  {"segment", "sg", T::Float, 0.0, "cm of strip per decal piece; 0 keeps each strip's own aspect.", {}, {}, 0.0},
                  {"coverage", "cv", T::Float, 0.8, "Share of the edge length covered, 0 to 1.", {}, {}, 0.0, 1.0},
                  {"fade", "fd", T::Float, 6.0, "cm over which a run narrows away at its ends.", {}, {}, 0.0},
                  {"jitter", "j", T::Float, 0.25, "Each piece's width varies by up to this fraction.", {}, {}, 0.0, 0.95},
                  {"sheet", "sh", T::String, nullptr, "Trim or decal sheet (.lilatrim or its name); the style's placeholder sheet when not given."},
                  {"trims", "tr", T::List, nullptr, "Only these strips of the sheet (names or indices), for both edge kinds."},
                  {"convexTags", "cxt", T::List, json::array({"chips", "worn", "crack"}), "Strip tags for convex edges."},
                  {"concaveTags", "cct", T::List, json::array({"dirt", "moss", "crack"}), "Strip tags for concave edges."},
                  piecesFlag("pieces", "pc", "mode='mesh': meshes to sweep (scene meshes, groups, sets, library ids, tag:<word>, weighted); the "
                                             "style's placeholder edge trims when not given."),
                  {"pieceScale", "ps", T::Float, 1.0, "mode='mesh': scale of the pieces across the edge.", {}, {}, 0.0001},
                  {"seed", "s", T::Int, int64_t(1), "Another seed gives other gaps and other strips."},
                  styleFlag(),
                  {"realize", "r", T::Bool, false, "Bake to plain meshes instead of live strips."},
                  {"name", "n", T::String, "EdgeDecals", "Name of the group made."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            Graph& g = Graph::current();
            const std::vector<Node*> hosts = hostMeshes(a, "edgeDecals");
            const Style style = styleArg(a);
            const bool meshMode = a.get<std::string>("mode", "decal") == "mesh";
            const std::string edgesName = a.get<std::string>("edges", "both");
            const int64_t edgesValue = edgesName == "convex" ? 0 : edgesName == "concave" ? 1 : 2;
            SheetChoice sheet;
            std::vector<Piece> pieces;
            if (meshMode)
                pieces = piecesOrDefault(g, a, "pieces", "edge-trim", style, "edgeDecals");
            else
            {
                sheet = loadSheet(a, style, "edgeDecals");
                ensureDecalMaterial(sheet);
            }
            std::vector<NodeId> hostIds;
            for (Node* h : hosts)
                hostIds.push_back(h->id());
            const std::string group = groupFor(a, "EdgeDecals");
            const NodeId groupId = g.find(group)->id();

            json meshes = json::array(), nodes = json::array(), stats = json::object(), warnings = json::array();
            for (NodeId hid : hostIds)
            {
                Node* host = g.node(hid);
                Node* strip = g.createNode(*types::find(kDecalType), host->name() + "EdgeStrips");
                const NodeId sid = strip->id();
                g.connect(decalSource(g, hid), {sid, attrOf(*strip, "inMesh"), -1}, true);
                g.connect({hid, Transform::kWorldMatrix, -1}, {sid, attrOf(*strip, "inMatrix"), -1}, true);
                auto setValue = [&](const char* attr, PlugValue v) { g.setValue({sid, attrOf(*g.node(sid), attr), -1}, std::move(v)); };
                setValue("mode", int64_t(meshMode ? 1 : 0));
                setValue("edges", edgesValue);
                for (const auto& [flag, def] : {std::pair{"angle", 35.0}, {"splitAngle", 40.0}, {"width", 8.0}, {"offset", 0.05}, {"segment", 0.0},
                                                {"coverage", 0.8}, {"fade", 6.0}, {"jitter", 0.25}, {"pieceScale", 1.0}})
                    setValue(flag, a.get<double>(flag, def));
                setValue("seed", a.get<int64_t>("seed", 1));
                if (!meshMode)
                {
                    setValue("trims", sheet.trims.dump());
                    setValue("material", sheet.material);
                }
                else
                    for (size_t k = 0; k < pieces.size(); ++k)
                    {
                        g.connect({pieces[k].mesh, MeshNode::kOutMesh, -1}, {sid, attrOf(*g.node(sid), "pieces"), -1, int(k)}, true);
                        g.setValue({sid, attrOf(*g.node(sid), "pieceWeights"), -1, int(k)}, pieces[k].weight);
                    }
                Node* mesh = g.createNode(*types::find("mesh"), host->name() + "_EdgeDecals", groupId);
                const NodeId mid = mesh->id();
                g.connect({sid, attrOf(*g.node(sid), "outMesh"), -1}, {mid, MeshNode::kInMesh, -1}, true);
                if (!meshMode)
                    setString(g, *g.node(mid), kMeshDecalAttr, sheet.file);
                g.evaluate();
                Node* s = g.node(sid);
                json st = json::parse(s->as<std::string>(attrOf(*s, "stats")), nullptr, false);
                if (const std::string w = s->as<std::string>(attrOf(*s, "warning")); !w.empty())
                    warnings.push_back(host->name() + ": " + w);
                if (a.get<bool>("realize", false))
                {
                    const MeshPtr baked = g.node(mid)->as<MeshPtr>(MeshNode::kOutMesh);
                    ops::execute("delete", {{"args", {s->path()}}});
                    g.setValue({mid, MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(baked ? *baked : geo::Mesh())));
                }
                else
                    nodes.push_back(g.node(sid)->path());
                meshes.push_back(g.node(mid)->path());
                stats[host->name()] = st;
            }
            json out = {{"group", g.node(groupId)->path()}, {"meshes", meshes}, {"nodes", nodes}, {"stats", stats}, {"warnings", warnings}};
            if (!meshMode)
            {
                out["sheet"] = sheet.file;
                out["material"] = sheet.material;
                out["notes"] = sheet.notes;
            }
            else
            {
                json names = json::array();
                for (const Piece& p : pieces)
                    names.push_back(p.name);
                out["pieces"] = names;
            }
            return out;
        },
        .examples = {"edgeDecals('Level', edges='both', width=10)", "edgeDecals('wall1', sheet='T_MyGrunge', convexTags=['chips'], concaveTags=['moss'])",
                     "edgeDecals('ruin', mode='mesh', pieces=['MossLip1', {'piece': 'MossLip2', 'weight': 2}], edges='convex', style='toon')"},
    });

    ctx.registerCommand({
        .name = "edgeChip",
        .doc = "Chip meshes' convex edges with a live Edge Chip modifier: chunks broken out along every edge sharper than angle, "
               "size cm long and up to depth cm deep, on amount of the edge. shape runs from noisy realistic breaks (0) to clean "
               "faceted toon chunks (1); the style picks one when shape isn't given. cutters= uses your own meshes as chip shapes "
               "(fitted to size along X and to depth around it). The mesh stays closed; cut faces get uvs projected in world space "
               "and the material of the face they break (or material=). Blockwork tool faces, hidden faces and edges where blocks "
               "touch never chip. Live: the chips rebuild when a block is extended, clipped or carved. Running it again on a chipped "
               "mesh changes its modifier. Returns modifiers, chips per mesh and group (empty: chips live on the meshes).",
        .positionalDoc = "meshes or groups (default the selection)",
        .flags = {{"angle", "a", T::Float, 40.0, "Faces must meet at more than this many degrees for their edge to chip.", {}, {}, 0.0, 180.0},
                  {"size", "sz", T::Float, 8.0, "Chunk size along the edge, cm.", {}, {}, 0.01},
                  {"depth", "d", T::Float, 2.5, "Deepest cut into the solid from the edge, cm.", {}, {}, 0.001},
                  {"amount", "am", T::Float, 0.5, "Share of the edge chipped, 0 to 1.", {}, {}, 0.0, 1.0},
                  {"shape", "sh", T::Float, nullptr, "0 noisy realistic breaks to 1 clean faceted chunks; 0 for realistic, 1 for toon when not given.",
                   {}, {}, 0.0, 1.0},
                  {"bevel", "bv", T::Float, 0.0, "Bevel the chipped edges first, cm; 0 for none.", {}, {}, 0.0},
                  {"uvScale", "uv", T::Float, 100.0, "cm per uv unit on the cut faces.", {}, {}, 0.001},
                  {"material", "mt", T::String, nullptr, "Material slot for the cut faces; the broken face's own when not given."},
                  piecesFlag("cutters", "ct", "Your own chip shapes (closed meshes, sets, library ids, tag:<word>), fitted to each chip."),
                  {"seed", "s", T::Int, int64_t(1), "Another seed chips other places."},
                  styleFlag()},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            Graph& g = Graph::current();
            const std::vector<Node*> hosts = hostMeshes(a, "edgeChip");
            std::vector<Piece> cutters;
            if (a.has("cutters"))
                cutters = resolvePieces(g, a.flags["cutters"], "edgeChip");
            json params = json::object();
            for (const char* k : {"angle", "size", "depth", "amount", "bevel", "uvScale", "material", "seed"})
                if (a.has(k))
                    params[k] = a.flags[k];
            params["shape"] = a.has("shape") ? a.flags["shape"] : json(styleArg(a) == Style::Toon ? 1.0 : 0.0);
            std::vector<NodeId> ids;
            for (Node* h : hosts)
                ids.push_back(h->id());
            json mods = json::array(), chips = json::object(), warnings = json::array();
            for (NodeId id : ids)
            {
                g.evaluate();
                Node* mesh = g.node(id);
                Node* mod = chipOn(g, id);
                if (!mod)
                {
                    const std::string made = ops::execute("addModifier", {{"args", {mesh->path()}}, {"kind", kChipKind}, {"params", params}});
                    mod = g.find(made);
                }
                else
                    for (const auto& [k, v] : params.items())
                        ops::execute("setAttr", {{"args", {mod->path() + "." + k, v}}});
                const NodeId modId = mod->id();
                rewireDecals(g, id);
                g.setValue({modId, attrOf(*mod, "uvSpace"), -1}, g.node(id)->as<glm::dmat4>(Transform::kWorldMatrix));
                if (a.has("cutters"))
                {
                    for (const PlugRef& e : g.elements({modId, attrOf(*g.node(modId), "cutters"), -1}))
                        g.disconnect(e);
                    for (const Piece& p : cutters)
                        g.connect({p.mesh, MeshNode::kOutMesh, -1}, {modId, attrOf(*g.node(modId), "cutters"), -1}, true);
                }
                g.evaluate();
                Node* m = g.node(modId);
                mods.push_back(m->path());
                chips[g.node(id)->name()] = m->as<int64_t>(attrOf(*m, "chips"));
                if (const std::string w = m->as<std::string>(attrOf(*m, "warning")); !w.empty())
                    warnings.push_back(g.node(id)->name() + ": " + w);
            }
            return {{"group", ""}, {"modifiers", mods}, {"chips", chips}, {"warnings", warnings}};
        },
        .examples = {"edgeChip('Level', size=10, depth=3, amount=0.6)", "edgeChip('pillar1', style='toon', size=14, depth=4)",
                     "edgeChip('wall1', cutters=['ChipShape1', 'ChipShape2'], material='M_Brick')"},
    });

    ctx.registerCommand({
        .name = "edgeDecalSheets",
        .doc = "Write the placeholder edge decal sheets (realistic and hand-painted toon: chipped paint, worn corners, cracks, dirt and "
               "moss strips), which edgeDecals uses when no sheet is given. Built on first use; force=True rewrites them. Returns dir, "
               "built and the two sheet files.",
        .flags = {{"force", "f", T::Bool, false, "Rewrite even when they are up to date."}},
        .undoable = false,
        .run = [](const Args& a) -> json { return edges::writeEdgeSheets(a.get<bool>("force", false)); },
        .examples = {"edgeDecalSheets()['toon']"},
    });

    ctx.registerCommand({
        .name = "edgeFind",
        .doc = "The sharp edges of a mesh as edgeDecals and edgeChip see them: convex and concave edge ids past angle (tool faces, hidden "
               "faces and block contacts skipped) and the runs they join into. Returns convex, concave and chains [{kind, edges, closed}].",
        .positionalDoc = "mesh",
        .flags = {{"angle", "a", T::Float, 35.0, "Faces must meet at more than this many degrees.", {}, {}, 0.0, 180.0},
                  {"splitAngle", "sa", T::Float, 40.0, "A run breaks where it turns by more than this many degrees.", {}, {}, 0.0, 180.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const std::vector<Node*> hosts = hostMeshes(a, "edgeFind");
            g.evaluate();
            const MeshPtr m = hosts.front()->as<MeshPtr>(MeshNode::kOutMesh);
            if (!m)
                return {{"convex", json::array()}, {"concave", json::array()}, {"chains", json::array()}};
            const std::vector<edges::SharpEdge> sharp = edges::sharpEdges(*m, a.get<double>("angle", 35.0), edges::kConvex | edges::kConcave);
            json convex = json::array(), concave = json::array(), chains = json::array();
            for (const edges::SharpEdge& e : sharp)
                (e.kind == edges::kConvex ? convex : concave).push_back(e.edge);
            for (const edges::Chain& c : edges::chainEdges(*m, sharp, a.get<double>("splitAngle", 40.0)))
                chains.push_back({{"kind", c.kind == edges::kConvex ? "convex" : "concave"}, {"edges", c.edges}, {"closed", c.closed}});
            return {{"convex", convex}, {"concave", concave}, {"chains", chains}};
        },
        .examples = {"len(edgeFind('wall1', angle=60)['convex'])"},
    });
}

void registerEdgeUi(ext::Context& ctx)
{
    using namespace lila::ui;
    actions::add(ctx, {.id = "weathering.edgeDecals",
                       .label = "Edge Decals",
                       .category = "Weathering",
                       .doc = "Decal strips along the selection's sharp edges: chips and wear on convex edges, dirt and moss in concave ones. Live.",
                       .python = "edgeDecals(edges='both', width=8)",
                       .command = "edgeDecals",
                       .call = json::object(),
                       .icon = "border-corners",
                       .keywords = {"edge wear", "edge decals", "grunge", "corner dirt", "chipped paint", "mesh decals", "trim decals", "moss",
                                    "cracks", "weathering", "Edge Wear", "Decal Machine", "Substance edges"}});
    actions::add(ctx, {.id = "weathering.edgeChip",
                       .label = "Edge Chip",
                       .category = "Weathering",
                       .doc = "Break the selection's convex edges with chips cut into the mesh, realistic to faceted toon chunks. Live.",
                       .python = "edgeChip(size=8, depth=2.5, amount=0.5)",
                       .command = "edgeChip",
                       .call = json::object(),
                       .icon = "hammer",
                       .keywords = {"edge chipping", "edge damage", "chips", "broken edges", "damage", "weathering", "Edge Wear",
                                    "Damage Edges", "ruin", "worn edges"}});

    ops::options::setOverride("edgeDecals",
                              {.order = {"mode", "edges", "angle", "splitAngle", "width", "offset", "coverage", "fade", "jitter", "segment", "sheet",
                                         "trims", "convexTags", "concaveTags", "pieces", "pieceScale", "style", "seed", "realize", "name"},
                               .labels = {{"splitAngle", "Split Angle"}, {"convexTags", "Convex Tags"}, {"concaveTags", "Concave Tags"},
                                          {"pieceScale", "Piece Scale"}},
                               .sections = {{"mode", "Edges"}, {"width", "Strips"}, {"sheet", "Sheet"}, {"pieces", "Swept Meshes"}, {"style", "Result"}},
                               .choices = {{"mode", {{"decal", "Decal"}, {"mesh", "Mesh"}}},
                                           {"edges", {{"convex", "Convex"}, {"concave", "Concave"}, {"both", "Both"}}},
                                           {"style", {{"realistic", "Realistic"}, {"toon", "Toon"}}}},
                               .ranges = {{"angle", {0.0, 180.0, 1.0, " deg"}}, {"splitAngle", {0.0, 180.0, 1.0, " deg"}}, {"width", {0.01, 200.0, 0.5, " cm"}},
                                          {"offset", {0.0, 5.0, 0.01, " cm"}}, {"segment", {0.0, 1000.0, 1.0, " cm"}}, {"coverage", {0.0, 1.0, 0.05, ""}},
                                          {"fade", {0.0, 100.0, 0.5, " cm"}}, {"jitter", {0.0, 0.95, 0.05, ""}}}});
    ops::options::setOverride("edgeChip",
                              {.order = {"size", "depth", "amount", "shape", "angle", "bevel", "cutters", "material", "uvScale", "style", "seed"},
                               .labels = {{"uvScale", "UV Scale"}},
                               .sections = {{"size", "Chips"}, {"angle", "Edges"}, {"cutters", "Surface"}, {"style", "Result"}},
                               .choices = {{"style", {{"realistic", "Realistic"}, {"toon", "Toon"}}}},
                               .ranges = {{"size", {0.01, 200.0, 0.5, " cm"}}, {"depth", {0.001, 50.0, 0.1, " cm"}}, {"amount", {0.0, 1.0, 0.05, ""}},
                                          {"shape", {0.0, 1.0, 0.05, ""}}, {"angle", {0.0, 180.0, 1.0, " deg"}}, {"bevel", {0.0, 20.0, 0.1, " cm"}},
                                          {"uvScale", {0.001, 10000.0, 10.0, " cm"}}}});

}
} // namespace edgesui

bool registerEdges(lila::ext::Context& ctx)
{
    if (!edgesui::registerEdgeNodes(ctx))
        return false;
    edgesui::registerEdgePlaceholders();
    edgesui::registerEdgeCommands(ctx);
    edgesui::registerEdgeUi(ctx);
    return true;
}
} // namespace weathering
