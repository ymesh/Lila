// Live grunge masks: the grungeModifier node computing edge wear, cavity dirt, occlusion, dust, streaks and ground fade
// into a mesh's corners, the look drawn per pixel in Lila's materials, and the commands that add, bake and send them.

#include "MaskCompute.h"
#include "Masks.h"
#include "Pieces.h"
#include "Weathering.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Events.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Math.h>
#include <lila/Menus.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/OptionBox.h>
#include <lila/Profile.h>
#include <lila/Relocate.h>
#include <lila/Tasks.h>
#include <lila/Transform.h>
#include <lila/Undo.h>
#include <lila/Viewport.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <set>

using namespace lila;
using namespace lila::scene;

namespace weathering
{
namespace masks
{
static_assert(int(kMaskCount) == render::kGrungeMasks);

namespace
{
using T = ArgType;

const std::vector<std::string> kUnrealModes = {"auto", "color", "uv", "off"};

/** Look attribute suffixes, in GrungeLayer order. */
enum LookAttr
{
    kOn,
    kLevel,
    kSoftness,
    kBreakup,
    kNoiseScale,
    kBands,
    kCut,
    kOutline,
    kStrength,
    kColor,
    kLow,
    kRoughness,
    kMultiply,
    kLookAttrs,
};
constexpr std::array<const char*, kLookAttrs> kLookSuffix = {"Enabled", "Level", "Softness", "Breakup", "NoiseScale", "Bands",
                                                            "Cut",     "Outline", "Strength", "Color",   "Low",        "Roughness", "Multiply"};
constexpr std::array<const char*, kMaskCount> kMaskTitles = {"Edge Wear", "Cavity Dirt", "Occlusion", "Dust", "Streaks", "Ground Fade"};

/** A mask's starting look, realistic. */
struct LookDefault
{
    double level, softness, breakup, noiseScale, strength;
    glm::dvec3 color, low;
    double roughness;
    bool multiply; // grime darkens the surface by its colour; worn edges and dust lay their own colour over it
};

/** Raised whenever the default looks change: grungeMask resets older nodes to them. */
constexpr int kLookVersion = 3;

constexpr std::array<LookDefault, kMaskCount> kRealistic = {{
    {0.45, 0.30, 0.60, 6.0, 0.45, {0.62, 0.60, 0.56}, {0.45, 0.43, 0.40}, 0.45, false},
    {0.35, 0.40, 0.60, 20.0, 0.60, {0.40, 0.36, 0.31}, {0.65, 0.62, 0.58}, 0.90, true},
    {0.35, 0.60, 0.00, 30.0, 0.35, {0.60, 0.60, 0.60}, {0.80, 0.80, 0.80}, -1.0, true},
    {0.60, 0.50, 0.50, 30.0, 0.20, {0.72, 0.69, 0.63}, {0.62, 0.59, 0.54}, 0.95, false},
    {0.35, 0.35, 0.80, 4.0, 0.35, {0.55, 0.52, 0.47}, {0.75, 0.72, 0.67}, 0.75, true},
    {0.40, 0.60, 0.50, 25.0, 0.45, {0.55, 0.50, 0.41}, {0.75, 0.71, 0.63}, 0.90, true},
}};

/** Painted toon colours: flat, warmer, readable bands. */
constexpr std::array<std::pair<glm::dvec3, glm::dvec3>, kMaskCount> kToonColors = {{
    {{0.92, 0.82, 0.62}, {0.70, 0.60, 0.45}},
    {{0.14, 0.10, 0.16}, {0.28, 0.22, 0.30}},
    {{0.10, 0.10, 0.20}, {0.22, 0.22, 0.34}},
    {{0.80, 0.74, 0.60}, {0.62, 0.56, 0.45}},
    {{0.18, 0.16, 0.24}, {0.32, 0.30, 0.40}},
    {{0.30, 0.42, 0.20}, {0.44, 0.56, 0.30}},
}};

std::string lookName(int mask, int attr) { return std::string(kMaskNames[size_t(mask)]) + kLookSuffix[size_t(attr)]; }

AttrDef maskParam(std::string name, PlugType type, PlugValue def, std::string doc, std::string category, std::optional<double> lo = {},
                  std::optional<double> hi = {}, uint32_t extra = 0)
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrKeyable | extra,
            .doc = std::move(doc), .minValue = lo, .maxValue = hi, .category = std::move(category)};
}

/** The look attributes of one mask, under its own heading. */
std::vector<AttrDef> lookAttrs(int k)
{
    const LookDefault& d = kRealistic[size_t(k)];
    const std::string cat = kMaskTitles[size_t(k)];
    return {
        maskParam(lookName(k, kOn), PlugType::Bool, true, "Show this mask.", cat),
        maskParam(lookName(k, kLevel), PlugType::Float, d.level, "Where the mask starts, 0 to 1 (as the baked generators' level).", cat, 0.0, 1.0),
        maskParam(lookName(k, kSoftness), PlugType::Float, d.softness, "Width of the blend at the level; near 0 is a hard edge.", cat, 0.0, 1.0),
        maskParam(lookName(k, kBreakup), PlugType::Float, d.breakup, "How much world noise breaks the edge up.", cat, 0.0, 2.0),
        maskParam(lookName(k, kNoiseScale), PlugType::Float, d.noiseScale, "Size of the breakup noise, cm.", cat, 0.1),
        maskParam(lookName(k, kBands), PlugType::Int, int64_t(0), "Posterize into this many bands, for toon looks; 0 is smooth.", cat, 0.0, 8.0),
        maskParam(lookName(k, kCut), PlugType::Float, 0.0, "Hard cut: below it nothing, above it full; 0 is off.", cat, 0.0, 1.0),
        maskParam(lookName(k, kOutline), PlugType::Float, 0.0, "Outline along the mask's edge, in mask units; 0 is off.", cat, 0.0, 0.5),
        maskParam(lookName(k, kStrength), PlugType::Float, d.strength, "How strongly the mask lays its colour over the surface.", cat, 0.0, 1.0),
        maskParam(lookName(k, kColor), PlugType::Vec3, d.color, "Colour where the mask is full: the ramp's top stop.", cat, {}, {}, AttrColor),
        maskParam(lookName(k, kLow), PlugType::Vec3, d.low, "Colour where the mask just starts: the ramp's bottom stop.", cat, {}, {}, AttrColor),
        maskParam(lookName(k, kRoughness), PlugType::Float, d.roughness, "Roughness under the mask; below 0 leaves the surface's own.", cat, -1.0, 1.0),
        maskParam(lookName(k, kMultiply), PlugType::Bool, d.multiply,
                  "Darken the surface by the colour, keeping its texture (grime); off lays the colour over it (paint, dust).", cat),
    };
}

/** The cache key of the geometric pass: its inputs by identity and the settings it reads. */
struct GeoKey
{
    std::vector<MeshPtr> meshes;
    std::vector<glm::dmat4> matrices;
    std::vector<double> numbers;

    bool operator==(const GeoKey&) const = default;
};

class GrungeNode final : public ModifierNode
{
public:
    GrungeNode()
    {
        inAttr = 0;
        enabledAttr = 1;
    }

    void compute(const EvalContext&) override
    {
        LILA_ZONE();
        mError.clear();
        const MeshPtr& in = as<MeshPtr>(inAttr);
        const int outIdx = attr("outMesh");
        if (!in || !as<bool>(enabledAttr) || modifiers::bypassed(in))
        {
            setOutput(outIdx, in);
            setOutput(attr("samples"), int64_t(0));
            return;
        }
        try
        {
            GeoKey key;
            key.meshes.push_back(in);
            key.matrices.push_back(as<glm::dmat4>(attr("hostMatrix")));
            std::vector<Neighbour> near;
            const auto& meshes = elements(attr("neighbours"));
            const int mats = attr("neighbourMatrices");
            for (const auto& [index, value] : meshes)
            {
                const MeshPtr* m = std::get_if<MeshPtr>(&value);
                if (!m || !*m)
                    continue;
                const glm::dmat4 w = elementOr<glm::dmat4>(mats, index);
                key.meshes.push_back(*m);
                key.matrices.push_back(w);
                near.push_back({m->get(), w});
            }
            const Settings s = settings();
            key.numbers = {s.edgeWidth, s.edgeAngle, s.cavityWidth, s.aoRadius, double(s.aoRays), s.dustCover, s.streakLength, s.streakSpread,
                           s.coverHeight, s.fadeHeight, s.groundHeight, s.groundPlane ? 1.0 : 0.0, s.up.x, s.up.y, s.up.z, double(s.seed),
                           double(as<int64_t>(attr("unrealChannels"))), double(as<int64_t>(attr("uvSet")))};
            std::scoped_lock lock(mCacheMutex);
            if (!mOut || !(key == mKey))
            {
                const Result r = masks::compute(*in, key.matrices[0], near, s);
                auto out = std::make_shared<geo::Mesh>(*in);
                writeAttributes(*out, r);
                mLayout = writeUnreal(*out, r, int(as<int64_t>(attr("unrealChannels"))), int(as<int64_t>(attr("uvSet"))),
                                      !geo::colors::sets(*in).empty());
                mSamples = int64_t(r.samples);
                mOut = std::move(out);
                mKey = std::move(key);
                ++mPasses;
            }
            // The same mesh while only the look changes, so the viewport keeps its copy and redraws the look alone.
            setOutput(outIdx, MeshPtr(mOut));
            setOutput(attr("samples"), mSamples);
            setOutput(attr("unrealLayout"), mLayout);
        }
        catch (const std::exception& e)
        {
            log::write(LogLevel::Warning, std::string(name()) + ": " + e.what());
            mError = e.what();
            setOutput(outIdx, in);
        }
    }

    /** Geometric passes this node has run; a look change must not add one. */
    int64_t passes() const { return mPasses; }

    Settings settings() const
    {
        Settings s;
        s.edgeWidth = as<double>(attr("edgeWidth"));
        s.edgeAngle = as<double>(attr("edgeAngle"));
        s.cavityWidth = as<double>(attr("cavityWidth"));
        s.aoRadius = as<double>(attr("aoRadius"));
        s.aoRays = int(as<int64_t>(attr("aoRays")));
        s.dustCover = as<double>(attr("dustCover"));
        s.streakLength = as<double>(attr("streakLength"));
        s.streakSpread = as<double>(attr("streakSpread"));
        s.coverHeight = as<double>(attr("coverHeight"));
        s.fadeHeight = as<double>(attr("fadeHeight"));
        s.groundHeight = as<double>(attr("groundHeight"));
        s.groundPlane = as<bool>(attr("groundPlane"));
        s.up = math::axisBasis().up;
        s.seed = uint32_t(as<int64_t>(attr("seed")));
        return s;
    }

private:
    int attr(std::string_view n) const { return type().attrIndex(n); }

    /** The masks again in channels an Unreal mesh carries; returns which, for the material instance. */
    static std::string writeUnreal(geo::Mesh& m, const Result& r, int mode, int uvSet, bool hadColors)
    {
        if (mode == 3)
            return "off";
        const bool color = mode == 1 || (mode == 0 && !hadColors);
        auto uv = [&](int set, int a, int b) {
            auto v = m.cornerAttrs().getOrAdd<glm::vec2>("uv" + std::to_string(set));
            for (geo::Index c = 0; c < m.cornerCapacity(); ++c)
                if (m.cornerValid(c))
                    v[c] = {r.corners[size_t(a)][c], r.corners[size_t(b)][c]};
        };
        // Per-pixel fields for Unreal in two more UV sets: (height, the face's longest contact line), (drop below its
        // first streak source, position along it). One line and one source each, as many as the channels hold.
        auto fields = [&](int set) {
            auto a = m.cornerAttrs().getOrAdd<glm::vec2>("uv" + std::to_string(set));
            auto b = m.cornerAttrs().getOrAdd<glm::vec2>("uv" + std::to_string(set + 1));
            for (geo::Index c = 0; c < m.cornerCapacity(); ++c)
            {
                if (!m.cornerValid(c))
                    continue;
                const float h = c < r.height.size() ? r.height[c] : kFarEdge;
                const float contact = c < r.contactDistance.size() ? r.contactDistance[c].x : kFarEdge;
                a[c] = {h, contact};
                b[c] = c < r.streakLines.size() ? glm::vec2(r.streakLines[c].x, r.streakLines[c].y) : glm::vec2(kFarEdge, 0.0f);
            }
        };
        // The sharp-edge distances follow the masks in two more UV sets, so Unreal's edge wear holds on plain blocks too.
        auto distances = [&](int set) {
            for (int k = 0; k < 2; ++k)
            {
                auto v = m.cornerAttrs().getOrAdd<glm::vec2>("uv" + std::to_string(set + k));
                for (geo::Index c = 0; c < m.cornerCapacity() && c < r.edgeDistance.size(); ++c)
                    if (m.cornerValid(c))
                        v[c] = {r.edgeDistance[c][k * 2], r.edgeDistance[c][k * 2 + 1]};
            }
        };
        if (color)
        {
            auto v = geo::colors::add(m, kColorSet, glm::vec4(0.0f));
            geo::colors::setData(m, kColorSet, true);
            for (geo::Index c = 0; c < m.cornerCapacity() && c < v.size(); ++c)
                if (m.cornerValid(c))
                    v[c] = {r.corners[kEdge][c], r.corners[kCavity][c], r.corners[kAo][c], r.corners[kStreak][c]};
            uv(uvSet, kDust, kFade);
            distances(uvSet + 1);
            fields(uvSet + 3);
            return "color";
        }
        uv(uvSet, kEdge, kCavity);
        uv(uvSet + 1, kAo, kStreak);
        uv(uvSet + 2, kDust, kFade);
        distances(uvSet + 3);
        return "uv";
    }

    std::mutex mCacheMutex;
    GeoKey mKey;
    MeshPtr mOut;
    int64_t mSamples = 0;
    int64_t mPasses = 0;
    std::string mLayout;
};

GrungeNode* grungeOn(Graph& g, NodeId mesh)
{
    for (NodeId id : modifiers::stack(g, mesh))
        if (Node* n = g.node(id); n && n->type().name == kGrungeType)
            return static_cast<GrungeNode*>(n);
    return nullptr;
}

int at(const Node& n, std::string_view name) { return n.type().attrIndex(name); }

/** The node's look, as the material draws it. */
render::GrungeLook lookOf(const Node& n)
{
    render::GrungeLook look;
    for (int k = 0; k < kMaskCount; ++k)
    {
        render::GrungeLayer& l = look.layers[size_t(k)];
        l.enabled = n.as<bool>(at(n, lookName(k, kOn)));
        l.level = float(n.as<double>(at(n, lookName(k, kLevel))));
        l.softness = float(n.as<double>(at(n, lookName(k, kSoftness))));
        l.breakup = float(n.as<double>(at(n, lookName(k, kBreakup))));
        l.noiseScale = float(n.as<double>(at(n, lookName(k, kNoiseScale))));
        l.bands = int(n.as<int64_t>(at(n, lookName(k, kBands))));
        l.cut = float(n.as<double>(at(n, lookName(k, kCut))));
        l.outline = float(n.as<double>(at(n, lookName(k, kOutline))));
        l.strength = float(n.as<double>(at(n, lookName(k, kStrength))));
        l.color = glm::vec3(n.as<glm::dvec3>(at(n, lookName(k, kColor))));
        l.low = glm::vec3(n.as<glm::dvec3>(at(n, lookName(k, kLow))));
        l.roughness = float(n.as<double>(at(n, lookName(k, kRoughness))));
        l.multiply = n.as<bool>(at(n, lookName(k, kMultiply)));
        l.seed = uint32_t(n.as<int64_t>(at(n, "seed")) * 7 + k);
    }
    look.outlineColor = glm::vec3(n.as<glm::dvec3>(at(n, "outlineColor")));
    look.edgeWidth = float(n.as<double>(at(n, "edgeWidth")));
    look.cavityWidth = float(n.as<double>(at(n, "cavityWidth")));
    // Occlusion from contact lines reaches half the ray radius: the rays' falloff over a flat wall.
    look.aoWidth = float(n.as<double>(at(n, "aoRadius")) * 0.5);
    look.fadeHeight = float(n.as<double>(at(n, "fadeHeight")));
    look.streakLength = float(n.as<double>(at(n, "streakLength")));
    return look;
}

/** Same look, same pointer: draws with equal materials keep batching together. */
std::shared_ptr<const render::GrungeLook> sharedLook(const render::GrungeLook& look)
{
    static std::mutex m;
    static std::vector<std::shared_ptr<const render::GrungeLook>> recent;
    std::scoped_lock lock(m);
    for (const auto& r : recent)
        if (*r == look)
            return r;
    if (recent.size() > 64)
        recent.erase(recent.begin());
    recent.push_back(std::make_shared<const render::GrungeLook>(look));
    return recent.back();
}

/** What feeds the grunge modifier on a mesh (its mesh before the masks), or the mesh's output when it has none. */
PlugRef unmaskedPlug(Graph& g, NodeId mesh)
{
    if (GrungeNode* n = grungeOn(g, mesh))
        if (std::optional<PlugRef> src = g.source({n->id(), n->inAttr, -1}))
            return *src;
    return {mesh, MeshNode::kOutMesh, -1};
}

geo::Bounds worldBounds(Graph& g, const Node& n)
{
    geo::Bounds b;
    const MeshPtr m = n.as<MeshPtr>(n.type().attrIndex("outMesh"));
    if (!m || m->numPoints() == 0)
        return b;
    const glm::dmat4 w = n.as<glm::dmat4>(Transform::kWorldMatrix);
    const geo::Bounds local = m->bounds();
    for (int c = 0; c < 8; ++c)
    {
        const glm::dvec3 p((c & 1) ? local.max.x : local.min.x, (c & 2) ? local.max.y : local.min.y, (c & 4) ? local.max.z : local.min.z);
        const glm::dvec3 q(w * glm::dvec4(p, 1.0));
        b.min = glm::min(b.min, q);
        b.max = glm::max(b.max, q);
    }
    (void)g;
    return b;
}

bool boundsNear(const geo::Bounds& a, const geo::Bounds& b, double r)
{
    for (int k = 0; k < 3; ++k)
        if (a.max[k] + r < b.min[k] || b.max[k] + r < a.min[k])
            return false;
    return true;
}

/** Every mesh node, for neighbour search. */
std::vector<Node*> allMeshes(Graph& g)
{
    std::vector<Node*> out;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().isA("mesh"))
            out.push_back(n);
    return out;
}

/** Every node downstream of a mesh: what is built from it (its dressing, ivy, decals), which must never feed its masks. */
std::set<NodeId> madeFrom(Graph& g, NodeId mesh)
{
    std::multimap<NodeId, PlugRef> out;
    for (const auto& [dst, src] : g.connections())
        out.insert({src.node, dst});
    std::set<NodeId> seen{mesh};
    std::vector<NodeId> todo{mesh};
    while (!todo.empty())
    {
        const NodeId at = todo.back();
        todo.pop_back();
        const auto [lo, hi] = out.equal_range(at);
        for (auto it = lo; it != hi; ++it)
        {
            const Node* n = g.node(it->second.node);
            // Other meshes' masks and ground tints read this one as a neighbour without being made from it.
            if (!n || n->type().name == kGrungeType || n->type().name == "groundTintModifier")
                continue;
            if (seen.insert(n->id()).second)
                todo.push_back(n->id());
        }
    }
    seen.erase(mesh);
    return seen;
}

/**
 * Connects the meshes near target (within radius of its bounds) as its grunge node's neighbours. bounds were taken
 * before anything was connected: reading a mesh built from the target here would recompute the masks once per neighbour.
 */
int wireNeighbours(Graph& g, Node& mesh, GrungeNode& grunge, const std::vector<Node*>& meshes, const std::map<NodeId, geo::Bounds>& bounds,
                   double radius)
{
    const int nAttr = at(grunge, "neighbours"), namesAttr = at(grunge, "neighbourNames");
    for (const PlugRef& e : g.elements({grunge.id(), nAttr, -1}))
        g.disconnect(e);
    for (const PlugRef& e : g.elements({grunge.id(), namesAttr, -1}))
        g.setValue(e, std::string());
    const auto selfIt = bounds.find(mesh.id());
    if (selfIt == bounds.end() || !selfIt->second.valid())
        return 0;
    const geo::Bounds self = selfIt->second;
    const std::set<NodeId> derived = madeFrom(g, mesh.id());
    int count = 0;
    for (Node* other : meshes)
    {
        if (other->id() == mesh.id() || derived.contains(other->id()))
            continue;
        const auto it = bounds.find(other->id());
        if (it == bounds.end() || !it->second.valid() || !boundsNear(self, it->second, radius))
            continue;
        // A neighbour built from this mesh (its own base dressing, ivy or decals) would read back into it: left out.
        try
        {
            g.connect(unmaskedPlug(g, other->id()), {grunge.id(), nAttr, -1, count}, true);
        }
        catch (const std::exception&)
        {
            continue;
        }
        g.setValue({grunge.id(), namesAttr, -1, count}, other->path());
        ++count;
    }
    return count;
}

/** Grunge nodes reading a mesh's output read its unmasked mesh instead, so masks never feed each other in a loop. */
void rewireReaders(Graph& g, NodeId mesh)
{
    const PlugRef unmasked = unmaskedPlug(g, mesh);
    for (const PlugRef& dst : g.destinations({mesh, MeshNode::kOutMesh, -1}))
        if (Node* n = g.node(dst.node); n && n->type().name == kGrungeType && dst.attr == at(*n, "neighbours"))
            g.connect(unmasked, dst, true);
}

/**
 * Copies the host's and the neighbours' world matrices into a grunge node when they moved. They are values, not
 * connections: a mesh's own matrix feeding its modifier, or two neighbours' feeding each other, would be loops.
 */
void syncMatrices(Graph& g, Node& grunge)
{
    auto put = [&](const PlugRef& plug, const glm::dmat4& m) {
        const PlugValue* now = plug.element >= 0 ? grunge.element(plug.attr, plug.element) : &grunge.value(plug.attr);
        if (!now || !std::holds_alternative<glm::dmat4>(*now) || std::get<glm::dmat4>(*now) != m)
            g.setValue(plug, m);
    };
    if (Node* host = g.node(modifiers::meshOf(g, grunge.id())))
        put({grunge.id(), at(grunge, "hostMatrix"), -1}, host->as<glm::dmat4>(Transform::kWorldMatrix));
    const int mats = at(grunge, "neighbourMatrices");
    for (const auto& [index, value] : grunge.elements(at(grunge, "neighbourNames")))
        if (const std::string* path = std::get_if<std::string>(&value); path && !path->empty())
            if (Node* n = g.find(*path))
                put({grunge.id(), mats, -1, index}, n->as<glm::dmat4>(Transform::kWorldMatrix));
}

/** Every grunge node's matrices in step, outside undo: following a move is not an edit of its own. */
void syncAll(Graph& g)
{
    // The idle list is kept per graph and structure; other callers pass through the same cache.
    static const Graph* graph = nullptr;
    static uint64_t structure = ~uint64_t(0);
    static std::vector<NodeId> nodes;
    if (&g != graph || g.structure() != structure)
    {
        graph = &g;
        structure = g.structure();
        nodes.clear();
        for (NodeId id : g.nodes())
            if (Node* n = g.node(id); n && n->type().name == kGrungeType)
                nodes.push_back(id);
    }
    if (nodes.empty())
        return;
    g.evaluate();
    UndoStack& undo = UndoStack::get();
    const bool was = undo.enabled();
    undo.setEnabled(false);
    for (NodeId id : nodes)
        if (Node* n = g.node(id))
            syncMatrices(g, *n);
    undo.setEnabled(was);
}

/** Look values for a style: realistic keeps the type's defaults, toon takes bands, cuts, outlines and painted colours. */
json styleLook(Style style)
{
    json v = json::object();
    for (int k = 0; k < kMaskCount; ++k)
    {
        const LookDefault& d = kRealistic[size_t(k)];
        const bool toon = style == Style::Toon;
        v[lookName(k, kSoftness)] = toon ? 0.04 : d.softness;
        v[lookName(k, kBreakup)] = toon ? d.breakup * 0.5 : d.breakup;
        v[lookName(k, kNoiseScale)] = toon ? d.noiseScale * 2.5 : d.noiseScale;
        v[lookName(k, kBands)] = toon ? (k == kEdge || k == kDust ? 2 : 3) : 0;
        v[lookName(k, kOutline)] = toon && (k == kEdge || k == kCavity || k == kStreak) ? 0.03 : 0.0;
        const glm::dvec3 c = toon ? kToonColors[size_t(k)].first : d.color, l = toon ? kToonColors[size_t(k)].second : d.low;
        v[lookName(k, kColor)] = json::array({c.x, c.y, c.z});
        v[lookName(k, kLow)] = json::array({l.x, l.y, l.z});
        v[lookName(k, kRoughness)] = toon && d.roughness >= 0.0 ? 0.8 : d.roughness;
        // Toon paints flat bands over the surface; realistic grime darkens it.
        v[lookName(k, kMultiply)] = toon ? false : d.multiply;
        v[lookName(k, kStrength)] = toon ? std::min(d.strength + 0.15, 0.75) : d.strength;
    }
    return v;
}

void setValue(Graph& g, Node& n, const std::string& name, const json& v)
{
    const int i = at(n, name);
    if (i < 0)
        throw CommandError("grungeMask: no setting " + name);
    const AttrDef& def = n.type().attrs[size_t(i)];
    PlugValue pv;
    switch (def.type)
    {
    case PlugType::Bool: pv = v.get<bool>(); break;
    case PlugType::Int:
    case PlugType::Enum: pv = v.is_string() ? int64_t(std::find(def.enumNames.begin(), def.enumNames.end(), v.get<std::string>()) - def.enumNames.begin()) : v.get<int64_t>(); break;
    case PlugType::Float: pv = v.get<double>(); break;
    case PlugType::Vec3: pv = glm::dvec3(v.at(0).get<double>(), v.at(1).get<double>(), v.at(2).get<double>()); break;
    default: throw CommandError("grungeMask: " + name + " can't be set here");
    }
    g.setValue({n.id(), i, -1}, pv);
}

/** Geometric settings the commands pass through, by flag name. */
const std::vector<std::string> kGeoFlags = {"edgeWidth", "edgeAngle", "cavityWidth", "aoRadius", "streakLength", "fadeHeight", "groundPlane", "groundHeight"};

std::vector<Node*> meshesOf(Graph& g, const Args& a, const char* cmd)
{
    std::vector<Node*> out;
    const std::vector<std::string> names = a.objects();
    for (const std::string& name : names)
    {
        Node* n = g.find(name);
        if (!n)
            throw CommandError(std::string(cmd) + ": no node named " + name);
        if (n->type().name == kGrungeType)
            n = g.node(modifiers::meshOf(g, n->id()));
        if (n && n->type().isA("mesh"))
            out.push_back(n);
        else if (n)
            for (NodeId c : g.descendants(n->id()))
                if (Node* m = g.node(c); m && m->type().isA("mesh"))
                    out.push_back(m);
    }
    if (out.empty())
        throw CommandError(std::string(cmd) + ": select or name meshes (a group takes every mesh under it)");
    return out;
}

/** Generator values for a mask's look, the baked twin of the live mask. */
struct BakeSpec
{
    const char* generator;
    json values;
};

BakeSpec bakeSpec(const Node& n, int k, double objectSize)
{
    const auto f = [&](int attr) { return n.as<double>(at(n, lookName(k, attr))); };
    const int64_t seed = n.as<int64_t>(at(n, "seed"));
    const int scale = int(std::clamp(std::lround(objectSize / std::max(f(kNoiseScale), 0.1)), 1L, 64L));
    json v = {{"level", f(kLevel)}, {"softness", std::max(f(kSoftness), 0.01)}};
    switch (k)
    {
    case kEdge: v.update({{"breakup", f(kBreakup)}, {"noiseScale", scale}, {"seed", seed}}); return {"genEdgeWear", v};
    case kCavity: v.update({{"breakup", f(kBreakup)}, {"noiseScale", scale}, {"seed", seed}}); return {"genDirt", v};
    case kAo: return {"genAoDarken", v};
    case kDust: v.update({{"breakup", f(kBreakup)}, {"noiseScale", scale}, {"seed", seed}}); return {"genDust", v};
    case kStreak: v.update({{"seed", seed}}); return {"genDrips", v};
    default:
        return {"genPositionGradient", {{"axis", math::upAxis() == math::UpAxis::Z ? "z" : "y"}, {"start", 0.0}, {"end", 0.35}, {"invert", true},
                                        {"breakup", f(kBreakup)}, {"noiseScale", scale}, {"seed", seed}}};
    }
}

const char* maskOfGenerator(const std::string& type)
{
    for (const auto& [gen, mask] : std::initializer_list<std::pair<const char*, const char*>>{
             {"genEdgeWear", "edge"}, {"genDirt", "cavity"}, {"genCavityGrime", "cavity"}, {"genAoDarken", "ao"}, {"genDust", "dust"},
             {"genDrips", "streak"}, {"genPositionGradient", "fade"}})
        if (type == gen)
            return mask;
    return nullptr;
}

double objectSize(const Node& n)
{
    const MeshPtr m = n.as<MeshPtr>(n.type().attrIndex("outMesh"));
    if (!m || m->numPoints() == 0)
        return 100.0;
    const geo::Bounds b = m->bounds();
    return std::max(glm::length(b.max - b.min), 1.0);
}

json colorJson(const glm::dvec3& c, double a = 1.0) { return json::array({c.x, c.y, c.z, a}); }

/** The texture set the mesh's first material picks, made (with that material) when there is none. */
std::string bakeSet(Graph& g, Node& mesh, const Args& a, json& made)
{
    if (a.has("set"))
        return a.get<std::string>("set");
    const MeshPtr m = mesh.as<MeshPtr>(MeshNode::kOutMesh);
    std::string slot = m && !m->materialNames().empty() ? m->materialNames().front() : std::string();
    if (Node* mat = slot.empty() ? nullptr : materials::find(g, slot))
        if (std::string set = materials::textureSet(*mat); !set.empty())
            return set;
    std::string name = mesh.name() + "_Grunge";
    std::replace_if(name.begin(), name.end(), [](char c) { return !std::isalnum(static_cast<unsigned char>(c)) && c != '_'; }, '_');
    if (!g.find(name))
    {
        json call = {{"args", {name}}};
        if (!slot.empty())
            call["materials"] = json::array({slot});
        ops::execute("texCreateSet", call);
        made.push_back(name);
    }
    return name;
}
} // namespace

const char* kColorSet = "Grunge";

bool registerMaskNode(ext::Context& ctx)
{
    NodeType t;
    t.name = kGrungeType;
    t.icon = "spray";
    t.doc = "Live grunge masks: edge wear on convex edges, dirt in cavities and where other objects touch, occlusion, top-down dust, "
            "streaks running down from sills and overhangs, and fade near the ground, found from the mesh and its neighbours with "
            "no UVs and no bake. The masks go into the mesh's corners (grungeEdge, grungeCavity, ...) and its vertex colours or "
            "UV sets for Unreal; the look (level, softness, bands, cut, outline, colours) draws per pixel in the viewport. "
            "Recomputes only when the mesh, a neighbour or a geometric setting changes.";
    t.ownAttrs = {
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden},
        maskParam("enabled", PlugType::Bool, true, "Turn the masks off without removing them.", ""),
        {.name = "exportEnabled", .type = PlugType::Bool, .defaultValue = true, .flags = AttrInput | AttrStorable, .doc = "Apply when exporting."},
        {.name = "hostMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden | AttrStorable,
         .doc = "The mesh's world matrix, kept in step as it moves: up, the ground and the neighbours are in world space."},
        {.name = "neighbours", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden | AttrArray,
         .doc = "Nearby objects' meshes (before their own masks)."},
        {.name = "neighbourMatrices", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden | AttrArray | AttrStorable,
         .doc = "Their world matrices, kept in step as they move."},
        {.name = "neighbourNames", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrHidden | AttrArray | AttrStorable,
         .doc = "The neighbouring objects, by path."},
        maskParam("seed", PlugType::Int, int64_t(0), "Another seed gives other occlusion sampling and breakup noise.", "Masks"),
        maskParam("edgeWidth", PlugType::Float, 4.0, "How far wear runs from a convex edge, cm.", "Masks", 0.0),
        maskParam("edgeAngle", PlugType::Float, 30.0, "Edges turning more than this, degrees, wear (convex) or gather dirt (concave).", "Masks", 1.0, 179.0),
        maskParam("cavityWidth", PlugType::Float, 10.0, "How far dirt runs from a concave edge, a touching object or the ground, cm.", "Masks", 0.0),
        maskParam("aoRadius", PlugType::Float, 80.0, "How far occlusion looks, cm.", "Masks", 0.0),
        maskParam("aoRays", PlugType::Int, int64_t(16), "Occlusion rays per sample.", "Masks", 1.0, 256.0),
        maskParam("dustCover", PlugType::Float, 1000.0, "Dust settles where nothing covers the surface this far up, cm.", "Masks", 0.0),
        maskParam("streakLength", PlugType::Float, 150.0, "How far streaks run down from a sill or an overhang, cm.", "Masks", 0.0),
        maskParam("streakSpread", PlugType::Float, 5.0, "How far streaks reach past the ends of their sill, cm.", "Masks", 0.0),
        maskParam("coverHeight", PlugType::Float, 400.0, "A ledge is an opening's sill when something covers it within this height, cm.", "Masks", 0.0),
        maskParam("fadeHeight", PlugType::Float, 60.0, "How high sides fade above the ground or the object under them, cm.", "Masks", 0.0),
        maskParam("groundPlane", PlugType::Bool, true, "Treat the ground plane (groundHeight on the up axis) as ground, as well as objects below.", "Masks"),
        maskParam("groundHeight", PlugType::Float, 0.0, "The ground plane's height, cm.", "Masks"),
        {.name = "style", .type = PlugType::String, .defaultValue = std::string("realistic"), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "The style grungeMask last set the look from."},
        maskParam("outlineColor", PlugType::Vec3, glm::dvec3(0.03), "Colour of every mask's outline.", "Masks", {}, {}, AttrColor),
    };
    {
        AttrDef unreal = maskParam("unrealChannels", PlugType::Enum, int64_t(0),
                                   "Where the masks go for Unreal: auto takes vertex colour unless the mesh has its own colour set, then UV sets.", "Unreal");
        unreal.enumNames = kUnrealModes;
        unreal.enumDocs = {"Vertex colour (edge, cavity, occlusion, streaks) plus UV sets for dust/fade and the edge distances; UV sets when the "
                           "mesh has colours.",
                           "Vertex colour plus three UV sets.", "Five UV sets from uvSet: edge/cavity, occlusion/streaks, dust/fade, edge distances.",
                           "None."};
        t.ownAttrs.push_back(unreal);
        t.ownAttrs.push_back(maskParam("uvSet", PlugType::Int, int64_t(3),
                                       "First UV set the masks write, uv<n>: color layout uses it and the next two (masks, edge distances), "
                                       "uv layout it and the next four.", "Unreal", 1.0, 3.0));
    }
    for (int k = 0; k < kMaskCount; ++k)
        for (AttrDef& d : lookAttrs(k))
            t.ownAttrs.push_back(std::move(d));
    t.ownAttrs.push_back({.name = "lookVersion", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable | AttrHidden,
                          .doc = "Which defaults the look was last reset to."});
    t.ownAttrs.push_back({.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden});
    t.ownAttrs.push_back({.name = "samples", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrOutput, .doc = "Surface samples evaluated."});
    t.ownAttrs.push_back({.name = "unrealLayout", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput,
                          .doc = "Where the masks went: color (vertex colour plus uvSet) or uv (three UV sets)."});
    const int out = int(t.ownAttrs.size()) - 3;
    t.create = [out] {
        auto n = std::make_unique<GrungeNode>();
        n->outAttr = out;
        return n;
    };
    relocate::registerNeutral(t.name);
    if (!types::registerType(ctx, std::move(t)))
        return false;

    ui::viewportHooks::setMaterialDecorator("weathering.grunge", [](NodeId id, const geo::Mesh& mesh, std::vector<render::Material>& mats) {
        if (mats.empty() || mesh.cornerAttrs().find(render::kGrungeAttrs[0]) == nullptr)
            return;
        Graph& g = Graph::current();
        const GrungeNode* n = grungeOn(g, id);
        if (!n || !n->as<bool>(n->enabledAttr))
            return;
        const auto look = sharedLook(lookOf(*n));
        for (render::Material& m : mats)
            m.grunge = look;
    });
    ctx.track("materialDecorator:weathering.grunge", [] { ui::viewportHooks::removeMaterialDecorator("weathering.grunge"); });
    ctx.subscribe(events::Event::AppIdle, [](const events::EventData&) {
        if (tasks::isMainThread())
            syncAll(Graph::current());
    });
    return true;
}

json addMasks(Graph& g, const Args& a, const char* cmd)
{
    LILA_ZONE();
    // One notification for the whole command: listeners re-evaluate the scene on every change otherwise, rebuilding
    // whatever is made from these meshes once per setting and per neighbour.
    Graph::Batch batch(g);
    const std::vector<Node*> targets = meshesOf(g, a, cmd);
    const std::vector<Node*> meshes = allMeshes(g);
    g.evaluate();

    std::map<NodeId, geo::Bounds> bounds;
    for (Node* m : meshes)
        bounds[m->id()] = worldBounds(g, *m);
    const Style style = styleArg(a);
    json mods = json::array(), objects = json::array(), neighbours = json::object();
    std::vector<std::pair<Node*, GrungeNode*>> made;
    for (Node* mesh : targets)
    {
        GrungeNode* grunge = grungeOn(g, mesh->id());
        const bool fresh = !grunge;
        if (fresh)
        {
            const std::string mod = ops::execute("addModifier", {{"args", {mesh->path()}}, {"kind", "grunge"}});
            grunge = static_cast<GrungeNode*>(g.find(mod));
        }
        // A style resets the look once, when first given or changed, so look edits survive running the command again;
        // a node whose look predates the current defaults takes them too.
        if (fresh || a.get<std::string>("style", "realistic") != grunge->as<std::string>(at(*grunge, "style")) ||
            grunge->as<int64_t>(at(*grunge, "lookVersion")) < kLookVersion)
        {
            g.setValue({grunge->id(), at(*grunge, "lookVersion"), -1}, int64_t(kLookVersion));
            g.setValue({grunge->id(), at(*grunge, "style"), -1}, std::string(styleName(style)));
            const json look = styleLook(style);
            for (const auto& [k, v] : look.items())
                setValue(g, *grunge, k, v);
        }
        if (a.has("seed"))
            setValue(g, *grunge, "seed", a.flags["seed"]);
        for (const std::string& f : kGeoFlags)
            if (a.has(f))
                setValue(g, *grunge, f, a.flags[f]);
        if (a.has("unreal"))
            setValue(g, *grunge, "unrealChannels", a.flags["unreal"]);
        if (a.has("masks"))
        {
            std::vector<std::string> on = a.flags["masks"].is_string() ? std::vector<std::string>{a.get<std::string>("masks")}
                                                                       : a.flags["masks"].get<std::vector<std::string>>();
            for (const std::string& m : on)
                if (std::find(kMaskNames.begin(), kMaskNames.end(), m) == kMaskNames.end())
                    throw CommandError(std::string(cmd) + ": no mask " + m + " (edge, cavity, ao, dust, streak, fade)");
            for (int k = 0; k < kMaskCount; ++k)
                setValue(g, *grunge, lookName(k, kOn), std::find(on.begin(), on.end(), kMaskNames[size_t(k)]) != on.end());
        }
        if (a.has("look"))
        {
            if (!a.flags["look"].is_object())
                throw CommandError(std::string(cmd) + ": look is a dict of setting: value");
            for (const auto& [k, v] : a.flags["look"].items())
                setValue(g, *grunge, k, v);
        }
        made.push_back({mesh, grunge});
        mods.push_back(grunge->path());
        objects.push_back(mesh->path());
    }
    for (auto& [mesh, grunge] : made)
        rewireReaders(g, mesh->id());
    if (a.get<bool>("neighbours", true))
        for (auto& [mesh, grunge] : made)
            neighbours[mesh->path()] = wireNeighbours(g, *mesh, *grunge, meshes, bounds, a.get<double>("radius", 100.0));
    g.evaluate();
    for (auto& [mesh, grunge] : made)
        syncMatrices(g, *grunge);
    return {{"group", ""}, {"modifiers", mods}, {"objects", objects}, {"neighbours", neighbours}};
}

void registerMaskCommands(ext::Context& ctx)
{
    std::vector<FlagDef> geo = {
        {"edgeWidth", "ew", T::Float, nullptr, "How far wear runs from a convex edge, cm.", {}, {}, 0.0},
        {"edgeAngle", "ea", T::Float, nullptr, "Edges turning more than this, degrees, count as edges.", {}, {}, 1.0, 179.0},
        {"cavityWidth", "cw", T::Float, nullptr, "How far dirt runs from a concave edge, a touching object or the ground, cm.", {}, {}, 0.0},
        {"aoRadius", "ar", T::Float, nullptr, "How far occlusion looks, cm.", {}, {}, 0.0},
        {"streakLength", "sl", T::Float, nullptr, "How far streaks run down from sills and overhangs, cm.", {}, {}, 0.0},
        {"fadeHeight", "fh", T::Float, nullptr, "How high sides fade above the ground, cm.", {}, {}, 0.0},
        {"groundPlane", "gp", T::Bool, nullptr, "Treat the ground plane as ground as well as objects below."},
        {"groundHeight", "gh", T::Float, nullptr, "The ground plane's height, cm."},
    };
    std::vector<FlagDef> flags = {
        styleFlag(),
        {"seed", "s", T::Int, nullptr, "Another seed gives other occlusion sampling and breakup noise."},
        {"masks", "m", T::Any, nullptr, "The masks to show: any of edge, cavity, ao, dust, streak, fade (default all)."},
        {"neighbours", "nb", T::Bool, true, "Let each mesh's masks see the meshes near it (dirt where a wall meets a floor)."},
        {"radius", "r", T::Float, 100.0, "Meshes within this far of a mesh's bounds are its neighbours, cm.", {}, {}, 0.0},
        {"unreal", "u", T::String, nullptr, "Where the masks go for Unreal: auto, color, uv or off.", kUnrealModes},
        {"look", "lk", T::Any, nullptr, "Look settings by attribute name: {'edgeLevel': 0.4, 'dustColor': [0.6, 0.55, 0.5], ...}."},
    };
    flags.insert(flags.begin() + 2, geo.begin(), geo.end());
    ctx.registerCommand({
        .name = "grungeMask",
        .doc = "Add live grunge masks to meshes, or update the ones they have: edge wear, cavity dirt, occlusion, dust, streaks "
               "below openings and ledges, and fade near the ground, found from the geometry and the meshes around it, with no UVs "
               "and no bake. The look draws live in the viewport's materials; style=toon gives posterized bands, hard cuts, "
               "outlines and painted colours. Sends to Unreal as vertex colours or UV sets for MF_LilaGrunge (grungeSend). "
               "Run it again to pick up new neighbours. Returns {group, modifiers, objects, neighbours}.",
        .positionalDoc = "meshes (or groups)",
        .flags = flags,
        .run = [](const Args& a) -> json { return addMasks(Graph::current(), a, "grungeMask"); },
        .examples = {"grungeMask('Wall', 'Floor')", "grungeMask('Blocks', style='toon', masks=['edge', 'cavity', 'fade'])",
                     "grungeMask('Tower', look={'streakStrength': 1.0, 'streakColor': [0.05, 0.05, 0.04]}, streakLength=300)"},
    });

    ctx.registerCommand({
        .name = "grungeInfo",
        .doc = "A mesh's grunge masks: per mask the mean, the largest value and how many samples it covers (above 0.5), plus the "
               "samples evaluated, the geometric passes run, its neighbours and where the masks go for Unreal.",
        .positionalDoc = "mesh",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const std::vector<Node*> meshes = meshesOf(g, a, "grungeInfo");
            Node* mesh = meshes.front();
            GrungeNode* n = grungeOn(g, mesh->id());
            if (!n)
                throw CommandError("grungeInfo: " + mesh->name() + " has no grunge masks (grungeMask adds them)");
            syncAll(g);
            g.evaluate();
            const MeshPtr m = mesh->as<MeshPtr>(MeshNode::kOutMesh);
            json masks = json::object();
            for (int k = 0; k < kMaskCount; ++k)
            {
                const auto v = m ? m->cornerAttrs().get<float>(render::kGrungeAttrs[size_t(k)]) : std::span<const float>();
                double sum = 0.0, top = 0.0;
                int64_t count = 0, over = 0;
                for (geo::Index c = 0; m && c < v.size(); ++c)
                    if (m->cornerValid(c))
                    {
                        sum += v[c];
                        top = std::max(top, double(v[c]));
                        over += v[c] > 0.5f ? 1 : 0;
                        ++count;
                    }
                masks[kMaskNames[size_t(k)]] = {{"mean", count ? sum / double(count) : 0.0}, {"max", top}, {"over", over},
                                                {"enabled", n->as<bool>(at(*n, lookName(k, kOn)))}};
            }
            return {{"modifier", n->path()},
                    {"masks", masks},
                    {"samples", n->as<int64_t>(at(*n, "samples"))},
                    {"passes", n->passes()},
                    {"neighbours", g.elements({n->id(), at(*n, "neighbours"), -1}).size()},
                    {"unreal", n->as<std::string>(at(*n, "unrealLayout"))},
                    {"error", n->error()}};
        },
    });

    ctx.registerCommand({
        .name = "grungeBake",
        .doc = "Bake meshes' live grunge masks into texture layers: one fill layer per shown mask, coloured as the mask, masked by its "
               "twin generator (genEdgeWear, genDirt, genAoDarken, genDust, genDrips, genPositionGradient) with the mask's level, "
               "softness, breakup and seed. Goes into the set the mesh's material picks, or a new <mesh>_Grunge set. Bands, cuts "
               "and outlines have no generator twin and are listed in skipped. The live masks turn off unless keepLive. "
               "Needs UVs. Returns {set, layers, skipped, created} per mesh.",
        .positionalDoc = "meshes",
        .flags = {{"set", "ts", T::String, nullptr, "Texture set to add the layers to."},
                  {"keepLive", "kl", T::Bool, false, "Leave the live masks on as well."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            json out = json::object();
            for (Node* mesh : meshesOf(g, a, "grungeBake"))
            {
                GrungeNode* n = grungeOn(g, mesh->id());
                if (!n)
                    throw CommandError("grungeBake: " + mesh->name() + " has no grunge masks (grungeMask adds them)");
                json created = json::array(), layers = json::array(), skipped = json::array();
                const std::string set = bakeSet(g, *mesh, a, created);
                const double size = objectSize(*mesh);
                for (int k = 0; k < kMaskCount; ++k)
                {
                    if (!n->as<bool>(at(*n, lookName(k, kOn))))
                        continue;
                    const glm::dvec3 color = n->as<glm::dvec3>(at(*n, lookName(k, kColor)));
                    json material = {{"baseColor", colorJson(color)}};
                    if (const double r = n->as<double>(at(*n, lookName(k, kRoughness))); r >= 0.0)
                        material["roughness"] = r;
                    const std::string title = std::string("Grunge ") + kMaskTitles[size_t(k)];
                    const json layer = ops::execute("texAddLayer", {{"args", {set, "fill"}}, {"name", title}, {"material", material}});
                    const std::string id = layer.at("id").get<std::string>();
                    const BakeSpec spec = bakeSpec(*n, k, size);
                    ops::execute("texAddGenerator", {{"args", {set, id, spec.generator}}, {"values", spec.values},
                                                     {"opacity", n->as<double>(at(*n, lookName(k, kStrength)))}});
                    layers.push_back({{"mask", kMaskNames[size_t(k)]}, {"layer", id}, {"generator", spec.generator}});
                    for (int attr : {kBands, kCut, kOutline})
                        if (const PlugValue& v = n->value(at(*n, lookName(k, attr)));
                            (std::holds_alternative<int64_t>(v) && std::get<int64_t>(v) > 0) || (std::holds_alternative<double>(v) && std::get<double>(v) > 0.0))
                            skipped.push_back(lookName(k, attr));
                }
                if (!a.get<bool>("keepLive", false))
                    g.setValue({n->id(), n->enabledAttr, -1}, false);
                out[mesh->path()] = {{"set", set}, {"layers", layers}, {"skipped", skipped}, {"created", created}};
            }
            return out;
        },
        .examples = {"grungeBake('Crate')", "grungeBake('Crate', set='CrateSet', keepLive=True)"},
    });

    ctx.registerCommand({
        .name = "grungeFromLayers",
        .doc = "The reverse of grungeBake: live masks from a texture set's generator layers. Each layer masked by genEdgeWear, genDirt "
               "or genCavityGrime, genAoDarken, genDust, genDrips or genPositionGradient sets the matching live mask's level, softness, "
               "breakup and seed, and takes the layer's base colour and roughness. Masks with no such layer turn off. Returns the "
               "masks set per mesh.",
        .positionalDoc = "set meshes...",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.size() < 2 || !a.positional[0].is_string())
                throw CommandError("grungeFromLayers: give a texture set and meshes");
            const std::string set = a.positional[0].get<std::string>();
            const json stack = ops::execute("texLayerStack", {{"args", {set}}});
            Args rest = a;
            rest.positional.erase(rest.positional.begin());
            json out = json::object();
            for (Node* mesh : meshesOf(g, rest, "grungeFromLayers"))
            {
                if (!grungeOn(g, mesh->id()))
                {
                    Args add;
                    add.positional = json::array({mesh->path()});
                    addMasks(g, add, "grungeFromLayers");
                }
                GrungeNode* n = grungeOn(g, mesh->id());
                const double size = objectSize(*mesh);
                json found = json::object();
                for (const json& layer : stack.value("layers", json::array()))
                    for (const json& m : layer.value("masks", json::array()))
                    {
                        const char* mask = m.value("kind", "") == "generator" ? maskOfGenerator(m.value("type", "")) : nullptr;
                        if (!mask)
                            continue;
                        const int k = int(std::find(kMaskNames.begin(), kMaskNames.end(), std::string(mask)) - kMaskNames.begin());
                        const json v = m.value("values", json::object());
                        if (v.contains("level"))
                            setValue(g, *n, lookName(k, kLevel), v["level"].get<double>());
                        if (v.contains("softness"))
                            setValue(g, *n, lookName(k, kSoftness), v["softness"].get<double>());
                        if (v.contains("breakup"))
                            setValue(g, *n, lookName(k, kBreakup), v["breakup"].get<double>());
                        if (v.contains("noiseScale"))
                            setValue(g, *n, lookName(k, kNoiseScale), size / std::max(1.0, v["noiseScale"].get<double>()));
                        if (v.contains("seed"))
                            setValue(g, *n, "seed", v["seed"].get<int64_t>());
                        setValue(g, *n, lookName(k, kStrength), m.value("opacity", 1.0));
                        const json mat = layer.value("material", json::object());
                        if (mat.contains("baseColor") && mat["baseColor"].is_array() && mat["baseColor"].size() >= 3)
                            setValue(g, *n, lookName(k, kColor), json::array({mat["baseColor"][0], mat["baseColor"][1], mat["baseColor"][2]}));
                        if (mat.contains("roughness") && mat["roughness"].is_number())
                            setValue(g, *n, lookName(k, kRoughness), mat["roughness"].get<double>());
                        found[mask] = layer.value("id", "");
                    }
                for (int k = 0; k < kMaskCount; ++k)
                    setValue(g, *n, lookName(k, kOn), found.contains(kMaskNames[size_t(k)]));
                g.setValue({n->id(), n->enabledAttr, -1}, true);
                out[mesh->path()] = found;
            }
            return out;
        },
        .examples = {"grungeFromLayers('CrateSet', 'Crate')"},
    });
}
} // namespace masks

bool registerMasks(ext::Context& ctx)
{
    if (!masks::registerMaskNode(ctx))
        return false;
    masks::registerMaskCommands(ctx);
    masks::registerMaskUnreal(ctx);
    masks::registerMaskUi(ctx);
    return true;
}
} // namespace weathering
