#include "ui/ViewportWidgetDetail.h"
#include "ui/ViewportToast.h"
#include "ui/Hotkeys.h"
#include "ui/ViewportHud.h"
#include "ui/ViewportToolbar.h"
#include "scene/HudStats.h"
#include "scene/SoftSelect.h"
#include "scene/Turntable.h"
#include "uv/Uv.h"

#include <QMenu>
#include <QVariantAnimation>

#include <map>
#include <set>

namespace lila::ui
{
using namespace viewportDetail;

void ViewportWidget::setView(const std::string& name)
{
    if (name == mViewName)
        return;
    if (!builtInView(name))
    {
        if (scene::Node* n = findNode(name); n && n->type().isA("camera"))
            lookThrough(n->id());
        return;
    }
    commitCameraEdit();
    if (mViewName == "persp" && mLookThrough == scene::kNoNode)
        mPerspCamera = mCamera;
    if (mLookThrough != scene::kNoNode)
    {
        mLookThrough = scene::kNoNode;
        mCamera.leaveLookThrough();
    }
    if (name == "persp")
    {
        bool zUp = mCamera.zUp;
        mCamera = mPerspCamera;
        mCamera.zUp = zUp;
        mCamera.ortho = false;
    }
    else
    {
        glm::dvec3 target = mCamera.target;
        double width = mCamera.ortho ? mCamera.orthoWidth : 2.0 * mCamera.distance * (mCamera.horizontalAperture * 0.5) / mCamera.focalLength;
        mCamera.setView(name);
        mCamera.target = target;
        mCamera.orthoWidth = std::max(width, 1.0);
    }
    mViewName = name;
    syncCameraSettings();
    requestFrame();
    Q_EMIT viewStateChanged();
}

nlohmann::json ViewportWidget::cameraState() const
{
    const bool through = mLookThrough != scene::kNoNode;
    const std::string& view = through ? mReturnView : mViewName;
    const bool persp = view == "persp";
    nlohmann::json j{{"view", view}, {"persp", (persp && !through ? mCamera : mPerspCamera).poseJson()}};
    if (!persp && !through)
        j["camera"] = mCamera.poseJson();
    return j;
}

void ViewportWidget::restoreCameraState(const nlohmann::json& state)
{
    if (!state.is_object())
        return;
    std::string view = state.value("view", std::string("persp"));
    if (!builtInView(view))
        return;
    if (mLookThrough != scene::kNoNode)
        stopLookingThrough();
    commitCameraEdit();
    mPerspCamera.setPose(state.value("persp", nlohmann::json()));
    if (view == "persp")
    {
        bool zUp = mCamera.zUp;
        mCamera = mPerspCamera;
        mCamera.zUp = zUp;
        mCamera.ortho = false;
    }
    else
    {
        mCamera.setView(view);
        mCamera.setPose(state.value("camera", nlohmann::json()));
    }
    mViewName = view;
    syncCameraSettings();
    requestFrame();
    Q_EMIT viewStateChanged();
}

void ViewportWidget::lookThrough(scene::NodeId camera)
{
    scene::Node* n = scene::Graph::current().node(camera);
    if (!n || !n->type().isA("camera") || camera == mLookThrough)
        return;
    commitCameraEdit();
    if (mLookThrough == scene::kNoNode)
    {
        mReturnView = mViewName;
        if (mViewName == "persp")
            mPerspCamera = mCamera;
    }
    mLookThrough = camera;
    mViewName = n->path();
    syncLookThrough();
    syncCameraSettings();
    requestFrame();
    Q_EMIT viewStateChanged();
}

void ViewportWidget::syncLookThrough()
{
    if (mLookThrough == scene::kNoNode)
        return;
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.node(mLookThrough);
    if (!n || !n->type().isA("camera"))
    {
        mCameraEdit.reset();
        setView(mReturnView);
        return;
    }
    g.evaluate();
    mCamera.lookThrough(*n, n->as<glm::dmat4>(scene::Transform::kWorldMatrix));
    bool renamed = n->path() != mViewName;
    mViewName = n->path();
    // The gate settings are the node's attributes, which can change under any edit.
    syncCameraSettings();
    if (renamed)
        Q_EMIT viewStateChanged();
}

bool ViewportWidget::beginCameraEdit()
{
    scene::Node* n = scene::Graph::current().node(mLookThrough);
    if (!n)
        return false;
    if (n->as<bool>(scene::Camera::kLocked))
    {
        // One message per gesture, not one per wheel click.
        if (double now = nowMs(); now - mLockedWarnMs > 1500.0)
        {
            mLockedWarnMs = now;
            log::warn("Camera '{}' is locked; unlock it (locked attribute) to move it through the view.", n->path());
        }
        return false;
    }
    if (mCameraEdit && mCameraEdit->id == mLookThrough)
        return true;
    commitCameraEdit();
    CameraEdit edit{mLookThrough, {}};
    for (int attr : kEditedAttrs)
        edit.saved.emplace_back(attr, n->value(attr));
    mCameraEdit = std::move(edit);
    return true;
}

void ViewportWidget::previewCameraEdit()
{
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = mCameraEdit ? g.node(mCameraEdit->id) : nullptr;
    if (!n)
        return;
    UndoStack& u = UndoStack::get();
    bool was = u.enabled();
    u.setEnabled(false);
    try
    {
        g.evaluate();
        glm::dmat4 placed = scene::camera::placedWorld(n->as<glm::dmat4>(scene::Transform::kWorldMatrix), mCamera.eye(), mCamera.target,
                                                       mCamera.viewUp());
        scene::Transform::setLocalMatrix(g, n->id(), glm::inverse(n->as<glm::dmat4>(scene::Transform::kParentMatrix)) * placed);
        if (mCamera.ortho)
            g.setValue({n->id(), scene::Camera::kOrthographicWidth, -1}, mCamera.orthoWidth);
        // A constraint or connection driving the camera puts it back, and the view with it.
        g.evaluate();
        const glm::dvec3 now(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
        if (glm::distance(now, glm::dvec3(placed[3])) > 1e-3 * std::max(1.0, mCamera.distance))
            if (double ms = nowMs(); ms - mLockedWarnMs > 1500.0)
            {
                mLockedWarnMs = ms;
                log::warn("Camera '{}' is driven by a constraint or connection, so the view can't move it; look through a free camera or "
                          "the perspective view instead.",
                          n->path());
            }
    }
    catch (const std::exception& e)
    {
        log::error("{}", e.what());
    }
    u.setEnabled(was);
}

void ViewportWidget::commitCameraEdit()
{
    mWheelCommit->stop();
    if (!mCameraEdit)
        return;
    CameraEdit edit = std::move(*mCameraEdit);
    mCameraEdit.reset();
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.node(edit.id);
    if (!n)
        return;
    std::vector<int> changed;
    for (auto& [attr, value] : edit.saved)
        if (n->value(attr) != value)
            changed.push_back(attr);
    if (changed.empty())
        return;
    io::markModified();
    if (!scene::anim::autoKey())
        return;
    // As Maya's Auto Key: only channels that already have keys get one.
    try
    {
        UndoChunk chunk("Auto Key Camera");
        for (const scene::anim::AnimatedPlug& a : scene::anim::animatedPlugs(g, edit.id))
        {
            if (std::ranges::find(changed, a.plug.attr) == changed.end())
                continue;
            const scene::PlugValue& v = n->value(a.plug.attr);
            double value = a.plug.component >= 0 && std::holds_alternative<glm::dvec3>(v) ? std::get<glm::dvec3>(v)[a.plug.component]
                           : std::holds_alternative<double>(v)                         ? std::get<double>(v)
                                                                                        : 0.0;
            ops::execute("setKeyframe", {{"args", {g.plugName(a.plug)}}, {"value", value}}, CallSource::UI);
        }
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

scene::NodeId ViewportWidget::droppedCamera(const QMimeData* mime) const
{
    if (!mime || !mime->hasFormat(kNodeNamesMime))
        return scene::kNoNode;
    for (const QString& name : QString::fromUtf8(mime->data(kNodeNamesMime)).split(QChar('\n'), Qt::SkipEmptyParts))
        if (scene::Node* n = findNode(name.toStdString()); n && n->type().isA("camera"))
            return n->id();
    return scene::kNoNode;
}

viewportHooks::DropTarget ViewportWidget::dropTarget(QPointF px)
{
    const selectionHandlers::Hit hit = selectionHit(px);
    viewportHooks::DropTarget t;
    t.node = hit.node;
    t.face = hit.face;
    t.position = hit.position;
    t.view = viewInfo();
    t.px = px;
    return t;
}

void ViewportWidget::dragLeaveEvent(QDragLeaveEvent* e)
{
    // Only handlers that drew a preview care; there is no mime data to offer by now.
    viewportHooks::DropTarget t;
    t.view = viewInfo();
    t.left = true;
    static const QMimeData kEmpty;
    viewportHooks::handleDrop(kEmpty, t, false);
    e->accept();
}

void ViewportWidget::dragEnterEvent(QDragEnterEvent* e)
{
    if (droppedCamera(e->mimeData()) == scene::kNoNode)
    {
        // Hooks get the move events too: entering only asks whether any of them wants this kind of drag.
        viewportHooks::DropTarget none;
        none.view = viewInfo();
        none.px = e->position();
        if (!e->mimeData() || !viewportHooks::handleDrop(*e->mimeData(), none, false))
            return e->ignore();
        e->setDropAction(Qt::CopyAction);
        return e->accept();
    }
    e->setDropAction(Qt::LinkAction);
    e->accept();
}

void ViewportWidget::dragMoveEvent(QDragMoveEvent* e)
{
    if (droppedCamera(e->mimeData()) == scene::kNoNode)
    {
        if (!e->mimeData() || !viewportHooks::handleDrop(*e->mimeData(), dropTarget(e->position()), false))
            return e->ignore();
        e->setDropAction(Qt::CopyAction);
        return e->accept();
    }
    e->setDropAction(Qt::LinkAction);
    e->accept();
}

void ViewportWidget::dropEvent(QDropEvent* e)
{
    scene::NodeId cam = droppedCamera(e->mimeData());
    if (cam == scene::kNoNode)
    {
        if (!e->mimeData() || !viewportHooks::handleDrop(*e->mimeData(), dropTarget(e->position()), true))
            return e->ignore();
        e->setDropAction(Qt::CopyAction);
        e->accept();
        setFocus();
        return;
    }
    e->setDropAction(Qt::LinkAction);
    e->accept();
    // Focus makes this the active view, as a click would.
    setFocus();
    lookThrough(cam);
}

void ViewportWidget::syncCameraSettings()
{
    const scene::RenderSettings& r = scene::renderSettings();
    scene::CameraSettings c = r.camera(mViewName);
    mCamera.filmFit = c.filmFit;
    mCamera.overscan = c.overscan;
    mCamera.gateAspect = c.resolutionGate ? std::optional<double>(r.deviceAspect()) : std::nullopt;
    if (mLookThrough == scene::kNoNode)
    {
        mCamera.autoNear = c.nearClip <= 0.0;
        mCamera.nearClip = c.nearClip > 0.0 ? c.nearClip : Camera{}.nearClip;
    }
    else
        mCamera.autoNear = false;
}

void ViewportWidget::drawSoftSelectRadius(OverlayPainter& p)
{
    if (!mSoftDrag)
        return;
    const ViewInfo view = viewInfo();
    const glm::dvec3 centre = scene::symmetricSelectionCenter();
    const std::optional<QPointF> at = view.project(centre);
    if (!at)
        return;
    const double radius = PrefStore::get().value<double>("general", scene::softSelect::kRadius, 25.0);
    const double px = radius / view.unitsPerPixel(centre);
    const scene::softSelect::Curve curve = scene::softSelect::currentCurve();
    const bool gray = viewportDisplay::grayscaleWeights();
    constexpr int kRings = 32, kSegments = 64;
    auto ring = [&](double r) {
        std::vector<QPointF> pts(kSegments);
        for (int k = 0; k < kSegments; ++k)
        {
            const double a = k * 6.283185307179586 / kSegments;
            pts[k] = *at + QPointF(std::cos(a), std::sin(a)) * r;
        }
        return pts;
    };
    // Bands shaded and faded by the falloff curve at their middle, so the disc reads as the weights the vertices get.
    std::vector<QPointF> inner = ring(0.0);
    for (int i = 0; i < kRings; ++i)
    {
        std::vector<QPointF> outer = ring(px * (i + 1) / kRings);
        const float w = float(curve.evaluate((i + 0.5) / kRings));
        glm::vec4 c = viewportDisplay::falloffColor(w, gray);
        c.a = 0.45f * w;
        if (c.a > 0.0f)
            for (int k = 0; k < kSegments; ++k)
            {
                const int n = (k + 1) % kSegments;
                p.triangle(inner[k], outer[k], outer[n], c);
                if (i > 0)
                    p.triangle(inner[k], outer[n], inner[n], c);
            }
        inner = std::move(outer);
    }
    p.polyline(inner, 1.5f, p.ink(0.9f, 0.9f), true);
    p.label(*at + QPointF(px + 8.0, 4.0), QString("Radius %1").arg(radius, 0, 'g', 4));
}

void ViewportWidget::drawGate(OverlayPainter& p)
{
    const scene::RenderSettings& r = scene::renderSettings();
    scene::CameraSettings c = r.camera(mViewName);
    if (!c.resolutionGate && !c.filmGate)
        return;
    glm::dvec2 view = mCamera.viewExtents(aspect());
    auto toRect = [&](glm::dvec2 half) {
        double w = half.x / view.x * width(), h = half.y / view.y * height();
        return QRectF((width() - w) * 0.5, (height() - h) * 0.5, w, h);
    };
    const glm::vec4 border = p.ink(0.75f, 0.9f);
    auto outline = [&](QRectF g, bool dashed) {
        QPointF pts[4] = {g.topLeft(), g.topRight(), g.bottomRight(), g.bottomLeft()};
        for (int i = 0; i < 4; ++i)
            dashed ? p.dashedLine(pts[i], pts[(i + 1) % 4], 1.0f, 6.0f, border) : p.line(pts[i], pts[(i + 1) % 4], 1.0f, border);
    };
    if (auto gate = mCamera.gateExtents(); gate && c.resolutionGate)
    {
        QRectF g = toRect(*gate);
        if (c.gateMask && c.gateMaskOpacity > 0.0)
        {
            const glm::vec4 mask(0.1f, 0.1f, 0.1f, float(c.gateMaskOpacity));
            QRectF all(0, 0, width(), height());
            QRectF in = g.intersected(all);
            p.rect(QRectF(0, 0, all.width(), in.top()), mask);
            p.rect(QRectF(0, in.bottom(), all.width(), all.bottom() - in.bottom()), mask);
            p.rect(QRectF(0, in.top(), in.left(), in.height()), mask);
            p.rect(QRectF(in.right(), in.top(), all.right() - in.right(), in.height()), mask);
        }
        outline(g, false);
        const PrefStore& prefs = PrefStore::get();
        auto inset = [&](double fraction) {
            const double dx = g.width() * (1.0 - fraction) * 0.5, dy = g.height() * (1.0 - fraction) * 0.5;
            return g.adjusted(dx, dy, -dx, -dy);
        };
        if (prefs.value<bool>("general", viewportDisplay::kSafeFrames, false))
        {
            // Broadcast action safe (93%) and title safe (90%).
            const glm::vec4 guide = p.ink(0.55f, 0.6f);
            for (double fraction : {0.93, 0.9})
            {
                QRectF s = inset(fraction);
                QPointF pts[4] = {s.topLeft(), s.topRight(), s.bottomRight(), s.bottomLeft()};
                for (int i = 0; i < 4; ++i)
                    p.dashedLine(pts[i], pts[(i + 1) % 4], 1.0f, 4.0f, guide);
            }
        }
        if (prefs.value<bool>("general", viewportDisplay::kThirds, false))
        {
            const glm::vec4 guide = p.ink(0.55f, 0.5f);
            for (int k = 1; k < 3; ++k)
            {
                const double x = g.left() + g.width() * k / 3.0, y = g.top() + g.height() * k / 3.0;
                p.line(QPointF(x, g.top()), QPointF(x, g.bottom()), 1.0f, guide);
                p.line(QPointF(g.left(), y), QPointF(g.right(), y), 1.0f, guide);
            }
        }
        p.setShadow(true);
        QString res = QString("%1 x %2").arg(r.width).arg(r.height);
        double right = std::max(std::min(g.right(), width() - 4.0), 4.0 + OverlayPainter::textWidth(res));
        p.text(QPointF(right, std::max(g.top() - 5.0, 14.0)), res, border, OverlayPainter::Right);
        p.setShadow(false);
    }
    if (c.filmGate)
        outline(toRect(mCamera.filmExtents()), true);
}

void ViewportWidget::toggleIsolate()
{
    if (mIsolation.active())
        mIsolation.clear();
    else
    {
        scene::Graph& g = scene::Graph::current();
        g.evaluate();
        mIsolation = scene::isolation::fromSelection(g);
        const QString key = Hotkeys::get().keyFor("viewport.isolate");
        viewportToast::show(key.isEmpty() ? QString("Isolate Select on") : QString("Isolate Select on: %1 to show all").arg(key), this, 3000);
    }
    setIsolation(std::move(mIsolation));
}

void ViewportWidget::addToIsolation()
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::isolation::addSelection(mIsolation, g);
    setIsolation(std::move(mIsolation));
}

void ViewportWidget::removeFromIsolation()
{
    if (!mIsolation.active())
        return;
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::isolation::removeSelection(mIsolation, g);
    setIsolation(std::move(mIsolation));
}

void ViewportWidget::setIsolation(scene::isolation::Set set)
{
    mIsolation = std::move(set);
    clearPreselect();
    requestFrame();
    Q_EMIT viewStateChanged();
}

render::Renderer::FrameStats ViewportWidget::frameStats() const
{
    return mId ? render::Renderer::get().stats(mId) : render::Renderer::FrameStats{};
}

uint64_t ViewportWidget::hudShownKey() const
{
    const uint64_t key = ((uint64_t(mHudPanel) + 1) * 0x9E3779B97F4A7C15ull) ^ (mIsolation.version * 0xC2B2AE3D27D4EB4Full) ^
                         (mLodHiddenKey * 0x165667B19E3779F9ull) ^ (mDisplayEpoch << 20);
    return key ? key : 1;
}

nlohmann::json ViewportWidget::inspect(const std::optional<QRectF>& pick)
{
    render::FrameState s = buildFrame(nullptr);
    scene::Graph& g = scene::Graph::current();
    auto counts = [](const scene::hud::PolyCount& c) {
        return nlohmann::json{{"verts", c.verts}, {"edges", c.edges}, {"faces", c.faces}, {"tris", c.tris}, {"uvs", c.uvs}};
    };
    const scene::hud::PolyStats stats = scene::hud::polyStats(
        g, uv::currentUvSet(),
        [this](scene::NodeId id) {
            const scene::Node* n = scene::Graph::current().node(id);
            return n && mIsolation.shows(id) && !mLodHidden.contains(id) && scene::selectionMask::shown(*n) && !viewportHooks::hiddenInView(id);
        },
        mIsolation.active() ? &mIsolation : nullptr, hudShownKey());
    nlohmann::json out{{"hud", {{"scene", counts(stats.scene)}, {"objects", counts(stats.objects)}, {"components", counts(stats.components)},
                                {"uniqueTris", stats.uniqueTris}, {"uniqueObjectTris", stats.uniqueObjectTris}}}};
    std::map<std::string, int64_t> flags;
    size_t placements = 0;
    std::set<uint64_t> nodes;
    for (const render::MeshDraw& d : s.meshes)
    {
        placements += d.instances && !d.instances->empty() ? d.instances->size() : 1;
        nodes.insert(d.node);
        for (auto [bit, name] : {std::pair(render::DrawShaded, "shaded"), {render::DrawWire, "wire"}, {render::DrawSelected, "selected"},
                                 {render::DrawUnderSelected, "underSelected"}, {render::DrawPreselect, "preselect"}, {render::DrawWireDepth, "wireDepth"}})
            if (d.flags & bit)
                ++flags[name];
        if (!d.materials.empty())
            ++flags["textured"];
        if (std::ranges::any_of(d.materials, [](const render::Material& m) { return (m.flags & render::MatTranslucent) && m.opacity < 1.0f; }))
            ++flags["translucent"];
    }
    out["size"] = {width(), height()};
    out["draws"] = {{"count", s.meshes.size()}, {"placements", placements}, {"nodes", nodes.size()}, {"flags", flags}};
    if (pick)
        out["picked"] = hitTest(pick->topLeft(), pick->bottomRight());
    return out;
}

void ViewportWidget::drawHud(OverlayPainter& p)
{
    double now = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    render::Renderer::FrameStats stats = render::Renderer::get().stats(mId);
    mFrameSamples.emplace_back(now, stats.frames);
    while (now - mFrameSamples.front().first > 1000.0)
        mFrameSamples.pop_front();
    hud::Context ctx;
    ctx.panel = mHudPanel;
    ctx.view = viewInfo();
    if (mIsolation.active())
        ctx.isolation = &mIsolation;
    // Counts cover what this view draws, so they skip the same nodes the mesh pass does.
    ctx.shownNode = [this](scene::NodeId id) {
        const scene::Node* n = scene::Graph::current().node(id);
        return n && mIsolation.shows(id) && !mLodHidden.contains(id) && scene::selectionMask::shown(*n) && !viewportHooks::hiddenInView(id);
    };
    ctx.shownKey = hudShownKey();
    // Frames the renderer drew, not states submitted: mouse moves submit far more often than the display can show.
    double span = mFrameSamples.back().first - mFrameSamples.front().first;
    ctx.framesPerSecond = span > 100.0 ? double(mFrameSamples.back().second - mFrameSamples.front().second) * 1000.0 / span : 0.0;
    ctx.gpuMs = stats.gpuMs;
    ctx.aoMs = stats.aoMs;
    std::vector<QRectF> drawn;
    ctx.drawn = &drawn;
    // A provisional uv count settles on the next frame once the mesh stops changing, which needs a frame to happen.
    if (hud::draw(p, ctx) && !mHudRefreshQueued)
    {
        mHudRefreshQueued = true;
        QTimer::singleShot(200, this, [this] {
            mHudRefreshQueued = false;
            requestFrame();
        });
    }
    flushToolMessages(p, ctx.view, drawn);
    viewportToast::draw(p, ctx.view, this, drawn);
}

void ViewportWidget::drawViewIndicator(OverlayPainter& p)
{
    // Axis triad in the bottom-left corner plus the view's name, Maya style. Letters tell the axes apart in monotone.
    glm::dmat3 v(mCamera.view());
    QPointF origin(36, height() - 36);
    const QRectF corner(0, height() - 72, 72, 72);
    bool axes = hud::shown(mHudPanel, "viewAxis");
    bool colored = viewportDisplay::accentColors(viewportDisplay::kViewAxisColors);
    p.setShadow(true);
    for (int axis = 0; axes && axis < 3; ++axis)
    {
        glm::dvec3 d = v * glm::dvec3(axis == 0, axis == 1, axis == 2);
        QPointF tip = origin + QPointF(d.x, -d.y) * 22.0;
        // Pointing away from the viewer (view -Z) reads dimmer.
        glm::vec4 c = colored ? glm::vec4(viewportDisplay::axisColor(axis) * (d.z < 0.0 ? 0.6f : 1.0f), 1.0f)
                              : p.ink(d.z < 0.0 ? 0.45f : 0.9f, 1.0f, corner);
        p.line(origin, tip, 1.5f, c);
        p.text(tip + QPointF(d.x, -d.y) * 8.0 + QPointF(0, 4), QString(QChar('X' + axis)), c, OverlayPainter::Center);
    }
    if (hud::shown(mHudPanel, "cameraNames"))
    {
        QString name = QString::fromStdString(mViewName);
        double w = OverlayPainter::textWidth(name);
        QRectF under((width() - w) / 2.0, height() - 24, w, 16);
        p.text(QPointF(width() / 2.0, height() - 12), name, p.ink(0.75f, 0.9f, under), OverlayPainter::Center);
    }
    p.setShadow(false);
}

namespace
{
using CubeStroke = std::vector<glm::dvec2>;

/** Stroke letters for the view cube's labels, in a box 0.6 wide and 1 tall, so they can lie flat on a face. */
const std::vector<CubeStroke>& cubeGlyph(char c)
{
    static const std::map<char, std::vector<CubeStroke>> glyphs = {
        {'A', {{{0, 0}, {0.3, 1}, {0.6, 0}}, {{0.1, 0.35}, {0.5, 0.35}}}},
        {'B', {{{0, 0}, {0, 1}, {0.45, 1}, {0.55, 0.9}, {0.55, 0.6}, {0.45, 0.5}, {0, 0.5}},
               {{0.45, 0.5}, {0.6, 0.4}, {0.6, 0.1}, {0.5, 0}, {0, 0}}}},
        {'C', {{{0.6, 0.85}, {0.45, 1}, {0.15, 1}, {0, 0.85}, {0, 0.15}, {0.15, 0}, {0.45, 0}, {0.6, 0.15}}}},
        {'E', {{{0.6, 1}, {0, 1}, {0, 0}, {0.6, 0}}, {{0, 0.5}, {0.45, 0.5}}}},
        {'F', {{{0.6, 1}, {0, 1}, {0, 0}}, {{0, 0.5}, {0.45, 0.5}}}},
        {'G', {{{0.6, 0.85}, {0.45, 1}, {0.15, 1}, {0, 0.85}, {0, 0.15}, {0.15, 0}, {0.45, 0}, {0.6, 0.15}, {0.6, 0.45}, {0.35, 0.45}}}},
        {'H', {{{0, 0}, {0, 1}}, {{0.6, 0}, {0.6, 1}}, {{0, 0.5}, {0.6, 0.5}}}},
        {'I', {{{0.3, 0}, {0.3, 1}}, {{0.1, 1}, {0.5, 1}}, {{0.1, 0}, {0.5, 0}}}},
        {'K', {{{0, 0}, {0, 1}}, {{0.6, 1}, {0, 0.4}}, {{0.2, 0.6}, {0.6, 0}}}},
        {'L', {{{0, 1}, {0, 0}, {0.6, 0}}}},
        {'M', {{{0, 0}, {0, 1}, {0.3, 0.5}, {0.6, 1}, {0.6, 0}}}},
        {'N', {{{0, 0}, {0, 1}, {0.6, 0}, {0.6, 1}}}},
        {'O', {{{0.15, 0}, {0, 0.15}, {0, 0.85}, {0.15, 1}, {0.45, 1}, {0.6, 0.85}, {0.6, 0.15}, {0.45, 0}, {0.15, 0}}}},
        {'P', {{{0, 0}, {0, 1}, {0.45, 1}, {0.6, 0.85}, {0.6, 0.65}, {0.45, 0.5}, {0, 0.5}}}},
        {'R', {{{0, 0}, {0, 1}, {0.45, 1}, {0.6, 0.85}, {0.6, 0.65}, {0.45, 0.5}, {0, 0.5}}, {{0.3, 0.5}, {0.6, 0}}}},
        {'T', {{{0, 1}, {0.6, 1}}, {{0.3, 1}, {0.3, 0}}}},
    };
    static const std::vector<CubeStroke> none;
    auto it = glyphs.find(c);
    return it == glyphs.end() ? none : it->second;
}
} // namespace

void ViewportWidget::drawViewCube(OverlayPainter& p)
{
    mViewCubeFaces.clear();
    if (!hud::shown(mHudPanel, "viewCube"))
        return;
    const QRectF box = hud::viewCubeRect(width());
    const QPointF centre = box.center();
    const double half = box.width() * 0.3;
    const glm::dmat3 v(mCamera.view());
    auto project = [&](const glm::dvec3& w) {
        const glm::dvec3 q = v * w;
        return centre + QPointF(q.x, -q.y) * half;
    };
    constexpr glm::vec4 kFace(0.27f, 0.27f, 0.29f, 0.9f), kHover(0.17f, 0.17f, 0.18f, 0.95f), kEdge(0.62f, 0.62f, 0.65f, 1.0f),
        kLabel(0.9f, 0.9f, 0.9f, 1.0f);
    constexpr double kLetter = 0.3, kAdvance = 0.85; // in face half-widths, and letter heights
    // A camera looking through a scene camera moves that node, which the cube doesn't do.
    const bool clickable = mLookThrough == scene::kNoNode;
    const QPointF cursor = mapFromGlobal(QCursor::pos().toPointF());
    ui::Camera sides;
    sides.zUp = mCamera.zUp;
    for (const char* side : {"front", "back", "left", "right", "top", "bottom"})
    {
        sides.setView(side);
        const glm::dvec3 n = -glm::normalize(sides.orthoForward), up = glm::normalize(sides.orthoUp);
        const glm::dvec3 right = glm::cross(-n, up);
        const double facing = (v * n).z;
        if (facing <= 0.02)
            continue;
        std::vector<QPointF> corners;
        for (const glm::dvec3& corner : {n - right - up, n + right - up, n + right + up, n - right + up})
            corners.push_back(project(corner));
        const QPolygonF poly(QList<QPointF>(corners.begin(), corners.end()));
        const bool hovered = clickable && poly.containsPoint(cursor, Qt::OddEvenFill);
        p.fill(corners, hovered ? kHover : kFace);
        p.polyline(corners, 1.0f, kEdge, true);
        const QString label = QString(side).toUpper();
        const double startU = -(label.size() * kAdvance - (kAdvance - 0.6)) * kLetter / 2.0;
        for (int i = 0; i < label.size(); ++i)
            for (const CubeStroke& stroke : cubeGlyph(char(label[i].unicode())))
            {
                std::vector<QPointF> pts;
                for (const glm::dvec2& g : stroke)
                    pts.push_back(project(n + right * (startU + (i * kAdvance + g.x) * kLetter) + up * ((g.y - 0.5) * kLetter)));
                p.polyline(pts, 1.2f, kLabel);
            }
        if (clickable)
            mViewCubeFaces.emplace_back(side, poly);
    }
}

namespace
{
const char* oppositeView(const std::string& v)
{
    static const std::map<std::string, const char*> kOpposite = {{"front", "back"}, {"back", "front"}, {"right", "left"},
                                                                 {"left", "right"}, {"top", "bottom"}, {"bottom", "top"}};
    const auto it = kOpposite.find(v);
    return it != kOpposite.end() ? it->second : nullptr;
}

QString viewLabel(const std::string& v)
{
    QString s = QString::fromStdString(v);
    return s.isEmpty() ? s : s.left(1).toUpper() + s.mid(1);
}
} // namespace

void ViewportWidget::switchView(const std::string& view)
{
    const bool own = mLookThrough == scene::kNoNode;
    if (own && mViewName == view)
    {
        if (const char* flip = oppositeView(view))
            setView(flip);
    }
    else
        setView(view);
    requestFrame();
    Q_EMIT viewStateChanged();
}

void ViewportWidget::drawViewSwitcher(OverlayPainter& p)
{
    if (!hud::shown(mHudPanel, "viewSwitcher"))
        return;
    const auto segments = hud::viewSwitcherSegments(width(), height(), hud::shown(mHudPanel, "viewCube"));
    if (segments.empty())
        return;
    const QPointF cursor = mapFromGlobal(QCursor::pos().toPointF());
    const bool own = mLookThrough == scene::kNoNode;
    const QRectF all = segments.front().rect.united(segments.back().rect).adjusted(-3, -3, 3, 3);
    p.rect(all, {0.0f, 0.0f, 0.0f, 0.45f});
    for (const hud::SwitcherSegment& seg : segments)
    {
        const std::string v = seg.view;
        const char* flip = oppositeView(v);
        const bool camera = v == "camera";
        const bool current = camera ? !own : own && (mViewName == v || (flip && mViewName == flip));
        const bool hot = seg.rect.contains(cursor);
        if (current)
            p.rect(seg.rect, {0.78f, 0.78f, 0.78f, 1.0f});
        else if (hot)
            p.rect(seg.rect, {0.32f, 0.32f, 0.32f, 0.9f});
        const glm::vec4 ink = current ? glm::vec4(0.08f, 0.08f, 0.08f, 1.0f) : hot ? glm::vec4(0.96f, 0.96f, 0.96f, 1.0f)
                                                                              : glm::vec4(0.78f, 0.78f, 0.78f, 1.0f);
        if (camera)
        {
            const QPointF c = seg.rect.center();
            p.icon(QRectF(c.x() - 8.0, c.y() - 8.0, 16.0, 16.0), "camera", ink);
            continue;
        }
        // The current side's own name: Back while looking from the back.
        const std::string shown = current && own ? mViewName : v;
        p.text(QPointF(seg.rect.center().x(), seg.rect.center().y() + 4.5), viewLabel(shown), ink, OverlayPainter::Center);
    }
}

bool ViewportWidget::viewSwitcherClick(QPointF at)
{
    if (!hud::shown(mHudPanel, "viewSwitcher"))
        return false;
    for (const hud::SwitcherSegment& seg : hud::viewSwitcherSegments(width(), height(), hud::shown(mHudPanel, "viewCube")))
    {
        if (!seg.rect.contains(at))
            continue;
        if (std::string_view(seg.view) == "camera")
        {
            auto* menu = new QMenu(this);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            fillCameraMenu(menu, this);
            menu->popup(mapToGlobal(seg.rect.bottomLeft().toPoint()));
        }
        else
            switchView(seg.view);
        return true;
    }
    return false;
}

bool ViewportWidget::viewCubeClick(QPointF at)
{
    for (const auto& [side, poly] : mViewCubeFaces)
        if (poly.containsPoint(at, Qt::OddEvenFill))
        {
            lookFromSide(side);
            return true;
        }
    return false;
}

void ViewportWidget::lookFromSide(const std::string& side)
{
    if (mLookThrough != scene::kNoNode)
        return;
    if (mViewName != "persp")
    {
        setView(side);
        return;
    }
    ui::Camera sides;
    sides.zUp = mCamera.zUp;
    sides.setView(side);
    glm::dvec3 f = glm::normalize(sides.orthoForward);
    if (mCamera.zUp)
        f = {f.x, f.z, -f.y}; // back to the Y-up frame yaw and pitch are measured in
    finishViewTurn();
    const double fromYaw = mCamera.yaw, fromPitch = mCamera.pitch;
    const double yaw = std::abs(f.y) > 0.999 ? 0.0 : glm::degrees(std::atan2(-f.x, -f.z));
    // The equivalent angle nearest the current yaw, so the turn takes the short way round.
    const double toYaw = fromYaw + std::remainder(yaw - fromYaw, 360.0);
    const double toPitch = std::clamp(glm::degrees(std::asin(f.y)), -89.9, 89.9);
    mViewTurnBefore = cameraView();
    const double seconds = PrefStore::get().value<double>("general", "/viewport/viewCube/turnTime", 0.15);
    if (seconds <= 0.0)
    {
        mCamera.yaw = toYaw;
        mCamera.pitch = toPitch;
        recordCamera(mViewTurnBefore);
        requestFrame();
        Q_EMIT viewStateChanged();
        return;
    }
    mViewTurn = new QVariantAnimation(this);
    mViewTurn->setDuration(std::max(1, int(seconds * 1000.0)));
    mViewTurn->setStartValue(0.0);
    mViewTurn->setEndValue(1.0);
    mViewTurn->setEasingCurve(QEasingCurve::OutCubic);
    connect(mViewTurn, &QVariantAnimation::valueChanged, this, [=, this](const QVariant& v) {
        const double t = v.toDouble();
        mCamera.yaw = fromYaw + (toYaw - fromYaw) * t;
        mCamera.pitch = fromPitch + (toPitch - fromPitch) * t;
        requestFrame();
    });
    connect(mViewTurn, &QVariantAnimation::finished, this, [this] { finishViewTurn(); });
    mViewTurn->start();
}

void ViewportWidget::finishViewTurn()
{
    if (!mViewTurn)
        return;
    QVariantAnimation* turn = std::exchange(mViewTurn, nullptr);
    if (turn->state() == QAbstractAnimation::Running)
        turn->setCurrentTime(turn->duration());
    turn->stop();
    turn->deleteLater();
    recordCamera(mViewTurnBefore);
    requestFrame();
    Q_EMIT viewStateChanged();
}

std::string ViewportWidget::skinnedMeshAt(QPointF at)
{
    scene::Graph& g = scene::Graph::current();
    for (const std::string& name : hitTest(at, at))
        if (scene::Node* n = findNode(name); n && n->type().isA("mesh") && scene::skin::find(g, n->id()) != scene::kNoNode)
            return name;
    return {};
}

namespace
{
/** How much closer than a plain fit a framing ends, from its Preferences > Viewport factor. */
double frameZoom(const char* key)
{
    return std::max(PrefStore::get().value<double>("general", key, 1.0), 0.01);
}
} // namespace

void ViewportWidget::frameSelection(bool all)
{
    const double zoom = frameZoom(all ? "/viewport/frameAll/zoomFactor" : "/viewport/frameSelected/zoomFactor");
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    if (mLookThrough != scene::kNoNode)
    {
        syncLookThrough();
        if (!beginCameraEdit())
            return;
    }
    CameraView before = cameraView();
    std::vector<scene::NodeId> ids = all ? g.nodes() : g.selection();
    const bool inherit = scene::inheritVisibility();
    glm::dvec3 lo(1e30), hi(-1e30);
    int count = 0;
    if (!all && scene::components::mode() != scene::ComponentMode::Object)
    {
        for (const auto& [id, picked] : scene::components::selected(scene::components::mode()))
        {
            scene::Node* n = g.node(id);
            if (!n || picked.empty())
                continue;
            if (scene::components::pointProvider(*n))
            {
                std::unordered_set<uint32_t> want(picked.begin(), picked.end());
                for (auto& [cid, w] : worldPoints(g, id))
                    if (want.contains(cid))
                    {
                        lo = glm::min(lo, w);
                        hi = glm::max(hi, w);
                        ++count;
                    }
                continue;
            }
            const scene::MeshPtr mesh = n->type().isA("mesh") ? scene::modifiers::cage(g, id) : nullptr;
            if (!mesh)
                continue;
            glm::dmat4 world = shownWorld(*n);
            for (uint32_t p : scene::components::pointsOf(id))
                if (mesh->pointValid(p))
                {
                    glm::dvec3 w(world * glm::dvec4(mesh->position(p), 1.0));
                    lo = glm::min(lo, w);
                    hi = glm::max(hi, w);
                    ++count;
                }
        }
        if (count > 0)
        {
            // A lone vertex has no extent; keep some context around it like Maya does.
            double radius = std::max(glm::length(hi - lo) * 0.5, 5.0) / zoom;
            mCamera.frame((lo + hi) * 0.5, radius, aspect());
            if (mLookThrough != scene::kNoNode)
                previewCameraEdit();
            recordCamera(before);
            commitCameraEdit();
            requestFrame();
            return;
        }
    }
    auto addGeometry = [&](scene::Node* n) {
        if (scene::components::pointProvider(*n))
        {
            auto pts = worldPoints(g, n->id());
            for (auto& [cid, w] : pts)
            {
                lo = glm::min(lo, w);
                hi = glm::max(hi, w);
            }
            return !pts.empty();
        }
        if (!n->type().isA("mesh"))
            return false;
        auto rm = renderMeshFor(n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh));
        if (!rm || rm->vertices.empty())
            return false;
        const glm::dmat4 world = shownWorld(*n);
        std::vector<glm::dmat4> placed{world};
        for (const scene::arrays::Copy& c : scene::arrays::copiesOf(g, n->id()))
            placed.push_back(c.transform * world);
        for (const glm::dmat4& at : placed)
            for (int c = 0; c < 8; ++c)
            {
                glm::dvec3 corner((c & 1) ? rm->boundsMax.x : rm->boundsMin.x, (c & 2) ? rm->boundsMax.y : rm->boundsMin.y,
                                  (c & 4) ? rm->boundsMax.z : rm->boundsMin.z);
                glm::dvec3 w(at * glm::dvec4(corner, 1.0));
                lo = glm::min(lo, w);
                hi = glm::max(hi, w);
            }
        return true;
    };
    bool group = false;
    for (scene::NodeId id : ids)
    {
        scene::Node* n = g.node(id);
        if (!n || !n->type().dag || id == mLookThrough || (all && !scene::effectivelyVisible(g, id, inherit)))
            continue;
        bool geometry = addGeometry(n);
        if (!all)
            for (scene::NodeId d : g.descendants(id))
                if (scene::Node* dn = g.node(d); dn && dn->type().dag && scene::effectivelyVisible(g, d, inherit) && addGeometry(dn))
                {
                    group = group || !n->type().isA("mesh");
                    geometry = true;
                }
        // Only geometry decides the framing; a bare pivot counts when nothing under it has any.
        if (!geometry)
        {
            glm::dvec3 p(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
            lo = glm::min(lo, p - 1.0);
            hi = glm::max(hi, p + 1.0);
        }
        ++count;
    }
    if (count == 0)
    {
        lo = glm::dvec3(-50.0);
        hi = glm::dvec3(50.0);
    }
    // Something on a turntable is framed for its whole turn, so it never leaves the view while it spins.
    else if (scene::turntable::sweepBounds(g, ids, lo, hi))
        group = false;
    // Maya frames a group from further out than a single mesh.
    double radius = glm::length(hi - lo) * 0.5 * (group ? 2.0 : 1.0) / zoom;
    mCamera.frame((lo + hi) * 0.5, radius, aspect());
    if (mLookThrough != scene::kNoNode)
        previewCameraEdit();
    recordCamera(before);
    commitCameraEdit();
    requestFrame();
}

std::optional<glm::dvec3> ViewportWidget::surfaceAt(QPointF px)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const ViewInfo view = viewInfo();
    glm::dvec3 o, d;
    view.ray(px, o, d);
    const bool inherit = scene::inheritVisibility();
    const std::unordered_set<scene::NodeId> lodHidden = scene::lod::hidden(g, lodDisplay::view(mHudPanel, view));
    double nearest = 1e300;
    for (scene::NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!n->type().isA("mesh") || overridden(*n) || !scene::effectivelyVisible(g, id, inherit) || !mIsolation.shows(id) ||
            !scene::selectionMask::shown(*n) || viewportHooks::hiddenInView(id) || lodHidden.contains(id))
            continue;
        const int smooth = int(n->as<int64_t>(scene::MeshNode::kSmoothPreview));
        const scene::MeshPtr& mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        const glm::dmat4 world = shownWorld(*n);
        std::vector<glm::dmat4> placed{world};
        for (const scene::arrays::Copy& c : scene::arrays::copiesOf(g, id))
            placed.push_back(c.transform * world);
        for (const glm::dmat4& at : placed)
            if (auto hit = isolate::raycast(&mIsolation, id, mesh, at, o, d, smooth); hit && hit->distance < nearest)
                nearest = hit->distance;
    }
    if (nearest == 1e300)
        return std::nullopt;
    return o + d * nearest;
}

bool ViewportWidget::frameAt(QPointF px)
{
    std::optional<glm::dvec3> hit = surfaceAt(px);
    if (!hit)
        return false;
    if (mLookThrough != scene::kNoNode)
    {
        syncLookThrough();
        if (!beginCameraEdit())
            return true;
    }
    CameraView before = cameraView();
    const glm::dvec3 eye = mCamera.eye();
    mCamera.target = *hit;
    const double zoom = PrefStore::get().value<bool>("general", "/viewport/frameUnderCursor/zoom", true)
                            ? std::max(PrefStore::get().value<double>("general", "/viewport/frameUnderCursor/zoomFactor", 8.0), 0.01)
                            : 1.0;
    if (mCamera.ortho)
        mCamera.orthoWidth /= zoom;
    else
        mCamera.distance = std::max(glm::length(*hit - eye) / zoom, 0.5);
    if (mLookThrough != scene::kNoNode)
        previewCameraEdit();
    recordCamera(before);
    commitCameraEdit();
    requestFrame();
    return true;
}

void ViewportWidget::frameUnderCursor()
{
    const QPointF cursor = mapFromGlobal(QCursor::pos().toPointF());
    if (rect().contains(cursor.toPoint()))
    {
        frameAt(cursor);
        return;
    }
    mFramePickArmed = true;
    updateCursor();
}

} // namespace lila::ui
