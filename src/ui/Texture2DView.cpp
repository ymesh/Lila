#include "ui/TextureGraphUi.h"

#include "core/Profile.h"
#include "geo/Mesh.h"
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
#include "ui/TexPreview.h"
#include "ui/Theme.h"
#include "ui/UiCommon.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"

#include <QApplication>
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

#include <algorithm>
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
    bool queued = false;
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

void notify()
{
    std::vector<std::function<void()>> fns;
    for (auto& [id, fn] : ctl().listeners)
        fns.push_back(fn);
    for (auto& fn : fns)
        fn();
}

const std::string& boundSet(const std::string& slot)
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
                requestSoon();
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

void request()
{
    LILA_ZONE();
    Controller& c = ctl();
    if (c.source.set.empty() || !tex::findSet(scene::Graph::current(), c.source.set))
    {
        c.result = {};
        if (c.source.set.empty() == false)
            c.result.error = "no texture set named '" + c.source.set + "'";
    }
    else
        c.result = texPreview::get({c.source.set, c.source.node, c.source.socket, c.source.channel, c.size}, [] { requestSoon(); });
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
        if (boundSet(scene::materials::slotName(mesh, int32_t(i))) == c.source.set)
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
    scene::Node* n = set.empty() ? nullptr : tex::findSet(scene::Graph::current(), set);
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

/** The image area: zoom with the wheel, pan with any drag, double-click or F to fit. */
class Canvas final : public QWidget
{
public:
    explicit Canvas(QWidget* parent) : QWidget(parent)
    {
        setMouseTracking(true);
        setFocusPolicy(Qt::ClickFocus);
        setMinimumSize(160, 160);
    }

    QImage image;
    bool tiled = false;
    bool histogram = true;
    /** Bins per channel (R, G, B, A); empty before an image. */
    std::array<std::vector<int>, 4> bins;
    int histChannels = 3;
    /** UV edges in the image's unit square, v down. */
    QList<QLineF> uvLines;
    bool showUvs = true;
    /** Shown in place of the image while there is none. */
    QString emptyText;
    std::function<void(int x, int y, bool inside)> onHover;

    void fit()
    {
        if (image.isNull())
            return;
        const double span = double(std::max(image.width(), image.height())) * (tiled ? 3.0 : 1.0);
        mScale = 0.92 * std::min(width(), height()) / span;
        mPan = {};
        update();
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

    QPointF toTexel(QPointF w) const
    {
        QPointF o = origin() - QPointF(image.width(), image.height()) * mScale * 0.5;
        return (w - o) / mScale;
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
        p.setRenderHint(QPainter::SmoothPixmapTransform, mScale < 1.0);
        const QSizeF sz = QSizeF(image.size()) * mScale;
        const QPointF topLeft = origin() - QPointF(sz.width(), sz.height()) * 0.5;
        const int reach = tiled ? 1 : 0;
        for (int ty = -reach; ty <= reach; ++ty)
            for (int tx = -reach; tx <= reach; ++tx)
                p.drawImage(QRectF(topLeft + QPointF(tx * sz.width(), ty * sz.height()), sz), image);
        if (tiled)
        {
            p.setPen(QPen(gray(200, 120), 1.0, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(topLeft, sz));
        }
        if (showUvs && !uvLines.isEmpty())
            drawUvs(p, topLeft, sz);
        if (histogram && !bins[0].empty())
            drawHistogram(p);
    }

    void drawUvs(QPainter& p, const QPointF& topLeft, const QSizeF& size)
    {
        p.save();
        p.translate(topLeft);
        p.scale(size.width(), size.height());
        p.setRenderHint(QPainter::Antialiasing, uvLines.size() < 20000);
        // Dark under light, so the edges read on any texture.
        QPen under(gray(0, 110), 3.0);
        under.setCosmetic(true);
        p.setPen(under);
        p.drawLines(uvLines);
        QPen pen(gray(240, 200));
        pen.setCosmetic(true);
        p.setPen(pen);
        p.drawLines(uvLines);
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
        const QPointF c = e->position() - QPointF(width() * 0.5, height() * 0.5);
        mPan = c - (c - mPan) * (next / mScale);
        mScale = next;
        update();
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        mLast = e->position();
        mDragging = true;
        setCursor(Qt::ClosedHandCursor);
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (mDragging && (e->buttons() & (Qt::LeftButton | Qt::MiddleButton | Qt::RightButton)))
        {
            mPan += e->position() - mLast;
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

    void mouseReleaseEvent(QMouseEvent*) override
    {
        mDragging = false;
        unsetCursor();
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
        if (!mFitted && !image.isNull())
        {
            mFitted = true;
            fit();
        }
    }

private:
    double mScale = 1.0;
    QPointF mPan;
    QPointF mLast;
    bool mDragging = false;
    bool mFitted = false;
};
} // namespace

std::string textureSetForSlot(scene::Graph& g, const std::string& slot)
{
    std::string named;
    for (const std::string& name : tex::setNames(g))
    {
        scene::Node* n = tex::findSet(g, name);
        if (!n)
            continue;
        try
        {
            if (tex::readSettings(*n).material == slot)
                return name;
        }
        catch (const std::exception&)
        {
        }
        if (name == slot)
            named = name;
    }
    return named;
}

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
    if (set.empty() || c.source.set == set)
        return;
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
    int sceneListener = 0;
    bool uvsQueued = false;

    /** The UV edges of the faces the shown set textures, on every mesh whose material it drives. */
    void rebuildUvs()
    {
        const std::string& set = ctl().source.set;
        scene::Graph& g = scene::Graph::current();
        std::vector<std::pair<scene::MeshPtr, std::vector<int32_t>>> bound;
        std::vector<const geo::Mesh*> meshes;
        if (canvas->showUvs && !set.empty())
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
                    if (boundSet(scene::materials::slotName(*mesh, int32_t(i))) == set)
                        slots.push_back(int32_t(i));
                if (slots.empty())
                    continue;
                meshes.push_back(mesh.get());
                bound.emplace_back(std::move(mesh), std::move(slots));
            }
        if (meshes == uvMeshes)
            return;
        uvMeshes = std::move(meshes);
        QList<QLineF> lines;
        for (const auto& [mesh, slots] : bound)
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
        }
        rebuildUvs();
        if (c.result.image != data || c.result.type != type)
        {
            data = c.result.image;
            type = c.result.type;
            redisplay();
        }
        QString text = describe(c.source);
        if (data)
            text += QString("   %1 x %2 %3").arg(data->width).arg(data->height).arg(qs(type));
        if (!c.result.error.empty())
            text += "   " + qs(c.result.error);
        else if (c.result.stale || (!data && !c.source.set.empty()))
            text += "   rendering...";
        status->setText(text);
        const QString empty = c.source.set.empty()       ? QString("No texture set. Click New Texture Set in the Texture Set panel.")
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
            viewportTools::requestRedraw();
        }
        else
            texture2d::clearMeshPreview();
    });
    connect(d->uvs, &QToolButton::toggled, this, [impl](bool on) {
        impl->canvas->showUvs = on;
        impl->uvMeshes.clear();
        impl->canvas->uvLines.clear();
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
    // Mesh edits and material assignments change the UVs drawn without touching the set.
    d->sceneListener = scene::Graph::current().addListener([self](scene::Change change, scene::NodeId) {
        if (!self || change == scene::Change::Selection || self->d->uvsQueued || !self->isVisible())
            return;
        self->d->uvsQueued = true;
        QTimer::singleShot(0, self, [self] {
            self->d->uvsQueued = false;
            self->d->rebuildUvs();
        });
    });
    if (c.source.set.empty())
        if (std::string set = currentTextureSet(); !set.empty())
            c.source = {set, {}, {}, "baseColor"};
    request();
}

Texture2DView::~Texture2DView()
{
    ctl().listeners.erase(d->listener);
    scene::Graph::current().removeListener(d->sceneListener);
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
