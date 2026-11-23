#include "ui/TextureGraphUi.h"

#include "core/Profile.h"
#include "geo/Mesh.h"
#include "render/PaintGpu.h"
#include "render/RenderMesh.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
// Plan.h has a member named like Qt's slots macro.
#pragma push_macro("slots")
#undef slots
#include "tex/Ports.h"
#include "tex/TextureSet.h"
#pragma pop_macro("slots")
#include "ui/Controls.h"
#include "ui/FlowLayout.h"
#include "ui/Icons.h"
#include "ui/MaterialDraw.h"
#include "ui/TexCompare.h"
#include "ui/QuickMaskOverlay.h"
#include "ui/TexLive.h"
#include "ui/TexPreview.h"
#include "ui/TexturePaintTool.h"
#include "ui/TextureSetPanel.h"
#include "ui/Theme.h"
#include "ui/UiCommon.h"
#include "ui/UvTexturePaint.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QButtonGroup>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <numeric>
#include <array>
#include <cmath>
#include <cstring>
#include <map>

namespace lila::ui
{
namespace
{
constexpr const char* kMeshHook = "texture2d.meshPreview";

/** What the 2D View and the mesh preview show, shared by every view and kept while none is open. */
struct Controller
{
    texture2d::Source source;
    int size = 512;
    texPreview::Result result;
    bool onMesh = false;
    render::TexturePtr meshTexture;
    const image::Image* meshFrom = nullptr;
    std::map<int, std::function<void()>> listeners;
    int nextListener = 1;
    int sceneListener = 0;
    int paintListener = 0;
    bool queued = false;
    /** When paint last changed the shown set: edits to it soon after are the paint's too, and wait for it to settle. */
    QElapsedTimer painted;
    /** The canvases of the open views: with none shown and no mesh preview, nothing renders until one shows. */
    std::vector<QPointer<QWidget>> canvases;
    bool skipped = false;
    /** Slot name -> bound set, rebuilt after scene edits. */
    std::map<std::string, std::string> bindings;
    bool bindingsDirty = true;
};

Controller& ctl()
{
    static Controller c;
    return c;
}

void request();

void requestSoon()
{
    Controller& c = ctl();
    if (c.queued || !QCoreApplication::instance())
        return;
    c.queued = true;
    QTimer::singleShot(0, QCoreApplication::instance(), [] {
        ctl().queued = false;
        request();
    });
}

/** How long paint must pause before the preview renders it again. */
constexpr int kPaintSettleMs = 1500;

/** A request once paint has paused for kPaintSettleMs; each call restarts the wait. */
void requestSettled()
{
    static QTimer* settle = nullptr;
    if (!QCoreApplication::instance())
        return;
    if (!settle)
    {
        settle = new QTimer(QCoreApplication::instance());
        settle->setSingleShot(true);
        settle->setInterval(kPaintSettleMs);
        QObject::connect(settle, &QTimer::timeout, [] { requestSoon(); });
    }
    settle->start();
}

void notify()
{
    std::vector<std::function<void()>> fns;
    for (auto& [id, fn] : ctl().listeners)
        fns.push_back(fn);
    for (auto& fn : fns)
        fn();
}

const std::string& texBoundSet(const std::string& slot)
{
    Controller& c = ctl();
    if (c.bindingsDirty)
    {
        c.bindings.clear();
        c.bindingsDirty = false;
    }
    auto it = c.bindings.find(slot);
    if (it == c.bindings.end())
        it = c.bindings.emplace(slot, textureSetForSlot(scene::Graph::current(), slot)).first;
    return it->second;
}

void installSceneListener()
{
    Controller& c = ctl();
    if (c.sceneListener)
        return;
    // The views draw the live channel while painting; the preview (histogram, pixel readout) catches up once painting
    // pauses, as each update reads every paint buffer back and evaluates the whole graph again.
    c.paintListener = render::paint::addChangeListener([](scene::NodeId id) {
        const scene::Node* set = tex::findSet(scene::Graph::current(), tex::splitAddress(ctl().source.set).set);
        if (!set || set->id() != id || !QCoreApplication::instance())
            return;
        ctl().painted.start();
        requestSettled();
    });
    c.sceneListener = scene::Graph::current().addListener([](scene::Change change, scene::NodeId id) {
        Controller& c = ctl();
        if (change == scene::Change::Selection || change == scene::Change::Connection || change == scene::Change::Reparented)
            return;
        if (change == scene::Change::Value)
        {
            scene::Node* n = scene::Graph::current().node(id);
            if (!n || n->type().name != tex::kTextureSetType)
                return;
            c.bindingsDirty = true;
            if (n->name() == c.source.set)
            {
                if (c.painted.isValid() && c.painted.elapsed() < kPaintSettleMs)
                    requestSettled();
                else
                    requestSoon();
            }
            return;
        }
        c.bindingsDirty = true;
        requestSoon();
    });
}

void updateMeshTexture()
{
    Controller& c = ctl();
    const image::Image* img = c.result.image.get();
    if (img == c.meshFrom)
        return;
    c.meshFrom = img;
    c.meshTexture.reset();
    if (img && img->width > 0)
    {
        QImage q = texPreview::toQImage(*img, c.result.type, true);
        auto t = std::make_shared<render::TextureImage>();
        t->width = q.width();
        t->height = q.height();
        t->format = render::TextureFormat::RGBA8;
        t->srgb = true;
        t->rgba.resize(size_t(q.width()) * size_t(q.height()) * 4);
        for (int y = 0; y < q.height(); ++y)
            std::memcpy(&t->rgba[size_t(y) * size_t(q.width()) * 4], q.constScanLine(y), size_t(q.width()) * 4);
        c.meshTexture = std::move(t);
    }
    viewportTools::requestRedraw();
}

/** A view shows or the mesh preview is on: someone sees what a render would make. */
bool wanted()
{
    Controller& c = ctl();
    if (c.onMesh)
        return true;
    for (const QPointer<QWidget>& w : c.canvases)
        if (w && w->isVisible())
            return true;
    return false;
}

void request()
{
    LILA_ZONE();
    Controller& c = ctl();
    // A hidden view's preview reads every paint buffer back and evaluates the graph again on each edit.
    c.skipped = !wanted();
    if (c.skipped)
        return;
    if (c.source.set.empty() || !tex::hasGraphAt(scene::Graph::current(), c.source.set))
    {
        c.result = {};
        if (c.source.set.empty() == false)
            c.result.error = "no texture set named '" + c.source.set + "'";
    }
    else
        c.result = texPreview::get({c.source.set, c.source.node, c.source.socket, c.source.channel, c.size, true}, [] { requestSoon(); });
    updateMeshTexture();
    notify();
}

bool meshHook(scene::NodeId, const geo::Mesh& mesh, std::vector<render::Material>& out)
{
    Controller& c = ctl();
    if (!c.onMesh || !c.meshTexture || c.source.set.empty())
        return false;
    const size_t slotCount = std::max<size_t>(mesh.materialNames().size(), 1);
    std::vector<size_t> hits;
    for (size_t i = 0; i < slotCount; ++i)
        if (texBoundSet(scene::materials::slotName(mesh, int32_t(i))) == c.source.set)
            hits.push_back(i);
    if (hits.empty())
        return false;
    out = drawMaterials(mesh);
    for (size_t i : hits)
        if (i < out.size())
        {
            out[i].baseColorMap = c.meshTexture;
            out[i].baseColor = glm::vec3(1.0f);
            out[i].flags |= render::MatBaseColorMap;
        }
    return true;
}

/** "Channel: baseColor" / "Bricks.mask", and the source they stand for, for the current set. */
std::vector<std::pair<QString, texture2d::Source>> sourcesOf(const std::string& set)
{
    std::vector<std::pair<QString, texture2d::Source>> out;
    scene::Node* n = set.empty() ? nullptr : tex::findSet(scene::Graph::current(), tex::splitAddress(set).set);
    if (!n)
        return out;
    try
    {
        tex::Graph g = tex::readGraph(*n);
        tex::SetSettings s = tex::readSettings(*n);
        for (const tex::Channel& ch : s.channels)
            out.push_back({"Channel: " + qs(ch.name), {set, {}, {}, ch.name}});
        tex::ChannelList extra = tex::extraChannels(s);
        for (const tex::GNode& gn : g.nodes)
        {
            tex::NodePorts p = tex::portsOf(g, gn, &extra);
            for (const tex::PortDecl& o : p.outputs)
                out.push_back({qs(gn.name + "." + o.id), {set, gn.name, o.id, {}}});
        }
    }
    catch (const std::exception&)
    {
    }
    return out;
}

QString describe(const texture2d::Source& s)
{
    if (s.set.empty())
        return {};
    if (!s.channel.empty())
        return qs(s.set) + "  channel " + qs(s.channel);
    return qs(s.set) + "  " + qs(s.node) + (s.socket.empty() ? QString() : "." + qs(s.socket));
}

/** The image area: zoom with the wheel, pan with any drag, Alt+left drag to rotate (Shift snaps), double-click or F to fit. */
class Canvas final : public QWidget
{
public:
    explicit Canvas(QWidget* parent) : QWidget(parent)
    {
        setMouseTracking(true);
        setFocusPolicy(Qt::ClickFocus);
        setMinimumSize(160, 160);
        brush.onUpdate = [this] {
            if (!mLive && !paintsHere())
                return;
            // Paint landing from the viewport refreshes this view at a lower rate: each refresh reads the channel back.
            if (mStroking)
                update();
            else if (!mThrottle.isActive())
                mThrottle.start();
        };
        mThrottle.setSingleShot(true);
        mThrottle.setInterval(100);
        connect(&mThrottle, &QTimer::timeout, this, [this] { update(); });
    }

    QImage image;
    bool tiled = false;
    bool histogram = true;
    /** Bins per channel (R, G, B, A); empty before an image. */
    std::array<std::vector<int>, 4> bins;
    int histChannels = 3;
    /** UV edges in the image's unit square, v down; bump uvVersion when they change. */
    QList<QLineF> uvLines;
    uint64_t uvVersion = 0;
    /** The UV edges drawn at the canvas's size, redrawn only when the view, the edges or the dimming change. */
    QImage uvCache;
    QTransform uvCacheTransform;
    uint64_t uvCacheVersion = 0;
    bool uvCacheDim = false;
    bool showUvs = true;
    /** Shown in place of the image while there is none. */
    QString emptyText;
    std::function<void(int x, int y, bool inside)> onHover;
    /** The quick mask tint last drawn, and its version. */
    QImage mTint;
    uint64_t mTintVersion = 0;

    /** The selected meshes' UVs (the set's meshes when none is selected), in the unit square with v down; fit frames them. */
    QRectF focus;
    /** The texture paint brush in uv space, as the UV editor paints: on while Texture Paint is the active tool. */
    UvTexturePaint brush;

    /** Texture Paint is the active tool and there is an image to paint over. */
    bool paintsHere() const { return !image.isNull() && brush.shown() && texturePaintActive(); }

    /** The image shown is a set channel in full colour, so the live paint can stand in for it during a stroke. */
    bool liveMatches = false;

    /** A settled image arrived: once it is newer than the last stroke, it replaces the live paint. */
    void settled()
    {
        if (mLive && !mStroking && mReleased.isValid() && mReleased.elapsed() > 300)
        {
            mLive = false;
            update();
        }
    }

    double pxPerUv() const { return double(image.width()) * mScale; }

    UvTexturePaint::ToUv toUvFn() const
    {
        return [this](const QPointF& w) {
            const QPointF t = toTexel(w);
            return glm::dvec2(t.x() / image.width(), 1.0 - t.y() / image.height());
        };
    }

    /** Frames the focus, else the whole image; the view then follows resizes and new focus until zoomed or panned. */
    void fit()
    {
        mUserView = false;
        mAngle = 0.0;
        if (image.isNull())
            return;
        const QSizeF size(image.size());
        if (!tiled && focus.isValid() && !focus.isEmpty())
        {
            QRectF r(focus.x() * size.width(), focus.y() * size.height(), focus.width() * size.width(), focus.height() * size.height());
            r = r.marginsAdded(QMarginsF(r.width(), r.height(), r.width(), r.height()) * 0.04);
            mScale = std::clamp(0.92 * std::min(width() / r.width(), height() / r.height()), 0.02, 64.0);
            mPan = (QPointF(size.width(), size.height()) * 0.5 - r.center()) * mScale;
        }
        else
        {
            const double span = std::max(size.width(), size.height()) * (tiled ? 3.0 : 1.0);
            mScale = 0.92 * std::min(width(), height()) / span;
            mPan = {};
        }
        update();
    }

    void setFocus(const QRectF& r)
    {
        if (r == focus)
            return;
        focus = r;
        if (!mUserView)
            fit();
    }

    void setImage(QImage next)
    {
        const bool resized = image.size() != next.size();
        image = std::move(next);
        if (resized)
            fit();
        update();
    }

protected:
    QPointF origin() const { return QPointF(width() * 0.5, height() * 0.5) + mPan; }

    QPointF centre() const { return QPointF(width() * 0.5, height() * 0.5); }

    /** A widget point in the unrotated view, where origin(), mScale and mPan apply. */
    QPointF unrotate(QPointF w) const
    {
        const double a = -mAngle * glm::pi<double>() / 180.0;
        const QPointF d = w - centre();
        return centre() + QPointF(d.x() * std::cos(a) - d.y() * std::sin(a), d.x() * std::sin(a) + d.y() * std::cos(a));
    }

    QPointF toTexel(QPointF w) const
    {
        QPointF o = origin() - QPointF(image.width(), image.height()) * mScale * 0.5;
        return (unrotate(w) - o) / mScale;
    }

    /** The cursor angle around the centre, in degrees. */
    double angleAt(QPointF w) const
    {
        const QPointF d = w - centre();
        return std::atan2(d.y(), d.x()) * 180.0 / glm::pi<double>();
    }

    /** The UVs show unless painting hides them, as Fill picking faces still needs them, or dims them. */
    bool uvsShown() const { return showUvs && !uvLines.isEmpty() && (!paintsHere() || texturePaintShowsWire() || texturePaintDimWire()); }
    bool uvsDimmed() const { return paintsHere() && !texturePaintShowsWire(); }

    /** The live paint over the image while strokes land, in the unrotated view. */
    void drawLive(QPainter& p, const QPointF& topLeft, const QSizeF& sz)
    {
        QTransform t;
        t.translate(centre().x(), centre().y());
        t.rotate(-mAngle);
        t.translate(-centre().x(), -centre().y());
        const QRect area = t.mapRect(QRectF(rect())).toAlignedRect();
        const auto toScreen = [&](const glm::dvec2& uv) { return topLeft + QPointF(uv.x * sz.width(), (1.0 - uv.y) * sz.height()); };
        const auto toUv = [&](const QPointF& w) {
            const QPointF f = w - topLeft;
            return glm::dvec2(f.x() / sz.width(), 1.0 - f.y() / sz.height());
        };
        brush.drawBackground(p, area, toScreen, toUv, false);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), gray(30));
        if (image.isNull())
        {
            // The UV square stands in for the texture, so the layout shows before the first render.
            const double side = 0.92 * std::min(width(), height());
            const QRectF square(width() * 0.5 - side * 0.5, height() * 0.5 - side * 0.5, side, side);
            if (showUvs && !uvLines.isEmpty())
            {
                p.fillRect(square, gray(42));
                drawUvs(p, square.topLeft(), square.size());
            }
            p.setPen(gray(140));
            p.drawText(rect(), Qt::AlignCenter, emptyText);
            return;
        }
        p.translate(centre());
        p.rotate(mAngle);
        p.translate(-centre());
        p.setRenderHint(QPainter::SmoothPixmapTransform, mScale < 1.0);
        const QSizeF sz = QSizeF(image.size()) * mScale;
        const QPointF topLeft = origin() - QPointF(sz.width(), sz.height()) * 0.5;
        const int reach = tiled ? 1 : 0;
        for (int ty = -reach; ty <= reach; ++ty)
            for (int tx = -reach; tx <= reach; ++tx)
                p.drawImage(QRectF(topLeft + QPointF(tx * sz.width(), ty * sz.height()), sz), image);
        // Only the set's evaluated channel stands in for the image; a set that isn't live would show its bare layer.
        if ((mLive || paintsHere()) && liveMatches && texLive::channelImage(ctl().source.set, ctl().source.channel))
            drawLive(p, topLeft, sz);
        texCompare::paint2D(p, QRectF(topLeft, sz));
        if (const QImage tint = quickMaskTint(); !tint.isNull())
        {
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            for (int ty = -reach; ty <= reach; ++ty)
                for (int tx = -reach; tx <= reach; ++tx)
                    p.drawImage(QRectF(topLeft + QPointF(tx * sz.width(), ty * sz.height()), sz), tint);
        }
        if (tiled)
        {
            p.setPen(QPen(gray(200, 120), 1.0, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(topLeft, sz));
        }
        if (uvsShown())
            drawUvs(p, topLeft, sz, uvsDimmed());
        p.resetTransform();
        if (histogram && !bins[0].empty())
            drawHistogram(p);
        if (paintsHere() && rect().contains(mMouse.toPoint()))
            brush.drawCursor(p, mMouse, toUvFn(), pxPerUv());
    }

    /** The quick mask's tint while it limits strokes on the set shown, kept per tint version. */
    QImage quickMaskTint()
    {
        const std::string& set = ctl().source.set;
        uint64_t version = 0;
        if (set.empty() || quickMaskSet() != set || !quickMaskOverlay::style().shown || !quickMaskOverlay::tint(set, &version))
        {
            mTint = {};
            return {};
        }
        if (version != mTintVersion || mTint.isNull())
        {
            mTint = quickMaskOverlay::tintImage(set);
            mTintVersion = version;
        }
        return mTint;
    }

    void drawUvs(QPainter& p, const QPointF& topLeft, const QSizeF& size, bool dim = false)
    {
        // Thousands of antialiased edges take milliseconds; repaints while paint lands reuse them.
        QTransform t = p.worldTransform();
        t.translate(topLeft.x(), topLeft.y());
        t.scale(size.width(), size.height());
        const qreal dpr = devicePixelRatioF();
        const QSize pixels = (QSizeF(this->size()) * dpr).toSize();
        if (uvCache.size() != pixels || uvCacheTransform != t || uvCacheVersion != uvVersion || uvCacheDim != dim)
        {
            uvCache = QImage(pixels, QImage::Format_ARGB32_Premultiplied);
            uvCache.setDevicePixelRatio(dpr);
            uvCache.fill(Qt::transparent);
            QPainter c(&uvCache);
            c.setWorldTransform(t);
            c.setRenderHint(QPainter::Antialiasing, uvLines.size() < 20000);
            // Dark under light, so the edges read on any texture.
            QPen under(gray(0, dim ? 40 : 110), 3.0);
            under.setCosmetic(true);
            c.setPen(under);
            c.drawLines(uvLines);
            QPen pen(dim ? gray(200, 70) : gray(240, 200));
            pen.setCosmetic(true);
            c.setPen(pen);
            c.drawLines(uvLines);
            uvCacheTransform = t;
            uvCacheVersion = uvVersion;
            uvCacheDim = dim;
        }
        p.save();
        p.resetTransform();
        p.drawImage(QPointF(0.0, 0.0), uvCache);
        p.restore();
    }

    void drawHistogram(QPainter& p)
    {
        const QRectF r(width() - 172.0, height() - 72.0, 164.0, 64.0);
        p.setPen(Qt::NoPen);
        p.setBrush(gray(18, 200));
        p.drawRect(r);
        int peak = 1;
        for (int c = 0; c < histChannels; ++c)
            for (size_t b = 1; b + 1 < bins[size_t(c)].size(); ++b)
                peak = std::max(peak, bins[size_t(c)][b]);
        // Data colours: one curve per channel, white when there is only one.
        static const QColor kColors[4] = {QColor(235, 80, 80), QColor(90, 210, 90), QColor(90, 140, 240), QColor(220, 220, 220)};
        p.setRenderHint(QPainter::Antialiasing);
        for (int c = 0; c < histChannels; ++c)
        {
            const std::vector<int>& h = bins[size_t(c)];
            QPolygonF line;
            for (size_t b = 0; b < h.size(); ++b)
            {
                double x = r.left() + 2.0 + (r.width() - 4.0) * double(b) / double(h.size() - 1);
                double y = r.bottom() - 2.0 - (r.height() - 4.0) * std::min(1.0, double(h[b]) / peak);
                line << QPointF(x, y);
            }
            p.setPen(QPen(histChannels == 1 ? kColors[3] : kColors[c], 1.2));
            p.drawPolyline(line);
        }
    }

    void wheelEvent(QWheelEvent* e) override
    {
        const double f = std::pow(1.0015, e->angleDelta().y());
        const double next = std::clamp(mScale * f, 0.02, 64.0);
        const QPointF c = unrotate(e->position()) - centre();
        mPan = c - (c - mPan) * (next / mScale);
        mScale = next;
        mUserView = true;
        update();
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::AltModifier))
        {
            mRotating = true;
            mRotateFrom = mAngle - angleAt(e->position());
            mUserView = true;
            return;
        }
        // With Texture Paint on, the left button paints; middle and right always pan.
        if (paintsHere() && brush.mousePress(e, toUvFn(), pxPerUv()))
        {
            mStroking = true;
            mLive = true;
            return;
        }
        mLast = e->position();
        mDragging = true;
        setCursor(Qt::ClosedHandCursor);
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        mMouse = e->position();
        if (mRotating)
        {
            double a = mRotateFrom + angleAt(e->position());
            if (e->modifiers() & Qt::ShiftModifier)
                a = std::round(a / 15.0) * 15.0;
            mAngle = std::remainder(a, 360.0);
            update();
            return;
        }
        if (mStroking)
        {
            brush.mouseMove(e, toUvFn(), pxPerUv());
            update();
            return;
        }
        if (paintsHere())
            update();
        if (mDragging && (e->buttons() & (Qt::LeftButton | Qt::MiddleButton | Qt::RightButton)))
        {
            mPan += unrotate(e->position()) - unrotate(mLast);
            mUserView = true;
            mLast = e->position();
            update();
        }
        if (onHover && !image.isNull())
        {
            QPointF t = toTexel(e->position());
            int x = int(std::floor(t.x())), y = int(std::floor(t.y()));
            bool inside = x >= 0 && y >= 0 && x < image.width() && y < image.height();
            if (tiled && !inside && x >= -image.width() && y >= -image.height() && x < 2 * image.width() && y < 2 * image.height())
            {
                x = (x % image.width() + image.width()) % image.width();
                y = (y % image.height() + image.height()) % image.height();
                inside = true;
            }
            onHover(x, y, inside);
        }
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (mRotating)
        {
            mRotating = false;
            return;
        }
        if (mStroking)
        {
            brush.mouseRelease(e, toUvFn(), pxPerUv());
            mStroking = false;
            mReleased.start();
            return;
        }
        mDragging = false;
        unsetCursor();
    }

    void leaveEvent(QEvent*) override
    {
        mMouse = QPointF(-1e6, -1e6);
        update();
    }

    void mouseDoubleClickEvent(QMouseEvent*) override { fit(); }

    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_F || e->key() == Qt::Key_Home)
            fit();
        else
            QWidget::keyPressEvent(e);
    }

    void resizeEvent(QResizeEvent*) override
    {
        if (!mUserView)
            fit();
    }

    void showEvent(QShowEvent*) override
    {
        if (!mUserView)
            fit();
        if (ctl().skipped)
            requestSoon();
    }

private:
    double mScale = 1.0;
    QPointF mPan;
    QPointF mLast;
    bool mDragging = false;
    /** Zoomed or panned by hand: resizes and focus changes leave the view alone until the next fit. */
    bool mUserView = false;
    bool mStroking = false;
    QPointF mMouse{-1e6, -1e6};
    /** View rotation in degrees, clockwise; the drag's offset from the cursor angle while rotating. */
    double mAngle = 0.0;
    double mRotateFrom = 0.0;
    bool mRotating = false;
    /** The live paint shows over the image from a stroke's press until a settled image after its release. */
    bool mLive = false;
    QElapsedTimer mReleased;
    QTimer mThrottle;
};
} // namespace

std::string textureSetForSlot(scene::Graph& g, const std::string& slot) { return tex::setForMaterial(g, slot); }

namespace texture2d
{
void view(const Source& source)
{
    installSceneListener();
    Controller& c = ctl();
    c.source = source;
    c.onMesh = true;
    viewportHooks::setMaterialHook(kMeshHook, meshHook);
    request();
}

Source current() { return ctl().source; }

void showSet(const std::string& set)
{
    Controller& c = ctl();
    if (c.source.set == set)
        return;
    // No set shows nothing, rather than the last set picked.
    c.source = {set, {}, {}, c.source.channel.empty() ? std::string("baseColor") : c.source.channel};
    // With no view open the next one renders it.
    if (!c.listeners.empty())
        request();
}

void clearMeshPreview()
{
    ctl().onMesh = false;
    viewportTools::requestRedraw();
    notify();
}

bool meshPreviewShown() { return ctl().onMesh && ctl().meshTexture != nullptr; }
} // namespace texture2d

struct Texture2DView::Impl
{
    Canvas* canvas = nullptr;
    QComboBox* source = nullptr;
    QComboBox* resolution = nullptr;
    QToolButton* tile = nullptr;
    QToolButton* hist = nullptr;
    QToolButton* onMesh = nullptr;
    QToolButton* uvs = nullptr;
    controls::Segmented* solo = nullptr;
    QLabel* status = nullptr;
    QLabel* pixel = nullptr;
    int listener = 0;
    int soloChannel = 0;
    std::shared_ptr<const image::Image> data;
    std::string type;
    std::string shownSet;
    texture2d::Source shownSource;
    std::vector<const geo::Mesh*> uvMeshes;
    bool showing = true;
    int sceneListener = 0;
    int redrawListener = 0;
    bool uvsQueued = false;

    /** The UV bounds of a mesh's faces in the slots, in the unit square with v down; empty without UVs. */
    static QRectF uvBounds(const geo::Mesh& mesh, const std::vector<int32_t>& slots)
    {
        const std::span<const glm::vec2> uv = mesh.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
        const std::span<const int32_t> material = mesh.faceAttrs().get<int32_t>(geo::attr::kMaterial);
        glm::vec2 lo(1e30f), hi(-1e30f);
        if (!uv.empty())
            for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
            {
                if (!mesh.faceValid(f) || std::find(slots.begin(), slots.end(), material.empty() ? 0 : material[f]) == slots.end())
                    continue;
                for (geo::Index c : mesh.faceCorners(f))
                {
                    lo = glm::min(lo, uv[c]);
                    hi = glm::max(hi, uv[c]);
                }
            }
        if (lo.x > hi.x)
            return {};
        return QRectF(QPointF(lo.x, 1.0 - hi.y), QPointF(hi.x, 1.0 - lo.y));
    }

    /** The UV edges of the faces the shown set textures, on every mesh whose material uses it; with no set, the selected meshes'. */
    void rebuildUvs()
    {
        const std::string& set = ctl().source.set;
        scene::Graph& g = scene::Graph::current();
        std::vector<std::pair<scene::MeshPtr, std::vector<int32_t>>> bound;
        std::vector<const geo::Mesh*> meshes;
        std::vector<bool> selected;
        if (set.empty() && !g.selection().empty())
            for (scene::NodeId id : texturePaintTargets())
            {
                scene::MeshPtr mesh = g.node(id)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
                if (!mesh)
                    continue;
                std::vector<int32_t> slots(std::max<size_t>(mesh->materialNames().size(), 1));
                std::iota(slots.begin(), slots.end(), 0);
                meshes.push_back(mesh.get());
                bound.emplace_back(std::move(mesh), std::move(slots));
                selected.push_back(true);
            }
        if (!set.empty())
            for (scene::NodeId id : g.nodes())
            {
                scene::Node* n = g.node(id);
                if (!n || !n->type().isA("mesh"))
                    continue;
                scene::MeshPtr mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
                if (!mesh)
                    continue;
                std::vector<int32_t> slots;
                const size_t slotCount = std::max<size_t>(mesh->materialNames().size(), 1);
                for (size_t i = 0; i < slotCount; ++i)
                    if (texBoundSet(scene::materials::slotName(*mesh, int32_t(i))) == set)
                        slots.push_back(int32_t(i));
                if (slots.empty())
                    continue;
                meshes.push_back(mesh.get());
                bound.emplace_back(std::move(mesh), std::move(slots));
                selected.push_back(g.isSelected(id) || g.isSelected(g.parent(id)));
            }
        const bool anySelected = std::find(selected.begin(), selected.end(), true) != selected.end();
        QRectF box;
        for (size_t m = 0; m < bound.size(); ++m)
            if (selected[m] || !anySelected)
                box = box.united(uvBounds(*bound[m].first, bound[m].second));
        canvas->setFocus(box);
        if (meshes == uvMeshes && showing == canvas->showUvs)
            return;
        uvMeshes = std::move(meshes);
        showing = canvas->showUvs;
        QList<QLineF> lines;
        for (const auto& [mesh, slots] : canvas->showUvs ? bound : decltype(bound){})
        {
            const std::span<const glm::vec2> uv = mesh->cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
            if (uv.empty())
                continue;
            const std::span<const int32_t> material = mesh->faceAttrs().get<int32_t>(geo::attr::kMaterial);
            for (geo::Index f = 0; f < mesh->faceCapacity(); ++f)
            {
                if (!mesh->faceValid(f) || std::find(slots.begin(), slots.end(), material.empty() ? 0 : material[f]) == slots.end())
                    continue;
                const std::vector<geo::Index> corners = mesh->faceCorners(f);
                for (size_t i = 0; i < corners.size(); ++i)
                {
                    const glm::vec2 a = uv[corners[i]], b = uv[corners[(i + 1) % corners.size()]];
                    lines.push_back(QLineF(a.x, 1.0 - a.y, b.x, 1.0 - b.y));
                }
            }
        }
        canvas->uvLines = std::move(lines);
        ++canvas->uvVersion;
        canvas->update();
    }

    void rebuildSources()
    {
        const texture2d::Source cur = ctl().source;
        QSignalBlocker block(source);
        source->clear();
        mSources = sourcesOf(cur.set);
        int at = -1;
        for (size_t i = 0; i < mSources.size(); ++i)
        {
            source->addItem(mSources[i].first);
            const texture2d::Source& s = mSources[i].second;
            // A node shown by name alone shows its first output.
            if (at < 0 && s.channel == cur.channel && s.node == cur.node && (s.socket == cur.socket || cur.socket.empty()))
                at = int(i);
        }
        source->setCurrentIndex(at);
        source->setEnabled(!mSources.empty());
        source->setToolTip(cur.set.empty() ? "Ctrl+Shift+click a node in the Texture Graph to show it here" : "What " + qs(cur.set) + " shows here");
    }

    /** The set the tiling was last set for. */
    std::string tiledFor;

    void redisplay()
    {
        if (!data)
        {
            canvas->setImage({});
            canvas->bins = {};
            return;
        }
        const texture2d::Source& s = ctl().source;
        const std::string name = !s.channel.empty() ? s.channel : s.socket.empty() ? s.node : s.socket;
        canvas->setImage(texPreview::toQImage(*data, type, texPreview::isColor(type, name), soloChannel - 1));
        canvas->liveMatches = !s.channel.empty() && soloChannel == 0;
        // A tileable set shows tiled the first time it comes up, so strokes across its edges are seen wrapping.
        if (s.set != tiledFor)
        {
            tiledFor = s.set;
            if (scene::Node* set = tex::findSet(scene::Graph::current(), tex::splitAddress(s.set).set); set && tex::readSettings(*set).tileable && !tile->isChecked())
                tile->setChecked(true);
        }
        computeHistogram();
    }

    void computeHistogram()
    {
        constexpr int kBins = 64;
        for (auto& b : canvas->bins)
            b.assign(kBins, 0);
        const bool single = soloChannel > 0 || type == "gray" || type == "float" || type == "sdf2d";
        canvas->histChannels = single ? 1 : 3;
        const int first = soloChannel > 0 ? soloChannel - 1 : 0;
        const size_t texels = size_t(data->width) * size_t(data->height);
        const size_t stride = std::max<size_t>(1, texels / 65536);
        for (size_t i = 0; i < texels; i += stride)
            for (int c = 0; c < canvas->histChannels; ++c)
            {
                float v = std::clamp(data->rgba[i * 4 + size_t(first + c)], 0.0f, 1.0f);
                ++canvas->bins[size_t(c)][size_t(std::min(kBins - 1, int(v * kBins)))];
            }
    }

    void sync()
    {
        Controller& c = ctl();
        if (c.source.set != shownSet || !(c.source == shownSource) || source->count() == 0)
        {
            shownSet = c.source.set;
            shownSource = c.source;
            rebuildSources();
            // The brush paints the set shown; a node output shown still paints its set's channels.
            canvas->brush.setChannel(c.source.set, c.source.channel.empty() ? std::string("baseColor") : c.source.channel);
        }
        rebuildUvs();
        if (c.result.image != data || c.result.type != type)
        {
            data = c.result.image;
            type = c.result.type;
            redisplay();
            if (!c.result.stale)
                canvas->settled();
        }
        QString text = describe(c.source);
        if (data)
            text += QString("   %1 x %2 %3").arg(data->width).arg(data->height).arg(qs(type));
        if (!c.result.error.empty())
            text += "   " + qs(c.result.error);
        else if (c.result.stale || (!data && !c.source.set.empty()))
            text += "   rendering...";
        status->setText(text);
        const QString empty = c.source.set.empty()       ? QString("No texture set. Click New Texture Set in the Texture Set panel, or pick one in the Material Editor.")
                              : !c.result.error.empty() ? qs(c.result.error)
                                                        : QString("Rendering...");
        if (empty != canvas->emptyText)
        {
            canvas->emptyText = empty;
            canvas->update();
        }
        QSignalBlocker block(onMesh);
        onMesh->setChecked(c.onMesh);
    }

    std::vector<std::pair<QString, texture2d::Source>> mSources;
};

Texture2DView::Texture2DView(QWidget* parent) : QWidget(parent), d(std::make_unique<Impl>())
{
    installSceneListener();
    viewportHooks::setMaterialHook(kMeshHook, meshHook);
    setObjectName("texture2dView");
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    // Source and resolution on one row; view toggles below, wrapping in a narrow dock.
    auto* bar = new QWidget(this);
    auto* bv = new QVBoxLayout(bar);
    bv->setContentsMargins(6, 4, 6, 4);
    bv->setSpacing(4);
    auto* h = new QHBoxLayout;
    h->setSpacing(4);
    d->source = new QComboBox(bar);
    d->source->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    d->source->setMinimumContentsLength(12);
    d->source->setToolTip("Source: the texture set channel or node output shown");
    h->addWidget(d->source, 1);
    d->resolution = new QComboBox(bar);
    for (int s : {128, 256, 512, 1024, 2048})
        d->resolution->addItem(QString("%1 px").arg(s), s);
    d->resolution->setCurrentIndex(d->resolution->findData(ctl().size));
    d->resolution->setToolTip("Resolution: the size the output is rendered at");
    h->addWidget(d->resolution);
    bv->addLayout(h);

    auto* flow = new FlowLayout(6);
    flow->setContentsMargins(0, 0, 0, 0);
    auto* toggles = new QWidget(bar);
    toggles->setLayout(flow);
    d->solo = new controls::Segmented(toggles);
    const char* soloNames[] = {"RGB", "R", "G", "B", "A"};
    for (int i = 0; i < 5; ++i)
        d->solo->add(soloNames[i], {}, i == 0 ? QString("RGB: show every channel") : QString("%1: show only %1, as grey").arg(soloNames[i]), i);
    d->solo->setCurrent(0);
    d->solo->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    flow->addWidget(d->solo);
    auto toggle = [toggles, flow](const QString& text, const QString& tip) {
        auto* b = new QToolButton(toggles);
        b->setCheckable(true);
        b->setText(text);
        b->setToolTip(tip);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        controls::setRole(b, "push");
        flow->addWidget(b);
        return b;
    };
    d->tile = toggle("Tile", "Tile: 3 x 3, to check the texture repeats without seams");
    d->hist = toggle("Histogram", "Histogram: of the shown channels");
    d->hist->setChecked(true);
    d->onMesh = toggle("On Mesh", "On Mesh: show this output as base colour on the meshes whose material the texture set drives");
    d->uvs = toggle("UVs", "UVs: draw the UV edges of the meshes the texture set textures");
    d->uvs->setChecked(true);
    auto* fit = new QToolButton(toggles);
    fit->setText("Fit");
    fit->setToolTip("Fit: the whole image in the view (F, or double-click)");
    controls::setRole(fit, "push");
    flow->addWidget(fit);
    bv->addWidget(toggles);
    lay->addWidget(bar);

    d->canvas = new Canvas(this);
    ctl().canvases.push_back(d->canvas);
    d->canvas->brush.setPainting(true);
    lay->addWidget(d->canvas, 1);
    auto* foot = new QWidget(this);
    auto* fl = new QVBoxLayout(foot);
    fl->setContentsMargins(6, 2, 6, 4);
    fl->setSpacing(0);
    d->status = new QLabel(foot);
    d->status->setStyleSheet(themedStyleSheet("color: #aaaaaa;"));
    d->pixel = new QLabel(foot);
    d->pixel->setStyleSheet(themedStyleSheet("color: #969696;"));
    d->pixel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    fl->addWidget(d->status);
    fl->addWidget(d->pixel);
    lay->addWidget(foot);

    Impl* impl = d.get();
    connect(d->source, &QComboBox::activated, this, [impl](int i) {
        if (i >= 0 && size_t(i) < impl->mSources.size())
            texture2d::view(impl->mSources[size_t(i)].second);
    });
    connect(d->resolution, &QComboBox::activated, this, [this](int) { setResolution(d->resolution->currentData().toInt()); });
    connect(d->tile, &QToolButton::toggled, this, [this](bool on) { setTiled(on); });
    connect(d->solo, &controls::Segmented::picked, this, [this](int id) { setSolo(id); });
    connect(d->hist, &QToolButton::toggled, this, [impl](bool on) {
        impl->canvas->histogram = on;
        impl->canvas->update();
    });
    connect(d->onMesh, &QToolButton::toggled, this, [](bool on) {
        if (on)
        {
            ctl().onMesh = true;
            if (ctl().skipped)
                requestSoon();
            viewportTools::requestRedraw();
        }
        else
            texture2d::clearMeshPreview();
    });
    connect(d->uvs, &QToolButton::toggled, this, [impl](bool on) {
        impl->canvas->showUvs = on;
        impl->uvMeshes.clear();
        impl->canvas->uvLines.clear();
        ++impl->canvas->uvVersion;
        impl->rebuildUvs();
        impl->canvas->update();
    });
    connect(fit, &QToolButton::clicked, this, [impl] { impl->canvas->fit(); });
    d->canvas->onHover = [this](int x, int y, bool inside) { d->pixel->setText(inside ? inspect(x, y) : QString()); };

    Controller& c = ctl();
    d->listener = c.nextListener++;
    QPointer<Texture2DView> self(this);
    c.listeners[d->listener] = [self] {
        if (self)
            self->d->sync();
    };
    // The quick mask's tint follows its paint, which asks for a redraw as it changes.
    d->redrawListener = viewportTools::addListener([self] {
        if (self && self->isVisible() && (!quickMaskSet().empty() || !self->d->canvas->mTint.isNull()))
            self->d->canvas->update();
    });
    // Mesh edits and material assignments change the UVs drawn without touching the set; selection changes what fit frames.
    d->sceneListener = scene::Graph::current().addListener([self](scene::Change, scene::NodeId) {
        if (!self || self->d->uvsQueued || !self->isVisible())
            return;
        self->d->uvsQueued = true;
        QTimer::singleShot(0, self, [self] {
            self->d->uvsQueued = false;
            self->d->rebuildUvs();
        });
    });
    // The Texture Set dock's set when there is one, even none; else the set the graph or last view showed.
    TextureSetPanel* panel = TextureSetPanel::active();
    if (c.source.set.empty())
        if (std::string set = panel ? panel->textureSet() : currentTextureSet(); !set.empty())
            c.source = {set, {}, {}, "baseColor"};
    request();
}

Texture2DView::~Texture2DView()
{
    ctl().listeners.erase(d->listener);
    std::erase_if(ctl().canvases, [this](const QPointer<QWidget>& w) { return !w || w == d->canvas; });
    scene::Graph::current().removeListener(d->sceneListener);
    viewportTools::removeListener(d->redrawListener);
}

QImage Texture2DView::displayed() const { return d->canvas->image; }

QString Texture2DView::status() const { return d->status->text(); }

QString Texture2DView::inspect(int x, int y) const
{
    if (!d->data || x < 0 || y < 0 || x >= d->data->width || y >= d->data->height)
        return {};
    const float* px = &d->data->rgba[(size_t(y) * size_t(d->data->width) + size_t(x)) * 4];
    return QString("%1, %2   %3  %4  %5  %6")
        .arg(x)
        .arg(y)
        .arg(px[0], 0, 'f', 4)
        .arg(px[1], 0, 'f', 4)
        .arg(px[2], 0, 'f', 4)
        .arg(px[3], 0, 'f', 4);
}

void Texture2DView::setSolo(int channel)
{
    d->soloChannel = std::clamp(channel, 0, 4);
    d->solo->setCurrent(d->soloChannel);
    d->redisplay();
}

void Texture2DView::setTiled(bool on)
{
    if (d->tile->isChecked() != on)
        d->tile->setChecked(on);
    d->canvas->tiled = on;
    d->canvas->fit();
}

void Texture2DView::setResolution(int size)
{
    ctl().size = std::clamp(size, 16, 8192);
    if (int i = d->resolution->findData(ctl().size); i >= 0)
        d->resolution->setCurrentIndex(i);
    request();
}
} // namespace lila::ui
