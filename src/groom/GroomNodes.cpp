#include "groom/GroomNodes.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "curve/NodeHelpers.h"
#include "curve/Pipe.h"
#include "groom/Attrs.h"
#include "groom/GroomEvaluator.h"
#include "groom/GroomGraph.h"
#include "groom/GroomLab.h"
#include "groom/GroomLabBridge.h"
#include "groom/GuideData.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <sstream>

namespace lila::groom
{
using namespace scene;
using namespace curve::helpers;
using curve::makeCurveData;
using curve::splinesOf;

namespace
{
std::vector<std::string> profileNames()
{
    std::vector<std::string> out;
    for (int p = 0; p < int(LockProfile::Count); ++p)
        out.push_back(profileName(LockProfile(p)));
    return out;
}

/** "x,y x,y ..." pairs. */
std::vector<glm::dvec2> parsePairs(const std::string& text)
{
    std::vector<double> n = curve::parseNumbers(text);
    std::vector<glm::dvec2> out;
    for (size_t i = 0; i + 1 < n.size(); i += 2)
        out.push_back({n[i], n[i + 1]});
    return out;
}

geo::Mesh transformedMesh(const geo::Mesh& in, const glm::dmat4& m)
{
    geo::Mesh out = in;
    for (geo::Index p = 0; p < out.pointCapacity(); ++p)
        if (out.pointValid(p))
            out.setPosition(p, D3(m * glm::dvec4(out.position(p), 1.0)));
    return out;
}

/** The scalp in world space from a node's inMesh and meshMatrix, rebuilt only when either changes. */
struct ScalpCache
{
    const geo::Mesh* mesh = nullptr;
    glm::dmat4 matrix{0.0};
    std::unique_ptr<Surface> surface;

    const Surface* get(const Node& n)
    {
        const MeshPtr& m = n.as<MeshPtr>(idx(n, "inMesh"));
        const glm::dmat4& mm = n.as<glm::dmat4>(idx(n, "meshMatrix"));
        if (m.get() != mesh || mm != matrix)
        {
            mesh = m.get();
            matrix = mm;
            surface.reset();
            if (m && m->numFaces() > 0)
                surface = std::make_unique<Surface>(transformedMesh(*m, mm));
        }
        return surface.get();
    }
};

/** A node with a warning output it logs when it changes. */
class WarningNode : public Node
{
protected:
    void report(const std::vector<std::string>& warnings)
    {
        std::string w;
        for (const std::string& s : warnings)
            w += (w.empty() ? "" : "; ") + s;
        if (!w.empty() && w != as<std::string>(idx(*this, "warning")))
            log::warn("{}: {}", name(), w);
        setOutput(idx(*this, "warning"), w);
    }
};

/** The shell: resolved locks into one surface, re-meshing only what a change touched. */
class ShellNode final : public WarningNode
{
public:
    void compute(const EvalContext&) override
    {
        LILA_ZONE();
        try
        {
            const Splines& resolved = splinesOf(value(idx(*this, "inCurve")));
            std::shared_ptr<const geo::Mesh> m = mBuilder.update(resolved, shellOptionsOf(*this));
            setOutput(idx(*this, "outMesh"), MeshPtr(std::const_pointer_cast<geo::Mesh>(m)));
            report({});
        }
        catch (const std::exception& e)
        {
            report({e.what()});
            setOutput(idx(*this, "outMesh"), MeshPtr());
        }
    }

private:
    ShellBuilder mBuilder;
};

} // namespace

std::string groomSurfaceOf(const Node& curve)
{
    const int a = curve.type().attrIndex("groomSurface");
    return a < 0 ? std::string() : curve.as<std::string>(a);
}

LockOptions lockOptionsOf(const Node& n)
{
    LockOptions o;
    o.width = std::max(1e-3, f(n, "width"));
    o.thickness = std::clamp(f(n, "thickness"), 0.02, 4.0);
    o.profile = LockProfile(std::clamp<int64_t>(i(n, "profile"), 0, int(LockProfile::Count) - 1));
    o.customProfile = parsePairs(s(n, "customProfile"));
    o.widthRamp = curve::parseRamp(s(n, "widthRamp"));
    o.thicknessRamp = curve::parseRamp(s(n, "thicknessRamp"));
    o.twist = glm::radians(f(n, "twist"));
    o.twistRamp = curve::parseRamp(s(n, "twistRamp"));
    o.flatten = std::clamp(f(n, "flatten"), 0.0, 1.0);
    o.rest = b(n, "rest");
    o.lift = f(n, "lift");
    o.strayWidth = std::max(1e-3, f(n, "strayWidth"));
    o.segments = int(std::clamp<int64_t>(i(n, "segments"), 0, 512));
    o.segmentLength = std::max(0.05, f(n, "segmentLength"));
    o.sides = int(std::clamp<int64_t>(i(n, "sides"), 3, 64));
    o.caps = b(n, "caps");
    o.columns = int(std::clamp<int64_t>(i(n, "atlasColumns"), 1, 64));
    o.rows = int(std::clamp<int64_t>(i(n, "atlasRows"), 1, 64));
    o.seed = uint32_t(i(n, "seed"));
    return o;
}

ShellOptions shellOptionsOf(const Node& n)
{
    ShellOptions o;
    o.voxelSize = std::max(0.02, f(n, "voxelSize"));
    o.blend = std::max(0.0, f(n, "blend"));
    o.inflate = f(n, "inflate");
    o.segments = int(std::clamp<int64_t>(i(n, "segments"), 0, 256));
    o.skipStrays = b(n, "skipStrays");
    o.projectSteps = int(std::clamp<int64_t>(i(n, "projectSteps"), 0, 4));
    o.gpu = b(n, "gpu");
    return o;
}


std::optional<std::pair<D3, D3>> mirrorPlane(const Node& curve)
{
    const int axis = curve::symmetryAxis(curve);
    if (axis < 0)
        return std::nullopt;
    const int space = curve.type().attrIndex("symmetrySpace");
    if (space < 0 || curve.as<int64_t>(space) == 0)
    {
        D3 n(0.0);
        n[axis] = 1.0;
        return std::pair{D3(0.0), n};
    }
    const glm::dmat4 w = curve.as<glm::dmat4>(Transform::kWorldMatrix);
    const D3 n = glm::cross(D3(w[(axis + 1) % 3]), D3(w[(axis + 2) % 3]));
    if (glm::length(n) < 1e-12)
        return std::nullopt;
    return std::pair{D3(w[3]), glm::normalize(n)};
}

bool mirrorDabToSource(const Node& curve, const Splines& ownLocal, D3& center, D3& normal, D3& delta, D3& view)
{
    const auto plane = mirrorPlane(curve);
    if (!plane)
        return false;
    const auto [at, n] = *plane;
    auto point = [&](const D3& p) { return p - n * (2.0 * glm::dot(p - at, n)); };
    auto dir = [&](const D3& v) { return v - n * (2.0 * glm::dot(v, n)); };
    const glm::dmat4 w = curve.as<glm::dmat4>(Transform::kWorldMatrix);
    const D3 flipped = point(center);
    double direct = 1e300, mirrored = 1e300;
    for (const Spline& s : ownLocal)
        for (const D3& p : s.points)
        {
            const D3 wp(w * glm::dvec4(p, 1.0));
            direct = std::min(direct, glm::distance(wp, center));
            mirrored = std::min(mirrored, glm::distance(wp, flipped));
        }
    if (!(mirrored < direct))
        return false;
    center = flipped;
    normal = dir(normal);
    delta = dir(delta);
    view = dir(view);
    return true;
}

Node* groomOf(Graph& g, const Node& curve)
{
    // The guides curve shows a groom's outGuides.
    if (auto src = g.source({curve.id(), curve::CurveShape::kCreate, -1}))
        if (Node* n = g.node(src->node); n && n->type().isA("groom") && src->attr == idx(*n, "outGuides"))
            return n;
    return nullptr;
}

Node* guidesOf(Graph& g, const Node& groom)
{
    for (const PlugRef& d : g.destinations({groom.id(), idx(groom, "outGuides"), -1}))
        if (Node* n = g.node(d.node); n && n->type().isA("curve") && d.attr == curve::CurveShape::kCreate)
            return n;
    return nullptr;
}

Node* findGroom(Graph& g, const Node& part)
{
    if (part.type().isA("groom"))
        return const_cast<Node*>(&part);
    if (part.type().isA("curve"))
        return groomOf(g, part);
    if (part.type().isA("mesh") || part.type().isA("groomShell"))
    {
        const NodeId start = part.id();
        PlugRef in = part.type().isA("mesh") ? PlugRef{start, MeshNode::kInMesh, -1} : PlugRef{start, idx(part, "inCurve"), -1};
        for (int depth = 0; depth < 4; ++depth)
        {
            auto src = g.source(in);
            Node* n = src ? g.node(src->node) : nullptr;
            if (!n)
                return nullptr;
            if (n->type().isA("groom"))
                return n;
            if (!n->type().isA("groomShell"))
                return nullptr;
            in = {n->id(), idx(*n, "inCurve"), -1};
        }
    }
    return nullptr;
}

void registerHairCardsType();

void registerTypes()
{
    if (types::find("groom"))
        return;
    registerHairCardsType();
    registerGroomLabType();
    const std::string lock = "Lock", mesh = "Mesh", uv = "Atlas", graphCat = "Groom";
    NodeType t;
    t.name = "groom";
    t.icon = "feather";
    t.timeDependent = true;
    t.doc = "A groom: its guides through its graph into locks swept along them (a profile with width, thickness and twist "
            "ramps from root to tip, lying flat on the scalp near it, one mesh with uvs along each lock) and strands.";
    t.ownAttrs = {
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden, .doc = "The scalp."},
        {.name = "meshMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden, .doc = "The scalp's world matrix."},
        {.name = "inCurve", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = kIn | AttrHidden, .doc = "More guides: curves, world space, rooted on the scalp at their first point."},
        {.name = "hierarchy", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrHidden, .doc = "The guides: a GroomLab guide hierarchy as JSON."},
        {.name = "parts", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrHidden, .doc = "Parts drawn on the scalp: a GroomLab part set as JSON."},
        {.name = "flow", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrHidden, .doc = "Flow arrows and sinks on the scalp: a GroomLab flow set as JSON."},
        {.name = "scalpMaps", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrHidden, .doc = "Maps painted on the scalp's vertices: GroomLab scalp maps as JSON."},
        {.name = "sculpts", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrHidden, .doc = "Sculpt nodes' stored edits: a GroomLab sculpt set as JSON."},
        {.name = "cardMaps", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrHidden, .doc = "The last card bake's maps (groomBakeCards): cards keep their atlas cells from it."},
        {.name = "correctives", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrHidden, .doc = "Shot sculpts: GroomLab correctives as JSON."},
    };
    for (AttrDef d : std::vector<AttrDef>{
             sAttrDefault("graph", "", "The groom graph as JSON; empty: guides into locks, strands interpolated from them.", graphCat),
             eAttr("symmetry", {"off", "x", "y", "z"}, 1, "Mirrors the guides across the world plane facing this axis.", graphCat),
             bAttr("useSceneTime", true, "The graph runs at the scene's time; off, at Time.", graphCat),
             fAttr("time", 0.0, "Seconds the graph runs at while Use Scene Time is off.", graphCat),
             bAttr("scaleWithScalp", true, "The graph's lengths, widths and densities scale with the scalp's area against Scale Reference.", graphCat),
             fAttr("scaleReference", 0.0, "The scalp area the graph's sizes are 1x at; 0 takes the area on the next evaluation.", graphCat),
             fAttr("width", 2.0, "Lock width, cm, times each lock's width and its points' radius.", lock, 0.0),
             fAttr("thickness", 0.35, "Thickness as a fraction of width; 1 is round.", lock, 0.02, 4.0),
             eAttr("profile", profileNames(), int64_t(LockProfile::Ribbon), "Cross-section of every lock without one of its own.", lock),
             sAttrDefault("customProfile", "", "Custom profile: \"x,y x,y ...\" around a closed outline, x across the width.", lock),
             sAttrDefault("widthRamp", "0:0.55 0.12:1 0.7:0.8 1:0.08", "Width from root to tip: keys \"t:scale\".", lock),
             sAttrDefault("thicknessRamp", "", "Thickness from root to tip: keys \"t:scale\"; empty keeps it.", lock),
             fAttr("twist", 0.0, "Twist from root to tip, degrees.", lock),
             sAttrDefault("twistRamp", "", "How the twist builds from root to tip: keys \"t:fraction\"; empty is even.", lock),
             fAttr("flatten", 1.0, "How strongly locks turn their flat side to the scalp where they lie on it, 0 to 1.", lock, 0.0, 1.0),
             bAttr("rest", true, "Locks rest on the scalp instead of sinking into it.", lock),
             fAttr("lift", 0.0, "Gap between the locks and the scalp, cm.", lock),
             fAttr("strayWidth", 0.08, "Width of stray strands, cm.", lock, 0.0),
             fAttr("segmentLength", 0.75, "Spacing of rings along each lock, cm.", mesh, 0.05),
             iAttr("segments", 0, "Rings along each lock; 0 follows segmentLength.", mesh, 0),
             iAttr("sides", 10, "Points around each lock.", mesh, 3),
             bAttr("caps", true, "Close the root and tip.", mesh),
             iAttr("atlasColumns", 1, "Texture atlas cells across; each lock takes one by its id.", uv, 1),
             iAttr("atlasRows", 1, "Texture atlas cells down.", uv, 1),
             iAttr("seed", 1, "Seed for picking atlas cells.", uv),
         })
        t.ownAttrs.push_back(std::move(d));
    t.ownAttrs.push_back(dataOut("outGuides", "The guides as lock curves, world space: what the guides curve shows."));
    t.ownAttrs.push_back(meshOut());
    t.ownAttrs.push_back(dataOut("outCurve", "The resolved locks (curves with their cross-sections), world space."));
    t.ownAttrs.push_back(dataOut("outStrands", "The graph's strands, world space, with a width per point."));
    t.ownAttrs.push_back({.name = "outCards", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput, .doc = "The graph's Cards nodes' cards as one mesh: uv0 their atlas cells, uv1 a layout of their own."});
    t.ownAttrs.push_back({.name = "lockCount", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrOutput, .doc = "Locks made."});
    t.ownAttrs.push_back({.name = "strandCount", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrOutput, .doc = "Strands grown."});
    t.ownAttrs.push_back(stringOut("warning", "Problems with the last build."));
    t.create = [] { return makeGroomNode(); };
    types::registerType(std::move(t));

    NodeType sh;
    sh.name = "groomShell";
    sh.icon = "blob";
    sh.doc = "One watertight surface over a groom's locks, smooth-unioned and meshed with surface nets, ready for retopology.";
    const std::string c = "Shell";
    sh.ownAttrs = {
        {.name = "inCurve", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = kIn | AttrHidden, .doc = "Resolved locks (a groom's outCurve)."},
        fAttr("voxelSize", 0.3, "Mesh resolution, cm.", c, 0.02),
        fAttr("blend", 0.6, "Smooth union between locks, cm: larger fills the gaps between them.", c, 0.0),
        fAttr("inflate", 0.0, "Grows every lock, cm.", c),
        iAttr("segments", 16, "Round cones per lock; 0 uses every ring.", c, 0),
        bAttr("skipStrays", true, "Leave stray strands out.", c),
        iAttr("projectSteps", 1, "Steps moving vertices onto the surface.", c, 0),
        bAttr("gpu", false, "Sample the field on the GPU when one is available.", c),
        meshOut(),
        stringOut("warning", "Problems with the last build."),
    };
    sh.create = [] { return std::make_unique<ShellNode>(); };
    types::registerType(std::move(sh));
}
} // namespace lila::groom
