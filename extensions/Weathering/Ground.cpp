// Ground dressing: a Stagehand scatter plus a ground material together, and the ground tint modifier that darkens the
// ground under scattered things and along walls and marks low spots wet. The tint is written to the ground's
// "weathering" colour set (R tint, G wet) and as point attributes, so any material can read it, in Lila and in Unreal
// (MF_LilaGroundTint).

#include "Pieces.h"
#include "Placeholders.h"
#include "Weathering.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/Relocate.h>
#include <lila/Transform.h>
#include <lila/Viewport.h>

#include <algorithm>
#include <cmath>
#include <map>

using namespace lila;
using namespace lila::scene;

namespace weathering
{
namespace ground
{
constexpr const char* kTintType = "groundTintModifier";
constexpr const char* kColorSet = "weathering";

/** How dark the ground gets near a source: 1 at contact, easing to 0 at reach. */
double nearness(double d, double reach)
{
    if (reach <= 0.0)
        return 0.0;
    const double t = std::clamp(1.0 - d / reach, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/** Where scattered things lie on a ground: centres with the radius they darken. */
struct Spots
{
    std::vector<glm::dvec3> centres;
    std::vector<double> radii;
};

struct TintOptions
{
    double strength = 0.8;
    double wetness = 0.5;
    double wetDepth = 8.0;
    double wetRadius = 150.0;
    int bands = 0; // 0 smooth; above 1, the tint is stepped into that many bands for a toon look
};

/** Per point tint and wetness of a ground mesh, given its world matrix and world-space sources with their reach. */
void computeTint(const geo::Mesh& ground, const glm::dmat4& world, const std::vector<std::pair<const geo::MeshBvh*, double>>& sources,
                 const Spots& spots, const TintOptions& o, std::vector<float>& tint, std::vector<float>& wet)
{
    LILA_ZONE();

    const geo::Index n = ground.pointCapacity();
    tint.assign(size_t(n), 0.0f);
    wet.assign(size_t(n), 0.0f);
    std::vector<glm::dvec3> pos(static_cast<size_t>(n));
    for (geo::Index p = 0; p < n; ++p)
        if (ground.pointValid(p))
            pos[size_t(p)] = glm::dvec3(world * glm::dvec4(ground.position(p), 1.0));
    // Spots bucketed on a grid as wide as the widest spot, so each point looks at its neighbourhood only.
    double spotCell = 1.0;
    for (double r : spots.radii)
        spotCell = std::max(spotCell, r);
    std::map<std::pair<int64_t, int64_t>, std::vector<size_t>> spotGrid;
    for (size_t s = 0; s < spots.centres.size(); ++s)
        spotGrid[{int64_t(std::floor(spots.centres[s].x / spotCell)), int64_t(std::floor(spots.centres[s].z / spotCell))}].push_back(s);
    for (geo::Index p = 0; p < n; ++p)
    {
        if (!ground.pointValid(p))
            continue;
        double t = 0.0;
        for (const auto& [bvh, reach] : sources)
        {
            geo::BvhClosest c;
            if (bvh->closestPoint(pos[size_t(p)], c, reach))
                t = std::max(t, nearness(c.distance, reach));
        }
        if (!spots.centres.empty())
        {
            const glm::dvec3& q = pos[size_t(p)];
            const int64_t cx = int64_t(std::floor(q.x / spotCell)), cz = int64_t(std::floor(q.z / spotCell));
            for (int64_t dx = -1; dx <= 1; ++dx)
                for (int64_t dz = -1; dz <= 1; ++dz)
                    if (const auto it = spotGrid.find({cx + dx, cz + dz}); it != spotGrid.end())
                        for (size_t s : it->second)
                        {
                            const glm::dvec3 d = q - spots.centres[s];
                            t = std::max(t, nearness(std::sqrt(d.x * d.x + d.z * d.z), spots.radii[s]));
                        }
        }
        if (o.bands > 1)
            t = std::ceil(t * o.bands - 1e-6) / o.bands;
        tint[size_t(p)] = float(t * std::clamp(o.strength, 0.0, 1.0));
    }
    if (o.wetness <= 0.0 || o.wetDepth <= 0.0)
        return;
    // Low spots: points below the average height of their surroundings, found by smoothing heights over the mesh.
    std::vector<std::vector<geo::Index>> neighbours(static_cast<size_t>(n));
    double edgeSum = 0.0;
    size_t edgeCount = 0;
    for (geo::Index f = 0; f < ground.faceCapacity(); ++f)
    {
        if (!ground.faceValid(f))
            continue;
        const std::vector<geo::Index> pts = ground.facePoints(f);
        for (size_t k = 0; k < pts.size(); ++k)
        {
            const geo::Index a = pts[k], b = pts[(k + 1) % pts.size()];
            neighbours[size_t(a)].push_back(b);
            neighbours[size_t(b)].push_back(a);
            edgeSum += glm::length(pos[size_t(a)] - pos[size_t(b)]);
            ++edgeCount;
        }
    }
    const double edge = edgeCount ? edgeSum / double(edgeCount) : 1.0;
    const int passes = std::clamp(int(std::pow(o.wetRadius / std::max(edge, 1e-6), 2.0) * 0.5), 1, 200);
    std::vector<double> h(static_cast<size_t>(n)), next(static_cast<size_t>(n));
    for (geo::Index p = 0; p < n; ++p)
        h[size_t(p)] = pos[size_t(p)].y;
    for (int pass = 0; pass < passes; ++pass)
    {
        for (geo::Index p = 0; p < n; ++p)
        {
            const auto& nb = neighbours[size_t(p)];
            if (nb.empty())
            {
                next[size_t(p)] = h[size_t(p)];
                continue;
            }
            double sum = 0.0;
            for (geo::Index q : nb)
                sum += h[size_t(q)];
            next[size_t(p)] = 0.5 * h[size_t(p)] + 0.5 * sum / double(nb.size());
        }
        h.swap(next);
    }
    const std::span<const float> painted = ground.pointAttrs().get<float>("wetMask");
    for (geo::Index p = 0; p < n; ++p)
    {
        if (!ground.pointValid(p))
            continue;
        const double dip = h[size_t(p)] - pos[size_t(p)].y;
        double w = std::clamp(dip / o.wetDepth, 0.0, 1.0);
        if (size_t(p) < painted.size())
            w = std::max(w, double(painted[size_t(p)]));
        if (o.bands > 1)
            w = w > 0.5 ? 1.0 : 0.0;
        wet[size_t(p)] = float(w * std::clamp(o.wetness, 0.0, 1.0));
    }
}

class GroundTintNode final : public ModifierNode
{
public:
    enum Attr
    {
        kInMesh,
        kEnabled,
        kExportEnabled,
        kMatrix,
        kSources,
        kMatrices,
        kReach,
        kSpots,
        kStrength,
        kWetness,
        kWetDepth,
        kWetRadius,
        kBands,
        kOutMesh,
        kWarning,
    };

    GroundTintNode()
    {
        inAttr = kInMesh;
        enabledAttr = kEnabled;
        outAttr = kOutMesh;
    }

    void compute(const EvalContext&) override
    {
        LILA_ZONE();
        mError.clear();
        const MeshPtr& in = as<MeshPtr>(kInMesh);
        if (!in || !as<bool>(kEnabled) || modifiers::bypassed(in))
        {
            setOutput(kOutMesh, in);
            setOutput(kWarning, std::string());
            return;
        }
        std::vector<geo::Mesh> worldMeshes;
        std::vector<double> reaches;
        for (const auto& [index, value] : elements(kSources))
        {
            const MeshPtr* m = std::get_if<MeshPtr>(&value);
            if (!m || !*m || (*m)->numFaces() == 0)
                continue;
            geo::Mesh w = **m;
            if (const PlugValue* mat = element(kMatrices, index))
            {
                const glm::dmat4 xf = std::get<glm::dmat4>(*mat);
                for (geo::Index p = 0; p < w.pointCapacity(); ++p)
                    if (w.pointValid(p))
                        w.setPosition(p, glm::dvec3(xf * glm::dvec4(w.position(p), 1.0)));
            }
            worldMeshes.push_back(std::move(w));
            const PlugValue* r = element(kReach, index);
            reaches.push_back(r ? std::get<double>(*r) : 40.0);
        }
        std::vector<geo::MeshBvh> bvhs;
        bvhs.reserve(worldMeshes.size());
        std::vector<std::pair<const geo::MeshBvh*, double>> sources;
        for (size_t k = 0; k < worldMeshes.size(); ++k)
        {
            bvhs.emplace_back(worldMeshes[k]);
            sources.emplace_back(&bvhs.back(), reaches[k]);
        }
        TintOptions o;
        o.strength = as<double>(kStrength);
        o.wetness = as<double>(kWetness);
        o.wetDepth = as<double>(kWetDepth);
        o.wetRadius = as<double>(kWetRadius);
        o.bands = int(as<int64_t>(kBands));
        Spots spots;
        if (const MeshPtr& cloud = as<MeshPtr>(kSpots))
        {
            const std::span<const float> radius = cloud->pointAttrs().get<float>("radius");
            for (geo::Index p = 0; p < cloud->pointCapacity(); ++p)
                if (cloud->pointValid(p))
                {
                    spots.centres.push_back(cloud->position(p));
                    spots.radii.push_back(size_t(p) < radius.size() ? double(radius[size_t(p)]) : 25.0);
                }
        }
        std::vector<float> tint, wet;
        computeTint(*in, as<glm::dmat4>(kMatrix), sources, spots, o, tint, wet);
        auto out = std::make_shared<geo::Mesh>(*in);
        std::span<glm::vec4> colors = geo::colors::has(*out, kColorSet) ? geo::colors::values(*out, kColorSet)
                                                                            : geo::colors::add(*out, kColorSet, glm::vec4(0, 0, 0, 1));
        geo::colors::setData(*out, kColorSet, true);
        auto ta = out->pointAttrs().getOrAdd<float>("weatheringTint");
        auto wa = out->pointAttrs().getOrAdd<float>("weatheringWet");
        // The viewport draws them through the grunge layers: tint as occlusion, wet as dust (see the decorator).
        auto va = out->pointAttrs().getOrAdd<float>(render::kGrungeAttrs[2]);
        auto da = out->pointAttrs().getOrAdd<float>(render::kGrungeAttrs[3]);
        for (size_t p = 0; p < tint.size() && p < ta.size(); ++p)
        {
            ta[p] = tint[p];
            wa[p] = wet[p];
            va[p] = tint[p];
            da[p] = wet[p];
        }
        // Colour sets are per corner: each corner takes its point's values.
        for (geo::Index f = 0; f < out->faceCapacity(); ++f)
        {
            if (!out->faceValid(f))
                continue;
            geo::Index c = out->faceCorner(f);
            const geo::Index first = c;
            do
            {
                const geo::Index p = out->cornerPoint(c);
                if (size_t(c) < colors.size())
                    colors[size_t(c)] = glm::vec4(tint[size_t(p)], wet[size_t(p)], 0.0f, 1.0f);
                c = out->cornerNext(c);
            } while (c != first && c != geo::kInvalid);
        }
        setOutput(kOutMesh, MeshPtr(std::move(out)));
        setOutput(kWarning, std::string(worldMeshes.empty() && spots.centres.empty() ? "Nothing to darken round: add walls or a scatter" : ""));
    }
};

AttrDef param(std::string name, PlugType type, PlugValue def, std::string doc, std::optional<double> lo = {}, std::optional<double> hi = {})
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc), .minValue = lo, .maxValue = hi};
}

AttrDef hiddenArray(std::string name, PlugType type, PlugValue def, bool storable, std::string doc)
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def),
            .flags = AttrInput | AttrHidden | AttrArray | (storable ? AttrStorable : 0u), .doc = std::move(doc)};
}

/** A ground preset: what is scattered, how thick, and the ground's own colour in each style. */
struct GroundPreset
{
    const char* name;
    const char* description;
    std::vector<const char*> roles;
    double density;
    glm::dvec3 realistic, toon; // linear base colours
    double roughness;
    double wetness;
};

const std::vector<GroundPreset>& groundPresets()
{
    static const std::vector<GroundPreset> list = {
        {"Rubble Ground", "Broken concrete and pebbles over a dusty ground, dark round walls and rubble.", {"rubble", "pebble"}, 3.0,
         {0.22, 0.2, 0.18}, {0.42, 0.38, 0.34}, 0.95, 0.2},
        {"Asphalt Crack", "Dark tarmac with grit and weeds in the cracks, puddles in the dips.", {"pebble", "grass"}, 2.0, {0.05, 0.05, 0.055},
         {0.18, 0.18, 0.22}, 0.8, 0.7},
        {"Dirt and Litter", "Bare earth with paper, cans and leaves blown about.", {"litter", "leaf", "pebble"}, 1.5, {0.17, 0.12, 0.08},
         {0.45, 0.32, 0.2}, 0.95, 0.4},
        {"Grass Patches", "Patchy grass and weeds over earth.", {"grass", "plant"}, 14.0, {0.12, 0.13, 0.06}, {0.3, 0.45, 0.15}, 0.9, 0.2},
        {"Moss Floor", "Damp stone floor with moss clumps creeping over it.", {"moss", "pebble"}, 6.0, {0.1, 0.11, 0.09}, {0.25, 0.32, 0.22},
         0.85, 0.6},
    };
    return list;
}

const GroundPreset* findGroundPreset(const std::string& name)
{
    for (const GroundPreset& p : groundPresets())
        if (name == p.name)
            return &p;
    return nullptr;
}

/** The meshes standing on a ground (walls, rocks), for the tint: visible meshes whose bounds start near its top. */
std::vector<NodeId> standingOn(Graph& g, NodeId ground, const std::vector<NodeId>& skip)
{
    std::vector<NodeId> out;
    Node* gn = g.node(ground);
    const auto gm = gn->as<MeshPtr>(gn->type().attrIndex("outMesh"));
    if (!gm)
        return out;
    const glm::dmat4 gw = gn->as<glm::dmat4>(Transform::kWorldMatrix);
    glm::dvec3 lo(1e300), hi(-1e300);
    for (geo::Index p = 0; p < gm->pointCapacity(); ++p)
        if (gm->pointValid(p))
        {
            const glm::dvec3 w(gw * glm::dvec4(gm->position(p), 1.0));
            lo = glm::min(lo, w);
            hi = glm::max(hi, w);
        }
    for (NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n || id == ground || !n->type().isA("mesh") || std::ranges::find(skip, id) != skip.end())
            continue;
        if (!n->as<bool>(Transform::kVisibility))
            continue;
        const auto m = n->as<MeshPtr>(n->type().attrIndex("outMesh"));
        if (!m || m->numFaces() == 0)
            continue;
        const glm::dmat4 w = n->as<glm::dmat4>(Transform::kWorldMatrix);
        glm::dvec3 a(1e300), b(-1e300);
        for (geo::Index p = 0; p < m->pointCapacity(); ++p)
            if (m->pointValid(p))
            {
                const glm::dvec3 q(w * glm::dvec4(m->position(p), 1.0));
                a = glm::min(a, q);
                b = glm::max(b, q);
            }
        if (b.x < lo.x || a.x > hi.x || b.z < lo.z || a.z > hi.z)
            continue;
        if (a.y > hi.y + 50.0 || b.y < lo.y)
            continue;
        out.push_back(id);
    }
    return out;
}
/** How the tint shows in Lila's viewport: grime darkening for the tint, a dark glossy film for the wet. */
std::shared_ptr<const render::GrungeLook> tintLook(int bands)
{
    static std::shared_ptr<const render::GrungeLook> looks[9];
    const int b = std::clamp(bands, 0, 8);
    if (looks[b])
        return looks[b];
    render::GrungeLook look;
    render::GrungeLayer& tintLayer = look.layers[2];
    tintLayer.enabled = true;
    tintLayer.level = 0.5f;
    tintLayer.softness = 1.0f;
    tintLayer.bands = b > 1 ? b : 0;
    tintLayer.strength = 0.85f;
    tintLayer.color = {0.035f, 0.03f, 0.022f};
    tintLayer.low = tintLayer.color;
    render::GrungeLayer& wetLayer = look.layers[3];
    wetLayer.enabled = true;
    wetLayer.level = 0.5f;
    wetLayer.softness = b > 1 ? 0.0f : 0.6f;
    wetLayer.strength = 0.55f;
    wetLayer.color = {0.02f, 0.02f, 0.022f};
    wetLayer.low = wetLayer.color;
    wetLayer.roughness = 0.08f;
    looks[b] = std::make_shared<const render::GrungeLook>(look);
    return looks[b];
}

const GroundTintNode* tintOn(Graph& g, NodeId mesh)
{
    for (NodeId id : modifiers::stack(g, mesh))
        if (Node* n = g.node(id); n && n->type().name == kTintType)
            return static_cast<const GroundTintNode*>(n);
    return nullptr;
}
} // namespace ground

bool registerGround(ext::Context& ctx)
{
    using namespace ground;
    using T = ArgType;
    ui::viewportHooks::setMaterialDecorator("weathering.groundTint", [](NodeId id, const geo::Mesh& mesh, std::vector<render::Material>& mats) {
        if (mats.empty() || mesh.pointAttrs().get<float>("weatheringTint").empty())
            return;
        const GroundTintNode* n = tintOn(Graph::current(), id);
        if (!n || !n->as<bool>(GroundTintNode::kEnabled))
            return;
        const auto look = tintLook(int(n->as<int64_t>(GroundTintNode::kBands)));
        for (render::Material& m : mats)
            if (!m.grunge)
                m.grunge = look;
    });
    ctx.track("materialDecorator:weathering.groundTint", [] { ui::viewportHooks::removeMaterialDecorator("weathering.groundTint"); });
    NodeType t;
    t.name = kTintType;
    t.icon = "brush";
    t.doc = "Ground Tint: darkens the ground under and round what stands on it (walls, rubble, plants) and marks its low spots wet, "
            "live. Writes the weathering colour set (R tint, G wet) and the point attributes weatheringTint and weatheringWet for "
            "materials to read; in Unreal, MF_LilaGroundTint reads the colours.";
    t.ownAttrs = {
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden},
        param("enabled", PlugType::Bool, true, "Turn the tint off without removing it."),
        {.name = "exportEnabled", .type = PlugType::Bool, .defaultValue = true, .flags = AttrInput | AttrStorable, .doc = "Apply when exporting."},
        {.name = "matrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden | AttrStorable,
         .doc = "The ground's world matrix when the tint was made; run groundTint again after moving the ground itself."},
        hiddenArray("sources", PlugType::Mesh, MeshPtr(), false, "Meshes the ground darkens round."),
        hiddenArray("matrices", PlugType::Matrix, glm::dmat4(1.0), false, "Each source's world matrix."),
        hiddenArray("reach", PlugType::Float, 40.0, true, "How far out each source darkens the ground, cm."),
        {.name = "spots", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden | AttrStorable,
         .doc = "World-space spots darkened as they lay when the tint was made (a scatter on this ground): points with a radius attribute."},
        param("strength", PlugType::Float, 0.8, "How dark the tint gets at contact, 0 to 1.", 0.0, 1.0),
        param("wetness", PlugType::Float, 0.5, "How wet the low spots get, 0 to 1 (0 for none).", 0.0, 1.0),
        param("wetDepth", PlugType::Float, 8.0, "How far below its surroundings a spot must sit to be fully wet, cm.", 0.1),
        param("wetRadius", PlugType::Float, 150.0, "How wide an area a dip is measured against, cm.", 1.0),
        param("bands", PlugType::Int, int64_t(0), "0 smooth; 2 or more steps the tint into hard bands (toon).", 0.0, 8.0),
        {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden},
        {.name = "warning", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput, .doc = "Problems with the last build."},
    };
    t.create = [] { return std::make_unique<GroundTintNode>(); };
    lila::scene::relocate::registerNeutral(t.name);
    if (!types::registerType(ctx, std::move(t)))
        return false;

    ctx.registerCommand({
        .name = "groundTint",
        .doc = "Darken a ground under and round what stands on it, and wet its low spots, live: a Ground Tint modifier fed by the "
               "sources, so moving a wall or rescattering moves the tint. Sources that read the ground themselves (a scatter on it, "
               "grunge that sees it) can't feed it live and are taken as they stand. Writes the 'weathering' colour set (R tint, G "
               "wet) for materials and MF_LilaGroundTint in Unreal. Returns modifier, live and static (source paths) and a note "
               "when anything is static.",
        .positionalDoc = "ground mesh",
        .flags = {nodeFlag({"sources", "src", T::List, nullptr, "Meshes to darken round; default every visible mesh standing on the ground."}, "mesh"),
                  nodeFlag({"spotsFrom", "sp", T::List, nullptr, "Meshes lying on the ground (a scatter's output): spots under them darken as they lie now."}, "mesh"),
                  {"spotRadius", "spr", T::Float, 25.0, "How far round those spots the ground darkens, cm.", {}, {}, 0.0},
                  {"reach", "r", T::Float, 60.0, "How far out each source darkens the ground, cm.", {}, {}, 0.0},
                  {"strength", "st", T::Float, 0.8, "How dark at contact, 0 to 1.", {}, {}, 0.0, 1.0},
                  {"wetness", "we", T::Float, 0.5, "How wet low spots get, 0 to 1.", {}, {}, 0.0, 1.0},
                  {"wetDepth", "wd", T::Float, 8.0, "Depth of a fully wet dip, cm.", {}, {}, 0.1},
                  {"wetRadius", "wr", T::Float, 150.0, "Area a dip is measured against, cm.", {}, {}, 1.0},
                  styleFlag()},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("groundTint: give one ground mesh");
            Node* gr = g.find(names[0]);
            if (!gr || !gr->type().isA("mesh"))
                throw CommandError("groundTint: '" + names[0] + "' is not a mesh");
            g.evaluate();
            std::vector<NodeId> sources;
            if (a.has("sources"))
            {
                for (const json& s : a.flags["sources"])
                {
                    Node* n = g.find(s.get<std::string>());
                    if (!n || !n->type().isA("mesh"))
                        throw CommandError("groundTint: source '" + s.get<std::string>() + "' is not a mesh");
                    sources.push_back(n->id());
                }
            }
            else
                sources = standingOn(g, gr->id(), {});
            const bool toon = styleArg(a) == Style::Toon;
            const json params = {{"strength", a.get<double>("strength", 0.8)}, {"wetness", a.get<double>("wetness", 0.5)},
                                 {"wetDepth", a.get<double>("wetDepth", 8.0)}, {"wetRadius", a.get<double>("wetRadius", 150.0)},
                                 {"bands", toon ? 3 : 0}};
            const std::string mod = ops::execute("addModifier", {{"args", {gr->path()}}, {"kind", "groundTint"}, {"params", params}});
            Node* m = g.find(mod);
            const int srcAttr = m->type().attrIndex("sources"), matAttr = m->type().attrIndex("matrices"), reachAttr = m->type().attrIndex("reach");
            // The ground's own matrix can't be connected (it would loop through its own inMesh), so it is set here.
            g.setValue({m->id(), m->type().attrIndex("matrix"), -1}, gr->as<glm::dmat4>(Transform::kWorldMatrix));
            const double reach = a.get<double>("reach", 60.0);
            // Spots: where things lie, taken as they are now, for sources that can't feed the tint live.
            std::map<std::pair<int64_t, int64_t>, std::pair<glm::dvec3, double>> spotCells;
            auto addSpots = [&](Node* n, double radius) {
                const auto mesh = n->as<MeshPtr>(n->type().attrIndex("outMesh"));
                if (!mesh)
                    return;
                const double cell = std::max(radius * 0.5, 1.0);
                const glm::dmat4 w = n->as<glm::dmat4>(Transform::kWorldMatrix);
                auto put = [&](const glm::dvec3& q) {
                    const std::pair<int64_t, int64_t> k{int64_t(std::floor(q.x / cell)), int64_t(std::floor(q.z / cell))};
                    auto [it, fresh] = spotCells.try_emplace(k, std::pair{q, radius});
                    if (!fresh && q.y < it->second.first.y)
                        it->second = {q, std::max(radius, it->second.second)};
                };
                for (geo::Index f = 0; f < mesh->faceCapacity(); ++f)
                {
                    if (!mesh->faceValid(f))
                        continue;
                    const std::vector<geo::Index> pts = mesh->facePoints(f);
                    for (size_t k = 1; k + 1 < pts.size(); ++k)
                    {
                        const glm::dvec3 p0(w * glm::dvec4(mesh->position(pts[0]), 1.0)), p1(w * glm::dvec4(mesh->position(pts[k]), 1.0)),
                            p2(w * glm::dvec4(mesh->position(pts[k + 1]), 1.0));
                        const int steps = std::clamp(int(std::ceil(std::max({glm::length(p1 - p0), glm::length(p2 - p0), glm::length(p2 - p1)}) / cell)), 1, 400);
                        for (int i = 0; i <= steps; ++i)
                            for (int j = 0; i + j <= steps; ++j)
                                put(p0 + (p1 - p0) * (double(i) / steps) + (p2 - p0) * (double(j) / steps));
                    }
                }
            };
            json live = json::array(), still = json::array();
            int index = 0;
            for (NodeId s : sources)
            {
                // A source that reads this ground (its grunge sees the ground, a scatter stands on it) would loop: it is
                // taken as it stands now instead, and listed under static.
                try
                {
                    g.connect({s, MeshNode::kOutMesh, -1}, {m->id(), srcAttr, -1, index}, true);
                }
                catch (const std::exception&)
                {
                    addSpots(g.node(s), reach);
                    still.push_back(g.node(s)->path());
                    continue;
                }
                g.connect({s, Transform::kWorldMatrix, -1}, {m->id(), matAttr, -1, index}, true);
                g.setValue({m->id(), reachAttr, -1, index}, reach);
                live.push_back(g.node(s)->path());
                ++index;
            }
            for (const json& s : a.flags.value("spotsFrom", json::array()))
            {
                Node* n = g.find(s.get<std::string>());
                if (!n || !n->type().isA("mesh"))
                    throw CommandError("groundTint: '" + s.get<std::string>() + "' is not a mesh");
                addSpots(n, a.get<double>("spotRadius", 25.0));
                still.push_back(n->path());
            }
            auto cloud = std::make_shared<geo::Mesh>();
            for (const auto& [_, c] : spotCells)
                cloud->addPoint(c.first);
            auto radii = cloud->pointAttrs().getOrAdd<float>("radius");
            size_t r = 0;
            for (const auto& [_, c] : spotCells)
                radii[r++] = float(c.second);
            g.setValue({m->id(), m->type().attrIndex("spots"), -1}, MeshPtr(std::move(cloud)));
            json out = {{"modifier", m->path()}, {"live", live}, {"static", still}};
            if (!still.empty())
                out["note"] = "static sources are taken as they stand now: run groundTint again after moving them";
            return out;
        },
        .examples = {"groundTint('Ground')", "groundTint('Street', sources=['Wall', 'Rubble_Base'], reach=80, wetness=0.8)"},
    });

    ctx.registerCommand({
        .name = "groundDressing",
        .doc = "Dress a ground in one step: scatter pieces over it (a live Stagehand scatter), give it a ground material, and "
               "tint it darker round walls and the scattered pieces, wetter in the dips. Presets: Rubble Ground, Asphalt Crack, "
               "Dirt and Litter, Grass Patches, Moss Floor, each in realistic or toon. items= swaps in your own pieces and "
               "material= your own material. Returns group (the scatter), scatter (Stagehand's result), tint (the modifier) "
               "and material.",
        .positionalDoc = "ground mesh",
        .flags = {{"preset", "p", T::String, "Rubble Ground", "Rubble Ground, Asphalt Crack, Dirt and Litter, Grass Patches or Moss Floor.",
                   {"Rubble Ground", "Asphalt Crack", "Dirt and Litter", "Grass Patches", "Moss Floor"}},
                  styleFlag(),
                  piecesFlag("items", "it", "Pieces to scatter (meshes, library ids, tag:a+b); default the preset's roles from the library."),
                  {"density", "de", T::Float, nullptr, "Pieces per square metre (default the preset's).", {}, {}, 0.0},
                  {"material", "mat", T::String, nullptr, "Your own material for the ground; default a new one in the preset's colour."},
                  {"tint", "ti", T::Bool, true, "Darken the ground round walls and pieces, and wet the dips."},
                  {"wetness", "we", T::Float, nullptr, "How wet the dips get, 0 to 1 (default the preset's).", {}, {}, 0.0, 1.0},
                  {"mask", "mk", T::String, "", "Point attribute (0 to 1) on the ground that thins the scatter."},
                  {"seed", "s", T::Int, int64_t(1), "Another seed gives another scatter."},
                  {"name", "n", T::String, nullptr, "Name of the scatter made."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("groundDressing: give one ground mesh");
            Node* gr = g.find(names[0]);
            if (!gr || !gr->type().isA("mesh"))
                throw CommandError("groundDressing: '" + names[0] + "' is not a mesh");
            const std::string presetName = a.get<std::string>("preset", "Rubble Ground");
            const GroundPreset* preset = findGroundPreset(presetName);
            if (!preset)
                throw CommandError("groundDressing: no ground preset '" + presetName + "'");
            const Style style = styleArg(a);
            const bool toon = style == Style::Toon;
            json items;
            if (a.has("items"))
                items = a.flags["items"];
            else
            {
                buildPlaceholders(false);
                items = json::array();
                for (const char* role : preset->roles)
                    for (const std::string& id : roleAssets(role, style))
                        items.push_back(id);
                if (items.empty())
                    throw CommandError("groundDressing: no pieces for " + presetName + "; give items=[...]");
            }
            const double density = a.get<double>("density", preset->density * (toon ? 0.6 : 1.0));
            const std::string scatterName = a.get<std::string>("name", gr->name() + "_Ground");
            json scatter = ops::execute("scatterProps", {{"args", {gr->path()}},
                                                         {"items", items},
                                                         {"density", density},
                                                         {"slopeMax", 40.0},
                                                         {"align", 0.6},
                                                         {"mask", a.get<std::string>("mask", "")},
                                                         {"maxCount", 2000},
                                                         {"seed", a.get<int64_t>("seed", 1)},
                                                         {"name", scatterName}});
            json out = {{"scatter", scatter}};
            const std::string host = scatter.value("host", std::string());
            if (!host.empty())
            {
                const json made = ops::execute("group", {{"args", {host}}, {"name", scatterName + "_grp"}});
                out["group"] = made.is_array() ? made[0] : made;
            }

            std::string material = a.get<std::string>("material", "");
            if (material.empty())
            {
                const glm::dvec3 c = toon ? preset->toon : preset->realistic;
                std::string base = std::string("Weathering_") + preset->name + (toon ? "_Toon" : "");
                std::ranges::replace(base, ' ', '_');
                const json made = ops::execute("createMaterial", {{"args", {base}}, {"baseColor", json::array({c.x, c.y, c.z})},
                                                                  {"roughness", toon ? 1.0 : preset->roughness}});
                material = made.is_string() ? made.get<std::string>() : made.value("name", base);
            }
            ops::execute("assignMaterial", {{"args", {material, gr->path()}}});
            out["material"] = material;

            if (a.get<bool>("tint", true))
            {
                json sources = json::array();
                std::vector<NodeId> skip;
                if (Node* h = g.find(host))
                    skip.push_back(h->id());
                g.evaluate();
                for (NodeId id : standingOn(g, gr->id(), skip))
                    sources.push_back(g.node(id)->path());
                json call = {{"args", {gr->path()}}, {"sources", sources}};
                if (Node* h = g.find(host))
                    call["spotsFrom"] = json::array({h->path()});
                out["tint"] = ops::execute("groundTint", {{"args", call["args"]},
                                                          {"sources", sources},
                                                          {"spotsFrom", call.value("spotsFrom", json::array())},
                                                          {"wetness", a.get<double>("wetness", preset->wetness)},
                                                          {"style", styleName(style)}});
            }
            return out;
        },
        .examples = {"groundDressing('Ground', preset='Asphalt Crack')", "groundDressing('Yard', preset='Grass Patches', style='toon')",
                     "groundDressing('Floor', preset='Moss Floor', items=['MyMoss_grp'], material='M_Stone')"},
    });

    for (const char* style : {"realistic", "toon"})
        for (const GroundPreset& p : groundPresets())
        {
            const std::string name = std::string(p.name) + (std::string(style) == "toon" ? " Toon" : "");
            json items = json::array();
            for (const char* role : p.roles)
                items.push_back(std::string("tag:weathering+") + role + "+" + style);
            const json spec = {{"description", p.description},
                               {"category", "Weathering"},
                               {"kind", "ground"},
                               {"thumbnailVersion", 2},
                               {"style", style},
                               {"tags", json::array({"weathering", "ground", style})},
                               {"slots", {{"ground", {{"kind", "mesh"}, {"doc", "The ground to dress."}}},
                                          {"items", {{"kind", "items"}, {"doc", "Pieces to scatter."}, {"default", items}}}}},
                               {"steps", json::array({{{"command", "groundDressing"},
                                                       {"args", json::array({"$ground"})},
                                                       {"flags", {{"preset", p.name}, {"style", style}, {"items", "$items"}}}}})}};
            ops::execute("arrangementRegister", {{"args", {name}}, {"spec", spec}, {"source", "Weathering"}});
        }
    return true;
}
} // namespace weathering
