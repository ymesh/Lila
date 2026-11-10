#include "sculpt/SculptSession.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "geo/ColorSets.h"
#include "geo/Symmetry.h"
#include "retopo/Retopo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "sculpt/Reproject.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <optional>

namespace lila::sculpt
{
namespace
{
using scene::Graph;
using scene::MeshNode;
using scene::MeshPtr;
using scene::Node;
using scene::NodeId;
using scene::PlugRef;
using scene::Transform;
namespace colors = geo::colors;

std::shared_ptr<Session>& activeRef()
{
    static std::shared_ptr<Session> s;
    return s;
}

std::map<int, std::function<void()>>& listeners()
{
    static std::map<int, std::function<void()>> l;
    return l;
}

void setActive(std::shared_ptr<Session> s)
{
    activeRef() = std::move(s);
    notify();
}

/** Rough: the cage, the proxy and the per-point layers the proxy carries (normals, mask, adjacency, BVH). */
size_t sessionBytes(const Session& s)
{
    return sizeof(Session) + s.sculpt.mesh().memoryBytes() * 2;
}

size_t stackBytes(const SubdStack& stack)
{
    size_t n = 0;
    for (const geo::PointStencil& st : stack.stencils)
        n += (st.offsets.capacity() + st.cols.capacity()) * sizeof(Index) + st.weights.capacity() * sizeof(double);
    for (const auto& l : stack.levels)
        n += l->mesh.memoryBytes() + (l->parentPoint.capacity() + l->framePeer.capacity() + l->down.cols.capacity()) * sizeof(Index) +
             l->down.weights.capacity() * sizeof(double);
    return n;
}

size_t detailBytes(const LevelDetail& d)
{
    return d.offset.capacity() * sizeof(glm::dvec3) + d.layers.color.capacity() * sizeof(glm::vec4) +
           (d.layers.mask.capacity() + d.layers.freeze.capacity()) * sizeof(float);
}

/** The session and the level data it shares with the sessions of its other levels. */
void addSessionBytes(UndoShared& shared, const Session& s)
{
    shared.add(&s, sessionBytes(s));
    if (s.stack)
        shared.add(s.stack.get(), stackBytes(*s.stack));
    for (const DetailPtr& d : s.detail)
        if (d)
            shared.add(d.get(), detailBytes(*d));
}

/** Switches the open session; undo switches back. */
struct SwitchAction : UndoAction
{
    std::shared_ptr<Session> from, to;
    SwitchAction(std::shared_ptr<Session> f, std::shared_ptr<Session> t) : from(std::move(f)), to(std::move(t)) {}
    void undo() override { setActive(from); }
    void redo() override { setActive(to); }
    void sharedBytes(UndoShared& shared) const override
    {
        for (const auto& s : {from, to})
            if (s)
                addSessionBytes(shared, *s);
    }
};

std::function<void(const std::optional<glm::dvec3>&)>& pivotHandler()
{
    static std::function<void(const std::optional<glm::dvec3>&)> fn;
    return fn;
}

struct StrokeAction : UndoAction
{
    std::shared_ptr<Session> session;
    StrokeRecord record;
    StrokeAction(std::shared_ptr<Session> s, StrokeRecord r) : session(std::move(s)), record(std::move(r)) {}
    void undo() override
    {
        session->sculpt.apply(record, true);
        if (record.pivotMoved && pivotHandler())
            pivotHandler()(record.pivotBefore);
        notify();
    }
    void redo() override
    {
        session->sculpt.apply(record, false);
        if (record.pivotMoved && pivotHandler())
            pivotHandler()(record.pivotAfter);
        notify();
    }
    size_t bytes() const override
    {
        const StrokeRecord& r = record;
        return sizeof(*this) + r.points.capacity() * sizeof(Index)
            + (r.before.capacity() + r.after.capacity()) * sizeof(glm::dvec3)
            + (r.maskBefore.capacity() + r.maskAfter.capacity()) * sizeof(float) + r.frozenBefore.capacity()
            + r.frozenAfter.capacity() + (r.colorBefore.capacity() + r.colorAfter.capacity()) * sizeof(glm::vec4);
    }
    void sharedBytes(UndoShared& shared) const override { addSessionBytes(shared, *session); }
};

Graph& graph() { return Graph::current(); }

Node& meshArg(const std::string& name, const char* cmd)
{
    Node* n = graph().find(name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return *n;
}

std::shared_ptr<Session> requireSession(const char* cmd)
{
    std::shared_ptr<Session> s = activeRef();
    if (!s)
        throw CommandError(std::string(cmd) + ": not in sculpt mode; run sculptEnter first");
    if (!graph().node(s->node))
        throw CommandError(std::string(cmd) + ": the sculpted mesh no longer exists; run sculptCancel");
    return s;
}

glm::dmat4 worldOf(const Node& n) { return n.as<glm::dmat4>(Transform::kWorldMatrix); }

double symmetryTolerance(const geo::Mesh& m) { return std::max(1e-4, glm::length(m.bounds().size()) * 1e-5); }

geo::Mesh storedMesh(const PlugRef& p)
{
    const MeshPtr& m = std::get<MeshPtr>(graph().storedValue(p));
    return m ? *m : geo::Mesh();
}

/** The drawn result of src, in the object space of another node. */
geo::Mesh meshInSpaceOf(const Node& src, const Node& space)
{
    graph().evaluate();
    MeshPtr m = src.as<MeshPtr>(MeshNode::kOutMesh);
    if (!m)
        return {};
    geo::Mesh out = *m;
    out.compact();
    glm::dmat4 x = glm::inverse(worldOf(space)) * worldOf(src);
    for (glm::dvec3& p : out.positions())
        p = glm::dvec3(x * glm::dvec4(p, 1.0));
    return out;
}

/** A hidden attr on a mesh with stored levels, added on first use. */
int levelAttr(Node& n, const char* name, scene::PlugType type, scene::PlugValue def, const char* doc)
{
    int i = n.type().attrIndex(name);
    if (i < 0)
        i = graph().addAttr(n.id(), scene::AttrDef{.name = name, .type = type, .defaultValue = std::move(def), .flags = scene::AttrHidden, .doc = doc});
    return i;
}

int64_t intAttr(const Node& n, const char* name, int64_t fallback)
{
    const int i = n.type().attrIndex(name);
    return i < 0 ? fallback : n.as<int64_t>(i);
}

MeshPtr meshAttr(const Node& n, const char* name)
{
    const int i = n.type().attrIndex(name);
    return i < 0 ? nullptr : std::get<MeshPtr>(graph().storedValue({n.id(), i, -1}));
}

glm::dvec3 vec3(const json& j, const glm::dvec3& fallback = glm::dvec3(0.0))
{
    if (!j.is_array() || j.size() != 3)
        return fallback;
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

glm::vec3 colorArg(const Args& a, const char* cmd)
{
    const json c = a.flags.value("color", json::array({1.0, 1.0, 1.0}));
    if (!c.is_array() || c.size() < 3 || !c[0].is_number() || !c[1].is_number() || !c[2].is_number())
        throw CommandError(std::string(cmd) + ": color must be [r, g, b], linear 0 to 1");
    return {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
}

BrushSettings settingsFrom(const Args& a)
{
    BrushSettings s;
    std::string name = a.get<std::string>("brush", "draw");
    if (!setBrush(s, name))
        throw CommandError("sculptStroke: unknown brush " + name);
    s.paint = a.get<bool>("paint");
    s.color = colorArg(a, "sculptStroke");
    std::string falloff = a.get<std::string>("falloff", "smooth");
    auto f = brush::Falloff::find(falloff);
    if (!f)
        throw CommandError("sculptStroke: unknown falloff " + falloff);
    s.falloff = *f;
    const std::string axis = a.get<std::string>("radialAxis", "y");
    s.radial = {std::clamp(a.get<int>("radialCount", 1), 1, 64), axis == "x" ? 0 : axis == "z" ? 2 : 1};
    s.strength = std::clamp(a.get<double>("strength", 0.5), 0.0, 1.0);
    s.invert = a.get<bool>("invert");
    s.blur = a.get<bool>("blur");
    s.keepVolume = a.get<bool>("keepVolume", true);
    s.pinchDepth = std::clamp(a.get<double>("pinchDepth", 1.0), 0.0, 4.0);
    s.frontFacesOnly = a.get<bool>("frontFacesOnly");
    return s;
}

/** Dabs given in world space, moved into the mesh's object space. */
std::vector<Dab> dabsFrom(const json& list, const glm::dmat4& world)
{
    std::vector<Dab> out;
    if (!list.is_array())
        throw CommandError("sculptStroke: dabs must be a list");
    const glm::dmat4 inv = glm::inverse(world);
    const glm::dmat3 normalToLocal = glm::transpose(glm::dmat3(world));
    const double scale = std::max(1e-12, std::cbrt(std::abs(glm::determinant(glm::dmat3(world)))));
    for (const json& j : list)
    {
        if (!j.is_object() || !j.contains("center"))
            throw CommandError("sculptStroke: each dab needs at least a center");
        Dab d;
        d.center = glm::dvec3(inv * glm::dvec4(vec3(j["center"]), 1.0));
        glm::dvec3 n = normalToLocal * vec3(j.value("normal", json()), glm::dvec3(0.0, 1.0, 0.0));
        d.normal = glm::length(n) > 0.0 ? glm::normalize(n) : glm::dvec3(0.0, 1.0, 0.0);
        d.radius = j.value("radius", 1.0) / scale;
        d.pressure = j.value("pressure", 1.0);
        // Scripts from before the brush core gave the amount as pressure.
        d.opacity = j.value("opacity", d.pressure);
        d.flow = j.value("flow", 1.0);
        d.viewDir = glm::dvec3(inv * glm::dvec4(vec3(j.value("view", json())), 0.0));
        d.delta = glm::dvec3(inv * glm::dvec4(vec3(j.value("delta", json())), 0.0));
        d.direction = glm::dvec3(inv * glm::dvec4(vec3(j.value("direction", json())), 0.0));
        out.push_back(d);
    }
    return out;
}

std::atomic<uint64_t> gLiveToken{1};

void loadLayers(SculptMesh& sm, const LevelLayers& layers)
{
    if (!layers.mask.empty())
        sm.setMask(layers.mask);
    if (!layers.freeze.empty())
    {
        StrokeRecord r;
        for (Index p = 0; p < layers.freeze.size(); ++p)
            if (layers.freeze[p] > 0.5f)
            {
                r.points.push_back(p);
                r.frozenAfter.push_back(1);
            }
        sm.apply(r, false);
    }
    if (!layers.color.empty())
        sm.setColors(layers.color);
}

LevelLayers layersOf(const SculptMesh& sm)
{
    LevelLayers out;
    if (std::ranges::any_of(sm.mask(), [](float m) { return m > 0.0f; }))
        out.mask.assign(sm.mask().begin(), sm.mask().end());
    if (std::ranges::any_of(sm.frozen(), [](uint8_t f) { return f != 0; }))
        for (uint8_t f : sm.frozen())
            out.freeze.push_back(f ? 1.0f : 0.0f);
    if (sm.painted())
        out.color.assign(sm.colors().begin(), sm.colors().end());
    return out;
}

/** The layers a stored finest level carries as point attrs and its polypaint color set. */
LevelLayers layersFrom(const geo::Mesh& m)
{
    LevelLayers out;
    if (auto mask = m.pointAttrs().get<float>(kMaskLayer); !mask.empty())
        out.mask.assign(mask.begin(), mask.end());
    if (auto freeze = m.pointAttrs().get<float>(kFreezeLayer); !freeze.empty())
        out.freeze.assign(freeze.begin(), freeze.end());
    if (colors::has(m, kColorSet))
        out.color = colors::pointColors(m, kColorSet);
    return out;
}

/** A new session for s's mesh sculpting level `level` of stack at these positions and layers. */
std::shared_ptr<Session> sessionAt(const Session& s, std::shared_ptr<const SubdStack> stack, int level, std::span<const glm::dvec3> pos,
                                   const LevelLayers& layers, std::vector<DetailPtr> detail = {})
{
    auto out = std::make_shared<Session>();
    out->node = s.node;
    out->basePoints = s.basePoints;
    out->baseFaces = s.baseFaces;
    out->base = s.base;
    out->stack = std::move(stack);
    out->level = level;
    detail.resize(size_t(out->stack->top()) + 1);
    out->detail = std::move(detail);
    geo::Mesh mesh = out->stack->levels[size_t(level)]->mesh;
    std::ranges::copy(pos, mesh.positions().begin());
    out->sculpt = SculptMesh(std::move(mesh));
    loadLayers(out->sculpt, layers);
    syncSymmetry(*out);
    return out;
}

/**
 * The session sculpting `target` of stack, carrying s's sculpt there: down by least squares, recording the detail each
 * level adds, or up through the detail recorded before.
 */
std::shared_ptr<Session> atLevel(const Session& s, int target, std::shared_ptr<const SubdStack> stack)
{
    LILA_ZONE();

    std::vector<glm::dvec3> pos(s.sculpt.mesh().positions().begin(), s.sculpt.mesh().positions().end());
    LevelLayers layers = layersOf(s.sculpt);
    std::vector<DetailPtr> detail = s.detail;
    detail.resize(size_t(stack->top()) + 1);
    int l = s.level;
    for (; l > target; --l)
        detail[size_t(l)] = stepDown(*stack, l, pos, layers);
    for (; l < target; ++l)
    {
        stepUp(*stack, l + 1, detail[size_t(l) + 1], pos, layers);
        detail[size_t(l) + 1] = nullptr;
    }
    return sessionAt(s, std::move(stack), target, pos, layers, std::move(detail));
}

/** s, or the session at its finest level. */
std::shared_ptr<Session> atTop(const std::shared_ptr<Session>& s)
{
    return s->level == s->stack->top() ? s : atLevel(*s, s->stack->top(), s->stack);
}

json levelJson(const Session& s)
{
    return {{"level", s.level}, {"levels", s.stack->top()}, {"points", s.sculpt.mesh().numPoints()}, {"faces", s.sculpt.mesh().numFaces()}};
}

/** Level `level`'s positions: stepped down from the sculpted level by least squares, or up through the detail. */
std::vector<glm::dvec3> positionsAt(const Session& s, int level)
{
    std::vector<glm::dvec3> pos(s.sculpt.mesh().positions().begin(), s.sculpt.mesh().positions().end());
    int l = s.level;
    for (; l > level; --l)
    {
        LevelLayers none;
        stepDown(*s.stack, l, pos, none);
    }
    for (; l < level; ++l)
    {
        LevelLayers none;
        stepUp(*s.stack, l + 1, s.detail[size_t(l) + 1], pos, none);
    }
    return pos;
}

/** Per point of dst, the polypaint of src at the closest point of its surface. */
std::vector<glm::vec4> sampleColors(const SculptMesh& src, const geo::Mesh& dst)
{
    LILA_ZONE();

    const auto& corners = src.bvh().triangles().corners;
    std::vector<glm::vec4> out(dst.pointCapacity(), glm::vec4(1.0f));
    tbb::parallel_for(Index(0), dst.pointCapacity(), [&](Index p) {
        geo::BvhClosest hit;
        if (!dst.pointValid(p) || !src.bvh().closestPoint(dst.position(p), hit))
            return;
        glm::vec4 c(0.0f);
        for (int k = 0; k < 3; ++k)
            c += src.colors()[src.mesh().cornerPoint(corners[size_t(hit.triangle) * 3 + k])] * float(hit.bary[k]);
        out[p] = c;
    });
    return out;
}

/** The sculpt as a mesh to leave in the scene: its polypaint on the polypaint color set. */
geo::Mesh withPolypaint(geo::Mesh m, const SculptMesh& sm)
{
    if (sm.painted())
    {
        colors::add(m, kColorSet);
        colors::setFromPoints(m, kColorSet, sm.colors());
        colors::settle(m, kColorSet);
    }
    return m;
}

/** The finest level to store, with its mask and freeze state as point layers. */
MeshPtr finestToStore(const Session& s)
{
    auto m = std::make_shared<geo::Mesh>(withPolypaint(s.sculpt.mesh(), s.sculpt));
    auto mask = m->pointAttrs().getOrAdd<float>(kMaskLayer);
    auto freeze = m->pointAttrs().getOrAdd<float>(kFreezeLayer);
    for (Index p = 0; p < m->pointCapacity(); ++p)
    {
        mask[p] = s.sculpt.mask()[p];
        freeze[p] = s.sculpt.frozen()[p] ? 1.0f : 0.0f;
    }
    return m;
}

/** Tells an exit's positions apart from later edits: FNV-1a over the point positions. */
int64_t positionStamp(const geo::Mesh& m)
{
    uint64_t h = 1469598103934665603ull;
    for (const glm::dvec3& p : m.positions())
        for (double v : {p.x, p.y, p.z})
        {
            uint64_t bits;
            std::memcpy(&bits, &v, sizeof bits);
            h = (h ^ bits) * 1099511628211ull;
        }
    return int64_t(h);
}

/** The mesh's stored levels: its cage, finest level, and the counts of the level it is at. */
struct Stored
{
    MeshPtr cage, finest;
    int level = 0, levels = 0;
    int64_t points = -1, faces = -1, stamp = 0;
};

/** Levels between a cage and its subdivision: the first splits each face into one quad per corner, each after it quadruples. */
int levelsBetween(const geo::Mesh& cage, const geo::Mesh& finest)
{
    if (cage.numFaces() > 0)
        for (Index faces = cage.numCorners(), l = 1; l <= kMaxLevels; ++l, faces *= 4)
            if (faces == finest.numFaces())
                return int(l);
    return 0;
}

std::optional<Stored> stored(const Node& n)
{
    if (!n.type().isA("mesh"))
        return std::nullopt;
    Stored s;
    s.cage = meshAttr(n, kCageAttr);
    s.finest = meshAttr(n, kFinestAttr);
    if (!s.cage || !s.finest || (s.levels = levelsBetween(*s.cage, *s.finest)) == 0)
        return std::nullopt;
    s.level = int(std::clamp<int64_t>(intAttr(n, kLevelAttr, 0), 0, s.levels));
    s.points = intAttr(n, kPointsAttr, -1);
    s.faces = intAttr(n, kFacesAttr, -1);
    s.stamp = intAttr(n, kStampAttr, 0);
    return s;
}

bool fits(const Stored& s, const geo::Mesh& base) { return base.numPoints() == s.points && base.numFaces() == s.faces; }

void clearStored(Node& n)
{
    Graph& g = graph();
    for (const char* name : {kStampAttr, kFacesAttr, kPointsAttr, kLevelAttr, kFinestAttr, kCageAttr})
        if (const int i = n.type().attrIndex(name); i >= 0)
            g.deleteAttr({n.id(), i, -1});
}

void store(Node& n, const std::shared_ptr<Session>& s, const geo::Mesh& written)
{
    Graph& g = graph();
    const NodeId id = n.id();
    g.setValue({id, levelAttr(n, kCageAttr, scene::PlugType::Mesh, MeshPtr(), "Sculpt level 0."), -1},
               MeshPtr(std::make_shared<geo::Mesh>(s->stack->levels.front()->mesh)));
    g.setValue({id, levelAttr(n, kFinestAttr, scene::PlugType::Mesh, MeshPtr(), "The finest sculpt level."), -1}, finestToStore(*atTop(s)));
    g.setValue({id, levelAttr(n, kLevelAttr, scene::PlugType::Int, int64_t(0), "The sculpt level the mesh is at."), -1}, int64_t(s->level));
    g.setValue({id, levelAttr(n, kPointsAttr, scene::PlugType::Int, int64_t(-1), "The points at that level."), -1}, int64_t(written.numPoints()));
    g.setValue({id, levelAttr(n, kFacesAttr, scene::PlugType::Int, int64_t(-1), "The faces at that level."), -1}, int64_t(written.numFaces()));
    g.setValue({id, levelAttr(n, kStampAttr, scene::PlugType::Int, int64_t(0), "The level's positions at exit."), -1}, positionStamp(written));
}

/** The session Enter Sculpt opens on n: its stored levels when they still fit its topology, else its mesh at level 0. */
std::shared_ptr<Session> openSession(Node& n, int startLevels)
{
    LILA_ZONE();

    Graph& g = graph();
    g.evaluate();
    MeshPtr base = scene::modifiers::cage(g, n.id());
    if (!base || base->numFaces() == 0)
        throw CommandError("sculptEnter: " + n.path() + " has no faces");
    auto s = std::make_shared<Session>();
    s->node = n.id();
    s->basePoints = base->pointCapacity();
    s->baseFaces = base->numFaces();

    if (std::optional<Stored> st = stored(n))
    {
        geo::SubdivideOptions o;
        o.levels = st->levels;
        std::shared_ptr<const SubdStack> stack = fits(*st, *base) ? buildSubdStack(*st->cage, o) : nullptr;
        const geo::Mesh* at = stack ? &stack->levels[size_t(st->level)]->mesh : nullptr;
        if (at && stack->levels.back()->mesh.pointCapacity() == st->finest->pointCapacity() && at->pointCapacity() == base->pointCapacity())
        {
            s->base = stack->levels[size_t(st->level)];
            const auto& finest = st->finest->positions();
            s = atLevel(*sessionAt(*s, stack, stack->top(), finest, layersFrom(*st->finest)), st->level, stack);
            // Points moved outside sculpt mode: the finer levels' detail rides on the moved shape.
            if (positionStamp(*base) != st->stamp)
                s = sessionAt(*s, stack, st->level, base->positions(), layersOf(s->sculpt), s->detail);
            return s;
        }
        log::warn("sculptEnter: {} changed topology since its {} sculpt levels were stored; they no longer fit and were removed",
                  n.path(), st->levels);
        clearStored(n);
    }

    geo::SubdivideOptions o;
    o.levels = 0;
    s->stack = buildSubdStack(*base, o);
    s->base = s->stack->levels.front();
    LevelLayers layers;
    if (colors::has(*base, kColorSet))
        layers.color = colors::pointColors(*base, kColorSet);
    s = sessionAt(*s, s->stack, 0, base->positions(), layers);
    for (int i = 0; i < std::clamp(startLevels, 0, kMaxLevels); ++i)
    {
        std::shared_ptr<const SubdStack> stack = addSubdLevel(*s->stack);
        s = atLevel(*s, stack->top(), stack);
    }
    return s;
}

/** Writes the sculpted level into the mesh and stores the levels on it. Returns the mesh as written. */
geo::Mesh writeBack(const Session& s, Node& n)
{
    Graph& g = graph();
    PlugRef plug = scene::modifiers::editPlug(g, n.id());
    geo::Mesh mesh = storedMesh(plug);
    const geo::Mesh& at = s.sculpt.mesh();
    // The level the mesh was at keeps the mesh itself (its UVs and any edits since), its points moved.
    if (s.base == s.stack->levels[size_t(s.level)] && mesh.pointCapacity() == at.pointCapacity() && mesh.numFaces() == at.numFaces())
    {
        for (Index p = 0; p < mesh.pointCapacity(); ++p)
            if (mesh.pointValid(p))
                mesh.setPosition(p, at.position(p));
        mesh = withPolypaint(std::move(mesh), s.sculpt);
    }
    else
        mesh = withPolypaint(at, s.sculpt);
    g.setValue(plug, MeshPtr(std::make_shared<geo::Mesh>(mesh)));
    return mesh;
}

/** Records the switch to `to` as an undo step and makes it the open session. */
json switchTo(const std::shared_ptr<Session>& from, std::shared_ptr<Session> to, const char* undoName)
{
    UndoChunk chunk(undoName);
    UndoStack::get().record(std::make_unique<SwitchAction>(from, to));
    setActive(to);
    return levelJson(*to);
}

/** Meshes whose stored levels a top-level command left not fitting their topology. */
struct LevelsGuard
{
    std::function<void(const std::vector<NodeId>&, std::string_view)> handler;
    bool watching = false;
    bool touched = false;
    bool dirty = true;
    std::vector<NodeId> leveled;
};

LevelsGuard& guard()
{
    static LevelsGuard g;
    return g;
}

void checkLevels(std::string_view command)
{
    LevelsGuard& lg = guard();
    Graph& g = graph();
    if (lg.dirty)
    {
        lg.leveled.clear();
        for (NodeId id : g.nodes())
            if (Node* n = g.node(id); n && n->type().isA("mesh") && n->type().attrIndex(kFinestAttr) >= 0)
                lg.leveled.push_back(id);
        lg.dirty = false;
    }
    std::vector<NodeId> broken;
    const std::shared_ptr<Session> open = activeRef();
    for (NodeId id : lg.leveled)
        if (Node* n = g.node(id); n && !(open && open->node == id))
            if (std::optional<Stored> st = stored(*n))
                if (MeshPtr base = scene::modifiers::cage(g, id); base && !fits(*st, *base))
                    broken.push_back(id);
    if (!broken.empty())
    {
        for (NodeId id : broken)
            log::warn("{}: {} changed its topology, so its sculpt levels no longer fit", g.node(id)->path(), command);
        if (lg.handler)
            lg.handler(broken, command);
    }
}
} // namespace

std::shared_ptr<Session> active() { return activeRef(); }

std::optional<StoredLevels> storedLevels(const scene::Node& mesh)
{
    std::optional<Stored> st = stored(mesh);
    if (!st)
        return std::nullopt;
    MeshPtr base = scene::modifiers::cage(graph(), mesh.id());
    return StoredLevels{.level = st->level, .levels = st->levels, .fits = base && fits(*st, *base)};
}

void setLevelsBrokenHandler(std::function<void(const std::vector<scene::NodeId>&, std::string_view)> fn) { guard().handler = std::move(fn); }

int addListener(std::function<void()> fn)
{
    static int next = 1;
    listeners()[next] = std::move(fn);
    return next++;
}

void removeListener(int id) { listeners().erase(id); }

void notify()
{
    auto copy = listeners();
    for (auto& [id, fn] : copy)
        fn();
}

void syncSymmetry(Session& s)
{
    Node* n = graph().node(s.node);
    const std::optional<geo::SymmetrySpec> spec = n ? scene::symmetrySpec(*n) : std::nullopt;
    const std::string key = !spec ? std::string() : spec->radial ? geo::radialLayerName(*spec->radial) : geo::symmetryLayerName(spec->axis);
    if (key == s.symmetryKey)
        return;
    s.symmetryKey = key;
    s.sculpt.setSymmetry(spec, symmetryTolerance(s.sculpt.mesh()));
}

uint64_t nextLiveToken() { return gLiveToken.fetch_add(1); }

void setPivotHandler(std::function<void(const std::optional<glm::dvec3>&)> handler) { pivotHandler() = std::move(handler); }

void registerCommands()
{
    using T = ArgType;

    graph().addListener([](scene::Change c, NodeId) {
        if (c == scene::Change::Cleared && activeRef())
            setActive(nullptr);
        LevelsGuard& lg = guard();
        if (c == scene::Change::Value && lg.watching)
            lg.touched = true;
        if (c == scene::Change::Created || c == scene::Change::Deleted || c == scene::Change::Cleared || c == scene::Change::Reset ||
            c == scene::Change::Attrs)
            lg.dirty = true;
    }, true);

    ops::addExecuteListener([](const ExecuteEvent& e) {
        LevelsGuard& lg = guard();
        if (!e.done)
        {
            lg.watching = true;
            lg.touched = false;
            return;
        }
        lg.watching = false;
        // Sculpt commands keep the stored levels in step themselves; undo and redo restore a state where they fit.
        if (e.ok && lg.touched && !e.name.starts_with("sculpt") && e.name != "undo" && e.name != "redo")
            checkLevels(e.name);
    });

    ops::registerCommand({
        .name = "sculptEnter",
        .doc = "Enter sculpt mode on a mesh: strokes sculpt the mesh itself. Levels stored by an earlier sculptExit come back, "
               "at the level the mesh is at; edits made to the mesh since that keep its topology carry into them. levels adds "
               "that many subdivision levels to a mesh without any. Returns {mesh, level, levels, points, faces}.",
        .positionalDoc = "mesh",
        .flags = {{"levels", "", T::Int, 0, "Subdivision levels to add to a mesh without stored levels, 0 to 6.", {}, {}, 0.0, double(kMaxLevels)}},
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("sculptEnter: select a mesh");
            Node& n = meshArg(objs.front(), "sculptEnter");
            std::shared_ptr<Session> prev = activeRef();
            if (prev && prev->node == n.id())
            {
                json out = levelJson(*prev);
                out["mesh"] = n.path();
                return out;
            }
            UndoChunk chunk("Enter Sculpt");
            if (prev)
                ops::execute("sculptExit", json::object());
            std::shared_ptr<Session> s = openSession(n, a.get<int>("levels"));
            UndoStack::get().record(std::make_unique<SwitchAction>(activeRef(), s));
            setActive(s);
            log::info("sculptEnter {}: level {} of {}, {} points, {} faces", n.path(), s->level, s->stack->top(), s->sculpt.mesh().numPoints(),
                      s->sculpt.mesh().numFaces());
            json out = levelJson(*s);
            out["mesh"] = n.path();
            return out;
        },
    });

    ops::registerCommand({
        .name = "sculptExit",
        .doc = "Leave sculpt mode. The mesh stays at the level being sculpted, and every level is stored on it for the next "
               "sculptEnter (sculptDeleteLevels removes them). Polypaint lands on the mesh's polypaint color set. Returns "
               "{mesh, level, levels}.",
        .run = [](const Args&) -> json {
            std::shared_ptr<Session> s = requireSession("sculptExit");
            Node& n = *graph().node(s->node);
            UndoChunk chunk("Exit Sculpt");
            const geo::Mesh written = writeBack(*s, n);
            if (s->stack->top() > 0)
                store(n, s, written);
            else
                clearStored(n);
            UndoStack::get().record(std::make_unique<SwitchAction>(s, nullptr));
            setActive(nullptr);
            return {{"mesh", n.path()}, {"level", s->level}, {"levels", s->stack->top()}};
        },
    });

    ops::registerCommand({
        .name = "sculptCancel",
        .doc = "Leave sculpt mode without changing the mesh or its stored levels, discarding this session's sculpting.",
        .run = [](const Args&) -> json {
            std::shared_ptr<Session> s = activeRef();
            if (!s)
                return nullptr;
            UndoChunk chunk("Cancel Sculpt");
            UndoStack::get().record(std::make_unique<SwitchAction>(s, nullptr));
            setActive(nullptr);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "sculptDuplicate",
        .doc = "Copy the mesh at a sculpt level as a new mesh beside it in the outliner, with its materials, transform and "
               "modifiers but no levels of its own. Without hide, the copy is selected and sculpt mode ends; with hide, the "
               "copy is hidden and sculpting carries on. Returns {mesh, level}.",
        .flags = {{"level", "", T::Int, -1, "The level to copy; -1 for the one being sculpted.", {}, {}, -1.0, double(kMaxLevels)},
                  {"hide", "", T::Bool, false, "Hide the copy and stay in sculpt mode."},
                  {"name", "", T::String, "", "Name for the copy; default <mesh>_level<N>."}},
        .run = [](const Args& a) -> json {
            std::shared_ptr<Session> s = requireSession("sculptDuplicate");
            const int want = a.get<int>("level");
            const int level = want < 0 ? s->level : std::min(want, s->stack->top());
            Graph& g = graph();
            Node& n = *g.node(s->node);
            std::string name = a.get<std::string>("name", "");
            if (name.empty())
            {
                // A suffix after the level number, so a second copy of level 1 isn't named as level 2.
                const std::string base = n.name() + "_level" + std::to_string(level);
                name = base;
                for (int i = 2; g.find(name); ++i)
                    name = base + "_" + std::to_string(i);
            }
            const std::shared_ptr<Session> at = level == s->level ? s : atLevel(*s, level, s->stack);
            UndoChunk chunk("Duplicate Sculpt Level");
            json made = ops::execute("duplicate", {{"args", {n.path()}}, {"name", name}});
            const std::string path = made.is_array() && !made.empty() ? made[0].get<std::string>() : std::string();
            Node* copy = g.find(path);
            if (!copy)
                throw CommandError("sculptDuplicate: duplicating " + n.path() + " failed");
            const NodeId copyId = copy->id();
            for (NodeId c : std::vector<NodeId>(g.children(copyId)))
                g.deleteNode(c);
            clearStored(*copy);
            g.setValue(scene::modifiers::editPlug(g, copyId), MeshPtr(std::make_shared<geo::Mesh>(withPolypaint(at->sculpt.mesh(), at->sculpt))));
            const NodeId parent = g.parent(n.id());
            const std::vector<NodeId>& siblings = parent == scene::kNoNode ? g.roots() : g.children(parent);
            const size_t index = size_t(std::ranges::find(siblings, n.id()) - siblings.begin());
            g.setParent(copyId, parent, false, index + 1);
            if (a.get<bool>("hide"))
                g.setValue({copyId, Transform::kVisibility, -1}, false);
            else
            {
                ops::execute("sculptExit", json::object());
                g.select({copyId});
            }
            return {{"mesh", g.node(copyId)->path()}, {"level", level}};
        },
    });

    ops::registerCommand({
        .name = "sculptStroke",
        .doc = "One brush stroke on the sculpt: dabs is a list of {center, normal, radius, pressure, opacity, flow, view, "
               "delta, direction} in world space (only center is required; opacity defaults to pressure). Honours the mask, "
               "frozen points and the mesh's symmetry, mirrored exactly. Returns {points} changed.",
        .flags = {{"brush", "", T::String, "draw", "draw, clayStrips, inflate, grab, smooth, flatten, pinch, crease, scrape, damStandard, "
                                                     "trimDynamic, polish, fold, mask, relax, freeze, paint, fill, clay, layer, blob, magnify, planar, snakeHook, "
                                                     "elasticGrab, nudge, thumb, rotate, pose, boundary, or a brush an extension registered."},
                  {"paint", "", T::Bool, false, "Shape brushes also paint color where they move points."},
                  {"color", "", T::List, json::array({1.0, 1.0, 1.0}), "Paint color, linear [r, g, b]."},
                  {"falloff", "", T::String, "smooth", "Falloff: smooth, sphere, root, sharp, linear, constant, bell, or a soft select preset name."},
                  {"strength", "", T::Float, 0.5, "0 to 1.", {}, {}, 0.0, 1.0},
                  {"invert", "", T::Bool, false, "Dig, unmask, unfreeze or magnify instead; grab moves only along the surface normal."},
                  {"blur", "", T::Bool, false, "Paint and mask: blur the color or mask under the brush instead of laying it down."},
                  {"keepVolume", "", T::Bool, true, "Smooth: keep the form's volume; off averages points, which also flattens it."},
                  {"pinchDepth", "", T::Float, 1.0, "Pinch: how far it presses the surface in, 0 for none."},
                  {"frontFacesOnly", "", T::Bool, false, "Leave points facing away from the view."},
                  {"radialCount", "", T::Int, 1, "Copies of every dab turned evenly about an object axis; 1 is off.", {}, {}, 1.0, 64.0},
                  {"radialAxis", "", T::String, "y", "The object axis radial copies turn about.", {"x", "y", "z"}},
                  {"dabs", "", T::List, json::array(), "The stamps, in order."},
                  {"live", "", T::Int, 0, "Internal: adopts the stroke the viewport tool already applied."}},
        .run = [](const Args& a) -> json {
            std::shared_ptr<Session> s = requireSession("sculptStroke");
            StrokeRecord rec;
            uint64_t token = uint64_t(a.get<int64_t>("live"));
            if (token != 0 && token == s->liveToken)
            {
                rec = std::move(s->live);
                s->live = {};
                s->liveToken = 0;
            }
            else
            {
                syncSymmetry(*s);
                std::vector<Dab> dabs = dabsFrom(a.flags.value("dabs", json::array()), worldOf(*graph().node(s->node)));
                s->sculpt.beginStroke(settingsFrom(a));
                for (const Dab& d : dabs)
                    s->sculpt.dab(d);
                rec = s->sculpt.endStroke();
            }
            size_t count = rec.points.size();
            if (!rec.empty())
            {
                UndoChunk chunk("Sculpt Stroke");
                UndoStack::get().record(std::make_unique<StrokeAction>(s, std::move(rec)));
            }
            notify();
            return {{"points", count}};
        },
    });

    ops::registerCommand({
        .name = "sculptMask",
        .doc = "Change the sculpt mask (masked points resist every brush) or the freeze layer (frozen points never move): "
               "clear, fill, invert, or blur (mask only).",
        .flags = {{"op", "", T::String, "clear", "clear, fill, invert or blur.", {"clear", "fill", "invert", "blur"}},
                  {"layer", "", T::String, "mask", "mask or freeze.", {"mask", "freeze"}},
                  {"iterations", "", T::Int, 2, "Blur passes.", {}, {}, 1.0, 100.0}},
        .run = [](const Args& a) -> json {
            std::shared_ptr<Session> s = requireSession("sculptMask");
            std::string op = a.get<std::string>("op", "clear");
            MaskOp m = op == "fill" ? MaskOp::Fill : op == "invert" ? MaskOp::Invert : op == "blur" ? MaskOp::Blur : MaskOp::Clear;
            if (op != "clear" && op != "fill" && op != "invert" && op != "blur")
                throw CommandError("sculptMask: op must be clear, fill, invert or blur");
            Layer layer = a.get<std::string>("layer", "mask") == "freeze" ? Layer::Freeze : Layer::Mask;
            StrokeRecord rec = s->sculpt.maskOp(m, layer, std::clamp(a.get<int>("iterations"), 1, 100));
            size_t count = rec.points.size();
            if (!rec.empty())
            {
                UndoChunk chunk("Sculpt Mask");
                UndoStack::get().record(std::make_unique<StrokeAction>(s, std::move(rec)));
            }
            notify();
            return {{"points", count}};
        },
    });

    ops::registerCommand({
        .name = "sculptColor",
        .doc = "Fill the sculpt's polypaint with a color (masked points keep theirs) or clear it back to white.",
        .flags = {{"op", "", T::String, "fill", "fill or clear.", {"fill", "clear"}},
                  {"color", "", T::List, json::array({1.0, 1.0, 1.0}), "Fill color, linear [r, g, b]."}},
        .run = [](const Args& a) -> json {
            std::shared_ptr<Session> s = requireSession("sculptColor");
            const bool clear = a.get<std::string>("op", "fill") == "clear";
            StrokeRecord rec = clear ? s->sculpt.clearColor() : s->sculpt.fillColor(colorArg(a, "sculptColor"));
            size_t count = rec.points.size();
            if (!rec.empty())
            {
                UndoChunk chunk(clear ? "Clear Polypaint" : "Fill Polypaint");
                UndoStack::get().record(std::make_unique<StrokeAction>(s, std::move(rec)));
            }
            notify();
            return {{"points", count}};
        },
    });

    ops::registerCommand({
        .name = "sculptLevel",
        .doc = "Sculpt another subdivision level: 0 is the cage, the highest the finest. Going down keeps the finer detail, "
               "and going back up lays it over whatever changed below. Returns {level, levels, points, faces}.",
        .flags = {{"level", "", T::Int, 0, "The level to sculpt.", {}, {}, 0.0, double(kMaxLevels)},
                  {"step", "", T::Int, 0, "Move this many levels from the current one instead (1 up, -1 down)."}},
        .run = [](const Args& a) -> json {
            std::shared_ptr<Session> s = requireSession("sculptLevel");
            const int step = a.get<int>("step");
            const int target = std::clamp(step != 0 ? s->level + step : a.get<int>("level"), 0, s->stack->top());
            if (target == s->level)
                return levelJson(*s);
            return switchTo(s, atLevel(*s, target, s->stack), "Sculpt Level");
        },
    });

    ops::registerCommand({
        .name = "sculptAddLevel",
        .doc = "Subdivide the sculpt once more and sculpt the new level (from a lower level, the finest is subdivided). "
               "Returns {level, levels, points, faces}.",
        .run = [](const Args&) -> json {
            std::shared_ptr<Session> s = requireSession("sculptAddLevel");
            if (s->stack->top() >= kMaxLevels)
                throw CommandError("sculptAddLevel: already at the most levels, " + std::to_string(kMaxLevels));
            std::shared_ptr<const SubdStack> stack = addSubdLevel(*s->stack);
            return switchTo(s, atLevel(*s, stack->top(), stack), "Add Sculpt Level");
        },
    });

    ops::registerCommand({
        .name = "sculptDeleteHigher",
        .doc = "Delete the levels above the one being sculpted, and their detail. Returns {level, levels, points, faces}.",
        .run = [](const Args&) -> json {
            std::shared_ptr<Session> s = requireSession("sculptDeleteHigher");
            if (s->level == s->stack->top())
                return levelJson(*s);
            std::shared_ptr<const SubdStack> stack = truncateSubdStack(*s->stack, s->level);
            return switchTo(s, atLevel(*s, s->level, stack), "Delete Higher Levels");
        },
    });

    ops::registerCommand({
        .name = "sculptDeleteLower",
        .doc = "Delete the levels below the one being sculpted: it becomes level 0, the mesh's new topology on exit. The "
               "levels above keep their detail. Returns {level, levels, points, faces}.",
        .run = [](const Args&) -> json {
            std::shared_ptr<Session> s = requireSession("sculptDeleteLower");
            if (s->level == 0)
                return levelJson(*s);
            const std::shared_ptr<Session> top = atTop(s);
            std::shared_ptr<const SubdStack> stack = dropSubdLevelsBelow(*s->stack, s->level);
            std::shared_ptr<Session> to = sessionAt(*s, stack, stack->top(), top->sculpt.mesh().positions(), layersOf(top->sculpt));
            return switchTo(s, atLevel(*to, 0, stack), "Delete Lower Levels");
        },
    });

    ops::registerCommand({
        .name = "sculptReconstruct",
        .doc = "Rebuild a level below level 0 from a mesh that was subdivided elsewhere: the new cage is the mesh before "
               "that subdivision, its UVs and materials taken from the fine mesh. Level 0 must be all quads laid out the way "
               "a subdivision lays them out. Sculpting carries on at the same level, one number higher. Returns {level, "
               "levels, points, faces}.",
        .run = [](const Args&) -> json {
            std::shared_ptr<Session> s = requireSession("sculptReconstruct");
            Node& n = *graph().node(s->node);
            const int oldTop = s->stack->top();
            if (oldTop >= kMaxLevels)
                throw CommandError("sculptReconstruct: already at the most levels, " + std::to_string(kMaxLevels));
            geo::Mesh fine = s->stack->levels.front()->mesh;
            {
                const std::vector<glm::dvec3> pos = positionsAt(*s, 0);
                std::ranges::copy(pos, fine.positions().begin());
            }
            std::vector<Index> coarseOf;
            std::optional<geo::Mesh> cage = reconstructCage(fine, coarseOf);
            if (!cage)
                throw CommandError("sculptReconstruct: level 0 of " + n.path() + " isn't a subdivided mesh: it needs all quads, "
                                   "laid out the way a subdivision lays them out");
            geo::SubdivideOptions o = s->stack->opts;
            o.levels = oldTop + 1;
            std::shared_ptr<const SubdStack> stack = buildSubdStack(*cage, o);
            // Each old level k is new level k + 1, in the new level's point order.
            std::vector<Index> perm = matchSubdivisions(fine, coarseOf, stack->levels[1]->mesh, coarsePoints(*stack->levels[1]));
            for (int k = 1; k <= oldTop && !perm.empty(); ++k)
            {
                std::vector<Index> coarse = coarsePoints(*s->stack->levels[size_t(k)]);
                for (Index& c : coarse)
                    if (c != geo::kInvalid)
                        c = perm[c];
                perm = matchSubdivisions(s->stack->levels[size_t(k)]->mesh, coarse, stack->levels[size_t(k) + 1]->mesh,
                                         coarsePoints(*stack->levels[size_t(k) + 1]));
            }
            if (perm.empty())
                throw CommandError("sculptReconstruct: " + n.path() + "'s levels don't line up with the rebuilt cage's subdivision");
            const std::shared_ptr<Session> top = atTop(s);
            const geo::Mesh& finest = top->sculpt.mesh();
            const LevelLayers from = layersOf(top->sculpt);
            const Index count = stack->levels.back()->mesh.pointCapacity();
            std::vector<glm::dvec3> pos(count, glm::dvec3(0.0));
            LevelLayers layers;
            layers.color.resize(from.color.empty() ? 0 : count, glm::vec4(1.0f));
            layers.mask.resize(from.mask.empty() ? 0 : count, 0.0f);
            layers.freeze.resize(from.freeze.empty() ? 0 : count, 0.0f);
            for (Index p = 0; p < finest.pointCapacity(); ++p)
            {
                if (!finest.pointValid(p))
                    continue;
                const Index q = perm[p];
                pos[q] = finest.position(p);
                if (!layers.color.empty())
                    layers.color[q] = from.color[p];
                if (!layers.mask.empty())
                    layers.mask[q] = from.mask[p];
                if (!layers.freeze.empty())
                    layers.freeze[q] = from.freeze[p];
            }
            std::shared_ptr<Session> to = sessionAt(*s, stack, stack->top(), pos, layers);
            return switchTo(s, atLevel(*to, s->level + 1, stack), "Reconstruct Sculpt Level");
        },
    });

    ops::registerCommand({
        .name = "sculptRemesh",
        .doc = "Replace the sculpt with an even voxel remesh of its finest level, free of the mesh's topology, as level 0 "
               "with no levels above. Polypaint carries over; UVs, the mask and freeze don't. Returns {level, levels, points, faces}.",
        .flags = {{"resolution", "", T::Int, 192, "Voxels along the longest side.", {}, {}, 16.0, 2048.0}},
        .run = [](const Args& a) -> json {
            std::shared_ptr<Session> s = requireSession("sculptRemesh");
            const std::shared_ptr<Session> top = atTop(s);
            retopo::VoxelOptions vo;
            vo.resolution = std::clamp(a.get<int>("resolution"), 16, 2048);
            geo::Mesh remeshed = retopo::voxelRemesh(top->sculpt.mesh(), vo);
            if (remeshed.numFaces() == 0)
                throw CommandError("sculptRemesh: the remesh came out empty; raise resolution");
            LevelLayers layers;
            if (top->sculpt.painted())
                layers.color = sampleColors(top->sculpt, remeshed);
            geo::SubdivideOptions o = s->stack->opts;
            o.levels = 0;
            std::shared_ptr<const SubdStack> stack = buildSubdStack(remeshed, o);
            return switchTo(s, sessionAt(*s, stack, 0, remeshed.positions(), layers), "Remesh Sculpt");
        },
    });

    ops::registerCommand({
        .name = "sculptDeleteLevels",
        .doc = "Remove the sculpt levels stored on meshes, leaving each at the level it is at. Returns the meshes that had levels.",
        .positionalDoc = "meshes...",
        .run = [](const Args& a) -> json {
            json out = json::array();
            UndoChunk chunk("Delete Sculpt Levels");
            for (const std::string& name : a.objects())
            {
                Node& n = meshArg(name, "sculptDeleteLevels");
                if (std::shared_ptr<Session> s = activeRef(); s && s->node == n.id())
                    throw CommandError("sculptDeleteLevels: " + n.path() + " is being sculpted; use sculptDeleteHigher and sculptDeleteLower");
                if (n.type().attrIndex(kFinestAttr) < 0)
                    continue;
                clearStored(n);
                out.push_back(n.path());
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "sculptState",
        .doc = "The open sculpt session: {active, mesh, level, levels, points, faces, symmetry, masked, frozen, painted}.",
        .undoable = false,
        .run = [](const Args&) -> json {
            std::shared_ptr<Session> s = activeRef();
            Node* n = s ? graph().node(s->node) : nullptr;
            if (!s || !n)
                return {{"active", false}};
            size_t masked = 0, frozen = 0;
            for (float m : s->sculpt.mask())
                masked += m > 0.0f;
            for (uint8_t f : s->sculpt.frozen())
                frozen += f != 0;
            json out = {{"active", true},
                        {"mesh", n->path()},
                        {"symmetry", s->sculpt.symmetric()},
                        {"masked", masked},
                        {"frozen", frozen},
                        {"painted", s->sculpt.painted()}};
            out.update(levelJson(*s));
            return out;
        },
    });

    ops::registerCommand({
        .name = "transferShape",
        .doc = "Move low-poly meshes onto a high-poly one (an imported sculpt): the same cage fit sculptExit uses. The first "
               "mesh is the high poly, the rest are the cages. Only positions change, so UVs and other attributes stay.",
        .positionalDoc = "source targets...",
        .flags = {{"method", "", T::String, "fit", "fit, closest or normal.", {"fit", "closest", "normal"}},
                  {"levels", "", T::Int, 2, "Subdivision levels the fit goes through.", {}, {}, 1.0, 5.0},
                  {"iterations", "", T::Int, 3, "Project-and-fit rounds.", {}, {}, 1.0, 20.0},
                  {"pinBoundary", "", T::Bool, false, "Keep border points where they are."}},
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.size() < 2)
                throw CommandError("transferShape: give the high-poly source, then the low-poly targets");
            Graph& g = graph();
            Node& src = meshArg(objs.front(), "transferShape");
            std::string method = a.get<std::string>("method", "fit");
            if (method != "fit" && method != "closest" && method != "normal")
                throw CommandError("transferShape: method must be fit, closest or normal");
            UndoChunk chunk("Transfer Shape");
            json out = json::object();
            for (size_t i = 1; i < objs.size(); ++i)
            {
                Node& n = meshArg(objs[i], "transferShape");
                if (n.id() == src.id())
                    continue;
                geo::Mesh surface = meshInSpaceOf(src, n);
                if (surface.numFaces() == 0)
                    throw CommandError("transferShape: " + src.path() + " has no faces");
                geo::MeshBvh bvh(surface);
                PlugRef base = scene::modifiers::editPlug(g, n.id());
                geo::Mesh cage = storedMesh(base);
                FitOptions fo;
                fo.method = method == "closest" ? FitMethod::Closest : method == "normal" ? FitMethod::Normal : FitMethod::LeastSquares;
                fo.subdivide.levels = std::clamp(a.get<int>("levels"), 1, 5);
                fo.iterations = std::clamp(a.get<int>("iterations"), 1, 20);
                if (a.get<bool>("pinBoundary"))
                    fo.pinned = retopo::featurePoints(cage, {});
                std::optional<geo::SymmetryMap> map;
                if (map = scene::symmetryMapFor(n, cage); map)
                    fo.symmetry = &*map;
                geo::Mesh fitted = fitCageToSurface(cage, surface, bvh, fo);
                double maxMove = 0.0;
                for (Index p = 0; p < cage.pointCapacity(); ++p)
                    if (cage.pointValid(p))
                        maxMove = std::max(maxMove, glm::length(fitted.position(p) - cage.position(p)));
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(fitted))));
                out[n.path()] = maxMove;
            }
            return out;
        },
    });
}
} // namespace lila::sculpt
