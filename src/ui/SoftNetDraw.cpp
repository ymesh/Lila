#include "ui/SoftNetDraw.h"

#include "core/Profile.h"
#include "geo/Mesh.h"
#include "layout/SoftNet.h"
#include "layout/SoftNetBind.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"
#include "ui/DrawOverrides.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lila::ui
{
namespace
{
using namespace lila::scene;
namespace sn = lila::deform::softnet;

/** A net's lines as surface points in order along each line, parsed once per binding. */
struct NetLines
{
    std::vector<std::vector<sn::SurfacePoint>> lines;
    size_t points = 0;
};

std::shared_ptr<const NetLines> netLinesOf(const std::string& binding)
{
    static std::mutex m;
    static std::map<size_t, std::shared_ptr<const NetLines>> cache;
    const size_t key = std::hash<std::string>{}(binding);
    std::lock_guard lock(m);
    if (auto it = cache.find(key); it != cache.end())
        return it->second;
    auto out = std::make_shared<NetLines>();
    layout::MeshBind b = layout::MeshBind::fromJson(binding);
    out->points = b.points;
    std::map<int, const sn::Sample*> knotSample;
    std::vector<std::vector<const sn::Sample*>> along(b.runs.size());
    for (const sn::Sample& s : b.samples)
        if (s.run >= 0 && size_t(s.run) < along.size())
            along[size_t(s.run)].push_back(&s);
        else if (s.knot >= 0)
            knotSample.emplace(s.knot, &s);
    // The body's lines as bound, finely; else the samples, coarser.
    if (b.lines.size() == b.runs.size())
    {
        for (const std::vector<sn::SurfacePoint>& line : b.lines)
            if (line.size() > 1)
                out->lines.push_back(line);
        b.runs.clear();
    }
    for (size_t r = 0; r < b.runs.size(); ++r)
    {
        std::sort(along[r].begin(), along[r].end(), [](const sn::Sample* x, const sn::Sample* y) { return x->t < y->t; });
        std::vector<sn::SurfacePoint> line;
        if (auto it = knotSample.find(b.runs[r].knot[0]); it != knotSample.end())
            line.push_back(it->second->at);
        for (const sn::Sample* s : along[r])
            line.push_back(s->at);
        if (auto it = knotSample.find(b.runs[r].knot[1]); it != knotSample.end())
            line.push_back(it->second->at);
        if (line.size() > 1)
            out->lines.push_back(std::move(line));
    }
    if (cache.size() > 16)
        cache.clear();
    cache.emplace(key, out);
    return out;
}

glm::vec4 selectedLinear() { return themeLinear("wireSelected", glm::vec4(0.25f, 1.0f, 0.4f, 1.0f)); }
glm::vec4 preselectLinear() { return themeLinear("wirePreselect", glm::vec4(1.0f, 0.75f, 0.2f, 1.0f)); }

glm::vec4 toSrgb(const glm::vec4& c)
{
    auto f = [](float v) { return v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f; };
    return {f(c.r), f(c.g), f(c.b), c.a};
}

/** The net's lines on the body as it is now: drawn on the surface it shapes, so they show what the knots did. */
void drawNet(drawOverrides::DrawContext& c)
{
    LILA_ZONE();
    Graph& g = Graph::current();
    const std::vector<NodeId> deformers = layout::netDeformers(g, c.node.id());
    const std::vector<NodeId> meshes = layout::netMeshes(g, c.node.id());
    if (deformers.empty() || meshes.empty())
        return;
    const Node* d = g.node(deformers.front());
    const Node* mesh = g.node(meshes.front());
    if (!d || !mesh)
        return;
    const std::shared_ptr<const NetLines> net = netLinesOf(d->as<std::string>(d->type().attrIndex("binding")));
    const MeshPtr out = mesh->as<MeshPtr>(MeshNode::kOutMesh);
    if (!out || out->pointCapacity() != net->points)
        return;
    const std::span<const glm::dvec3> pts = out->positions();
    const glm::dmat4 world = mesh->as<glm::dmat4>(Transform::kWorldMatrix);
    const bool bold = c.highlight == drawOverrides::Highlight::Selected || c.highlight == drawOverrides::Highlight::Preselected;
    const glm::vec4 color = c.highlight == drawOverrides::Highlight::Selected      ? selectedLinear()
                            : c.highlight == drawOverrides::Highlight::Preselected ? preselectLinear()
                                                                                   : glm::vec4(0.75f, 0.75f, 0.75f, 1.0f);
    std::vector<render::LineVertex>& halo = bold ? c.frame.curveHaloBold : c.frame.curveHalo;
    std::vector<render::LineVertex>& lines = bold ? c.frame.curveLinesBold : c.frame.curveLines;
    for (const std::vector<sn::SurfacePoint>& line : net->lines)
        for (size_t i = 0; i + 1 < line.size(); ++i)
        {
            const glm::vec3 a(world * glm::dvec4(sn::at(line[i], pts), 1.0)), b(world * glm::dvec4(sn::at(line[i + 1], pts), 1.0));
            halo.push_back({a, {0.02f, 0.02f, 0.02f, 0.9f}});
            halo.push_back({b, {0.02f, 0.02f, 0.02f, 0.9f}});
            lines.push_back({a, color});
            lines.push_back({b, color});
        }
}

/** A knot faces the viewer when its frame's z, the surface normal, points back toward the eye. */
bool facing(const ViewInfo& view, const glm::dmat4& world)
{
    const glm::dvec3 at(world[3]);
    return view.ortho || glm::dot(glm::dvec3(world[2]), view.eye - at) >= 0.0;
}

bool junction(const Node& knot) { return knot.as<int64_t>(knot.type().attrIndex("degree")) >= 3; }

double knotRadius(const Node& knot) { return junction(knot) ? 6.0 : 4.0; }

/** Junctions are a ring round a dot, other knots a dot; dimmed on the far side of the surface. */
void drawKnot(drawOverrides::DrawContext& c)
{
    const std::optional<QPointF> s = c.view.project(glm::dvec3(c.world[3]));
    if (!s)
        return;
    const bool front = facing(c.view, c.world);
    const float alpha = front ? 1.0f : 0.35f;
    glm::vec4 color(0.92f, 0.92f, 0.92f, alpha);
    if (c.highlight == drawOverrides::Highlight::Selected)
        color = glm::vec4(glm::vec3(toSrgb(selectedLinear())), alpha);
    else if (c.highlight == drawOverrides::Highlight::Preselected)
        color = glm::vec4(glm::vec3(toSrgb(preselectLinear())), alpha);
    const glm::vec4 rim(0.05f, 0.05f, 0.05f, 0.85f * alpha);
    const double r = knotRadius(c.node) + (c.highlight == drawOverrides::Highlight::Preselected ? 1.0 : 0.0);
    if (junction(c.node))
    {
        c.painter.arc(*s, r, 0.0, 360.0, 3.5f, rim);
        c.painter.arc(*s, r, 0.0, 360.0, 2.0f, color);
        c.painter.smoothDisc(*s, 2.5, rim);
        c.painter.smoothDisc(*s, 1.6, color);
    }
    else
    {
        c.painter.smoothDisc(*s, r + 1.2, rim);
        c.painter.smoothDisc(*s, r, color);
    }
}

std::optional<double> pickPoint(const drawOverrides::PickContext& c, double radius)
{
    const std::optional<QPointF> s = c.view.project(glm::dvec3(c.world[3]));
    if (!s)
        return std::nullopt;
    if (!c.click)
    {
        const double x0 = std::min(c.a.x(), c.b.x()), x1 = std::max(c.a.x(), c.b.x()), y0 = std::min(c.a.y(), c.b.y()), y1 = std::max(c.a.y(), c.b.y());
        return s->x() >= x0 && s->x() <= x1 && s->y() >= y0 && s->y() <= y1 ? std::optional<double>(0.0) : std::nullopt;
    }
    const double d = std::hypot(s->x() - c.a.x(), s->y() - c.a.y());
    return d <= radius + drawOverrides::kClickPixels ? std::optional<double>(d) : std::nullopt;
}

bool handleShown(drawOverrides::Highlight h) { return h != drawOverrides::Highlight::None; }

/** A tangent handle shows while its knot or itself is selected: a square on a stem from the knot. */
void drawTangent(drawOverrides::DrawContext& c)
{
    if (!handleShown(c.highlight))
        return;
    Graph& g = Graph::current();
    const Node* knot = g.node(g.parent(c.node.id()));
    const std::optional<QPointF> s = c.view.project(glm::dvec3(c.world[3]));
    if (!s || !knot)
        return;
    glm::vec4 color(0.92f, 0.92f, 0.92f, 1.0f);
    if (c.highlight == drawOverrides::Highlight::Selected)
        color = toSrgb(selectedLinear());
    else if (c.highlight == drawOverrides::Highlight::Preselected)
        color = toSrgb(preselectLinear());
    const glm::vec4 rim(0.05f, 0.05f, 0.05f, 0.85f);
    if (const std::optional<QPointF> k = c.view.project(glm::dvec3(knot->as<glm::dmat4>(Transform::kWorldMatrix)[3])))
    {
        c.painter.smoothLine(*k, *s, 3.0f, rim);
        c.painter.smoothLine(*k, *s, 1.5f, color);
    }
    const double h = c.highlight == drawOverrides::Highlight::Preselected ? 4.5 : 3.5;
    c.painter.rect(QRectF(s->x() - h - 1.2, s->y() - h - 1.2, 2 * (h + 1.2), 2 * (h + 1.2)), rim);
    c.painter.rect(QRectF(s->x() - h, s->y() - h, 2 * h, 2 * h), color);
}

std::optional<double> pickTangent(const drawOverrides::PickContext& c)
{
    Graph& g = Graph::current();
    // Hidden handles don't take clicks: only while their knot, a sibling or they are selected.
    const NodeId knot = g.parent(c.node.id());
    bool shown = g.isSelected(c.node.id()) || (knot != kNoNode && g.isSelected(knot));
    if (!shown && knot != kNoNode)
        for (NodeId s : g.children(knot))
            shown = shown || g.isSelected(s);
    return shown ? pickPoint(c, 4.5) : std::nullopt;
}
} // namespace

void registerSoftNetDraw()
{
    if (drawOverrides::registry().find("lila.softNetKnot"))
        return;
    drawOverrides::add({.id = "lila.softNetLines", .nodeType = layout::kSoftNetKnotsType, .draw = drawNet});
    drawOverrides::add({.id = "lila.softNetKnot",
                        .nodeType = layout::kSoftNetKnotType,
                        .draw = drawKnot,
                        .pick = [](const drawOverrides::PickContext& c) { return pickPoint(c, knotRadius(c.node)); }});
    drawOverrides::add({.id = "lila.softNetTangent", .nodeType = layout::kSoftNetTangentType, .draw = drawTangent, .pick = pickTangent});
}
} // namespace lila::ui
