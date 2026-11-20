// Ivy and other growth over surfaces: the live ivy node fed by its hosts' meshes, the Ivy geometry node, the ivy,
// ivyPaint, ivyInfo and ivySend commands, and the leaf, flower and moss placeholders.

#include "Ivy.h"
#include "IvyGrow.h"
#include "Pieces.h"
#include "Placeholders.h"
#include "Weathering.h"

#include <lila/Command.h>
#include <lila/Curves.h>
#include <lila/Extension.h>
#include <lila/Fbx.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Profile.h>
#include <lila/Relocate.h>
#include <lila/Transform.h>
#include <lila/UnrealLink.h>

#include <algorithm>
#include <filesystem>
#include <map>

namespace weathering
{
namespace ivynode
{
using namespace lila;
using namespace lila::scene;
using T = ArgType;
using D3 = glm::dvec3;

const std::vector<std::string> kStyles = {"realistic", "toon"};
const std::vector<std::string> kProfiles = {"round", "ribbon", "custom"};
const std::vector<std::string> kOrients = {"surface", "up", "stem", "random"};
const std::vector<std::string> kPlants = {"ivy", "vine", "roots", "moss", "tendril"};

/** What a plant preset sets before the flags given on top of it. */
struct Plant
{
    const char* name;
    double length, branchiness, climb, gravity, smoothness, stemRadius, leafDensity, leafSize, flowerDensity;
    const char* profile;
    const char* leafRole; // empty: no leaves
    const char* orient;
    const char* doc;
};

constexpr Plant kPlantPresets[] = {
    {"ivy", 300, 0.5, 0.7, 0.3, 0.15, 0.6, 30, 8, 0, "round", "ivy-leaf", "surface", "Climbing ivy: dense leaves facing out, hangs over ledges."},
    {"vine", 450, 0.3, 0.5, 0.6, 0.25, 0.9, 14, 12, 0, "round", "vine-leaf", "up", "Long vines with broad leaves that drape from ledges."},
    {"roots", 250, 0.7, 0.0, 0.2, 0.1, 1.2, 0, 8, 0, "round", "", "stem", "Bare roots spreading every way over the surface."},
    {"moss", 120, 0.9, 0.2, 0.4, 0.2, 0.25, 60, 3, 0, "ribbon", "moss-tuft", "random", "Moss strands: flat, dense, short, tufted."},
    {"tendril", 200, 0.2, 0.6, 0.2, 0.9, 0.5, 6, 6, 0, "round", "ivy-leaf", "stem", "Stylized tendrils: smooth curling arcs, few leaves."},
};

const Plant& plantNamed(const std::string& name)
{
    for (const Plant& p : kPlantPresets)
        if (name == p.name)
            return p;
    throw CommandError("ivy: plant is ivy, vine, roots, moss or tendril, not '" + name + "'");
}

int indexIn(const std::vector<std::string>& list, const std::string& v, int fallback = 0)
{
    const auto it = std::ranges::find(list, v);
    return it == list.end() ? fallback : int(it - list.begin());
}

// ---------------------------------------------------------------------------------------------------------------------
// The live node

class IvyNode final : public Node
{
public:
    enum Attr
    {
        kSurface,
        kSurfaceMatrix,
        kLeaf,
        kLeafWeight,
        kFlower,
        kFlowerWeight,
        kStemPiece,
        kStemPieceWeight,
        kSeeds,
        kStrokes,
        kFromGround,
        kMask,
        kCount,
        kPlant,
        kStyle,
        kSeed,
        kLength,
        kBranchiness,
        kClimb,
        kGravity,
        kAdhesion,
        kSmoothness,
        kStemRadius,
        kProfile,
        kProfilePoints,
        kSides,
        kLeafDensity,
        kLeafSize,
        kLeafSizeJitter,
        kLeafOrient,
        kFlowerDensity,
        kFlowerSize,
        kBrush,
        kOutMesh,
        kStrands,
        kLeaves,
        kFlowers,
        kLongest,
        kWarning,
    };

    void compute(const EvalContext&) override
    {
        LILA_ZONE_N("Ivy node");
        std::vector<std::string> warnings;
        try
        {
            build(warnings);
        }
        catch (const std::exception& e)
        {
            warnings.push_back(e.what());
            setOutput(kOutMesh, MeshPtr(std::make_shared<geo::Mesh>()));
            setOutput(kStrands, int64_t(0));
            setOutput(kLeaves, int64_t(0));
            setOutput(kFlowers, int64_t(0));
            setOutput(kLongest, 0.0);
        }
        std::string text;
        for (const std::string& w : warnings)
            text += (text.empty() ? "" : "\n") + w;
        setOutput(kWarning, std::move(text));
    }

private:
    std::vector<ivy::Piece> pieces(int meshAttr, int weightAttr) const
    {
        std::vector<ivy::Piece> out;
        for (const auto& [index, value] : elements(meshAttr))
        {
            const MeshPtr* m = std::get_if<MeshPtr>(&value);
            if (!m || !*m || (*m)->numFaces() == 0)
                continue;
            const PlugValue* w = element(weightAttr, index);
            out.push_back({**m, w ? std::get<double>(*w) : 1.0});
        }
        return out;
    }

    void build(std::vector<std::string>& warnings)
    {
        // Every host in world space, one surface; points remember where each host's own points start.
        geo::Mesh surface;
        std::map<int, glm::dmat4> matrices;
        for (const auto& [index, value] : elements(kSurfaceMatrix))
            matrices[index] = std::get<glm::dmat4>(value);
        std::vector<float> mask;
        const std::string maskName = as<std::string>(kMask);
        bool maskFound = false;
        for (const auto& [index, value] : elements(kSurface))
        {
            const MeshPtr* m = std::get_if<MeshPtr>(&value);
            if (!m || !*m || (*m)->numFaces() == 0)
                continue;
            geo::Mesh part = **m;
            part.compact();
            const glm::dmat4 xf = matrices.contains(index) ? matrices[index] : glm::dmat4(1.0);
            for (D3& q : part.positions())
                q = D3(xf * glm::dvec4(q, 1.0));
            if (!maskName.empty())
            {
                std::vector<float> w(part.pointCapacity(), 0.0f);
                if (auto pa = part.pointAttrs().get<float>(maskName); !pa.empty())
                {
                    std::copy(pa.begin(), pa.end(), w.begin());
                    maskFound = true;
                }
                else if (geo::colors::has(part, maskName))
                {
                    const std::vector<glm::vec4> c = geo::colors::pointColors(part, maskName);
                    for (size_t k = 0; k < c.size() && k < w.size(); ++k)
                        w[k] = c[k].r;
                    maskFound = true;
                }
                mask.resize(surface.pointCapacity(), 0.0f);
                mask.insert(mask.end(), w.begin(), w.end());
            }
            surface.append(part);
        }
        if (surface.numFaces() == 0)
            throw std::runtime_error("no host mesh connected");

        auto toWorld = [&](int s, const json& p) {
            const D3 q(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
            const glm::dmat4 xf = matrices.contains(s) ? matrices[s] : glm::dmat4(1.0);
            return D3(xf * glm::dvec4(q, 1.0));
        };
        std::vector<D3> seeds;
        const json stored = json::parse(as<std::string>(kSeeds), nullptr, false);
        if (stored.is_array())
            for (const json& s : stored)
                if (s.is_object() && s.contains("p"))
                    seeds.push_back(toWorld(s.value("s", 0), s["p"]));
        std::vector<std::vector<D3>> strokes;
        const json storedStrokes = json::parse(as<std::string>(kStrokes), nullptr, false);
        if (storedStrokes.is_array())
            for (const json& s : storedStrokes)
                if (s.is_object() && s.contains("points"))
                {
                    std::vector<D3> line;
                    for (const json& p : s["points"])
                        line.push_back(toWorld(s.value("s", 0), p));
                    if (line.size() >= 2)
                        strokes.push_back(std::move(line));
                }
        const int count = int(as<int64_t>(kCount));
        const uint32_t seed = uint32_t(as<int64_t>(kSeed));
        if (!maskName.empty())
        {
            if (!maskFound)
                warnings.push_back("no mask attribute or colour set named '" + maskName + "' on the hosts");
            else
            {
                const std::vector<D3> m = ivy::maskSeeds(surface, mask, count, seed);
                if (m.empty())
                    warnings.push_back("the mask '" + maskName + "' is empty");
                seeds.insert(seeds.end(), m.begin(), m.end());
            }
        }
        if (as<bool>(kFromGround))
        {
            const std::vector<D3> gs = ivy::groundSeeds(surface, count, seed);
            if (gs.empty())
                warnings.push_back("no steep faces meet the ground; give seeds or a mask");
            seeds.insert(seeds.end(), gs.begin(), gs.end());
        }

        ivy::Params p;
        p.seed = seed;
        p.length = as<double>(kLength);
        p.branchiness = as<double>(kBranchiness);
        p.climb = as<double>(kClimb);
        p.gravity = as<double>(kGravity);
        p.adhesion = as<double>(kAdhesion);
        p.smoothness = as<double>(kSmoothness);
        p.stemRadius = as<double>(kStemRadius);
        p.profile = ivy::Profile(std::clamp<int64_t>(as<int64_t>(kProfile), 0, 2));
        if (p.profile == ivy::Profile::Custom)
        {
            const json pts = json::parse(as<std::string>(kProfilePoints), nullptr, false);
            if (pts.is_array())
                for (const json& q : pts)
                    if (q.is_array() && q.size() >= 2)
                        p.profilePoints.push_back({q[0].get<double>(), q[1].get<double>()});
            if (p.profilePoints.size() < 3)
            {
                warnings.push_back("the custom stem profile has fewer than three points; stems are round");
                p.profile = ivy::Profile::Round;
            }
        }
        p.sides = int(as<int64_t>(kSides));
        p.leafDensity = as<double>(kLeafDensity);
        p.leafSize = as<double>(kLeafSize);
        p.leafSizeJitter = as<double>(kLeafSizeJitter);
        p.leafOrient = ivy::LeafOrient(std::clamp<int64_t>(as<int64_t>(kLeafOrient), 0, 3));
        p.flowerDensity = as<double>(kFlowerDensity);
        p.flowerSize = as<double>(kFlowerSize);

        ivy::Pieces pc;
        pc.leaves = pieces(kLeaf, kLeafWeight);
        pc.flowers = pieces(kFlower, kFlowerWeight);
        pc.stems = pieces(kStemPiece, kStemPieceWeight);
        const bool toon = as<int64_t>(kStyle) == 1;
        if (pc.leaves.empty() && p.leafDensity > 0.0)
        {
            warnings.push_back("no leaf pieces connected: the built-in leaf shape is used");
            pc.leaves.push_back({ivy::leafCard(toon, 0), 1.0});
        }
        if (pc.flowers.empty() && p.flowerDensity > 0.0)
        {
            warnings.push_back("no flower pieces connected: the built-in flower shape is used");
            pc.flowers.push_back({ivy::flowerCard(toon), 1.0});
        }

        ivy::Result r = ivy::grow(surface, seeds, strokes, p, pc);
        for (std::string& w : r.warnings)
            warnings.push_back(std::move(w));
        double longest = 0.0;
        for (const ivy::Strand& s : r.strands)
            longest = std::max(longest, s.growth.empty() ? 0.0 : s.growth.back());
        setOutput(kStrands, int64_t(r.strands.size()));
        setOutput(kLeaves, int64_t(r.leaves));
        setOutput(kFlowers, int64_t(r.flowers));
        setOutput(kLongest, longest);
        setOutput(kOutMesh, MeshPtr(std::make_shared<geo::Mesh>(std::move(r.mesh))));
    }
};

AttrDef param(std::string name, PlugType type, PlugValue def, std::string doc, std::optional<double> lo = {}, std::optional<double> hi = {})
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc), .minValue = lo, .maxValue = hi};
}

AttrDef choice(std::string name, const std::vector<std::string>& names, std::string doc)
{
    AttrDef d{.name = std::move(name), .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable, .doc = std::move(doc)};
    d.enumNames = names;
    return d;
}

AttrDef hidden(std::string name, PlugType type, PlugValue def, uint32_t extra, std::string doc)
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrHidden | extra, .doc = std::move(doc)};
}

bool registerNode(ext::Context& ctx)
{
    NodeType t;
    t.name = kIvyNodeType;
    t.icon = "plant-2";
    t.doc = "Ivy: grows climbing plants over its host meshes from seed points, a painted mask, the ground contact or brush strokes, "
            "with leaves and flowers from your pieces. Rebuilds when a host changes; the same seed gives the same ivy.";
    t.ownAttrs = {
        hidden("surface", PlugType::Mesh, MeshPtr(), AttrArray, "The hosts' meshes the ivy grows on."),
        hidden("surfaceMatrix", PlugType::Matrix, glm::dmat4(1.0), AttrArray, "The hosts' world matrices."),
        hidden("leaf", PlugType::Mesh, MeshPtr(), AttrArray, "Leaf piece meshes."),
        hidden("leafWeight", PlugType::Float, 1.0, AttrArray | AttrStorable, "How often each leaf piece is picked."),
        hidden("flower", PlugType::Mesh, MeshPtr(), AttrArray, "Flower piece meshes."),
        hidden("flowerWeight", PlugType::Float, 1.0, AttrArray | AttrStorable, "How often each flower piece is picked."),
        hidden("stemPiece", PlugType::Mesh, MeshPtr(), AttrArray, "Stem segment meshes laid along the strands instead of a swept profile."),
        hidden("stemPieceWeight", PlugType::Float, 1.0, AttrArray | AttrStorable, "How often each stem piece is picked."),
        hidden("seeds", PlugType::String, std::string("[]"), AttrStorable, "Seed points in their host's space: [{s: host index, p: [x, y, z]}]."),
        hidden("strokes", PlugType::String, std::string("[]"), AttrStorable, "Brush strokes in their host's space: [{s, points: [[x, y, z], ...]}]."),
        param("fromGround", PlugType::Bool, false, "Also seed where the hosts meet the ground."),
        {.name = "mask", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable,
         .doc = "A painted point attribute or colour set (red) on the hosts: seeds where it is high."},
        param("count", PlugType::Int, int64_t(6), "Seeds taken from the ground contact or the mask.", 0.0, 1000.0),
        {.name = "plant", .type = PlugType::String, .defaultValue = std::string("ivy"), .flags = AttrInput | AttrStorable,
         .doc = "The plant preset it was made with: ivy, vine, roots, moss or tendril."},
        choice("style", kStyles, "Realistic or toon: the built-in leaf shape when no pieces are connected."),
        param("seed", PlugType::Int, int64_t(1), "Random seed: the same seed grows the same ivy."),
        param("length", PlugType::Float, 300.0, "Longest path from a seed, hanging strands included, cm.", 1.0),
        param("branchiness", PlugType::Float, 0.5, "Sparse single strands (0) to dense mats of side shoots (1).", 0.0, 1.0),
        param("climb", PlugType::Float, 0.7, "Spreads every way (0) to heads straight up (1).", 0.0, 1.0),
        param("gravity", PlugType::Float, 0.3, "How much hangs over ledges and droops from overhangs.", 0.0, 1.0),
        param("adhesion", PlugType::Float, 2.0, "Farthest a clinging stem sits from the surface, cm.", 0.01),
        param("smoothness", PlugType::Float, 0.15, "Natural wander (0) to smooth stylized arcs (1).", 0.0, 1.0),
        param("stemRadius", PlugType::Float, 0.6, "Stem radius at the base, cm.", 0.01),
        choice("profile", kProfiles, "Stem cross-section: round, a flat ribbon, or the custom profile curve."),
        hidden("profilePoints", PlugType::String, std::string("[]"), AttrStorable, "Custom profile outline: [[x, y], ...]."),
        param("sides", PlugType::Int, int64_t(5), "Sides of a round stem.", 3.0, 16.0),
        param("leafDensity", PlugType::Float, 30.0, "Leaves per metre of stem.", 0.0),
        param("leafSize", PlugType::Float, 8.0, "Leaf length, cm.", 0.0),
        param("leafSizeJitter", PlugType::Float, 0.35, "How much leaf sizes vary.", 0.0, 1.0),
        choice("leafOrient", kOrients, "Which way leaves point: up and out from the surface, straight up, along the stem, or any way."),
        param("flowerDensity", PlugType::Float, 0.0, "Flowers per metre of stem.", 0.0),
        param("flowerSize", PlugType::Float, 4.0, "Flower size, cm.", 0.0),
        hidden("brush", PlugType::Bool, false, AttrStorable, "Made by the Ivy brush, which adds its strokes here."),
        {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden},
        {.name = "strandCount", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrOutput, .doc = "Strands grown."},
        {.name = "leafCount", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrOutput, .doc = "Leaves placed."},
        {.name = "flowerCount", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrOutput, .doc = "Flowers placed."},
        {.name = "longest", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrOutput, .doc = "Longest growth from a seed, cm."},
        {.name = "warning", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput, .doc = "Problems with the last build."},
    };
    t.create = [] { return std::make_unique<IvyNode>(); };
    if (!types::registerType(ctx, std::move(t)))
        return false;
    relocate::registerWorldAttrs(kIvyNodeType, {.worldOutput = true});
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// The Ivy geometry node

void registerGeometryNode(ext::Context& ctx)
{
    using gn::SocketDecl;
    using gn::SocketType;
    auto geoIn = [](const char* id, const char* doc) { return SocketDecl{.id = id, .type = SocketType::Geometry, .doc = doc}; };
    auto num = [](const char* id, SocketType t, json def, const char* doc, std::optional<double> lo = {}, std::optional<double> hi = {}) {
        return SocketDecl{.id = id, .type = t, .defaultValue = std::move(def), .doc = doc, .min = lo, .max = hi};
    };
    gn::registerDecl(ctx, {
        .type = kIvyGeometryNode,
        .label = "Ivy",
        .category = "Procedural",
        .doc = "Grows ivy (or vines, roots, moss) over a surface from seed points or along curves: climbs, branches, hangs over "
               "ledges, and carries leaves and flowers. Without seeds it starts where the surface meets the ground. Built on Space "
               "Colonization.",
        .props = {{.id = "style", .options = kStyles, .defaultValue = "realistic", .doc = "The built-in leaf shape when no leaves are given."},
                  {.id = "profile", .options = {"round", "ribbon"}, .defaultValue = "round", .doc = "Stem cross-section."},
                  {.id = "leafOrient", .options = kOrients, .defaultValue = "surface", .doc = "Which way leaves point."}},
        .sockets = {.inputs = {geoIn("surface", "What it grows on (a mesh)."), geoIn("seeds", "Points it starts from."),
                               geoIn("strokes", "Curves it grows along."),
                               geoIn("leaves", "Leaf pieces: a mesh, or instances each of a piece. Built-in leaves when empty."),
                               geoIn("flowers", "Flower pieces, as leaves."), num("seed", SocketType::Int, 1, "Random seed."),
                               num("count", SocketType::Int, 6, "Seeds from the ground contact when no seeds are given.", 0, 1000),
                               num("length", SocketType::Float, 300.0, "Longest path from a seed, cm.", 1.0),
                               num("branchiness", SocketType::Float, 0.5, "Sparse (0) to dense (1).", 0.0, 1.0),
                               num("climb", SocketType::Float, 0.7, "Every way (0) to straight up (1).", 0.0, 1.0),
                               num("gravity", SocketType::Float, 0.3, "Hanging over ledges and overhangs.", 0.0, 1.0),
                               num("adhesion", SocketType::Float, 2.0, "Farthest a clinging stem sits from the surface, cm.", 0.01),
                               num("smoothness", SocketType::Float, 0.15, "Natural wander (0) to smooth arcs (1).", 0.0, 1.0),
                               num("stemRadius", SocketType::Float, 0.6, "cm.", 0.01),
                               num("leafDensity", SocketType::Float, 30.0, "Leaves per metre.", 0.0),
                               num("leafSize", SocketType::Float, 8.0, "cm.", 0.0),
                               num("flowerDensity", SocketType::Float, 0.0, "Flowers per metre.", 0.0),
                               num("flowerSize", SocketType::Float, 4.0, "cm.", 0.0)},
                    .outputs = {SocketDecl{.id = "geometry", .type = SocketType::Geometry, .doc = "Stems, leaves and flowers, a mesh."},
                                SocketDecl{.id = "stems", .type = SocketType::Geometry,
                                           .doc = "The strands as curves with radius, growth (cm from the seed) and hang (1 hanging free)."}}},
        .exec =
            [](gn::ExecContext& c) {
                LILA_ZONE_N("Ivy geometry node");
                const gn::GeometryPtr surf = c.geometry("surface");
                if (!surf->mesh || surf->mesh->numFaces() == 0)
                    throw gn::ExecError("Ivy: connect a mesh to grow on");
                auto positions = [](const gn::Geometry& g) {
                    std::vector<D3> pts;
                    for (const auto& m : {g.mesh, g.points})
                        if (m)
                            for (geo::Index p = 0; p < m->pointCapacity(); ++p)
                                if (m->pointValid(p))
                                    pts.push_back(m->position(p));
                    return pts;
                };
                auto piecesOf = [](const gn::Geometry& g) {
                    std::vector<ivy::Piece> out;
                    if (g.instances)
                        for (const gn::GeometryPtr& ref : g.instances->references)
                            if (ref && ref->mesh && ref->mesh->numFaces() > 0)
                                out.push_back({*ref->mesh, 1.0});
                    if (g.mesh && g.mesh->numFaces() > 0)
                        out.push_back({*g.mesh, 1.0});
                    return out;
                };
                const bool toon = c.prop("style") == "toon";
                ivy::Params p;
                p.seed = uint32_t(c.i("seed"));
                p.length = c.f("length");
                p.branchiness = c.f("branchiness");
                p.climb = c.f("climb");
                p.gravity = c.f("gravity");
                p.adhesion = c.f("adhesion");
                p.smoothness = c.f("smoothness");
                p.stemRadius = c.f("stemRadius");
                p.leafDensity = c.f("leafDensity");
                p.leafSize = c.f("leafSize");
                p.flowerDensity = c.f("flowerDensity");
                p.flowerSize = c.f("flowerSize");
                p.profile = c.prop("profile") == "ribbon" ? ivy::Profile::Ribbon : ivy::Profile::Round;
                p.leafOrient = ivy::LeafOrient(std::max(0, indexIn(kOrients, c.prop("leafOrient"))));
                p.sides = toon ? 6 : 5;
                std::vector<D3> seeds = positions(*c.geometry("seeds"));
                std::vector<std::vector<D3>> strokes;
                if (const gn::GeometryPtr st = c.geometry("strokes"); st->curves)
                {
                    const gn::CurveSet ev = st->curves->evaluated();
                    for (size_t k = 0; k < ev.numCurves(); ++k)
                    {
                        const auto pts = ev.curvePoints(k);
                        strokes.emplace_back(pts.begin(), pts.end());
                    }
                }
                if (seeds.empty() && strokes.empty())
                    seeds = ivy::groundSeeds(*surf->mesh, int(c.i("count")), p.seed);
                ivy::Pieces pc;
                pc.leaves = piecesOf(*c.geometry("leaves"));
                pc.flowers = piecesOf(*c.geometry("flowers"));
                if (pc.leaves.empty())
                    pc.leaves.push_back({ivy::leafCard(toon, 0), 1.0});
                if (pc.flowers.empty())
                    pc.flowers.push_back({ivy::flowerCard(toon), 1.0});
                ivy::Result r = ivy::grow(*surf->mesh, seeds, strokes, p, pc);
                gn::CurveSet cs;
                std::vector<double> radius, growth, hang;
                for (const ivy::Strand& s : r.strands)
                {
                    cs.addCurve(s.points, false);
                    radius.insert(radius.end(), s.radius.begin(), s.radius.end());
                    growth.insert(growth.end(), s.growth.begin(), s.growth.end());
                    for (uint8_t h : s.hang)
                        hang.push_back(h);
                }
                gn::writeAttribute(cs, "radius", gn::Domain::Point, gn::Column::of(std::move(radius)), SocketType::Float);
                gn::writeAttribute(cs, "growth", gn::Domain::Point, gn::Column::of(std::move(growth)), SocketType::Float);
                gn::writeAttribute(cs, "hang", gn::Domain::Point, gn::Column::of(std::move(hang)), SocketType::Float);
                c.out("geometry", gn::meshGeometry(std::move(r.mesh)));
                c.out("stems", gn::curveGeometry(std::move(cs)));
            },
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// Commands

Node& nodeOf(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    return *n;
}

/** The ivy node behind a name: the node itself, or the mesh it feeds. */
Node* ivyNodeOf(Graph& g, const std::string& name)
{
    Node* n = g.find(name);
    if (!n)
        return nullptr;
    if (n->type().name == kIvyNodeType)
        return n;
    if (n->type().isA("mesh"))
        if (auto src = g.source({n->id(), MeshNode::kInMesh, -1}))
            if (Node* s = g.node(src->node); s && s->type().name == kIvyNodeType)
                return s;
    return nullptr;
}

/** The mesh an ivy node feeds. */
Node* ivyMeshOf(Graph& g, const Node& ivyNode)
{
    for (const PlugRef& d : g.destinations({ivyNode.id(), IvyNode::kOutMesh, -1}))
        if (Node* m = g.node(d.node); m && m->type().isA("mesh"))
            return m;
    return nullptr;
}

std::vector<FlagDef> tuningFlags()
{
    auto f = [](const char* n, const char* s, T t, const char* doc, std::optional<double> lo = {}, std::optional<double> hi = {}) {
        FlagDef d{n, s, t, nullptr, doc};
        d.min = lo;
        d.max = hi;
        return d;
    };
    std::vector<FlagDef> flags = {
        {"plant", "pl", T::String, "ivy",
         "Preset the other settings start from: ivy (climbing, leafy), vine (long, drapes), roots (bare, spreads every way), "
         "moss (flat dense strands) or tendril (smooth curling arcs).",
         kPlants},
        styleFlag(),
        {"seed", "sd", T::Int, 1, "Random seed: the same seed grows the same ivy."},
        f("length", "len", T::Float, "Longest path from a seed, hanging strands included, cm.", 1.0),
        f("branchiness", "br", T::Float, "Sparse single strands (0) to dense mats of side shoots (1).", 0.0, 1.0),
        f("climb", "cl", T::Float, "Spreads every way (0) to heads straight up (1).", 0.0, 1.0),
        f("gravity", "gr", T::Float, "How much hangs over ledges and droops from overhangs, 0 to 1.", 0.0, 1.0),
        f("adhesion", "ad", T::Float, "Farthest a clinging stem sits from the surface, cm (default 2).", 0.01),
        f("smoothness", "sm", T::Float, "Curve smoothness: natural wander (0) to smooth stylized arcs (1). Toon starts at 0.85.", 0.0, 1.0),
        f("stemRadius", "sr", T::Float, "Stem radius at the base, cm.", 0.01),
        {"profile", "pr", T::String, nullptr, "Stem cross-section: round, ribbon (flat on the surface), or the name of a curve whose shape is the profile."},
        f("leafDensity", "ld", T::Float, "Leaves per metre of stem; 0 for bare stems.", 0.0),
        f("leafSize", "ls", T::Float, "Leaf length, cm.", 0.0),
        f("leafSizeJitter", "lj", T::Float, "How much leaf sizes vary, 0 to 1.", 0.0, 1.0),
        {"leafOrient", "lo", T::String, nullptr, "Which way leaves point: surface (up and out, splayed), up, stem (along it) or random.", kOrients},
        f("flowerDensity", "fd", T::Float, "Flowers per metre of stem.", 0.0),
        f("flowerSize", "fs", T::Float, "Flower size, cm.", 0.0),
        piecesFlag("leaves", "lv",
                   "Leaf pieces: meshes, groups, sets, library ids, tag:<word> or {piece, weight}. Laid out like a Foliage leaf card "
                   "(along +Z from the stem end, face +Y) and fitted by their bounds. Default: library assets tagged weathering and "
                   "the plant's leaf role (ivy-leaf, vine-leaf, moss-tuft)."),
        piecesFlag("flowers", "fl", "Flower pieces, as leaves; default the ivy-flower role. Used when flowerDensity is above 0."),
        piecesFlag("stems", "st", "Stem segment pieces laid along the strands (along +Z) instead of a swept profile."),
    };
    return flags;
}

/** Settings from the plant preset, the style, then the flags given. */
json settingsFrom(const Args& a)
{
    const Plant& pl = plantNamed(a.get<std::string>("plant", "ivy"));
    const bool toon = styleArg(a) == Style::Toon;
    json s = {{"length", pl.length},           {"branchiness", pl.branchiness}, {"climb", pl.climb},       {"gravity", pl.gravity},
              {"adhesion", 2.0},               {"smoothness", toon ? std::max(0.85, pl.smoothness) : pl.smoothness},
              {"stemRadius", pl.stemRadius},   {"profile", pl.profile},         {"leafDensity", pl.leafDensity},
              {"leafSize", toon ? pl.leafSize * 1.4 : pl.leafSize},            {"leafSizeJitter", toon ? 0.2 : 0.35},
              {"leafOrient", pl.orient},       {"flowerDensity", pl.flowerDensity}, {"flowerSize", 4.0}};
    for (auto& [k, v] : s.items())
        if (a.has(k))
            v = a.flags[k];
    s["plant"] = pl.name;
    s["leafRole"] = pl.leafRole;
    s["style"] = toon ? "toon" : "realistic";
    s["seed"] = a.get<int64_t>("seed", 1);
    return s;
}

/** A curve's shape as a stem profile: its points in its own XY plane. */
json profileFromCurve(Graph& g, const std::string& name)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError("ivy: profile is round, ribbon or a curve; no node named '" + name + "'");
    if (!n->type().isA("curve"))
        throw CommandError("ivy: the profile " + n->path() + " is not a curve");
    g.evaluate();
    const curve::Splines& sp = curve::splinesOf(g.value({n->id(), curve::CurveShape::kLocal, -1}));
    json out = json::array();
    if (!sp.empty())
        for (const D3& q : curve::tessellate(sp.front()).points)
            out.push_back({q.x, q.y});
    if (out.size() < 3)
        throw CommandError("ivy: the profile curve " + n->path() + " needs at least three points");
    return out;
}

void setAttr(Graph& g, Node& n, const char* attr, PlugValue v) { g.setValue({n.id(), n.type().attrIndex(attr), -1}, std::move(v)); }

/** Writes settings onto an ivy node. */
void applySettings(Graph& g, Node& n, const json& s)
{
    for (const char* k : {"length", "branchiness", "climb", "gravity", "adhesion", "smoothness", "stemRadius", "leafDensity", "leafSize",
                          "leafSizeJitter", "flowerDensity", "flowerSize"})
        if (s.contains(k))
            setAttr(g, n, k, s[k].get<double>());
    if (s.contains("seed"))
        setAttr(g, n, "seed", s["seed"].get<int64_t>());
    if (s.contains("plant"))
        setAttr(g, n, "plant", s["plant"].get<std::string>());
    if (s.contains("style"))
    {
        const bool toon = s["style"] == "toon";
        setAttr(g, n, "style", int64_t(toon ? 1 : 0));
        setAttr(g, n, "sides", int64_t(toon ? 6 : 5));
    }
    if (s.contains("leafOrient"))
        setAttr(g, n, "leafOrient", int64_t(std::max(0, indexIn(kOrients, s["leafOrient"].get<std::string>()))));
    if (s.contains("profile"))
    {
        const std::string pr = s["profile"].get<std::string>();
        if (pr == "round" || pr == "ribbon")
            setAttr(g, n, "profile", int64_t(pr == "ribbon" ? 1 : 0));
        else
        {
            setAttr(g, n, "profile", int64_t(2));
            setAttr(g, n, "profilePoints", profileFromCurve(g, pr).dump());
        }
    }
}

void connectPieces(Graph& g, Node& n, const std::vector<Piece>& pieces, const char* meshAttr, const char* weightAttr)
{
    const int ma = n.type().attrIndex(meshAttr), wa = n.type().attrIndex(weightAttr);
    for (const auto& [index, value] : std::vector<std::pair<int, PlugValue>>(n.elements(ma)))
    {
        (void)value;
        g.disconnect({n.id(), ma, -1, index});
    }
    int k = 0;
    for (const Piece& p : pieces)
    {
        Node* m = g.node(p.mesh);
        if (!m)
            continue;
        g.connect({p.mesh, m->type().attrIndex("outMesh"), -1}, {n.id(), ma, -1, k}, true);
        g.setValue({n.id(), wa, -1, k}, p.weight);
        ++k;
    }
}

/** Which host (surface index) a world point lies on, and the point in that host's space. */
std::pair<int, D3> onHost(Graph& g, const std::vector<NodeId>& hosts, const D3& world)
{
    int best = 0;
    double bestD = std::numeric_limits<double>::max();
    D3 local = world;
    for (size_t k = 0; k < hosts.size(); ++k)
    {
        Node* h = g.node(hosts[k]);
        const auto m = h->as<MeshPtr>(MeshNode::kOutMesh);
        if (!m || m->numFaces() == 0)
            continue;
        const glm::dmat4 xf = h->as<glm::dmat4>(Transform::kWorldMatrix);
        const D3 l = D3(glm::inverse(xf) * glm::dvec4(world, 1.0));
        geo::MeshBvh bvh(*m);
        geo::BvhClosest c;
        if (!bvh.closestPoint(l, c))
            continue;
        const double d = glm::distance(D3(xf * glm::dvec4(c.point, 1.0)), world);
        if (d < bestD)
        {
            bestD = d;
            best = int(k);
            local = l;
        }
    }
    return {best, local};
}

D3 vec3Of(const json& j, const char* cmd)
{
    if (!j.is_array() || j.size() < 3)
        throw CommandError(std::string(cmd) + ": a point is [x, y, z]");
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

std::vector<NodeId> hostsOf(Graph& g, const Node& ivyNode)
{
    std::vector<NodeId> out;
    for (const auto& [index, value] : ivyNode.elements(IvyNode::kSurface))
    {
        (void)value;
        if (auto src = g.source({ivyNode.id(), IvyNode::kSurface, -1, index}))
            out.push_back(src->node);
    }
    return out;
}

json infoOf(Graph& g, Node& n)
{
    g.evaluate();
    Node* mesh = ivyMeshOf(g, n);
    json hosts = json::array();
    for (NodeId h : hostsOf(g, n))
        hosts.push_back(g.node(h)->path());
    return {{"node", n.path()},
            {"mesh", mesh ? json(mesh->path()) : json()},
            {"hosts", hosts},
            {"plant", n.as<std::string>(IvyNode::kPlant)},
            {"strands", n.as<int64_t>(IvyNode::kStrands)},
            {"leaves", n.as<int64_t>(IvyNode::kLeaves)},
            {"flowers", n.as<int64_t>(IvyNode::kFlowers)},
            {"longest", n.as<double>(IvyNode::kLongest)},
            {"length", n.as<double>(IvyNode::kLength)},
            {"seeds", json::parse(n.as<std::string>(IvyNode::kSeeds), nullptr, false).size()},
            {"strokes", json::parse(n.as<std::string>(IvyNode::kStrokes), nullptr, false).size()},
            {"warning", n.as<std::string>(IvyNode::kWarning)}};
}

/** Makes one ivy node on the hosts, its mesh in a group; returns the node. */
Node& makeIvy(Graph& g, const std::vector<NodeId>& hosts, const Args& a, const json& settings, const std::string& groupPath, const char* cmd)
{
    const std::string base = g.node(hosts[0])->name();
    Node* n = g.createNode(*types::find(kIvyNodeType), base + "Ivy");
    for (size_t k = 0; k < hosts.size(); ++k)
    {
        g.connect({hosts[k], MeshNode::kOutMesh, -1}, {n->id(), IvyNode::kSurface, -1, int(k)}, true);
        g.connect({hosts[k], Transform::kWorldMatrix, -1}, {n->id(), IvyNode::kSurfaceMatrix, -1, int(k)}, true);
    }
    applySettings(g, *n, settings);
    const Style style = settings["style"] == "toon" ? Style::Toon : Style::Realistic;
    const std::string role = settings["leafRole"].get<std::string>();
    if (a.has("leaves") || (!role.empty() && settings["leafDensity"].get<double>() > 0.0))
        connectPieces(g, *n, piecesOrDefault(g, a, "leaves", role.empty() ? "ivy-leaf" : role, style, cmd), "leaf", "leafWeight");
    if (a.has("flowers") || settings["flowerDensity"].get<double>() > 0.0)
        connectPieces(g, *n, piecesOrDefault(g, a, "flowers", "ivy-flower", style, cmd), "flower", "flowerWeight");
    if (a.has("stems"))
        connectPieces(g, *n, resolvePieces(g, a.flags["stems"], cmd), "stemPiece", "stemPieceWeight");
    Node* mesh = g.createNode(*types::find("mesh"), base + "_ivy");
    g.connect({n->id(), IvyNode::kOutMesh, -1}, {mesh->id(), MeshNode::kInMesh, -1}, true);
    ops::execute("parent", {{"args", {mesh->path(), groupPath}}});
    return *n;
}

/** Bakes an ivy node's mesh into its mesh node and deletes the node. */
void realizeIvy(Graph& g, Node& n)
{
    g.evaluate();
    Node* mesh = ivyMeshOf(g, n);
    const MeshPtr out = n.as<MeshPtr>(IvyNode::kOutMesh);
    if (mesh)
    {
        g.disconnect({mesh->id(), MeshNode::kInMesh, -1});
        g.setValue({mesh->id(), MeshNode::kInMesh, -1}, out ? out : MeshPtr(std::make_shared<geo::Mesh>()));
    }
    g.deleteNode(n.id());
}

std::vector<NodeId> meshHosts(Graph& g, const std::vector<std::string>& names, const char* cmd)
{
    std::vector<NodeId> hosts;
    for (const std::string& name : names)
    {
        Node& n = nodeOf(g, name, cmd);
        if (!n.type().isA("mesh"))
            throw CommandError(std::string(cmd) + ": " + n.path() + " is not a mesh");
        if (std::ranges::find(hosts, n.id()) == hosts.end())
            hosts.push_back(n.id());
    }
    return hosts;
}

json strokeOf(Graph& g, const std::vector<NodeId>& hosts, const json& points, const char* cmd)
{
    if (!points.is_array() || points.size() < 2)
        throw CommandError(std::string(cmd) + ": a stroke is a list of at least two [x, y, z] points");
    const auto [s, first] = onHost(g, hosts, vec3Of(points[0], cmd));
    (void)first;
    const glm::dmat4 inv = glm::inverse(g.node(hosts[size_t(s)])->as<glm::dmat4>(Transform::kWorldMatrix));
    json local = json::array();
    for (const json& p : points)
    {
        const D3 l = D3(inv * glm::dvec4(vec3Of(p, cmd), 1.0));
        local.push_back({l.x, l.y, l.z});
    }
    return {{"s", s}, {"points", local}};
}

void registerCommands(ext::Context& ctx)
{
    std::vector<FlagDef> ivyFlags = {
        {"seeds", "sp", T::Any, nullptr, "World points the ivy starts from, [[x, y, z], ...]; each sticks to the host it lies on."},
        {"strokes", "sk", T::Any, nullptr, "World polylines the ivy grows along, [[[x, y, z], ...], ...], as the Ivy brush paints them."},
        {"mask", "mk", T::String, nullptr, "A painted point attribute or colour set (red) on the hosts: seeds where it is high."},
        {"ground", "gd", T::Bool, nullptr, "Seed where the hosts meet the ground. Default on when no seeds, strokes or mask are given."},
        {"count", "c", T::Int, 6, "Seeds taken from the ground contact or the mask.", {}, {}, 0.0, 1000.0},
        {"surfaces", "sf", T::Any, nullptr, "More meshes the ivy may climb onto (the ground, a neighbouring wall)."},
        {"name", "n", T::String, "Ivy", "The group holding the ivy meshes."},
        {"realize", "rz", T::Bool, false, "Bake to a plain mesh instead of staying live."},
    };
    for (FlagDef& f : tuningFlags())
        ivyFlags.push_back(std::move(f));

    ctx.registerCommand({
        .name = "ivy",
        .doc = "Grow ivy over meshes: strands climb from seed points, a painted mask, brush strokes or where the meshes meet the "
               "ground; they branch, follow the surface into crevices and round edges, hang over ledges and droop from overhangs, "
               "and carry leaves and flowers from your pieces (library assets tagged weathering and ivy-leaf, vine-leaf, "
               "moss-tuft or ivy-flower until you give your own). Any growing plant: plant=vine, roots, moss or tendril.\n\n"
               "Live: one ivy node per call, fed by the hosts' meshes and transforms, so the ivy regrows when a host is edited, "
               "deterministically for its seed. Change it afterwards with setAttr on the node (length, gravity, smoothness...). "
               "Returns {group, ivy: [meshes], nodes: [ivy nodes], strands, leaves, warnings}.",
        .positionalDoc = "host meshes (default the selection)",
        .flags = ivyFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE_N("ivy command");
            Graph& g = Graph::current();
            std::vector<NodeId> hosts = meshHosts(g, a.objects(), "ivy");
            if (hosts.empty())
                throw CommandError("ivy: select or name a mesh to grow on");
            if (a.has("surfaces"))
            {
                std::vector<std::string> extra;
                const json s = a.flags["surfaces"];
                if (s.is_string())
                    extra.push_back(s.get<std::string>());
                else if (s.is_array())
                    for (const json& x : s)
                        extra.push_back(x.get<std::string>());
                for (NodeId h : meshHosts(g, extra, "ivy"))
                    if (std::ranges::find(hosts, h) == hosts.end())
                        hosts.push_back(h);
            }
            g.evaluate();
            const json settings = settingsFrom(a);
            const json group = ops::execute("group", {{"empty", true}, {"name", a.get<std::string>("name", "Ivy")}});
            const std::string groupPath = group.is_array() ? group[0].get<std::string>() : group.get<std::string>();
            Node& n = makeIvy(g, hosts, a, settings, groupPath, "ivy");
            json seeds = json::array(), strokes = json::array();
            if (a.has("seeds"))
                for (const json& p : a.flags["seeds"])
                {
                    const auto [s, local] = onHost(g, hosts, vec3Of(p, "ivy"));
                    seeds.push_back({{"s", s}, {"p", {local.x, local.y, local.z}}});
                }
            if (a.has("strokes"))
                for (const json& st : a.flags["strokes"])
                    strokes.push_back(strokeOf(g, hosts, st, "ivy"));
            setAttr(g, n, "seeds", seeds.dump());
            setAttr(g, n, "strokes", strokes.dump());
            setAttr(g, n, "count", a.get<int64_t>("count", 6));
            if (a.has("mask"))
                setAttr(g, n, "mask", a.get<std::string>("mask"));
            const bool ground = a.has("ground") ? a.get<bool>("ground") : seeds.empty() && strokes.empty() && !a.has("mask");
            setAttr(g, n, "fromGround", ground);
            json info = infoOf(g, n);
            json out = {{"group", groupPath},
                        {"ivy", json::array({info["mesh"]})},
                        {"nodes", json::array({n.path()})},
                        {"strands", info["strands"]},
                        {"leaves", info["leaves"]},
                        {"warnings", info["warning"].get<std::string>().empty() ? json::array() : json::array({info["warning"]})}};
            for (const json& w : out["warnings"])
                log::warn("ivy: {}", w.get<std::string>());
            if (a.get<bool>("realize", false))
            {
                realizeIvy(g, n);
                out["nodes"] = json::array();
            }
            return out;
        },
        .examples = {"ivy('Wall')", "ivy('Wall', seeds=[[0, 0, 50]], length=400, gravity=0.6)",
                     "ivy('Tower', plant='vine', style='toon', leaves=['MyLeafA', {'piece': 'MyLeafB', 'weight': 0.3}])",
                     "ivy('Ruin', mask='ivyMask', count=12, flowerDensity=4)", "ivy('Pillar', plant='roots', climb=0.0, profile='RootProfile')"},
    });

    ctx.registerCommand({
        .name = "ivyPaint",
        .doc = "Paint ivy: grow it along a stroke on a mesh, as the Ivy brush does. Strokes collect on the host's brush ivy (made on "
               "the first stroke with these settings), so they regrow together. erase=True removes strokes and seeds within radius of "
               "the points instead. Returns what ivy returns.",
        .positionalDoc = "host mesh",
        .flags = [] {
            std::vector<FlagDef> f = {{"points", "p", T::Any, nullptr, "The stroke, world points [[x, y, z], ...]."},
                                      {"erase", "e", T::Bool, false, "Remove strokes and seeds near the points."},
                                      {"radius", "r", T::Float, 20.0, "Erase radius, cm.", {}, {}, 0.0},
                                      {"name", "n", T::String, "Ivy", "The group a new brush ivy goes in."}};
            for (FlagDef& d : tuningFlags())
                f.push_back(std::move(d));
            return f;
        }(),
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<NodeId> hosts = meshHosts(g, a.objects(), "ivyPaint");
            if (hosts.size() != 1)
                throw CommandError("ivyPaint: give one host mesh");
            if (!a.has("points") || !a.flags["points"].is_array())
                throw CommandError("ivyPaint: give points=[[x, y, z], ...]");
            g.evaluate();
            Node* n = nullptr;
            for (const PlugRef& d : g.destinations({hosts[0], MeshNode::kOutMesh, -1}))
                if (Node* c = g.node(d.node); c && c->type().name == kIvyNodeType && c->as<bool>(IvyNode::kBrush))
                    n = c;
            if (a.get<bool>("erase", false))
            {
                if (!n)
                    return {{"erased", 0}};
                const double r = a.get<double>("radius", 20.0);
                std::vector<D3> at;
                for (const json& p : a.flags["points"])
                    at.push_back(vec3Of(p, "ivyPaint"));
                const std::vector<NodeId> hs = hostsOf(g, *n);
                auto near = [&](int s, const json& p) {
                    const D3 w = D3(g.node(hs[size_t(std::clamp(s, 0, int(hs.size()) - 1))])->as<glm::dmat4>(Transform::kWorldMatrix) *
                                    glm::dvec4(vec3Of(p, "ivyPaint"), 1.0));
                    return std::ranges::any_of(at, [&](const D3& q) { return glm::distance(q, w) < r; });
                };
                int erased = 0;
                json strokes = json::array();
                for (const json& st : json::parse(n->as<std::string>(IvyNode::kStrokes)))
                {
                    // Erasing through a stroke splits it; pieces too short to grow go.
                    json piece = json::array();
                    auto flush = [&] {
                        if (piece.size() >= 2)
                            strokes.push_back({{"s", st["s"]}, {"points", piece}});
                        piece = json::array();
                    };
                    for (const json& p : st["points"])
                    {
                        if (near(st.value("s", 0), p))
                        {
                            ++erased;
                            flush();
                        }
                        else
                            piece.push_back(p);
                    }
                    flush();
                }
                json seeds = json::array();
                for (const json& s : json::parse(n->as<std::string>(IvyNode::kSeeds)))
                    if (near(s.value("s", 0), s["p"]))
                        ++erased;
                    else
                        seeds.push_back(s);
                setAttr(g, *n, "strokes", strokes.dump());
                setAttr(g, *n, "seeds", seeds.dump());
                json info = infoOf(g, *n);
                info["erased"] = erased;
                return info;
            }
            std::string groupPath;
            if (!n)
            {
                const json group = ops::execute("group", {{"empty", true}, {"name", a.get<std::string>("name", "Ivy")}});
                groupPath = group.is_array() ? group[0].get<std::string>() : group.get<std::string>();
                n = &makeIvy(g, hosts, a, settingsFrom(a), groupPath, "ivyPaint");
                setAttr(g, *n, "brush", true);
            }
            else
            {
                if (Node* m = ivyMeshOf(g, *n); m && g.parent(m->id()) != kNoNode)
                    groupPath = g.node(g.parent(m->id()))->path();
                // Later strokes keep the ivy's plant, style and seed; only tuning flags given again change it.
                json s = settingsFrom(a);
                for (const char* k : {"plant", "style", "leafRole", "seed"})
                    s.erase(k);
                for (auto it = s.begin(); it != s.end();)
                    it = a.has(it.key()) ? std::next(it) : s.erase(it);
                applySettings(g, *n, s);
            }
            json strokes = json::parse(n->as<std::string>(IvyNode::kStrokes));
            strokes.push_back(strokeOf(g, hostsOf(g, *n), a.flags["points"], "ivyPaint"));
            setAttr(g, *n, "strokes", strokes.dump());
            json info = infoOf(g, *n);
            return {{"group", groupPath}, {"ivy", json::array({info["mesh"]})}, {"nodes", json::array({n->path()})},
                    {"strands", info["strands"]}, {"leaves", info["leaves"]}, {"strokes", info["strokes"]},
                    {"warnings", info["warning"].get<std::string>().empty() ? json::array() : json::array({info["warning"]})}};
        },
        .examples = {"ivyPaint('Wall', points=[[0, 10, 50], [40, 120, 50], [90, 200, 50]])",
                     "ivyPaint('Wall', points=[[40, 120, 50]], erase=True, radius=30)"},
    });

    ctx.registerCommand({
        .name = "ivyInfo",
        .doc = "An ivy's state: its mesh, hosts, plant, strand, leaf and flower counts, the longest growth (cm), seeds, strokes and "
               "warnings.",
        .positionalDoc = "ivy node or its mesh",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            json out = json::array();
            for (const std::string& name : a.objects())
                if (Node* n = ivyNodeOf(g, name))
                    out.push_back(infoOf(g, *n));
            if (out.empty())
                throw CommandError("ivyInfo: give an ivy node or the mesh it feeds");
            return out.size() == 1 ? out[0] : out;
        },
    });

    ctx.registerCommand({
        .name = "ivySend",
        .doc = "Send an ivy to Unreal's open level as its own static mesh (stems, leaf and flower cards) with the foliage material: "
               "M_LilaIvy on MF_LilaFoliageWind, which sways leaves and hanging strands by the mesh's wind colours (R sway, G phase, "
               "B leaf). Drop MF_LilaFoliageWind into your own foliage materials' World Position Offset to use your own. "
               "Returns {mesh, contentPath, materials, warnings}.",
        .positionalDoc = "ivy node or its mesh",
        .flags = {{"contentPath", "cp", T::String, nullptr, "The static mesh asset, like /Game/Weathering/SM_WallIvy (default /Game/LilaLink/Ivy/SM_<mesh>)."},
                  {"nanite", "na", T::Bool, false, "Build Nanite data (Nanite foliage on UE 5.7+)."},
                  {"material", "mt", T::String, "generate", "generate (M_LilaIvy instances on every slot), none (keep the import's), or a parent material to instance."},
                  {"windStrength", "ws", T::Float, 1.0, "Wind sway on the generated instances, 0 for none.", {}, {}, 0.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("ivySend: give one ivy");
            Node* mesh = nullptr;
            if (Node* n = ivyNodeOf(g, names[0]))
                mesh = ivyMeshOf(g, *n);
            else if (Node* m = g.find(names[0]); m && m->type().isA("mesh"))
                mesh = m;
            if (!mesh)
                throw CommandError("ivySend: " + names[0] + " is not an ivy or a mesh");
            g.evaluate();
            const std::string cp = a.get<std::string>("contentPath", "/Game/LilaLink/Ivy/SM_" + mesh->name());
            const std::filesystem::path file = link::cacheDir() / (mesh->name() + "_ivy.fbx");
            std::error_code ec;
            std::filesystem::create_directories(file.parent_path(), ec);
            std::string err;
            if (!fbx::exportForUnreal({mesh->id()}, file, "ue5", &err, "static", false))
                throw CommandError("ivySend: writing " + file.string() + " failed: " + err);
            try
            {
                link::Link::get().ensureConnected();
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("ivySend: not connected to Unreal: ") + e.what());
            }
            std::shared_ptr<link::RpcClient> client = link::Link::get().client();
            if (!client)
                throw CommandError("ivySend: not connected to Unreal");
            const std::string material = a.get<std::string>("material", "generate");
            json slots = json::array();
            if (const MeshPtr m = mesh->as<MeshPtr>(MeshNode::kOutMesh))
                for (const auto& [slot, faces] : materials::slotUsage(*m))
                    slots.push_back(slot);
            json mats = json::array();
            for (const json& s : slots)
                mats.push_back(link::materialInputs(g, s.get<std::string>()));
            json mapping = {{"contentPath", cp}, {"kind", "static"}, {"nanite", a.get<bool>("nanite", false)}, {"collision", "none"},
                            {"existing", "reimport"}, {"reapplyOptions", true}};
            if (material != "none")
                mapping["importMaterials"] = false;
            link::RpcResult r = client->call("importAsset", link::importParams(file, mapping, mats), link::importCallOptions());
            if (!r.ok)
                throw CommandError("ivySend: " + r.error);
            json warnings = r.result.value("warnings", json::array());
            for (const json& e : r.result.value("errors", json::array()))
                warnings.push_back(e);
            std::string objectPath = cp;
            if (json made = r.result.value("assets", json::array()); made.is_array() && !made.empty() && made[0].is_string())
                objectPath = made[0].get<std::string>();
            json placed = client->call("placeInstances", {{"group", "LilaIvy_" + mesh->name()},
                                                          {"label", mesh->name()},
                                                          {"mesh", objectPath},
                                                          {"mode", "actors"},
                                                          {"placements", json::array({{{"id", mesh->name()},
                                                                                       {"name", mesh->name()},
                                                                                       {"location", {0.0, 0.0, 0.0}},
                                                                                       {"rotation", {0.0, 0.0, 0.0, 1.0}},
                                                                                       {"scale", {1.0, 1.0, 1.0}}}})}},
                                       link::placementTimeoutMs(1))
                              .result;
            json materialsMade = json::object();
            if (material != "none")
            {
                const json features = link::Link::get().status().value("features", json::array());
                if (!features.is_array() || std::ranges::find(features, json("ivyMaterials")) == features.end())
                    warnings.push_back("the LilaLink plugin in this editor is too old for ivy materials; rebuild it from Lila's ue/LilaLink, "
                                       "or send with material='none'");
                else
                {
                    json instances = json::array();
                    for (const json& s : slots)
                    {
                        const std::string slot = s.get<std::string>();
                        const json in = link::materialInputs(g, slot);
                        // A slot with no Lila material gets a plant colour; one with a material sends its colour and map.
                        glm::dvec3 tint = slot == ivy::kStemSlot ? glm::dvec3(0.22, 0.15, 0.09)
                                                                 : (slot == ivy::kFlowerSlot ? glm::dvec3(0.9, 0.85, 0.6) : glm::dvec3(0.12, 0.26, 0.05));
                        if (in.contains("baseColor") && in["baseColor"].is_array() && in["baseColor"].size() >= 3)
                            tint = {in["baseColor"][0].get<double>(), in["baseColor"][1].get<double>(), in["baseColor"][2].get<double>()};
                        json inst = {{"name", "MI_" + mesh->name() + "_" + slot},
                                     {"slot", slot},
                                     {"scalars", {{"WindStrength", a.get<double>("windStrength", 1.0)}, {"Roughness", in.value("roughness", 0.6)}}},
                                     {"vectors", {{"Tint", {tint.x, tint.y, tint.z}}}}};
                        if (in.contains("baseColorMap") && in["baseColorMap"].is_string() && !in["baseColorMap"].get<std::string>().empty())
                            inst["files"] = {{"BaseColorMap", {{"file", in["baseColorMap"]}}}};
                        instances.push_back(std::move(inst));
                    }
                    json req = {{"folder", cp.substr(0, cp.find_last_of('/')) + "/Materials"}, {"meshes", {objectPath}}, {"instances", instances}};
                    if (material != "generate")
                        req["parent"] = material;
                    link::RpcResult mr = client->call("ivyMaterials", req, link::importCallOptions());
                    if (mr.ok)
                        materialsMade = mr.result;
                    else
                        warnings.push_back("ivyMaterials: " + mr.error);
                }
            }
            return {{"mesh", objectPath}, {"contentPath", cp}, {"placed", placed}, {"materials", materialsMade}, {"warnings", warnings}};
        },
        .examples = {"ivySend('WallIvy', nanite=True)", "ivySend('Wall_ivy', contentPath='/Game/Env/SM_WallIvy', material='/Game/Env/M_MyFoliage')"},
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// Placeholders

void registerPlaceholders()
{
    auto add = [](const char* name, const char* role, const char* style, std::vector<std::string> tags, const char* doc, std::function<geo::Mesh()> build) {
        Placeholder p;
        p.collection = "Weathering/Ivy";
        p.name = name;
        p.role = role;
        p.style = style;
        p.tags = std::move(tags);
        p.description = doc;
        p.physicsMaterial = "paper";
        p.physicsShape = "box";
        p.build = std::move(build);
        addPlaceholder(std::move(p));
    };
    add("IvyLeaf", "ivy-leaf", "realistic", {"leaf", "ivy", "card"}, "A five-lobed ivy leaf card.", [] { return ivy::leafCard(false, 0); });
    add("IvyLeafToon", "ivy-leaf", "toon", {"leaf", "ivy", "chunky"}, "A plump toon leaf with thickness.", [] { return ivy::leafCard(true, 0); });
    add("IvyLeafToonBroad", "ivy-leaf", "toon", {"leaf", "ivy", "chunky"}, "A broad plump toon leaf.", [] { return ivy::leafCard(true, 1); });
    add("VineLeaf", "vine-leaf", "realistic", {"leaf", "vine", "card"}, "A broad heart-shaped vine leaf card.", [] { return ivy::leafCard(false, 1); });
    add("VineLeafToon", "vine-leaf", "toon", {"leaf", "vine", "chunky"}, "A broad plump toon vine leaf.", [] { return ivy::leafCard(true, 1); });
    add("IvyFlower", "ivy-flower", "realistic", {"flower", "card"}, "A five-petal flower card.", [] { return ivy::flowerCard(false); });
    add("IvyFlowerToon", "ivy-flower", "toon", {"flower", "chunky"}, "A round-petalled toon flower with thickness.", [] { return ivy::flowerCard(true); });
    add("MossTuft", "moss-tuft", "realistic", {"moss"}, "A fan of fine moss blades.", [] { return ivy::mossTuft(false); });
    add("MossTuftToon", "moss-tuft", "toon", {"moss", "chunky"}, "A soft low moss dome.", [] { return ivy::mossTuft(true); });
}
} // namespace ivynode

bool registerIvy(lila::ext::Context& ctx)
{
    if (!ivynode::registerNode(ctx))
        return false;
    ivynode::registerGeometryNode(ctx);
    ivynode::registerCommands(ctx);
    ivynode::registerPlaceholders();
    registerIvyUi(ctx);
    return true;
}
} // namespace weathering
