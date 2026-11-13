// Props and weapons: prop and grip point nodes, fitting points to a mesh, holding a prop in one or both hands, keyed
// handoffs between hands, holsters and the world, aiming, the solve that shares the load between the arms (a grapple
// with frame contacts), and setting a prop up for Unreal.

#include "prop/Props.h"

#include "Puppeteer.h"
#include "hand/Hands.h"
#include "rig/Rig.h"

#include <lila/Anim.h>
#include <lila/AnimCurve.h>
#include <lila/Grapple.h>
#include <lila/Math.h>
#include <lila/Mesh.h>
#include <lila/OptionBox.h>
#include <lila/Pins.h>
#include <lila/Relocate.h>
#include <lila/Simulation.h>
#include <lila/Spaces.h>
#include <lila/Interchange.h>

#include "RigidScene.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/norm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>

using namespace lila;
using namespace lila::scene;

namespace puppeteer::prop
{
namespace gp = lila::scene::grapple;
namespace sp = lila::scene::spaces;

namespace
{
const std::vector<std::string>& presetIds()
{
    static const std::vector<std::string> ids = [] {
        std::vector<std::string> v;
        for (const Preset& p : presets())
            v.push_back(p.id);
        return v;
    }();
    return ids;
}
template <class E, class F> std::vector<std::string> idsOf(int count, F id)
{
    std::vector<std::string> v;
    for (int i = 0; i < count; ++i)
        v.push_back(id(E(i)));
    return v;
}
const std::vector<std::string>& shapeIds()
{
    static const std::vector<std::string> ids = idsOf<Shape>(int(Shape::Count), [](Shape x) { return std::string(shapeId(x)); });
    return ids;
}
const std::vector<std::string>& aimIds()
{
    static const std::vector<std::string> ids = idsOf<AimStyle>(4, [](AimStyle x) { return std::string(aimStyleId(x)); });
    return ids;
}
const std::vector<std::string>& holsterIds()
{
    static const std::vector<std::string> ids =
        idsOf<HolsterSpot>(int(HolsterSpot::Count), [](HolsterSpot x) { return std::string(holsterSpot(x).id); });
    return ids;
}
const std::vector<std::string>& roleIds()
{
    static const std::vector<std::string> ids = [] {
        std::vector<std::string> v;
        for (int i = 0; i < int(Role::Count); ++i)
            v.push_back(roleId(Role(i)));
        return v;
    }();
    return ids;
}

int64_t intOf(Graph& g, NodeId id, const char* attr) { return std::get<int64_t>(g.value(plugOf(*g.node(id), attr))); }
bool boolOf(Graph& g, NodeId id, const char* attr) { return std::get<bool>(g.value(plugOf(*g.node(id), attr))); }
double numOf(Graph& g, NodeId id, const char* attr) { return std::get<double>(g.value(plugOf(*g.node(id), attr))); }
std::string strOf(Graph& g, NodeId id, const char* attr) { return std::get<std::string>(g.value(plugOf(*g.node(id), attr))); }
void setPlug(Graph& g, NodeId id, const char* attr, PlugValue v) { g.setValue(plugOf(*g.node(id), attr), std::move(v)); }

glm::dvec3 up() { return lila::math::axisBasis().up; }

double fps() { return std::max(anim::playback().fps, 1e-6); }

/** Sets the scene's time and evaluates. */
void at(Graph& g, double t)
{
    g.setTime(t, false);
    g.evaluate();
}

/** Puts the scene's time back when it leaves scope. */
struct KeepTime
{
    Graph& g;
    double was;
    explicit KeepTime(Graph& graph) : g(graph), was(graph.time()) {}
    ~KeepTime()
    {
        g.setTime(was, false);
        g.evaluate();
    }
};
} // namespace

// Reading props ------------------------------------------------------------------------------------------------------

NodeId Prop::point(Graph& g, Role r) const
{
    for (NodeId p : points)
        if (roleOf(*g.node(p)) == r)
            return p;
    return kNoNode;
}

Role roleOf(const Node& point) { return Role(std::clamp<int64_t>(point.as<int64_t>(attrOf(point, "role")), 0, int64_t(Role::Count) - 1)); }

NodeId propFor(Graph& g, NodeId node)
{
    for (NodeId cur = node; cur; cur = g.parent(cur))
    {
        Node* n = g.node(cur);
        if (!n)
            break;
        if (n->type().isA(kPropType))
            return cur;
        for (NodeId c : g.children(cur))
            if (Node* cn = g.node(c); cn && cn->type().isA(kPropType))
                return c;
    }
    return kNoNode;
}

std::optional<Prop> load(Graph& g, NodeId propNode)
{
    Node* n = g.node(propNode);
    if (!n || !n->type().isA(kPropType) || !g.parent(propNode))
        return std::nullopt;
    Prop p;
    p.node = propNode;
    p.root = g.parent(propNode);
    p.rightHanded = intOf(g, propNode, "hand") == 0;
    Settings& s = p.settings;
    s.twoHanded = boolOf(g, propNode, "twoHanded");
    s.mainPose = strOf(g, propNode, "mainPose");
    s.offPose = strOf(g, propNode, "offPose");
    s.trigger = boolOf(g, propNode, "trigger");
    s.aim = AimStyle(std::clamp<int64_t>(intOf(g, propNode, "aimStyle"), 0, 3));
    s.drop = numOf(g, propNode, "aimDrop");
    s.reach = numOf(g, propNode, "aimReach");
    s.holster = HolsterSpot(std::clamp<int64_t>(intOf(g, propNode, "holsterSpot"), 0, int64_t(HolsterSpot::Count) - 1));
    s.shape = Shape(std::clamp<int64_t>(intOf(g, propNode, "fitShape"), 0, int64_t(Shape::Count) - 1));
    for (NodeId c : g.children(propNode))
        if (Node* cn = g.node(c); cn && cn->type().isA(kPointType))
            p.points.push_back(c);
    return p;
}

void writeSettings(Graph& g, NodeId propNode, const Settings& s)
{
    setPlug(g, propNode, "twoHanded", s.twoHanded);
    setPlug(g, propNode, "mainPose", s.mainPose);
    setPlug(g, propNode, "offPose", s.offPose);
    setPlug(g, propNode, "trigger", s.trigger);
    setPlug(g, propNode, "aimStyle", int64_t(s.aim));
    setPlug(g, propNode, "aimDrop", s.drop);
    setPlug(g, propNode, "aimReach", s.reach);
    setPlug(g, propNode, "holsterSpot", int64_t(s.holster));
    setPlug(g, propNode, "fitShape", int64_t(s.shape));
}

std::vector<NodeId> props(Graph& g)
{
    std::vector<NodeId> out;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().isA(kPropType))
            out.push_back(id);
    return out;
}

glm::dmat4 pointLocal(Graph& g, const Prop& p, NodeId point) { return glm::inverse(rigidOf(worldOf(g, p.root))) * rigidOf(worldOf(g, point)); }

std::vector<NodeId> propMeshes(Graph& g, const Prop& p)
{
    std::vector<NodeId> out;
    std::vector<NodeId> all{p.root};
    for (NodeId d : g.descendants(p.root))
        all.push_back(d);
    for (NodeId id : all)
        if (Node* n = g.node(id); n && n->type().attrIndex("outMesh") >= 0 && !n->type().isA(kPointType))
            out.push_back(id);
    return out;
}

namespace
{
std::string spaceName(Graph& g, NodeId node, int index)
{
    for (const sp::Space& s : sp::list(g, node))
        if (s.index == index)
            return s.kind == sp::Kind::World ? "world" : s.kind == sp::Kind::Parent ? "parent" : s.target ? g.path(s.target) : s.name;
    return "parent";
}

Hold holdFor(Graph& g, NodeId node, int index, double time)
{
    Hold h;
    h.time = time;
    h.space = spaceName(g, node, index);
    h.kind = h.space == "world" ? "world" : h.space == "parent" ? "parent" : "node";
    for (const sp::Space& s : sp::list(g, node))
        if (s.index == index && s.kind == sp::Kind::Node)
        {
            h.target = s.target;
            if (Node* t = g.node(s.target))
            {
                if (t->type().isA(kPointType))
                    h.kind = "holster";
                else if (const NodeId hn = hand::handFor(g, s.target); hn && hand::load(g, hn) && hand::load(g, hn)->joint == s.target)
                {
                    h.kind = "hand";
                    h.side = hand::load(g, hn)->side == hand::Side::Left ? "left" : "right";
                }
            }
        }
    return h;
}
} // namespace

std::vector<Hold> holdsOf(Graph& g, const Prop& p)
{
    std::vector<Hold> out;
    const NodeId curve = anim::curveFor(g, {p.root, Transform::kSpace, -1});
    if (!curve)
    {
        out.push_back(holdFor(g, p.root, sp::active(g, p.root), anim::playback().minTime));
        return out;
    }
    for (double t : anim::curveKeyTimes(*g.node(curve)))
    {
        const int index = int(std::llround(anim::curveValueAt(*g.node(curve), 0, t)));
        if (!out.empty() && spaceName(g, p.root, index) == out.back().space)
            continue;
        out.push_back(holdFor(g, p.root, index, t));
    }
    return out;
}

json eventsOf(Graph& g, const Prop& p)
{
    const std::string text = strOf(g, p.node, "events");
    json j = json::parse(text.empty() ? std::string("[]") : text, nullptr, false);
    return j.is_array() ? j : json::array();
}

void recordEvent(Graph& g, const Prop& p, const json& ev)
{
    const double t = ev.value("time", 0.0), half = 0.5 / fps();
    json out = json::array();
    for (const json& e : eventsOf(g, p))
        if (std::abs(e.value("time", 0.0) - t) > half)
            out.push_back(e);
    out.push_back(ev);
    std::stable_sort(out.begin(), out.end(), [](const json& x, const json& y) { return x.value("time", 0.0) < y.value("time", 0.0); });
    setPlug(g, p.node, "events", out.dump());
}

NodeId holderNow(Graph& g, const Prop& p)
{
    const int index = sp::active(g, p.root);
    for (const sp::Space& s : sp::list(g, p.root))
        if (s.index == index && s.kind == sp::Kind::Node && s.target && rig::isJoint(*g.node(s.target)))
            return s.target;
    return kNoNode;
}

NodeId propGrapple(Graph& g, NodeId character, bool make)
{
    NodeId id = kNoNode;
    for (NodeId n : g.nodes())
        if (Node* nn = g.node(n); nn && nn->type().isA(gp::kType) && attrOf(*nn, kPropGrappleAttr) >= 0)
            id = n;
    if (!id && !make)
        return kNoNode;
    if (!id)
    {
        Node* gn = g.createNode(*types::find(gp::kType), "props_grapple");
        id = gn->id();
        g.addAttr(id, {.name = kPropGrappleAttr,
                       .type = PlugType::Bool,
                       .defaultValue = true,
                       .flags = AttrInput | AttrStorable | AttrHidden,
                       .doc = "Solves the arms of every character holding props."});
        // Bodies aren't kept apart and nothing is posed but the arms reaching for their grips.
        setPlug(g, id, "collide", false);
        setPlug(g, id, "autoPose", false);
        setPlug(g, id, "iterations", int64_t(8));
        g.evaluate();
    }
    if (!character)
        return id;
    const NodeId root = characterRoot(g, character);
    std::vector<gp::FigureSetup> figures = gp::figuresOf(g, id);
    for (const gp::FigureSetup& f : figures)
        if (f.root == root)
            return id;
    if (!make)
        return id;
    figures.push_back(gp::fitFigure(g, root));
    gp::setFigures(g, id, figures);
    return id;
}

json propJson(Graph& g, const Prop& p)
{
    const Settings& st = p.settings;
    const Preset* pr = matchingPreset(st);
    json points = json::array();
    for (NodeId pt : p.points)
    {
        const Node& n = *g.node(pt);
        points.push_back({{"point", g.path(pt)},
                          {"name", n.name()},
                          {"role", roleId(roleOf(n))},
                          {"radius", numOf(g, pt, "radius")},
                          {"position", vecJson(glm::dvec3(worldOf(g, pt)[3]))}});
    }
    json holds = json::array();
    for (const Hold& h : holdsOf(g, p))
    {
        json j = {{"time", h.time}, {"frame", h.time * fps()}, {"space", h.space}, {"kind", h.kind}};
        if (!h.side.empty())
            j["side"] = h.side;
        holds.push_back(j);
    }
    const NodeId holder = holderNow(g, p);
    return {{"prop", g.path(p.node)},
            {"root", g.path(p.root)},
            {"preset", pr ? json(pr->id) : json(nullptr)},
            {"hand", p.rightHanded ? "right" : "left"},
            {"twoHanded", st.twoHanded},
            {"mainPose", st.mainPose},
            {"offPose", st.offPose},
            {"trigger", st.trigger},
            {"aimStyle", aimStyleId(st.aim)},
            {"aimDrop", st.drop},
            {"aimReach", st.reach},
            {"holsterSpot", holsterSpot(st.holster).id},
            {"shape", shapeId(st.shape)},
            {"holster", strOf(g, p.node, "holster")},
            {"points", points},
            {"holds", holds},
            {"events", eventsOf(g, p)},
            {"holder", holder ? json(g.path(holder)) : json(nullptr)}};
}

// Commands ------------------------------------------------------------------------------------------------------------

namespace
{
/**
 * The prop's mesh points in its root's space; with sampled, its faces too, about a centimetre apart, so a low-poly box
 * reads by its shape and not by where its few corners happen to be.
 */
std::vector<glm::dvec3> meshPointsIn(Graph& g, const Prop& p, bool sampled = false)
{
    std::vector<glm::dvec3> out;
    const glm::dmat4 toRoot = glm::inverse(worldOf(g, p.root));
    struct Tri
    {
        glm::dvec3 a, b, c;
    };
    std::vector<Tri> tris;
    double area = 0.0;
    for (NodeId m : propMeshes(g, p))
    {
        const MeshPtr mesh = std::get<MeshPtr>(g.value({m, g.node(m)->type().attrIndex("outMesh"), -1}));
        if (!mesh)
            continue;
        const glm::dmat4 xf = toRoot * worldOf(g, m);
        auto at = [&](geo::Index i) { return glm::dvec3(xf * glm::dvec4(mesh->positions()[i], 1.0)); };
        for (const glm::dvec3& q : mesh->positions())
            out.push_back(glm::dvec3(xf * glm::dvec4(q, 1.0)));
        for (geo::Index f = 0; f < mesh->numFaces(); ++f)
        {
            const std::vector<geo::Index> fp = mesh->facePoints(f);
            for (size_t i = 1; i + 1 < fp.size(); ++i)
            {
                tris.push_back({at(fp[0]), at(fp[i]), at(fp[i + 1])});
                area += 0.5 * glm::length(glm::cross(tris.back().b - tris.back().a, tris.back().c - tris.back().a));
            }
        }
    }
    if (!sampled)
        return out;
    // A centimetre apart, coarser on a big prop so the points stay under about 200k.
    const double step = std::max(1.0, std::sqrt(area / 200000.0));
    for (const Tri& t : tris)
    {
        const double edge = std::max({glm::length(t.b - t.a), glm::length(t.c - t.b), glm::length(t.a - t.c)});
        const int n = std::min(400, int(std::ceil(edge / step)));
        if (n < 2)
            continue;
        for (int i = 0; i <= n; ++i)
            for (int j = 0; i + j <= n; ++j)
            {
                const double u = double(i) / n, v = double(j) / n;
                out.push_back(t.a + (t.b - t.a) * u + (t.c - t.a) * v);
            }
    }
    return out;
}

/** A point's node name: after its prop, so points of several props read apart in the outliner and never take a mesh's name. */
std::string pointName(Graph& g, const Prop& p, const std::string& name) { return g.node(p.root)->name() + "_" + name + "_point"; }

/** Makes or moves a point under the prop node to frame (prop root space); fitted when Fit Points made it. */
NodeId writePoint(Graph& g, const Prop& p, const PointDef& d, NodeId existing = kNoNode, bool fitted = true)
{
    NodeId id = existing;
    if (!id)
    {
        Node* n = g.createNode(*types::find(kPointType), pointName(g, p, d.name), p.node);
        id = n->id();
    }
    setPlug(g, id, "role", int64_t(d.role));
    setPlug(g, id, "radius", d.radius);
    setPlug(g, id, "fitted", fitted);
    g.evaluate();
    // The prop node rides the root with no offset, so the point's world is the root's times its frame.
    const glm::dmat4 world = rigidOf(worldOf(g, p.root)) * d.frame;
    setWorldRotation(g, id, glm::dmat3(world));
    g.evaluate();
    setWorldPosition(g, id, glm::dvec3(world[3]));
    g.evaluate();
    return id;
}

/** Fits the prop's points, told what its settings say unless hints are given, and returns the fit. */
Fit fitProp(Graph& g, Prop& p, std::optional<FitHints> given = std::nullopt)
{
    g.evaluate();
    const glm::dvec3 upLocal = glm::normalize(glm::transpose(glm::dmat3(rigidOf(worldOf(g, p.root)))) * up());
    FitHints hints = given ? *given : FitHints{p.settings.shape, p.settings.twoHanded};
    // The shape is read from the whole surface; the points are placed from the mesh's own vertices.
    if (hints.shape == Shape::Auto)
        hints.shape = classify(meshPointsIn(g, p, true), upLocal);
    const Fit fit = fitPoints(meshPointsIn(g, p), upLocal, hints);
    // Refitting replaces the points an earlier fit made and keeps those added or moved by hand.
    std::map<std::string, NodeId> byName;
    for (NodeId pt : p.points)
        byName[g.node(pt)->name()] = pt;
    std::set<NodeId> kept;
    for (const PointDef& d : fit.points)
    {
        const auto it = byName.find(pointName(g, p, d.name));
        if (it != byName.end() && !boolOf(g, it->second, "fitted"))
        {
            kept.insert(it->second);
            continue;
        }
        kept.insert(writePoint(g, p, d, it == byName.end() ? kNoNode : it->second));
    }
    for (NodeId pt : p.points)
        if (!kept.count(pt) && boolOf(g, pt, "fitted"))
            g.deleteNode(pt);
    g.evaluate();
    p = *load(g, p.node);
    if (fit.from != "mesh")
        log::write(LogLevel::Warning, "puppeteerPropFit: " + g.path(p.root) + ": " + fit.from);
    return fit;
}

json fitJson(const Fit& fit) { return {{"from", fit.from}, {"length", fit.length}, {"points", fit.points.size()}, {"shape", shapeId(fit.shape)}}; }

/** The prop named by a command's objects, else the selection, else the scene's only prop. */
Prop propArg(Graph& g, const Args& a, const char* cmd)
{
    std::vector<NodeId> cands;
    for (const std::string& s : objectArgs(a))
        cands.push_back(nodeNamed(g, s, cmd).id());
    if (cands.empty())
        cands = g.selection();
    for (NodeId c : cands)
        if (const NodeId pn = propFor(g, c))
            return *load(g, pn);
    const std::vector<NodeId> all = props(g);
    if (all.size() == 1 && objectArgs(a).empty())
        return *load(g, all.front());
    throw CommandError(std::string(cmd) + ": give a prop (puppeteerPropSetup makes one), or select it");
}

/**
 * The character a prop command works with: the flag, the one holding the prop at the command's time (else the last one
 * that held it before then, else the first that ever does), the selection's, or the only one.
 */
NodeId characterArg(Graph& g, const Args& a, const Prop& p, const char* cmd, const char* flag = "character")
{
    if (a.has(flag))
    {
        const NodeId c = characterFor(g, nodeNamed(g, a.get<std::string>(flag), cmd).id());
        if (!c)
            throw CommandError(std::string(cmd) + ": " + a.get<std::string>(flag) + " is not in a Puppeteer character");
        return c;
    }
    const double t = a.has("time") ? a.get<double>("time") : g.time();
    NodeId before = kNoNode, first = kNoNode;
    for (const Hold& h : holdsOf(g, p))
        if (h.target)
            if (const NodeId c = characterFor(g, h.target))
            {
                if (!first)
                    first = c;
                if (h.time <= t + 1e-9)
                    before = c;
            }
    if (before || first)
        return before ? before : first;
    for (NodeId s : g.selection())
        if (const NodeId c = characterFor(g, s))
            return c;
    const std::vector<NodeId> all = characters(g);
    if (all.size() == 1)
        return all.front();
    throw CommandError(std::string(cmd) + ": give character (the scene has " + std::to_string(all.size()) + ")");
}

struct HandRef
{
    hand::Hand h;
    bool right = true;
};

std::optional<HandRef> handOf(Graph& g, NodeId character, bool right)
{
    if (hand::handsOf(g, character).empty())
        setupHands(g, character);
    for (NodeId hn : hand::handsOf(g, character))
        if (std::optional<hand::Hand> h = hand::load(g, hn); h && (h->side == hand::Side::Right) == right && h->side != hand::Side::Center)
            return HandRef{*h, right};
    return std::nullopt;
}

HandRef needHand(Graph& g, NodeId character, bool right, const char* cmd)
{
    std::optional<HandRef> h = handOf(g, character, right);
    if (!h)
        throw CommandError(std::format("{}: {} has no {} hand the hand rig could find (puppeteerHandSetup)", cmd, g.node(character)->name(),
                                       right ? "right" : "left"));
    return *h;
}

/** The hand's measurements in its joint's rigid space. */
HandShape shapeOf(Graph& g, const hand::Hand& h)
{
    HandShape s;
    s.forward = h.forward;
    s.thumbSide = h.thumbSide;
    s.palm = h.palm;
    const glm::dmat4 toHand = glm::inverse(rigidOf(worldOf(g, h.joint)));
    glm::dvec3 k(0.0);
    int n = 0;
    for (const hand::Finger& f : h.fingers)
        if (f.role != hand::Role::Thumb && !f.joints.empty())
        {
            const glm::dvec3 knuckle(toHand * worldOf(g, f.joints.front())[3]);
            k += knuckle;
            ++n;
            double length = 0.0;
            for (size_t j = 1; j < f.joints.size(); ++j)
                length += glm::length(glm::dvec3(worldOf(g, f.joints[j])[3]) - glm::dvec3(worldOf(g, f.joints[j - 1])[3]));
            if (f.tip)
                length += glm::length(glm::dvec3(worldOf(g, f.tip)[3]) - glm::dvec3(worldOf(g, f.joints.back())[3]));
            // The grip's own estimate of a finger's thickness when it has no skin to measure.
            s.fingers.push_back({knuckle, std::max(0.1, 0.16 * length)});
        }
    if (n)
        s.knuckles = k / double(n);
    return s;
}

/** The prop's meshes as one surface in the point's rigid frame. */
std::vector<std::unique_ptr<geo::MeshBvh>> surfaceIn(Graph& g, const Prop& p, NodeId point)
{
    std::vector<std::unique_ptr<geo::MeshBvh>> out;
    const glm::dmat4 toPoint = glm::inverse(rigidOf(worldOf(g, point)));
    for (NodeId id : propMeshes(g, p))
    {
        const MeshPtr m = std::get<MeshPtr>(g.value({id, g.node(id)->type().attrIndex("outMesh"), -1}));
        if (!m || m->numFaces() == 0)
            continue;
        geo::Mesh local = *m;
        const glm::dmat4 xf = toPoint * worldOf(g, id);
        for (glm::dvec3& q : local.positions())
            q = glm::dvec3(xf * glm::dvec4(q, 1.0));
        out.push_back(std::make_unique<geo::MeshBvh>(local));
    }
    return out;
}

/** Distance from q to the nearest surface, up to within. */
double gapTo(const std::vector<std::unique_ptr<geo::MeshBvh>>& surface, const glm::dvec3& q, double within = 100.0)
{
    double best = within;
    for (const auto& bvh : surface)
    {
        geo::BvhClosest c;
        if (bvh->closestPoint(q, c, within))
            best = std::min(best, c.distance);
    }
    return best;
}

/**
 * The hand's grip frame on a point (the point's frame in the hand joint's space), fitted to the prop's own surface: the
 * hand slides along the handle until the fingers that wrap lie on it, then sits as close as the knuckles allow, each
 * clearing the surface toward it by its finger's thickness, so every finger can close onto it. trigger leaves the index
 * out of the fingers that must lie on the handle.
 */
glm::dmat4 gripOf(Graph& g, const hand::Hand& h, const Prop& p, NodeId point, bool trigger = false)
{
    const HandShape shape = shapeOf(g, h);
    const double radius = numOf(g, point, "radius");
    glm::dmat4 frame = gripFrame(shape, radius);
    const auto surface = surfaceIn(g, p, point);
    if (surface.empty())
        return frame;
    struct Clear
    {
        glm::dvec3 at;
        double thickness;
        bool wraps;
    };
    std::vector<Clear> clears;
    double mean = 0.0;
    for (size_t i = 0, k = 0; i < h.fingers.size(); ++i)
        if (h.fingers[i].role != hand::Role::Thumb && !h.fingers[i].joints.empty() && k < shape.fingers.size())
        {
            const HandShape::Finger& f = shape.fingers[k++];
            clears.push_back({f.knuckle, f.thickness, !(trigger && h.fingers[i].role == hand::Role::Index)});
            mean += f.thickness;
        }
    if (clears.empty())
        return frame;
    mean /= double(clears.size());
    // The heel of the palm halfway to the knuckles keeps the handle out of the palm too.
    clears.push_back({0.5 * shape.knuckles, 1.2 * mean, false});
    for (int round = 0; round < 4; ++round)
    {
        // Along the handle: the wrapping fingers' span inside the handle's ends (where the axis is within the radius of it).
        double lo = 1e300, hi = -1e300;
        for (const Clear& c : clears)
            if (c.wraps)
            {
                const double x = glm::dvec3(glm::inverse(frame) * glm::dvec4(c.at, 1.0)).x;
                lo = std::min(lo, x - c.thickness);
                hi = std::max(hi, x + c.thickness);
            }
        auto present = [&](double x) { return gapTo(surface, {x, 0.0, 0.0}, radius + 1.0) <= radius; };
        double slide = 0.0;
        if (lo < hi && present(0.0))
        {
            const double step = 0.25;
            double up = 0.0, down = 0.0;
            while (up < 40.0 && present(up + step))
                up += step;
            while (down < 40.0 && present(-down - step))
                down += step;
            if (hi - lo >= up + down)
                slide = 0.5 * (up - down) - 0.5 * (lo + hi);
            else if (hi > up)
                slide = up - hi;
            else if (lo < -down)
                slide = -down - lo;
        }
        frame = frame * glm::translate(glm::dmat4(1.0), glm::dvec3(-slide, 0.0, 0.0));
        // Off the palm: as close as the tightest knuckle allows, each clearing the surface by a thickness and a quarter.
        double shift = -1e300;
        for (const Clear& c : clears)
        {
            const glm::dvec3 q(glm::inverse(frame) * glm::dvec4(c.at, 1.0));
            const double need = 1.25 * c.thickness;
            // Moving the handle off the palm by s takes the knuckle to q - s Z in the point's frame.
            auto gap = [&](double s) { return gapTo(surface, q - glm::dvec3(0.0, 0.0, s), need + 20.0); };
            // Coming in from well off the palm, the first place the knuckle would get too close.
            double b = q.z + 20.0;
            if (gap(b) < need)
                continue;
            double a = b;
            while (a > q.z - 30.0 && gap(a) >= need)
                a -= 0.25;
            if (gap(a) >= need)
                continue;
            b = a + 0.25;
            for (int i = 0; i < 20; ++i)
            {
                const double m = 0.5 * (a + b);
                (gap(m) < need ? a : b) = m;
            }
            shift = std::max(shift, b);
        }
        if (shift > -1e299)
            frame = frame * glm::translate(glm::dmat4(1.0), glm::dvec3(0.0, 0.0, shift));
    }
    return frame;
}

/** The grip point a hold uses: by name (or role id), else the role's first point. */
NodeId gripPoint(Graph& g, const Prop& p, const std::string& name, Role fallback, const char* cmd)
{
    if (!name.empty())
    {
        for (NodeId pt : p.points)
        {
            const std::string& n = g.node(pt)->name();
            if (n == name || n == pointName(g, p, name) || roleId(roleOf(*g.node(pt))) == name)
                return pt;
        }
        throw CommandError(std::format("{}: {} has no point named {}", cmd, g.node(p.root)->name(), name));
    }
    return p.point(g, fallback);
}

int ensureSpace(Graph& g, NodeId node, NodeId target)
{
    const int found = sp::find(g, node, g.path(target));
    if (found >= 0)
        return found;
    return sp::addSpace(g, node, g.path(target), false);
}

int worldSpace(Graph& g, NodeId node)
{
    const int found = sp::find(g, node, "world");
    return found >= 0 ? found : sp::addSpace(g, node, "world", false);
}

/** Keys the root's translate and rotate at t so its world matrix is world there (in its active space then). */
void keyRootAt(Graph& g, NodeId root, const glm::dmat4& world, double t)
{
    at(g, t);
    setWorldRotation(g, root, glm::dmat3(rigidOf(world)));
    g.evaluate();
    setWorldPosition(g, root, glm::dvec3(world[3]));
    const std::string path = g.path(root);
    const glm::dvec3 tr = vecOf(g, root, Transform::kTranslate), ro = vecOf(g, root, Transform::kRotate);
    keyVec(path, "translate", tr, t);
    keyVec(path, "rotate", ro, t);
    g.evaluate();
}

/**
 * Keys the prop settling from where it is at t into `end` (a pivot's frame in the space's frame; no space target is the
 * world) by t + blend: every frame between, the pivot's frame against the space eases there rigidly about the pivot
 * itself. local is the pivot's frame in the root's space. Interpolating the root's own channels instead swings a prop
 * whose origin is far from its mesh through a wide arc.
 */
void settleAbout(Graph& g, const Prop& p, const glm::dmat4& local, NodeId spaceTarget, const glm::dmat4& end, double t, double blend)
{
    auto spaceAt = [&](double time) {
        at(g, time);
        return spaceTarget ? rigidOf(worldOf(g, spaceTarget)) : glm::dmat4(1.0);
    };
    const glm::dmat4 start = glm::inverse(spaceAt(t)) * rigidOf(worldOf(g, p.root)) * local;
    const glm::dquat q0 = glm::quat_cast(glm::dmat3(start)), q1 = glm::quat_cast(glm::dmat3(end));
    const long long frames = std::max<long long>(1, std::llround(blend * fps()));
    for (long long f = 1; f <= frames; ++f)
    {
        const double time = blend <= 0.0 ? t : t + blend * double(f) / double(frames);
        const double u = double(f) / double(frames), e = u * u * (3.0 - 2.0 * u);
        glm::dmat4 rel = glm::dmat4(glm::mat3_cast(glm::slerp(q0, q1, e)));
        rel[3] = glm::dvec4(glm::mix(glm::dvec3(start[3]), glm::dvec3(end[3]), e), 1.0);
        keyRootAt(g, p.root, placeOn(spaceAt(time) * rel, local), time);
    }
}

void settleInto(Graph& g, const Prop& p, NodeId point, NodeId spaceTarget, const glm::dmat4& end, double t, double blend)
{
    settleAbout(g, p, pointLocal(g, p, point), spaceTarget, end, t, blend);
}

/** Drops the bake layers of the character's prop grapple, leaving the animation itself. */
void dropSolve(Graph& g, NodeId grapple)
{
    if (!grapple)
        return;
    Node& n = *g.node(grapple);
    const json layers = json::parse(textOf(n, sim::kRecordingsAttr).empty() ? std::string("[]") : textOf(n, sim::kRecordingsAttr), nullptr, false);
    if (layers.is_array())
        for (const json& l : layers)
            if (l.is_string() && named(g, l.get<std::string>()))
                ops::execute("deleteAnimLayer", {{"args", {l.get<std::string>()}}});
    setPlug(g, grapple, sim::kRecordingsAttr, std::string("[]"));
    g.evaluate();
}

/** Keys a weight at t. */
void keyWeight(Graph& g, NodeId node, double t, double w) { anim::setKey(g, plugOf(*g.node(node), "weight"), t, w); }

double weightAt(Graph& g, NodeId node, double t)
{
    at(g, t);
    return numOf(g, node, "weight");
}

/** Keys a weight from what it is at t to `to` at t + blend (a hard switch a frame apart with no blend). */
void rampWeight(Graph& g, NodeId node, double t, double blend, double to)
{
    const PlugRef plug = plugOf(*g.node(node), "weight");
    const double end = t + std::max(blend, 1.0 / fps());
    const double from = weightAt(g, node, t);
    const double later = weightAt(g, node, end);
    if (std::abs(from - to) < 1e-9 && std::abs(later - to) < 1e-9)
        return;
    anim::setKey(g, plug, t, from);
    anim::setKey(g, plug, end, to);
}

/** The prop's frame contacts in the character's grapple: source joint path, frame path, node. */
struct PropContact
{
    NodeId node;
    std::string source, target, frame;
};
std::vector<PropContact> propContacts(Graph& g, NodeId grapple, const Prop& p)
{
    std::vector<PropContact> out;
    if (!grapple)
        return out;
    std::set<std::string> mine;
    for (NodeId pt : p.points)
        mine.insert(g.path(pt));
    for (NodeId c : gp::contactsOf(g, grapple))
        if (gp::contactFollowsFrame(g, c) && mine.count(strOf(g, c, "frame")))
            out.push_back({c, strOf(g, c, "source"), strOf(g, c, "target"), strOf(g, c, "frame")});
    return out;
}

NodeId contactFor(Graph& g, NodeId grapple, const Prop& p, const hand::Hand& source, NodeId targetJoint, NodeId point)
{
    const std::string s = g.path(source.joint), t = targetJoint ? g.path(targetJoint) : std::string(), f = g.path(point);
    for (const PropContact& c : propContacts(g, grapple, p))
        if (c.source == s && c.target == t && c.frame == f)
            return c.node;
    Node* n = g.createNode(*types::find(gp::kContactType), g.node(source.joint)->name() + "_on_" + g.node(point)->name(), grapple);
    const NodeId id = n->id();
    setPlug(g, id, "source", s);
    setPlug(g, id, "target", t);
    setPlug(g, id, "frame", f);
    setPlug(g, id, "orient", true);
    setPlug(g, id, "gripOffset", gripOf(g, source, p, point));
    // Off until keyed on.
    setPlug(g, id, "weight", 0.0);
    anim::setKey(g, plugOf(*n, "weight"), anim::playback().minTime, 0.0);
    return id;
}

/** The hands holding the prop at t: by its space, plus every contact of it on there. */
std::set<NodeId> handsHolding(Graph& g, NodeId grapple, const Prop& p, double t)
{
    std::set<NodeId> out;
    at(g, t);
    if (const NodeId j = holderNow(g, p))
        out.insert(j);
    for (const PropContact& c : propContacts(g, grapple, p))
        if (numOf(g, c.node, "weight") > 0.5)
            if (Node* s = named(g, c.source))
                out.insert(s->id());
    return out;
}

/**
 * Closes the hand's fingers onto the prop from open, at the current time, keyed. A finger that starts in the prop opens
 * out of it and closes back onto it; only one that can't get out (its knuckle in the prop) keeps the class pose, which
 * puppeteerHandGrip logs.
 */
void wrapFingers(Graph& g, const hand::Hand& h, const Prop& p, const std::string& pose, bool trigger)
{
    json targets = json::array();
    for (NodeId m : propMeshes(g, p))
        targets.push_back(g.path(m));
    if (targets.empty())
        return;
    const std::string hn = g.path(h.node);
    const json fingers = trigger ? json::array({"thumb", "middle", "ring", "pinky"}) : json::array({"thumb", "index", "middle", "ring", "pinky"});
    ops::execute("puppeteerHandPose", {{"args", {hn}}, {"pose", "flat"}, {"fingers", fingers}, {"key", false}});
    const json got = ops::execute("puppeteerHandGrip", {{"args", {hn}}, {"target", targets}, {"fingers", fingers}, {"key", true}});
    json stuck = json::array();
    const json fingerResults = got.is_array() && !got.empty() ? got.at(0).value("fingers", json::object()) : json::object();
    for (const auto& [role, r] : fingerResults.items())
        if (r.value("stayed", false))
            stuck.push_back(role);
    if (!stuck.empty())
        ops::execute("puppeteerHandPose", {{"args", {hn}}, {"pose", pose}, {"fingers", stuck}, {"key", true}});
}

/** Keys a hand's fingers at t0 as they are, then poses (and wraps onto the prop's meshes) at t1. */
void poseFingers(Graph& g, const hand::Hand& h, double t0, double t1, const std::string& pose, bool wrap, bool trigger, const Prop* p)
{
    const std::string hn = g.path(h.node);
    at(g, t0);
    ops::execute("puppeteerHandKey", {{"args", {hn}}});
    at(g, t1);
    ops::execute("puppeteerHandPose", {{"args", {hn}}, {"pose", pose}, {"key", true}});
    if (wrap && p)
        wrapFingers(g, h, *p, pose, trigger);
}

double blendArg(const Args& a) { return std::max(0.0, a.get<double>("blend", 0.2)); }

/** Solves the character's prop grapple over the playback range onto its layer. */
json solveGrapple(Graph& g, NodeId grapple, const Args& a)
{
    json call = {{"args", {g.path(grapple)}}};
    if (a.has("start"))
        call["start"] = a.get<int64_t>("start");
    if (a.has("end"))
        call["end"] = a.get<int64_t>("end");
    return ops::execute("bakeGrapple", call);
}

const char* presetFlagDoc()
{
    static const std::string doc = [] {
        std::string s = "A preset that fills the prop's settings (hands, hand poses, aim, holster spot, how its shape is read):";
        for (const Preset& p : presets())
            s += std::string(" ") + p.id;
        return s + ". Default: none; a new prop takes its settings from its shape.";
    }();
    return doc.c_str();
}

const Preset* presetArg(const Args& a, const char* cmd)
{
    if (!a.has("preset"))
        return nullptr;
    const std::string id = a.get<std::string>("preset");
    if (const Preset* p = presetFor(id))
        return p;
    throw CommandError(std::format("{}: no preset '{}'; one of {}", cmd, id, [] {
        std::string s;
        for (const Preset& p : presets())
            s += (s.empty() ? "" : ", ") + std::string(p.id);
        return s;
    }()));
}

/** A joint of the character by bare name (no namespace), first in the given order; kNoNode when none matches. */
NodeId jointByNames(Graph& g, NodeId character, const std::vector<std::string>& names)
{
    const NodeId root = characterRoot(g, character);
    std::vector<NodeId> joints{root};
    for (NodeId d : g.descendants(root))
        if (rig::isJoint(*g.node(d)))
            joints.push_back(d);
    auto bare = [&](NodeId j) {
        std::string n = g.node(j)->name();
        if (const size_t c = n.rfind(':'); c != std::string::npos)
            n = n.substr(c + 1);
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        return n;
    };
    for (const std::string& want : names)
        for (NodeId j : joints)
            if (bare(j) == want)
                return j;
    return kNoNode;
}

/** The character's right, up and forward in the world, from its hands (right minus left, levelled). */
glm::dmat3 characterFrame(Graph& g, NodeId character)
{
    const glm::dvec3 u = up();
    glm::dvec3 right(-1.0, 0.0, 0.0);
    const std::optional<HandRef> r = handOf(g, character, true), l = handOf(g, character, false);
    if (r && l)
    {
        NodeId ra = g.parent(g.parent(r->h.joint)), la = g.parent(g.parent(l->h.joint));
        glm::dvec3 d = glm::dvec3(worldOf(g, ra ? ra : r->h.joint)[3]) - glm::dvec3(worldOf(g, la ? la : l->h.joint)[3]);
        d -= u * glm::dot(d, u);
        if (glm::length(d) > 1e-6)
            right = glm::normalize(d);
    }
    return glm::dmat3(right, u, glm::normalize(glm::cross(u, right)));
}

/** The prop's holster point on the character, made from the class's default when it has none. */
NodeId holsterFor(Graph& g, const Prop& p, NodeId character, const char* cmd)
{
    if (Node* h = named(g, strOf(g, p.node, "holster")); h && characterFor(g, h->id()) == character)
        return h->id();
    const HolsterDefault& hd = holsterSpot(p.settings.holster);
    const NodeId joint = jointByNames(g, character, hd.joints);
    if (!joint)
        throw CommandError(std::format("{}: {} has no joint for a {} holster (looked for {})", cmd, g.node(character)->name(), hd.label, hd.joints.front()));
    g.evaluate();
    const glm::dmat3 cf = characterFrame(g, character);
    const glm::dvec3 origin = glm::dvec3(worldOf(g, joint)[3]) + cf * hd.offset;
    const glm::dmat4 frame = frameXZ(origin, cf * hd.x, cf * hd.z);
    Node* n = g.createNode(*types::find(kPointType), g.node(p.root)->name() + "_holster", joint);
    const NodeId id = n->id();
    setPlug(g, id, "role", int64_t(Role::Holster));
    g.evaluate();
    setWorldRotation(g, id, glm::dmat3(frame));
    g.evaluate();
    setWorldPosition(g, id, glm::dvec3(frame[3]));
    setPlug(g, p.node, "holster", g.path(id));
    g.evaluate();
    return id;
}

/** The shoulder pocket an aimed stock goes to: a point under the upper arm, made once per side. */
NodeId shoulderFor(Graph& g, NodeId character, const hand::Hand& h)
{
    const NodeId lower = g.parent(h.joint), arm = lower ? g.parent(lower) : kNoNode;
    if (!arm || !rig::isJoint(*g.node(arm)))
        return kNoNode;
    // On the collarbone, which the aim holds still, so the pocket doesn't swing with the arm.
    const NodeId collar = g.parent(arm);
    const NodeId upper = collar && rig::isJoint(*g.node(collar)) ? collar : arm;
    const std::string name = std::string("propShoulder_") + (h.side == hand::Side::Left ? "l" : "r");
    for (NodeId c : g.children(upper))
        if (g.node(c)->name() == name || g.node(c)->name().ends_with(":" + name))
            return c;
    g.evaluate();
    const glm::dmat3 cf = characterFrame(g, character);
    const double side = h.side == hand::Side::Left ? -1.0 : 1.0;
    // In front of the shoulder joint and toward the chest, where a stock sits.
    const glm::dvec3 at3 = glm::dvec3(worldOf(g, arm)[3]) + cf[2] * 6.0 - cf[0] * side * 4.0 - cf[1] * 3.0;
    Node* n = g.createNode(*types::find(kPointType), name, upper);
    setPlug(g, n->id(), "role", int64_t(Role::Stock));
    g.evaluate();
    setWorldPosition(g, n->id(), at3);
    g.evaluate();
    return n->id();
}

const FlagDef kTimeFlag{"time", "t", ArgType::Float, nullptr, "Seconds the change happens at. Default: now."};
const FlagDef kBlendFlag{"blend", "b", ArgType::Float, 0.2, "Seconds after time the new hold settles over, so nothing pops. 0 snaps.", {}, {}, 0.0};
const FlagDef kSolveFlag{"solve", "sv", ArgType::Bool, true, "Solve the arms over the playback range afterwards (puppeteerPropSolve)."};
const FlagDef kCharacterFlag{"character", "ch", ArgType::String, nullptr, "The character. Default: the one holding the prop, else the selected or only one."};

/** Lets go of values set and keyed at one time (they read the same from their curves), so the solve can sample. */
void releaseHeld(Graph& g)
{
    const double t = g.time();
    g.setTime(t + 1.0, false);
    g.setTime(t, false);
    g.evaluate();
}

json finish(Graph& g, const Prop& p, NodeId grapple, const Args& a, json extra)
{
    releaseHeld(g);
    if (a.get<bool>("solve", true) && grapple)
        extra["solve"] = solveGrapple(g, grapple, a);
    else if (grapple)
    {
        // Unsolved, the scene shows the animation itself rather than a solve of the holds before this edit.
        dropSolve(g, grapple);
        extra["solve"] = nullptr;
    }
    json out = propJson(g, *load(g, p.node));
    for (auto& [k, v] : extra.items())
        out[k] = v;
    return out;
}

// puppeteerPropHold -------------------------------------------------------------------------------------------------

json holdCmd(const Args& a)
{
    Graph& g = Graph::current();
    const char* cmd = "puppeteerPropHold";
    Prop p = propArg(g, a, cmd);
    const NodeId character = characterArg(g, a, p, cmd);
    const Settings& st = p.settings;
    const std::string handName = a.get<std::string>("hand", "main");
    if (handName != "main" && handName != "right" && handName != "left")
        throw CommandError(std::string(cmd) + ": hand is main, right or left");
    const bool right = handName == "main" ? p.rightHanded : handName == "right";
    const int hands = int(a.get<int64_t>("hands", st.twoHanded ? 2 : 1));
    if (hands != 1 && hands != 2)
        throw CommandError(std::string(cmd) + ": hands is 1 or 2");
    const HandRef main = needHand(g, character, right, cmd);
    std::optional<HandRef> off;
    if (hands == 2)
        off = needHand(g, character, !right, cmd);
    const NodeId mainPt = gripPoint(g, p, a.get<std::string>("point", ""), Role::Main, cmd);
    if (!mainPt)
        throw CommandError(std::string(cmd) + ": " + g.node(p.root)->name() + " has no main grip point (puppeteerPropFit)");
    NodeId offPt = kNoNode;
    if (off)
    {
        offPt = gripPoint(g, p, a.get<std::string>("offPoint", ""), Role::Off, cmd);
        if (!offPt)
            offPt = p.point(g, Role::Alt);
        if (!offPt)
            throw CommandError(std::string(cmd) + ": " + g.node(p.root)->name() + " has no off grip point for a second hand");
    }

    const KeepTime keep(g);
    const double t = a.has("time") ? a.get<double>("time") : g.time();
    const double blend = blendArg(a);
    const NodeId grapple = propGrapple(g, character, true);
    const std::set<NodeId> before = handsHolding(g, grapple, p, t);
    // Reaching: the hand goes to the prop where it is over the reach before t, and takes it there.
    const double reach = std::min(std::max(0.0, a.get<double>("reach", 0.0)), t - anim::playback().minTime);
    if (reach > 0.0)
    {
        const NodeId pin = contactFor(g, grapple, p, main.h, kNoNode, mainPt);
        setPlug(g, pin, "gripOffset", gripOf(g, main.h, p, mainPt, st.trigger));
        keyWeight(g, pin, t - reach, 0.0);
        keyWeight(g, pin, t, 1.0);
    }

    // The switch keeps the prop where it is at t; by t + blend it sits in the grip (at once when the hand reached it).
    at(g, t);
    const int space = ensureSpace(g, p.root, main.h.joint);
    sp::SwitchOptions so;
    so.time = t;
    so.key = true;
    sp::switchSpace(g, p.root, space, so);
    const double settle = reach > 0.0 ? t : t + blend;
    settleInto(g, p, mainPt, main.h.joint, gripOf(g, main.h, p, mainPt, st.trigger), t, reach > 0.0 ? 0.0 : blend);

    // Contacts: the off hand on its grip, riding the main hand; every other contact of this prop lets go.
    NodeId offContact = kNoNode;
    if (off)
        offContact = contactFor(g, grapple, p, off->h, main.h.joint, offPt);
    for (const PropContact& c : propContacts(g, grapple, p))
        rampWeight(g, c.node, t, blend, c.node == offContact ? 1.0 : 0.0);
    std::set<std::string> using_{g.path(main.h.joint)};
    if (off)
        using_.insert(g.path(off->h.joint));
    for (const NodeId other : props(g))
        if (const std::optional<Prop> op = load(g, other); op && op->node != p.node)
        {
            for (const PropContact& c : propContacts(g, grapple, *op))
                if (Node* s = named(g, c.source); s && using_.count(g.path(s->id())) && weightAt(g, c.node, t) > 1e-6)
                    rampWeight(g, c.node, t, blend, 0.0);
            at(g, settle);
            if (const NodeId h = holderNow(g, *op); h && using_.count(g.path(h)))
                log::write(LogLevel::Warning, std::format("{}: {} is still in {} at frame {:g}; holster or drop it", cmd, g.node(op->root)->name(),
                                                          g.node(h)->name(), settle * fps()));
        }

    // Fingers: the holding hands close over the blend, a hand that let go opens.
    std::set<NodeId> after{main.h.joint};
    if (off)
        after.insert(off->h.joint);
    if (a.get<bool>("fingers", true))
    {
        poseFingers(g, main.h, reach > 0.0 ? t - reach : t, settle, st.mainPose, true, st.trigger, &p);
        if (off)
            poseFingers(g, off->h, t, settle, st.offPose, false, false, nullptr);
        for (NodeId j : before)
            if (!after.count(j))
                if (const NodeId hn = hand::handFor(g, j))
                    if (const std::optional<hand::Hand> h = hand::load(g, hn))
                        poseFingers(g, *h, t, settle, "relaxed", false, false, nullptr);
    }
    json extra = {{"main", g.path(main.h.joint)}, {"mainPoint", g.path(mainPt)}};
    json ev = {{"time", t}, {"kind", a.get<std::string>("event", reach > 0.0 ? "take" : "hold")}, {"hand", g.path(main.h.joint)},
               {"point", g.path(mainPt)}, {"character", g.path(character)}};
    if (off)
    {
        extra["off"] = g.path(off->h.joint);
        extra["offPoint"] = g.path(offPt);
        extra["contact"] = g.path(offContact);
        ev["offHand"] = g.path(off->h.joint);
        ev["offPoint"] = g.path(offPt);
    }
    recordEvent(g, p, ev);
    json out = finish(g, p, grapple, a, extra);
    // The off hand reaches its grip in the solve; its fingers wrap once it is there.
    if (off && a.get<bool>("fingers", true) && a.get<bool>("solve", true))
    {
        at(g, settle);
        wrapFingers(g, off->h, p, st.offPose, false);
    }
    return out;
}

// puppeteerPropDrop / puppeteerPropHolster -----------------------------------------------------------------------

/** Lets every hand go of the prop over [t, t + blend]: contacts off, fingers relaxed. */
void release(Graph& g, NodeId grapple, const Prop& p, double t, double blend, bool fingers)
{
    const std::set<NodeId> before = handsHolding(g, grapple, p, t);
    for (const PropContact& c : propContacts(g, grapple, p))
        rampWeight(g, c.node, t, blend, 0.0);
    if (fingers)
        for (NodeId j : before)
            if (const NodeId hn = hand::handFor(g, j))
                if (const std::optional<hand::Hand> h = hand::load(g, hn))
                    poseFingers(g, *h, t, t + std::max(blend, 1.0 / fps()), "relaxed", false, false, nullptr);
}

json dropCmd(const Args& a)
{
    Graph& g = Graph::current();
    const char* cmd = "puppeteerPropDrop";
    Prop p = propArg(g, a, cmd);
    const KeepTime keep(g);
    const double t = a.has("time") ? a.get<double>("time") : g.time();
    NodeId character = kNoNode;
    for (const Hold& h : holdsOf(g, p))
        if (h.target && !character)
            character = characterFor(g, h.target);
    const NodeId grapple = character ? propGrapple(g, character, false) : kNoNode;
    release(g, grapple, p, t, blendArg(a), a.get<bool>("fingers", true));
    at(g, t);
    sp::SwitchOptions so;
    so.time = t;
    so.key = true;
    sp::switchSpace(g, p.root, worldSpace(g, p.root), so);
    recordEvent(g, p, {{"time", t}, {"kind", "drop"}});
    return finish(g, p, grapple, a, json::object());
}

json holsterCmd(const Args& a)
{
    Graph& g = Graph::current();
    const char* cmd = "puppeteerPropHolster";
    Prop p = propArg(g, a, cmd);
    const NodeId character = characterArg(g, a, p, cmd);
    const KeepTime keep(g);
    const double t = a.has("time") ? a.get<double>("time") : g.time();
    const double blend = blendArg(a);
    NodeId holster = kNoNode;
    if (a.has("holster"))
    {
        Node& n = nodeNamed(g, a.get<std::string>("holster"), cmd);
        if (n.type().attrIndex("worldMatrix") < 0)
            throw CommandError(std::string(cmd) + ": " + n.path() + " is not a transform");
        holster = n.id();
        setPlug(g, p.node, "holster", g.path(holster));
    }
    else
        holster = holsterFor(g, p, character, cmd);
    const NodeId mainPt = p.point(g, Role::Main);
    if (!mainPt)
        throw CommandError(std::string(cmd) + ": " + g.node(p.root)->name() + " has no main grip point");
    const NodeId grapple = propGrapple(g, character, true);
    release(g, grapple, p, t, blend, a.get<bool>("fingers", true));
    at(g, t);
    sp::SwitchOptions so;
    so.time = t;
    so.key = true;
    sp::switchSpace(g, p.root, ensureSpace(g, p.root, holster), so);
    settleInto(g, p, mainPt, holster, glm::dmat4(1.0), t, blend);
    recordEvent(g, p, {{"time", t}, {"kind", "holster"}, {"character", g.path(character)}});
    return finish(g, p, grapple, a, {{"holster", g.path(holster)}});
}

// puppeteerPropAim ----------------------------------------------------------------------------------------------------

AimStyle styleFor(AimStyle own, const std::string& s, const char* cmd)
{
    if (s.empty() || s == "auto")
        return own;
    for (AimStyle st : {AimStyle::Turn, AimStyle::Shoulder, AimStyle::Extend, AimStyle::Guard})
        if (s == aimStyleId(st))
            return st;
    throw CommandError(std::string(cmd) + ": style is auto, turn, shoulder, extend or guard");
}

json aimCmd(const Args& a)
{
    Graph& g = Graph::current();
    const char* cmd = "puppeteerPropAim";
    Prop p = propArg(g, a, cmd);
    const NodeId character = characterArg(g, a, p, cmd);
    const Settings& st = p.settings;
    const KeepTime keep(g);
    const double t = a.has("time") ? a.get<double>("time") : g.time();
    const double blend = blendArg(a);
    const NodeId grapple = propGrapple(g, character, true);
    at(g, t + blend);
    const NodeId holder = holderNow(g, p);
    if (!holder)
        throw CommandError(std::string(cmd) + ": " + g.node(p.root)->name() + " isn't in a hand then; hold it first (puppeteerPropHold)");

    // The aims of this prop: one per holding joint, reused when aimed again.
    std::vector<NodeId> mine;
    std::set<std::string> points;
    for (NodeId pt : p.points)
        points.insert(g.path(pt));
    for (NodeId n : gp::aimsOf(g, grapple))
        if (points.count(strOf(g, n, "axis")))
            mine.push_back(n);

    if (a.get<bool>("off", false))
    {
        for (NodeId n : mine)
            rampWeight(g, n, t, blend, 0.0);
        return finish(g, p, grapple, a, json::object());
    }
    if (!a.has("target"))
        throw CommandError(std::string(cmd) + ": give target, the transform to aim at");
    const NodeId target = nodeNamed(g, a.get<std::string>("target"), cmd).id();
    const NodeId axis = p.point(g, Role::Muzzle) ? p.point(g, Role::Muzzle) : p.point(g, Role::Tip);
    if (!axis)
        throw CommandError(std::string(cmd) + ": " + g.node(p.root)->name() + " has no muzzle or tip point to aim");
    const AimStyle style = styleFor(st.aim, a.get<std::string>("style", "auto"), cmd);
    const NodeId hn = hand::handFor(g, holder);
    const std::optional<hand::Hand> h = hn ? hand::load(g, hn) : std::nullopt;
    NodeId anchor = kNoNode;
    if (style == AimStyle::Shoulder && h)
        anchor = shoulderFor(g, character, *h);
    else if (style != AimStyle::Turn)
    {
        const NodeId lower = g.parent(holder);
        anchor = lower ? g.parent(lower) : kNoNode;
    }
    const NodeId pivot = style == AimStyle::Shoulder && p.point(g, Role::Stock) ? p.point(g, Role::Stock) : p.point(g, Role::Main);

    NodeId aim = kNoNode;
    for (NodeId n : mine)
        if (strOf(g, n, "joint") == g.path(holder))
            aim = n;
    if (!aim)
    {
        json call = {{"args", {g.path(holder), g.path(target)}}, {"axis", g.path(axis)}, {"grapple", g.path(grapple)},
                     {"style", anchor ? aimStyleId(style) : "turn"},    {"blend", 0.0},           {"name", g.node(p.root)->name() + "_aim"}};
        if (anchor)
            call["anchor"] = g.path(anchor);
        const json made = ops::execute("grappleAim", call);
        aim = g.find(made.at("aim").get<std::string>())->id();
        setPlug(g, aim, "weight", 0.0);
        anim::setKey(g, plugOf(*g.node(aim), "weight"), anim::playback().minTime, 0.0);
    }
    setPlug(g, aim, "target", g.path(target));
    setPlug(g, aim, "axis", g.path(axis));
    setPlug(g, aim, "pivot", pivot ? g.path(pivot) : std::string());
    setPlug(g, aim, "anchor", anchor ? g.path(anchor) : std::string());
    setPlug(g, aim, "style", int64_t(anchor ? style : AimStyle::Turn));
    setPlug(g, aim, "drop", style == AimStyle::Guard ? st.drop : 0.0);
    if (anchor && (style == AimStyle::Extend || style == AimStyle::Guard))
    {
        double arm = 0.0;
        for (NodeId j = holder; j && j != anchor; j = g.parent(j))
            arm += glm::length(glm::dvec3(worldOf(g, j)[3]) - glm::dvec3(worldOf(g, g.parent(j))[3]));
        setPlug(g, aim, "reach", st.reach * arm);
    }
    const double w = std::clamp(a.get<double>("weight", 1.0), 0.0, 1.0);
    rampWeight(g, aim, t, blend, w);
    for (NodeId n : mine)
        if (n != aim)
            rampWeight(g, n, t, blend, 0.0);
    return finish(g, p, grapple, a, {{"aim", g.path(aim)}, {"style", aimStyleId(anchor ? style : AimStyle::Turn)}, {"target", g.path(target)}});
}

// Handoffs: shared pieces ----------------------------------------------------------------------------------------------

/** The prop's mesh in its root's space, measured: its box along its principal axes, longest first. */
struct PropShape
{
    glm::dvec3 centre{0.0};
    std::array<glm::dvec3, 3> axes{glm::dvec3(1, 0, 0), glm::dvec3(0, 1, 0), glm::dvec3(0, 0, 1)};
    glm::dvec3 lo{-1.0}, hi{1.0};
    bool measured = false;
    /** A point by its place in the box: -1 one face, 1 the other, along each axis. */
    glm::dvec3 at(const glm::dvec3& f) const
    {
        glm::dvec3 p = centre;
        for (int k = 0; k < 3; ++k)
            p += axes[size_t(k)] * (0.5 * (lo[k] + hi[k]) + 0.5 * (hi[k] - lo[k]) * f[k]);
        return p;
    }
    std::array<glm::dvec3, 8> corners() const
    {
        std::array<glm::dvec3, 8> out;
        for (int i = 0; i < 8; ++i)
            out[size_t(i)] = at({i & 1 ? 1.0 : -1.0, i & 2 ? 1.0 : -1.0, i & 4 ? 1.0 : -1.0});
        return out;
    }
    glm::dvec3 middle() const { return at(glm::dvec3(0.0)); }
    glm::dvec3 extent() const { return hi - lo; }
};

PropShape shapeIn(Graph& g, const Prop& p)
{
    PropShape s;
    const std::vector<glm::dvec3> pts = meshPointsIn(g, p);
    if (pts.size() < 4)
        return s;
    const detail::Frame f = detail::frameOf(pts);
    s.centre = f.centre;
    s.axes = f.axes;
    s.lo = glm::dvec3(1e300);
    s.hi = glm::dvec3(-1e300);
    for (const glm::dvec3& q : pts)
        for (int k = 0; k < 3; ++k)
        {
            const double v = glm::dot(q - f.centre, f.axes[size_t(k)]);
            s.lo[k] = std::min(s.lo[k], v);
            s.hi[k] = std::max(s.hi[k], v);
        }
    s.measured = true;
    return s;
}

/** What a prop can land, stand or rest on: scene meshes (props and characters left out) and the ground. */
struct Surfaces
{
    struct Item
    {
        NodeId node = kNoNode;
        std::unique_ptr<geo::MeshBvh> bvh;
    };
    std::vector<Item> items;
    bool ground = true;
    struct Hit
    {
        glm::dvec3 point{0.0}, normal{0.0, 1.0, 0.0};
        double t = 0.0;
        /** kNoNode for the ground. */
        NodeId node = kNoNode;
    };
    /** The nearest surface along the ray within maxT, its normal facing back along the ray. */
    std::optional<Hit> cast(const glm::dvec3& origin, const glm::dvec3& dir, double maxT) const
    {
        const double len = glm::length(dir);
        if (len < 1e-12)
            return std::nullopt;
        const glm::dvec3 d = dir / len;
        std::optional<Hit> best;
        for (const Item& it : items)
        {
            geo::BvhHit h;
            if (!it.bvh->raycast(origin, d, h, best ? best->t : maxT))
                continue;
            glm::dvec3 a, b, c;
            it.bvh->trianglePoints(h.triangle, a, b, c);
            glm::dvec3 n = glm::cross(b - a, c - a);
            n = glm::length(n) > 1e-12 ? glm::normalize(n) : -d;
            best = Hit{origin + d * h.t, glm::dot(n, d) > 0.0 ? -n : n, h.t, it.node};
        }
        const glm::dvec3 u = up();
        const double denom = glm::dot(d, u);
        if (ground && std::abs(denom) > 1e-9)
        {
            const double t = -glm::dot(origin, u) / denom;
            if (t >= 0.0 && t <= (best ? best->t : maxT))
                best = Hit{origin + d * t, denom > 0.0 ? -u : u, t, kNoNode};
        }
        return best;
    }
};

/** The scene's surfaces in world space; with only, that object's meshes alone (and no ground). */
Surfaces surfacesFor(Graph& g, NodeId only = kNoNode)
{
    Surfaces s;
    s.ground = !only;
    g.evaluate();
    for (NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n || !n->type().isA("mesh"))
            continue;
        if (only && id != only && !isUnder(g, id, only))
            continue;
        if (propFor(g, id) || characterFor(g, id) || skin::find(g, id))
            continue;
        if (const int vis = n->type().attrIndex("visibility"); vis >= 0)
        {
            const PlugValue v = g.value({id, vis, -1});
            if (const bool* on = std::get_if<bool>(&v); on && !*on)
                continue;
        }
        const PlugValue mv = g.value({id, n->type().attrIndex("outMesh"), -1});
        const MeshPtr* mesh = std::get_if<MeshPtr>(&mv);
        if (!mesh || !*mesh || (*mesh)->numFaces() == 0)
            continue;
        geo::Mesh w = **mesh;
        const glm::dmat4 xf = worldOf(g, id);
        for (glm::dvec3& q : w.positions())
            q = glm::dvec3(xf * glm::dvec4(q, 1.0));
        s.items.push_back({id, std::make_unique<geo::MeshBvh>(w)});
    }
    return s;
}

std::string surfaceName(Graph& g, NodeId n) { return n ? g.path(n) : std::string("ground"); }

/** The rotation turning direction a onto b, the shortest way. */
glm::dquat turnOnto(glm::dvec3 a, glm::dvec3 b)
{
    a = glm::normalize(a);
    b = glm::normalize(b);
    const double c = glm::dot(a, b);
    if (c > 1.0 - 1e-12)
        return glm::dquat(1.0, 0.0, 0.0, 0.0);
    if (c < -1.0 + 1e-12)
    {
        glm::dvec3 axis = glm::cross(a, std::abs(a.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0));
        return glm::angleAxis(glm::pi<double>(), glm::normalize(axis));
    }
    return glm::angleAxis(std::acos(std::clamp(c, -1.0, 1.0)), glm::normalize(glm::cross(a, b)));
}

glm::dmat4 rotationMatrix(const glm::dquat& q) { return glm::dmat4(glm::mat3_cast(q)); }
glm::dmat4 translation(const glm::dvec3& v) { return glm::translate(glm::dmat4(1.0), v); }
glm::dvec3 pointOf(const glm::dmat4& m, const glm::dvec3& p) { return glm::dvec3(m * glm::dvec4(p, 1.0)); }

/**
 * Keys the root's translate and rotate at t so its world matrix is `world`, like keyRootAt but straight onto the curves
 * when no animation layer is involved (a throw keys every frame). Rotation is kept continuous with the key before.
 */
void keyRootFast(Graph& g, NodeId root, const glm::dmat4& world, double t, std::optional<glm::dvec3>& last)
{
    if (anim::keyLayer(g, {root, Transform::kTranslate, -1}) != kNoNode || anim::keyLayer(g, {root, Transform::kRotate, -1}) != kNoNode)
    {
        keyRootAt(g, root, world, t);
        return;
    }
    at(g, t);
    setWorldRotation(g, root, glm::dmat3(rigidOf(world)));
    g.evaluate();
    setWorldPosition(g, root, glm::dvec3(world[3]));
    g.evaluate();
    const glm::dvec3 tr = vecOf(g, root, Transform::kTranslate);
    glm::dvec3 ro = vecOf(g, root, Transform::kRotate);
    if (last)
        for (int c = 0; c < 3; ++c)
            ro[c] += 360.0 * std::round(((*last)[c] - ro[c]) / 360.0);
    last = ro;
    for (int c = 0; c < 3; ++c)
        anim::setKey(g, {root, Transform::kTranslate, c}, t, tr[c]);
    if (anim::keysAsRotation(g, {root, Transform::kRotate, -1}))
        anim::setRotationKey(g, {root, Transform::kRotate, -1}, t, ro);
    else
        for (int c = 0; c < 3; ++c)
            anim::setKey(g, {root, Transform::kRotate, c}, t, ro[c]);
}

/** Removes the root's translate and rotate keys in [t0, t1]. */
void clearRootKeys(Graph& g, NodeId root, double t0, double t1)
{
    if (t1 < t0)
        return;
    for (int attr : {Transform::kTranslate, Transform::kRotate})
        for (int c = -1; c < 3; ++c)
            if (const NodeId curve = anim::curveFor(g, {root, attr, c}))
                anim::removeKeys(g, curve, t0, t1);
}

/** The time of the root's next space key after t, or a long way off when none. */
double nextSwitchAfter(Graph& g, NodeId root, double t)
{
    const double half = 0.5 / fps();
    if (const NodeId curve = anim::curveFor(g, {root, Transform::kSpace, -1}))
        for (double k : anim::curveKeyTimes(*g.node(curve)))
            if (k > t + half)
            {
                // The switch's hold key a frame before belongs to the switch.
                const double before = k - 1.0 / fps();
                return before > t + half ? before : k;
            }
    return 1e9;
}

/** Switches the prop's root into a space at t keeping its pose, and clears the keys of whatever it did before up to its next switch. */
void switchFresh(Graph& g, const Prop& p, int space, double t)
{
    at(g, t);
    sp::SwitchOptions so;
    so.time = t;
    so.key = true;
    sp::switchSpace(g, p.root, space, so);
    const double half = 0.5 / fps();
    clearRootKeys(g, p.root, t + half, nextSwitchAfter(g, p.root, t) - half);
}

/** A hand holding the prop at a time, and the grip point it holds. */
struct Holding
{
    hand::Hand h;
    NodeId point = kNoNode;
};

/** The grip point the hand took the prop by when it last did, else the main grip. */
NodeId heldPoint(Graph& g, const Prop& p, double t, NodeId joint)
{
    NodeId out = kNoNode;
    const std::string jp = g.path(joint);
    for (const json& e : eventsOf(g, p))
    {
        if (e.value("time", 0.0) > t + 1e-9)
            break;
        for (const char* side : {"hand", "offHand"})
            if (e.value(side, std::string()) == jp)
                if (Node* n = named(g, e.value(std::string(side) == "hand" ? "point" : "offPoint", std::string())))
                    out = n->id();
    }
    return out ? out : p.point(g, Role::Main);
}

std::vector<Holding> holdingAt(Graph& g, NodeId grapple, const Prop& p, double t)
{
    std::vector<Holding> out;
    auto add = [&](NodeId joint, NodeId point) {
        for (const Holding& h : out)
            if (h.h.joint == joint)
                return;
        if (const NodeId hn = hand::handFor(g, joint))
            if (std::optional<hand::Hand> h = hand::load(g, hn); h && h->joint == joint)
                out.push_back({*h, point});
    };
    at(g, t);
    if (const NodeId j = holderNow(g, p))
        add(j, heldPoint(g, p, t, j));
    for (const PropContact& c : propContacts(g, grapple, p))
        if (numOf(g, c.node, "weight") > 0.5)
            if (Node* s = named(g, c.source))
                if (Node* f = named(g, c.frame))
                    add(s->id(), f->id());
    return out;
}

/**
 * The hands holding the prop at t carry it while it moves over [t, t + carry] (each held on its grip in the world, so
 * the arm follows the prop) and let go over the release after; with no carry they let go at once (a throw). The
 * fingers open as they let go.
 */
void carryAndLetGo(Graph& g, NodeId grapple, const Prop& p, const std::vector<Holding>& holding, double t, double carry, double release, bool fingers)
{
    if (!grapple)
        return;
    const double frame = 1.0 / fps();
    std::set<NodeId> pins;
    if (carry > 0.0)
        for (const Holding& h : holding)
        {
            const NodeId pin = contactFor(g, grapple, p, h.h, kNoNode, h.point);
            pins.insert(pin);
            // Where the hand holds it now, the hold's own grip, so taking over from the hold moves nothing.
            g.evaluate();
            at(g, t);
            setPlug(g, pin, "gripOffset", glm::inverse(rigidOf(worldOf(g, h.h.joint))) * rigidOf(worldOf(g, h.point)));
            if (t - frame >= anim::playback().minTime)
                keyWeight(g, pin, t - frame, weightAt(g, pin, t - frame));
            keyWeight(g, pin, t, 1.0);
            keyWeight(g, pin, t + carry, 1.0);
            keyWeight(g, pin, t + carry + std::max(release, frame), 0.0);
        }
    for (const PropContact& c : propContacts(g, grapple, p))
        if (!pins.count(c.node) && weightAt(g, c.node, t) > 1e-6)
        {
            keyWeight(g, c.node, t, weightAt(g, c.node, t));
            keyWeight(g, c.node, t + frame, 0.0);
        }
    if (fingers)
        for (const Holding& h : holding)
            poseFingers(g, h.h, t + carry, t + carry + std::max(release, frame), "relaxed", false, false, nullptr);
}

/** The character of a handoff when there is one (the flag, the holder, the only one); none is allowed. */
NodeId characterIfAny(Graph& g, const Args& a, const Prop& p, const char* cmd)
{
    if (!a.has("character") && characters(g).empty())
        return kNoNode;
    try
    {
        return characterArg(g, a, p, cmd);
    }
    catch (const CommandError&)
    {
        if (a.has("character"))
            throw;
        return kNoNode;
    }
}

/** Right, up and forward for a handoff: the character's when there is one, else the world's. */
glm::dmat3 frameFor(Graph& g, NodeId character)
{
    if (character)
        return characterFrame(g, character);
    const lila::math::AxisBasis& b = lila::math::axisBasis();
    return glm::dmat3(b.right, b.up, b.forward);
}

const FlagDef kReleaseFlag{"release", "rl", ArgType::Float, 0.25, "Seconds the hands take to let go once it is down.", {}, {}, 0.0};

// puppeteerPropPlant ---------------------------------------------------------------------------------------------------

json plantCmd(const Args& a)
{
    Graph& g = Graph::current();
    const char* cmd = "puppeteerPropPlant";
    Prop p = propArg(g, a, cmd);
    const KeepTime keep(g);
    const double t = a.has("time") ? a.get<double>("time") : g.time();
    const double blend = std::max(0.0, a.get<double>("blend", 0.4));
    const NodeId character = characterIfAny(g, a, p, cmd);
    const NodeId grapple = character ? propGrapple(g, character, true) : propGrapple(g, kNoNode, false);
    const std::vector<Holding> holding = holdingAt(g, grapple, p, t);
    const PropShape sh = shapeIn(g, p);
    if (!sh.measured)
        throw CommandError(std::string(cmd) + ": " + g.node(p.root)->name() + " has no mesh to stand on its end");
    at(g, t);
    const glm::dmat4 m0 = rigidOf(worldOf(g, p.root));

    // The two ends along its length; tip is the end its tip or muzzle point is at.
    const glm::dvec3 endHi = sh.at({1.0, 0.0, 0.0}), endLo = sh.at({-1.0, 0.0, 0.0});
    glm::dvec3 tipEnd = endHi;
    if (const NodeId tp = p.point(g, Role::Tip) ? p.point(g, Role::Tip) : p.point(g, Role::Muzzle))
    {
        const glm::dvec3 tl(pointLocal(g, p, tp)[3]);
        tipEnd = glm::length(tl - endHi) < glm::length(tl - endLo) ? endHi : endLo;
    }
    const glm::dvec3 baseEnd = tipEnd == endHi ? endLo : endHi;
    const std::string endName = a.get<std::string>("standOn", "tip");
    glm::dvec3 planted = tipEnd, other = baseEnd;
    if (endName == "base")
        std::swap(planted, other);
    else if (endName != "tip")
    {
        const NodeId pt = gripPoint(g, p, endName, Role::Main, cmd);
        planted = glm::dvec3(pointLocal(g, p, pt)[3]);
        other = glm::length(planted - endHi) > glm::length(planted - endLo) ? endHi : endLo;
    }
    const glm::dvec3 plantedW = pointOf(m0, planted), otherW = pointOf(m0, other);

    // Where: on the surface below the planted end, ahead of the character by `ahead`, or below position.
    const glm::dmat3 cf = frameFor(g, character);
    const glm::dvec3 u = up();
    glm::dvec3 probe = a.has("position") ? jsonVec(a.flags.at("position")) : plantedW + cf[2] * a.get<double>("ahead", 25.0);
    const Surfaces surfaces = surfacesFor(g);
    const std::optional<Surfaces::Hit> hit = surfaces.cast(probe + u * 150.0, -u, 2000.0);
    if (!hit)
        throw CommandError(std::format("{}: nothing below {:.0f}, {:.0f}, {:.0f} to plant it on", cmd, probe.x, probe.y, probe.z));
    glm::dvec3 n = glm::dot(hit->normal, u) < 0.0 ? -hit->normal : hit->normal;
    // Leaning back toward where it was held by default (the handle toward the hand), or toward the given direction.
    glm::dvec3 toward = a.has("toward") ? jsonVec(a.flags.at("toward")) : otherW - hit->point;
    toward -= n * glm::dot(toward, n);
    if (glm::length(toward) < 1e-6)
        toward = -cf[2] - n * glm::dot(-cf[2], n);
    toward = glm::length(toward) > 1e-9 ? glm::normalize(toward) : glm::dvec3(cf[0]);
    const double lean = glm::radians(std::clamp(a.get<double>("lean", 12.0), 0.0, 89.0));
    const glm::dvec3 along = std::cos(lean) * n + std::sin(lean) * toward;
    const glm::dquat q = turnOnto(otherW - plantedW, along);
    const glm::dmat4 endPivot = translation(hit->point) * rotationMatrix(q) * glm::dmat4(glm::dmat3(m0));

    switchFresh(g, p, worldSpace(g, p.root), t);
    settleAbout(g, p, translation(planted), kNoNode, endPivot, t, blend);
    carryAndLetGo(g, grapple, p, holding, t, blend, std::max(0.0, a.get<double>("release", 0.25)), a.get<bool>("fingers", true));
    recordEvent(g, p, {{"time", t}, {"kind", "plant"}, {"end", endName}, {"surface", surfaceName(g, hit->node)}, {"lean", glm::degrees(lean)}});
    return finish(g, p, grapple, a,
                  {{"surface", surfaceName(g, hit->node)}, {"at", vecJson(hit->point)}, {"lean", glm::degrees(lean)}, {"end", endName}, {"settled", t + blend}});
}

// puppeteerPropThrow / puppeteerPropArc ---------------------------------------------------------------------------------

/** A throw's arc as stored on its node. */
struct Arc
{
    NodeId node = kNoNode;
    double start = 0.0, end = 0.0, until = 3.0, spin = 0.0, gravity = 981.0;
    glm::dvec3 launch{0.0}, velocity{0.0}, spinAxis{1.0, 0.0, 0.0}, centre{0.0};
    glm::dmat4 startWorld{1.0};
    bool physics = false;
};

Arc readArc(Graph& g, NodeId node)
{
    Arc r;
    r.node = node;
    r.start = numOf(g, node, "start");
    r.end = numOf(g, node, "end");
    r.until = numOf(g, node, "until");
    r.spin = numOf(g, node, "spin");
    r.gravity = numOf(g, node, "gravity");
    r.launch = vecOf(g, node, attrOf(*g.node(node), "launch"));
    r.velocity = vecOf(g, node, attrOf(*g.node(node), "velocity"));
    r.spinAxis = vecOf(g, node, attrOf(*g.node(node), "spinAxis"));
    r.centre = vecOf(g, node, attrOf(*g.node(node), "centre"));
    r.startWorld = std::get<glm::dmat4>(g.value(plugOf(*g.node(node), "startMatrix")));
    r.physics = boolOf(g, node, "physics");
    return r;
}

/** The root's world matrix on a keyed arc at tau seconds after the launch. */
glm::dmat4 ballistic(const Arc& r, double tau)
{
    const glm::dvec3 gv = -up() * r.gravity;
    const glm::dvec3 c = r.launch + r.velocity * tau + 0.5 * gv * tau * tau;
    const glm::dvec3 axis = glm::length(r.spinAxis) > 1e-9 ? glm::normalize(r.spinAxis) : glm::dvec3(1, 0, 0);
    const glm::dquat spin = glm::angleAxis(glm::radians(r.spin) * tau, axis);
    return translation(c) * rotationMatrix(spin) * translation(-r.launch) * r.startWorld;
}

/** The launch velocity that puts the prop's centre at `through` tau seconds after the launch. */
glm::dvec3 velocityThrough(const Arc& r, const glm::dvec3& through, double tau)
{
    const glm::dvec3 gv = -up() * r.gravity;
    return (through - r.launch - 0.5 * gv * tau * tau) / std::max(tau, 1e-3);
}

/** The pose a landed prop settles into: its thinnest side down on the surface, its length kept turned as it was. */
glm::dmat4 restingOn(const PropShape& sh, const glm::dmat4& world, const glm::dvec3& point, const glm::dvec3& normal)
{
    const glm::dvec3 thin = glm::dmat3(world) * sh.axes[2];
    const glm::dquat q = turnOnto(glm::dot(thin, normal) >= 0.0 ? thin : -thin, normal);
    const glm::dmat4 turned = rotationMatrix(q) * glm::dmat4(glm::dmat3(world));
    const glm::dvec3 centre = point + normal * (0.5 * sh.extent()[2] + 0.05);
    return translation(centre) * turned * translation(-sh.middle());
}

/** Samples the arc frame by frame, from the launch to a landing, the arc's end, its time limit or the playback end. */
struct ArcSamples
{
    std::vector<std::pair<double, glm::dmat4>> poses;
    std::optional<Surfaces::Hit> landed;
    double landTime = 0.0;
    std::string how;
};

ArcSamples sampleArc(Graph& g, const Prop& p, const Arc& r, double stopAt)
{
    ArcSamples out;
    const double dt = 1.0 / fps();
    const PropShape sh = shapeIn(g, p);
    const Surfaces surfaces = surfacesFor(g);
    const double last = std::min({r.start + r.until, stopAt, anim::playback().maxTime});
    const long long frames = std::max<long long>(0, std::llround((last - r.start) * fps()));
    const double settle = 0.2;
    auto land = [&](double time, const glm::dmat4& contact, const Surfaces::Hit& h) {
        out.landed = h;
        out.landTime = time;
        // Ease from the pose it met the surface in to lying on it, about its middle.
        const glm::dmat4 rest = restingOn(sh, contact, h.point, glm::dot(h.normal, up()) < 0.0 ? -h.normal : h.normal);
        const glm::dquat q0 = glm::quat_cast(glm::dmat3(contact)), q1 = glm::quat_cast(glm::dmat3(rest));
        const glm::dvec3 c0 = pointOf(contact, sh.middle()), c1 = pointOf(rest, sh.middle());
        const long long n = std::max<long long>(1, std::llround(settle * fps()));
        for (long long f = 1; f <= n && time + f * dt <= last + 1e-9; ++f)
        {
            const double u = double(f) / double(n), e = u * u * (3.0 - 2.0 * u);
            const glm::dmat4 m = translation(glm::mix(c0, c1, e)) * rotationMatrix(glm::slerp(q0, q1, e)) * translation(-sh.middle());
            out.poses.push_back({time + f * dt, m});
        }
    };
    if (!r.physics)
    {
        const std::array<glm::dvec3, 8> corners = sh.corners();
        glm::dmat4 prev = r.startWorld;
        for (long long f = 1; f <= frames; ++f)
        {
            const double time = r.start + double(f) * dt;
            glm::dmat4 m = ballistic(r, time - r.start);
            // Landing: the first corner whose path this frame meets a surface.
            std::optional<Surfaces::Hit> best;
            glm::dvec3 bestFrom(0.0), bestTo(0.0);
            if (sh.measured)
                for (const glm::dvec3& c : corners)
                {
                    const glm::dvec3 a0 = pointOf(prev, c), a1 = pointOf(m, c);
                    if (std::optional<Surfaces::Hit> h = surfaces.cast(a0, a1 - a0, glm::length(a1 - a0)); h && (!best || h->t < best->t))
                    {
                        best = h;
                        bestFrom = a0;
                        bestTo = a1;
                    }
                }
            if (best)
            {
                // Back along this frame's move until that corner touches.
                const glm::dmat4 contact = translation(best->point - bestTo) * m;
                out.poses.push_back({time, contact});
                land(time, contact, *best);
                out.how = "keyed";
                return out;
            }
            out.poses.push_back({time, m});
            prev = m;
        }
        out.how = "keyed";
        return out;
    }

    // Jolt: the prop as a convex hull, the scene's meshes and the ground as still colliders.
    using namespace lila::physics;
    initJolt();
    lila::physics::Settings set;
    set.gravity = -up() * r.gravity;
    set.ground = false;
    RigidScene scene(set);
    const glm::dquat r0 = glm::quat_cast(glm::dmat3(r.startWorld));
    std::vector<glm::dvec3> hull;
    {
        const std::vector<glm::dvec3> pts = meshPointsIn(g, p);
        const size_t stride = std::max<size_t>(1, pts.size() / 2000);
        const glm::dmat3 back = glm::transpose(glm::dmat3(glm::mat3_cast(r0)));
        for (size_t i = 0; i < pts.size(); i += stride)
            hull.push_back(back * (pointOf(r.startWorld, pts[i]) - r.launch));
    }
    if (hull.size() < 4)
        throw CommandError("puppeteerPropThrow: " + g.node(p.root)->name() + " has no mesh to simulate");
    BodyDesc body;
    body.shape.kind = lila::physics::Shape::Kind::Hull;
    body.shape.points = std::make_shared<const std::vector<glm::dvec3>>(std::move(hull));
    body.position = r.launch;
    body.rotation = r0;
    body.velocity = r.velocity;
    body.angularVelocity = (glm::length(r.spinAxis) > 1e-9 ? glm::normalize(r.spinAxis) : glm::dvec3(1, 0, 0)) * glm::radians(r.spin);
    body.bounce = 0.2;
    body.friction = 0.6;
    body.continuous = true;
    body.user = 0;
    scene.add(body);
    for (const Surfaces::Item& it : surfaces.items)
    {
        auto soup = std::make_shared<std::vector<glm::dvec3>>();
        for (geo::Index tri = 0; tri < it.bvh->triangleCount(); ++tri)
        {
            glm::dvec3 a, b, c;
            it.bvh->trianglePoints(tri, a, b, c);
            soup->insert(soup->end(), {a, b, c});
        }
        BodyDesc s;
        s.shape.kind = lila::physics::Shape::Kind::Mesh;
        s.shape.points = soup;
        s.role = lila::physics::Role::Static;
        s.user = int64_t(it.node);
        scene.add(s);
    }
    {
        BodyDesc gd;
        gd.shape.kind = lila::physics::Shape::Kind::Box;
        gd.shape.halfExtents = glm::dvec3(1e5, 50.0, 1e5);
        gd.rotation = turnOnto({0, 1, 0}, up());
        gd.position = -up() * 50.0;
        gd.role = lila::physics::Role::Static;
        gd.user = -2;
        scene.add(gd);
    }
    const glm::dmat4 toBody = glm::inverse(translation(r.launch) * rotationMatrix(r0));
    int quiet = 0;
    for (long long f = 1; f <= frames; ++f)
    {
        for (int k = 0; k < 4; ++k)
            scene.step(dt / 4.0);
        const Pose pose = scene.pose(0);
        out.poses.push_back({r.start + double(f) * dt, translation(pose.position) * rotationMatrix(pose.rotation) * toBody * r.startWorld});
        if (!out.landed)
            for (const Contact& c : scene.contacts())
                if (c.a == 0 || c.b == 0)
                {
                    Surfaces::Hit h;
                    h.point = c.point;
                    h.normal = c.normal;
                    const int other = c.a == 0 ? c.b : c.a;
                    const int64_t user = other >= 0 && size_t(other) < scene.size() ? scene.desc(other).user : -2;
                    h.node = user >= 0 ? NodeId(user) : kNoNode;
                    out.landed = h;
                    out.landTime = r.start + double(f) * dt;
                    break;
                }
        // At rest: a few quiet frames end it.
        quiet = scene.asleep(0) || (glm::length(scene.velocity(0)) < 1.0 && glm::length(scene.angularVelocity(0)) < 0.02) ? quiet + 1 : 0;
        if (quiet >= 3)
            break;
    }
    out.how = "physics";
    return out;
}

/** The arc's samples written as the prop's keys from its launch, replacing what it did up to its next switch. */
json keyArc(Graph& g, const Prop& p, NodeId arcNode)
{
    const Arc r = readArc(g, arcNode);
    const double half = 0.5 / fps();
    const double next = nextSwitchAfter(g, p.root, r.start);
    const ArcSamples s = sampleArc(g, p, r, next - 1.0 / fps());
    clearRootKeys(g, p.root, r.start + half, next - half);
    std::optional<glm::dvec3> last = vecOf(g, p.root, Transform::kRotate);
    json path = json::array();
    const glm::dvec3 middle = r.centre;
    path.push_back({r.start, r.launch.x, r.launch.y, r.launch.z});
    for (const auto& [time, m] : s.poses)
    {
        keyRootFast(g, p.root, m, time, last);
        const glm::dvec3 c = pointOf(m, middle);
        path.push_back({time, c.x, c.y, c.z});
    }
    const double end = s.poses.empty() ? r.start : s.poses.back().first;
    setPlug(g, arcNode, "end", end);
    setPlug(g, arcNode, "landed", bool(s.landed));
    setPlug(g, arcNode, "path", path.dump());
    g.evaluate();
    json out = {{"arc", g.path(arcNode)}, {"frames", s.poses.size()}, {"end", end}, {"landed", bool(s.landed)}, {"how", s.how}};
    if (s.landed)
    {
        out["landTime"] = s.landTime;
        out["surface"] = surfaceName(g, s.landed->node);
    }
    return out;
}

json throwCmd(const Args& a)
{
    Graph& g = Graph::current();
    const char* cmd = "puppeteerPropThrow";
    Prop p = propArg(g, a, cmd);
    const KeepTime keep(g);
    const double t = a.has("time") ? a.get<double>("time") : g.time();
    const double dt = 1.0 / fps();
    const NodeId character = characterIfAny(g, a, p, cmd);
    const NodeId grapple = character ? propGrapple(g, character, true) : propGrapple(g, kNoNode, false);
    const std::vector<Holding> holding = holdingAt(g, grapple, p, t);
    const PropShape sh = shapeIn(g, p);
    at(g, t);
    const glm::dmat4 m0 = rigidOf(worldOf(g, p.root));
    const glm::dvec3 c0 = pointOf(m0, sh.middle());
    const glm::dmat3 cf = frameFor(g, character);

    // Launch: given, else from how the hand moved it into the release, else a toss forward (said so).
    glm::dvec3 v(0.0), w(0.0);
    std::string from = "velocity";
    if (t - dt >= anim::playback().minTime - 1e-9)
    {
        at(g, t - dt);
        const glm::dmat4 mp = rigidOf(worldOf(g, p.root));
        v = (c0 - pointOf(mp, sh.middle())) / dt;
        const glm::dquat dq = glm::quat_cast(glm::dmat3(m0)) * glm::inverse(glm::quat_cast(glm::dmat3(mp)));
        const double angle = 2.0 * std::acos(std::clamp(std::abs(dq.w), 0.0, 1.0));
        if (angle > 1e-9)
            w = glm::normalize(glm::dvec3(dq.x, dq.y, dq.z) * (dq.w < 0.0 ? -1.0 : 1.0)) * (angle / dt);
        at(g, t);
    }
    if (a.has("velocity"))
        v = jsonVec(a.flags.at("velocity"));
    else if (a.has("speed") || a.has("angle") || a.has("direction"))
    {
        glm::dvec3 dir = a.has("direction") ? jsonVec(a.flags.at("direction")) : cf[2];
        dir -= up() * glm::dot(dir, up());
        dir = glm::length(dir) > 1e-9 ? glm::normalize(dir) : glm::dvec3(cf[2]);
        const double elev = glm::radians(a.get<double>("angle", 35.0));
        v = (dir * std::cos(elev) + up() * std::sin(elev)) * a.get<double>("speed", 500.0);
        from = "speed";
    }
    else if (glm::length(v) < 100.0)
    {
        const double speed = 500.0, elev = glm::radians(35.0);
        log::write(LogLevel::Warning, std::format("{}: {} barely moves at the release ({:.0f} cm/s); thrown forward at {:.0f} cm/s, 35 deg up (give speed, "
                                                  "angle and direction, or animate the throw into the release)",
                                                  cmd, g.node(p.root)->name(), glm::length(v), speed));
        v = (cf[2] * std::cos(elev) + up() * std::sin(elev)) * speed;
        from = "default";
    }
    else
        from = "hand";
    double spin = a.get<double>("spin", 0.0);
    glm::dvec3 spinAxis = a.has("spinAxis") ? jsonVec(a.flags.at("spinAxis")) : glm::cross(v, up());
    if (!a.has("spin") && from == "hand" && glm::length(w) > glm::radians(30.0))
    {
        spin = glm::degrees(glm::length(w));
        if (!a.has("spinAxis"))
            spinAxis = w;
    }
    if (glm::length(spinAxis) < 1e-9)
        spinAxis = cf[0];

    // The arc lives under the prop; a throw again at the same time replaces it.
    NodeId arcNode = kNoNode;
    for (NodeId c : g.children(p.node))
        if (Node* n = g.node(c); n && n->type().isA(kArcType) && std::abs(numOf(g, c, "start") - t) < 0.5 * dt)
            arcNode = c;
    if (!arcNode)
        arcNode = g.createNode(*types::find(kArcType), g.node(p.root)->name() + "_throw", p.node)->id();
    setPlug(g, arcNode, "start", t);
    setPlug(g, arcNode, "until", std::max(dt, a.get<double>("until", 3.0)));
    setPlug(g, arcNode, "launch", c0);
    setPlug(g, arcNode, "velocity", v);
    setPlug(g, arcNode, "spin", spin);
    setPlug(g, arcNode, "spinAxis", glm::normalize(spinAxis));
    setPlug(g, arcNode, "gravity", std::max(0.0, a.get<double>("gravity", 981.0)));
    setPlug(g, arcNode, "physics", a.get<bool>("physics", false));
    setPlug(g, arcNode, "startMatrix", m0);
    setPlug(g, arcNode, "centre", sh.middle());

    switchFresh(g, p, worldSpace(g, p.root), t);
    carryAndLetGo(g, grapple, p, holding, t, 0.0, std::max(0.0, a.get<double>("release", 0.15)), a.get<bool>("fingers", true));
    json arc = keyArc(g, p, arcNode);
    recordEvent(g, p, {{"time", t}, {"kind", "throw"}, {"arc", g.path(arcNode)}, {"physics", a.get<bool>("physics", false)}});
    arc["velocity"] = vecJson(v);
    arc["speed"] = glm::length(v);
    arc["spin"] = spin;
    arc["launchFrom"] = from;
    return finish(g, p, grapple, a, arc);
}

/**
 * Takes a catch out: its event, the hold's space keys, the prop's keys while it settled into the hand, the reaching
 * contact's weight keys and the catching hand's finger keys over the reach and settle.
 */
void uncatch(Graph& g, const Prop& p, const json& ev)
{
    const double t = ev.value("time", 0.0), half = 0.5 / fps();
    const json by = ev.value("catchBy", json::object());
    const double reach = by.value("reach", 0.3), blend = by.value("blend", 0.2);
    const double from = t - reach - half, to = t + blend + 1.0 / fps() + half;
    if (const NodeId curve = anim::curveFor(g, {p.root, Transform::kSpace, -1}))
        anim::removeKeys(g, curve, t - 1.0 / fps() - half, t + half);
    clearRootKeys(g, p.root, t - half, to);
    Node* handJoint = named(g, ev.value("hand", std::string()));
    if (const NodeId grapple = propGrapple(g, kNoNode, false); grapple && handJoint)
        for (const PropContact& c : propContacts(g, grapple, p))
            if (Node* src = named(g, c.source); src && src->id() == handJoint->id())
                if (const NodeId curve = anim::curveFor(g, plugOf(*g.node(c.node), "weight")))
                    anim::removeKeys(g, curve, from, to);
    if (handJoint)
        if (const NodeId hn = hand::handFor(g, handJoint->id()))
            if (const std::optional<hand::Hand> h = hand::load(g, hn))
                for (const hand::Finger& f : h->fingers)
                {
                    std::vector<NodeId> joints = f.joints;
                    if (f.hose)
                        joints.push_back(f.hose);
                    for (NodeId j : joints)
                        for (const anim::AnimatedPlug& ap : anim::animatedPlugs(g, j))
                            anim::removeKeys(g, ap.curve, from, to);
                }
    json kept = json::array();
    for (const json& e : eventsOf(g, p))
        if (std::abs(e.value("time", 0.0) - t) > half || e.value("kind", std::string()) != "catch")
            kept.push_back(e);
    setPlug(g, p.node, "events", kept.dump());
    g.evaluate();
}

json arcCmd(const Args& a)
{
    Graph& g = Graph::current();
    const char* cmd = "puppeteerPropArc";
    NodeId arcNode = kNoNode;
    for (const std::string& s : objectArgs(a))
        if (Node& n = nodeNamed(g, s, cmd); n.type().isA(kArcType))
            arcNode = n.id();
    if (!arcNode && objectArgs(a).empty())
        for (NodeId s : g.selection())
            if (Node* n = g.node(s); n && n->type().isA(kArcType))
                arcNode = s;
    if (!arcNode)
        throw CommandError(std::string(cmd) + ": give a throw's arc (puppeteerPropThrow makes one), or select it");
    Prop p = *load(g, propFor(g, arcNode));
    const KeepTime keep(g);
    Arc r = readArc(g, arcNode);
    if (a.has("through"))
    {
        const double tau = a.has("at") ? a.get<double>("at") - r.start : std::max(r.end - r.start, 1.0 / fps());
        if (tau <= 0.0)
            throw CommandError(std::string(cmd) + ": at is the time on the arc the prop passes through, after its launch");
        setPlug(g, arcNode, "velocity", velocityThrough(r, jsonVec(a.flags.at("through")), tau));
    }
    if (a.has("velocity"))
        setPlug(g, arcNode, "velocity", jsonVec(a.flags.at("velocity")));
    if (a.has("spin"))
        setPlug(g, arcNode, "spin", a.get<double>("spin"));
    if (a.has("spinAxis"))
        setPlug(g, arcNode, "spinAxis", jsonVec(a.flags.at("spinAxis")));
    if (a.has("physics"))
        setPlug(g, arcNode, "physics", a.get<bool>("physics"));
    if (a.has("until"))
        setPlug(g, arcNode, "until", std::max(1.0 / fps(), a.get<double>("until")));
    if (a.has("gravity"))
        setPlug(g, arcNode, "gravity", std::max(0.0, a.get<double>("gravity")));
    // A catch timed to the old arc is taken out and made again on the new one, the same way it was asked for.
    json recatch;
    for (const json& e : eventsOf(g, p))
        if (e.value("kind", std::string()) == "catch" && e.value("time", 0.0) > r.start && e.value("arc", std::string()) == g.path(arcNode))
        {
            recatch = e;
            break;
        }
    if (recatch.is_object())
        uncatch(g, p, recatch);
    json out = keyArc(g, p, arcNode);
    out["velocity"] = vecJson(vecOf(g, arcNode, attrOf(*g.node(arcNode), "velocity")));
    if (recatch.is_object())
    {
        const json by = recatch.value("catchBy", json::object());
        json call = {{"args", {g.path(p.root)}}, {"time", r.start + 0.5 / fps()}, {"solve", false}};
        for (const char* k : {"character", "hand", "point", "reach", "blend", "fingers"})
            if (by.contains(k))
                call[k] = by[k];
        if (!by.contains("character") && recatch.contains("character"))
            call["character"] = recatch["character"];
        const json caught = ops::execute("puppeteerPropCatch", call);
        out["caughtAt"] = caught.value("time", 0.0);
        out["caughtBefore"] = recatch.value("time", 0.0);
    }
    const NodeId grapple = propGrapple(g, kNoNode, false);
    return finish(g, p, grapple, a, out);
}

// puppeteerPropCatch ---------------------------------------------------------------------------------------------------

json catchCmd(const Args& a)
{
    Graph& g = Graph::current();
    const char* cmd = "puppeteerPropCatch";
    Prop p = propArg(g, a, cmd);
    const KeepTime keep(g);
    const double now = a.has("time") ? a.get<double>("time") : g.time();
    // The throw: the last one launched by now, else the first.
    json thrown;
    for (const json& e : eventsOf(g, p))
        if (e.value("kind", std::string()) == "throw" && (thrown.is_null() || e.value("time", 0.0) <= now + 1e-9))
            thrown = e;
    Node* arcN = thrown.is_null() ? nullptr : named(g, thrown.value("arc", std::string()));
    if (!arcN)
        throw CommandError(std::string(cmd) + ": " + g.node(p.root)->name() + " isn't thrown (puppeteerPropThrow)");
    const Arc r = readArc(g, arcN->id());
    // The catcher: given, else whoever threw it.
    NodeId catcher = kNoNode;
    if (a.has("character"))
        catcher = characterArg(g, a, p, cmd);
    else
    {
        Args before = a;
        before.flags["time"] = r.start;
        catcher = characterArg(g, before, p, cmd);
    }
    const std::string side = a.get<std::string>("hand", "either");
    std::vector<HandRef> hands;
    for (const bool right : {true, false})
        if (side == "either" || side == (right ? "right" : "left"))
            if (std::optional<HandRef> h = handOf(g, catcher, right))
                hands.push_back(*h);
    if (hands.empty())
        throw CommandError(std::format("{}: {} has no {} hand the hand rig could find", cmd, g.node(catcher)->name(), side == "either" ? "" : side));
    const NodeId point = gripPoint(g, p, a.get<std::string>("point", ""), Role::Main, cmd);
    if (!point)
        throw CommandError(std::string(cmd) + ": " + g.node(p.root)->name() + " has no grip to catch it by");
    const double reach = std::max(0.0, a.get<double>("reach", 0.3));

    // When: the frame on the arc its grip comes nearest the hand, among those the arm can reach.
    struct Best
    {
        double time = -1.0, gap = 1e300, short_ = 1e300;
        size_t hand = 0;
    } best, nearest;
    const double dt = 1.0 / fps();
    const long long frames = std::llround((r.end - r.start) * fps());
    for (long long f = 2; f <= frames; ++f)
    {
        const double time = r.start + double(f) * dt;
        at(g, time);
        const glm::dvec3 grip(worldOf(g, point)[3]);
        for (size_t i = 0; i < hands.size(); ++i)
        {
            const NodeId lower = g.parent(hands[i].h.joint), upper = lower ? g.parent(lower) : kNoNode;
            if (!upper)
                continue;
            const glm::dvec3 hj(worldOf(g, hands[i].h.joint)[3]), lj(worldOf(g, lower)[3]), uj(worldOf(g, upper)[3]);
            const double arm = glm::length(hj - lj) + glm::length(lj - uj);
            const double gap = glm::length(grip - hj), shortBy = glm::length(grip - uj) - 0.95 * arm;
            if (shortBy <= 0.0 && gap < best.gap)
                best = {time, gap, shortBy, i};
            if (shortBy < nearest.short_)
                nearest = {time, gap, shortBy, i};
        }
    }
    if (best.time < 0.0)
    {
        if (nearest.time < 0.0)
            throw CommandError(std::string(cmd) + ": the arc is too short to catch");
        best = nearest;
        log::write(LogLevel::Warning, std::format("{}: the arc never comes within {}'s reach; caught at frame {:g}, {:.0f} cm short, the arm stretching as far as it goes",
                                                  cmd, g.node(catcher)->name(), best.time * fps(), best.short_));
    }
    const HandRef& h = hands[best.hand];
    // The arc stops where the hand takes it; the hold reaches for it there.
    clearRootKeys(g, p.root, best.time + 0.5 * dt, r.end + 0.5 * dt);
    setPlug(g, r.node, "end", best.time);
    json call = {{"args", {g.path(p.root)}},
                 {"character", g.path(catcher)},
                 {"hand", h.right ? "right" : "left"},
                 {"point", g.node(point)->name()},
                 {"time", best.time},
                 {"reach", std::min(reach, best.time - r.start - dt)},
                 {"blend", std::max(0.0, a.get<double>("blend", 0.2))},
                 {"hands", 1},
                 {"fingers", a.get<bool>("fingers", true)},
                 {"event", "catch"},
                 {"solve", false}};
    ops::execute("puppeteerPropHold", call);
    json ev;
    for (const json& e : eventsOf(g, p))
        if (std::abs(e.value("time", 0.0) - best.time) < 0.5 * dt)
            ev = e;
    if (ev.is_object())
    {
        ev["arc"] = g.path(r.node);
        ev["catchBy"] = {{"character", g.path(catcher)}, {"hand", side}, {"point", g.node(point)->name()}, {"reach", reach},
                         {"blend", std::max(0.0, a.get<double>("blend", 0.2))}, {"fingers", a.get<bool>("fingers", true)}};
        recordEvent(g, p, ev);
    }
    return finish(g, p, propGrapple(g, catcher, true), a,
                  {{"time", best.time}, {"frame", best.time * fps()}, {"hand", g.path(h.h.joint)}, {"gap", best.gap}, {"character", g.path(catcher)},
                   {"reachable", best.short_ <= 0.0}});
}

// puppeteerPropPlace ---------------------------------------------------------------------------------------------------

json placeCmd(const Args& a)
{
    Graph& g = Graph::current();
    const char* cmd = "puppeteerPropPlace";
    Prop p = propArg(g, a, cmd);
    if (!a.has("on"))
        throw CommandError(std::string(cmd) + ": give on, the object to put it on (a table, a rack, a wall)");
    Node& onN = nodeNamed(g, a.get<std::string>("on"), cmd);
    if (onN.type().attrIndex("worldMatrix") < 0)
        throw CommandError(std::string(cmd) + ": " + onN.path() + " is not a transform");
    if (propFor(g, onN.id()) == p.node || isUnder(g, onN.id(), p.root))
        throw CommandError(std::string(cmd) + ": it can't go on itself");
    const NodeId on = onN.id();
    const KeepTime keep(g);
    const double t = a.has("time") ? a.get<double>("time") : g.time();
    const double blend = std::max(0.0, a.get<double>("blend", 0.4));
    const NodeId character = characterIfAny(g, a, p, cmd);
    const NodeId grapple = character ? propGrapple(g, character, true) : propGrapple(g, kNoNode, false);
    const std::vector<Holding> holding = holdingAt(g, grapple, p, t);
    const PropShape sh = shapeIn(g, p);
    at(g, t + blend);
    const glm::dmat4 onWorld = rigidOf(worldOf(g, on));
    at(g, t);
    const glm::dmat4 m0 = rigidOf(worldOf(g, p.root));
    const glm::dvec3 u = up();

    glm::dmat4 local(1.0), endWorld(1.0);
    std::string how;
    if (a.has("hook"))
    {
        // Hung by a point (default its main grip) from a hook, the rest hanging below it.
        Node& hookN = nodeNamed(g, a.get<std::string>("hook"), cmd);
        if (hookN.type().attrIndex("worldMatrix") < 0)
            throw CommandError(std::string(cmd) + ": " + hookN.path() + " is not a transform");
        const NodeId pt = gripPoint(g, p, a.get<std::string>("point", ""), Role::Main, cmd);
        const glm::dvec3 hangBy = pt ? glm::dvec3(pointLocal(g, p, pt)[3]) : sh.at({-1.0, 0.0, 0.0});
        at(g, t + blend);
        const glm::dvec3 hookAt(worldOf(g, hookN.id())[3]);
        at(g, t);
        glm::dvec3 down = pointOf(m0, sh.middle()) - pointOf(m0, hangBy);
        const glm::dquat q = glm::length(down) > 1e-6 ? turnOnto(down, -u) : glm::dquat(1, 0, 0, 0);
        local = translation(hangBy);
        endWorld = translation(hookAt) * rotationMatrix(q) * glm::dmat4(glm::dmat3(m0));
        how = "hung on " + hookN.path();
    }
    else
    {
        // Laid on the object's top: below the given position, else below where it is held, else the top's middle.
        const Surfaces surfaces = surfacesFor(g, on);
        if (surfaces.items.empty())
            throw CommandError(std::string(cmd) + ": " + onN.path() + " has no mesh to put it on; give hook to hang it from a point instead");
        geo::Bounds box;
        for (const Surfaces::Item& it : surfaces.items)
            box.extend(it.bvh->bounds());
        const double top = std::max(glm::dot(box.min, u), glm::dot(box.max, u));
        // From above the object's top straight down, so whatever it is held over is found first.
        auto below = [&](glm::dvec3 q) {
            q += u * (top - glm::dot(q, u) + 50.0);
            return surfaces.cast(q, -u, 1e5);
        };
        std::optional<Surfaces::Hit> hit;
        if (a.has("position"))
            hit = below(jsonVec(a.flags.at("position")));
        else
        {
            hit = below(pointOf(m0, sh.middle()));
            if (!hit)
            {
                hit = below(0.5 * (box.min + box.max));
                if (hit)
                    log::write(LogLevel::Warning, std::format("{}: {} isn't held over {}; put on the middle of its top", cmd, g.node(p.root)->name(), onN.name()));
            }
        }
        if (!hit)
            throw CommandError(std::string(cmd) + ": found no top on " + onN.path() + " to put it on");
        const glm::dvec3 nrm = glm::dot(hit->normal, u) < 0.0 ? -hit->normal : hit->normal;
        const glm::dmat4 rest = restingOn(sh, m0, hit->point, nrm);
        local = translation(sh.middle());
        endWorld = rest * local;
        how = "on " + surfaceName(g, hit->node);
    }
    // The pivot's end frame in the object's frame where it settles, so it rides the object from then on.
    switchFresh(g, p, ensureSpace(g, p.root, on), t);
    settleAbout(g, p, local, on, rigidOf(glm::inverse(onWorld) * endWorld), t, blend);
    carryAndLetGo(g, grapple, p, holding, t, blend, std::max(0.0, a.get<double>("release", 0.25)), a.get<bool>("fingers", true));
    recordEvent(g, p, {{"time", t}, {"kind", "place"}, {"on", g.path(on)}, {"how", how}});
    return finish(g, p, grapple, a, {{"on", g.path(on)}, {"how", how}, {"settled", t + blend}});
}

// puppeteerPropPass ----------------------------------------------------------------------------------------------------

json passCmd(const Args& a)
{
    Graph& g = Graph::current();
    const char* cmd = "puppeteerPropPass";
    Prop p = propArg(g, a, cmd);
    if (!a.has("to"))
        throw CommandError(std::string(cmd) + ": give to, the character who takes it");
    const KeepTime keep(g);
    const double t = a.has("time") ? a.get<double>("time") : g.time();
    const double duration = std::max(2.0 / fps(), a.get<double>("duration", 0.6));
    const double handover = t + 0.5 * duration, end = t + duration, frame = 1.0 / fps();
    const NodeId giver = characterArg(g, a, p, cmd);
    const NodeId receiver = characterArg(g, a, p, cmd, "to");
    if (giver == receiver)
        throw CommandError(std::string(cmd) + ": " + g.node(giver)->name() + " already has it; to is another character");
    const NodeId grapple = propGrapple(g, giver, true);
    propGrapple(g, receiver, true);
    at(g, t);
    const NodeId giverJoint = holderNow(g, p);
    if (!giverJoint || characterFor(g, giverJoint) != giver)
        throw CommandError(std::format("{}: {} isn't in {}'s hand at frame {:g}; hold it first", cmd, g.node(p.root)->name(), g.node(giver)->name(), t * fps()));
    const hand::Hand gh = *hand::load(g, hand::handFor(g, giverJoint));
    const NodeId giverPt = heldPoint(g, p, t, giverJoint);
    const std::string handName = a.get<std::string>("hand", "main");
    const bool right = handName == "main" ? p.rightHanded : handName == "right";
    const HandRef rh = needHand(g, receiver, right, cmd);

    // The receiver's grip: given, else another grip than the giver's (off, alt, then main), else one made beside it.
    NodeId recvPt = kNoNode;
    json made = nullptr;
    if (a.has("point"))
        recvPt = gripPoint(g, p, a.get<std::string>("point"), Role::Main, cmd);
    for (Role r : {Role::Off, Role::Alt, Role::Main})
        for (NodeId pt : p.points)
            if (!recvPt && pt != giverPt && roleOf(*g.node(pt)) == r)
                recvPt = pt;
    if (!recvPt)
    {
        const PropShape sh = shapeIn(g, p);
        PointDef d;
        d.role = Role::Alt;
        d.name = "pass";
        d.radius = numOf(g, giverPt, "radius");
        d.frame = pointLocal(g, p, giverPt);
        const glm::dvec3 from(d.frame[3]);
        glm::dvec3 toward = sh.middle() - from;
        toward = glm::length(toward) > 1e-6 ? glm::normalize(toward) : glm::dvec3(d.frame[0]);
        d.frame[3] = glm::dvec4(from + toward * 10.0, 1.0);
        recvPt = writePoint(g, p, d, kNoNode, false);
        made = g.path(recvPt);
        log::write(LogLevel::Warning, std::format("{}: {} has one grip, which {} holds; made {} 10 cm along for {} to take", cmd, g.node(p.root)->name(),
                                                  g.node(giver)->name(), g.node(recvPt)->name(), g.node(receiver)->name()));
    }

    // Both hands share it through one contact: the giver's hand held on its grip riding the receiver's hand, for the
    // whole exchange. The prop goes into the receiver's hand at the start, where it is, and eases into the receiver's
    // grip by the handover; the contact pulls the two hands together, each by half the gap, so both arms reach. Its gap is
    // nothing at the start, so nothing jumps, and the giver lets go by easing the contact out.
    for (const PropContact& c : propContacts(g, grapple, p))
        if (weightAt(g, c.node, t) > 1e-6)
        {
            keyWeight(g, c.node, t, weightAt(g, c.node, t));
            keyWeight(g, c.node, t + frame, 0.0);
        }
    at(g, t);
    const glm::dmat4 giverGrip = glm::inverse(rigidOf(worldOf(g, giverJoint))) * rigidOf(worldOf(g, giverPt));
    switchFresh(g, p, ensureSpace(g, p.root, rh.h.joint), t);
    settleInto(g, p, recvPt, rh.h.joint, gripOf(g, rh.h, p, recvPt, p.settings.trigger), t, handover - t);
    const NodeId cg = contactFor(g, grapple, p, gh, rh.h.joint, giverPt);
    setPlug(g, cg, "gripOffset", giverGrip);
    if (t - frame >= anim::playback().minTime)
        keyWeight(g, cg, t - frame, 0.0);
    keyWeight(g, cg, t, 1.0);
    keyWeight(g, cg, handover, 1.0);
    keyWeight(g, cg, end, 0.0);

    if (a.get<bool>("fingers", true))
    {
        poseFingers(g, rh.h, t, handover, p.settings.mainPose, true, p.settings.trigger, &p);
        poseFingers(g, gh, end - std::min(0.15, 0.5 * duration), end, "relaxed", false, false, nullptr);
    }
    json ev = {{"time", t}, {"kind", "pass"}, {"from", g.path(giver)}, {"to", g.path(receiver)}, {"character", g.path(receiver)},
               {"hand", g.path(rh.h.joint)}, {"point", g.path(recvPt)}, {"handover", handover}, {"end", end}};
    recordEvent(g, p, ev);
    json extra = {{"handover", handover}, {"end", end}, {"giver", g.path(giverJoint)}, {"receiver", g.path(rh.h.joint)}, {"giverPoint", g.path(giverPt)},
                  {"receiverPoint", g.path(recvPt)}, {"contact", g.path(cg)}};
    if (!made.is_null())
        extra["made"] = made;
    return finish(g, p, grapple, a, extra);
}

// puppeteerPropUnreal -------------------------------------------------------------------------------------------------

/** World matrices the gun, right and left ik bones follow each frame: the holding hand, then each hand. */
json unrealCmd(const Args& a)
{
    Graph& g = Graph::current();
    const char* cmd = "puppeteerPropUnreal";
    Prop p = propArg(g, a, cmd);
    const NodeId character = characterArg(g, a, p, cmd);
    const KeepTime keep(g);
    g.evaluate();
    NodeId bone = kNoNode;
    if (a.has("bone"))
    {
        bone = jointByNames(g, character, {a.get<std::string>("bone")});
        if (!bone)
            throw CommandError(std::format("{}: {} has no bone {}", cmd, g.node(character)->name(), a.get<std::string>("bone")));
    }
    else
    {
        bone = holderNow(g, p);
        if (!bone)
            bone = jointByNames(g, character, {p.rightHanded ? "hand_r" : "hand_l"});
        if (!bone)
            if (const std::optional<HandRef> h = handOf(g, character, p.rightHanded))
                bone = h->h.joint;
        if (!bone)
            throw CommandError(std::string(cmd) + ": give bone, the joint the prop attaches to");
    }
    std::string bareBone = g.node(bone)->name();
    if (const size_t c = bareBone.rfind(':'); c != std::string::npos)
        bareBone = bareBone.substr(c + 1);
    std::string socket = a.get<std::string>("socket", "");
    if (socket.empty())
        socket = bareBone.ends_with("_r") ? "weapon_r" : bareBone.ends_with("_l") ? "weapon_l" : bareBone + "_prop";

    const anim::Playback& pb = anim::playback();
    const double start = a.has("start") ? a.get<double>("start") : pb.minTime, end = a.has("end") ? a.get<double>("end") : pb.maxTime;
    const long long frames = std::max<long long>(0, std::llround((end - start) * fps()));
    // The socket's offset: the prop against the bone where the grip has settled, the last frame of the first run of frames
    // it is held; how far it strays from that over the held frames (the ease into the grip included) is reported.
    std::vector<glm::dmat4> held;
    std::optional<glm::dmat4> offset;
    bool runOver = false;
    for (long long f = 0; f <= frames; ++f)
    {
        at(g, start + double(f) / fps());
        if (holderNow(g, p) != bone)
        {
            runOver = runOver || !held.empty();
            continue;
        }
        held.push_back(glm::inverse(rigidOf(worldOf(g, bone))) * rigidOf(worldOf(g, p.root)));
        if (!runOver)
            offset = held.back();
    }
    const int heldFrames = int(held.size());
    double strayCm = 0.0, strayDeg = 0.0;
    for (const glm::dmat4& rel : held)
    {
        strayCm = std::max(strayCm, glm::length(glm::dvec3(rel[3]) - glm::dvec3((*offset)[3])));
        const glm::dquat q0 = glm::quat_cast(glm::dmat3(*offset)), q1 = glm::quat_cast(glm::dmat3(rel));
        strayDeg = std::max(strayDeg, glm::degrees(2.0 * std::acos(std::clamp(std::abs(glm::dot(q0, q1)), 0.0, 1.0))));
    }
    if (!offset)
    {
        at(g, g.time());
        offset = glm::inverse(rigidOf(worldOf(g, bone))) * rigidOf(worldOf(g, p.root));
    }
    auto setSocket = [&](const std::string& sock, const std::string& sockBone, const glm::dmat4& off) {
        for (const auto& [name, type, value] : {std::tuple{pins::kSocketAttr, PlugType::String, PlugValue(sock)},
                                                std::tuple{pins::kSocketBoneAttr, PlugType::String, PlugValue(sockBone)},
                                                std::tuple{pins::kSocketOffsetAttr, PlugType::Matrix, PlugValue(off)}})
        {
            if (g.node(p.root)->type().attrIndex(name) < 0)
                g.addAttr(p.root, {.name = name, .type = type, .defaultValue = type == PlugType::Matrix ? PlugValue(glm::dmat4(1.0)) : PlugValue(std::string()),
                                   .flags = AttrInput | AttrStorable,
                                   .doc = "Unreal: how the prop attaches to its character's skeletal mesh (puppeteerPropUnreal)."});
            g.setValue({p.root, g.node(p.root)->type().attrIndex(name), -1}, value);
        }
    };
    const std::string mode = a.get<std::string>("mode", "auto");
    if (mode != "auto" && mode != "socket" && mode != "baked")
        throw CommandError(std::string(cmd) + ": mode is auto, socket or baked");
    // Out of the hand for some of the range (thrown, planted, put down, passed): a socket can't carry that, so its
    // motion goes onto a bone of its own under the skeleton's root, keyed every frame, which it attaches to with no offset.
    const bool baked = mode == "baked" || (mode == "auto" && heldFrames < frames + 1 && !a.has("bone"));
    json out = {{"prop", g.path(p.root)}, {"mode", baked ? "baked" : "socket"}, {"heldFrames", heldFrames}, {"frames", frames + 1}};
    if (baked)
    {
        const NodeId carrier = characterRoot(g, character);
        std::string boneName = a.get<std::string>("socket", "");
        if (boneName.empty())
        {
            boneName = g.node(p.root)->name();
            if (const size_t c = boneName.rfind(':'); c != std::string::npos)
                boneName = boneName.substr(c + 1);
            boneName += "_motion";
        }
        NodeId joint = kNoNode;
        for (NodeId c : g.children(carrier))
            if (g.node(c)->name() == boneName && rig::isJoint(*g.node(c)))
                joint = c;
        if (!joint)
        {
            joint = g.createNode(*types::find("joint"), boneName, carrier)->id();
            g.evaluate();
        }
        g.setValue({joint, Transform::kRotationInterpolation, -1}, int64_t(RotationInterpolation::Euler));
        std::vector<double> times;
        std::vector<glm::dvec3> tr, ro;
        for (long long f = 0; f <= frames; ++f)
        {
            const double time = start + double(f) / fps();
            at(g, time);
            const glm::dmat4 local = glm::inverse(rigidOf(worldOf(g, carrier))) * rigidOf(worldOf(g, p.root));
            const glm::dvec3 near = ro.empty() ? glm::dvec3(0.0) : ro.back();
            times.push_back(time);
            tr.push_back(glm::dvec3(local[3]));
            ro.push_back(eulerNear(math::rotationOf(local), RotateOrder::XYZ, near));
        }
        for (const auto& [attr, values] : {std::pair{Transform::kTranslate, &tr}, std::pair{Transform::kRotate, &ro}})
            for (int c = 0; c < 3; ++c)
            {
                anim::Curve curve;
                for (size_t i = 0; i < times.size(); ++i)
                    curve.keys.push_back({.time = times[i], .value = (*values)[i][c], .in = anim::Tangent::Linear, .out = anim::Tangent::Linear});
                const NodeId cn = anim::setKey(g, {joint, attr, c}, curve.keys.front().time, curve.keys.front().value);
                anim::setCurve(g, cn, curve);
            }
        setSocket(boneName, "", glm::dmat4(1.0));
        out["socket"] = boneName;
        out["bone"] = boneName;
        out["joint"] = g.path(joint);
    }
    else
    {
        setSocket(socket, bareBone, *offset);
        out["socket"] = socket;
        out["bone"] = bareBone;
        out["strayCm"] = strayCm;
        out["strayDeg"] = strayDeg;
        if (heldFrames == 0)
            log::write(LogLevel::Warning, std::format("{}: {} is never held by {} in the range; the socket uses the pose now", cmd, g.path(p.root), bareBone));
        else if (strayCm > 0.5 || strayDeg > 1.0)
            log::write(LogLevel::Warning, std::format("{}: {} moves against {} while held (up to {:.1f} cm, {:.1f} deg); a socket holds one offset",
                                                      cmd, g.path(p.root), bareBone, strayCm, strayDeg));
        if (heldFrames < frames + 1)
            log::write(LogLevel::Warning, std::format("{}: {} is out of {} for {} of {} frames; a socket keeps it in the hand (mode=baked carries the rest)", cmd,
                                                      g.path(p.root), bareBone, frames + 1 - heldFrames, frames + 1));
    }

    // Optional ik bones (UE5's ik_hand_gun, ik_hand_r, ik_hand_l): keyed to follow the hands every frame of the range.
    if (a.get<bool>("ikBones", false))
    {
        const std::string gunName = a.get<std::string>("gunBone", "ik_hand_gun"), rName = a.get<std::string>("rightBone", "ik_hand_r"),
                          lName = a.get<std::string>("leftBone", "ik_hand_l");
        const NodeId gun = jointByNames(g, character, {gunName}), ikR = jointByNames(g, character, {rName}), ikL = jointByNames(g, character, {lName});
        std::string missing;
        for (const auto& [id, n] : {std::pair{gun, gunName}, std::pair{ikR, rName}, std::pair{ikL, lName}})
            if (!id)
                missing += (missing.empty() ? "" : ", ") + n;
        if (!missing.empty())
            throw CommandError(std::format("{}: the skeleton has no {}; ikBones needs them (or name yours with gunBone, rightBone, leftBone)", cmd, missing));
        const std::optional<HandRef> hr = handOf(g, character, true), hl = handOf(g, character, false);
        if (!hr || !hl)
            throw CommandError(std::string(cmd) + ": ikBones needs both hands set up (puppeteerHandSetup)");
        struct Track
        {
            NodeId bone;
            std::vector<double> t;
            std::vector<glm::dvec3> tr, ro;
        };
        std::vector<Track> tracks{{gun}, {ikR}, {ikL}};
        for (long long f = 0; f <= frames; ++f)
        {
            const double time = start + double(f) / fps();
            at(g, time);
            const NodeId holding = holderNow(g, p);
            const glm::dmat4 gunWorld = rigidOf(worldOf(g, holding ? holding : hr->h.joint));
            const glm::dmat4 want[3] = {gunWorld, rigidOf(worldOf(g, hr->h.joint)), rigidOf(worldOf(g, hl->h.joint))};
            // Each bone's parent as it will be: the gun bone's children follow its new pose.
            glm::dmat4 placed[3];
            for (int k = 0; k < 3; ++k)
            {
                const NodeId b = tracks[size_t(k)].bone;
                glm::dmat4 parent = parentWorldOf(g, b);
                if (k > 0 && g.parent(b) == gun)
                    parent = placed[0];
                placed[k] = want[k];
                const glm::dmat4 local = glm::inverse(parent) * want[k];
                const glm::dmat3 orient = orientOf(g, b);
                const glm::dmat3 r = glm::transpose(orient) * math::rotationOf(local);
                Track& tk = tracks[size_t(k)];
                const glm::dvec3 near = tk.ro.empty() ? vecOf(g, b, Transform::kRotate) : tk.ro.back();
                tk.t.push_back(time);
                tk.tr.push_back(glm::dvec3(local[3]));
                tk.ro.push_back(eulerNear(r, rotateOrderOf(g, b), near));
            }
        }
        for (const Track& tk : tracks)
        {
            g.setValue({tk.bone, Transform::kRotationInterpolation, -1}, int64_t(RotationInterpolation::Euler));
            for (const auto& [attr, values] : {std::pair{Transform::kTranslate, &tk.tr}, std::pair{Transform::kRotate, &tk.ro}})
                for (int c = 0; c < 3; ++c)
                {
                    anim::Curve curve;
                    for (size_t i = 0; i < tk.t.size(); ++i)
                        curve.keys.push_back({.time = tk.t[i], .value = (*values)[i][c], .in = anim::Tangent::Linear, .out = anim::Tangent::Linear});
                    const NodeId cn = anim::setKey(g, {tk.bone, attr, c}, curve.keys.front().time, curve.keys.front().value);
                    anim::setCurve(g, cn, curve);
                }
        }
        out["ikBones"] = {g.path(gun), g.path(ikR), g.path(ikL)};
        out["ikFrames"] = frames + 1;
    }
    return out;
}

// Registration ------------------------------------------------------------------------------------------------------

void registerTypes(ext::Context& ctx)
{
    const NodeType* transform = types::find("transform");
    NodeType prop{.name = kPropType,
                  .base = transform,
                  .dag = true,
                  .doc = "Marks its parent as a prop: any object a character holds (a weapon, a bottle, a chair leg). Its settings say "
                         "which hand holds it, whether two do, the hand poses, how it aims and where it is put away; its points "
                         "(puppeteerPropPoint children) say where hands take it and what aims. Made by puppeteerPropSetup.",
                  .icon = "sword",
                  .create = transform->create};
    const uint32_t io = AttrInput | AttrStorable;
    prop.ownAttrs = {
        {.name = "hand", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = io, .enumNames = {"right", "left"},
         .doc = "The hand that holds it by default."},
        {.name = "twoHanded", .type = PlugType::Bool, .defaultValue = false, .flags = io, .doc = "Held in both hands by default."},
        {.name = "mainPose", .type = PlugType::String, .defaultValue = std::string("grip"), .flags = io,
         .doc = "The main hand's pose (a Hands panel pose) before its fingers wrap the prop."},
        {.name = "offPose", .type = PlugType::String, .defaultValue = std::string("grip"), .flags = io, .doc = "The other hand's pose."},
        {.name = "trigger", .type = PlugType::Bool, .defaultValue = false, .flags = io,
         .doc = "The main hand's index finger stays posed instead of wrapping (on a trigger)."},
        {.name = "aimStyle", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = io, .enumNames = aimIds(),
         .doc = "How it aims: turn where it is, shoulder (the stock to the shoulder), extend (held out), guard (held out, dropped)."},
        number("aimDrop", 0.0, "Guard aims: cm the grip sits below the line to the target."),
        number("aimReach", 0.9, "Extend and guard aims: the share of the arm's length the grip is held out."),
        {.name = "holsterSpot", .type = PlugType::Enum, .defaultValue = int64_t(HolsterSpot::Belt), .flags = io, .enumNames = holsterIds(),
         .doc = "Where puppeteerPropHolster puts it away the first time (then the holster point it made)."},
        {.name = "fitShape", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = io, .enumNames = shapeIds(),
         .doc = "How Fit Points reads its shape: auto reads it from the mesh."},
        text("holster", "The point on the character it is put away at (made by puppeteerPropHolster)."),
        text("events", "Its handoffs (holds, throws, plants, passes), as JSON, for the Props panel's track.", AttrHidden),
    };
    NodeType point{.name = kPointType,
                   .base = transform,
                   .dag = true,
                   .doc = "A point on a prop (a grip, the muzzle or tip, the stock) or on a character (a holster). Grips: +X runs "
                          "along the handle toward the thumb, +Z is the way the palm faces. Muzzle and tip: +X is aimed.",
                   .icon = "point",
                   .create = transform->create};
    point.ownAttrs = {
        {.name = "role", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = io, .enumNames = roleIds(),
         .doc = "main, off and alt are grips; muzzle and tip are aimed; stock goes to the shoulder; holster holds a put-away prop."},
        number("radius", 1.6, "Grips: the handle's radius, cm, which sets how far the palm sits from it."),
        {.name = "fitted", .type = PlugType::Bool, .defaultValue = false, .flags = io | AttrHidden,
         .doc = "Made by Fit Points, which replaces it when it fits again; points added by hand are kept."},
    };
    NodeType arc{.name = kArcType,
                 .base = transform,
                 .dag = true,
                 .doc = "A prop's throw: where and when it leaves the hand, how fast and spinning, and the path it was keyed along to "
                        "its landing or catch. Made by puppeteerPropThrow, changed by puppeteerPropArc or by dragging its path.",
                 .icon = "ball-bowling",
                 .create = transform->create};
    arc.ownAttrs = {
        number("start", 0.0, "Seconds it leaves the hand."),
        number("end", 0.0, "Seconds its keyed path ends (landed and settled, caught, or the time limit)."),
        number("until", 3.0, "Seconds of flight at most."),
        vector3("launch", glm::dvec3(0.0), "World point its middle leaves from, cm."),
        vector3("velocity", glm::dvec3(0.0), "Launch velocity, cm/s, world."),
        number("spin", 0.0, "Degrees a second it turns in flight."),
        vector3("spinAxis", glm::dvec3(1.0, 0.0, 0.0), "World axis it spins about."),
        number("gravity", 981.0, "cm/s2 pulling it down."),
        {.name = "physics", .type = PlugType::Bool, .defaultValue = false, .flags = io,
         .doc = "Simulated (Jolt: it bounces, tumbles and rolls to rest on the scene's meshes) instead of a keyed arc."},
        {.name = "landed", .type = PlugType::Bool, .defaultValue = false, .flags = io | AttrHidden, .doc = "It met a surface before its end."},
        matrix("startMatrix", "The prop root's world matrix at the launch.", AttrHidden),
        vector3("centre", glm::dvec3(0.0), "The prop's middle in its root's space, which the path follows.", AttrHidden),
        text("path", "The middle's path as keyed: [[seconds, x, y, z], ...], for drawing.", AttrHidden),
    };
    for (NodeType* t : {&prop, &point, &arc})
    {
        // Points live in their parent's space; the prop node rides its root with no offset.
        relocate::registerNeutral(t->name);
        types::registerType(ctx, std::move(*t));
    }
}

/** Puts a point's world frame where the flags say: position, its +X (aim) and +Z (palm), each kept where not given. */
void placePoint(Graph& g, NodeId point, const Args& a)
{
    g.evaluate();
    const glm::dmat4 now = rigidOf(worldOf(g, point));
    const glm::dvec3 x = a.has("aim") ? jsonVec(a.flags.at("aim")) : glm::dvec3(now[0]);
    const glm::dvec3 z = a.has("palm") ? jsonVec(a.flags.at("palm")) : glm::dvec3(now[2]);
    if (glm::length(x) < 1e-9 || glm::length(z) < 1e-9)
        throw CommandError("puppeteerPropPoint: aim and palm need a length");
    const glm::dmat4 frame = frameXZ(a.has("position") ? jsonVec(a.flags.at("position")) : glm::dvec3(now[3]), x, z);
    if (a.has("aim") || a.has("palm"))
    {
        setWorldRotation(g, point, glm::dmat3(frame));
        g.evaluate();
    }
    if (a.has("position"))
    {
        setWorldPosition(g, point, glm::dvec3(frame[3]));
        g.evaluate();
    }
}

json pointCmd(const Args& a)
{
    Graph& g = Graph::current();
    const char* cmd = "puppeteerPropPoint";
    // A point named: edit or remove it. A prop: add one.
    NodeId edit = kNoNode;
    for (const std::string& s : objectArgs(a))
        if (Node& n = nodeNamed(g, s, cmd); n.type().isA(kPointType))
            edit = n.id();
    if (!edit && objectArgs(a).empty())
        for (NodeId s : g.selection())
            if (Node* n = g.node(s); n && n->type().isA(kPointType) && (a.has("remove") || !a.has("role")))
                edit = s;
    if (edit && roleOf(*g.node(edit)) == Role::Holster && !propFor(g, edit))
        throw CommandError(std::string(cmd) + ": " + g.path(edit) + " is a holster; move it in the viewport, or give puppeteerPropHolster another");
    if (edit)
    {
        const std::string path = g.path(edit);
        if (a.get<bool>("remove", false))
        {
            g.deleteNode(edit);
            g.evaluate();
            return {{"removed", path}};
        }
        if (a.has("role"))
        {
            const std::optional<Role> role = roleFromId(a.get<std::string>("role"));
            if (!role || *role == Role::Holster)
                throw CommandError(std::string(cmd) + ": role is main, off, alt, muzzle, tip or stock");
            setPlug(g, edit, "role", int64_t(*role));
        }
        if (a.has("radius"))
            setPlug(g, edit, "radius", std::max(0.0, a.get<double>("radius")));
        // Moved or turned by hand: Fit Points leaves it be.
        if (a.has("position") || a.has("aim") || a.has("palm"))
            setPlug(g, edit, "fitted", false);
        placePoint(g, edit, a);
        if (a.has("name"))
            if (const NodeId pn = propFor(g, edit))
                g.rename(edit, pointName(g, *load(g, pn), a.get<std::string>("name")));
        return {{"point", g.path(edit)}, {"role", roleId(roleOf(*g.node(edit)))}, {"radius", numOf(g, edit, "radius")},
                {"position", vecJson(glm::dvec3(worldOf(g, edit)[3]))}};
    }
    if (a.get<bool>("remove", false))
        throw CommandError(std::string(cmd) + ": give the point to remove");
    Prop p = propArg(g, a, cmd);
    const std::string roleName = a.get<std::string>("role", "alt");
    const std::optional<Role> role = roleFromId(roleName);
    if (!role || *role == Role::Holster)
        throw CommandError(std::string(cmd) + ": role is main, off, alt, muzzle, tip or stock");
    g.evaluate();
    PointDef d;
    d.role = *role;
    d.name = a.get<std::string>("name", roleName);
    for (int n = 2; named(g, pointName(g, p, d.name)); ++n)
        d.name = a.get<std::string>("name", roleName) + std::to_string(n);
    const NodeId like = p.point(g, isGrip(*role) ? Role::Main : *role);
    d.radius = a.get<double>("radius", like ? numOf(g, like, "radius") : 1.6);
    d.frame = like ? pointLocal(g, p, like) : glm::dmat4(1.0);
    const NodeId id = writePoint(g, p, d, kNoNode, false);
    placePoint(g, id, a);
    return {{"point", g.path(id)}, {"role", roleId(*role)}, {"radius", numOf(g, id, "radius")}, {"position", vecJson(glm::dvec3(worldOf(g, id)[3]))}};
}

void registerCommands(ext::Context& ctx)
{
    using T = ArgType;
    ctx.registerCommand({
        .name = "puppeteerPropSetup",
        .doc = "Makes a mesh or group a prop (any object a character holds) and fits its points to its shape: grips (main, off, "
               "alternates), the tip or muzzle that aims, the stock, whatever the shape has. Its settings (hands, hand poses, aim, "
               "holster spot) come from what the shape reads as, or from a preset. Points are transforms under the prop: move and "
               "turn them in the viewport, add or remove them with puppeteerPropPoint. Already a prop: a preset fills its settings "
               "and fits again. Returns the prop.",
        .positionalDoc = "mesh or group (default the selection)",
        .flags = {
            {"preset", "pr", T::String, nullptr, presetFlagDoc(), presetIds()},
            {"hand", "hd", T::String, nullptr, "right or left: the hand that holds it by default.", {"right", "left"}},
            {"twoHanded", "th", T::Bool, nullptr, "Held in both hands by default. Default: by preset or shape."},
            {"fit", "ft", T::Bool, true, "Fit the points to the mesh."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const char* cmd = "puppeteerPropSetup";
            const Preset* preset = presetArg(a, cmd);
            std::vector<NodeId> roots;
            for (const std::string& s : objectArgs(a))
                roots.push_back(nodeNamed(g, s, cmd).id());
            if (roots.empty())
                for (NodeId s : g.selection())
                    if (!propFor(g, s) || std::find(roots.begin(), roots.end(), s) == roots.end())
                        roots.push_back(s);
            if (roots.empty())
                throw CommandError("puppeteerPropSetup: give or select the prop's mesh or group");
            json out = json::array();
            for (NodeId r : roots)
            {
                NodeId pn = propFor(g, r);
                if (pn && g.parent(pn) != r)
                    r = g.parent(pn);
                if (characterFor(g, r) && !pn)
                    throw CommandError("puppeteerPropSetup: " + g.path(r) + " is part of a character");
                if (g.node(r)->type().attrIndex("worldMatrix") < 0)
                    throw CommandError("puppeteerPropSetup: " + g.path(r) + " is not a transform");
                const bool made = !pn;
                if (made)
                    pn = g.createNode(*types::find(kPropType), g.node(r)->name() + "_prop", r)->id();
                if (a.has("hand"))
                    setPlug(g, pn, "hand", int64_t(a.get<std::string>("hand") == "left" ? 1 : 0));
                Prop p = *load(g, pn);
                std::optional<Fit> fitted;
                if (preset)
                {
                    Settings s = preset->settings;
                    if (a.has("twoHanded"))
                        s.twoHanded = a.get<bool>("twoHanded");
                    writeSettings(g, pn, s);
                    p = *load(g, pn);
                    if (a.get<bool>("fit", true))
                        fitted = fitProp(g, p);
                }
                else if (made || p.points.empty())
                {
                    // Read from the shape: the fit says what it is, and the settings follow from that.
                    std::optional<bool> two;
                    if (a.has("twoHanded"))
                        two = a.get<bool>("twoHanded");
                    if (a.get<bool>("fit", true))
                    {
                        fitted = fitProp(g, p, FitHints{Shape::Auto, two});
                        Settings s = fitted->suggested;
                        if (two)
                            s.twoHanded = *two;
                        writeSettings(g, pn, s);
                    }
                    else if (two)
                        setPlug(g, pn, "twoHanded", *two);
                }
                else if (a.has("twoHanded"))
                    setPlug(g, pn, "twoHanded", a.get<bool>("twoHanded"));
                p = *load(g, pn);
                json j = propJson(g, p);
                j["fit"] = fitted ? fitJson(*fitted) : json(nullptr);
                out.push_back(j);
            }
            g.evaluate();
            return out.size() == 1 ? out.front() : out;
        },
        .examples = {"puppeteerPropSetup(\"bottle\")", "puppeteerPropSetup(\"rifle\", preset=\"rifle\")", "puppeteerPropSetup(\"sword\", preset=\"sword2h\")"},
    });

    ctx.registerCommand({
        .name = "puppeteerPropFit",
        .doc = "Fits a prop's points to its shape again, read as its fitShape setting says (points added or moved by hand are "
               "kept). Returns how they were found and the shape they were placed for.",
        .positionalDoc = "prop (default the selection)",
        .flags = {{"shape", "sh", T::String, nullptr, "Read the shape as this instead (and keep it as the prop's setting).", shapeIds()}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Prop p = propArg(g, a, "puppeteerPropFit");
            if (a.has("shape"))
            {
                const std::optional<Shape> sh = shapeFromId(a.get<std::string>("shape"));
                if (!sh)
                    throw CommandError("puppeteerPropFit: shape is one of auto, blade, haft, pole, firearm, tube, plate, bow, other");
                setPlug(g, p.node, "fitShape", int64_t(*sh));
                p = *load(g, p.node);
            }
            json out = fitJson(fitProp(g, p));
            out["prop"] = propJson(g, *load(g, p.node));
            return out;
        },
    });

    ctx.registerCommand({
        .name = "puppeteerPropPoint",
        .doc = "Adds, moves or removes a prop's point. Given a prop: adds a point (another grip, a muzzle, tip or stock), starting "
               "on the point of that role or the main grip, at position if given. Given a point: moves it to position, turns its "
               "+X to aim and its +Z (a grip's palm side) to palm, changes its role, radius or name; remove=True deletes it. "
               "Returns the point.",
        .positionalDoc = "prop, or one of its points (default the selection)",
        .flags = {{"role", "r", T::String, nullptr, "main, off, alt, muzzle, tip or stock. Adding: default alt.", {"main", "off", "alt", "muzzle", "tip", "stock"}},
                  {"name", "n", T::String, nullptr, "Name (after the prop's). Default: the role."},
                  {"position", "p", T::Vec3, nullptr, "World point to put it at."},
                  {"aim", "am", T::Vec3, nullptr, "World direction of its +X: along a grip's handle toward the thumb, or what a tip aims."},
                  {"palm", "pm", T::Vec3, nullptr, "World direction of its +Z: the way a hand's palm faces on a grip."},
                  {"radius", "rd", T::Float, nullptr, "Grips: the handle's radius, cm.", {}, {}, 0.0},
                  {"remove", "rm", T::Bool, false, "Delete the point."}},
        .run = pointCmd,
        .examples = {"puppeteerPropPoint(\"guitar\", role=\"off\", position=(10, 120, 30))", "puppeteerPropPoint(\"guitar_off_point\", remove=True)"},
    });

    ctx.registerCommand({
        .name = "puppeteerPropHold",
        .doc = "Puts a prop in a character's hand from time on: it switches into the hand's space there (no pop) and settles "
               "into the grip by time + blend, the hand closing round it. hands=2 puts the other hand on the off grip too, held by "
               "the solve, which shares the reach between both arms. A hand that held it before and doesn't now lets go. Use it "
               "again later to hand it to the other hand or change grip. Returns the prop with its holds.",
        .positionalDoc = "prop (default the selection)",
        .flags = {
            kCharacterFlag,
            {"hand", "hd", T::String, "main", "main (the prop's), right or left.", {"main", "right", "left"}},
            {"hands", "n", T::Int, nullptr, "1 or 2. Default: the prop's two-handed setting.", {}, {}, 1.0, 2.0},
            {"point", "pt", T::String, nullptr, "The grip point the hand takes, by name or role. Default: main."},
            {"offPoint", "op", T::String, nullptr, "With two hands: the other hand's grip. Default: off."},
            {"fingers", "fi", T::Bool, true, "Pose and wrap the fingers round the prop."},
            {"reach", "re", T::Float, 0.0, "Seconds before time the hand reaches for the prop where it is (on a table, in the "
                                           "air, on the ground) and takes it there; 0 brings the prop to the hand instead.", {}, {}, 0.0},
            {"event", "", T::String, nullptr, "What the track calls the hold (take, catch). Default: hold, or take when reaching."},
            kTimeFlag, kBlendFlag, kSolveFlag,
            {"start", "", T::Int, nullptr, "Solve from this frame. Default: the playback start."},
            {"end", "", T::Int, nullptr, "Solve to this frame."},
        },
        .run = holdCmd,
        .examples = {"puppeteerPropHold(\"sword\", hands=2, time=0.5)", "puppeteerPropHold(\"pistol\", hand=\"left\", time=2.0, blend=0.25)"},
    });

    const FlagDef releaseFlag{"release", "rl", T::Float, 0.25, "Seconds the hands take to let go once it is down.", {}, {}, 0.0};
    const FlagDef fingersFlag{"fingers", "fi", T::Bool, true, "Open the hands that held it."};
    ctx.registerCommand({
        .name = "puppeteerPropPlant",
        .doc = "Stands a held prop on its tip or base on the surface below it, a little ahead of the character: over blend the "
               "hands carry it down (the arms follow it) and it settles leaning at lean degrees from the surface's normal, then "
               "the hands let go and it stays there in the world. Surfaces are the scene's meshes (not props or characters) and "
               "the ground. Returns the prop with where it stands.",
        .positionalDoc = "prop (default the selection)",
        .flags = {kCharacterFlag,
                  {"standOn", "so", T::String, "tip", "The end it stands on: tip (where its tip or muzzle is), base, or a point's name.", {"tip", "base"}},
                  {"lean", "ln", T::Float, 12.0, "Degrees it leans from upright.", {}, {}, 0.0, 89.0},
                  {"toward", "tw", T::Vec3, nullptr, "World direction it leans toward. Default: back toward where it was held."},
                  {"ahead", "ah", T::Float, 25.0, "cm ahead of the character the surface is looked for below."},
                  {"position", "p", T::Vec3, nullptr, "World point to plant it below instead."},
                  kTimeFlag,
                  {"blend", "b", T::Float, 0.4, "Seconds it takes to go down and settle.", {}, {}, 0.0},
                  releaseFlag, fingersFlag, kSolveFlag},
        .run = plantCmd,
        .examples = {"puppeteerPropPlant(\"sword\", time=1.0, lean=10)", "puppeteerPropPlant(\"staff\", standOn=\"base\", lean=0)"},
    });

    ctx.registerCommand({
        .name = "puppeteerPropThrow",
        .doc = "Throws a held prop at time: it leaves the hands with the speed and spin the hand gave it (or speed, angle and "
               "direction, or velocity, given) and flies a keyed ballistic arc until it meets a surface, where it settles lying "
               "on it. physics=True simulates it with Jolt instead (bouncing, tumbling, coming to rest) and bakes that to keys. "
               "The arc is a node under the prop: drag its path in the viewport or change it with puppeteerPropArc; "
               "puppeteerPropCatch catches it. Returns the arc.",
        .positionalDoc = "prop (default the selection)",
        .flags = {kCharacterFlag,
                  {"speed", "sp", T::Float, nullptr, "cm/s at the release. Default: the hand's, else 500.", {}, {}, 0.0},
                  {"angle", "an", T::Float, nullptr, "Degrees above level it leaves at, with speed. Default 35.", {}, {}, -89.0, 89.0},
                  {"direction", "dr", T::Vec3, nullptr, "World direction it is thrown, levelled, with speed. Default: the character's forward."},
                  {"velocity", "v", T::Vec3, nullptr, "World launch velocity, cm/s, instead."},
                  {"spin", "sn", T::Float, nullptr, "Degrees a second it turns. Default: the hand's when it turns it, else none."},
                  {"spinAxis", "sx", T::Vec3, nullptr, "World axis it spins about. Default: across its flight (end over end)."},
                  {"physics", "ph", T::Bool, false, "Simulate it (Jolt) instead of keying an arc."},
                  {"gravity", "gr", T::Float, 981.0, "cm/s2.", {}, {}, 0.0},
                  {"until", "ut", T::Float, 3.0, "Seconds of flight at most.", {}, {}, 0.0},
                  kTimeFlag,
                  {"release", "rl", T::Float, 0.15, "Seconds the fingers take to open.", {}, {}, 0.0},
                  fingersFlag, kSolveFlag},
        .run = throwCmd,
        .examples = {"puppeteerPropThrow(\"ball\", time=0.5)", "puppeteerPropThrow(\"axe\", time=0.5, speed=900, spin=720, physics=True)"},
    });

    ctx.registerCommand({
        .name = "puppeteerPropArc",
        .doc = "Changes a throw's arc and keys it again: through a world point at a time on it (what dragging its path does), "
               "or a new velocity, spin, axis, gravity or time limit, or physics on or off. Returns the arc.",
        .positionalDoc = "the arc (default the selection)",
        .flags = {{"through", "th", T::Vec3, nullptr, "World point its middle passes through, at at."},
                  {"at", "a", T::Float, nullptr, "Seconds on the arc for through. Default: its end."},
                  {"velocity", "v", T::Vec3, nullptr, "World launch velocity, cm/s."},
                  {"spin", "sn", T::Float, nullptr, "Degrees a second."},
                  {"spinAxis", "sx", T::Vec3, nullptr, "World spin axis."},
                  {"physics", "ph", T::Bool, nullptr, "Simulate it instead of keying an arc."},
                  {"gravity", "gr", T::Float, nullptr, "cm/s2.", {}, {}, 0.0},
                  {"until", "ut", T::Float, nullptr, "Seconds of flight at most.", {}, {}, 0.0},
                  kSolveFlag},
        .run = arcCmd,
        .examples = {"puppeteerPropArc(\"ball_throw\", through=(0, 220, 300), at=0.6)"},
    });

    ctx.registerCommand({
        .name = "puppeteerPropCatch",
        .doc = "Catches a thrown prop: finds the frame its grip comes nearest a hand (either, or the one given, of the thrower or "
               "another character) within that arm's reach, ends the arc there, and the hand reaches for it over reach and takes "
               "it. Out of reach, it is caught where it comes closest and says so. Returns when and by which hand.",
        .positionalDoc = "prop (default the selection)",
        .flags = {{"character", "ch", T::String, nullptr, "Who catches it. Default: whoever threw it."},
                  {"hand", "hd", T::String, "either", "either, right or left.", {"either", "right", "left"}},
                  {"point", "pt", T::String, nullptr, "The grip it is caught by. Default: main."},
                  {"reach", "re", T::Float, 0.3, "Seconds the hand moves to meet it before the catch.", {}, {}, 0.0},
                  {"time", "t", T::Float, nullptr, "Catches the throw launched by then. Default: now, else the first throw."},
                  {"blend", "b", T::Float, 0.2, "Seconds it settles into the grip after.", {}, {}, 0.0},
                  fingersFlag, kSolveFlag},
        .run = catchCmd,
        .examples = {"puppeteerPropCatch(\"ball\")", "puppeteerPropCatch(\"ball\", character=\"bob\", hand=\"left\")"},
    });

    ctx.registerCommand({
        .name = "puppeteerPropPlace",
        .doc = "Puts a held prop on an object (a table, a rack, a shelf) at time: over blend the hands carry it onto the "
               "object's top below where it is held (or below position), where it settles lying on its thinnest side, or hang it "
               "by a point from hook (a transform on a wall hook or rack); then the hands let go. From then on it rides the "
               "object, following it when it moves. puppeteerPropHold with reach takes it back. Returns the prop.",
        .positionalDoc = "prop (default the selection)",
        .flags = {kCharacterFlag,
                  nodeFlag({"on", "on", T::String, nullptr, "The object it goes on."}, "transform"),
                  nodeFlag({"hook", "hk", T::String, nullptr, "A transform to hang it from instead of laying it on the top."}, "transform"),
                  {"point", "pt", T::String, nullptr, "With hook: the point it hangs by. Default: main."},
                  {"position", "p", T::Vec3, nullptr, "World point on the object to put it at."},
                  kTimeFlag,
                  {"blend", "b", T::Float, 0.4, "Seconds it takes to go there and settle.", {}, {}, 0.0},
                  releaseFlag, fingersFlag, kSolveFlag},
        .run = placeCmd,
        .examples = {"puppeteerPropPlace(\"mug\", on=\"table\", time=2.0)", "puppeteerPropPlace(\"sword\", on=\"rack\", hook=\"rack_hook1\")"},
    });

    ctx.registerCommand({
        .name = "puppeteerPropPass",
        .doc = "Hands a held prop to another character over duration from time: the receiver's hand takes it by its own grip "
               "(off, alt or main, whichever the giver isn't holding), both hands holding it with both arms sharing the reach "
               "until the handover halfway through, and the giver lets go by the end. The solve moves both characters. "
               "Returns the handover.",
        .positionalDoc = "prop (default the selection)",
        .flags = {{"character", "ch", T::String, nullptr, "Who gives it. Default: whoever holds it then."},
                  {"to", "to", T::String, nullptr, "Who takes it."},
                  {"hand", "hd", T::String, "main", "The receiver's hand: main (the prop's), right or left.", {"main", "right", "left"}},
                  {"point", "pt", T::String, nullptr, "The grip the receiver takes. Default: one the giver isn't holding."},
                  {"duration", "du", T::Float, 0.6, "Seconds from the receiver reaching to the giver letting go.", {}, {}, 0.0},
                  kTimeFlag, fingersFlag, kSolveFlag},
        .run = passCmd,
        .examples = {"puppeteerPropPass(\"mug\", to=\"bob\", time=1.0)"},
    });

    ctx.registerCommand({
        .name = "puppeteerPropDrop",
        .doc = "Lets go of a prop at time: it stays in the world where it is, the hands open over blend. Returns the prop.",
        .positionalDoc = "prop (default the selection)",
        .flags = {kTimeFlag, kBlendFlag, kSolveFlag, {"fingers", "fi", T::Bool, true, "Open the hands that held it."}},
        .run = dropCmd,
    });

    ctx.registerCommand({
        .name = "puppeteerPropHolster",
        .doc = "Puts a prop away at time: it switches to its holster on the character (made from the class's default spot, "
               "back, hip or thigh, the first time; move it to taste) and settles there by time + blend while the hands let go. "
               "puppeteerPropHold draws it again. Returns the prop.",
        .positionalDoc = "prop (default the selection)",
        .flags = {kCharacterFlag,
                  nodeFlag({"holster", "hs", T::String, nullptr, "A transform on the character to put it at instead."}, "transform"),
                  kTimeFlag, kBlendFlag, kSolveFlag, {"fingers", "fi", T::Bool, true, "Open the hands that held it."}},
        .run = holsterCmd,
    });

    ctx.registerCommand({
        .name = "puppeteerPropAim",
        .doc = "Aims a held prop's muzzle or tip at a target from time on, weight blending in over blend; the arms and body "
               "follow in the solve. style auto takes the class's: shoulder (rifles, shotguns, launchers: the stock to the "
               "shoulder), extend (pistols, bows: held out toward the target), guard (blades: held out, dropped below the line), "
               "turn (turns where it is). off=True blends the aim out. Returns the prop.",
        .positionalDoc = "prop (default the selection)",
        .flags = {kCharacterFlag,
                  nodeFlag({"target", "tg", T::String, nullptr, "The transform to aim at (a locator)."}, "transform"),
                  {"style", "s", T::String, "auto", "auto, turn, shoulder, extend or guard.", {"auto", "turn", "shoulder", "extend", "guard"}},
                  {"weight", "w", T::Float, 1.0, "How fully to aim.", {}, {}, 0.0, 1.0},
                  {"off", "", T::Bool, false, "Blend the prop's aims out instead."},
                  kTimeFlag, kBlendFlag, kSolveFlag,
                  {"start", "", T::Int, nullptr, "Solve from this frame."},
                  {"end", "", T::Int, nullptr, "Solve to this frame."}},
        .run = aimCmd,
        .examples = {"puppeteerPropAim(\"rifle\", target=\"enemy\", time=1.0)"},
    });

    ctx.registerCommand({
        .name = "puppeteerPropSolve",
        .doc = "Solves the arms and body holding props over a range onto the character's props layer: off hands held on their "
               "grips with both arms sharing the reach, aims turned onto their targets. Holds and aims do this themselves unless "
               "solve=False. Returns {'layer', 'frames', 'contactError'} (cm).",
        .positionalDoc = "prop or character (default the selection)",
        .flags = {kCharacterFlag, {"start", "", T::Int, nullptr, "First frame. Default: the playback start."}, {"end", "", T::Int, nullptr, "Last frame."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            NodeId character = kNoNode;
            for (const std::string& s : objectArgs(a))
                if (!character)
                    character = characterFor(g, nodeNamed(g, s, "puppeteerPropSolve").id());
            if (!character)
                character = characterArg(g, a, propArg(g, a, "puppeteerPropSolve"), "puppeteerPropSolve");
            const NodeId grapple = propGrapple(g, character, false);
            if (!grapple)
                throw CommandError("puppeteerPropSolve: " + g.node(character)->name() + " holds no props");
            return solveGrapple(g, grapple, a);
        },
    });

    ctx.registerCommand({
        .name = "puppeteerPropInfo",
        .doc = "A prop's settings, points (world positions), holds over time (who or what holds it from when), its keyed "
               "events (holds, plants, throws, catches, places, passes) and its holder now.",
        .positionalDoc = "prop (default the selection)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const Prop p = propArg(g, a, "puppeteerPropInfo");
            g.evaluate();
            return propJson(g, p);
        },
    });

    ctx.registerCommand({
        .name = "puppeteerPropPresets",
        .doc = "The prop presets and the settings each fills: id, label, two-handed, hand poses, trigger, aim style, drop, reach, "
               "holster spot and how the shape is read.",
        .undoable = false,
        .run = [](const Args&) -> json {
            json out = json::array();
            for (const Preset& p : presets())
            {
                const Settings& s = p.settings;
                out.push_back({{"id", p.id},           {"label", p.label},     {"icon", p.icon},        {"twoHanded", s.twoHanded},
                               {"mainPose", s.mainPose}, {"offPose", s.offPose}, {"trigger", s.trigger}, {"aimStyle", aimStyleId(s.aim)},
                               {"aimDrop", s.drop},    {"aimReach", s.reach},  {"holsterSpot", holsterSpot(s.holster).id}, {"shape", shapeId(s.shape)}});
            }
            return out;
        },
    });

    ctx.registerCommand({
        .name = "puppeteerPropUnreal",
        .doc = "Sets a prop up for Unreal: it attaches to a bone of the character's skeletal mesh (by default the hand holding "
               "it, hand_r) through a socket with the offset it is held at, which the Unreal link makes on the mesh when it sends "
               "the prop; the character's own bones carry the animation. With ikBones, also keys the skeleton's hand IK bones "
               "(by default UE5's ik_hand_gun, ik_hand_r, ik_hand_l) to follow the hands every frame, for projects whose "
               "locomotion reads them. A prop that leaves the hand in the range (thrown, planted, put down, passed) is baked "
               "instead: its motion keyed every frame onto a bone of its own under the skeleton's root (<prop>_motion), which "
               "it attaches to with no offset, so the character's animation carries it. Returns the mode, socket and bone, and "
               "for a socket how far the prop strays against the bone while held.",
        .positionalDoc = "prop (default the selection)",
        .flags = {kCharacterFlag,
                  {"bone", "bn", T::String, nullptr, "The bone it attaches to. Default: the hand holding it now, else hand_r (hand_l left-handed)."},
                  {"socket", "sk", T::String, nullptr, "The socket's name. Default: weapon_r or weapon_l by the bone's side."},
                  {"ikBones", "ik", T::Bool, false, "Key the hand IK bones to follow the hands. Off by default; needs the bones."},
                  {"gunBone", "", T::String, "ik_hand_gun", "ikBones: the bone that follows the holding hand."},
                  {"rightBone", "", T::String, "ik_hand_r", "ikBones: the bone that follows the right hand."},
                  {"leftBone", "", T::String, "ik_hand_l", "ikBones: the bone that follows the left hand."},
                  {"mode", "md", T::String, "auto", "socket: on the holding bone at the held offset. baked: its motion on a bone of its own "
                                                    "under the skeleton's root, every frame. auto: socket while it never leaves the hand, else baked.",
                   {"auto", "socket", "baked"}},
                  {"start", "st", T::Float, nullptr, "Seconds; default the playback start."},
                  {"end", "et", T::Float, nullptr, "Seconds; default the playback end."}},
        .run = unrealCmd,
        .examples = {"puppeteerPropUnreal(\"rifle\")", "puppeteerPropUnreal(\"rifle\", ikBones=True)"},
    });
}
} // namespace

void registerProps(ext::Context& ctx)
{
    registerTypes(ctx);
    registerCommands(ctx);
}
} // namespace puppeteer::prop

namespace puppeteer
{
void registerProps(lila::ext::Context& ctx) { prop::registerProps(ctx); }
} // namespace puppeteer
