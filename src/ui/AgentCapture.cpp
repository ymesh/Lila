#include "ui/AgentCapture.h"

#include "ui/CurveUi.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "mcp/McpServer.h"
#include "render/Renderer.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"
#include "ui/Camera.h"
#include "ui/DebugDrawUi.h"
#include "ui/HeightfieldDisplay.h"
#include "ui/Environments.h"
#include "ui/ImagePlaneUi.h"
#include "ui/ViewInfo.h"
#include "ui/MaterialDraw.h"
#include "ui/MeshCache.h"
#include "ui/Overlay.h"
#include "ui/TexLive.h"
#include "ui/TextureCache.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportRenderers.h"
#include "ui/ViewportWidget.h"

#include <QApplication>
#include <QBuffer>
#include <QPainter>

#include <algorithm>
#include <cfloat>
#include <stdexcept>
#include <unordered_set>

namespace lila::ui::capture
{
namespace
{
ViewportWidget* activeViewport()
{
    if (!qobject_cast<QApplication*>(QCoreApplication::instance()))
        return nullptr;
    ViewportWidget* visible = nullptr;
    for (QWidget* top : QApplication::topLevelWidgets())
        for (ViewportWidget* v : top->findChildren<ViewportWidget*>())
            if (v->isVisible())
            {
                if (v->hasFocus())
                    return v;
                if (!visible)
                    visible = v;
            }
    return visible;
}

/** Meshes the capture draws: visible ones, or only the selection and its children when isolating. */
std::vector<scene::NodeId> shownMeshes(bool isolate)
{
    scene::Graph& g = scene::Graph::current();
    std::unordered_set<scene::NodeId> keep;
    if (isolate)
        for (scene::NodeId id : g.selection())
        {
            keep.insert(id);
            for (scene::NodeId d : g.descendants(id))
                keep.insert(d);
        }
    // Isolating with only components selected keeps the meshes they belong to.
    if (isolate && keep.empty())
        for (auto kind : {scene::ComponentMode::Vertex, scene::ComponentMode::Edge, scene::ComponentMode::Face})
            for (auto& [node, ids] : scene::components::selected(kind))
                keep.insert(node);
    std::vector<scene::NodeId> out;
    for (scene::NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!n->type().isA("mesh") || !scene::effectivelyVisible(g, id))
            continue;
        if (isolate && !keep.contains(id))
            continue;
        out.push_back(id);
    }
    return out;
}

struct Box
{
    glm::dvec3 lo{DBL_MAX}, hi{-DBL_MAX};
    bool empty() const { return lo.x > hi.x; }
    void add(const glm::dvec3& p)
    {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
};

Box worldBounds(const std::vector<scene::NodeId>& nodes, bool selectionOnly)
{
    scene::Graph& g = scene::Graph::current();
    Box box;
    std::unordered_set<scene::NodeId> selected(g.selection().begin(), g.selection().end());
    for (scene::NodeId id : nodes)
    {
        scene::Node* n = g.node(id);
        scene::MeshPtr m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        if (!m || m->numPoints() == 0)
            continue;
        glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        const auto& faces = scene::components::selected(id, scene::ComponentMode::Face);
        bool hasComponents = !faces.empty() || !scene::components::selected(id, scene::ComponentMode::Vertex).empty() ||
                             !scene::components::selected(id, scene::ComponentMode::Edge).empty();
        if (selectionOnly && hasComponents)
        {
            scene::MeshPtr cage = scene::modifiers::cage(g, id);
            if (cage)
                for (auto kind : {scene::ComponentMode::Vertex, scene::ComponentMode::Edge, scene::ComponentMode::Face})
                    for (uint32_t p : scene::components::convert(*cage, kind, scene::components::selected(id, kind), scene::ComponentMode::Vertex))
                        if (cage->pointValid(p))
                            box.add(glm::dvec3(w * glm::dvec4(cage->position(p), 1.0)));
            continue;
        }
        if (selectionOnly && !selected.contains(id))
            continue;
        geo::Bounds b = m->bounds();
        for (int i = 0; i < 8; ++i)
            box.add(glm::dvec3(w * glm::dvec4(i & 1 ? b.max.x : b.min.x, i & 2 ? b.max.y : b.min.y, i & 4 ? b.max.z : b.min.z, 1.0)));
    }
    return box;
}

void addSelection(render::FrameState& s, scene::NodeId id)
{
    scene::Graph& g = scene::Graph::current();
    scene::MeshPtr cage = scene::modifiers::cage(g, id);
    if (!cage)
        return;
    const geo::Mesh& m = *cage;
    glm::dmat4 world = g.node(id)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    auto wp = [&](uint32_t p) { return glm::vec3(world * glm::dvec4(m.position(p), 1.0)); };
    // Linear colors; the viewport's own selection colors would vanish on the light default material.
    const glm::vec4 fill(0.9f, 0.9f, 0.9f, 0.45f), hot(1.0f, 1.0f, 1.0f, 1.0f);
    for (uint32_t f : scene::components::selected(id, scene::ComponentMode::Face))
    {
        if (!m.faceValid(f))
            continue;
        std::vector<glm::vec3> poly;
        m.forEachFaceCorner(f, [&](geo::Index c) { poly.push_back(wp(m.cornerPoint(c))); });
        for (uint32_t p : scene::componentPick::fillTriangles(m, f))
            s.tris3d.push_back({wp(p), fill});
        for (size_t k = 0; k < poly.size(); ++k)
        {
            s.linesOnTop.push_back({poly[k], hot});
            s.linesOnTop.push_back({poly[(k + 1) % poly.size()], hot});
        }
    }
    for (uint32_t e : scene::components::selected(id, scene::ComponentMode::Edge))
        if (m.edgeValid(e))
        {
            s.linesOnTop.push_back({wp(m.edgePoint(e, 0)), hot});
            s.linesOnTop.push_back({wp(m.edgePoint(e, 1)), hot});
        }
    double tick = std::max(0.2, glm::length(m.bounds().size()) * 0.006);
    for (uint32_t p : scene::components::selected(id, scene::ComponentMode::Vertex))
        if (m.pointValid(p))
            for (int axis = 0; axis < 3; ++axis)
            {
                glm::vec3 d(0.0f);
                d[axis] = float(tick);
                s.linesOnTop.push_back({wp(p) - d, hot});
                s.linesOnTop.push_back({wp(p) + d, hot});
            }
}

void applyDisplayPrefs(render::FrameState& s, bool ortho)
{
    const PrefStore& p = PrefStore::get();
    auto bg = p.value("general", "/viewport/background", json::array({0.30, 0.30, 0.30}));
    if (bg.is_array() && bg.size() == 3)
        s.background = {bg[0].get<float>(), bg[1].get<float>(), bg[2].get<float>()};
    s.envPath = environments::resolve(p.value<std::string>("general", "/viewport/env/path", ""));
    s.envRotation = p.value<float>("general", "/viewport/env/rotation", 0.0f);
    s.envIntensity = environments::intensity();
    s.envBackground = p.value<bool>("general", "/viewport/env/background", true) && !ortho;
    s.envBlur = p.value<float>("general", "/viewport/env/blur", 0.6f);
    s.envSaturation = p.value<float>("general", "/viewport/env/saturation", 0.0f);
    s.envLightSaturation = p.value<float>("general", environments::kLightSaturation, 1.0f);
    s.envBackgroundExposure = p.value<float>("general", "/viewport/env/backgroundExposure", 0.25f);
    s.groundShadow = p.value<float>("general", "/viewport/groundShadow", 0.5f);
    s.ambientOcclusion = p.value<bool>("general", "/viewport/ao/enabled", true);
    s.aoRadius = p.value<float>("general", "/viewport/ao/radius", 30.0f);
    s.aoIntensity = p.value<float>("general", "/viewport/ao/intensity", 1.0f);
    s.aoFast = p.value<std::string>("general", "/viewport/ao/mode", "accurate") == "fast";
    s.aoRayTraced = p.value<std::string>("general", "/viewport/ao/mode", "accurate") == "raytraced";
    viewportDisplay::apply(s);
}

Camera cameraFor(const Options& o, double aspect, const Box& subject)
{
    Camera cam;
    cam.zUp = PrefStore::get().value<std::string>("general", "/scene/upAxis", "y") == "z";
    if (!o.lookThrough.empty())
    {
        const scene::Node* n = scene::Graph::current().find(o.lookThrough);
        if (!n || !n->type().isA("camera"))
            throw std::runtime_error("no camera '" + o.lookThrough + "'");
        cam.lookThrough(*n, n->as<glm::dmat4>(scene::Transform::kWorldMatrix));
        cam.overscan = 1.0;
        return cam;
    }
    std::string view = o.view;
    bool fromActive = false;
    if (view == "active")
    {
        if (ViewportWidget* vp = activeViewport())
        {
            cam = vp->camera();
            fromActive = true;
        }
        else
            view = "three-quarter";
    }
    if (view == "side")
        view = "right";
    if (!fromActive)
    {
        if (view == "persp" || view == "three-quarter" || view == "perspective")
        {
            cam.yaw = 35.0;
            cam.pitch = -20.0;
        }
        else if (view == "front" || view == "back" || view == "left" || view == "right" || view == "top" || view == "bottom")
            cam.setView(view);
        else
            throw std::runtime_error("unknown view '" + o.view + "' (active, persp, three-quarter, front, back, left, right, side, top, bottom)");
    }
    if (o.yaw || o.pitch)
    {
        cam.ortho = false;
        if (o.yaw)
            cam.yaw = *o.yaw;
        if (o.pitch)
            cam.pitch = std::clamp(*o.pitch, -89.9, 89.9);
    }
    // A wide gate fit keeps the whole subject in frame at any image aspect.
    cam.filmFit = scene::FilmFit::Fill;
    cam.overscan = 1.0;
    cam.gateAspect.reset();
    // The active view keeps the user's framing unless asked to zoom in on something.
    bool reframe = fromActive ? o.frameSelection || o.isolate : o.frame;
    if (reframe && !subject.empty())
    {
        glm::dvec3 centre = (subject.lo + subject.hi) * 0.5;
        double radius = std::max(0.5, glm::length(subject.hi - subject.lo) * 0.5);
        if (cam.ortho)
        {
            // Fit the box's extent across the view plane, not its bounding sphere, so silhouettes fill the frame.
            glm::dvec3 r = cam.right(), u = glm::cross(cam.right(), cam.forward());
            double halfW = 0.0, halfH = 0.0;
            for (int i = 0; i < 8; ++i)
            {
                glm::dvec3 c(i & 1 ? subject.hi.x : subject.lo.x, i & 2 ? subject.hi.y : subject.lo.y, i & 4 ? subject.hi.z : subject.lo.z);
                halfW = std::max(halfW, std::abs(glm::dot(c - centre, r)));
                halfH = std::max(halfH, std::abs(glm::dot(c - centre, u)));
            }
            cam.target = centre;
            // View extents scale linearly with orthoWidth; measure them at width 1 and scale to fit.
            cam.orthoWidth = 1.0;
            glm::dvec2 unit = cam.viewExtents(aspect);
            cam.orthoWidth = std::max({halfW / unit.x, halfH / unit.y, 1.0}) * 1.15;
            cam.distance = std::max(10000.0, radius * 4.0);
        }
        else
            cam.frame(centre, radius, aspect);
    }
    cam.nearClip = std::max(0.01, std::min(cam.distance * 0.001, 1.0));
    return cam;
}

/** Asks for every material map the meshes use and waits for the reads, so the picture has its textures on the first call. */
void waitForMaterialMaps(scene::Graph& g, const std::vector<scene::NodeId>& nodes)
{
    LILA_ZONE();

    for (scene::NodeId id : nodes)
        if (scene::MeshPtr m = g.node(id)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
            drawMaterials(*m);
    if (!texLive::waitIdle(30000.0))
        log::warn("capture: texture sets still evaluating after 30 s; captured as they stand");
    if (!textures::waitForLoads(30.0))
        log::warn("capture: material maps still loading after 30 s; captured without them");
}

std::string viewLabel(const Options& o)
{
    if (!o.lookThrough.empty())
        return o.lookThrough;
    if (o.yaw || o.pitch)
        return std::format("yaw {:.0f} pitch {:.0f}", o.yaw.value_or(35.0), o.pitch.value_or(-20.0));
    return o.view;
}
} // namespace

QImage render(const Options& o, Timing* timing, int repeat)
{
    LILA_ZONE();

    render::Renderer& r = render::Renderer::get();
    if (!r.running() && !r.start())
        throw std::runtime_error("the GPU renderer could not start");
    int w = std::clamp(o.width, 16, 4096), h = std::clamp(o.height, 16, 4096);
    double aspect = double(w) / double(h);

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    std::vector<scene::NodeId> nodes = shownMeshes(o.isolate);
    Box subject = worldBounds(nodes, o.frameSelection);
    if (subject.empty())
        subject = worldBounds(nodes, false);
    Camera cam = cameraFor(o, aspect, subject);

    render::FrameState s;
    s.view = cam.view();
    s.proj = cam.proj(aspect);
    s.eye = cam.eye();
    s.zUp = cam.zUp;
    applyDisplayPrefs(s, cam.ortho);
    s.grid = o.grid && !cam.ortho;
    if (o.ambientOcclusion)
        s.ambientOcclusion = *o.ambientOcclusion;

    bool showMaterials = materialsShown() && !viewportDisplay::defaultMaterial();
    if (showMaterials)
        waitForMaterialMaps(g, nodes);
    const heightfieldDisplay::Settings terrain = heightfieldDisplay::settings();
    if (terrain.drawsHeightfields() && showMaterials)
    {
        // The paint layers' maps load like material maps: ask for them all, then wait.
        int pending = 0;
        for (scene::NodeId id : nodes)
            heightfieldDisplay::drawFor(g, id, terrain, true, &pending);
        if (pending > 0 && !textures::waitForLoads(30.0))
            log::warn("capture: terrain layer maps still loading after 30 s; captured without them");
    }
    const std::unordered_set<scene::NodeId> selected(g.selection().begin(), g.selection().end());
    for (scene::NodeId id : nodes)
    {
        // Waiting for maps runs queued work, which can delete nodes gathered before it.
        scene::Node* n = g.node(id);
        if (!n)
            continue;
        if (terrain.drawsHeightfields())
            if (std::optional<render::HeightfieldDraw> hd = heightfieldDisplay::drawFor(g, id, terrain, showMaterials))
            {
                hd->world = glm::mat4(n->as<glm::dmat4>(scene::Transform::kWorldMatrix));
                hd->flags = (o.display != "wireframe" ? render::DrawShaded : 0u) | (o.display != "shaded" ? render::DrawWire : 0u);
                if (o.showSelection && selected.contains(id))
                    hd->flags |= render::DrawSelected;
                s.heightfields.push_back(std::move(*hd));
                continue;
            }
        int smooth = int(n->as<int64_t>(scene::MeshNode::kSmoothPreview));
        scene::MeshPtr shown = smoothedFor(n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh), smooth);
        bool textured = showMaterials && shown;
        auto rm = renderMeshFor(shown, textured && needsTangents(*shown));
        if (!rm)
            continue;
        render::MeshDraw d;
        d.mesh = std::move(rm);
        if (textured)
            d.materials = drawMaterials(*shown);
        d.world = glm::mat4(n->as<glm::dmat4>(scene::Transform::kWorldMatrix));
        if (viewportDisplay::vertexColorMode() != render::VertexColorMode::Off && !geo::colors::current(*shown).empty())
        {
            d.vertexColors = viewportDisplay::vertexColorMode();
            d.colorChannel = viewportDisplay::colorChannel();
        }
        d.flags = 0;
        if (o.display != "wireframe")
            d.flags |= render::DrawShaded;
        if (o.display != "shaded")
            d.flags |= render::DrawWire;
        s.meshes.push_back(std::move(d));
        // A smoothed or modified mesh also shows the cage being edited, as Lila's component mode does.
        if (o.display != "shaded")
        {
            scene::MeshPtr cage = scene::modifiers::cage(g, id);
            if (cage && cage != shown)
                if (auto crm = renderMeshFor(cage))
                {
                    render::MeshDraw c;
                    c.mesh = std::move(crm);
                    c.world = glm::mat4(n->as<glm::dmat4>(scene::Transform::kWorldMatrix));
                    c.flags = render::DrawWire;
                    s.meshes.push_back(std::move(c));
                }
        }
        if (o.showSelection)
            addSelection(s, id);
    }
    addDebugDraw(s);
    {
        ViewInfo view;
        view.view = s.view;
        view.proj = s.proj;
        view.eye = s.eye;
        view.width = w;
        view.height = h;
        view.camera = o.lookThrough.empty() && !o.yaw && !o.pitch ? (o.view == "side" ? "right" : o.view == "active" ? "persp" : o.view) : "persp";
        view.ortho = cam.ortho;
        view.film = cam.filmExtents();
        // Groom strands show as hair, as in the viewport.
        for (scene::NodeId id : g.nodes())
            if (scene::Node* n = g.node(id); n && n->type().isA("curve") && scene::effectivelyVisible(g, id))
                drawStrandsFor(s, *n, n->as<glm::dmat4>(scene::Transform::kWorldMatrix), view);
        // The first pass asks for the planes' pictures; the second draws them once they have loaded.
        imagePlanes::drawInto(s, view);
        textures::waitForLoads(30.0);
        s.imagePlanes.clear();
        imagePlanes::drawInto(s, view);
    }
    if (o.label && qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
    {
        OverlayPainter painter(s.overlayTris, 1.0);
        painter.label(QPointF(10, 22), QString::fromStdString(viewLabel(o)));
        s.atlas = overlayAtlas();
    }

    std::vector<uint8_t> bgra;
    std::string err;
    render::Renderer::FrameStats stats;
    if (!r.renderOffscreen(std::move(s), w, h, bgra, &err, &stats, repeat, std::max(o.samples, 1)))
        throw std::runtime_error("capture failed: " + err);
    if (timing)
        *timing = {stats.gpuMs, stats.aoMs, stats.cpuMs, stats.aoCpuMs};
    QImage img(bgra.data(), w, h, w * 4, QImage::Format_ARGB32);
    return img.convertToFormat(QImage::Format_RGB32);
}

namespace
{
QImage tile(const std::vector<QImage>& images, int columns)
{
    if (images.empty())
        return {};
    columns = std::clamp(columns, 1, int(images.size()));
    int rows = (int(images.size()) + columns - 1) / columns;
    int tw = images[0].width(), th = images[0].height();
    QImage sheet(tw * columns, th * rows, QImage::Format_RGB32);
    sheet.fill(QColor(20, 20, 20));
    QPainter p(&sheet);
    for (size_t i = 0; i < images.size(); ++i)
        p.drawImage(int(i % columns) * tw, int(i / columns) * th, images[i]);
    // One-pixel gutters so tiles read as separate views.
    p.setPen(QColor(20, 20, 20));
    for (int c = 1; c < columns; ++c)
        p.drawLine(c * tw, 0, c * tw, sheet.height());
    for (int r = 1; r < rows; ++r)
        p.drawLine(0, r * th, sheet.width(), r * th);
    return sheet;
}
} // namespace

QImage turntable(Options o, int count, double pitch, int columns, std::vector<std::string>* labels)
{
    count = std::clamp(count, 1, 16);
    std::vector<QImage> images;
    for (int i = 0; i < count; ++i)
    {
        o.view = "three-quarter";
        o.yaw = 360.0 * i / count;
        o.pitch = pitch;
        o.frame = true;
        images.push_back(render(o));
        if (labels)
            labels->push_back(viewLabel(o));
    }
    return tile(images, columns > 0 ? columns : count <= 4 ? count : (count + 1) / 2);
}

QImage sheet(Options o, const std::vector<std::string>& views, int columns)
{
    std::vector<QImage> images;
    for (const std::string& v : views)
    {
        o.view = v;
        o.yaw.reset();
        o.pitch.reset();
        o.frame = true;
        images.push_back(render(o));
    }
    return tile(images, columns > 0 ? columns : 2);
}

std::string png(const QImage& image)
{
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    if (!image.save(&buf, "PNG"))
        throw std::runtime_error("PNG encoding failed");
    return bytes.toStdString();
}

namespace
{
using T = ArgType;

std::vector<FlagDef> optionFlags()
{
    return {
        {"view", "v", T::String, "active", "active, persp, three-quarter, front, back, left, right, side, top, bottom.",
         {"active", "persp", "three-quarter", "front", "back", "left", "right", "side", "top", "bottom"}},
        nodeFlag({"lookThrough", "lt", T::String, "", "A scene camera to look through, framed as it is, instead of view."}, "camera"),
        {"yaw", "", T::Float, nullptr, "Perspective angle around the up axis, degrees (0 front, 90 right)."},
        {"pitch", "", T::Float, nullptr, "Perspective elevation, degrees (negative looks down)."},
        {"width", "w", T::Int, nullptr, "Pixels (512 for one view, 320 per tile).", {}, {}, 16.0, 4096.0},
        {"height", "h", T::Int, nullptr, "Pixels (512 for one view, 320 per tile).", {}, {}, 16.0, 4096.0},
        {"display", "d", T::String, "wireOnShaded", "shaded, wireframe or wireOnShaded.", {"shaded", "wireframe", "wireOnShaded"}},
        {"isolate", "", T::Bool, false, "Only the selected objects."},
        {"frameSelection", "fs", T::Bool, false, "Frame the selected objects or components instead of everything shown."},
        {"grid", "", T::Bool, true, "Ground grid in perspective views."},
        {"showSelection", "", T::Bool, true, "Tint selected components, and outline selected terrains drawn from their heights."},
        {"samples", "", T::Int, 1, "Jittered frames averaged into the image: 16 or more gives a supersampled still.", {}, {}, 1.0, 1024.0},
        {"label", "", T::Bool, true, "Write the view's name in the top-left corner."},
    };
}

Options optionsFrom(const json& j)
{
    Options o;
    auto str = [&](const char* a, const char* b, std::string& out) {
        if (j.contains(a) && j[a].is_string())
            out = j[a].get<std::string>();
        else if (b && j.contains(b) && j[b].is_string())
            out = j[b].get<std::string>();
    };
    str("view", "camera", o.view);
    str("display", nullptr, o.display);
    str("lookThrough", "look_through", o.lookThrough);
    if (j.value("wireframe", false))
        o.display = "wireframe";
    for (auto [key, out] : {std::pair{"yaw", &o.yaw}, {"pitch", &o.pitch}})
        if (j.contains(key) && j[key].is_number())
            *out = j[key].get<double>();
    auto num = [&](const char* key, int& out) {
        if (j.contains(key) && j[key].is_number())
            out = j[key].get<int>();
    };
    num("width", o.width);
    num("height", o.height);
    num("samples", o.samples);
    if (j.contains("resolution") && j["resolution"].is_number())
        o.width = o.height = j["resolution"].get<int>();
    auto flag = [&](std::initializer_list<const char*> keys, bool& out) {
        for (const char* k : keys)
            if (j.contains(k) && j[k].is_boolean())
                out = j[k].get<bool>();
    };
    flag({"isolate", "isolate_selection"}, o.isolate);
    flag({"frameSelection", "frame_selection"}, o.frameSelection);
    flag({"grid"}, o.grid);
    flag({"showSelection", "show_selection"}, o.showSelection);
    flag({"label"}, o.label);
    return o;
}

json optionSchema()
{
    return {
        {"view", {{"type", "string"},
                  {"description", "active (default: the user's current view), persp/three-quarter, front, back, left, right, side, top, bottom"}}},
        {"look_through", {{"type", "string"}, {"description", "A scene camera to look through, framed as it is; overrides view, yaw and pitch."}}},
        {"yaw", {{"type", "number"}, {"description", "Perspective orbit angle in degrees: 0 front, 90 right side, 180 back."}}},
        {"pitch", {{"type", "number"}, {"description", "Perspective elevation in degrees, negative looks down (default -20)."}}},
        {"width", {{"type", "integer"}, {"description", "Pixels (default 512, 320 per turntable tile: enough to judge shape)."}}},
        {"height", {{"type", "integer"}, {"description", "Pixels (default 512, 320 per turntable tile: enough to judge shape)."}}},
        {"display", {{"type", "string"}, {"enum", {"shaded", "wireframe", "wireOnShaded"}}, {"description", "Default wireOnShaded."}}},
        {"isolate_selection", {{"type", "boolean"}, {"description", "Show only the selected objects."}}},
        {"frame_selection", {{"type", "boolean"}, {"description", "Frame the selected objects/components rather than everything."}}},
        {"grid", {{"type", "boolean"}}},
        {"show_selection", {{"type", "boolean"}, {"description", "Highlight selected components (default true)."}}},
        {"samples", {{"type", "integer"}, {"description", "Jittered frames averaged into the image (default 1); 16+ for a supersampled still."}}},
    };
}

json imageContent(const QImage& img, const std::string& caption)
{
    QByteArray b64 = QByteArray::fromStdString(png(img)).toBase64();
    return json::array({{{"type", "image"}, {"data", b64.toStdString()}, {"mimeType", "image/png"}}, {{"type", "text"}, {"text", caption}}});
}

std::string sceneCounts()
{
    scene::Graph& g = scene::Graph::current();
    size_t meshes = 0, faces = 0, tris = 0;
    for (scene::NodeId id : g.nodes())
        if (scene::Node* n = g.node(id); n->type().isA("mesh") && scene::effectivelyVisible(g, id))
            if (scene::MeshPtr m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
            {
                ++meshes;
                faces += m->numFaces();
                for (geo::Index f = 0; f < m->faceCapacity(); ++f)
                    if (m->faceValid(f))
                        tris += m->faceSize(f) - 2;
            }
    return std::format("{} visible meshes, {} faces, {} triangles", meshes, faces, tris);
}

void savePng(const QImage& img, const std::string& path)
{
    if (!img.save(QString::fromStdString(path), "PNG"))
        throw CommandError("cannot write " + path);
}
} // namespace

void registerCommands()
{
    std::vector<FlagDef> flags = optionFlags();
    flags.push_back({"path", "p", T::String, nullptr, "PNG file to write."});
    ops::registerCommand({
        .name = "captureView",
        .doc = "Render the scene offscreen from a named view or angle and save a PNG. Never moves a viewport's camera, works with Lila in "
               "the background and in batch mode. Returns the path.",
        .flags = flags,
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (!a.has("path"))
                throw CommandError("captureView: give path=");
            json opts = a.flags;
            opts.erase("path");
            try
            {
                savePng(render(optionsFrom(opts)), a.get<std::string>("path"));
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("captureView: ") + e.what());
            }
            return a.get<std::string>("path");
        },
    });

    std::vector<FlagDef> mflags = optionFlags();
    mflags.push_back({"frames", "", T::Int, 20, "Frames to draw; the result is their median.", {}, {}, 1.0, 1000.0});
    mflags.push_back({"ambientOcclusion", "ao", T::Bool, nullptr, "Force ambient occlusion on or off instead of the viewport pref."});
    ops::registerCommand({
        .name = "measureView",
        .doc = "Draw the scene offscreen several times like captureView and return the median GPU time per frame in ms, with the "
               "ambient occlusion share, and the render thread's time recording it: {gpuMs, aoMs, cpuMs, aoCpuMs, frames}. 0 where the GPU has no timestamps.",
        .flags = mflags,
        .undoable = false,
        .run = [](const Args& a) -> json {
            Options o = optionsFrom(a.flags);
            o.label = false;
            if (a.has("ambientOcclusion"))
                o.ambientOcclusion = a.get<bool>("ambientOcclusion");
            int frames = std::clamp(a.get<int>("frames", 20), 1, 1000);
            Timing t;
            try
            {
                render(o, &t, frames);
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("measureView: ") + e.what());
            }
            return json{{"gpuMs", t.gpuMs}, {"aoMs", t.aoMs}, {"cpuMs", t.cpuMs}, {"aoCpuMs", t.aoCpuMs}, {"frames", frames}};
        },
    });

    ops::registerCommand({
        .name = "viewportStats",
        .doc = "The active viewport's last finished frame as the GPU measured it: {frames, gpuMs, aoMs, cpuMs, meshDrawCalls, meshUploads, renderer}. "
               "frames counts frames drawn so far, so two calls apart give the frame rate; meshUploads counts GPU mesh copies the same way. Unlike measureView it times what the "
               "view really draws, components and highlights included. When an external renderer such as a Hydra delegate draws "
               "the view, rendererStats holds what it reports.",
        .undoable = false,
        .run = [](const Args&) -> json {
            ViewportWidget* vp = activeViewport();
            if (!vp)
                throw CommandError("viewportStats: no viewport");
            const render::Renderer::FrameStats st = vp->frameStats();
            json out{{"frames", st.frames}, {"gpuMs", st.gpuMs}, {"aoMs", st.aoMs}, {"cpuMs", st.cpuMs}, {"meshDrawCalls", st.meshDrawCalls},
                     {"meshUploads", st.meshUploads}, {"renderer", viewportRenderers::selected(vp->hudPanel())}};
            if (json rs = viewportRenderers::stats(vp->hudPanel()); !rs.is_null())
                out["rendererStats"] = std::move(rs);
            return out;
        },
    });

    ops::registerCommand({
        .name = "viewportInspect",
        .doc = "What the active viewport draws and counts now: {hud: {scene, objects, components, uniqueTris, uniqueObjectTris}, size: [width, height], draws: "
               "{count, placements, nodes, flags}, picked}. picked, with pick, is what a click (two equal corners) or marquee there "
               "would select. For checking the view in tests and scripts.",
        .flags = {{"pick", "", T::List, nullptr, "Widget pixels: [x, y] for a click, or [x0, y0, x1, y1] for a marquee."},
                  {"path", "p", T::String, nullptr, "Also render the view's live frame (tools and their drawing included) offscreen to this PNG."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            ViewportWidget* vp = activeViewport();
            if (!vp)
                throw CommandError("viewportInspect: no viewport");
            if (a.has("path"))
            {
                const int w = std::max(vp->width(), 64), h = std::max(vp->height(), 64);
                std::vector<uint8_t> bgra;
                std::string err;
                if (!render::Renderer::get().renderOffscreen(vp->liveFrame(), w, h, bgra, &err))
                    throw CommandError("viewportInspect: render failed: " + err);
                QImage img(bgra.data(), w, h, w * 4, QImage::Format_ARGB32);
                if (!img.convertToFormat(QImage::Format_RGB32).save(QString::fromStdString(a.get<std::string>("path"))))
                    throw CommandError("viewportInspect: cannot write " + a.get<std::string>("path"));
            }
            std::optional<QRectF> pick;
            if (a.has("pick"))
            {
                const json p = a.flags["pick"];
                if (p.size() != 2 && p.size() != 4)
                    throw CommandError("viewportInspect: pick takes [x, y] or [x0, y0, x1, y1]");
                const QPointF from(p[0].get<double>(), p[1].get<double>());
                pick = QRectF(from, p.size() == 4 ? QPointF(p[2].get<double>(), p[3].get<double>()) : from);
            }
            return vp->inspect(pick);
        },
    });

    std::vector<FlagDef> tflags = optionFlags();
    tflags.push_back({"path", "p", T::String, nullptr, "PNG file to write."});
    tflags.push_back({"count", "c", T::Int, 4, "Views around the up axis, starting at the front.", {}, {}, 1.0, 16.0});
    tflags.push_back({"columns", "", T::Int, 0, "Tiles per row; 0 picks.", {}, {}, 0.0});
    tflags.push_back({"views", "", T::List, nullptr, "Named views to tile instead of a turn, e.g. ['front', 'right', 'top', 'persp']."});
    ops::registerCommand({
        .name = "captureTurntable",
        .doc = "Render several views offscreen and tile them into one PNG contact sheet (row by row): count angles around the subject, "
               "or the named views. Returns the path.",
        .flags = tflags,
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (!a.has("path"))
                throw CommandError("captureTurntable: give path=");
            json opts = a.flags;
            Options o = optionsFrom(opts);
            o.width = a.has("width") ? o.width : 320;
            o.height = a.has("height") ? o.height : 320;
            try
            {
                QImage img;
                if (a.has("views"))
                    img = sheet(o, a.flags["views"].get<std::vector<std::string>>(), a.get<int>("columns"));
                else
                    img = turntable(o, a.get<int>("count", 4), a.has("pitch") ? a.get<double>("pitch") : -15.0, a.get<int>("columns"));
                savePng(img, a.get<std::string>("path"));
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("captureTurntable: ") + e.what());
            }
            return a.get<std::string>("path");
        },
    });
}

void registerMcpTools()
{
    mcp::Server& s = mcp::Server::get();

    json props = optionSchema();
    s.addTool({
        .name = "viewport_capture",
        .description = "Look at your work: renders the scene offscreen and returns a PNG image. Works while Lila is in the background "
                       "and never moves the user's camera. Default view 'active' is what the user sees; use front/right/top for "
                       "orthographic silhouettes and isolate_selection/frame_selection to zoom in on a part. Wireframe is drawn "
                       "over shaded by default so you can judge topology.",
        .inputSchema = {{"type", "object"}, {"properties", props}},
        .runContent = [](const json& args) {
            Options o = optionsFrom(args);
            QImage img = render(o);
            return imageContent(img, std::format("{} view, {}x{}, {}. {}", viewLabel(o), img.width(), img.height(), o.display, sceneCounts()));
        },
    });

    json tprops = optionSchema();
    tprops["count"] = {{"type", "integer"}, {"description", "Angles around the subject (default 4: front, right, back, left)."}};
    tprops["columns"] = {{"type", "integer"}, {"description", "Tiles per row (default picks)."}};
    tprops["views"] = {{"type", "array"},
                       {"items", {{"type", "string"}}},
                       {"description", "Named views instead of a turn, e.g. [\"front\", \"right\", \"top\", \"persp\"]."}};
    tprops["pitch"] = {{"type", "number"}, {"description", "Elevation for the turn, degrees (default -15)."}};
    s.addTool({
        .name = "capture_turntable",
        .description = "Several views of the scene in one contact-sheet PNG, to judge silhouette and proportions from every side in one "
                       "call. Tiles run left to right, top to bottom; the text block lists them. Default 4 angles (front, right, "
                       "back, left) at 320px each, or pass views=[\"front\",\"right\",\"top\",\"persp\"].",
        .inputSchema = {{"type", "object"}, {"properties", tprops}},
        .runContent = [](const json& args) {
            Options o = optionsFrom(args);
            if (!args.contains("width") && !args.contains("resolution"))
                o.width = 320;
            if (!args.contains("height") && !args.contains("resolution"))
                o.height = 320;
            std::vector<std::string> labels;
            QImage img;
            int columns = args.value("columns", 0);
            if (args.contains("views") && args["views"].is_array())
            {
                labels = args["views"].get<std::vector<std::string>>();
                img = sheet(o, labels, columns);
            }
            else
                img = turntable(o, args.value("count", 4), args.value("pitch", -15.0), columns, &labels);
            std::string list;
            for (const std::string& l : labels)
                list += (list.empty() ? "" : ", ") + l;
            return imageContent(img, std::format("Tiles in order: {}. {}x{} overall. {}", list, img.width(), img.height(), sceneCounts()));
        },
    });
}
} // namespace lila::ui::capture
