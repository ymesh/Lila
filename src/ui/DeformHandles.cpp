#include "ui/DeformHandles.h"

// Before Qt: Qt's emit macro breaks TBB's headers.
#include <tbb/parallel_for.h>

#include "core/PrefStore.h"
#include "core/Profile.h"
#include "deform/Deform.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/DeformWeights.h"
#include "scene/Deformers.h"
#include "scene/Fields.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "ui/Actions.h"
#include "ui/DrawOverrides.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportToolMath.h"
#include "ui/WeightPaintTool.h"
#include "scene/SceneIndex.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <unordered_map>

namespace lila::ui
{
namespace
{
using scene::Graph;
using scene::kNoNode;
using scene::Node;
using scene::NodeId;

constexpr const char* kHeatmapPref = "/viewport/deformHeatmap";

using Segments = std::vector<std::pair<glm::dvec3, glm::dvec3>>;

/** A circle of radius r around the origin in the plane spanned by axes a and b, as segments through m. */
void circle(Segments& out, const glm::dmat4& m, const glm::dvec3& c, double r, int a, int b, int steps = 32, double from = 0.0,
            double to = 2.0 * std::numbers::pi)
{
    glm::dvec3 prev{};
    for (int i = 0; i <= steps; ++i)
    {
        double t = from + (to - from) * double(i) / steps;
        glm::dvec3 p = c;
        p[a] += r * std::cos(t);
        p[b] += r * std::sin(t);
        p = xf(m, p);
        if (i > 0)
            out.emplace_back(prev, p);
        prev = p;
    }
}

void square(Segments& out, const glm::dmat4& m, double y, double h)
{
    glm::dvec3 c[4] = {{-h, y, -h}, {h, y, -h}, {h, y, h}, {-h, y, h}};
    for (int i = 0; i < 4; ++i)
        out.emplace_back(xf(m, c[i]), xf(m, c[(i + 1) % 4]));
}

void push(std::vector<render::LineVertex>& lines, const Segments& segs, const glm::vec4& color)
{
    for (auto& [a, b] : segs)
    {
        lines.push_back({glm::dvec3(a), color});
        lines.push_back({glm::dvec3(b), color});
    }
}

/** Click: pixels from a to the nearest segment; marquee: 0 when any end is inside. */
std::optional<double> pickSegments(const Segments& segs, const ViewInfo& view, QPointF a, QPointF b, bool click)
{
    QRectF box = QRectF(a, b).normalized();
    std::optional<double> best;
    for (auto& [s0, s1] : segs)
    {
        auto p0 = view.project(s0), p1 = view.project(s1);
        if (!p0 || !p1)
            continue;
        if (!click)
        {
            if (box.contains(*p0) || box.contains(*p1))
                return 0.0;
            continue;
        }
        QPointF d = *p1 - *p0;
        double len2 = QPointF::dotProduct(d, d);
        double t = len2 > 0 ? std::clamp(QPointF::dotProduct(a - *p0, d) / len2, 0.0, 1.0) : 0.0;
        QPointF q = *p0 + d * t - a;
        double dist = std::hypot(q.x(), q.y());
        if (!best || dist < *best)
            best = dist;
    }
    return best;
}

double num(const Node& n, const char* attr)
{
    int a = n.type().attrIndex(attr);
    if (a < 0)
        return 0.0;
    const scene::PlugValue& v = n.value(a);
    if (auto* d = std::get_if<double>(&v))
        return *d;
    if (auto* i = std::get_if<int64_t>(&v))
        return double(*i);
    return 0.0;
}

// ------------------------------------------------------------------------------------------------ deformer handles

/** The deformed axis of a nonlinear deformer between its bounds, with ticks at the bounds, in handle space. */
Segments nonlinearShape(const Node& d)
{
    const std::string& t = d.type().name;
    const double lo = num(d, "lowBound"), hi = num(d, "highBound");
    std::function<glm::dvec3(const glm::dvec3&)> f = [](const glm::dvec3& p) { return p; };
    if (t == "bendDeformer")
        f = [o = deform::BendParams{num(d, "angle"), lo, hi}](const glm::dvec3& p) { return deform::bend(p, o); };
    else if (t == "sineDeformer")
        f = [o = deform::SineParams{num(d, "amplitude"), num(d, "wavelength"), num(d, "offset"), num(d, "dropoff"), lo, hi}](const glm::dvec3& p) {
            return deform::sine(p, o);
        };
    Segments out;
    if (t == "waveDeformer")
    {
        circle(out, glm::dmat4(1.0), glm::dvec3(0.0), std::max(num(d, "minRadius"), 1e-3), 0, 2);
        circle(out, glm::dmat4(1.0), glm::dvec3(0.0), std::max(num(d, "maxRadius"), 1e-3), 0, 2);
        out.emplace_back(glm::dvec3(0, -0.25, 0), glm::dvec3(0, 0.25, 0));
    }
    else
    {
        const int steps = 32;
        glm::dvec3 prev{};
        for (int i = 0; i <= steps; ++i)
        {
            glm::dvec3 p = f(glm::dvec3(0.0, lo + (hi - lo) * double(i) / steps, 0.0));
            if (i > 0)
                out.emplace_back(prev, p);
            prev = p;
        }
        for (double y : {lo, hi})
        {
            out.emplace_back(f({-0.15, y, 0}), f({0.15, y, 0}));
            out.emplace_back(f({0, y, -0.15}), f({0, y, 0.15}));
        }
    }
    if (double r = num(d, "falloffRadius"); r > 0.0)
    {
        circle(out, glm::dmat4(1.0), glm::dvec3(0.0), r, 0, 2, 48);
        circle(out, glm::dmat4(1.0), glm::dvec3(0.0), r, 0, 1, 48);
    }
    return out;
}

struct HandleDraw
{
    NodeId handle = kNoNode;
    Segments segs;
    bool selected = false;
};

std::vector<HandleDraw> handleDraws(Graph& g)
{
    std::vector<HandleDraw> out;
    for (Node* d : scene::sceneIndex::matching(g, "deformer", scene::deformers::isDeformer))
    {
        const NodeId id = d->id();
        int hm = d->type().attrIndex("handleMatrix");
        if (hm < 0)
            continue;
        auto src = g.source({id, hm, -1});
        Node* h = src ? g.node(src->node) : nullptr;
        if (!h || !h->type().isA("transform") || !scene::effectivelyVisible(g, h->id()))
            continue;
        NodeId mesh = scene::modifiers::meshOf(g, id);
        HandleDraw hd;
        hd.handle = h->id();
        hd.selected = g.isSelected(h->id()) || g.isSelected(id) || (mesh != kNoNode && g.isSelected(mesh));
        const glm::dmat4 world = h->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        if (d->type().name == "softMod")
        {
            // The falloff only shows while the handle, the deformer or its mesh is selected; it would clutter otherwise.
            double r = num(*d, "falloffRadius");
            if (!hd.selected || r <= 0.0)
                continue;
            glm::dvec3 c = std::get<glm::dvec3>(d->value(d->type().attrIndex("falloffCenter")));
            for (auto [a, b] : std::initializer_list<std::pair<int, int>>{{0, 1}, {0, 2}, {1, 2}})
                circle(hd.segs, glm::dmat4(1.0), c, r, a, b, 48);
        }
        else
            for (auto& [a, b] : nonlinearShape(*d))
                hd.segs.emplace_back(xf(world, a), xf(world, b));
        out.push_back(std::move(hd));
    }
    return out;
}

void drawHandles(render::FrameState& f, OverlayPainter&, const ViewInfo&)
{
    LILA_ZONE();

    for (const HandleDraw& h : handleDraws(Graph::current()))
        // Selected handles show through the mesh: their axis usually runs inside it.
        if (h.selected)
            push(f.linesXray, h.segs, f.wireSelected);
        else
            push(f.lines, h.segs, glm::vec4(0.35f, 0.12f, 0.45f, 1.0f));
}

std::vector<viewportHooks::PickHit> pickHandles(QPointF a, QPointF b, bool click, const ViewInfo& view)
{
    Graph& g = Graph::current();
    std::vector<viewportHooks::PickHit> out;
    for (const HandleDraw& h : handleDraws(g))
        if (auto d = pickSegments(h.segs, view, a, b, click); d && (!click || *d <= 6.0))
            out.push_back({g.node(h.handle)->path(), *d});
    if (click && out.size() > 1)
    {
        auto best = std::ranges::min_element(out, {}, &viewportHooks::PickHit::pixels);
        out = {*best};
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ fields

Segments fieldShape(const Node& n, const glm::dmat4& world)
{
    Segments out;
    const auto* layers = dynamic_cast<const scene::fields::Layers*>(n.as<scene::DataPtr>(scene::fields::kLayers).get());
    bool drew = false;
    if (layers)
        for (const scene::fields::Layer& l : layers->layers)
        {
            if (!l.enabled || l.kind != scene::fields::Kind::Shape)
                continue;
            drew = true;
            const glm::dmat4 m = world * l.matrix();
            using S = scene::fields::Shape;
            switch (l.shape)
            {
            case S::Sphere:
                for (auto [a, b] : std::initializer_list<std::pair<int, int>>{{0, 1}, {0, 2}, {1, 2}})
                    circle(out, m, glm::dvec3(0.0), 1.0, a, b);
                if (l.softness > 0.0 && l.softness < 1.0)
                    circle(out, m, glm::dvec3(0.0), 1.0 - l.softness, 0, 2, 24);
                break;
            case S::Box:
                for (double y : {-1.0, 1.0})
                    square(out, m, y, 1.0);
                for (auto [x, z] : std::initializer_list<std::pair<double, double>>{{-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0}})
                    out.emplace_back(xf(m, {x, -1, z}), xf(m, {x, 1, z}));
                break;
            case S::Capsule:
            {
                double h = l.length * 0.5;
                circle(out, m, {0, h, 0}, 1.0, 0, 2);
                circle(out, m, {0, -h, 0}, 1.0, 0, 2);
                for (auto [x, z] : std::initializer_list<std::pair<double, double>>{{-1.0, 0.0}, {1.0, 0.0}, {0.0, -1.0}, {0.0, 1.0}})
                    out.emplace_back(xf(m, {x, -h, z}), xf(m, {x, h, z}));
                circle(out, m, {0, h, 0}, 1.0, 0, 1, 16, 0.0, std::numbers::pi);
                circle(out, m, {0, -h, 0}, 1.0, 0, 1, 16, std::numbers::pi, 2.0 * std::numbers::pi);
                circle(out, m, {0, h, 0}, 1.0, 2, 1, 16, 0.0, std::numbers::pi);
                circle(out, m, {0, -h, 0}, 1.0, 2, 1, 16, std::numbers::pi, 2.0 * std::numbers::pi);
                break;
            }
            case S::Plane:
                square(out, m, 0.0, 1.0);
                square(out, m, -1.0, 1.0);
                square(out, m, 1.0, 1.0);
                break;
            case S::Linear:
                square(out, m, -1.0, 1.0);
                square(out, m, 1.0, 1.0);
                out.emplace_back(xf(m, {0, -1, 0}), xf(m, {0, 1, 0}));
                out.emplace_back(xf(m, {0, 1, 0}), xf(m, {0.15, 0.75, 0}));
                out.emplace_back(xf(m, {0, 1, 0}), xf(m, {-0.15, 0.75, 0}));
                break;
            case S::Radial:
                circle(out, m, glm::dvec3(0.0), 1.0, 0, 2);
                out.emplace_back(xf(m, glm::dvec3(0.0)), xf(m, {1, 0, 0}));
                out.emplace_back(xf(m, {0, -0.5, 0}), xf(m, {0, 0.5, 0}));
                break;
            case S::Cone:
            {
                double r = l.length * std::tan(glm::radians(l.angle));
                circle(out, m, {0, l.length, 0}, r, 0, 2);
                for (auto [x, z] : std::initializer_list<std::pair<double, double>>{{-r, 0.0}, {r, 0.0}, {0.0, -r}, {0.0, r}})
                    out.emplace_back(xf(m, glm::dvec3(0.0)), xf(m, {x, l.length, z}));
                break;
            }
            }
        }
    if (!drew)
    {
        // Fields without shapes still need something to see and click: a small diamond.
        glm::dvec3 axes[3] = {{0.25, 0, 0}, {0, 0.25, 0}, {0, 0, 0.25}};
        for (int i = 0; i < 3; ++i)
            for (int j = i + 1; j < 3; ++j)
                for (double si : {-1.0, 1.0})
                    for (double sj : {-1.0, 1.0})
                        out.emplace_back(xf(world, axes[i] * si), xf(world, axes[j] * sj));
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ heatmaps

bool heatmapOn() { return PrefStore::get().value<bool>("general", kHeatmapPref, false); }

/** The points before any deformation in the mesh's stack: the input of its first deformer or skinCluster. */
scene::MeshPtr undeformed(Graph& g, NodeId mesh)
{
    for (NodeId id : scene::modifiers::stack(g, mesh))
    {
        Node* n = g.node(id);
        if (n && (scene::deformers::isDeformer(*n) || n->type().isA("skinCluster")))
        {
            auto* m = scene::modifiers::asModifier(n);
            return m ? std::get<scene::MeshPtr>(g.value({id, m->inAttr, -1})) : nullptr;
        }
    }
    return nullptr;
}

struct Overlay
{
    scene::MeshPtr mesh;
    const void* key = nullptr;
    glm::dmat4 world{0.0};
    double max = 0.0;
    std::vector<render::LineVertex> tris;
};

/** Gray per-point values over a mesh's faces, with a little shading from a fixed light so the form still reads. */
void buildTris(Overlay& o, const geo::Mesh& m, const glm::dmat4& world, const std::vector<float>& value)
{
    o.tris.clear();
    glm::dmat3 nrm = glm::transpose(glm::inverse(glm::dmat3(world)));
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        std::vector<geo::Index> pts = m.facePoints(f);
        glm::dvec3 n = nrm * m.faceNormal(f);
        double facing = glm::length(n) > 1e-18 ? std::abs(glm::dot(glm::normalize(n), glm::normalize(glm::dvec3(0.3, 0.8, 0.5)))) : 1.0;
        float shade = float(0.6 + 0.4 * facing);
        auto vert = [&](geo::Index p) {
            glm::vec4 col = viewportDisplay::weightColor(p < value.size() ? value[p] : 0.0f, true);
            return render::LineVertex{glm::dvec3(xf(world, m.position(p))), glm::vec4(glm::vec3(col) * shade, 1.0f)};
        };
        for (size_t k = 1; k + 1 < pts.size(); ++k)
        {
            o.tris.push_back(vert(pts[0]));
            o.tris.push_back(vert(pts[k]));
            o.tris.push_back(vert(pts[k + 1]));
        }
    }
}

std::unordered_map<NodeId, Overlay>& heatCache()
{
    static std::unordered_map<NodeId, Overlay> c;
    return c;
}
std::unordered_map<NodeId, Overlay>& fieldCache()
{
    static std::unordered_map<NodeId, Overlay> c;
    return c;
}

void drawHeatmaps(render::FrameState& f, OverlayPainter& p, const ViewInfo& view)
{
    LILA_ZONE();

    Graph& g = Graph::current();
    if (viewportTools::active() && std::string(viewportTools::active()->name()) == "weightPaint")
        return;
    g.evaluate();

    // A selected field shows its value on the meshes whose deformers read it.
    std::unordered_map<NodeId, Overlay> fieldNext;
    for (NodeId id : g.selection())
    {
        Node* fn = g.node(id);
        if (!fn || !scene::fields::isField(*fn))
            continue;
        auto eval = std::dynamic_pointer_cast<const scene::fields::Evaluator>(fn->as<scene::DataPtr>(scene::fields::kOutField));
        if (!eval)
            continue;
        for (const scene::PlugRef& dst : g.destinations({id, scene::fields::kOutField, -1}))
        {
            NodeId mesh = scene::modifiers::meshOf(g, dst.node);
            Node* mn = mesh != kNoNode ? g.node(mesh) : nullptr;
            if (!mn || fieldNext.contains(mesh) || !scene::effectivelyVisible(g, mesh))
                continue;
            scene::MeshPtr out = mn->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
            scene::MeshPtr in = std::get<scene::MeshPtr>(g.value({dst.node, scene::DeformerNode::kInMesh, -1}));
            if (!out || !in || in->pointCapacity() != out->pointCapacity())
                continue;
            glm::dmat4 world = mn->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            Overlay o = fieldCache().contains(mesh) ? std::move(fieldCache()[mesh]) : Overlay{};
            if (o.mesh != out || o.key != eval.get() || o.world != world)
            {
                const Node* dn = g.node(dst.node);
                const int ga = dn->type().attrIndex("geomMatrix");
                const glm::dmat4 geom = ga >= 0 ? dn->as<glm::dmat4>(ga) : world;
                buildTris(o, *out, world, scene::fields::evaluate(*eval, *in, geom));
                o.mesh = out;
                o.key = eval.get();
                o.world = world;
            }
            f.tris3d.insert(f.tris3d.end(), o.tris.begin(), o.tris.end());
            fieldNext[mesh] = std::move(o);
        }
    }
    fieldCache() = std::move(fieldNext);

    if (!heatmapOn())
    {
        heatCache().clear();
        return;
    }
    std::vector<NodeId> meshes;
    for (NodeId id : g.selection())
        if (Node* n = g.node(id); n && n->type().isA("mesh"))
            meshes.push_back(id);
    if (meshes.empty())
        for (NodeId id : g.nodes())
            if (Node* n = g.node(id); n && n->type().isA("mesh") && !scene::deformers::list(g, id).empty())
                meshes.push_back(id);
    std::unordered_map<NodeId, Overlay> next;
    double shownMax = 0.0;
    for (NodeId id : meshes)
    {
        Node* n = g.node(id);
        if (fieldCache().contains(id) || !scene::effectivelyVisible(g, id))
            continue;
        scene::MeshPtr out = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        scene::MeshPtr base = undeformed(g, id);
        if (!out || !base || base->pointCapacity() != out->pointCapacity())
            continue;
        glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        Overlay o = heatCache().contains(id) ? std::move(heatCache()[id]) : Overlay{};
        if (o.mesh != out || o.key != base.get() || o.world != world)
        {
            std::vector<float> mag(out->pointCapacity(), 0.0f);
            const double scale = std::cbrt(std::abs(glm::determinant(glm::dmat3(world))));
            double max = 0.0;
            for (geo::Index pt = 0; pt < out->pointCapacity(); ++pt)
                if (out->pointValid(pt))
                {
                    mag[pt] = float(glm::length(out->position(pt) - base->position(pt)) * scale);
                    max = std::max(max, double(mag[pt]));
                }
            for (float& v : mag)
                v = max > 1e-12 ? float(v / max) : 0.0f;
            buildTris(o, *out, world, mag);
            o.mesh = out;
            o.key = base.get();
            o.world = world;
            o.max = max;
        }
        shownMax = std::max(shownMax, o.max);
        f.tris3d.insert(f.tris3d.end(), o.tris.begin(), o.tris.end());
        next[id] = std::move(o);
    }
    heatCache() = std::move(next);
    if (!heatCache().empty())
        p.label(QPointF(12.0, view.height - 64.0), QString("Displacement heatmap: white = %1 cm").arg(shownMax, 0, 'g', 4));
}

void registerDeformHandleActions()
{
    actions::add({.id = "deform.paintWeights",
                  .label = "Paint Deformer Weights",
                  .category = "Deform",
                  .run = [] { startWeightPaint(); },
                  .doc = "Paint the weights of a deformer on the selected mesh, or a painted layer of a field its deformers read."});
    addDeformCommand("deform.floodWeights", "Flood Weights", "floodDeformerWeights", nlohmann::json::object(), "floodDeformerWeights()",
        "Sets the selected deformer's weights (or the top deformer of the selected mesh) to a value, on the selected components or everywhere.");
    addDeformCommand("deform.smoothWeights", "Smooth Weights", "smoothDeformerWeights", nlohmann::json::object(), "smoothDeformerWeights()",
        "Smooths the selected deformer's weights over the mesh.");
    addDeformCommand("deform.mirrorWeights", "Mirror Weights", "mirrorDeformerWeights", nlohmann::json::object(), "mirrorDeformerWeights()",
        "Copies the selected deformer's weights from one side of the mesh to the other.");
    addDeformCommand("deform.copyWeights", "Copy Weights", "copyDeformerWeights", nlohmann::json::object(), "copyDeformerWeights()",
        "Select the source deformer, then the destinations: copies the weights, by closest point across different meshes.");
    addDeformCommand("deform.exportWeights", "Export Weights", "exportDeformerWeights", nlohmann::json::object(), "exportDeformerWeights(file=...)",
        "Writes the selected deformer's weights to a JSON file.");
    addDeformCommand("deform.importWeights", "Import Weights", "importDeformerWeights", nlohmann::json::object(), "importDeformerWeights(file=...)",
        "Reads weights from a JSON file onto the selected deformer, by closest point when the mesh differs.");
    addDeformCommand("deform.setMembership", "Set Membership", "setDeformerMembership", nlohmann::json::object(), "setDeformerMembership()",
        "Restricts the selected mesh's top deformer (or the deformer named) to the selected components.");
    addDeformCommand("deform.clearMembership", "Clear Membership", "setDeformerMembership", {{"mode", "clear"}}, "setDeformerMembership(mode=\"clear\")",
        "Lets the deformer move every point again.");
    addDeformCommand("deform.field", "Field", "field", nlohmann::json::object(), "field()",
        "Creates a falloff field on the selected meshes; selected deformers read it.");
    addDeformCommand("deform.addFieldLayer", "Add Field Layer", "addFieldLayer", nlohmann::json::object(), "addFieldLayer(type=\"noise\")",
        "Adds a layer (shape, noise, components, painted, proximity, UV region or attribute) to the selected field.");
    addDeformCommand("deform.connectField", "Connect Field", "connectField", nlohmann::json::object(), "connectField()",
        "Select a field, then deformers (or meshes): they read the field as their weights.");
    addDeformCommand("deform.disconnectField", "Disconnect Field", "disconnectField", nlohmann::json::object(), "disconnectField()",
        "The selected deformers stop reading their field.");
    addDeformCommand("deform.softSelectField", "Soft Select With Field", "softSelectField", nlohmann::json::object(), "softSelectField()",
        "Soft selection weights are multiplied by the selected field; with nothing selected it stops.");
    addDeformCommand("deform.correction", "Correction", "correction", nlohmann::json::object(), "correction()",
        "Adds a correction deformer: sculpted fixes that ride on the deformation below.");
    addDeformCommand("deform.editCorrection", "Edit Correction", "editCorrection", nlohmann::json::object(), "editCorrection()",
        "Toggles sculpting the selected correction deformer on a stand-in copy.");
    addDeformCommand("deform.timing", "Deformer Timing", "deformerTiming", nlohmann::json::object(), "deformerTiming()",
        "Prints how long each deformer on the selected meshes took to evaluate.");
}
} // namespace

void registerDeformFoundationUi()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    registerDeformHandleActions();
    viewportHooks::addDrawer("lila.deformHandles", drawHandles);
    viewportHooks::addPicker("lila.deformHandles", pickHandles);
    viewportHooks::addDrawer("lila.deformHeatmaps", drawHeatmaps);
    drawOverrides::add({.id = "lila.field",
                        .nodeType = "field",
                        .draw = [](drawOverrides::DrawContext& c) { push(c.frame.lines, fieldShape(c.node, c.world), c.color); },
                        .pick = [](const drawOverrides::PickContext& c) {
                            return pickSegments(fieldShape(c.node, c.world), c.view, c.a, c.b, c.click);
                        }});

    ops::registerCommand({
        .name = "displacementHeatmap",
        .doc = "Show how far deformation moves each point as a grayscale overlay (white the most moved point) on the selected "
               "meshes, or on every deformed mesh when none is selected. A display setting, so not undoable.",
        .flags = {{"on", "", ArgType::Bool, nullptr, "Default: toggle."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            bool on = a.has("on") ? a.get<bool>("on") : !heatmapOn();
            PrefStore::get().set("general", kHeatmapPref, on);
            viewportTools::requestRedraw();
            return on;
        },
    });
    actions::add({.id = "deform.heatmap",
                  .label = "Displacement Heatmap",
                  .category = "Deform",
                  .doc = "Toggles a grayscale overlay of how far deformation moves each point: white is the most moved.",
                  .python = "displacementHeatmap()",
                  .command = "displacementHeatmap",
                  .call = nlohmann::json::object(),
                  .checked = [] { return heatmapOn(); }});
}
} // namespace lila::ui
