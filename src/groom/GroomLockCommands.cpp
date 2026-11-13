#include "groom/GroomNodes.h"

#include "core/Log.h"
#include "curve/CurveNodes.h"
#include "curve/NodeHelpers.h"
#include "groom/Attrs.h"
#include "groom/GroomEdit.h"
#include "groom/GroomExport.h"
#include "groom/GroomLook.h"
#include "groom/GroomGraph.h"
#include "groom/GroomLabBridge.h"
#include "groom/GuideData.h"
#include "groom/GroomPresets.h"
#include "groom/RodSettle.h"
#include "groom/Settle.h"
#include "groom/Shapes.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <glm/gtx/quaternion.hpp>

#include <algorithm>

namespace lila::groom
{
using namespace scene;
using namespace curve::helpers;
using curve::CurveShape;
using curve::makeCurveData;
using curve::splinesOf;
using json = nlohmann::json;
using T = ArgType;

static geo::Mesh meshMoved(const geo::Mesh& in, const glm::dmat4& m)
{
    geo::Mesh out = in;
    for (geo::Index p = 0; p < out.pointCapacity(); ++p)
        if (out.pointValid(p))
            out.setPosition(p, D3(m * glm::dvec4(out.position(p), 1.0)));
    return out;
}

/** The guides' scalp in the guides' object space, or null. */
std::unique_ptr<Surface> scalpOf(Graph& g, Node& guides)
{
    const std::string name = groomSurfaceOf(guides);
    Node* mesh = name.empty() ? nullptr : g.find(name);
    if (!mesh || !mesh->type().isA("mesh"))
        return nullptr;
    g.evaluate();
    MeshPtr m = mesh->as<MeshPtr>(MeshNode::kOutMesh);
    if (!m || m->numFaces() == 0)
        return nullptr;
    const glm::dmat4 toCurve = glm::inverse(guides.as<glm::dmat4>(Transform::kWorldMatrix)) * mesh->as<glm::dmat4>(Transform::kWorldMatrix);
    return std::make_unique<Surface>(meshMoved(*m, toCurve));
}

Splines guideSplines(Graph& g, Node& guides)
{
    g.evaluate();
    return splinesOf(g.value({guides.id(), CurveShape::kLocal, -1}));
}

void storeGuides(Graph& g, Node& guides, Splines s)
{
    Node* groom = groomOf(g, guides);
    if (!groom)
    {
        PlugRef create{guides.id(), CurveShape::kCreate, -1};
        if (g.source(create))
            g.disconnect(create);
        g.setValue(create, makeCurveData(std::move(s)));
        return;
    }
    // The guides curve shows the groom's hierarchy: the edit goes into it, compared with the guides as posed now.
    const glm::dmat4 toWorld = guides.as<glm::dmat4>(Transform::kWorldMatrix);
    if (toWorld != glm::dmat4(1.0))
        for (Spline& sp : s)
            for (D3& p : sp.points)
                p = D3(toWorld * glm::dvec4(p, 1.0));
    setGroomGuides(g, *groom, s);
}

void setGroomGuides(Graph& g, Node& groom, const Splines& world)
{
    g.evaluate();
    const std::string& text = groom.as<std::string>(idx(groom, "hierarchy"));
    groomlab::GuideHierarchy h = text.empty() ? groomlab::GuideHierarchy() : groomlab::GuideHierarchy::fromJson(text);
    std::shared_ptr<const groomlab::ScalpMesh> scalp;
    if (const MeshPtr& m = groom.as<MeshPtr>(idx(groom, "inMesh")); m && m->numFaces() > 0)
        scalp = groomLabBridge::scalpMesh(*m, groom.as<glm::dmat4>(idx(groom, "meshMatrix")));
    h.evaluate(scalp.get());
    if (applyLockSplines(h, world, lockOptionsOf(groom)))
    {
        h.evaluate(scalp.get());
        g.setValue({groom.id(), idx(groom, "hierarchy"), -1}, h.toJson());
    }
}

std::string groomGraphText(const Node& groom)
{
    const std::string& text = groom.as<std::string>(idx(groom, "graph"));
    return text.empty() ? groomGraph::defaultGraph() : text;
}

namespace
{
Node& lockGroomArg(Graph& g, const Args& a, const char* cmd)
{
    if (!a.positional.empty())
    {
        Node* n = g.find(a.positional[0].get<std::string>());
        if (!n)
            throw CommandError(std::string(cmd) + ": no node named '" + a.positional[0].get<std::string>() + "'");
        if (Node* gr = findGroom(g, *n))
            return *gr;
        throw CommandError(std::string(cmd) + ": " + n->path() + " is not part of a groom");
    }
    for (NodeId id : g.selection())
        if (Node* n = g.node(id))
            if (Node* gr = findGroom(g, *n))
                return *gr;
    throw CommandError(std::string(cmd) + ": select a groom (its guides, locks or shell)");
}

Node& guidesArg(Graph& g, Node& groom, const char* cmd)
{
    Node* c = guidesOf(g, groom);
    if (!c)
        throw CommandError(std::string(cmd) + ": " + groom.path() + " has no guide curve");
    return *c;
}

D3 vec(const json& j, const D3& def = D3(0.0))
{
    return j.is_array() && j.size() == 3 ? D3(j[0].get<double>(), j[1].get<double>(), j[2].get<double>()) : def;
}

json vecJson(const D3& v) { return json::array({v.x, v.y, v.z}); }

std::vector<D3> points(const json& j)
{
    std::vector<D3> out;
    if (j.is_array())
        for (const json& p : j)
            out.push_back(vec(p));
    return out;
}


/** A Lock settings flag for each groom attr that is a number, enum or string the user edits. */
std::vector<FlagDef> lockFlags()
{
    std::vector<FlagDef> out;
    for (const FlagDef& f : attrFlags("groom"))
        if (f.name == "width" || f.name == "thickness" || f.name == "profile" || f.name == "widthRamp" || f.name == "twist" || f.name == "sides")
            out.push_back(f);
    return out;
}

json stackJson(Node& groom)
{
    json out = json::array();
    for (const groomGraph::StyleEntry& e : groomGraph::styles(groomGraphText(groom)))
        out.push_back({{"node", e.node}, {"style", styleName(e.kind)}, {"enabled", e.enabled}});
    return out;
}

/** Turns each lock whose root lies within radius of the parting line so it leaves the root away from the line. */
int part(Splines& s, const std::vector<D3>& line, double radius, double strength, const Surface* scalp)
{
    int moved = 0;
    for (Spline& sp : s)
    {
        if (sp.size() < 2 || line.size() < 2)
            continue;
        const D3 root = sp.points[0];
        D3 best(0.0);
        double bestD = 1e300;
        for (size_t k = 0; k + 1 < line.size(); ++k)
        {
            const D3 ab = line[k + 1] - line[k];
            const double t = glm::dot(ab, ab) > 1e-12 ? std::clamp(glm::dot(root - line[k], ab) / glm::dot(ab, ab), 0.0, 1.0) : 0.0;
            const D3 q = line[k] + ab * t;
            if (double d = glm::distance(q, root); d < bestD)
            {
                bestD = d;
                best = q;
            }
        }
        if (bestD > radius)
            continue;
        D3 n(0, 1, 0);
        if (scalp)
            if (auto h = scalp->closest(root))
                n = h->normal;
        D3 away = root - best;
        away -= n * glm::dot(away, n);
        if (glm::length(away) < 1e-6)
            continue;
        away = glm::normalize(away);
        const size_t k = std::min<size_t>(sp.size() - 1, std::max<size_t>(1, sp.size() / 5));
        D3 dir = sp.points[k] - root;
        dir -= n * glm::dot(dir, n);
        if (glm::length(dir) < 1e-6)
            continue;
        dir = glm::normalize(dir);
        const double falloff = 1.0 - glm::smoothstep(0.0, radius, bestD) * 0.5;
        const double angle = std::atan2(glm::dot(glm::cross(dir, away), n), glm::dot(dir, away)) * std::clamp(strength, 0.0, 1.0) * falloff;
        const glm::dquat q = glm::angleAxis(angle, n);
        for (size_t i = 1; i < sp.size(); ++i)
            sp.points[i] = root + q * (sp.points[i] - root);
        ++moved;
    }
    return moved;
}
} // namespace

void registerLockCommands()
{
    registerIoCommands();
    registerLookCommands();
    presets::registerCommands();
    std::vector<std::string> styles, shapes, presets, profiles;
    for (int k = 0; k < int(Style::Count); ++k)
        styles.push_back(styleName(Style(k)));
    for (int k = 0; k < int(LockShape::Count); ++k)
        shapes.push_back(lockShapeName(LockShape(k)));
    for (int k = 0; k < int(Preset::Count); ++k)
        presets.push_back(presetName(Preset(k)));
    for (int k = 0; k < int(LockProfile::Count); ++k)
        profiles.push_back(profileName(LockProfile(k)));

    ops::registerCommand({
        .name = "createGroom",
        .doc = "A new groom growing from a scalp mesh: a guide curve (empty, or the curve given, or count guides grown from the "
               "scalp), the groom node and its locks mesh. Draw locks with groomDraw or the Draw Lock tool, style them with "
               "groomAddStyle. Returns {groom, guides, locks} and, with strands, the strands curve.",
        .positionalDoc = "scalp",
        .flags = withFlags({nodeFlag({"guides", "", T::String, nullptr, "An existing curve to use as the guides."}, "curve"),
                            {"count", "c", T::Int, 0, "Guides to grow straight out of the scalp; 0 starts empty.", {}, {}, 0.0, 100000.0},
                            {"length", "l", T::Float, 10.0, "Length of grown guides, cm.", {}, {}, 0.0},
                            {"symmetry", "sym", T::String, "x", "Mirror the guides across the world plane: off, x, y or z.", {"off", "x", "y", "z"}},
                            {"strands", "", T::Bool, false, "Also a curve showing the groom's strands, interpolated from its locks."},
                            {"name", "n", T::String, "Hair", "Base name of the new nodes."}},
                           lockFlags()),
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node* scalp = nullptr;
            if (!a.positional.empty())
                scalp = &meshNamed(g, a.positional[0].get<std::string>(), "createGroom");
            else
                for (NodeId id : g.selection())
                    if (Node* n = g.node(id); n && n->type().isA("mesh"))
                    {
                        scalp = n;
                        break;
                    }
            if (!scalp)
                throw CommandError("createGroom: select the scalp mesh");
            const std::string base = a.get<std::string>("name", "Hair");
            Node* guides = nullptr;
            if (a.has("guides"))
                guides = &curveNamed(g, a.get<std::string>("guides"), "createGroom");
            else
            {
                Splines s;
                if (const int count = a.get<int>("count", 0); count > 0)
                {
                    g.evaluate();
                    MeshPtr m = scalp->as<MeshPtr>(MeshNode::kOutMesh);
                    if (!m || m->numFaces() == 0)
                        throw CommandError("createGroom: " + scalp->path() + " has no faces");
                    Surface surface(meshMoved(*m, scalp->as<glm::dmat4>(Transform::kWorldMatrix)));
                    GuideOptions go;
                    go.count = count;
                    go.length = std::max(1e-3, a.get<double>("length", 10.0));
                    s = groom::guides(surface, go);
                    for (Spline& sp : s)
                        attr::set(sp, attr::kGuide, 1.0);
                    attr::ensureIds(s);
                }
                guides = g.createNode(*types::find("curve"), base + "Guides", kNoNode, true);
                g.setValue({guides->id(), CurveShape::kCreate, -1}, makeCurveData(std::move(s)));
            }
            g.setValue({guides->id(), guides->type().attrIndex("groomSurface"), -1}, scalp->path());
            const std::string sym = a.get<std::string>("symmetry", "x");
            const int64_t axis = sym == "x" ? 1 : sym == "y" ? 2 : sym == "z" ? 3 : 0;
            Node* groom = g.createNode(*types::find("groom"), base + "Groom", kNoNode, true);
            applyFlags(g, *groom, a, "createGroom");
            g.setValue({groom->id(), idx(*groom, "symmetry"), -1}, axis);
            g.connect({scalp->id(), MeshNode::kOutMesh, -1}, {groom->id(), idx(*groom, "inMesh"), -1});
            g.connect({scalp->id(), Transform::kWorldMatrix, -1}, {groom->id(), idx(*groom, "meshMatrix"), -1});
            // The curve's splines become the groom's guides; from then on it shows them.
            g.evaluate();
            Splines world = splinesOf(g.value({guides->id(), CurveShape::kWorldSpace, -1}));
            setGroomGuides(g, *groom, world);
            if (const int at = guides->type().attrIndex("symmetry"); at >= 0)
                g.connect({groom->id(), idx(*groom, "symmetry"), -1}, {guides->id(), at, -1});
            // Shown in world space, as the groom keeps them.
            for (const char* t : {"translate", "rotate"})
                if (const int at = guides->type().attrIndex(t); at >= 0)
                    g.setValue({guides->id(), at, -1}, glm::dvec3(0.0));
            if (const int at = guides->type().attrIndex("scale"); at >= 0)
                g.setValue({guides->id(), at, -1}, glm::dvec3(1.0));
            g.connect({groom->id(), idx(*groom, "outGuides"), -1}, {guides->id(), CurveShape::kCreate, -1});
            Node* locks = g.createNode(*types::find("mesh"), base + "Locks", kNoNode, true);
            g.connect({groom->id(), idx(*groom, "outMesh"), -1}, {locks->id(), MeshNode::kInMesh, -1});
            json out = {{"groom", groom->path()}, {"guides", guides->path()}, {"locks", locks->path()}};
            if (a.get<bool>("strands"))
            {
                Node* strands = g.createNode(*types::find("curve"), base + "Strands", kNoNode, true);
                g.connect({groom->id(), idx(*groom, "outStrands"), -1}, {strands->id(), CurveShape::kCreate, -1});
                out["strands"] = strands->path();
            }
            g.select({guides->id()});
            return out;
        },
    });

    std::vector<FlagDef> styleFlags = {{"style", "s", T::String, nullptr, "The modifier.", styles},
                                       {"position", "p", T::Int, -1, "Where in the stack, 0 first; -1 appends."}};
    for (const char* f : {"amount", "frequency", "radius", "count", "spread", "length", "widthScale", "randomness", "seed", "ramp"})
        styleFlags.push_back({f, "", std::string_view(f) == "count" || std::string_view(f) == "seed" ? T::Int : std::string_view(f) == "ramp" ? T::String : T::Float, nullptr,
                              "Sets the modifier's " + std::string(f) + " when it has one."});
    ops::registerCommand({
        .name = "groomAddStyle",
        .doc = "Adds a lock style to a groom's graph, between its guides and its locks: split, clump, curl, wave, frizz, noise, "
               "length, cut or flyaways. The strands follow the styled locks. Returns the new graph node's name.",
        .positionalDoc = "groom",
        .flags = styleFlags,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& groom = lockGroomArg(g, a, "groomAddStyle");
            guidesArg(g, groom, "groomAddStyle");
            Style k;
            if (!parseStyle(a.get<std::string>("style"), &k))
                throw CommandError("groomAddStyle: unknown style '" + a.get<std::string>("style") + "'");
            json params = json::object();
            for (const char* f : {"amount", "frequency", "radius", "count", "spread", "length", "widthScale", "randomness", "seed", "ramp"})
                if (a.has(f))
                    params[f] = a.flags[f];
            std::string graph = groomGraphText(groom);
            std::string made;
            try
            {
                made = groomGraph::addStyle(graph, k, a.get<int>("position", -1), params.dump());
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("groomAddStyle: ") + e.what());
            }
            g.setValue({groom.id(), idx(groom, "graph"), -1}, graph);
            return made;
        },
    });

    ops::registerCommand({
        .name = "groomStack",
        .doc = "Lists a groom's lock styles in order, or changes them: move one to a position, toggle it on or off, remove it, or "
               "set its params. Returns the styles as [{node, style, enabled}, ...].",
        .positionalDoc = "groom",
        .flags = {{"edit", "e", T::String, "list", "What to do.", {"list", "move", "toggle", "remove", "set"}},
                  {"node", "", T::String, nullptr, "The style to move, toggle, remove or set."},
                  {"to", "", T::Int, 0, "move: its new position, 0 first."},
                  {"params", "", T::String, nullptr, "set: params as a JSON object, e.g. {\"amount\": 0.5}."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& groom = lockGroomArg(g, a, "groomStack");
            const std::string edit = a.get<std::string>("edit", "list");
            if (edit != "list")
            {
                const std::string node = a.get<std::string>("node");
                std::string graph = groomGraphText(groom);
                const auto styles = groomGraph::styles(graph);
                const auto it = std::ranges::find_if(styles, [&](const groomGraph::StyleEntry& e) { return e.node == node; });
                if (it == styles.end())
                    throw CommandError("groomStack: " + node + " is not one of " + groom.path() + "'s lock styles");
                try
                {
                    if (edit == "toggle")
                        groomGraph::setEnabled(graph, node, !it->enabled);
                    else if (edit == "remove")
                        groomGraph::removeStyle(graph, node);
                    else if (edit == "set")
                        groomGraph::setParams(graph, node, a.get<std::string>("params", "{}"));
                    else
                        groomGraph::moveStyle(graph, node, a.get<int>("to", 0));
                }
                catch (const std::exception& e)
                {
                    throw CommandError(std::string("groomStack: ") + e.what());
                }
                g.setValue({groom.id(), idx(groom, "graph"), -1}, graph);
            }
            return stackJson(groom);
        },
    });

    ops::registerCommand({
        .name = "groomDraw",
        .doc = "Adds locks along a stroke to a groom's guides: points from the root, world space; the root snaps to the scalp. "
               "shape draws a lock, a braid (three strands), a rope (two) or a ringlet. pressure (0 to 1 per point) sets the "
               "width along it. Returns the new guides' ids.",
        .positionalDoc = "groom",
        .flags = {{"points", "pt", T::List, nullptr, "[[x, y, z], ...] from root to tip, world space."},
                  {"pressure", "pr", T::List, nullptr, "Pen pressure per point, 0 to 1."},
                  {"shape", "s", T::String, "lock", "What the stroke draws.", shapes},
                  {"size", "", T::Float, 1.0, "Braid and rope half width, ringlet radius, cm.", {}, {}, 0.0},
                  {"period", "", T::Float, 4.0, "Length of one turn of a braid, rope or ringlet, cm.", {}, {}, 0.01},
                  {"width", "w", T::Float, 1.0, "Width of the new locks relative to the groom's.", {}, {}, 0.0},
                  {"thickness", "th", T::Float, 1.0, "Thickness of the new locks relative to the groom's.", {}, {}, 0.0},
                  {"profile", "", T::String, nullptr, "Cross-section of the new locks; default the groom's.", profiles},
                  {"snapRoot", "", T::Bool, true, "Move the root onto the scalp."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& groom = lockGroomArg(g, a, "groomDraw");
            Node& guides = guidesArg(g, groom, "groomDraw");
            const glm::dmat4 toLocal = glm::inverse(guides.as<glm::dmat4>(Transform::kWorldMatrix));
            std::vector<D3> stroke;
            for (const D3& p : points(a.flags.value("points", json::array())))
                stroke.push_back(D3(toLocal * glm::dvec4(p, 1.0)));
            if (stroke.size() < 2)
                throw CommandError("groomDraw: give at least two points");
            std::vector<double> pressure;
            for (const json& p : a.flags.value("pressure", json::array()))
                pressure.push_back(p.get<double>());
            std::unique_ptr<Surface> scalp = scalpOf(g, guides);
            D3 up(0, 1, 0);
            if (scalp)
                if (auto h = scalp->closest(stroke.front()))
                {
                    up = h->normal;
                    if (a.get<bool>("snapRoot", true))
                        stroke.front() = h->point;
                }
            ShapeOptions so;
            if (!parseLockShape(a.get<std::string>("shape", "lock"), &so.shape))
                throw CommandError("groomDraw: unknown shape '" + a.get<std::string>("shape") + "'");
            so.size = a.get<double>("size", 1.0);
            so.period = a.get<double>("period", 4.0);
            Splines made = shapeAlong(stroke, pressure, up, so);
            if (made.empty())
                throw CommandError("groomDraw: the stroke is too short");
            Splines s = guideSplines(g, guides);
            int64_t next = attr::nextId(s);
            const double clump = double(next);
            LockProfile prof = LockProfile::Ribbon;
            const bool hasProfile = a.has("profile") && parseProfile(a.get<std::string>("profile"), &prof);
            json ids = json::array();
            for (Spline& sp : made)
            {
                attr::set(sp, attr::kId, double(next));
                attr::set(sp, attr::kWidth, attr::get(sp, attr::kWidth, 1.0) * a.get<double>("width", 1.0));
                if (const double th = a.get<double>("thickness", 1.0); th != 1.0)
                    attr::set(sp, attr::kThickness, attr::get(sp, attr::kThickness, 1.0) * th);
                if (made.size() > 1)
                    attr::set(sp, attr::kClump, clump);
                if (hasProfile)
                    attr::set(sp, attr::kProfile, double(int(prof)));
                ids.push_back(next++);
                s.push_back(std::move(sp));
            }
            storeGuides(g, guides, std::move(s));
            return ids;
        },
    });

    ops::registerCommand({
        .name = "groomPart",
        .doc = "A parting: turns the locks whose roots lie near the line so they leave the scalp away from it. points is the line, "
               "world space. Returns how many locks turned.",
        .positionalDoc = "groom",
        .flags = {{"points", "pt", T::List, nullptr, "[[x, y, z], ...] along the parting, world space."},
                  {"radius", "r", T::Float, 3.0, "How far from the line roots turn, cm.", {}, {}, 0.0},
                  {"strength", "s", T::Float, 1.0, "0 to 1: how far they turn.", {}, {}, 0.0, 1.0}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& groom = lockGroomArg(g, a, "groomPart");
            Node& guides = guidesArg(g, groom, "groomPart");
            const glm::dmat4 w = guides.as<glm::dmat4>(Transform::kWorldMatrix), toLocal = glm::inverse(w);
            std::vector<D3> line;
            for (const D3& p : points(a.flags.value("points", json::array())))
                line.push_back(D3(toLocal * glm::dvec4(p, 1.0)));
            if (line.size() < 2)
                throw CommandError("groomPart: give at least two points");
            const double scale = std::cbrt(std::abs(glm::determinant(glm::dmat3(toLocal))));
            std::unique_ptr<Surface> scalp = scalpOf(g, guides);
            Splines s = guideSplines(g, guides);
            const int n = part(s, line, a.get<double>("radius", 3.0) * scale, a.get<double>("strength", 1.0), scalp.get());
            if (n)
                storeGuides(g, guides, std::move(s));
            return n;
        },
    });

    ops::registerCommand({
        .name = "groomPreset",
        .doc = "Adds a hairstyle preset's guides to a groom: a ponytail or bun gathered at a point on the scalp, or bangs along a "
               "line between two. Returns the new guides' ids.",
        .positionalDoc = "groom",
        .flags = {{"preset", "p", T::String, "ponytail", "The preset.", presets},
                  {"at", "", T::Vec3, nullptr, "Ponytail and bun: where it gathers; bangs: one end of the fringe. World space."},
                  {"to", "", T::Vec3, nullptr, "Bangs: the other end of the fringe."},
                  {"count", "c", T::Int, 16, "Locks.", {}, {}, 1.0, 4096.0},
                  {"length", "l", T::Float, 20.0, "Length, cm.", {}, {}, 0.0},
                  {"radius", "r", T::Float, 6.0, "Ponytail: how far roots spread from the gather point; bun: its size, cm.", {}, {}, 0.0},
                  {"down", "", T::Vec3, json::array({0.0, -1.0, 0.0}), "Which way hair falls, world space."},
                  {"seed", "", T::Int, 1, "Random seed."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& groom = lockGroomArg(g, a, "groomPreset");
            Node& guides = guidesArg(g, groom, "groomPreset");
            std::unique_ptr<Surface> scalp = scalpOf(g, guides);
            if (!scalp)
                throw CommandError("groomPreset: " + guides.path() + " has no groom surface");
            const glm::dmat4 w = guides.as<glm::dmat4>(Transform::kWorldMatrix), toLocal = glm::inverse(w);
            const double scale = std::cbrt(std::abs(glm::determinant(glm::dmat3(toLocal))));
            PresetOptions po;
            if (!parsePreset(a.get<std::string>("preset", "ponytail"), &po.preset))
                throw CommandError("groomPreset: unknown preset '" + a.get<std::string>("preset") + "'");
            if (!a.has("at"))
                throw CommandError("groomPreset: give at");
            po.at = D3(toLocal * glm::dvec4(vec(a.flags["at"]), 1.0));
            po.to = D3(toLocal * glm::dvec4(vec(a.flags.value("to", json()), vec(a.flags["at"])), 1.0));
            po.down = D3(toLocal * glm::dvec4(vec(a.flags.value("down", json()), D3(0, -1, 0)), 0.0));
            po.count = a.get<int>("count", 16);
            po.length = a.get<double>("length", 20.0) * scale;
            po.radius = a.get<double>("radius", 6.0) * scale;
            po.seed = uint32_t(a.get<int64_t>("seed", 1));
            Splines made = preset(*scalp, po);
            Splines s = guideSplines(g, guides);
            int64_t next = attr::nextId(s);
            json ids = json::array();
            for (Spline& sp : made)
            {
                attr::set(sp, attr::kId, double(next));
                ids.push_back(next++);
                s.push_back(std::move(sp));
            }
            storeGuides(g, guides, std::move(s));
            return ids;
        },
    });

    ops::registerCommand({
        .name = "groomSettle",
        .doc = "Settles a groom's guides under gravity: roots stay on the scalp, locks fall, rest on it and on each other sized by "
               "their thickness, keeping some of their styled shape. Returns {curves, contacts, motion, penetration}.",
        .positionalDoc = "groom",
        .flags = {{"steps", "", T::Int, 90, "Solver steps (at 60 a second).", {}, {}, 1.0, 10000.0},
                  {"bend", "b", T::Float, 0.5, "Bend stiffness, 0 (rope) to 1 (wire).", {}, {}, 0.0, 1.0},
                  {"holdShape", "hs", T::Float, 0.15, "Pull back toward the styled shape, 0 to 1.", {}, {}, 0.0, 1.0},
                  {"rootStiffness", "", T::Float, 0.9, "How firmly roots keep their direction out of the scalp, 0 to 1.", {}, {}, 0.0, 1.0},
                  {"damping", "", T::Float, 0.08, "Damping per step, 0 to 1.", {}, {}, 0.0, 1.0},
                  {"slip", "", T::Float, 0.25, "How freely locks slide on what they rest on, 0 to 1.", {}, {}, 0.0, 1.0},
                  {"gravity", "", T::Vec3, json::array({0.0, -981.0, 0.0}), "Gravity, world space, cm/s^2."},
                  {"lockCollisions", "", T::Bool, true, "Locks collide with each other."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& groom = lockGroomArg(g, a, "groomSettle");
            Node& guides = guidesArg(g, groom, "groomSettle");
            const glm::dmat4 w = guides.as<glm::dmat4>(Transform::kWorldMatrix), toLocal = glm::inverse(w);
            const double scale = std::cbrt(std::abs(glm::determinant(glm::dmat3(toLocal))));
            Splines s = guideSplines(g, guides);
            if (s.empty())
                throw CommandError("groomSettle: " + guides.path() + " has no guides");
            std::vector<std::vector<double>> radii = collisionRadii(s, lockOptionsOf(groom));
            for (auto& r : radii)
                for (double& v : r)
                    v *= scale;
            std::unique_ptr<Surface> scalp = scalpOf(g, guides);
            SettleOptions so;
            so.steps = a.get<int>("steps", 90);
            so.bend = a.get<double>("bend", 0.5);
            so.holdShape = a.get<double>("holdShape", 0.15);
            so.rootStiffness = a.get<double>("rootStiffness", 0.9);
            so.damping = a.get<double>("damping", 0.08);
            so.slip = a.get<double>("slip", 0.25);
            so.lockCollisions = a.get<bool>("lockCollisions", true);
            so.gravity = D3(toLocal * glm::dvec4(vec(a.flags.value("gravity", json()), D3(0, -981, 0)), 0.0));
            SettleStats st;
            settleOnRods(s, radii, scalp.get(), so, &st);
            storeGuides(g, guides, std::move(s));
            return {{"curves", st.curves}, {"contacts", st.contacts}, {"motion", st.lastMotion / scale}, {"penetration", st.maxPenetration / scale}};
        },
    });

    ops::registerCommand({
        .name = "groomEditLock",
        .doc = "Sets one guide's lock width (relative to the groom's) and twist from root to tip, as the viewport lock handles do. "
               "index is the guide's place among the curve's own splines.",
        .positionalDoc = "guides",
        .flags = {{"index", "i", T::Int, 0, "Which guide.", {}, {}, 0.0},
                  {"width", "w", T::Float, nullptr, "Width relative to the groom's.", {}, {}, 0.001},
                  {"twist", "t", T::Float, nullptr, "Twist from root to tip, degrees: replaces the guide's tilt with an even ramp."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& groom = lockGroomArg(g, a, "groomEditLock");
            Node& guides = guidesArg(g, groom, "groomEditLock");
            Splines s = guideSplines(g, guides);
            const int i = a.get<int>("index", 0);
            if (i < 0 || i >= int(s.size()))
                throw CommandError("groomEditLock: " + guides.path() + " has no guide " + std::to_string(i));
            Spline& sp = s[size_t(i)];
            if (a.has("width"))
                attr::set(sp, attr::kWidth, std::max(0.001, a.get<double>("width")));
            if (a.has("twist"))
            {
                const double turn = glm::radians(a.get<double>("twist"));
                const double len = std::max(1e-12, attr::length(sp));
                sp.tilt.assign(sp.size(), 0.0);
                double walked = 0.0;
                for (size_t k = 1; k < sp.size(); ++k)
                {
                    walked += glm::distance(sp.points[k - 1], sp.points[k]);
                    sp.tilt[k] = turn * walked / len;
                }
            }
            storeGuides(g, guides, std::move(s));
            return true;
        },
    });

    std::vector<FlagDef> shellFlags = attrFlags("groomShell");
    shellFlags.push_back({"name", "n", T::String, nullptr, "Mesh name; default <groom>Shell."});
    shellFlags.push_back({"quads", "q", T::Int, 0, "Also quad remesh the shell (quadRemesh) to about this many faces; 0 leaves it as it is.", {}, {}, 0.0, 1000000.0});
    ops::registerCommand({
        .name = "groomShell",
        .doc = "One watertight mesh over a groom's locks, smooth-unioned so they join into a single surface ready for retopology "
               "(quadRemesh). Live: it follows the groom. With quads, also the quad remeshed shell. Returns [mesh, groomShell] and, "
               "with quads, quadRemesh's reply last.",
        .positionalDoc = "groom",
        .flags = shellFlags,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& groom = lockGroomArg(g, a, "groomShell");
            Node* op = g.createNode(*types::find("groomShell"), {}, kNoNode, true);
            applyFlags(g, *op, a, "groomShell");
            g.connect({groom.id(), idx(groom, "outCurve"), -1}, {op->id(), idx(*op, "inCurve"), -1});
            std::string name = a.get<std::string>("name");
            if (name.empty())
            {
                name = groom.name();
                if (name.ends_with("Groom"))
                    name.resize(name.size() - 5);
                name += "Shell";
            }
            Node* mesh = g.createNode(*types::find("mesh"), name, kNoNode, true);
            g.connect({op->id(), idx(*op, "outMesh"), -1}, {mesh->id(), MeshNode::kInMesh, -1});
            g.select({mesh->id()});
            json out = json::array({mesh->path(), op->path()});
            if (const int quads = a.get<int>("quads", 0); quads > 0)
                out.push_back(ops::execute("quadRemesh", {{"args", {mesh->path()}}, {"targetFaces", quads}}));
            return out;
        },
    });
}
} // namespace lila::groom
