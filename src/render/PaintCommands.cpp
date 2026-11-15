#include "render/PaintGpu.h"

#include "brush/Stroke.h"
#include "brush/Symmetry.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "geo/Symmetry.h"
#include "ops/Command.h"
#include "scene/Camera.h"
#include "scene/Components.h"
#include "scene/MeshNodes.h"
#include "scene/Node.h"
#include "tex/BrushPresets.h"
#include "tex/Layers.h"
#include "tex/PaintBuffer.h"
#include "tex/PaintParticles.h"
#include "tex/PaintSymmetry.h"
#include "tex/TextureSet.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace lila::render::paint
{
namespace
{
using namespace tex::paint;
using scene::Node;
using T = ArgType;

scene::Graph& graph() { return scene::Graph::current(); }

Node& setArg(const Args& a, const char* cmd)
{
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError(std::string(cmd) + ": texture set required");
    const std::string name = a.positional[0].get<std::string>();
    Node* n = tex::findSet(graph(), name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no texture set named '" + name + "'");
    return *n;
}

/** The layer named as the second positional or by layer=, else empty. */
std::string layerArg(const Args& a)
{
    if (a.positional.size() > 1 && a.positional[1].is_string())
        return a.positional[1].get<std::string>();
    return a.get<std::string>("layer", "");
}

std::string requireLayer(const Args& a, const Node& set, const char* cmd)
{
    std::string id = layerArg(a);
    PaintPtr data = readPaint(set);
    if (id.empty())
        id = data->active;
    if (id.empty() || !data->buffer(id))
        throw CommandError(std::string(cmd) + ": " + (id.empty() ? std::string("layer required") : "no paint layer " + id + " in " + set.name()));
    return id;
}

std::optional<tex::StackState> stackOf(const Node& set)
{
    try
    {
        tex::StackState st = tex::readStack(tex::readGraph(set), tex::readSettings(set));
        if (st.layered && !st.graphMode)
            return st;
    }
    catch (const std::exception&)
    {
    }
    return std::nullopt;
}

const tex::Layer* topPaintLayer(const std::vector<tex::Layer>& layers)
{
    for (auto it = layers.rbegin(); it != layers.rend(); ++it)
    {
        if (it->kind == tex::LayerKind::Paint)
            return &*it;
        if (it->kind == tex::LayerKind::Folder)
            if (const tex::Layer* l = topPaintLayer(it->children))
                return l;
    }
    return nullptr;
}

std::string pickLayer(const Args& a, Node& set) { return targetLayer(set, layerArg(a), true); }
} // namespace

std::string targetLayer(Node& set, const std::string& requested, bool create)
{
    if (!requested.empty())
        return requested;
    PaintPtr data = readPaint(set);
    std::optional<tex::StackState> stack = stackOf(set);
    if (!data->active.empty() && (data->buffer(data->active) || (stack && stack->stack.find(data->active))))
        return data->active;
    if (stack)
        if (const tex::Layer* l = topPaintLayer(stack->stack.layers))
            return l->id;
    if (!create)
        return {};
    try
    {
        json r = ops::execute("texAddLayer", {{"args", {set.name(), "paint"}}});
        if (r.is_object() && r.contains("id") && r["id"].is_string())
            return r["id"].get<std::string>();
    }
    catch (const std::exception& e)
    {
        log::warn("texPaintStroke: no paint layer could be added to {}'s stack ({}); painting a loose buffer", set.name(), e.what());
    }
    return data->uniqueId();
}

void wireLayer(Node& set, const std::string& layer, const std::vector<std::string>& channels)
{
    std::optional<tex::StackState> stack = stackOf(set);
    if (!stack)
        return;
    const tex::Layer* l = stack->stack.find(layer);
    if (!l || l->kind != tex::LayerKind::Paint)
        return;
    json patch = json::object();
    for (const std::string& c : channels)
    {
        const std::string ref = imageRef(layer, c);
        if (!l->paint.is_object() || l->paint.value(c, std::string()) != ref)
            patch[c] = ref;
    }
    if (patch.empty())
        return;
    try
    {
        ops::execute("texSetLayer", {{"args", {set.name(), layer, json{{"paint", patch}}}}});
    }
    catch (const std::exception& e)
    {
        log::warn("texPaintStroke: couldn't point layer {} at its paint ({})", layer, e.what());
    }
}

std::optional<tex::Graph> wiredGraph(Node& set, const std::string& layer, const std::vector<std::string>& channels)
{
    std::optional<tex::StackState> stack = stackOf(set);
    if (!stack)
        return std::nullopt;
    tex::SetSettings settings;
    tex::Graph graph;
    try
    {
        settings = tex::readSettings(set);
        graph = tex::readGraph(set);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
    tex::Layer* l = layer.empty() ? nullptr : stack->stack.find(layer);
    if (!layer.empty() && (!l || l->kind != tex::LayerKind::Paint))
        return std::nullopt;
    if (!l)
    {
        // As texAddLayer makes one: on top, named by kind and id, starting from the channels' defaults.
        tex::Layer added;
        added.kind = tex::LayerKind::Paint;
        added.id = stack->stack.newId('L');
        added.name = std::string(tex::layerKindName(added.kind)) + " " + added.id.substr(1);
        added.material = json::object();
        for (const char* c : {"baseColor", "roughness", "metallic", "height"})
            if (const tex::Channel* ch = settings.channel(c))
                added.material[c] = tex::channelDefault(c, ch->type);
        stack->stack.layers.push_back(std::move(added));
        l = &stack->stack.layers.back();
    }
    bool changed = layer.empty();
    if (!l->paint.is_object())
        l->paint = json::object();
    for (const std::string& c : channels)
    {
        const std::string ref = imageRef(l->id, c);
        if (l->paint.value(c, std::string()) != ref)
        {
            l->paint[c] = ref;
            changed = true;
        }
    }
    if (!changed)
        return std::nullopt;
    try
    {
        return tex::compileStack(stack->stack, settings, graph).graph;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

namespace
{

/** The first mesh using the set's material, for strokes that don't say which mesh they paint. */
std::string meshForSet(const Node& set)
{
    scene::Graph& g = graph();
    g.evaluate();
    const std::string material = setMaterial(set);
    std::string any;
    for (scene::NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n || !n->type().isA("mesh"))
            continue;
        scene::MeshPtr m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        if (!m || !m->numFaces())
            continue;
        if (any.empty())
            any = n->path();
        const auto& names = m->materialNames();
        if (std::find(names.begin(), names.end(), material) != names.end())
            return n->path();
    }
    return any;
}

glm::dmat4 worldOf(const Node& n) { return n.as<glm::dmat4>(scene::Transform::kWorldMatrix); }

glm::dmat4 matrixArg(const json& j, const char* what)
{
    if (!j.is_array() || j.size() != 16)
        throw CommandError(std::string("texPaintStroke: ") + what + " needs 16 numbers (column major)");
    glm::dmat4 m;
    for (int i = 0; i < 16; ++i)
        (&m[0][0])[i] = j[size_t(i)].get<double>();
    return m;
}

glm::dvec3 vecArg(const json& j, const char* what)
{
    if (!j.is_array() || j.size() != 3)
        throw CommandError(std::string("texPaintStroke: ") + what + " needs [x, y, z]");
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

/** A screen view from JSON: {width, height} and a camera node, view and proj matrices, viewProj, or eye/lookAt/fov. */
struct ScreenView
{
    glm::dmat4 viewProj{1.0};
    glm::dvec3 eye{0.0};
    double width = 1, height = 1;
};

ScreenView screenView(const json& v)
{
    if (!v.is_object())
        throw CommandError("texPaintStroke: screen strokes need view {width, height, and camera, view + proj, or eye + lookAt}");
    ScreenView s;
    s.width = std::max(1.0, v.value("width", 1920.0));
    s.height = std::max(1.0, v.value("height", 1080.0));
    const double aspect = s.width / s.height;
    if (v.contains("camera"))
    {
        Node* cam = graph().find(v["camera"].get<std::string>());
        if (!cam)
            throw CommandError("texPaintStroke: no camera " + v["camera"].get<std::string>());
        graph().evaluate();
        const glm::dmat4 world = worldOf(*cam);
        s.viewProj = scene::camera::projection(*cam, aspect) * scene::camera::viewMatrix(world);
        s.eye = glm::dvec3(world[3]);
    }
    else if (v.contains("viewProj"))
    {
        s.viewProj = matrixArg(v["viewProj"], "viewProj");
        s.eye = vecArg(v.at("eye"), "eye");
    }
    else if (v.contains("view") && v.contains("proj"))
    {
        const glm::dmat4 view = matrixArg(v["view"], "view");
        s.viewProj = matrixArg(v["proj"], "proj") * view;
        s.eye = glm::dvec3(glm::inverse(view)[3]);
    }
    else if (v.contains("eye") && v.contains("lookAt"))
    {
        s.eye = vecArg(v["eye"], "eye");
        const glm::dvec3 at = vecArg(v["lookAt"], "lookAt");
        const glm::dvec3 up = v.contains("up") ? vecArg(v["up"], "up") : glm::dvec3(0, 1, 0);
        const double fov = glm::radians(v.value("fov", 45.0));
        const double ty = std::tan(fov * 0.5);
        s.viewProj = scene::camera::projection(glm::dvec2(ty * aspect, ty), false, v.value("near", 0.1), 0.0) * glm::lookAt(s.eye, at, up);
    }
    else
        throw CommandError("texPaintStroke: view needs camera, viewProj + eye, view + proj, or eye + lookAt");
    return s;
}

/** Object-space ray through a pixel, as ViewInfo::ray does it. */
void pixelRay(const ScreenView& v, const glm::dmat4& toObject, glm::dvec2 px, glm::dvec3& o, glm::dvec3& d)
{
    const glm::dmat4 inv = glm::inverse(v.viewProj);
    const double x = px.x / v.width * 2.0 - 1.0, y = px.y / v.height * 2.0 - 1.0;
    const glm::dvec4 a = inv * glm::dvec4(x, y, 1.0, 1.0), b = inv * glm::dvec4(x, y, 0.5, 1.0);
    const glm::dvec3 nearP = glm::dvec3(a) / a.w, midP = glm::dvec3(b) / b.w;
    o = glm::dvec3(toObject * glm::dvec4(nearP, 1.0));
    d = glm::normalize(glm::dvec3(toObject * glm::dvec4(midP - nearP, 0.0)));
}

/** A screen view as a stroke view in the mesh's object space. */
StrokeView strokeViewOf(const ScreenView& v, const Node& meshNode)
{
    const glm::dmat4 world = worldOf(meshNode), toObject = glm::inverse(world);
    StrokeView out;
    out.objectToClip = glm::mat4(v.viewProj * world);
    out.eye = glm::vec3(toObject * glm::dvec4(v.eye, 1.0));
    out.width = int(v.width);
    out.height = int(v.height);
    glm::dvec3 o, d;
    pixelRay(v, toObject, {v.width * 0.5, v.height * 0.5}, o, d);
    out.direction = glm::vec3(d);
    return out;
}

/** Dabs for screen points (x, y, pressure): the shared brush sampler and rays onto the mesh (symmetry copies come after). */
std::vector<Dab> screenDabs(const json& points, const ScreenView& v, const Node& meshNode, const PaintMesh& mesh, const brush::BrushSettings& bs,
                            StrokeView& viewOut)
{
    if (!points.is_array() || points.empty())
        throw CommandError("texPaintStroke: screen strokes need points [[x, y, pressure?], ...]");
    const glm::dmat4 world = worldOf(meshNode), toObject = glm::inverse(world);
    const double scale = std::max(1e-12, glm::length(glm::dvec3(world[0])));
    viewOut = strokeViewOf(v, meshNode);
    auto query = [&](glm::dvec2 px, brush::SurfaceHit& out) {
        glm::dvec3 o, d;
        pixelRay(v, toObject, px, o, d);
        geo::BvhHit hit;
        if (!mesh.bvh->raycast(o, d, hit, std::numeric_limits<double>::max(), false) || hit.triangle >= mesh.triangleCount())
            return false;
        const glm::vec3* p = &mesh.positions[size_t(hit.triangle) * 3];
        const glm::dvec3 n = glm::cross(glm::dvec3(p[1] - p[0]), glm::dvec3(p[2] - p[0]));
        out.center = hit.point;
        out.normal = glm::length(n) > 1e-18 ? glm::normalize(n) : glm::dvec3(0, 1, 0);
        out.viewDir = d;
        out.eye = glm::dvec3(viewOut.eye);
        // Screen +x at the hit: the next pixel's ray at the same depth.
        glm::dvec3 o2, d2;
        pixelRay(v, toObject, px + glm::dvec2(1.0, 0.0), o2, d2);
        const double t = glm::dot(hit.point - o, d);
        const glm::dvec3 right = (o2 + d2 * t) - hit.point;
        out.right = glm::length(right) > 0.0 ? glm::normalize(right) : glm::dvec3(0.0);
        out.pixelSize = glm::length(right);
        out.worldToLocal = 1.0 / scale;
        return true;
    };

    struct Collect final : brush::DabSink
    {
        std::vector<brush::Dab> dabs;
        void dab(const brush::Dab& d) override { dabs.push_back(d); }
    } sink;
    auto input = [](const json& p) {
        if (!p.is_array() || p.size() < 2)
            throw CommandError("texPaintStroke: a point is [x, y] or [x, y, pressure]");
        return brush::StrokeInput{{p[0].get<double>(), p[1].get<double>()}, p.size() > 2 ? p[2].get<double>() : 1.0};
    };
    brush::Stroke stroke;
    if (!stroke.begin(bs, input(points[0]), query, sink, false))
        return {};
    for (size_t i = 1; i < points.size(); ++i)
        stroke.move(input(points[i]));
    stroke.end();

    std::vector<Dab> out;
    for (const brush::Dab& d : sink.dabs)
    {
        Dab r;
        if (recordDab(mesh, d, r))
            out.push_back(r);
    }
    return out;
}

/** Surface dabs given by position: snapped to the closest surface point. */
std::vector<Dab> positionDabs(const json& dabs, const PaintMesh& mesh, const brush::BrushSettings& bs)
{
    std::vector<Dab> out;
    for (const json& o : dabs)
    {
        brush::Dab d;
        d.center = vecArg(o.at("position"), "position");
        d.radius = o.value("radius", bs.worldRadius);
        d.pressure = o.value("pressure", 1.0);
        d.opacity = o.value("opacity", 1.0);
        d.flow = bs.flow;
        Dab r;
        if (!recordDab(mesh, d, r))
            throw CommandError("texPaintStroke: no paintable surface within the radius of " + o["position"].dump());
        r.angle = o.value("angle", 0.0f);
        out.push_back(r);
    }
    return out;
}

/** The triangle nearest an object-space point, for fills and clone sources picked by position. */
uint32_t triangleAt(const PaintMesh& mesh, const json& at, float* u = nullptr, float* v = nullptr)
{
    geo::BvhClosest hit;
    if (!mesh.bvh->closestPoint(vecArg(at, "at"), hit, std::numeric_limits<double>::max()) || hit.triangle >= mesh.triangleCount())
        throw CommandError("texPaintStroke: no surface near " + at.dump());
    if (u)
        *u = float(hit.bary.y);
    if (v)
        *v = float(hit.bary.z);
    return uint32_t(hit.triangle);
}

/** A fill's triangles from fillBy (mesh, island, material, polygon, triangle, selection) and at / triangle / faces / material. */
std::vector<uint32_t> fillTriangles(const json& pay, const PaintMesh& pm, const Node& meshNode)
{
    const std::string by = pay.value("fillBy", std::string("mesh"));
    auto seed = [&]() -> uint32_t {
        if (pay.contains("triangle"))
            return pay["triangle"].get<uint32_t>();
        if (pay.contains("at"))
            return triangleAt(pm, pay["at"]);
        throw CommandError("texPaintStroke: fillBy " + by + " needs at [x, y, z] (object space) or triangle");
    };
    if (by == "mesh")
        return pm.allTriangles();
    if (by == "island")
        return pm.islandOf(seed());
    if (by == "polygon")
    {
        const uint32_t face = pm.faces.at(seed());
        return pm.ofFaces(std::span<const uint32_t>(&face, 1));
    }
    if (by == "triangle")
    {
        const uint32_t t = seed();
        return pm.paintable.at(t) ? std::vector<uint32_t>{t} : std::vector<uint32_t>{};
    }
    if (by == "material")
    {
        if (pay.contains("material"))
        {
            scene::MeshPtr m = meshNode.as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
            const auto& names = m->materialNames();
            const auto it = std::find(names.begin(), names.end(), pay["material"].get<std::string>());
            if (it == names.end())
                throw CommandError("texPaintStroke: the mesh has no material " + pay["material"].get<std::string>());
            const int32_t slot = int32_t(it - names.begin());
            std::vector<uint32_t> out;
            for (size_t t = 0; t < pm.triangleCount(); ++t)
                if (pm.paintable[t] && pm.materials[t] == slot)
                    out.push_back(uint32_t(t));
            return out;
        }
        return pm.materialOf(seed());
    }
    if (by == "selection" || by == "faces")
    {
        std::vector<uint32_t> faces;
        if (pay.contains("faces"))
            faces = pay["faces"].get<std::vector<uint32_t>>();
        else
            faces = scene::components::selected(meshNode.id(), scene::ComponentMode::Face);
        if (faces.empty())
            throw CommandError("texPaintStroke: fillBy selection needs selected faces (or faces [...])");
        return pm.ofFaces(faces);
    }
    throw CommandError("texPaintStroke: fillBy is mesh, island, material, polygon, triangle or selection, not " + by);
}

/**
 * A projection's picture: a PaintImage document ({width, height, data}), the key of one the set already has, or an image
 * file ({path, rect [x, y, w, h], opacity, linear} or the path alone) laid onto a buffer the view's size.
 */
std::shared_ptr<const PaintImage> projectionImage(const json& img, const Node& set, const StrokeView& view)
{
    if (img.is_object() && img.contains("data"))
    {
        auto p = PaintImage::fromJson(img);
        if (!p)
            throw CommandError("texPaintStroke: the projection image needs width, height and data");
        return p;
    }
    if (img.is_string())
    {
        PaintPtr data = readPaint(set);
        if (auto it = data->images.find(img.get<std::string>()); it != data->images.end())
            return it->second;
    }
    const json spec = img.is_string() ? json{{"path", img}} : img;
    if (!spec.is_object() || !spec.contains("path"))
        throw CommandError("texPaintStroke: project payloads need image: a file path, {path, rect}, or {width, height, data}");
    const std::string path = spec["path"].get<std::string>();
    std::string err;
    image::Image pic = image::read(paths::fromUtf8(path), &err);
    if (pic.width <= 0)
        throw CommandError("texPaintStroke: can't read " + path + ": " + err);
    // 8-bit pictures hold sRGB colour; the channels are linear.
    std::string ext = paths::fromUtf8(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
    if (!spec.value("linear", ext == ".exr" || ext == ".hdr"))
        for (size_t i = 0; i < pic.rgba.size(); ++i)
            if (i % 4 != 3)
            {
                const float c = std::clamp(pic.rgba[i], 0.0f, 1.0f);
                pic.rgba[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
            }
    const double scale = std::min(1.0, 4096.0 / double(std::max(view.width, view.height)));
    ProjectionBuffer buf;
    buf.resize(std::max(1, int(view.width * scale)), std::max(1, int(view.height * scale)));
    glm::vec4 rect(0.0f);
    if (spec.contains("rect"))
    {
        const json& r = spec["rect"];
        if (!r.is_array() || r.size() != 4)
            throw CommandError("texPaintStroke: rect is [x, y, width, height] in view pixels");
        rect = glm::vec4(r[0].get<float>(), r[1].get<float>(), r[2].get<float>(), r[3].get<float>()) * float(scale);
    }
    buf.place(pic, rect, spec.value("opacity", 1.0f));
    const image::Image out = buf.resolved();
    return PaintImage::fromRgba(out.width, out.height, out.rgba);
}

/**
 * The symmetry a call asks for: symmetry {mirror: true | "x" | "y" | "z" | [nx, ny, nz, offset], radial {count, axis}}
 * when given, else the brush's mirror (across the mesh's symmetry plane, through its point pairs) and radial copies.
 */
tex::paint::PaintSymmetry symmetryOf(const json& j, const brush::BrushSettings& bs, const Node& meshNode)
{
    const int64_t attr = meshNode.as<int64_t>(scene::MeshNode::kSymmetry);
    int axis = attr >= 1 && attr <= 3 ? int(attr - 1) : 0;
    scene::MeshPtr mesh = meshNode.as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    brush::BrushSettings b = bs;
    if (!j.contains("symmetry") || !j["symmetry"].is_object())
        return tex::paint::brushSymmetry(b, mesh.get(), scene::paintSymmetrySpec(meshNode));
    if (j.contains("symmetry") && j["symmetry"].is_object())
    {
        const json& sj = j["symmetry"];
        const json mirror = sj.value("mirror", json());
        if (mirror.is_array() || sj.contains("group"))
            return tex::paint::PaintSymmetry::fromJson(sj);
        b.mirror = mirror.is_boolean() ? mirror.get<bool>() : mirror.is_string();
        if (mirror.is_string() && !mirror.get<std::string>().empty())
            if (const size_t a = std::string("xyz").find(char(std::tolower(static_cast<unsigned char>(mirror.get<std::string>()[0])))); a != std::string::npos)
                axis = int(a);
        b.radial = {};
        if (const auto r = sj.find("radial"); r != sj.end() && r->is_object())
        {
            b.radial.count = std::clamp(r->value("count", 1), 1, 64);
            b.radial.axis = std::clamp(r->value("axis", 1), 0, 2);
        }
    }
    return tex::paint::brushSymmetry(b, mesh.get(), axis, scene::symmetryOffset(meshNode, axis));
}

/** The fill's triangles with every symmetry copy's: islands and materials of the copies are filled whole. */
std::vector<uint32_t> symmetricFill(std::vector<uint32_t> tris, const std::string& by, const PaintMesh& pm, const tex::paint::PaintSymmetry& sym)
{
    if (!sym.any() || by == "mesh")
        return tris;
    std::vector<uint32_t> out = tris;
    for (uint32_t k = 1; k < sym.count(); ++k)
        for (uint32_t t : tris)
        {
            Dab c;
            if (!sym.copyPoint(pm, t, 1.0f / 3.0f, 1.0f / 3.0f, k, c))
                continue;
            if (std::binary_search(out.begin(), out.end(), c.triangle))
                continue;
            const uint32_t face = pm.faces[c.triangle];
            std::vector<uint32_t> add = by == "island"     ? pm.islandOf(c.triangle)
                                        : by == "material" ? pm.materialOf(c.triangle)
                                        : by == "polygon"  ? pm.ofFaces(std::span<const uint32_t>(&face, 1))
                                                           : std::vector<uint32_t>{c.triangle};
            out.insert(out.end(), add.begin(), add.end());
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
        }
    return out;
}

/** The stroke a texPaintStroke call describes, in surface space, with its mesh resolved. A projection's image comes back in image. */
Stroke strokeFromArgs(const Args& a, Node& set, std::shared_ptr<const PaintImage>* image = nullptr)
{
    json j = a.has("stroke") ? a.flags["stroke"] : json::object();
    if (j.is_string())
        j = json::parse(j.get<std::string>());
    if (!j.is_object())
        throw CommandError("texPaintStroke: stroke must be an object");
    for (const char* k : {"payload", "brush", "points", "view", "space", "mesh", "particles"})
        if (a.has(k) && !j.contains(k))
            j[k] = a.flags[k];
    if (!j.contains("payload"))
        j["payload"] = {{"channels", {{"baseColor", {1.0, 1.0, 1.0}}}}};
    json& pay = j["payload"];
    if (!pay.is_object())
        throw CommandError("texPaintStroke: payload must be an object");
    const std::string kind = pay.value("kind", std::string("paint"));
    const bool filter = kind == "smudge" || kind == "blur" || kind == "sharpen" || kind == "clone" || kind == "heal";
    // Filters with no channels named filter every channel of the set.
    if (filter && (!pay.contains("channels") || pay["channels"].empty()))
    {
        json ch = json::object();
        for (const tex::Channel& c : tex::readSettings(set).channels)
            ch[c.name] = 0.0;
        pay["channels"] = ch;
    }
    json projection;
    if (kind == "project")
    {
        if (!pay.contains("image"))
            throw CommandError("texPaintStroke: project payloads need image");
        projection = pay["image"];
        pay["image"] = "pending";
    }
    std::string space = j.value("space", std::string());
    if (space.empty())
        space = j.contains("points") ? "screen" : "surface";
    if (space != "surface" && space != "screen")
        throw CommandError("texPaintStroke: space is surface or screen");

    std::string mesh = j.value("mesh", std::string());
    if (mesh.empty())
        mesh = meshForSet(set);
    if (mesh.empty())
        throw CommandError("texPaintStroke: no mesh uses " + set.name() + "'s material; pass mesh=");
    Node* meshNode = graph().find(mesh);
    if (!meshNode)
        throw CommandError("texPaintStroke: no mesh " + mesh);
    std::shared_ptr<const PaintMesh> pm = paintMesh(graph(), mesh, setMaterial(set));
    if (!pm)
        throw CommandError("texPaintStroke: " + mesh + " isn't a mesh with faces");
    j["mesh"] = meshNode->path();

    brush::BrushSettings bs = brush::BrushSettings::fromJson(j.value("brush", json::object()));
    bs.ensureAlpha();
    const tex::paint::PaintSymmetry sym = symmetryOf(j, bs, *meshNode);
    // Dabs the command makes (from points or positions) get particles and symmetry copies; a record's dabs are final.
    bool generated = false;
    auto header = [&](json h) {
        h.erase("points");
        h.erase("view");
        h.erase("particles");
        try
        {
            return Stroke::fromJson(h);
        }
        catch (const std::exception& e)
        {
            throw CommandError(std::string("texPaintStroke: ") + e.what());
        }
    };
    Stroke s;
    if (kind == "fill" || kind == "project")
    {
        json h = j;
        h.erase("dabs");
        s = header(h);
        if (kind == "fill")
        {
            if (s.payload.fillBy.empty())
                s.payload.fillBy = pay.value("fillBy", std::string("mesh"));
            if (s.payload.triangles.empty())
                s.payload.triangles = symmetricFill(fillTriangles(pay, *pm, *meshNode), s.payload.fillBy, *pm, sym);
            s.view.reset();
        }
        else
        {
            if (!j.contains("view"))
                throw CommandError("texPaintStroke: a projection needs view {width, height, camera | view + proj | eye + lookAt}");
            s.view = strokeViewOf(screenView(j["view"]), *meshNode);
            std::shared_ptr<const PaintImage> img = projectionImage(projection, set, *s.view);
            s.payload.image = img->key();
            if (image)
                *image = img;
        }
    }
    else if (space == "screen")
    {
        json h = j;
        h.erase("dabs");
        s = header(h);
        StrokeView view;
        s.dabs = screenDabs(j.value("points", json()), screenView(j.value("view", json())), *meshNode, *pm, bs, view);
        s.view = view;
        generated = true;
    }
    else
    {
        const json dabs = j.value("dabs", json::array());
        const bool byPosition = dabs.is_array() && !dabs.empty() && dabs[0].is_object() && dabs[0].contains("position");
        json h = j;
        if (byPosition)
            h.erase("dabs");
        s = header(h);
        if (byPosition)
        {
            s.dabs = positionDabs(dabs, *pm, bs);
            generated = true;
        }
        for (const Dab& d : s.dabs)
            if (d.triangle >= pm->triangleCount())
                throw CommandError("texPaintStroke: triangle " + std::to_string(d.triangle) + " is past the mesh's " +
                                   std::to_string(pm->triangleCount()));
    }
    if (j.contains("particles") && j["particles"].is_object())
    {
        try
        {
            const tex::paint::ParticleSpec spec = tex::paint::ParticleSpec::fromJson(j["particles"]);
            if (!j["particles"].value("expanded", false))
                s.dabs = tex::paint::withParticles(*pm, s.dabs, 0, spec);
            s.particles = spec.toJson();
            s.particles["expanded"] = true;
        }
        catch (const std::invalid_argument& e)
        {
            throw CommandError(std::string("texPaintStroke: ") + e.what());
        }
    }
    if (generated && sym.any())
    {
        std::vector<Dab> all;
        for (const Dab& d : s.dabs)
        {
            std::vector<Dab> c = sym.copies(*pm, d);
            all.insert(all.end(), c.begin(), c.end());
        }
        s.dabs = std::move(all);
    }
    if (sym.any())
        s.symmetry = sym.toJson();
    if ((kind == "clone" || kind == "heal") && pay.contains("cloneFrom"))
    {
        if (s.dabs.empty())
            throw CommandError("texPaintStroke: cloneFrom is measured from the first dab, and the stroke has none");
        float u = 0.0f, v = 0.0f;
        const uint32_t t = triangleAt(*pm, pay["cloneFrom"], &u, &v);
        const Dab& d0 = s.dabs.front();
        s.payload.sourceOffset = pm->uv(t, u, v) - pm->uv(d0.triangle, d0.u, d0.v);
        // Each copy clones from the copy of the source, measured from its own first dab.
        s.payload.sourceOffsets.clear();
        for (uint32_t k = 0; k < sym.count(); ++k)
        {
            glm::vec2 off = s.payload.sourceOffset;
            Dab src;
            const auto first = std::find_if(s.dabs.begin(), s.dabs.end(), [&](const Dab& d) { return d.copy == k; });
            if (k > 0 && first != s.dabs.end() && sym.copyPoint(*pm, t, u, v, k, src))
                off = pm->uv(src.triangle, src.u, src.v) - pm->uv(first->triangle, first->u, first->v);
            s.payload.sourceOffsets.push_back(off);
        }
    }
    s.brush = bs.toJson();
    if (s.payload.channels.size() > 7)
        throw CommandError("texPaintStroke: a stroke paints at most 7 channels and the mask");
    if (s.dabs.empty() && kind != "fill" && kind != "project")
        throw CommandError("texPaintStroke: the stroke has no dabs on the surface");
    return s;
}

json strokeInfo(Node& set, const std::string& layer)
{
    json info = bufferInfo(set.id(), layer);
    PaintPtr data = readPaint(set);
    const Buffer* b = data->buffer(layer);
    info["layer"] = layer;
    info["strokes"] = b ? b->strokes.size() : 0;
    info["flattened"] = b && b->baseline;
    info["width"] = b ? b->width : 0;
    info["height"] = b ? b->height : 0;
    return info;
}

/** The set's paint data with one buffer replaced (or added), and an image a new stroke refers to; images nothing refers to go. */
PaintPtr withBuffer(const PaintData& d, const std::string& id, Buffer b, bool activate = true, std::shared_ptr<const PaintImage> image = nullptr)
{
    auto next = std::make_shared<PaintData>(d);
    next->buffers[id] = std::move(b);
    if (activate)
        next->active = id;
    if (image)
        next->images[image->key()] = image;
    next->pruneImages();
    return next;
}

json paintStroke(const Args& a)
{
    LILA_ZONE();
    Node& set = setArg(a, "texPaintStroke");
    std::shared_ptr<const PaintImage> image;
    Stroke s = strokeFromArgs(a, set, &image);
    const std::string layer = pickLayer(a, set);
    PaintPtr data = readPaint(set);
    Buffer buf = data->buffer(layer) ? *data->buffer(layer) : Buffer{};
    if (s.id == 0)
        s.id = uint64_t(std::max<int64_t>(0, a.get<int64_t>("id", 0)));
    if (s.id == 0)
    {
        uint64_t top = 0;
        for (const StrokePtr& p : buf.strokes)
            top = std::max(top, p->id);
        s.id = top + 1;
    }
    for (const StrokePtr& p : buf.strokes)
        if (p->id == s.id)
            throw CommandError("texPaintStroke: " + layer + " already has stroke " + std::to_string(s.id));
    // Every channel at once on a layer made unwired: the graph then changes on its first stroke only.
    {
        std::vector<std::string> wired = s.payload.written();
        for (const tex::Channel& c : tex::readSettings(set).channels)
            if (std::find(wired.begin(), wired.end(), c.name) == wired.end())
                wired.push_back(c.name);
        if (std::find(wired.begin(), wired.end(), std::string(kMaskChannel)) == wired.end())
            wired.push_back(kMaskChannel);
        wireLayer(set, layer, wired);
    }
    ApplyResult r;
    try
    {
        syncBuffer(graph(), set, layer);
        r = applyStroke(graph(), set, layer, s, image);
    }
    catch (const CommandError&)
    {
        throw;
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string("texPaintStroke: ") + e.what());
    }
    buf.strokes.push_back(std::make_shared<const Stroke>(std::move(s)));
    const uint64_t id = buf.strokes.back()->id;
    writePaint(graph(), set, withBuffer(*readPaint(set), layer, std::move(buf), true, image));
    return {{"layer", layer}, {"stroke", id}, {"dabs", r.dabs}, {"pages", r.pages}};
}
} // namespace

void registerCommands()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    installSync();
    // Texture exports and texEvaluate read paint layers from the pages, brought up to date first.
    tex::setExternalSource([](Node& set, const std::string& name, int udim) -> image::Image {
        std::string buffer, channel;
        if (!parseExternalName(name, &buffer, &channel))
            return {};
        syncBuffer(graph(), set, buffer);
        image::Image img = readChannel(set.id(), buffer, channel, udim);
        // A channel never painted covers nothing; a mask never painted masks nothing.
        if (img.width <= 0)
            img = channel == kMaskChannel ? image::Image{1, 1, {1.0f, 1.0f, 1.0f, 1.0f}} : image::Image{1, 1, {0.0f, 0.0f, 0.0f, 0.0f}};
        return img;
    });

    ops::registerCommand({
        .name = "texPaintStroke",
        .doc = "Paint a stroke into a texture set's paint layer (layer=, else the active one, else the stack's top paint layer, else a new "
               "paint layer). The stroke is a record: {mesh, brush (radius, strength, flow, spacing, falloff, roundness, angle, alpha, "
               "strokeMode line, lazyRadius, mirror...), payload {kind, channels {baseColor: [r, g, b], roughness: 0.3, ...}, blend "
               "normal|erase|add|subtract|multiply, mask}, occlusion, particles}, plus either space=surface with dabs [{triangle, bary [u, v], "
               "radius, pressure, opacity, angle}] or [{position [x, y, z], radius}] (object space), or space=screen with points [[x, y, "
               "pressure]] and view {width, height, camera | view + proj | viewProj + eye | eye + lookAt + fov}. Payload kinds: paint; "
               "direction (tilt, channel: normal, or another channel for a flow map); smudge (amount), blur and sharpen (filterRadius texels, "
               "amount), clone and heal (sourceOffset [du, dv] or cloneFrom [x, y, z]); fill (fillBy mesh|island|material|polygon|triangle|selection with at "
               "[x, y, z], triangle, material or faces; no dabs); project (image: a file path, {path, rect [x, y, w, h], opacity}, or "
               "{width, height, data}, through view; facingFade, colorChannel; no dabs); normal (normalMode bump|stamp, stampImage, intensity, "
               "channel). paint and fill take source {library id | graph | node type, values, resolution, scale} to paint a material's "
               "channels, and any kind tip {node | graph, values, resolution} for a texture graph brush tip. particles {kind drips|rain|sparks, "
               "count, length, size, spread, rate, seed} adds particle dabs. Each stroke is one undo step and is kept, so texRasterize can "
               "replay it. Returns {layer, stroke, dabs, pages}.",
        .positionalDoc = "set [layer]",
        .flags = {{"stroke", "", T::Any, nullptr, "The stroke record (an object, or its JSON text)."},
                  {"layer", "", T::String, nullptr, "The paint layer (its id)."},
                  nodeFlag({"mesh", "", T::String, nullptr, "The painted mesh; default the first using the set's material."}, "mesh"),
                  {"payload", "", T::Any, nullptr, "What the stroke writes, when stroke has none."},
                  {"brush", "", T::Any, nullptr, "Brush settings, when stroke has none."},
                  {"points", "", T::List, nullptr, "Screen points [[x, y, pressure]], when stroke has none."},
                  {"view", "", T::Any, nullptr, "Screen view, when stroke has none."},
                  {"space", "", T::String, nullptr, "surface or screen (default: screen when there are points).", {"surface", "screen"}},
                  {"particles", "", T::Any, nullptr, "Particle brush {kind, count, length, size, spread, rate, seed}, when stroke has none."},
                  {"id", "", T::Int, nullptr, "The stroke id the paint tool painted live under.", {}, {}, 0.0}},
        .run = paintStroke,
    });

    ops::registerCommand({
        .name = "texRasterize",
        .doc = "Re-rasterise a paint layer from its stroke records (and flattened pixels), at resolution= texels per side (0 follows the "
               "set's tiles) or its current size. Deterministic: the same records give the same pixels. Returns the layer's info.",
        .positionalDoc = "set [layer]",
        .flags = {{"layer", "", T::String, nullptr, "The paint layer."}, {"resolution", "r", T::Int, nullptr, "Texels per side; 0 follows the set.", {}, {}, 0.0, 16384.0}},
        .run = [](const Args& a) -> json {
            Node& set = setArg(a, "texRasterize");
            const std::string layer = requireLayer(a, set, "texRasterize");
            try
            {
                if (a.has("resolution"))
                {
                    const int r = a.get<int>("resolution");
                    if (r < 0 || r > 16384)
                        throw CommandError("texRasterize: resolution must be 0 to 16384");
                    PaintPtr data = readPaint(set);
                    Buffer b = *data->buffer(layer);
                    if (b.width != r || b.height != r)
                    {
                        b.width = b.height = r;
                        writePaint(graph(), set, withBuffer(*data, layer, std::move(b), false));
                    }
                }
                rasterize(graph(), set, layer);
            }
            catch (const CommandError&)
            {
                throw;
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("texRasterize: ") + e.what());
            }
            return strokeInfo(set, layer);
        },
    });

    ops::registerCommand({
        .name = "texFlattenLayer",
        .doc = "Flatten a paint layer: keep its pixels and drop its stroke records, so the layer no longer replays or re-rasterises them "
               "(the pixels resample instead).",
        .positionalDoc = "set [layer]",
        .flags = {{"layer", "", T::String, nullptr, "The paint layer."}},
        .run = [](const Args& a) -> json {
            Node& set = setArg(a, "texFlattenLayer");
            const std::string layer = requireLayer(a, set, "texFlattenLayer");
            try
            {
                syncBuffer(graph(), set, layer);
                PaintPtr data = readPaint(set);
                Buffer b = *data->buffer(layer);
                b.strokes.clear();
                b.baseline = capture(set.id(), layer);
                adopt(set.id(), layer, b);
                writePaint(graph(), set, withBuffer(*data, layer, std::move(b), false));
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("texFlattenLayer: ") + e.what());
            }
            return strokeInfo(set, layer);
        },
    });

    ops::registerCommand({
        .name = "texClearLayer",
        .doc = "Clear a paint layer: drop its strokes and pixels.",
        .positionalDoc = "set [layer]",
        .flags = {{"layer", "", T::String, nullptr, "The paint layer."}},
        .run = [](const Args& a) -> json {
            Node& set = setArg(a, "texClearLayer");
            const std::string layer = requireLayer(a, set, "texClearLayer");
            size_t pages = 0;
            try
            {
                syncBuffer(graph(), set, layer);
                pages = clearPages(set.id(), layer);
                PaintPtr data = readPaint(set);
                Buffer b = *data->buffer(layer);
                b.strokes.clear();
                b.baseline = nullptr;
                b.clears += 1;
                writePaint(graph(), set, withBuffer(*data, layer, std::move(b), false));
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("texClearLayer: ") + e.what());
            }
            return {{"layer", layer}, {"pages", pages}};
        },
    });

    ops::registerCommand({
        .name = "texBrushPresets",
        .doc = "Texture brush presets (.lilabrush) from every tier: built-in, extensions, the user's and the workspace's. name= returns one "
               "preset with its brush, payload and particles; otherwise a list of {id, name, category, tier, kind}.",
        .flags = {{"name", "n", T::String, nullptr, "One preset, by id or name."}, {"reload", "", T::Bool, nullptr, "Rescan the folders first."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.get<bool>("reload", false))
                tex::brushes::reload();
            if (a.has("name"))
            {
                auto p = tex::brushes::find(a.get<std::string>("name"));
                if (!p)
                    throw CommandError("texBrushPresets: no preset " + a.get<std::string>("name"));
                json j = p->toJson();
                j["id"] = p->id;
                j["tier"] = p->tier;
                return j;
            }
            json list = json::array();
            for (const tex::brushes::Preset& p : tex::brushes::all())
                list.push_back({{"id", p.id}, {"name", p.name}, {"category", p.category}, {"tier", p.tier}, {"kind", p.payload.value("kind", "paint")}});
            return {{"presets", list}, {"errors", tex::brushes::errors()}};
        },
    });

    ops::registerCommand({
        .name = "texSaveBrushPreset",
        .doc = "Save a texture brush preset to the user's (or the workspace's) brushes folder: brush settings, payload and particles, "
               "each optional. Returns {id, file}.",
        .positionalDoc = "name",
        .flags = {{"brush", "", T::Any, nullptr, "Brush settings."},
                  {"payload", "", T::Any, nullptr, "Payload (kind, channels, ...)."},
                  {"particles", "", T::Any, nullptr, "Particle brush settings."},
                  {"category", "", T::String, nullptr, "Where it lists."},
                  {"doc", "", T::String, nullptr, "What it is for."},
                  {"tier", "", T::String, nullptr, "user or workspace.", {"user", "workspace"}}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("texSaveBrushPreset: name required");
            tex::brushes::Preset p;
            p.name = a.positional[0].get<std::string>();
            p.id = p.name;
            p.category = a.get<std::string>("category", "");
            p.doc = a.get<std::string>("doc", "");
            if (a.has("brush"))
                p.brush = brush::BrushSettings::fromJson(a.flags["brush"]).toJson();
            if (a.has("payload"))
                p.payload = a.flags["payload"];
            if (a.has("particles"))
                p.particles = a.flags["particles"];
            try
            {
                const auto file = tex::brushes::save(p, a.get<std::string>("tier", "user"));
                return {{"id", p.id}, {"file", paths::toUtf8(file)}};
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("texSaveBrushPreset: ") + e.what());
            }
        },
    });

    ops::registerCommand({
        .name = "texPaintInfo",
        .doc = "A texture set's paint layers: per layer its strokes, whether it is flattened, its size and the GPU pages each channel and "
               "UDIM tile has allocated. layer= picks one.",
        .positionalDoc = "set [layer]",
        .flags = {{"layer", "", T::String, nullptr, "One paint layer."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Node& set = setArg(a, "texPaintInfo");
            PaintPtr data = readPaint(set);
            try
            {
                sync(graph(), set);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("texPaintInfo: ") + e.what());
            }
            const std::string layer = layerArg(a);
            if (!layer.empty())
                return strokeInfo(set, requireLayer(a, set, "texPaintInfo"));
            json layers = json::object();
            for (const auto& [id, b] : data->buffers)
                layers[id] = strokeInfo(set, id);
            return {{"active", data->active}, {"layers", layers}};
        },
    });
}
} // namespace lila::render::paint
