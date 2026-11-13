#include "ui/CommandPreview.h"

#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "ops/Undo.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/LastOp.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/MeshCache.h"
#include "ui/Overlay.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"

#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace lila::ui::commandPreview
{
namespace
{
using scene::Graph;
using scene::NodeId;

/** Per kind, so a remesh preview stays drawable. */
constexpr size_t kMaxEdges = 100000;
/** Opacity of the pass that shows edges just behind the surface. */
constexpr float kBehindAlpha = 0.3f;
/** Positions closer than this (cm) count as the same point when telling kept edges from new ones. */
constexpr double kQuantum = 1e-4;

const glm::vec4 kAddedColor(0.25f, 0.65f, 1.0f, 0.85f);
const glm::vec4 kRemovedColor(0.9f, 0.3f, 0.25f, 0.6f);
const glm::vec4 kTitleColor(0.95f, 0.95f, 0.95f, 1.0f);
const glm::vec4 kValueColor(1.0f, 0.85f, 0.25f, 1.0f);
const glm::vec4 kOptionColor(0.45f, 0.75f, 1.0f, 1.0f);

struct Shot
{
    scene::MeshPtr mesh;
    glm::dmat4 world{1.0};
};
using Shots = std::unordered_map<NodeId, Shot>;

struct Line
{
    QString text;
    glm::vec4 color;
};

/** A world-space edge and the normals of up to two faces on it, zero where there is none. */
struct Edge
{
    glm::vec3 a, b;
    glm::vec3 n0{0.0f}, n1{0.0f};
};

struct Preview
{
    std::vector<Edge> added;
    std::vector<Edge> removed;
    glm::dvec3 anchor{0.0};
    std::vector<Line> lines;
    /** What the changed meshes become, drawn in their place so the result hides what it should. */
    Shots results;
    /** Meshes the command deletes or hides. */
    std::vector<NodeId> gone;
    /** Result points under the cursor and the view's center when the menu opened, which coverage centers on. */
    std::optional<glm::dvec3> cursorSpot, centerSpot;
    /** Diagonal of the changed edges' bounds: what the pull and the fade behind the surface scale with. */
    double size = 1.0;
};

std::optional<Preview> gPreview;
struct Slow
{
    size_t faces = 0;
    QElapsedTimer since;
};
std::unordered_map<std::string, Slow> gSlow;

bool knownSlow(const std::string& command, size_t faces)
{
    auto it = gSlow.find(command);
    if (it == gSlow.end())
        return false;
    const double retryMs = PrefStore::get().value<double>("general", kRetrySecondsPref, kRetrySecondsDefault) * 1000.0;
    // A selection under half the slow one's size gets a try anyway.
    if (double(it->second.since.elapsed()) < retryMs && faces * 2 >= it->second.faces)
        return true;
    gSlow.erase(it);
    return false;
}

struct Resolved
{
    std::string command;
    json call;
    std::string title;
};

std::optional<Resolved> resolve(const json& target)
{
    Resolved r;
    if (target.is_string())
    {
        const Action* a = actions::find(target.get<std::string>());
        if (!a || a->command.empty() || (a->enabled && !a->enabled()))
            return std::nullopt;
        r = {a->command, ops::options::callWith(a->command, a->call), a->label};
    }
    else if (target.is_object() && target.contains("command") && target["command"].is_string())
    {
        r.command = target["command"].get<std::string>();
        r.call = target.value("args", json::object());
        r.title = scene::lastOp::displayName(r.command);
    }
    else
        return std::nullopt;
    const CommandDef* def = ops::find(r.command);
    if (!def || !def->adjustable || !def->undoable)
        return std::nullopt;
    return r;
}

/** Faces of the selected meshes, which are what a command works on, stopping once past limit. */
size_t selectedFaces(Graph& g, size_t limit)
{
    size_t faces = 0;
    for (NodeId id : g.selection())
    {
        const scene::Node* n = g.node(id);
        if (!n || !n->type().isA("mesh"))
            continue;
        if (scene::MeshPtr m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
            for (geo::Index f = 0; f < m->faceCapacity() && faces <= limit; ++f)
                faces += m->faceValid(f) ? 1 : 0;
        if (faces > limit)
            break;
    }
    return faces;
}

Shots snapshot(Graph& g)
{
    Shots shots;
    for (NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!n || !n->type().isA("mesh") || !scene::effectivelyVisible(g, id))
            continue;
        if (scene::MeshPtr m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
            shots[id] = {m, n->as<glm::dmat4>(scene::Transform::kWorldMatrix)};
    }
    return shots;
}

uint64_t pointKey(const glm::dvec3& p)
{
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 3; ++i)
        h = (h ^ uint64_t(std::llround(p[i] / kQuantum))) * 1099511628211ull;
    return h;
}

uint64_t edgeKey(uint64_t a, uint64_t b)
{
    if (a > b)
        std::swap(a, b);
    return a * 0x9E3779B97F4A7C15ull ^ (b + 0x7F4A7C159E3779B9ull + (a << 6) + (a >> 2));
}

/** fn(a, b, n0, n1) per edge, in world space. */
template <class F> void forEachWorldEdge(const Shot& s, F&& fn)
{
    const geo::Mesh& m = *s.mesh;
    const glm::dmat3 normalMatrix = glm::transpose(glm::inverse(glm::dmat3(s.world)));
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e))
            continue;
        glm::dvec3 n[2]{glm::dvec3(0.0), glm::dvec3(0.0)};
        int faces = 0;
        m.forEachEdgeCorner(e, [&](geo::Index c) {
            if (faces < 2)
                n[faces++] = glm::normalize(normalMatrix * m.faceNormal(m.cornerFace(c)));
        });
        fn(glm::dvec3(s.world * glm::dvec4(m.position(m.edgePoint(e, 0)), 1.0)),
           glm::dvec3(s.world * glm::dvec4(m.position(m.edgePoint(e, 1)), 1.0)), n[0], n[1]);
    }
}

std::unordered_set<uint64_t> edgeKeys(const Shot& s)
{
    std::unordered_set<uint64_t> keys;
    keys.reserve(s.mesh->edgeCapacity());
    forEachWorldEdge(s, [&](const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3&, const glm::dvec3&) {
        keys.insert(edgeKey(pointKey(a), pointKey(b)));
    });
    return keys;
}

/** Edges of `from` that `against` lacks. */
void edgesMissing(const Shot& from, const Shot* against, std::vector<Edge>& out)
{
    const std::unordered_set<uint64_t> keys = against ? edgeKeys(*against) : std::unordered_set<uint64_t>{};
    forEachWorldEdge(from, [&](const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& n0, const glm::dvec3& n1) {
        if (out.size() < kMaxEdges && !keys.contains(edgeKey(pointKey(a), pointKey(b))))
            out.push_back({glm::dvec3(a), glm::vec3(b), glm::vec3(n0), glm::vec3(n1)});
    });
}

bool same(const Shot& a, const Shot& b)
{
    return a.mesh == b.mesh && a.world == b.world;
}

void diff(const Shots& before, const Shots& after, Preview& p)
{
    for (const auto& [id, shot] : after)
    {
        auto it = before.find(id);
        if (it != before.end() && same(it->second, shot))
            continue;
        const Shot* old = it != before.end() ? &it->second : nullptr;
        edgesMissing(shot, old, p.added);
        if (old)
            edgesMissing(*old, &shot, p.removed);
        p.results[id] = shot;
    }
    glm::dvec3 lo(std::numeric_limits<double>::max()), hi(std::numeric_limits<double>::lowest());
    for (const auto* edges : {&p.added, &p.removed})
        for (const Edge& e : *edges)
        {
            lo = glm::min(lo, glm::min(glm::dvec3(e.a), glm::dvec3(e.b)));
            hi = glm::max(hi, glm::max(glm::dvec3(e.a), glm::dvec3(e.b)));
        }
    if (hi.x >= lo.x)
        p.size = std::max(glm::distance(lo, hi), 1e-6);
    for (const auto& [id, shot] : before)
        if (!after.contains(id))
        {
            edgesMissing(shot, nullptr, p.removed);
            p.gone.push_back(id);
        }
}

QString targets(Graph& g)
{
    const scene::ComponentMode mode = scene::components::mode();
    if (mode == scene::ComponentMode::Object)
    {
        const std::vector<NodeId>& sel = g.selection();
        if (sel.size() == 1)
            if (const scene::Node* n = g.node(sel[0]))
                return QString::fromStdString(n->name());
        return QStringLiteral("Objects: %1").arg(sel.size());
    }
    size_t count = 0;
    for (const auto& [node, ids] : scene::components::selected(mode))
        count += ids.size();
    const char* noun = mode == scene::ComponentMode::Vertex ? (count == 1 ? "Vertex" : "Vertices")
                       : mode == scene::ComponentMode::Edge ? (count == 1 ? "Edge" : "Edges")
                                                            : (count == 1 ? "Face" : "Faces");
    return QStringLiteral("%1: %2").arg(noun).arg(count);
}

/** The first few scalar settings the call runs with, as the option box labels them. */
void settings(const Resolved& r, std::vector<Line>& lines)
{
    using ops::options::FieldKind;
    int shown = 0;
    for (const ops::options::Field& f : ops::options::fields(r.command))
    {
        if (shown == 3)
            break;
        const json v = r.call.contains(f.flag) ? r.call[f.flag] : f.defaultValue;
        const QString label = QString::fromStdString(f.label);
        if (f.kind == FieldKind::Float && v.is_number())
            lines.push_back({label + ": " + QString::number(v.get<double>(), 'g', 4) + QString::fromStdString(f.suffix), kValueColor});
        else if (f.kind == FieldKind::Int && v.is_number())
            lines.push_back({label + ": " + QString::number(v.get<long long>()) + QString::fromStdString(f.suffix), kValueColor});
        else if (f.kind == FieldKind::Check && v.is_boolean() && v.get<bool>())
            lines.push_back({label, kOptionColor});
        else if (f.kind == FieldKind::Choice && v.is_string())
        {
            const auto it = std::find(f.choices.begin(), f.choices.end(), v.get<std::string>());
            const size_t i = size_t(it - f.choices.begin());
            lines.push_back({label + ": " + QString::fromStdString(i < f.choiceLabels.size() ? f.choiceLabels[i] : v.get<std::string>()), kOptionColor});
        }
        else
            continue;
        ++shown;
    }
}

glm::dvec3 centroid(const std::vector<Edge>& edges)
{
    glm::dvec3 sum(0.0);
    for (const Edge& e : edges)
        sum += glm::dvec3(e.a) + glm::dvec3(e.b);
    return edges.empty() ? sum : sum / double(edges.size() * 2);
}

/** Runs the command and reverts it, keeping only what it did to the meshes. Nothing outside sees the run. */
std::optional<Preview> trial(const Resolved& r)
{
    LILA_ZONE();
    UndoStack& undo = UndoStack::get();
    if (!undo.enabled() || undo.chunkOpen() || undo.replaying())
        return std::nullopt;
    Graph& g = Graph::current();
    g.evaluate();
    const size_t maxFaces = size_t(PrefStore::get().value<double>("general", kMaxFacesPref, kMaxFacesDefault));
    const size_t faces = selectedFaces(g, maxFaces > 0 ? maxFaces : SIZE_MAX);
    if ((maxFaces > 0 && faces > maxFaces) || knownSlow(r.command, faces))
        return std::nullopt;
    const Shots before = snapshot(g);
    const scene::ComponentMode mode = scene::components::mode();
    Preview p;
    p.lines.push_back({QString::fromStdString(r.title) + " - " + targets(g), kTitleColor});
    settings(r, p.lines);

    Shots after;
    bool ok = false;
    QElapsedTimer time;
    time.start();
    {
        struct Previewing
        {
            Previewing() { ops::setPreviewing(true); }
            ~Previewing() { ops::setPreviewing(false); }
        } previewing;
        Graph::Batch batch(g);
        undo.openChunk("Preview " + r.command);
        try
        {
            ops::execute(r.command, r.call, CallSource::Script);
            g.evaluate();
            after = snapshot(g);
            ok = true;
        }
        catch (const std::exception&)
        {
        }
        undo.cancelChunk();
        // Plain mode switches are not undoable, and a command may make one as it finishes.
        if (scene::components::mode() != mode)
            scene::components::setMode(mode);
    }
    if (double(time.elapsed()) > PrefStore::get().value<double>("general", kSlowMsPref, kSlowMsDefault))
    {
        Slow& slow = gSlow[r.command];
        slow.faces = faces;
        slow.since.start();
    }
    if (!ok)
        return std::nullopt;

    diff(before, after, p);
    if (p.added.empty() && p.removed.empty())
        return std::nullopt;
    p.anchor = centroid(p.added.empty() ? p.removed : p.added);
    return p;
}

/** Puts the results in the frame in place of the meshes they replace, so they hide the edges they should. */
void swapResults(render::FrameState& s)
{
    std::unordered_set<NodeId> placed;
    for (render::MeshDraw& d : s.meshes.edit())
    {
        auto it = gPreview->results.find(NodeId(d.node));
        if (it == gPreview->results.end())
            continue;
        if (auto rm = renderMeshFor(it->second.mesh))
        {
            d.mesh = std::move(rm);
            d.world = it->second.world;
            d.instances.reset();
        }
        placed.insert(it->first);
    }
    std::erase_if(s.meshes.edit(), [](const render::MeshDraw& d) {
        return std::find(gPreview->gone.begin(), gPreview->gone.end(), NodeId(d.node)) != gPreview->gone.end();
    });
    for (const auto& [id, shot] : gPreview->results)
        if (!placed.contains(id))
            if (auto rm = renderMeshFor(shot.mesh))
            {
                render::MeshDraw d;
                d.mesh = std::move(rm);
                d.world = shot.world;
                d.node = uint64_t(id);
                s.meshes.push_back(std::move(d));
            }
}

/**
 * Edges are depth tested against the result, and fade out as their faces turn away from the camera, so the ones near
 * the silhouette go before the surface does. 0 for an edge on back faces only.
 */
float facing(const Edge& e, const ViewInfo& view, float fadeEnd)
{
    if (e.n0 == glm::vec3(0.0f))
        return 1.0f;
    const glm::dvec3 toEye = -view.viewDir((glm::dvec3(e.a) + glm::dvec3(e.b)) * 0.5);
    double d = glm::dot(glm::dvec3(e.n0), toEye);
    if (e.n1 != glm::vec3(0.0f))
        d = std::max(d, glm::dot(glm::dvec3(e.n1), toEye));
    return fadeEnd > 0.0f ? float(glm::smoothstep(0.0, double(fadeEnd), d)) : (d > 0.0 ? 1.0f : 0.0f);
}

/** Pulls p toward the camera by distance, so a line on the surface wins the depth test by that much. */
glm::vec3 pulled(const glm::dvec3& p, const ViewInfo& view, double distance)
{
    return glm::vec3(p - view.viewDir(p) * distance);
}

void pushLine(std::vector<render::LineVertex>& out, const ViewInfo& view, const Edge& e, glm::vec4 color, float fadeEnd, double pull)
{
    color.a *= facing(e, view, fadeEnd);
    if (color.a <= 0.0f)
        return;
    out.push_back({pulled(e.a, view, pull), color});
    out.push_back({pulled(e.b, view, pull), color});
}

/** The middle of the front-facing edge nearest the focus pixel in its view. */
std::optional<glm::dvec3> focusPoint(const std::vector<Edge>& edges, const Focus& f)
{
    std::optional<glm::dvec3> best;
    double bestPx = std::numeric_limits<double>::max();
    for (const Edge& e : edges)
    {
        if (facing(e, f.view, 0.0f) <= 0.0f)
            continue;
        const glm::dvec3 mid = (glm::dvec3(e.a) + glm::dvec3(e.b)) * 0.5;
        if (const std::optional<QPointF> at = f.view.project(mid))
            if (const double d = std::hypot(at->x() - f.px.x(), at->y() - f.px.y()); d < bestPx)
            {
                bestPx = d;
                best = mid;
            }
    }
    return best;
}

struct Spot
{
    glm::dvec3 at;
    double radius; // world units
};

/** The spots coverage shows the result around in this view; empty shows all of it. */
std::vector<Spot> spotsFor(const std::string& coverage, const ViewInfo& view, double radiusPx)
{
    std::vector<Spot> spots;
    auto add = [&](const std::optional<glm::dvec3>& at) {
        if (at)
            spots.push_back({*at, radiusPx * view.unitsPerPixel(*at)});
    };
    if (coverage == "cursor" || coverage == "both")
        add(gPreview->cursorSpot);
    if (coverage == "center" || coverage == "both")
        add(gPreview->centerSpot);
    return spots;
}

/** Opacity at p: full inside a spot, fading out over its outer 40%. */
float spotlight(const std::vector<Spot>& spots, const glm::dvec3& p)
{
    float alpha = 0.0f;
    for (const Spot& s : spots)
        alpha = std::max(alpha, 1.0f - float(glm::smoothstep(s.radius * 0.6, s.radius, glm::distance(p, s.at))));
    return alpha;
}

void draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view)
{
    if (!gPreview)
        return;
    LILA_ZONE();
    PrefStore& prefs = PrefStore::get();
    const std::string coverage = prefs.value<std::string>("general", kCoveragePref, kCoverageDefault);
    // Within a radius the original stays outside it, so the result mesh only replaces it when asked.
    if (coverage == "all" || prefs.value<bool>("general", kResultMeshPref, false))
        swapResults(s);
    const float fadeEnd = float(prefs.value<double>("general", kEdgeFadePref, kEdgeFadeDefault));
    const double pull = prefs.value<double>("general", kPullPref, kPullDefault) * 0.01 * gPreview->size;
    const double behind = prefs.value<double>("general", kBehindPref, kBehindDefault) * 0.01 * gPreview->size;
    // Removed edges only say something when nothing replaces them (a delete or collapse).
    const std::vector<Edge>& edges = gPreview->added.empty() ? gPreview->removed : gPreview->added;
    const glm::vec4 color = gPreview->added.empty() ? kRemovedColor : kAddedColor;
    const std::vector<Spot> spots = spotsFor(coverage, view, prefs.value<double>("general", kRadiusPxPref, kRadiusPxDefault));
    for (const Edge& e : edges)
    {
        glm::vec4 c = color;
        if (!spots.empty())
        {
            c.a *= spotlight(spots, (glm::dvec3(e.a) + glm::dvec3(e.b)) * 0.5);
            if (c.a <= 0.0f)
                continue;
        }
        // A faint pass pulled further shows edges just behind the surface, so they fade out instead of cutting off.
        if (behind > 0.0)
            pushLine(s.linesOnTop, view, e, c * glm::vec4(1.0f, 1.0f, 1.0f, kBehindAlpha), fadeEnd, pull + behind);
        pushLine(s.linesOnTop, view, e, c, fadeEnd, pull);
    }

    const std::optional<QPointF> at = view.project(gPreview->cursorSpot.value_or(gPreview->centerSpot.value_or(gPreview->anchor)));
    if (!at)
        return;
    QPointF line = *at + QPointF(18.0, -6.0);
    p.setShadow(true);
    for (const Line& l : gPreview->lines)
    {
        p.text(line, l.text, l.color);
        line.ry() += 15.0;
    }
    p.setShadow(false);
}
} // namespace

bool show(const json& target, const std::optional<Focus>& focus)
{
    gPreview.reset();
    if (PrefStore::get().value<bool>("general", kEnabledPref, true))
        if (std::optional<Resolved> r = resolve(target))
            gPreview = trial(*r);
    if (gPreview && focus)
    {
        const std::vector<Edge>& edges = gPreview->added.empty() ? gPreview->removed : gPreview->added;
        gPreview->cursorSpot = focusPoint(edges, *focus);
        gPreview->centerSpot = focusPoint(edges, {focus->view, QPointF(focus->view.width * 0.5, focus->view.height * 0.5)});
    }
    viewportTools::requestRedraw();
    return gPreview.has_value();
}

void clear()
{
    if (!gPreview)
        return;
    gPreview.reset();
    viewportTools::requestRedraw();
}

bool showing()
{
    return gPreview.has_value();
}

void install()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    viewportHooks::addDrawer("lila.commandPreview", draw);
}
} // namespace lila::ui::commandPreview
