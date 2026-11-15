// Before Qt (the header pulls it in): its slots macro breaks tex/Plan.h.
#include "render/PaintGpu.h"
#include "tex/PaintSymmetry.h"
#include "tex/TextureSet.h"

#include "ui/UvTexturePaint.h"

#include "brush/Stroke.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Node.h"
#include "ui/TexLive.h"
#include "ui/TexturePaintTool.h"
#include "ui/TextureSetPanel.h"

#include <QMouseEvent>
#include <QPainter>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <map>

namespace lila::ui
{
namespace
{
namespace tp = tex::paint;
namespace rp = render::paint;

/** The mesh a set paints: the first using its material, else the first mesh. */
std::string meshOf(scene::Graph& g, const scene::Node& set)
{
    g.evaluate();
    const std::string material = rp::setMaterial(set);
    std::string any;
    for (scene::NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
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

double area3(const tp::PaintMesh& m, uint32_t t)
{
    const glm::vec3* p = &m.positions[size_t(t) * 3];
    return 0.5 * glm::length(glm::cross(glm::dvec3(p[1] - p[0]), glm::dvec3(p[2] - p[0])));
}

double areaUv(const tp::PaintMesh& m, uint32_t t)
{
    const glm::vec2* u = &m.uvs[size_t(t) * 3];
    const glm::dvec2 a(u[1] - u[0]), b(u[2] - u[0]);
    return 0.5 * std::abs(a.x * b.y - a.y * b.x);
}
} // namespace

struct UvTexturePaint::State
{
    std::string set, channel;
    bool painting = false;
    struct Tile
    {
        uint64_t key = 0, version = 0;
        glm::vec4 rect{0.0f};
        QSize size;
        QRectF screen;
        QImage image;
    };
    std::map<int, Tile> tiles;
    int listener = 0;
    QTimer poll;

    // A stroke in progress.
    bool stroking = false;
    scene::NodeId setId = scene::kNoNode;
    std::string layer, meshPath;
    std::shared_ptr<const tp::PaintMesh> mesh;
    brush::BrushSettings brush;
    brush::StrokeSampler sampler;
    tp::PaintSymmetry sym;
    tp::Stroke record;
    QPointF cursor;
};

UvTexturePaint::UvTexturePaint() : m(std::make_unique<State>())
{
    m->listener = rp::addChangeListener([this](scene::NodeId) {
        if (onUpdate && shown())
            onUpdate();
    });
    // The live texture set finishes evaluating after the paint lands: look again shortly.
    m->poll.setInterval(150);
    QObject::connect(&m->poll, &QTimer::timeout, [this] {
        if (onUpdate && shown())
            onUpdate();
    });
}

UvTexturePaint::~UvTexturePaint() { rp::removeChangeListener(m->listener); }

std::vector<std::pair<std::string, std::string>> UvTexturePaint::choices()
{
    std::vector<std::pair<std::string, std::string>> out;
    scene::Graph& g = scene::Graph::current();
    for (const std::string& name : tex::setNames(g))
        if (scene::Node* n = tex::findSet(g, name))
            try
            {
                for (const tex::Channel& c : tex::readSettings(*n).channels)
                    out.emplace_back(name, c.name);
            }
            catch (const std::exception&)
            {
            }
    return out;
}

void UvTexturePaint::setChannel(const std::string& set, const std::string& channel)
{
    m->set = set;
    m->channel = channel;
    m->tiles.clear();
    if (shown())
        m->poll.start();
    else
        m->poll.stop();
    if (onUpdate)
        onUpdate();
}

bool UvTexturePaint::shown() const { return !m->set.empty() && !m->channel.empty(); }

void UvTexturePaint::setPainting(bool on)
{
    // Builds the stroke pipelines now, so the first stroke doesn't wait on them.
    if (on && !m->painting)
        rp::prewarm();
    m->painting = on;
}

bool UvTexturePaint::painting() const { return m->painting; }

void UvTexturePaint::drawBackground(QPainter& p, const QRect& widget, const ToScreen& toScreen, const ToUv& toUv, bool udim)
{
    if (!shown())
        return;
    LILA_ZONE();
    scene::Graph& g = scene::Graph::current();
    scene::Node* set = tex::findSet(g, m->set);
    if (!set)
        return;
    const bool color = m->channel == "baseColor" || m->channel == "emissive";
    const int tilesU = udim ? 10 : 1, tilesV = udim ? 2 : 1;
    for (int tv = 0; tv < tilesV; ++tv)
        for (int tu = 0; tu < tilesU; ++tu)
        {
            const int udimId = 1001 + tu + 10 * tv;
            render::GpuImageRef img = texLive::channelImage(m->set, m->channel, udimId);
            if (!img)
            {
                std::string layer = textureSets::activeLayer(m->set);
                if (layer.empty())
                    layer = rp::targetLayer(*set, "", false);
                if (!layer.empty())
                    img = rp::channelImage(set->id(), layer, m->channel, udimId);
            }
            if (!img)
                continue;
            const QRectF tile = QRectF(toScreen({double(tu), double(tv + 1)}), toScreen({double(tu + 1), double(tv)})).normalized();
            const QRectF vis = tile.intersected(QRectF(widget));
            if (vis.width() < 1.0 || vis.height() < 1.0)
                continue;
            const glm::dvec2 lo = toUv(vis.bottomLeft()), hi = toUv(vis.topRight());
            const glm::vec4 rect(float(lo.x - tu), float(lo.y - tv), float(hi.x - tu), float(hi.y - tv));
            // At most one texel per screen pixel, and no more than 2048 across.
            const double k = std::min({1.0, 2048.0 / vis.width(), 2048.0 / vis.height()});
            const QSize size(std::max(1, int(vis.width() * k)), std::max(1, int(vis.height() * k)));
            State::Tile& c = m->tiles[udimId];
            if (c.key != img->key() || c.version != img->version() || c.rect != rect || c.size != size)
            {
                try
                {
                    const std::vector<uint8_t> px = rp::viewImage(img, rect, size.width(), size.height(), color);
                    c.image = QImage(px.data(), size.width(), size.height(), size.width() * 4, QImage::Format_RGBA8888).copy();
                }
                catch (const std::exception& e)
                {
                    log::warn("UV editor: texture view: {}", e.what());
                    c.image = {};
                }
                c.key = img->key();
                c.version = img->version();
                c.rect = rect;
                c.size = size;
            }
            if (!c.image.isNull())
                p.drawImage(vis, c.image);
        }
}

void UvTexturePaint::drawCursor(QPainter& p, QPointF at) const
{
    if (!m->painting || !shown())
        return;
    const json s = texturePaintSettings();
    const double r = s["brush"].value("radius", 40.0);
    p.setPen(QPen(QColor(230, 230, 230), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(at, r, r);
}

bool UvTexturePaint::mousePress(QMouseEvent* e, const ToUv& toUv, double pxPerUv)
{
    if (!m->painting || !shown() || e->button() != Qt::LeftButton)
        return false;
    scene::Graph& g = scene::Graph::current();
    scene::Node* set = tex::findSet(g, m->set);
    if (!set)
        return true;
    const json s = texturePaintSettings();
    tp::Payload payload;
    try
    {
        payload = tp::Payload::fromJson(s["payload"]);
    }
    catch (const std::exception& ex)
    {
        log::warn("UV editor: {}", ex.what());
        return true;
    }
    m->meshPath = meshOf(g, *set);
    m->mesh = m->meshPath.empty() ? nullptr : rp::paintMesh(g, m->meshPath, rp::setMaterial(*set));
    if (!m->mesh)
    {
        log::warn("UV editor: no mesh uses {}'s material", m->set);
        return true;
    }
    if (payload.kind == "project")
    {
        log::warn("UV editor: the projection buffer paints in the viewport");
        return true;
    }
    m->layer = textureSets::activeLayer(m->set);
    if (m->layer.empty())
        m->layer = rp::targetLayer(*set, "", true);
    m->setId = set->id();
    if (payload.kind == "fill")
    {
        uint32_t tri = 0;
        float u = 0.0f, v = 0.0f;
        if (!m->mesh->atUv(glm::vec2(toUv(e->position())), tri, u, v))
            return true;
        json pay = s["payload"];
        pay["triangle"] = tri;
        try
        {
            ops::execute("texPaintStroke", {{"args", {m->set, m->layer}}, {"stroke", {{"mesh", m->meshPath}, {"payload", pay}}}}, CallSource::UI);
        }
        catch (const std::exception& ex)
        {
            log::warn("UV editor: {}", ex.what());
        }
        return true;
    }
    if (payload.filters() && payload.channels.empty())
        for (const tex::Channel& c : tex::readSettings(*set).channels)
            payload.channels[c.name] = glm::vec3(0.0f);
    m->brush = brush::BrushSettings::fromJson(s["brush"]);
    auto header = std::make_shared<tp::Stroke>();
    uint64_t top = 0;
    if (const tp::Buffer* b = tp::readPaint(*set)->buffer(m->layer))
        for (const auto& st : b->strokes)
            top = std::max(top, st->id);
    header->id = top + 1;
    header->mesh = m->meshPath;
    header->brush = m->brush.toJson();
    header->payload = payload;
    // No view: painted in uv, both sides of a seam take the paint by 3D distance as in the viewport.
    header->occlusion = false;
    {
        // Mirrored and radial copies land on the surface (through the mesh's point pairs), wherever their uvs are.
        const scene::Node* meshNode = g.find(m->meshPath);
        scene::MeshPtr outMesh = meshNode->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        m->sym = tp::brushSymmetry(m->brush, outMesh.get(), scene::paintSymmetrySpec(*meshNode));
        header->symmetry = m->sym.toJson();
    }
    header->seed = m->brush.seed;
    m->record = *header;
    m->record.dabs.clear();
    try
    {
        rp::beginLive(g, *set, m->layer, header);
    }
    catch (const std::exception& ex)
    {
        log::warn("UV editor: {}", ex.what());
        return true;
    }
    m->stroking = true;
    m->sampler.begin(m->brush, {{e->position().x(), e->position().y()}, 1.0}, m->brush.radius);
    mouseMove(e, toUv, pxPerUv);
    return true;
}

bool UvTexturePaint::mouseMove(QMouseEvent* e, const ToUv& toUv, double pxPerUv)
{
    m->cursor = e->position();
    if (!m->stroking)
        return false;
    if (e->type() == QEvent::MouseMove)
        m->sampler.move({{e->position().x(), e->position().y()}, 1.0});
    std::vector<tp::Dab> dabs;
    for (const brush::StrokeSample& s : m->sampler.take())
    {
        const glm::vec2 uv(toUv(QPointF(s.position.x, s.position.y)));
        const double radiusUv = m->brush.radius * m->brush.pressureSize.map(s.pressure) * s.size / std::max(pxPerUv, 1e-9);
        uint32_t tri = 0;
        float u = 0.0f, v = 0.0f;
        if (!m->mesh->atUv(uv, tri, u, v, float(radiusUv)))
            continue;
        const double ratio = std::sqrt(area3(*m->mesh, tri) / std::max(areaUv(*m->mesh, tri), 1e-18));
        tp::Dab d;
        d.triangle = tri;
        d.u = u;
        d.v = v;
        d.radius = float(std::max(radiusUv * ratio, 1e-6));
        d.pressure = float(s.pressure);
        d.opacity = float(std::clamp(m->brush.flow * m->brush.pressureOpacity.map(s.pressure) * s.opacity, 0.0, 1.0));
        for (const tp::Dab& c : m->sym.copies(*m->mesh, d))
            dabs.push_back(c);
    }
    if (!dabs.empty())
    {
        try
        {
            rp::liveDabs(m->setId, m->layer, dabs);
        }
        catch (const std::exception& ex)
        {
            log::warn("UV editor: {}", ex.what());
        }
        m->record.dabs.insert(m->record.dabs.end(), dabs.begin(), dabs.end());
    }
    return true;
}

bool UvTexturePaint::mouseRelease(QMouseEvent* e, const ToUv& toUv, double pxPerUv)
{
    if (!m->stroking)
        return false;
    m->sampler.end();
    mouseMove(e, toUv, pxPerUv);
    m->stroking = false;
    if (m->record.dabs.empty())
    {
        rp::cancelLive(m->setId, m->layer);
        return true;
    }
    try
    {
        ops::execute("texPaintStroke", {{"args", {m->set, m->layer}}, {"stroke", m->record.toJson(false)}}, CallSource::Script);
    }
    catch (const std::exception& ex)
    {
        log::warn("UV editor: {}", ex.what());
        rp::cancelLive(m->setId, m->layer);
    }
    return true;
}
} // namespace lila::ui
