// Before Qt (the header pulls it in): its slots macro breaks tex/Plan.h.
#include "render/PaintGpu.h"
#include "tex/PaintSymmetry.h"
#include "tex/TextureSet.h"

#include "ui/UvTexturePaint.h"

#include "brush/Stroke.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Node.h"
#include "scene/Transform.h"
#include "ui/CameraBookmarks.h"
#include "ui/TexLive.h"
#include "ui/TexturePaintTool.h"
#include "ui/TextureSetPanel.h"
#include "ui/ViewportWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <tuple>

namespace lila::ui
{
namespace
{
namespace tp = tex::paint;
namespace rp = render::paint;

/** The meshes Texture Paint reaches that use the set's material. */
std::vector<std::string> meshesOf(scene::Graph& g, const scene::Node& set)
{
    g.evaluate();
    std::vector<std::string> out;
    for (scene::NodeId id : texturePaintTargets())
    {
        scene::Node* n = g.node(id);
        scene::MeshPtr m = n ? n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh) : nullptr;
        if (!m || !m->numFaces())
            continue;
        const auto& names = m->materialNames();
        if (rp::usesSet(names, set))
            out.push_back(n->path());
    }
    return out;
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

/** Object units per uv on a painted mesh: at one triangle, and over the whole mesh for uvs off it. */
struct UvScale
{
    const tp::PaintMesh* mesh = nullptr;
    glm::dvec3 centre{0.0};
    double average = 1.0;

    void update(const tp::PaintMesh& m)
    {
        if (mesh == &m)
            return;
        mesh = &m;
        glm::dvec3 lo(1e300), hi(-1e300);
        double a3 = 0.0, auv = 0.0;
        for (uint32_t t = 0; t < m.triangleCount(); ++t)
        {
            if (!m.paintable[t])
                continue;
            a3 += area3(m, t);
            auv += areaUv(m, t);
            for (size_t k = 0; k < 3; ++k)
            {
                lo = glm::min(lo, glm::dvec3(m.positions[size_t(t) * 3 + k]));
                hi = glm::max(hi, glm::dvec3(m.positions[size_t(t) * 3 + k]));
            }
        }
        centre = lo.x <= hi.x ? (lo + hi) * 0.5 : glm::dvec3(0.0);
        average = auv > 1e-18 ? std::sqrt(a3 / auv) : 1.0;
    }

    double at(uint32_t tri) const { return std::sqrt(area3(*mesh, tri) / std::max(areaUv(*mesh, tri), 1e-18)); }

    double at(const glm::vec2& uv) const
    {
        uint32_t tri = 0;
        float u = 0.0f, v = 0.0f;
        return mesh->atUv(uv, tri, u, v) ? at(tri) : average;
    }
};

/**
 * The brush radius in the mesh's object space, the size the viewport paints: a screen radius goes through the active
 * 3D view's scale at the mesh. Nullopt without a 3D view.
 */
std::optional<double> objectRadius(const brush::BrushSettings& b, const scene::Node& meshNode, const glm::dvec3& centre)
{
    const glm::dmat4 world = meshNode.as<glm::dmat4>(scene::Transform::kWorldMatrix);
    const double scale = std::max(1e-12, glm::length(glm::dvec3(world[0])));
    if (b.radiusUnit == brush::RadiusUnit::World)
        return b.worldRadius / scale;
    ViewportWidget* view = cameraBookmarks::activeView();
    if (!view)
    {
        static bool told = false;
        if (!std::exchange(told, true))
            log::info("2D texture paint: no 3D view to size the brush from, so it keeps its screen size");
        return std::nullopt;
    }
    return b.radius * view->viewInfo().unitsPerPixel(glm::dvec3(world * glm::dvec4(centre, 1.0))) / scale;
}
} // namespace

struct UvTexturePaint::State
{
    /** Selection tool: the shape drawn so far, in uv, and the modifiers it started with. */
    std::vector<glm::dvec2> select;
    /** The set repeats: strokes wrap round the uv square's edges. */
    bool tileable = false;
    bool selecting = false;
    bool gradient = false;
    Qt::KeyboardModifiers selectMods;
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
    /** The stroke's radius in object space; nullopt paints at the screen radius. */
    std::optional<double> radius;

    UvScale scale;
    /** The meshes the set paints here, found again at each press; and the one under the cursor. */
    std::vector<std::string> paths;
    std::string hoverPath;
    /** What each mesh's part of a stroke starts from; the parts commit as one undo step. */
    tp::Stroke header;
    std::optional<UndoChunk> chunk;

    std::shared_ptr<const tp::PaintMesh> paintMesh(const std::string& path) const
    {
        scene::Graph& g = scene::Graph::current();
        scene::Node* s = tex::findSet(g, set);
        return s ? rp::paintMesh(g, path, rp::setMaterial(*s)) : nullptr;
    }

    /** The mesh with a triangle within tolerance (uv) of uv, prefer tried first; empty when none. */
    std::string meshAt(const glm::vec2& uv, float tolerance, const std::string& prefer = {})
    {
        if (paths.empty())
            if (scene::Node* s = tex::findSet(scene::Graph::current(), set))
                paths = meshesOf(scene::Graph::current(), *s);
        auto hits = [&](const std::string& p) {
            std::shared_ptr<const tp::PaintMesh> pm = paintMesh(p);
            uint32_t tri = 0;
            float u = 0.0f, v = 0.0f;
            return pm && pm->atUv(uv, tri, u, v, tolerance);
        };
        if (!prefer.empty() && hits(prefer))
            return prefer;
        for (const std::string& p : paths)
            if (p != prefer && hits(p))
                return p;
        return {};
    }

    /** The mesh node and paint mesh for the cursor: the one under uv, else the last hovered, else the first; nulls without one. */
    std::pair<scene::Node*, std::shared_ptr<const tp::PaintMesh>> target(const glm::vec2& uv)
    {
        if (std::string under = meshAt(uv, 0.0f, hoverPath); !under.empty())
            hoverPath = under;
        if (hoverPath.empty() && !paths.empty())
            hoverPath = paths.front();
        scene::Node* n = hoverPath.empty() ? nullptr : scene::Graph::current().find(hoverPath);
        std::shared_ptr<const tp::PaintMesh> pm = n ? paintMesh(hoverPath) : nullptr;
        if (!pm)
            return {};
        scale.update(*pm);
        return {n, pm};
    }

    /** Starts the stroke's part on the mesh at path: its own record, symmetry and size. False when it can't paint. */
    bool beginSegment(const std::string& path)
    {
        scene::Graph& g = scene::Graph::current();
        scene::Node* s = g.node(setId);
        scene::Node* node = g.find(path);
        std::shared_ptr<const tp::PaintMesh> pm = paintMesh(path);
        if (!s || !node || !pm)
            return false;
        auto h = std::make_shared<tp::Stroke>(header);
        uint64_t top = 0;
        if (const tp::Buffer* b = tp::readPaint(*s)->buffer(layer))
            for (const auto& st : b->strokes)
                top = std::max(top, st->id);
        h->id = top + 1;
        h->mesh = path;
        // Mirrored and radial copies land on the surface (through the mesh's point pairs), wherever their uvs are, as viewport strokes do.
        scene::MeshPtr outMesh = node->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        texturePaintSymmetryFor(*node, outMesh.get(), sym);
        h->symmetry = sym.toJson();
        try
        {
            rp::beginLive(g, *s, layer, h);
        }
        catch (const std::exception& ex)
        {
            log::warn("UV editor: {}", ex.what());
            return false;
        }
        mesh = pm;
        meshPath = path;
        scale.update(*pm);
        radius = objectRadius(brush, *node, scale.centre);
        record = *h;
        record.dabs.clear();
        return true;
    }

    /** Commits the current mesh's part of the stroke; with no dabs, only drops its live pages. */
    void commitSegment()
    {
        if (record.dabs.empty())
        {
            rp::cancelLive(setId, layer);
            return;
        }
        try
        {
            ops::execute("texPaintStroke", {{"args", {set, layer}}, {"stroke", record.toJson(false)}}, CallSource::Script);
        }
        catch (const std::exception& ex)
        {
            log::warn("UV editor: {}", ex.what());
            rp::cancelLive(setId, layer);
        }
        record.dabs.clear();
    }

    void paint(std::vector<tp::Dab>& dabs)
    {
        if (dabs.empty())
            return;
        try
        {
            rp::liveDabs(setId, layer, dabs);
        }
        catch (const std::exception& ex)
        {
            log::warn("UV editor: {}", ex.what());
        }
        record.dabs.insert(record.dabs.end(), dabs.begin(), dabs.end());
        dabs.clear();
    }
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
    m->hoverPath.clear();
    m->paths.clear();
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

void UvTexturePaint::drawCursor(QPainter& p, QPointF at, const ToUv& toUv, double pxPerUv) const
{
    if (!m->painting || !shown())
        return;
    const brush::BrushSettings b = brush::BrushSettings::fromJson(texturePaintSettings()["brush"]);
    double r = b.radius;
    if (auto [node, mesh] = m->target(glm::vec2(toUv(at))); node)
        if (std::optional<double> radius = objectRadius(b, *node, m->scale.centre))
            r = *radius / m->scale.at(glm::vec2(toUv(at))) * pxPerUv;
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
    if (s.value("tool", "") == "select" || s.value("tool", "") == "gradient")
    {
        m->gradient = s.value("tool", "") == "gradient";
        m->select = {toUv(e->position())};
        m->selecting = true;
        m->selectMods = e->modifiers();
        return true;
    }
    if (s.value("tool", "") == "pick")
    {
        const glm::dvec2 uv = toUv(e->position());
        pickTextureChannelsAt(m->set, uv.x, uv.y, e->modifiers() & Qt::ShiftModifier);
        return true;
    }
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
    m->paths = meshesOf(g, *set);
    if (m->paths.empty())
    {
        log::warn("UV editor: no mesh Texture Paint reaches uses {}'s material; select the meshes to paint, or none for all", m->set);
        return true;
    }
    const glm::vec2 at(toUv(e->position()));
    if (payload.kind == "project")
    {
        log::warn("UV editor: the projection buffer paints in the viewport");
        return true;
    }
    m->layer = textureSets::activeLayer(m->set);
    if (m->layer.empty())
        m->layer = rp::targetLayer(*set, "", true);
    m->setId = set->id();
    m->tileable = tex::readSettings(*set).tileable;
    if (payload.kind == "fill")
    {
        const std::string path = m->meshAt(at, 0.0f);
        std::shared_ptr<const tp::PaintMesh> pm = path.empty() ? nullptr : m->paintMesh(path);
        uint32_t tri = 0;
        float u = 0.0f, v = 0.0f;
        if (!pm || !pm->atUv(at, tri, u, v))
            return true;
        json pay = s["payload"];
        pay["triangle"] = tri;
        try
        {
            ops::execute("texPaintStroke", {{"args", {m->set, m->layer}}, {"stroke", {{"mesh", path}, {"payload", pay}}}}, CallSource::UI);
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
    m->header = tp::Stroke();
    m->header.brush = m->brush.toJson();
    m->header.payload = payload;
    // No view: painted in uv, both sides of a seam take the paint by 3D distance as in the viewport.
    m->header.occlusion = false;
    m->header.seed = m->brush.seed;
    std::string first = m->meshAt(at, 0.0f);
    if (first.empty())
        first = m->paths.front();
    m->chunk.emplace("Paint Stroke");
    if (!m->beginSegment(first))
    {
        m->chunk.reset();
        return true;
    }
    m->hoverPath = first;
    m->stroking = true;
    const double screenRadius = m->radius ? *m->radius / m->scale.at(at) * pxPerUv : m->brush.radius;
    m->sampler.begin(m->brush, {{e->position().x(), e->position().y()}, 1.0}, screenRadius);
    mouseMove(e, toUv, pxPerUv);
    return true;
}

bool UvTexturePaint::mouseMove(QMouseEvent* e, const ToUv& toUv, double pxPerUv)
{
    m->cursor = e->position();
    if (m->selecting)
    {
        m->select.push_back(toUv(e->position()));
        return true;
    }
    if (!m->stroking)
        return false;
    if (e->type() == QEvent::MouseMove)
        m->sampler.move({{e->position().x(), e->position().y()}, 1.0});
    std::vector<tp::Dab> dabs;
    for (const brush::StrokeSample& s : m->sampler.take())
    {
        const glm::vec2 uv(toUv(QPointF(s.position.x, s.position.y)));
        uint32_t tri = 0;
        float u = 0.0f, v = 0.0f;
        tp::Dab d;
        // As the viewport sizes dabs: a light touch still leaves 5%.
        const double radius = m->radius ? *m->radius * std::max(0.05, m->brush.pressureSize.map(s.pressure)) * s.size : 0.0;
        const double radiusUv = m->brush.radius * m->brush.pressureSize.map(s.pressure) * s.size / std::max(pxPerUv, 1e-9);
        const float reach = float(m->radius ? radius / m->scale.average : radiusUv);
        if (!m->mesh->atUv(uv, tri, u, v, reach))
        {
            // Off this mesh: another one in the same uv space may be under the dab.
            const std::string other = m->meshAt(uv, reach, m->meshPath);
            if (other.empty() || other == m->meshPath)
                continue;
            m->paint(dabs);
            m->commitSegment();
            if (!m->beginSegment(other) || !m->mesh->atUv(uv, tri, u, v, reach))
                continue;
        }
        if (m->radius)
            d.radius = float(std::max(radius, 1e-6));
        else
            d.radius = float(std::max(radiusUv * m->scale.at(tri), 1e-6));
        d.triangle = tri;
        d.u = u;
        d.v = v;
        d.pressure = float(s.pressure);
        d.opacity = float(std::clamp(m->brush.flow * m->brush.pressureOpacity.map(s.pressure) * s.opacity, 0.0, 1.0));
        for (const tp::Dab& c : m->sym.copies(*m->mesh, d))
        {
            dabs.push_back(c);
            if (m->tileable)
                for (const tp::Dab& w : tp::tileWrapped(*m->mesh, c))
                    dabs.push_back(w);
        }
    }
    m->paint(dabs);
    return true;
}

bool UvTexturePaint::mouseRelease(QMouseEvent* e, const ToUv& toUv, double pxPerUv)
{
    if (m->selecting && m->gradient)
    {
        // A gradient dragged in uv: from the press to the release.
        m->selecting = false;
        std::vector<glm::dvec2> pts = std::exchange(m->select, {});
        if (pts.size() < 2 || glm::length(pts.back() - pts.front()) < 1e-4)
            return true;
        const std::string layer = textureSets::activeLayer(m->set).empty() ? rp::targetLayer(*tex::findSet(scene::Graph::current(), m->set), "", true)
                                                                            : textureSets::activeLayer(m->set);
        const json pay = texturePaintGradient(pts.front().x, pts.front().y, pts.back().x, pts.back().y, m->selectMods & Qt::ControlModifier);
        try
        {
            ops::execute("texPaintStroke", {{"args", {m->set, layer}}, {"stroke", {{"payload", pay}}}}, CallSource::UI);
        }
        catch (const std::exception& ex)
        {
            log::warn("UV editor: {}", ex.what());
        }
        return true;
    }
    if (m->selecting)
    {
        // A drag is a lasso in uv; the layer selected in the Texture Set dock takes the mask.
        m->selecting = false;
        std::vector<glm::dvec2> pts = std::exchange(m->select, {});
        const std::string layer = textureSets::activeLayer(m->set);
        if (pts.size() < 3 || layer.empty())
            return true;
        json list = json::array();
        for (const glm::dvec2& q : pts)
            list.push_back({q.x, q.y});
        const char* mode = (m->selectMods & Qt::ShiftModifier) ? "add" : (m->selectMods & Qt::ControlModifier) ? "subtract" : "replace";
        try
        {
            ops::execute("texSelectionMask", {{"args", {m->set, layer}}, {"uvPoints", list}, {"mode", mode}}, CallSource::UI);
        }
        catch (const std::exception& ex)
        {
            log::warn("UV editor: {}", ex.what());
        }
        return true;
    }
    if (!m->stroking)
        return false;
    m->sampler.end();
    mouseMove(e, toUv, pxPerUv);
    m->stroking = false;
    m->commitSegment();
    m->chunk.reset();
    return true;
}
} // namespace lila::ui
