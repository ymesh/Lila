#include "ui/ImagePlaneUi.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "core/Workspace.h"
#include "curve/RegionSplit.h"
#include "geo/Triangulate.h"
#include "media/Media.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/Camera.h"
#include "scene/ImagePlane.h"
#include "ui/Actions.h"
#include "ui/NodeLookup.h"
#include "ops/Undo.h"
#include "ui/ReferenceOverlay.h"
#include "ui/StepHistory.h"
#include "ui/ToolColors.h"
#include "ui/Animation.h"
#include "ui/Camera.h"
#include "ui/TextureCache.h"
#include "ui/VideoTextures.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportWidget.h"
#include "ui/FileDialog.h"
#include "scene/SceneIndex.h"

#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QMimeData>
#include <QPolygonF>
#include <QTimer>
#include <QUrl>

#include <filesystem>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>

namespace lila::ui::imagePlanes
{
namespace
{
namespace fs = std::filesystem;
namespace ip = scene::imagePlane;
using scene::ImagePlane;

constexpr const char* kHiddenKey = "/viewport/imagePlanes/hidden";
const char* const kImageFilter = "Images and Videos (*.png *.jpg *.jpeg *.tga *.hdr *.bmp *.tif *.tiff *.webp *.mp4 *.mov *.mkv *.webm *.avi *.m4v);;"
                                  "All Files (*)";
const glm::vec4 kSelectedColor(0.95f, 0.95f, 0.95f, 1.0f);

std::function<ViewportWidget*()>& activeView()
{
    static std::function<ViewportWidget*()> fn;
    return fn;
}

/** The last frame each perspective view drew with, so a plane riding that camera can show in the other views. */
std::map<std::string, ip::CameraFrame>& seenCameras()
{
    static std::map<std::string, ip::CameraFrame> m;
    return m;
}

struct Showing
{
    std::string file;
    render::TexturePtr image;
};

std::unordered_map<scene::NodeId, Showing>& showing()
{
    static std::unordered_map<scene::NodeId, Showing> m;
    return m;
}

bool zUp()
{
    return PrefStore::get().value<std::string>("general", "/scene/upAxis", "y") == "z";
}

/** The frame of a video plane at a scene time, from its start frame, fps override and frame offset. */
int64_t videoFrameOf(const scene::Node& n, const std::string& file, double time)
{
    const media::VideoInfo* info = videoTextures::info(file);
    if (!info)
        return 0;
    scene::Graph& g = scene::Graph::current();
    media::VideoTiming t;
    t.start = media::startSeconds(g, n.id(), n.as<double>(ImagePlane::kStartFrame));
    t.fps = n.as<double>(ImagePlane::kFps);
    t.offset = n.as<int64_t>(ImagePlane::kFrameOffset);
    return media::videoFrame(t, info->fps, time);
}

/** The picture a plane shows now: its file, the current frame of its sequence, or its video's frame. */
render::TexturePtr imageFor(const scene::Node& n, double time)
{
    const std::string& stored = n.as<std::string>(ImagePlane::kImageName);
    if (stored.empty())
        return nullptr;
    std::string file = resolve(stored);
    if (media::isVideoFile(file))
    {
        render::TexturePtr t = videoTextures::texture(file, videoFrameOf(n, file, time));
        static std::set<std::string> warnedVideo;
        if (!t && !videoTextures::error(file).empty() && warnedVideo.insert(file).second)
            log::warn("imagePlane {}: {}", n.path(), videoTextures::error(file));
        return t;
    }
    bool sequence = n.as<bool>(ImagePlane::kUseFrameExtension);
    if (sequence)
        file = ip::frameFile(file, std::llround(scene::anim::frameOf(time)) + n.as<int64_t>(ImagePlane::kFrameOffset));
    render::TexturePtr t = textures::get(file, true);
    Showing& s = showing()[n.id()];
    if (t)
    {
        if (sequence && !s.file.empty() && s.file != file)
            textures::release(s.file, true);
        s.file = file;
        s.image = t;
        return t;
    }
    static std::set<std::string> warned;
    if (std::string e = textures::error(file); !e.empty() && warned.insert(file).second)
        log::warn("imagePlane {}: {}: {}", n.path(), file, e);
    // A sequence holds its last frame on screen while the next one loads.
    return sequence ? s.image : nullptr;
}

ip::CameraFrame frameOf(const ViewInfo& v)
{
    glm::dmat4 inv = glm::inverse(v.view);
    ip::CameraFrame c;
    c.right = glm::normalize(glm::dvec3(inv[0]));
    c.up = glm::normalize(glm::dvec3(inv[1]));
    c.forward = -glm::normalize(glm::dvec3(inv[2]));
    c.eye = glm::dvec3(inv[3]);
    c.ortho = v.ortho;
    c.film = v.film;
    return c;
}

/** The camera of a view name as seen from view: orthographic views have a fixed frame, persp the one it last drew. */
std::optional<ip::CameraFrame> cameraFor(const std::string& camera, const ViewInfo& view)
{
    if (camera == view.camera)
        return frameOf(view);
    if (const scene::Node* cam = findNode(camera); cam && cam->type().isA("camera"))
        return scene::camera::frame(*cam, cam->as<glm::dmat4>(scene::Transform::kWorldMatrix));
    if (camera != "persp")
    {
        Camera c;
        c.zUp = zUp();
        c.setView(camera);
        ip::CameraFrame f;
        f.right = c.right();
        f.up = c.viewUp();
        f.forward = c.forward();
        f.eye = c.eye();
        f.ortho = true;
        f.film = c.filmExtents();
        return f;
    }
    auto it = seenCameras().find(camera);
    return it == seenCameras().end() ? std::nullopt : std::optional(it->second);
}

using St = glm::dvec2; // across the quad: (0, 0) its top left, (1, 1) its bottom right

/** A cut piece's region in quad coordinates, clipped to the quad. */
struct Shape
{
    std::vector<std::array<St, 3>> triangles;
    std::vector<std::vector<St>> outline;
};

struct Placed
{
    const scene::Node* node;
    ip::Quad quad;
    render::TexturePtr image;
    bool cut = false;
    Shape shape;
};

glm::dvec3 worldAt(const ip::Quad& q, St st)
{
    return q.center + q.halfU * (st.x * 2.0 - 1.0) + q.halfV * (1.0 - st.y * 2.0);
}

St stOf(const ip::Quad& q, const glm::dvec3& p)
{
    glm::dvec3 d = p - q.center;
    double u2 = glm::dot(q.halfU, q.halfU), v2 = glm::dot(q.halfV, q.halfV);
    return {0.5 + 0.5 * (u2 > 0.0 ? glm::dot(d, q.halfU) / u2 : 0.0), 0.5 - 0.5 * (v2 > 0.0 ? glm::dot(d, q.halfV) / v2 : 0.0)};
}

/** Sutherland-Hodgman against the unit square. */
std::vector<St> clipToQuad(std::vector<St> poly)
{
    for (int edge = 0; edge < 4 && !poly.empty(); ++edge)
    {
        const int axis = edge / 2;
        const bool low = edge % 2 == 0;
        auto inside = [&](St p) { return low ? p[axis] >= 0.0 : p[axis] <= 1.0; };
        std::vector<St> out;
        for (size_t i = 0; i < poly.size(); ++i)
        {
            St a = poly[i], b = poly[(i + 1) % poly.size()];
            if (inside(a))
                out.push_back(a);
            if (inside(a) != inside(b))
                out.push_back(glm::mix(a, b, ((low ? 0.0 : 1.0) - a[axis]) / (b[axis] - a[axis])));
        }
        poly = std::move(out);
    }
    return poly;
}

Shape shapeOf(const ip::Region& region, const glm::dmat4& world, const ip::Quad& q)
{
    Shape s;
    std::vector<geo::Index> tris;
    for (const auto& loop : region)
    {
        std::vector<St> st;
        st.reserve(loop.size());
        for (const glm::dvec2& p : loop)
            st.push_back(stOf(q, glm::dvec3(world * glm::dvec4(p, 0.0, 1.0))));
        double area = 0.0;
        for (size_t i = 0; i < st.size(); ++i)
            area += st[i].x * st[(i + 1) % st.size()].y - st[(i + 1) % st.size()].x * st[i].y;
        if (area < 0.0)
            std::ranges::reverse(st);
        tris.clear();
        geo::earClip(st, tris);
        for (size_t t = 0; t + 2 < tris.size(); t += 3)
        {
            std::vector<St> piece = clipToQuad({st[tris[t]], st[tris[t + 1]], st[tris[t + 2]]});
            for (size_t k = 1; k + 1 < piece.size(); ++k)
                s.triangles.push_back({piece[0], piece[k], piece[k + 1]});
        }
        if (std::vector<St> line = clipToQuad(st); line.size() >= 3)
            s.outline.push_back(std::move(line));
    }
    return s;
}

/** The background to clear: the top left texel's display color, when the picture is 8-bit and that corner is opaque. */
glm::vec4 keyOf(const render::TextureImage& t, float tolerance)
{
    if (t.format != render::TextureFormat::RGBA8 || t.blocks || t.rgba.size() < 4 || t.rgba[3] < 128 || tolerance <= 0.0f)
        return glm::vec4(0.0f);
    return {t.rgba[0] / 255.0f, t.rgba[1] / 255.0f, t.rgba[2] / 255.0f, tolerance};
}

/** The loops a plane is outlined and picked by: its quad, or a cut piece's region. */
std::vector<std::vector<glm::dvec3>> outlineOf(const Placed& p)
{
    if (!p.cut)
    {
        auto c = p.quad.corners();
        return {{c.begin(), c.end()}};
    }
    std::vector<std::vector<glm::dvec3>> out;
    for (const auto& loop : p.shape.outline)
    {
        auto& w = out.emplace_back();
        for (St st : loop)
            w.push_back(worldAt(p.quad, st));
    }
    return out;
}

/** Every image plane that shows in the view, laid out. */
std::vector<Placed> placedIn(const ViewInfo& view)
{
    std::vector<Placed> out;
    if (!shownIn(view.camera))
        return out;
    scene::Graph& g = scene::Graph::current();
    for (const scene::Node* n : scene::sceneIndex::ofType(g, "imagePlane"))
    {
        const scene::NodeId id = n->id();
        if (!scene::effectivelyVisible(g, id) || !media::isLive(g, id))
            continue;
        std::string camera = ip::cameraName(n->as<std::string>(ImagePlane::kCamera));
        std::optional<ip::CameraFrame> frame;
        if (!camera.empty())
        {
            if (camera != view.camera && ip::DisplayMode(n->as<int64_t>(ImagePlane::kDisplayMode)) != ip::DisplayMode::AllViews)
                continue;
            if (!(frame = cameraFor(camera, view)))
                continue;
        }
        render::TexturePtr image = imageFor(*n, g.time());
        if (!image || image->width <= 0 || image->height <= 0)
            continue;
        double aspect = double(image->width) / double(image->height);
        ip::Settings s = ip::settings(*n);
        const glm::dmat4& world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        ip::Quad q = frame ? ip::layoutAttached(s, *frame, aspect) : ip::layoutFree(s, world, aspect);
        Placed& p = out.emplace_back(Placed{n, q, std::move(image)});
        // Only free planes are cut: the region is in their local frame.
        if (ip::Region r = frame ? ip::Region() : ip::region(*n); !r.empty())
        {
            p.cut = true;
            p.shape = shapeOf(r, world, q);
        }
    }
    return out;
}

render::ImagePlaneDraw::Depth depthOf(const scene::Node& n)
{
    switch (ip::Placement(n.as<int64_t>(ImagePlane::kPlacement)))
    {
    case ip::Placement::InScene: return render::ImagePlaneDraw::Depth::Scene;
    case ip::Placement::InFront: return render::ImagePlaneDraw::Depth::Front;
    default: return render::ImagePlaneDraw::Depth::Behind;
    }
}

void draw(render::FrameState& s, OverlayPainter&, const ViewInfo& view)
{
    if (!view.ortho)
        seenCameras()[view.camera] = frameOf(view);
    imagePlanes::drawInto(s, view);
}

std::optional<glm::dmat4> cutMatrix(const scene::Node& n, const ViewInfo& view);

const glm::vec4 kGuideColor(0.95f, 0.78f, 0.3f, 0.9f);

/** Guides are a reposing aid: they show only while the cut or guide tool is active. */
bool guidesShown()
{
    const ViewportTool* t = viewportTools::active();
    return t && (std::string_view(t->name()) == "Cut Image Plane" || std::string_view(t->name()) == "Image Plane Guides");
}

/** Not a piece cut from another plane: guides live on these. */
bool isRootPlane(const scene::Graph& g, const scene::Node& n)
{
    const scene::Node* parent = g.node(g.parent(n.id()));
    return !parent || !parent->type().isA("imagePlane");
}

/** One guide as drawn: from its start through its end to the picture's edge, world space. */
struct GuideLine
{
    glm::dvec3 start{0.0};
    glm::dvec3 end{0.0};
    glm::dvec3 edge{0.0};
    double angle = 0.0; // degrees from the plane's horizontal, 0 to 90
};

/** Folds a direction in the plane into its angle from horizontal, 0 to 90 degrees either way. */
double angleFromHorizontal(glm::dvec2 d)
{
    double a = std::abs(glm::degrees(std::atan2(d.y, d.x)));
    return a > 90.0 ? 180.0 - a : a;
}

/** Runs from a point through another to the edge of the unit square, in quad coordinates. */
St toQuadEdge(St from, St through)
{
    const St d = through - from;
    double t = 1e300;
    for (int axis = 0; axis < 2; ++axis)
        if (std::abs(d[axis]) > 1e-12)
            for (double bound : {0.0, 1.0})
                if (double s = (bound - from[axis]) / d[axis]; s > 1e-9)
                    t = std::min(t, s);
    return t < 1e300 ? from + d * std::max(t, 1.0) : through;
}

std::vector<GuideLine> guidesOf(const Placed& p, const ViewInfo& view)
{
    std::vector<GuideLine> out;
    const std::optional<glm::dmat4> m = cutMatrix(*p.node, view);
    if (!m)
        return out;
    for (const glm::dvec4& l : ip::guides(*p.node))
    {
        GuideLine g;
        g.start = glm::dvec3(*m * glm::dvec4(l.x, l.y, 0.0, 1.0));
        g.end = glm::dvec3(*m * glm::dvec4(l.z, l.w, 0.0, 1.0));
        g.edge = worldAt(p.quad, toQuadEdge(stOf(p.quad, g.start), stOf(p.quad, g.end)));
        g.angle = angleFromHorizontal({l.z - l.x, l.w - l.y});
        out.push_back(g);
    }
    return out;
}
} // namespace

void drawInto(render::FrameState& s, const ViewInfo& view)
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    std::vector<Placed> placed = placedIn(view);
    const bool showGuides = guidesShown();
    // Drawn in order, so later ones cover earlier ones in the same placement.
    std::vector<int64_t> order;
    for (const Placed& p : placed)
        order.push_back(ip::drawOrder(g, *p.node));
    std::vector<size_t> sorted(placed.size());
    std::iota(sorted.begin(), sorted.end(), size_t(0));
    std::ranges::stable_sort(sorted, [&](size_t a, size_t b) { return order[a] < order[b]; });
    for (size_t index : sorted)
    {
        const Placed& p = placed[index];
        const scene::Node& n = *p.node;
        render::ImagePlaneDraw d;
        auto corners = p.quad.corners();
        for (int i = 0; i < 4; ++i)
            d.corners[i] = corners[size_t(i)];
        d.uv = glm::vec4(p.quad.uv);
        d.image = p.image;
        d.opacity = float(n.as<double>(ImagePlane::kAlphaGain));
        d.brightness = float(n.as<double>(ImagePlane::kBrightness));
        d.contrast = float(n.as<double>(ImagePlane::kContrast));
        d.grayscale = n.as<int64_t>(ImagePlane::kDisplayColor) == 1;
        d.depth = depthOf(n);
        if (n.as<bool>(ImagePlane::kKeyBackground))
            d.key = keyOf(*p.image, float(n.as<double>(ImagePlane::kKeyTolerance)));
        if (p.cut)
        {
            d.cornerUvs = true;
            const glm::dvec2 uv0(p.quad.uv.x, p.quad.uv.y), uv1(p.quad.uv.z, p.quad.uv.w);
            for (const auto& tri : p.shape.triangles)
            {
                for (size_t i = 0; i < 4; ++i)
                {
                    St st = tri[std::min<size_t>(i, 2)];
                    d.corners[i] = worldAt(p.quad, st);
                    d.cornerUv[i] = glm::vec2(glm::mix(uv0, uv1, st));
                }
                s.imagePlanes.push_back(d);
            }
        }
        else
            s.imagePlanes.push_back(std::move(d));
        if (g.isSelected(n.id()))
            for (const auto& loop : outlineOf(p))
                for (size_t i = 0; i < loop.size(); ++i)
                {
                    s.linesOnTop.push_back({glm::dvec3(loop[i]), kSelectedColor});
                    s.linesOnTop.push_back({glm::dvec3(loop[(i + 1) % loop.size()]), kSelectedColor});
                }
        if (showGuides && isRootPlane(g, n))
            for (const GuideLine& l : guidesOf(p, view))
            {
                s.linesOnTop.push_back({glm::dvec3(l.start), kGuideColor});
                s.linesOnTop.push_back({glm::dvec3(l.edge), kGuideColor});
            }
    }
}

namespace
{
int depthIn(const scene::Graph& g, scene::NodeId id)
{
    int d = 0;
    for (scene::NodeId p = g.parent(id); p != scene::kNoNode; p = g.parent(p))
        ++d;
    return d;
}

double segDistance(QPointF p, QPointF a, QPointF b)
{
    QPointF d = b - a;
    double len2 = QPointF::dotProduct(d, d);
    double t = len2 > 0.0 ? std::clamp(QPointF::dotProduct(p - a, d) / len2, 0.0, 1.0) : 0.0;
    QPointF q = a + d * t - p;
    return std::hypot(q.x(), q.y());
}

/**
 * A click picks a plane by its outline; its inside only when it draws in front, since the viewport can't tell here
 * whether a mesh is under the cursor. Locked planes never pick.
 */
std::vector<viewportHooks::PickHit> pick(QPointF a, QPointF b, bool click, const ViewInfo& view)
{
    LILA_ZONE();

    std::vector<viewportHooks::PickHit> hits;
    QRectF box = QRectF(a, b).normalized();
    for (const Placed& p : placedIn(view))
    {
        if (p.node->as<bool>(ImagePlane::kLocked))
            continue;
        std::vector<QPolygonF> polys;
        for (const auto& loop : outlineOf(p))
        {
            QPolygonF poly;
            for (const glm::dvec3& c : loop)
                if (auto px = view.project(c))
                    poly << *px;
            if (poly.size() == qsizetype(loop.size()))
                polys.push_back(std::move(poly));
        }
        if (polys.empty())
            continue;
        if (!click)
        {
            if (std::ranges::any_of(polys, [&](const QPolygonF& poly) { return std::ranges::any_of(poly, [&](const QPointF& c) { return box.contains(c); }); }))
                hits.push_back({p.node->path(), 0.0});
            continue;
        }
        double best = 1e300;
        bool inside = false;
        for (const QPolygonF& poly : polys)
        {
            for (qsizetype i = 0; i < poly.size(); ++i)
                best = std::min(best, segDistance(a, poly[i], poly[(i + 1) % poly.size()]));
            inside = inside || poly.containsPoint(a, Qt::OddEvenFill);
        }
        // Pieces share their cut edges: a click on the seam takes the one further down the chain, the limb.
        const double bias = p.cut ? 0.01 * double(std::min(depthIn(scene::Graph::current(), p.node->id()), 50)) : 0.0;
        if (best < 6.0)
            hits.push_back({p.node->path(), best - bias});
        else if (depthOf(*p.node) == render::ImagePlaneDraw::Depth::Front && inside)
            hits.push_back({p.node->path(), 5.0 - bias});
    }
    return hits;
}

/** A plane per file, in one undo step, all of them selected. */
void create(const std::vector<std::string>& files, const std::string& camera)
{
    if (files.empty())
        return;
    UndoChunk chunk(files.size() > 1 ? "Create Image Planes" : "Create Image Plane");
    json made = json::array();
    for (const std::string& file : files)
    {
        json call = {{"fileName", file}, {"skipSelect", true}};
        if (!camera.empty())
            call["camera"] = camera;
        try
        {
            made.push_back(ops::execute("imagePlane", call, CallSource::UI));
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
    }
    if (!made.empty())
        ops::execute("select", {{"args", made}}, CallSource::UI);
}

std::vector<std::string> droppedImages(const QMimeData* mime)
{
    static const QStringList kSuffixes = {"png", "jpg", "jpeg", "tga", "hdr", "bmp", "tif", "tiff", "webp", "mp4", "mov", "mkv", "webm", "avi", "m4v"};
    std::vector<std::string> out;
    if (!mime || !mime->hasUrls())
        return out;
    for (const QUrl& url : mime->urls())
        if (url.isLocalFile() && kSuffixes.contains(QFileInfo(url.toLocalFile()).suffix().toLower()))
            out.push_back(url.toLocalFile().toStdString());
    return out;
}

class DropFilter final : public QObject
{
public:
    explicit DropFilter(ViewportWidget* view) : QObject(view), mView(view) {}

    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() == QEvent::DragEnter || e->type() == QEvent::DragMove)
        {
            auto* d = static_cast<QDragMoveEvent*>(e);
            if (droppedImages(d->mimeData()).empty() || (e->type() == QEvent::DragMove && hookTakes(d)))
                return QObject::eventFilter(obj, e);
            d->acceptProposedAction();
            return true;
        }
        if (e->type() == QEvent::Drop)
        {
            auto* d = static_cast<QDropEvent*>(e);
            std::vector<std::string> files = droppedImages(d->mimeData());
            if (files.empty() || hookTakes(d))
                return QObject::eventFilter(obj, e);
            d->acceptProposedAction();
            mView->setFocus(Qt::OtherFocusReason);
            // Alt drops reference overlays, glued to the screen, instead of camera-attached planes.
            if (d->modifiers() & Qt::AltModifier)
                for (const std::string& f : files)
                    referenceOverlays::show(f, mView->viewName());
            else
                create(files, mView->viewName());
            return true;
        }
        return QObject::eventFilter(obj, e);
    }

private:
    /** A drop handler wants the images where the cursor is (a texture onto a mesh); Alt still drops an overlay. */
    bool hookTakes(const QDropEvent* d) const
    {
        return !(d->modifiers() & Qt::AltModifier) && d->mimeData() && viewportHooks::handleDrop(*d->mimeData(), mView->dropTarget(d->position()), false);
    }

    ViewportWidget* mView;
};

bool shapeContains(const Shape& s, St p)
{
    bool inside = false;
    for (const auto& loop : s.outline)
        for (size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++)
            if ((loop[i].y > p.y) != (loop[j].y > p.y) && p.x < (loop[j].x - loop[i].x) * (p.y - loop[i].y) / (loop[j].y - loop[i].y) + loop[i].x)
                inside = !inside;
    return inside;
}

bool insidePiece(const Placed& p, St st)
{
    return p.cut ? shapeContains(p.shape, st) : st.x >= 0.0 && st.x <= 1.0 && st.y >= 0.0 && st.y <= 1.0;
}

/** The piece imagePlaneCut works on for a joint: the deepest one shown there. */
const Placed* pieceUnder(const std::vector<Placed>& placed, const glm::dvec3& joint)
{
    const Placed* best = nullptr;
    int bestDepth = -1;
    const scene::Graph& g = scene::Graph::current();
    for (const Placed& p : placed)
    {
        const glm::dvec3 n = p.quad.normal();
        if (std::abs(glm::dot(joint - p.quad.center, n)) > 0.5 || !insidePiece(p, stOf(p.quad, joint)))
            continue;
        if (int d = depthIn(g, p.node->id()); d >= bestDepth)
            best = &p, bestDepth = d;
    }
    return best;
}

/** The world matrix a plane is cut in: its own, or for one on an orthographic view the free plane's it becomes. */
std::optional<glm::dmat4> cutMatrix(const scene::Node& n, const ViewInfo& view)
{
    const std::string cam = ip::cameraName(n.as<std::string>(ImagePlane::kCamera));
    if (cam.empty())
        return n.as<glm::dmat4>(scene::Transform::kWorldMatrix);
    const std::optional<ip::CameraFrame> f = cameraFor(cam, view);
    if (!f || !f->ortho)
        return std::nullopt;
    const glm::dvec3 r = glm::normalize(f->right), u = glm::normalize(f->up);
    glm::dmat4 m(1.0);
    m[0] = glm::dvec4(r, 0.0);
    m[1] = glm::dvec4(u, 0.0);
    m[2] = glm::dvec4(glm::cross(r, u), 0.0);
    m[3] = glm::dvec4(glm::normalize(f->forward) * n.as<double>(ImagePlane::kDepth), 1.0);
    return m;
}

double aspectOf(const Placed& p)
{
    return p.image && p.image->height > 0 ? double(p.image->width) / double(p.image->height) : 0.0;
}

/** Each end's run-on as imagePlaneCut makes it on piece p: {end, where it runs to}, world space. */
std::vector<std::pair<glm::dvec3, glm::dvec3>> runOns(const Placed& p, const std::vector<glm::dvec3>& world, const ViewInfo& view)
{
    std::vector<std::pair<glm::dvec3, glm::dvec3>> out;
    const std::optional<glm::dmat4> m = cutMatrix(*p.node, view);
    if (!m || world.size() < 2)
        return out;
    const glm::dmat4 inv = glm::inverse(*m);
    curve::RegionLoop cut;
    for (const glm::dvec3& w : world)
        cut.emplace_back(glm::dvec2(inv * glm::dvec4(w, 1.0)));
    const glm::dvec3 pivot = p.node->as<glm::dvec3>(scene::Transform::kRotatePivot);
    const curve::RegionLoop path = curve::runOnCut(ip::shownRegion(*p.node, aspectOf(p)), cut, {pivot.x, pivot.y}, 0.0);
    auto toWorld = [&](glm::dvec2 v) { return glm::dvec3(*m * glm::dvec4(v, 0.0, 1.0)); };
    if (path.front() != cut.front())
        out.push_back({world.front(), toWorld(path.front())});
    if (path.back() != cut.back())
        out.push_back({world.back(), toWorld(path.back())});
    return out;
}

/** An orthographic camera's plane as the free plane it shows as, so it can be cut. */
void freePlane(const scene::Node& n, const ViewInfo& view)
{
    const std::optional<glm::dmat4> m = cutMatrix(n, view);
    if (!m || ip::cameraName(n.as<std::string>(ImagePlane::kCamera)).empty())
        return;
    json matrix = json::array();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            matrix.push_back((*m)[c][r]);
    const std::string path = n.path();
    ops::execute("imagePlane", {{"args", {path}}, {"camera", ""}}, CallSource::UI);
    ops::execute("xform", {{"args", {path}}, {"worldSpace", true}, {"matrix", matrix}}, CallSource::UI);
}

const glm::vec4 kCutLine(0.95f, 0.95f, 0.95f, 1.0f);
const glm::vec4 kCutMirror(0.6f, 0.6f, 0.6f, 0.85f);

/**
 * Cut Image Plane: click points across a joint of a reference picture (the first two either side of it), Enter cuts.
 * The far part becomes a piece pivoting at the joint, parented under the rest; X mirrors the cut across world X.
 */
class CutTool final : public ViewportTool
{
public:
    const char* name() const override { return "Cut Image Plane"; }

    void activate() override
    {
        log::info("Cut Image Plane: click either side of a joint, then around the limb, Enter or RMB cuts, X mirrors across world X, "
                  "Backspace removes a point, Esc cancels or exits. Rotate a piece's Z to repose it.");
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        mCursorView = view.camera;
        if (e->button() == Qt::RightButton)
        {
            if (mPoints.size() < 2)
                return false;
            finish(view);
            return true;
        }
        if (e->button() != Qt::LeftButton)
            return false;
        if (mPoints.empty())
        {
            const Placed* p = nullptr;
            std::vector<Placed> placed = placedIn(view);
            std::optional<glm::dvec3> hit = pieceHit(placed, e->position(), view, &p);
            if (!hit)
                return true;
            mOrigin = p->quad.center;
            mNormal = p->quad.normal();
            mAttached = ip::cameraName(p->node->as<std::string>(ImagePlane::kCamera)).empty() ? scene::kNoNode : p->node->id();
            mAspect = aspectOf(*p);
            mSubject = p->node->name();
            mPoints.push_back(*hit);
        }
        else if (std::optional<glm::dvec3> hit = onSheet(e->position(), view))
            mPoints.push_back(*hit);
        else
            return true;
        mHistory.clear();
        mDragging = true;
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mCursorView = view.camera;
        if (mPoints.empty())
        {
            std::vector<Placed> placed = placedIn(view);
            mHover = pieceHit(placed, e->position(), view, nullptr);
        }
        else
            mHover = onSheet(e->position(), view);
        if (mDragging && (e->buttons() & Qt::LeftButton) && mHover && !mPoints.empty())
            mPoints.back() = *mHover;
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (e->button() == Qt::LeftButton)
            mDragging = false;
        return e->button() == Qt::LeftButton;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo& view) override { return key(e->key(), e->modifiers(), view); }

    bool hotkey(const QKeyEvent& e, std::string_view action) override
    {
        switch (StepHistory<glm::dvec3>::route(e.key(), action))
        {
        case StepKey::Undo:
            if (mHistory.undo(mPoints) && mPoints.empty())
                mSubject.clear();
            viewportTools::requestRedraw();
            return true;
        case StepKey::Redo:
            if (!mPoints.empty())
                mHistory.redo(mPoints);
            viewportTools::requestRedraw();
            return true;
        case StepKey::None: break;
        }
        // Bound letters only arrive here: X belongs to the tool while it is active.
        if (e.key() == Qt::Key_X && !(e.modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier)))
        {
            mirror() = !mirror();
            viewportTools::requestRedraw();
            return true;
        }
        return false;
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        mView = view;
        std::vector<glm::dvec3> pts = mPoints;
        if (mHover && view.camera == mCursorView && !mDragging && !mPoints.empty())
            pts.push_back(*mHover);
        const std::vector<Placed> placed = pts.size() >= 2 ? placedIn(view) : std::vector<Placed>();
        auto drawCut = [&](const std::vector<glm::dvec3>& world, const glm::vec4& color, bool dashed) {
            std::vector<QPointF> px;
            for (const glm::dvec3& w : world)
                if (auto s = view.project(w))
                    px.push_back(*s);
            if (px.size() != world.size())
                return;
            if (dashed)
                p.dashedPolyline(px, 1.5f, 6.0f, color);
            else
                p.polyline(px, 2.0f, color);
            // Three points or more are a lasso around the limb, closed back to the first.
            if (px.size() >= 3)
                p.dashedLine(px.back(), px.front(), 1.5f, 6.0f, color);
            // Where the cut runs on past its ends, so a slanting line shows what else it takes.
            if (const Placed* piece = world.size() >= 2 ? pieceUnder(placed, (world[0] + world[1]) * 0.5) : nullptr)
                for (const auto& [from, to] : runOns(*piece, world, view))
                    if (auto a = view.project(from), b = view.project(to); a && b)
                        p.dashedLine(*a, *b, 1.5f, 6.0f, color);
            for (const QPointF& s : px)
            {
                p.disc(s, 5.0f, kHandleRim, 16);
                p.disc(s, 3.0f, color, 16);
            }
            // The joint: where the piece will pivot.
            if (px.size() >= 2)
            {
                const QPointF j = (px[0] + px[1]) * 0.5;
                p.disc(j, 7.0f, kHandleRim, 20);
                p.disc(j, 5.0f, color, 20);
                p.disc(j, 3.0f, kHandleRim, 20);
            }
        };
        drawCut(pts, kCutLine, false);
        if (mirror() && pts.size() >= 2)
        {
            std::vector<glm::dvec3> m = pts;
            for (glm::dvec3& v : m)
                v.x = -v.x;
            drawCut(m, kCutMirror, true);
        }
        if (mPoints.empty() && mHover && view.camera == mCursorView)
            if (auto s = view.project(*mHover))
            {
                p.disc(*s, 5.0f, kHandleRim, 16);
                p.disc(*s, 3.0f, kHotColor, 16);
            }

        drawToolStatus(p, view,
                       {.tool = "Cut Image Plane",
                        .subject = QString::fromStdString(mSubject),
                        .prompt = mPoints.empty()       ? "click one side of a joint"
                                  : mPoints.size() == 1 ? "click its other side"
                                  : mPoints.size() == 2 ? "click around the limb, or Enter"
                                                        : "Enter to cut",
                        .keys = {{"Enter", "cut"}, {"RMB", "cut"}, {"Backspace", "remove point"}, {"X", "mirror"}, {"Esc", "cancel"}},
                        .symmetry = ToolSymmetry{mirror(), "World X"}});
    }

    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

private:
    /** Kept for the session, so each cut of a character keeps the last choice. */
    static bool& mirror()
    {
        static bool on = true;
        return on;
    }

    /** Where the cursor meets the nearest image plane piece under it; planes riding a perspective camera can't be cut. */
    std::optional<glm::dvec3> pieceHit(const std::vector<Placed>& placed, QPointF px, const ViewInfo& view, const Placed** which) const
    {
        glm::dvec3 o, d;
        view.ray(px, o, d);
        std::optional<glm::dvec3> best;
        double bestT = 1e300;
        for (const Placed& p : placed)
        {
            const std::string cam = ip::cameraName(p.node->as<std::string>(ImagePlane::kCamera));
            if (!cam.empty())
                if (std::optional<ip::CameraFrame> f = cameraFor(cam, view); !f || !f->ortho)
                    continue;
            const glm::dvec3 n = p.quad.normal();
            const double den = glm::dot(d, n);
            if (std::abs(den) < 1e-12)
                continue;
            const double t = glm::dot(p.quad.center - o, n) / den;
            const glm::dvec3 hit = o + d * t;
            if (t > 0.0 && insidePiece(p, stOf(p.quad, hit)) && t < bestT)
            {
                bestT = t;
                best = hit;
                if (which)
                    *which = &p;
            }
        }
        return best;
    }

    /** Where the cursor meets the plane the cut started on. */
    std::optional<glm::dvec3> onSheet(QPointF px, const ViewInfo& view) const
    {
        glm::dvec3 o, d;
        view.ray(px, o, d);
        const double den = glm::dot(d, mNormal);
        if (std::abs(den) < 1e-12)
            return std::nullopt;
        return o + d * (glm::dot(mOrigin - o, mNormal) / den);
    }

    bool key(int key, Qt::KeyboardModifiers, const ViewInfo& view)
    {
        switch (key)
        {
        case Qt::Key_Return:
        case Qt::Key_Enter: finish(view); break;
        case Qt::Key_Escape:
            if (mPoints.empty())
            {
                // Leaving deletes the tool, so it has to wait until this event is done with it.
                QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
                return true;
            }
            cancel();
            break;
        default: return false;
        }
        viewportTools::requestRedraw();
        return true;
    }

    void finish(const ViewInfo& view)
    {
        if (mPoints.size() < 2)
            return;
        json points = json::array();
        for (const glm::dvec3& v : mPoints)
            points.push_back({v.x, v.y, v.z});
        try
        {
            UndoChunk chunk("Cut Image Plane");
            if (const scene::Node* n = scene::Graph::current().node(mAttached))
                freePlane(*n, view);
            ops::execute("imagePlaneCut", {{"points", points}, {"symmetry", mirror() ? "x" : "off"}, {"aspect", mAspect}}, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
        cancel();
    }

    void cancel()
    {
        mPoints.clear();
        mHistory.clear();
        mDragging = false;
        mAttached = scene::kNoNode;
        mSubject.clear();
        viewportTools::requestRedraw();
    }

    std::vector<glm::dvec3> mPoints;
    StepHistory<glm::dvec3> mHistory;
    std::optional<glm::dvec3> mHover;
    glm::dvec3 mOrigin{0.0};
    glm::dvec3 mNormal{0.0, 0.0, 1.0};
    /** The plane under the first point when it follows an orthographic camera: freed when the cut is made. */
    scene::NodeId mAttached = scene::kNoNode;
    /** The picture's width over height, from the piece the cut started on. */
    double mAspect = 0.0;
    std::string mSubject;
    std::string mCursorView;
    ViewInfo mView;
    bool mDragging = false;
};

/**
 * Image Plane Guides: drag on a picture to lay a guide line from where the drag starts through where it ends, snapping to
 * 15 degree steps (Shift for any angle), mirrored across world X. Ctrl+click a guide to delete it. The guides belong to
 * the plane the picture was cut from, so its pieces turn under them.
 */
class GuideTool final : public ViewportTool
{
public:
    const char* name() const override { return "Image Plane Guides"; }

    void activate() override
    {
        log::info("Image Plane Guides: drag on a picture to lay a guide (15 degree steps, Shift for any angle), X mirrors across world X, "
                  "Ctrl+click a guide to delete it, Esc exits.");
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        mCursorView = view.camera;
        if (e->button() != Qt::LeftButton)
            return false;
        if (e->modifiers() & Qt::ControlModifier)
        {
            deleteNear(e->position(), view);
            return true;
        }
        std::vector<Placed> placed = placedIn(view);
        const scene::Graph& g = scene::Graph::current();
        const Placed* best = nullptr;
        double bestT = 1e300;
        glm::dvec3 o, d;
        view.ray(e->position(), o, d);
        for (const Placed& p : placed)
        {
            const glm::dvec3 n = p.quad.normal();
            const double den = glm::dot(d, n);
            if (std::abs(den) < 1e-12 || !cutMatrix(*p.node, view))
                continue;
            const double t = glm::dot(p.quad.center - o, n) / den;
            if (t > 0.0 && t < bestT && insidePiece(p, stOf(p.quad, o + d * t)))
                bestT = t, best = &p;
        }
        if (!best)
            return true;
        // Guides go on the plane the piece was cut from.
        scene::NodeId root = best->node->id();
        while (!isRootPlane(g, *g.node(root)))
            root = g.parent(root);
        const scene::Node* rootNode = g.node(root);
        std::optional<glm::dmat4> m = cutMatrix(*rootNode, view);
        if (!m)
            return true;
        mPlane = root;
        mMatrix = *m;
        mStart = o + d * bestT;
        mEnd = mStart;
        mDragging = true;
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mCursorView = view.camera;
        mCursor = e->position();
        if (!mDragging)
        {
            viewportTools::requestRedraw();
            return false;
        }
        glm::dvec3 o, d;
        view.ray(e->position(), o, d);
        const glm::dvec3 n = glm::normalize(glm::dvec3(mMatrix[2]));
        const double den = glm::dot(d, n);
        if (std::abs(den) > 1e-12)
            mEnd = snapped(o + d * (glm::dot(mStart - o, n) / den), e->modifiers() & Qt::ShiftModifier);
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton || !mDragging)
            return false;
        mDragging = false;
        auto a = view.project(mStart), b = view.project(mEnd);
        if (a && b && std::hypot(a->x() - b->x(), a->y() - b->y()) > 6.0)
            add();
        viewportTools::requestRedraw();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (e->key() != Qt::Key_Escape)
            return false;
        QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
        return true;
    }

    bool hotkey(const QKeyEvent& e, std::string_view) override
    {
        if (e.key() == Qt::Key_X && !(e.modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier)))
        {
            mirror() = !mirror();
            viewportTools::requestRedraw();
            return true;
        }
        if (e.key() == Qt::Key_Escape)
        {
            QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
            return true;
        }
        return false;
    }

    void draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view) override
    {
        const scene::Graph& g = scene::Graph::current();
        // Each guide's angle at its start, so they read without the tool's own measuring.
        for (const Placed& pl : placedIn(view))
            if (isRootPlane(g, *pl.node))
                for (const GuideLine& l : guidesOf(pl, view))
                    if (auto at = view.project(l.end))
                        p.label(*at + QPointF(10, -10), QString("%1\u00b0").arg(l.angle, 0, 'f', 0));
        if (mDragging)
        {
            const glm::dvec2 a = local(mStart), b = local(mEnd);
            s.linesOnTop.push_back({glm::dvec3(mStart), kGuideColor});
            s.linesOnTop.push_back({glm::dvec3(mEnd), kGuideColor});
            if (mirror())
            {
                s.linesOnTop.push_back({glm::dvec3(mirrored(mStart)), kGuideColor * glm::vec4(1, 1, 1, 0.6f)});
                s.linesOnTop.push_back({glm::dvec3(mirrored(mEnd)), kGuideColor * glm::vec4(1, 1, 1, 0.6f)});
            }
            if (auto at = view.project(mEnd); at && glm::length(b - a) > 0.0)
                p.label(*at + QPointF(14, -14), QString("%1\u00b0").arg(angleFromHorizontal(b - a), 0, 'f', 0));
        }
        drawToolStatus(p, view,
                       {.tool = "Image Plane Guides",
                        .prompt = mDragging ? "release to lay it" : "drag from a joint",
                        .keys = {{"Shift", "any angle"}, {"Ctrl+click", "delete"}, {"X", "mirror"}, {"Esc", "exit"}},
                        .symmetry = ToolSymmetry{mirror(), "World X"}});
    }

    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

private:
    static bool& mirror()
    {
        static bool on = true;
        return on;
    }

    glm::dvec2 local(const glm::dvec3& world) const { return glm::dvec2(glm::inverse(mMatrix) * glm::dvec4(world, 1.0)); }
    glm::dvec3 fromLocal(glm::dvec2 v) const { return glm::dvec3(mMatrix * glm::dvec4(v, 0.0, 1.0)); }
    static glm::dvec3 mirrored(glm::dvec3 v) { return {-v.x, v.y, v.z}; }

    /** The end, turned to the nearest 15 degrees about the start in the plane unless free. */
    glm::dvec3 snapped(const glm::dvec3& end, bool free) const
    {
        const glm::dvec2 a = local(mStart), d = local(end) - a;
        const double len = glm::length(d);
        if (free || len <= 0.0)
            return end;
        const double step = glm::radians(15.0);
        const double angle = std::round(std::atan2(d.y, d.x) / step) * step;
        return fromLocal(a + glm::dvec2(std::cos(angle), std::sin(angle)) * len);
    }

    void add()
    {
        const scene::Node* plane = scene::Graph::current().node(mPlane);
        if (!plane)
            return;
        const glm::dvec2 a = local(mStart), b = local(mEnd);
        json lines = json::array({json::array({a.x, a.y, b.x, b.y})});
        // A guide on the mirror plane is its own mirror image.
        if (mirror() && !(std::abs(mStart.x) < 1e-3 && std::abs(mEnd.x) < 1e-3))
        {
            const glm::dvec2 ma = local(mirrored(mStart)), mb = local(mirrored(mEnd));
            lines.push_back({ma.x, ma.y, mb.x, mb.y});
        }
        try
        {
            ops::execute("imagePlaneGuide", {{"args", {plane->path()}}, {"lines", lines}}, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
    }

    void deleteNear(QPointF px, const ViewInfo& view)
    {
        const scene::Graph& g = scene::Graph::current();
        double best = 8.0;
        std::string plane;
        int index = -1;
        for (const Placed& p : placedIn(view))
        {
            if (!isRootPlane(g, *p.node))
                continue;
            const std::vector<GuideLine> lines = guidesOf(p, view);
            for (size_t i = 0; i < lines.size(); ++i)
                if (auto a = view.project(lines[i].start), b = view.project(lines[i].edge); a && b)
                    if (double d = segDistance(px, *a, *b); d < best)
                        best = d, plane = p.node->path(), index = int(i);
        }
        if (index < 0)
            return;
        try
        {
            ops::execute("imagePlaneGuide", {{"args", {plane}}, {"remove", index}}, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
    }

    scene::NodeId mPlane = scene::kNoNode;
    glm::dmat4 mMatrix{1.0};
    glm::dvec3 mStart{0.0};
    glm::dvec3 mEnd{0.0};
    QPointF mCursor;
    std::string mCursorView;
    bool mDragging = false;
};
} // namespace

bool shownIn(const std::string& view)
{
    json hidden = PrefStore::get().value("general", kHiddenKey, json::array());
    return !hidden.is_array() || std::ranges::find(hidden, json(view)) == hidden.end();
}

void setShownIn(const std::string& view, bool on)
{
    json hidden = PrefStore::get().value("general", kHiddenKey, json::array());
    if (!hidden.is_array())
        hidden = json::array();
    json out = json::array();
    for (const json& v : hidden)
        if (v != json(view))
            out.push_back(v);
    if (!on)
        out.push_back(view);
    PrefStore::get().set("general", kHiddenKey, out);
}

std::string resolve(const std::string& stored)
{
    return paths::toUtf8(paths::resolvePath(stored));
}

void importImage(QWidget* parent, const std::string& camera)
{
    QString start = Workspace::get().isOpen() ? QString::fromStdWString(Workspace::get().root().wstring()) : QString();
    const ui::fileDialog::ContextScope scope("imagePlane", "Images");
    const QStringList files = ui::fileDialog::getOpenFileNames(parent, camera.empty() ? "Create Image Plane" : "Import Image", start, kImageFilter);
    std::vector<std::string> paths;
    for (const QString& f : files)
        paths.push_back(f.toStdString());
    create(paths, camera);
}

void fillCameraMenu(QMenu* menu, const std::string& camera)
{
    QObject::connect(menu->addAction("Import Image..."), &QAction::triggered, menu, [menu, camera] { importImage(menu->parentWidget(), camera); });
    scene::Graph& g = scene::Graph::current();
    bool first = true;
    for (scene::NodeId id : g.nodes())
    {
        const scene::Node* n = g.node(id);
        if (!n->type().isA("imagePlane") || ip::cameraName(n->as<std::string>(ImagePlane::kCamera)) != camera)
            continue;
        if (std::exchange(first, false))
            menu->addSeparator();
        std::string name = n->path();
        QObject::connect(menu->addAction(QString::fromStdString("Select " + name)), &QAction::triggered, menu, [name] {
            try
            {
                ops::execute("select", {{"args", {name}}}, CallSource::UI);
            }
            catch (const CommandError& e)
            {
                log::error("{}", e.what());
            }
        });
    }
}

void acceptDrops(ViewportWidget* view)
{
    view->setAcceptDrops(true);
    view->installEventFilter(new DropFilter(view));
}

void install(std::function<ViewportWidget*()> active)
{
    activeView() = std::move(active);
    viewportHooks::addDrawer(draw);
    viewportHooks::addPicker(pick);
    videoTextures::addListener([] { viewportTools::requestRedraw(); });
    onTimeChanged(qApp, [] {
        for (const scene::Node* n : scene::sceneIndex::ofType(scene::Graph::current(), "imagePlane"))
            if (n->as<bool>(ImagePlane::kUseFrameExtension) || media::isVideoFile(n->as<std::string>(ImagePlane::kImageName)))
            {
                viewportTools::requestRedraw();
                return;
            }
    });

    actions::add({.id = "create.imagePlane",
                  .label = "Image Plane...",
                  .category = "Create",
                  .run = [] { importImage(QApplication::activeWindow(), {}); },
                  .doc = "Creates a free image plane from an image file, placed by its transform like any object.",
                  .python = "lila.ops.imagePlane(fileName=\"refs/side.png\")"});
    actions::add({.id = "viewport.imagePlaneImport",
                  .label = "Import Image...",
                  .category = "View",
                  .run =
                      [] {
                          ViewportWidget* v = activeView() ? activeView()() : nullptr;
                          importImage(v ? v->window() : QApplication::activeWindow(), v ? v->viewName() : "persp");
                      },
                  .doc = "Attaches a new image plane to the active view's camera.",
                  .python = "lila.ops.imagePlane(fileName=\"refs/front.png\", camera=\"front\")"});
    actions::add({.id = "imagePlane.cut",
                  .label = "Cut Image Plane",
                  .category = "Create",
                  .run = [] { viewportTools::set(std::make_unique<CutTool>()); },
                  .doc = "Cuts a reference picture into pieces that pivot at the joints, to repose arms or legs drawn at a poor angle: "
                         "click either side of a joint, then around the limb, Enter cuts, X mirrors the cut across world X. Rotate a piece's Z to swing it.",
                  .python = "lila.ops.imagePlaneCut(points=[(20, 150, 0), (18, 130, 0)], symmetry=\"x\")"});
    actions::add({.id = "imagePlane.guides",
                  .label = "Image Plane Guides",
                  .category = "Create",
                  .run = [] { viewportTools::set(std::make_unique<GuideTool>()); },
                  .doc = "Lays angle guides over a reference picture, to line limbs up while reposing it: drag from a joint (15 degree steps, "
                         "Shift for any angle), X mirrors across world X, Ctrl+click a guide to delete it.",
                  .python = "lila.ops.imagePlaneGuide(\"front\", lines=[(15, 47, 50, 12)])"});
    actions::add({.id = "viewport.toggleImagePlanes",
                  .label = "Image Planes",
                  .category = "View",
                  .run =
                      [] {
                          if (ViewportWidget* v = activeView() ? activeView()() : nullptr)
                              setShownIn(v->viewName(), !shownIn(v->viewName()));
                      },
                  .doc = "Shows or hides image planes in the active view."});
}
} // namespace lila::ui::imagePlanes
