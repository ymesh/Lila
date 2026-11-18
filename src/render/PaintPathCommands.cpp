#include "render/PaintGpu.h"

#include "brush/Brush.h"
#include "curve/CurveNodes.h"
#include "curve/Spline.h"
#include "geo/Bvh.h"
#include "ops/Command.h"
#include "scene/MeshNodes.h"
#include "scene/Node.h"
#include "tex/PaintStrokes.h"
#include "tex/PaintSurface.h"
#include "tex/TextureSet.h"

#include <algorithm>
#include <cmath>

/**
 * Path strokes: a Bezier on the surface (placed point by point, or taken from a scene curve) sampled into dabs at the
 * brush's spacing and painted as one stroke. The path stays on the stroke record, so texEditPath moves its points and
 * samples the dabs again.
 */
namespace lila::render::paint
{
// Named, not anonymous: unity builds put this beside PaintCommands.cpp's own helpers.
namespace pathstroke
{
using namespace tex::paint;
using scene::Node;
using json = nlohmann::json;
using T = ArgType;

scene::Graph& graph() { return scene::Graph::current(); }

struct PathPoint
{
    glm::dvec3 p{0.0}, in{0.0}, out{0.0};
    bool hasIn = false, hasOut = false;
};

glm::dvec3 vec3(const json& j, const char* what)
{
    if (!j.is_array() || j.size() != 3)
        throw CommandError(std::string("path: ") + what + " must be [x, y, z]");
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

/** [[x, y, z], ...] or [{position, in?, out?}, ...]: handles left out are smooth. */
std::vector<PathPoint> pathPoints(const json& pts)
{
    if (!pts.is_array() || pts.size() < 2)
        throw CommandError("path: points needs at least two points");
    std::vector<PathPoint> out;
    for (const json& o : pts)
    {
        PathPoint pp;
        if (o.is_object())
        {
            pp.p = vec3(o.at("position"), "position");
            if (o.contains("in"))
            {
                pp.in = vec3(o["in"], "in");
                pp.hasIn = true;
            }
            if (o.contains("out"))
            {
                pp.out = vec3(o["out"], "out");
                pp.hasOut = true;
            }
        }
        else
            pp.p = vec3(o, "point");
        out.push_back(pp);
    }
    // Smooth handles: a third of the way along the neighbours' chord.
    for (size_t i = 0; i < out.size(); ++i)
    {
        const glm::dvec3 prev = out[i > 0 ? i - 1 : i].p, next = out[i + 1 < out.size() ? i + 1 : i].p;
        const glm::dvec3 t = (next - prev) * (i > 0 && i + 1 < out.size() ? 0.5 : 1.0);
        if (!out[i].hasIn)
            out[i].in = out[i].p - t / 3.0;
        if (!out[i].hasOut)
            out[i].out = out[i].p + t / 3.0;
    }
    return out;
}

json pathJson(const std::vector<PathPoint>& pts)
{
    json a = json::array();
    for (const PathPoint& p : pts)
        a.push_back({{"position", {p.p.x, p.p.y, p.p.z}}, {"in", {p.in.x, p.in.y, p.in.z}}, {"out", {p.out.x, p.out.y, p.out.z}}});
    return a;
}

glm::dvec3 bezier(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c, const glm::dvec3& d, double t)
{
    const double s = 1.0 - t;
    return a * (s * s * s) + b * (3.0 * s * s * t) + c * (3.0 * s * t * t) + d * (t * t * t);
}

/** Points along the path every step (object units), the first and last included. */
std::vector<glm::dvec3> samplePath(const std::vector<PathPoint>& pts, double step)
{
    constexpr int kFine = 64;
    std::vector<glm::dvec3> fine{pts.front().p};
    for (size_t i = 0; i + 1 < pts.size(); ++i)
        for (int k = 1; k <= kFine; ++k)
            fine.push_back(bezier(pts[i].p, pts[i].out, pts[i + 1].in, pts[i + 1].p, double(k) / kFine));
    std::vector<glm::dvec3> out{fine.front()};
    double carried = 0.0;
    for (size_t i = 1; i < fine.size(); ++i)
    {
        const glm::dvec3 a = fine[i - 1], b = fine[i];
        const double len = glm::length(b - a);
        double at = step - carried;
        while (at <= len)
        {
            out.push_back(a + (b - a) * (at / std::max(len, 1e-12)));
            at += step;
        }
        carried = len - (at - step);
    }
    if (glm::length(out.back() - fine.back()) > step * 0.25)
        out.push_back(fine.back());
    return out;
}

/** The surface dab positions of the samples: each moved to the closest point of the painted surface. */
json dabsOnSurface(const std::vector<glm::dvec3>& samples, const PaintMesh& mesh, double radius)
{
    json dabs = json::array();
    for (const glm::dvec3& s : samples)
    {
        geo::BvhClosest hit;
        if (!mesh.bvh->closestPoint(s, hit, std::numeric_limits<double>::max()))
            continue;
        dabs.push_back({{"position", {hit.point.x, hit.point.y, hit.point.z}}, {"radius", radius}});
    }
    if (dabs.empty())
        throw CommandError("path: no point of it is near the painted surface");
    return dabs;
}

/** A scene curve's splines in the mesh's object space, as path points with their own Bezier handles. */
std::vector<PathPoint> curvePoints(const Node& curveNode, const Node& meshNode)
{
    const curve::Splines& splines = curve::splinesOf(graph().value({curveNode.id(), curve::CurveShape::kWorldSpace, -1}));
    if (splines.empty())
        throw CommandError("texPaintPath: " + curveNode.path() + " holds no curve");
    const glm::dmat4 toObject = glm::inverse(meshNode.as<glm::dmat4>(scene::Transform::kWorldMatrix));
    curve::Spline bez = curve::convert(splines.front(), curve::Kind::Bezier);
    bez.normalize();
    std::vector<PathPoint> out;
    auto obj = [&](const glm::dvec3& w) { return glm::dvec3(toObject * glm::dvec4(w, 1.0)); };
    const size_t n = bez.points.size();
    for (size_t i = 0; i < n; ++i)
        out.push_back({obj(bez.points[i]), obj(bez.handleL[i]), obj(bez.handleR[i]), true, true});
    if (bez.closed && n > 1)
        out.push_back(out.front());
    if (out.size() < 2)
        throw CommandError("texPaintPath: " + curveNode.path() + " is too short to paint along");
    return out;
}

std::string meshFor(const Args& a, const Node& set)
{
    if (a.has("mesh"))
        return a.get<std::string>("mesh");
    std::string any;
    graph().evaluate();
    for (scene::NodeId id : graph().nodes())
    {
        Node* n = graph().node(id);
        if (!n || !n->type().isA("mesh"))
            continue;
        scene::MeshPtr m = paintSurface(*n);
        if (!m || !m->numFaces())
            continue;
        if (any.empty())
            any = n->path();
        const auto& names = m->materialNames();
        if (usesSet(names, set))
            return n->path();
    }
    return any;
}

Node& setOf(const Args& a, const char* cmd)
{
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError(std::string(cmd) + ": texture set required");
    Node* n = tex::findSet(graph(), a.positional[0].get<std::string>());
    if (!n)
        throw CommandError(std::string(cmd) + ": no texture set named '" + a.positional[0].get<std::string>() + "'");
    return *n;
}

/** The stroke record for a path: its dabs sampled from the points at the brush's spacing. */
json pathStroke(const std::vector<PathPoint>& pts, const PaintMesh& mesh, const brush::BrushSettings& bs, double radius)
{
    const double step = std::max(radius * 2.0 * std::max(bs.spacing, 0.01), 1e-4);
    return {{"space", "surface"}, {"dabs", dabsOnSurface(samplePath(pts, step), mesh, radius)}, {"path", {{"points", pathJson(pts)}}}};
}

json paintPath(const Args& a)
{
    Node& set = setOf(a, "texPaintPath");
    const std::string meshPath = meshFor(a, set);
    Node* meshNode = meshPath.empty() ? nullptr : graph().find(meshPath);
    if (!meshNode)
        throw CommandError("texPaintPath: no mesh uses " + set.name() + "'s material; pass mesh=");
    std::shared_ptr<const PaintMesh> pm = paintMesh(graph(), meshNode->path(), setMaterial(set));
    if (!pm)
        throw CommandError("texPaintPath: " + meshNode->path() + " isn't a mesh with faces");
    std::vector<PathPoint> pts;
    std::string curveFrom;
    if (a.has("curve"))
    {
        Node* c = graph().find(a.get<std::string>("curve"));
        if (!c || !c->type().isA("curve"))
            throw CommandError("texPaintPath: no curve " + a.get<std::string>("curve"));
        pts = curvePoints(*c, *meshNode);
        curveFrom = c->path();
    }
    else if (a.has("points"))
        pts = pathPoints(a.flags["points"]);
    else
        throw CommandError("texPaintPath: give points or curve");
    const brush::BrushSettings bs = brush::BrushSettings::fromJson(a.get<json>("brush", json::object()));
    const double radius = a.has("radius") ? a.get<double>("radius") : bs.worldRadius;
    if (!(radius > 0.0))
        throw CommandError("texPaintPath: radius must be positive");
    json stroke = pathStroke(pts, *pm, bs, radius);
    if (!curveFrom.empty())
        stroke["path"]["curve"] = curveFrom;
    stroke["mesh"] = meshNode->path();
    stroke["brush"] = a.get<json>("brush", json::object());
    for (const char* k : {"payload", "stencil", "particles"})
        if (a.has(k))
            stroke[k] = a.flags[k];
    // A recorded view keeps occlusion and stencils; the tool passes the one the path was placed in.
    if (a.has("view"))
        stroke["view"] = a.flags["view"];
    json call = {{"args", {set.name()}}, {"stroke", stroke}};
    if (a.has("layer"))
        call["layer"] = a.flags["layer"];
    json r = ops::execute("texPaintStroke", call, CallSource::Script);
    r["path"] = stroke["path"];
    return r;
}

json editPath(const Args& a)
{
    Node& set = setOf(a, "texEditPath");
    const std::string layer = a.positional.size() > 1 ? a.positional[1].get<std::string>() : a.get<std::string>("layer", "");
    PaintPtr data = readPaint(set);
    const Buffer* buf = data->buffer(layer);
    if (!buf)
        throw CommandError("texEditPath: no paint layer '" + layer + "' in " + set.name());
    const uint64_t id = uint64_t(a.get<int64_t>("stroke", 0));
    auto it = std::find_if(buf->strokes.begin(), buf->strokes.end(), [&](const StrokePtr& s) { return s->id == id; });
    if (it == buf->strokes.end())
        throw CommandError("texEditPath: " + layer + " has no stroke " + std::to_string(id));
    const Stroke& old = **it;
    if (old.path.is_null())
        throw CommandError("texEditPath: stroke " + std::to_string(id) + " isn't a path stroke");
    std::shared_ptr<const PaintMesh> pm = paintMesh(graph(), old.mesh, setMaterial(set));
    if (!pm)
        throw CommandError("texEditPath: " + old.mesh + " isn't a mesh with faces any more");
    const std::vector<PathPoint> pts = pathPoints(a.has("points") ? a.flags["points"] : old.path.value("points", json::array()));
    brush::BrushSettings bs = brush::BrushSettings::fromJson(old.brush);
    const double radius = a.has("radius") ? a.get<double>("radius") : (old.dabs.empty() ? bs.worldRadius : double(old.dabs.front().radius));

    // The new dabs come through texPaintStroke's own position snapping, in a scratch record that is never stored.
    json h = pathStroke(pts, *pm, bs, radius);
    Stroke next = old;
    next.dabs.clear();
    for (const json& d : h["dabs"])
    {
        brush::Dab bd;
        bd.center = vec3(d["position"], "position");
        bd.radius = radius;
        bd.flow = bs.flow;
        Dab r;
        if (recordDab(*pm, bd, r))
            next.dabs.push_back(r);
    }
    if (next.dabs.empty())
        throw CommandError("texEditPath: the edited path is off the surface");
    next.path["points"] = pathJson(pts);
    // A new id: the pages see a changed stroke list and rebuild, and undo rebuilds them back.
    uint64_t top = 0;
    for (const StrokePtr& s : buf->strokes)
        top = std::max(top, s->id);
    next.id = top + 1;
    Buffer b = *buf;
    b.strokes[size_t(it - buf->strokes.begin())] = std::make_shared<const Stroke>(std::move(next));
    auto nd = std::make_shared<PaintData>(*data);
    nd->buffers[layer] = std::move(b);
    writePaint(graph(), set, nd);
    try
    {
        rasterize(graph(), set, layer);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string("texEditPath: ") + e.what());
    }
    return {{"layer", layer}, {"stroke", top + 1}, {"dabs", nd->buffers[layer].strokes[size_t(it - buf->strokes.begin())]->dabs.size()}};
}
} // namespace pathstroke

void registerPathCommands()
{
    using namespace pathstroke;
    ops::registerCommand({
        .name = "texPaintPath",
        .doc = "Paint a stroke along a path on the surface: points [[x, y, z], ...] or [{position, in, out}, ...] in the mesh's object "
               "space (Bezier handles; left out, they are smooth), or curve= a scene curve, projected onto the mesh. Dabs are spaced by "
               "the brush's spacing at radius (object units, default the brush's world radius). The path stays on the stroke, so "
               "texEditPath can move it later. Takes texPaintStroke's payload, brush, stencil and view. Returns texPaintStroke's result "
               "plus the path.",
        .positionalDoc = "set",
        .flags = {{"points", "", T::List, nullptr, "Path points in the mesh's object space."},
                  {"curve", "", T::String, nullptr, "A scene curve to paint along instead of points."},
                  {"layer", "", T::String, nullptr, "The paint layer."},
                  {"mesh", "", T::String, nullptr, "The painted mesh; default the first using the set's material."},
                  {"radius", "", T::Float, nullptr, "Dab radius in object units."},
                  {"payload", "", T::Any, nullptr, "What the stroke writes (texPaintStroke's payload)."},
                  {"brush", "", T::Any, nullptr, "Brush settings."},
                  {"stencil", "", T::Any, nullptr, "A stencil, as texPaintStroke takes it (needs view)."},
                  {"particles", "", T::Any, nullptr, "Particle brush."},
                  {"view", "", T::Any, nullptr, "The stroke view record {objectToClip, eye, direction, width, height}."}},
        .run = paintPath,
    });
    ops::registerCommand({
        .name = "texEditPath",
        .doc = "Move a path stroke's points (points=, as texPaintPath takes them) or change its radius, and paint it again from them: "
               "the layer is rebuilt from its strokes with the path's new dabs. One undo step. Returns {layer, stroke (its new id), dabs}.",
        .positionalDoc = "set layer",
        .flags = {{"stroke", "", T::Int, nullptr, "The path stroke's id."},
                  {"layer", "", T::String, nullptr, "The paint layer."},
                  {"points", "", T::List, nullptr, "The path's new points."},
                  {"radius", "", T::Float, nullptr, "Dab radius in object units."}},
        .run = editPath,
    });
}
} // namespace lila::render::paint
