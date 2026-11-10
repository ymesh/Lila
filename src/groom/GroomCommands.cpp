#include "groom/GroomEdit.h"
#include "groom/GroomNodes.h"

#include "brush/Brush.h"
#include "groom/Attrs.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "curve/CurveNodes.h"
#include "curve/NodeHelpers.h"
#include "curve/Pipe.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <algorithm>
#include <map>
#include <set>

namespace lila::groom
{
using namespace scene;
using namespace curve;
using namespace curve::helpers;
using json = nlohmann::json;
using D3 = glm::dvec3;
using T = ArgType;

namespace
{
const std::vector<std::string> kSources = {"curves", "clumps", "guides"};
const std::vector<std::string> kFacings = {"surfaceNormal", "curveFrame", "upVector"};

geo::Mesh transformed(const geo::Mesh& in, const glm::dmat4& m)
{
    geo::Mesh out = in;
    for (geo::Index p = 0; p < out.pointCapacity(); ++p)
        if (out.pointValid(p))
            out.setPosition(p, D3(m * glm::dvec4(out.position(p), 1.0)));
    return out;
}

/** The hairCards node: rebuilds its card mesh whenever the curves, guides, scalp or settings change. */
class HairCardsNode final : public Node
{
public:
    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        const int out = idx(*this, "outMesh");
        try
        {
            const MeshPtr& mesh = as<MeshPtr>(idx(*this, "inMesh"));
            const glm::dmat4& mm = as<glm::dmat4>(idx(*this, "meshMatrix"));
            // The scalp's BVH is kept while the mesh and its matrix stay the same.
            if (mesh.get() != mScalpFor || mm != mScalpMatrix)
            {
                mScalpFor = mesh.get();
                mScalpMatrix = mm;
                mScalp.reset();
                if (mesh && mesh->numFaces() > 0)
                    mScalp = std::make_unique<groom::Surface>(transformed(*mesh, mm));
            }
            CardResult r = hairCards(splinesOf(value(idx(*this, "inCurve"))), splinesOf(value(idx(*this, "guideCurve"))), mScalp.get(), cardOptionsOf(*this));
            std::string warning;
            for (const std::string& w : r.warnings)
                warning += (warning.empty() ? "" : "; ") + w;
            if (warning != as<std::string>(idx(*this, "warning")) && !warning.empty())
                log::warn("{}: {}", name(), warning);
            setOutput(idx(*this, "warning"), warning);
            setOutput(idx(*this, "cardCount"), int64_t(r.cards));
            setOutput(out, MeshPtr(std::make_shared<geo::Mesh>(std::move(r.mesh))));
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", name(), e.what());
            setOutput(out, MeshPtr());
        }
    }

private:
    const geo::Mesh* mScalpFor = nullptr;
    glm::dmat4 mScalpMatrix{0.0};
    std::unique_ptr<groom::Surface> mScalp;
};

std::vector<FlagDef> nodeFlags()
{
    std::vector<FlagDef> out;
    for (const AttrDef& d : types::find("hairCards")->ownAttrs)
    {
        if (!(d.flags & AttrInput) || (d.flags & AttrHidden))
            continue;
        out.push_back(curve::helpers::flagFromAttr(d));
    }
    return out;
}

Node& curveNode(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n || !n->type().isA("curve"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a curve");
    return *n;
}

Node& meshNode(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return *n;
}

/** The curves among the positional names, else the selected curves and those holding selected points. */
std::vector<Node*> curvesOrPointSelection(Graph& g, const Args& a, const char* cmd)
{
    std::vector<Node*> out;
    for (const json& p : a.positional)
        out.push_back(&curveNode(g, p.get<std::string>(), cmd));
    if (out.empty())
    {
        for (NodeId id : g.selection())
            if (Node* n = g.node(id); n && n->type().isA("curve"))
                out.push_back(n);
        for (auto& [id, ids] : components::selected(ComponentMode::Vertex))
            if (Node* n = g.node(id); n && n->type().isA("curve") && std::ranges::find(out, n) == out.end())
                out.push_back(n);
    }
    if (out.empty())
        throw CommandError(std::string(cmd) + ": select curves");
    return out;
}

D3 v3(const json& j, const D3& def = D3(0.0))
{
    if (!j.is_array() || j.size() != 3)
        return def;
    return D3(j[0].get<double>(), j[1].get<double>(), j[2].get<double>());
}

/** The surface named, else the curve's groomSurface, as a groom surface in the curve's object space; null for none. */
std::unique_ptr<groom::Surface> surfaceFor(Graph& g, Node& curve, const std::string& given, const std::string& densitySet, const char* cmd)
{
    std::string name = given.empty() ? groomSurfaceOf(curve) : given;
    if (name.empty())
        return nullptr;
    Node* mesh = g.find(name);
    if (!mesh || !mesh->type().isA("mesh"))
    {
        if (!given.empty())
            throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
        log::warn("{}: its groom surface {} is gone", curve.name(), name);
        return nullptr;
    }
    g.evaluate();
    MeshPtr m = mesh->as<MeshPtr>(MeshNode::kOutMesh);
    if (!m || m->numFaces() == 0)
        return nullptr;
    const glm::dmat4 toCurve = glm::inverse(curve.as<glm::dmat4>(Transform::kWorldMatrix)) * mesh->as<glm::dmat4>(Transform::kWorldMatrix);
    return std::make_unique<groom::Surface>(transformed(*m, toCurve), densitySet);
}

void storeSplines(Graph& g, Node& n, Splines s)
{
    groom::storeGuides(g, n, std::move(s));
}

} // namespace

void lockBrushSettings(Graph& g, Node& guides, const Splines& s, const glm::dmat4& worldToCurve, Settings& o)
{
    Node* groom = groomOf(g, guides);
    const LockOptions lo = groom ? lockOptionsOf(*groom) : LockOptions{};
    // Radii in the curve's object space: the lock settings are world sizes.
    const double scale = std::cbrt(std::abs(glm::determinant(glm::dmat3(worldToCurve))));
    o.radii = collisionRadii(s, lo);
    for (auto& r : o.radii)
        for (double& v : r)
            v *= scale;
    o.gravity = D3(worldToCurve * glm::dvec4(0.0, -981.0, 0.0, 0.0));
}

namespace
{
void registerGroomOptionLayout()
{
    namespace opt = ops::options;
    opt::Override o;
    o.order = {"source", "width", "taper", "segments", "facing", "atlasColumns", "atlasRows", "variations"};
    o.hidden = {"guides", "mesh", "name"};
    for (const AttrDef& d : types::find("hairCards")->ownAttrs)
        if (!d.category.empty())
            o.sections[d.name] = d.category;
    o.ranges["width"] = {0.001, 1e4, 0.1, " cm"};
    o.ranges["offset"] = {-100.0, 100.0, 0.05, " cm"};
    o.ranges["tilt"] = {-360.0, 360.0, 1.0, " deg"};
    o.ranges["twist"] = {-3600.0, 3600.0, 5.0, " deg"};
    o.labels = {{"taper", "Taper (t:scale ...)"}, {"segments", "Segments (0 = curve points)"}, {"variations", "Variations (0 = all cells)"}};
    opt::setOverride("hairCards", std::move(o));
}
} // namespace

CardOptions cardOptionsOf(const Node& n)
{
    CardOptions o;
    o.source = CardSource(std::clamp<int64_t>(i(n, "source"), 0, 2));
    o.clumps = int(std::max<int64_t>(1, i(n, "clumps")));
    o.width = std::max(1e-4, f(n, "width"));
    o.autoWidth = b(n, "autoWidth");
    o.widthScale = std::max(0.0, f(n, "widthScale"));
    o.taper = parseRamp(n.as<std::string>(idx(n, "taper")));
    o.segments = int(std::clamp<int64_t>(i(n, "segments"), 0, 512));
    o.widthSegments = int(std::clamp<int64_t>(i(n, "widthSegments"), 1, 64));
    o.facing = CardFacing(std::clamp<int64_t>(i(n, "facing"), 0, 2));
    const D3 up = n.as<D3>(idx(n, "upVector"));
    o.up = glm::length(up) > 1e-9 ? glm::normalize(up) : D3(0, 1, 0);
    o.tilt = glm::radians(f(n, "tilt"));
    o.twist = glm::radians(f(n, "twist"));
    o.offset = f(n, "offset");
    o.columns = int(std::clamp<int64_t>(i(n, "atlasColumns"), 1, 64));
    o.rows = int(std::clamp<int64_t>(i(n, "atlasRows"), 1, 64));
    o.variations = int(std::max<int64_t>(0, i(n, "variations")));
    o.seed = uint32_t(i(n, "seed"));
    o.flipU = b(n, "flipU");
    o.flipV = b(n, "flipV");
    o.padding = std::clamp(f(n, "padding"), 0.0, 0.45);
    o.doubleSided = b(n, "doubleSided");
    return o;
}

void registerHairCardsType()
{
    if (types::find("hairCards"))
        return;
    const std::string src = "Source", card = "Card", uv = "Atlas";
    NodeType t;
    t.name = "hairCards";
    t.icon = "brush";
    t.doc = "Textured hair cards along curves, clumps of curves or guide curves: tapered strips facing the scalp or following the "
            "curve, with twist, laid out per card in atlas cells picked at random.";
    t.ownAttrs = {
        {.name = "inCurve", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = kIn | AttrHidden, .doc = "Hair curves, world space."},
        {.name = "guideCurve", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = kIn | AttrHidden, .doc = "Guide curves (source = guides), world space."},
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden, .doc = "The scalp the cards face."},
        {.name = "meshMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden, .doc = "The scalp's world matrix."},
        eAttr("source", kSources, 0, "One card per curve, per clump of curves (by their roots), or per guide curve.", src),
        iAttr("clumps", 20, "Clumps the curves are gathered into (source = clumps).", src, 1),
        fAttr("width", 1.0, "Card width at the root, cm.", card, 0.0),
        bAttr("autoWidth", false, "Size each clump or guide card to the spread of its curves.", card),
        fAttr("widthScale", 1.0, "Scale of the automatic width.", card, 0.0),
        sAttrDefault("taper", "0:1 1:0.3", "Width along the card: keys \"t:scale\", t from 0 at the root to 1 at the tip.", card),
        iAttr("segments", 8, "Quads along each card; 0 follows each curve's own points.", card),
        iAttr("widthSegments", 1, "Quads across each card.", card, 1),
        eAttr("facing", kFacings, 0, "Cards lie flat to the scalp, follow rotation minimising curve frames, or face the up vector.", card),
        vAttr("upVector", D3(0, 1, 0), "Up for facing = upVector, and without a scalp.", card),
        fAttr("tilt", 0.0, "Turn of every card about its curve, degrees.", card),
        fAttr("twist", 0.0, "Twist from root to tip, degrees.", card),
        fAttr("offset", 0.0, "Lift off the scalp along the card's facing, cm.", card),
        bAttr("doubleSided", false, "Give each card a back face.", card),
        iAttr("atlasColumns", 4, "Texture atlas cells across.", uv, 1),
        iAttr("atlasRows", 1, "Texture atlas cells down.", uv, 1),
        iAttr("variations", 0, "Use only the first this many cells; 0 uses them all.", uv),
        iAttr("seed", 1, "Random seed for picking cells.", uv),
        bAttr("flipU", false, "Mirror each card's u.", uv),
        bAttr("flipV", false, "Root at the bottom of its cell instead of the top.", uv),
        fAttr("padding", 0.0, "Gap inside each cell, as a fraction of it.", uv, 0.0, 0.45),
        {.name = "outMesh", .shortName = "o", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden, .doc = "The card mesh."},
        {.name = "cardCount", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrOutput, .doc = "Cards made."},
        {.name = "warning", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput, .doc = "Problems with the last build."},
    };
    t.create = [] { return std::make_unique<HairCardsNode>(); };
    types::registerType(std::move(t));
}

void registerLockCommands();

void registerCommands()
{
    registerLockCommands();
    std::vector<std::string> brushes;
    for (int k = 0; k < int(groom::Brush::Count); ++k)
        brushes.push_back(groom::brushName(groom::Brush(k)));

    ops::registerCommand({
        .name = "curveSculptStroke",
        .doc = "One curve sculpting stroke on a curve holding many curves (hair): comb, smooth, length, puff, add, delete, density, cut, "
               "pinch or slide, and on a groom's guides the lock brushes grab, twist, widen, flatten, curl, frizz, clump, split, merge and settle. dabs is a list of {center, normal, radius, strength, delta, view} in world space, as the Sculpt Curves "
               "tool records them. Roots stay put (on the groom surface) except under slide and add. Returns {changed, added, removed, curves}.",
        .positionalDoc = "curve",
        .flags = {{"brush", "b", T::String, "comb", "The brush.", brushes},
                  {"dabs", "d", T::List, nullptr, "[{center, normal, radius, strength, delta, view}, ...] world space; radius in cm."},
                  {"falloff", "", T::String, "smooth", "Falloff: smooth, sphere, root, sharp, linear, constant, bell, or a soft select preset name."},
                  {"strength", "s", T::Float, 0.5, "Brush strength, 0 to 1.", {}, {}, 0.0, 1.0},
                  {"invert", "i", T::Bool, false, "The opposite: shrink, flatten, spread, thin out."},
                  {"keepLength", "", T::Bool, true, "Comb, smooth, puff and pinch keep segment lengths."},
                  {"frontOnly", "", T::Bool, true, "Leave points behind the surface under the brush alone."},
                  {"addCount", "", T::Int, 5, "Add and density: curves planted per dab.", {}, {}, 1.0, 1000.0},
                  {"addLength", "", T::Float, 5.0, "Add: length of new curves with no neighbours to copy, cm.", {}, {}, 0.0},
                  {"addPoints", "", T::Int, 8, "Add: points per new curve.", {}, {}, 2.0, 256.0},
                  {"interpolate", "", T::Bool, true, "Add: new curves take the shape of their nearest neighbours."},
                  {"minDistance", "", T::Float, 0.0, "Add and density: closest roots may be, cm (0: no limit for add, from density for density).", {}, {}, 0.0},
                  {"density", "", T::Float, 1.0, "Density: target curves per square cm, times the density map.", {}, {}, 0.0},
                  {"densitySet", "", T::String, nullptr, "Vertex color set of the surface used as the density map."},
                  {"seed", "", T::Int, 0, "Random seed for add, delete and density."},
                  nodeFlag({"surface", "", T::String, nullptr, "Mesh the roots grow from; default the curve's groomSurface."}, "mesh"),
                  {"selectedOnly", "", T::Bool, true, "With points of the curve selected, only the curves holding them change."},
                  {"settleWhileGrabbing", "", T::Bool, false, "Grab: the locks near the grabbed ones settle as it moves, making room."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& c = *curvesOrPointSelection(g, a, "curveSculptStroke").front();
            Settings o;
            if (!groom::parseBrush(a.get<std::string>("brush", "comb"), &o.brush))
                throw CommandError("curveSculptStroke: unknown brush '" + a.get<std::string>("brush") + "'");
            const brush::Falloff falloff = brush::Falloff::fromName(a.get<std::string>("falloff", "smooth"));
            o.falloff = [falloff](double t) { return falloff.evaluate(t); };
            o.strength = std::clamp(a.get<double>("strength", 0.5), 0.0, 1.0);
            o.invert = a.get<bool>("invert");
            o.keepLength = a.get<bool>("keepLength", true);
            o.frontOnly = a.get<bool>("frontOnly", true);
            o.addCount = std::clamp(a.get<int>("addCount", 5), 1, 1000);
            o.addLength = std::max(1e-4, a.get<double>("addLength", 5.0));
            o.addPoints = std::clamp(a.get<int>("addPoints", 8), 2, 256);
            o.interpolate = a.get<bool>("interpolate", true);
            o.minDistance = std::max(0.0, a.get<double>("minDistance", 0.0));
            o.density = std::max(1e-6, a.get<double>("density", 1.0));
            o.settleWhileGrabbing = a.get<bool>("settleWhileGrabbing");
            g.evaluate();
            Splines s = splinesOf(g.value({c.id(), CurveShape::kLocal, -1}));
            if (a.get<bool>("selectedOnly", true))
            {
                const auto& sel = components::selected(c.id(), ComponentMode::Vertex);
                if (!sel.empty())
                {
                    o.mask.assign(s.size(), 0);
                    for (uint32_t id : sel)
                    {
                        size_t si, pi;
                        PointRole r;
                        decodeComponent(id, &si, &pi, &r);
                        if (si < s.size())
                            o.mask[si] = 1;
                    }
                }
            }
            std::unique_ptr<groom::Surface> surface = surfaceFor(g, c, a.get<std::string>("surface"), a.get<std::string>("densitySet"), "curveSculptStroke");
            const glm::dmat4 w = c.as<glm::dmat4>(Transform::kWorldMatrix), inv = glm::inverse(w);
            if (groom::isLockBrush(o.brush))
                lockBrushSettings(g, c, s, inv, o);
            attr::ensureIds(s);
            const glm::dmat3 nrm = glm::transpose(glm::dmat3(w));
            const double scale = std::max(1e-12, std::cbrt(std::abs(glm::determinant(glm::dmat3(w)))));
            std::mt19937 rng(uint32_t(a.get<int64_t>("seed", 0)));
            groom::Stats stats;
            for (const json& j : a.flags.value("dabs", json::array()))
            {
                groom::Dab d;
                D3 wc = v3(j.value("center", json())), wn = v3(j.value("normal", json()), D3(0, 1, 0)), wd = v3(j.value("delta", json())),
                   wv = v3(j.value("view", json()));
                mirrorDabToSource(c, s, wc, wn, wd, wv);
                d.center = D3(inv * glm::dvec4(wc, 1.0));
                const D3 n = nrm * wn;
                d.normal = glm::length(n) > 1e-12 ? glm::normalize(n) : D3(0, 1, 0);
                d.radius = j.value("radius", 1.0) / scale;
                d.strength = j.value("strength", j.value("pressure", 1.0) * j.value("opacity", 1.0));
                d.delta = D3(inv * glm::dvec4(wd, 0.0));
                const D3 view = D3(inv * glm::dvec4(wv, 0.0));
                d.viewDir = glm::length(view) > 1e-12 ? glm::normalize(view) : D3(0.0);
                groom::applyDab(s, o, d, surface.get(), rng, &stats);
            }
            for (const std::string& warn : stats.warnings)
                log::warn("curveSculptStroke: {}", warn);
            if (s.empty())
                throw CommandError("curveSculptStroke: that would remove every curve");
            storeSplines(g, c, std::move(s));
            return {{"changed", stats.changed}, {"added", stats.added}, {"removed", stats.removed},
                    {"curves", splinesOf(g.value({c.id(), CurveShape::kLocal, -1})).size()}};
        },
    });

    ops::registerCommand({
        .name = "groomSurface",
        .doc = "Set the mesh curves grow from (their groom surface): sculpting keeps roots on it, add and density plant on it. "
               "attach moves each curve so its root sits on the surface. An empty mesh clears it. Returns the curves moved.",
        .positionalDoc = "curves",
        .flags = {nodeFlag({"mesh", "m", T::String, nullptr, "The surface mesh; empty clears it."}, "mesh"),
                  {"attach", "", T::Bool, true, "Move roots onto the surface."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<Node*> curves = curvesOrPointSelection(g, a, "groomSurface");
            const std::string mesh = a.get<std::string>("mesh");
            if (!mesh.empty())
                meshNode(g, mesh, "groomSurface");
            int moved = 0;
            for (Node* c : curves)
            {
                g.setValue({c->id(), c->type().attrIndex("groomSurface"), -1}, mesh.empty() ? std::string() : g.find(mesh)->path());
                if (mesh.empty() || !a.get<bool>("attach", true))
                    continue;
                std::unique_ptr<groom::Surface> surface = surfaceFor(g, *c, mesh, {}, "groomSurface");
                if (!surface)
                    continue;
                g.evaluate();
                Splines s = splinesOf(g.value({c->id(), CurveShape::kLocal, -1}));
                const int n = groom::attachRoots(s, *surface);
                if (n)
                    storeSplines(g, *c, std::move(s));
                moved += n;
            }
            return moved;
        },
    });

    ops::registerCommand({
        .name = "groomGuides",
        .doc = "Grow guide curves out of a mesh along its normals, spread evenly by area (and a density map), as a new curve with the "
               "mesh as its groom surface. Returns the curve.",
        .positionalDoc = "mesh",
        .flags = {{"count", "c", T::Int, 200, "How many curves.", {}, {}, 1.0, 1000000.0},
                  {"length", "l", T::Float, 10.0, "Length, cm.", {}, {}, 0.0},
                  {"lengthJitter", "", T::Float, 0.0, "Lengths vary by up to this fraction (0 to 1).", {}, {}, 0.0, 1.0},
                  {"points", "p", T::Int, 8, "Points per curve.", {}, {}, 2.0, 256.0},
                  {"minDistance", "", T::Float, 0.0, "Closest two roots may be, cm; 0 picks one from the area and count.", {}, {}, 0.0},
                  {"densitySet", "", T::String, nullptr, "Vertex color set used as the density map (bright: dense, black: none)."},
                  {"seed", "", T::Int, 1, "Random seed."},
                  {"name", "n", T::String, nullptr, "Name of the new curve."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node* mesh = nullptr;
            if (!a.positional.empty())
                mesh = &meshNode(g, a.positional[0].get<std::string>(), "groomGuides");
            else
                for (NodeId id : g.selection())
                    if (Node* n = g.node(id); n && n->type().isA("mesh"))
                    {
                        mesh = n;
                        break;
                    }
            if (!mesh)
                throw CommandError("groomGuides: select a mesh");
            g.evaluate();
            MeshPtr m = mesh->as<MeshPtr>(MeshNode::kOutMesh);
            if (!m || m->numFaces() == 0)
                throw CommandError("groomGuides: " + mesh->path() + " has no faces");
            groom::Surface surface(transformed(*m, mesh->as<glm::dmat4>(Transform::kWorldMatrix)), a.get<std::string>("densitySet"));
            groom::GuideOptions o;
            o.count = std::clamp(a.get<int>("count", 200), 1, 1000000);
            o.length = std::max(1e-4, a.get<double>("length", 10.0));
            o.lengthJitter = std::clamp(a.get<double>("lengthJitter", 0.0), 0.0, 1.0);
            o.points = std::clamp(a.get<int>("points", 8), 2, 256);
            o.minDistance = std::max(0.0, a.get<double>("minDistance", 0.0));
            o.seed = uint32_t(a.get<int64_t>("seed", 1));
            std::vector<std::string> warnings;
            Splines s = groom::guides(surface, o, &warnings);
            for (const std::string& w : warnings)
                log::warn("groomGuides: {}", w);
            if (s.empty())
                throw CommandError("groomGuides: no curves fit on " + mesh->path());
            Node* c = g.createNode(*types::find("curve"), a.get<std::string>("name", "Guides"), kNoNode, true);
            g.setValue({c->id(), CurveShape::kCreate, -1}, makeCurveData(std::move(s)));
            g.setValue({c->id(), c->type().attrIndex("groomSurface"), -1}, mesh->path());
            g.select({c->id()});
            return c->path();
        },
    });

    std::vector<FlagDef> flags = nodeFlags();
    for (FlagDef fd : std::vector<FlagDef>{nodeFlag({"guides", "", T::List, nullptr, "Guide curves (sets source = guides)."}, "curve"),
                                           nodeFlag({"mesh", "m", T::String, nullptr, "The scalp mesh; default the first curve's groomSurface."}, "mesh"),
                                           {"constructionHistory", "ch", T::Bool, true, "Keep the hairCards node so its settings stay live."},
                                           {"name", "n", T::String, nullptr, "Mesh name; made unique."}})
        flags.push_back(std::move(fd));
    ops::registerCommand({
        .name = "hairCards",
        .doc = "Build textured hair cards from curves with live history: one per curve, per clump, or per guide curve; width with a taper "
               "ramp, segments, facing the scalp or following the curve, twist, and UVs in random atlas cells. A card mesh per curve node. "
               "Returns [mesh, hairCards, ...].",
        .positionalDoc = "curves",
        .flags = flags,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<Node*> curves = curvesOrPointSelection(g, a, "hairCards");
            std::vector<Node*> guideNodes;
            for (const json& n : a.flags.value("guides", json::array()))
                guideNodes.push_back(&curveNode(g, n.get<std::string>(), "hairCards"));
            std::string meshName = a.get<std::string>("mesh");
            if (meshName.empty())
                meshName = groomSurfaceOf(*curves[0]);
            Node* scalp = meshName.empty() ? nullptr : g.find(meshName);
            if (scalp && !scalp->type().isA("mesh"))
                scalp = nullptr;
            if (!scalp && a.has("mesh"))
                throw CommandError("hairCards: " + meshName + " is not a mesh");
            json out = json::array();
            std::vector<NodeId> made;
            // One card node per curve, each live.
            for (Node* c : curves)
            {
                Node* op = g.createNode(*types::find("hairCards"), {}, kNoNode, true);
                if (!guideNodes.empty())
                    g.setValue({op->id(), op->type().attrIndex("source"), -1}, int64_t(2));
                applyFlags(g, *op, a, "hairCards");
                g.connect({c->id(), CurveShape::kWorldSpace, -1}, {op->id(), op->type().attrIndex("inCurve"), -1});
                if (!guideNodes.empty())
                    g.connect({guideNodes[0]->id(), CurveShape::kWorldSpace, -1}, {op->id(), op->type().attrIndex("guideCurve"), -1});
                if (scalp)
                {
                    g.connect({scalp->id(), MeshNode::kOutMesh, -1}, {op->id(), op->type().attrIndex("inMesh"), -1});
                    g.connect({scalp->id(), Transform::kWorldMatrix, -1}, {op->id(), op->type().attrIndex("meshMatrix"), -1});
                }
                Node* mesh = g.createNode(*types::find("mesh"), a.get<std::string>("name", "HairCards"), kNoNode, true);
                g.connect({op->id(), op->type().attrIndex("outMesh"), -1}, {mesh->id(), MeshNode::kInMesh, -1});
                json opName = op->path();
                if (!a.get<bool>("constructionHistory", true))
                {
                    g.evaluate();
                    PlugRef dst{mesh->id(), MeshNode::kInMesh, -1};
                    PlugValue v = g.value(dst);
                    g.disconnect(dst);
                    g.setValue(dst, v);
                    g.deleteNode(op->id());
                    opName = nullptr;
                }
                out.push_back(mesh->path());
                out.push_back(opName);
                made.push_back(mesh->id());
            }
            g.select(made);
            return out;
        },
    });
    registerGroomOptionLayout();
}
} // namespace lila::groom
