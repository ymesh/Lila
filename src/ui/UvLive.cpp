#include "ui/UvLive.h"

// Before Qt: Qt's emit macro breaks TBB's headers.
#include "uv/LiveSession.h"
#include <tbb/parallel_for.h>

#include "ops/Command.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Bvh.h"
#include "io/SceneFile.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "sculpt/SculptSession.h"
#include "ui/Actions.h"
#include "ui/Isolate.h"
#include "ui/MeshCache.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"
#include "uv/Uv.h"
#include "uv/UvCommands.h"

#include <QApplication>
#include <QKeyEvent>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <cmath>
#include <map>
#include <set>

namespace lila::ui::uvLive
{
namespace
{
constexpr const char* kPrefs = "general";
constexpr double kPi = 3.14159265358979323846;
using scene::ComponentMode;
using scene::MeshPtr;
using scene::NodeId;

struct Settings
{
    BrushSettings brush;
    bool live = false;
    double balance = 0.0;
    bool checker = false;
    int checkerTiling = 32;
    bool islands = false;
    bool seams = true;
    bool twoColour = false;
};

Settings& settings()
{
    static Settings s;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        const PrefStore& p = PrefStore::get();
        uv::liveBrushFromName(p.value<std::string>(kPrefs, "/uv/live/brush", "relax"), s.brush.brush);
        s.brush.radius = std::clamp(p.value<double>(kPrefs, "/uv/live/radius", s.brush.radius), 2.0, 2000.0);
        s.brush.strength = std::clamp(p.value<double>(kPrefs, "/uv/live/strength", s.brush.strength), 0.0, 1.0);
        s.live = p.value<bool>(kPrefs, "/uv/live/mode", s.live);
        s.balance = std::clamp(p.value<double>(kPrefs, "/uv/live/balance", s.balance), -1.0, 1.0);
        s.checker = p.value<bool>(kPrefs, "/uv/checker3d", s.checker);
        s.checkerTiling = std::clamp(p.value<int>(kPrefs, "/uv/checkerTiling", s.checkerTiling), 1, 256);
        s.seams = p.value<bool>(kPrefs, "/uv/seams3d", s.seams);
        s.twoColour = p.value<bool>(kPrefs, "/uv/heatmapTwoColour", s.twoColour);
        uv::live::options().balance = s.balance;
    }
    return s;
}

std::map<int, std::function<void()>>& listeners()
{
    static std::map<int, std::function<void()>> l;
    return l;
}

void notify()
{
    auto copy = listeners();
    for (auto& [id, fn] : copy)
        fn();
}

void save()
{
    const Settings& s = settings();
    PrefStore& p = PrefStore::get();
    p.set(kPrefs, "/uv/live/brush", std::string(uv::liveBrushName(s.brush.brush)));
    p.set(kPrefs, "/uv/live/radius", s.brush.radius);
    p.set(kPrefs, "/uv/live/strength", s.brush.strength);
    p.set(kPrefs, "/uv/live/mode", s.live);
    p.set(kPrefs, "/uv/live/balance", s.balance);
    p.set(kPrefs, "/uv/checker3d", s.checker);
    p.set(kPrefs, "/uv/checkerTiling", s.checkerTiling);
    p.set(kPrefs, "/uv/seams3d", s.seams);
    p.set(kPrefs, "/uv/heatmapTwoColour", s.twoColour);
}

/** The mesh the uv editor shows: the base under any modifiers. */
MeshPtr cageOf(NodeId id)
{
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.node(id);
    if (!n || !n->type().isA("mesh"))
        return nullptr;
    return scene::modifiers::cage(g, id);
}

/** The islands' options reset to the shared ones, so a Settle's SLIM pass does not stick to later drags. */
void resetOptions(uv::LiveIsland& is)
{
    is.setOptions(uv::live::options());
}

// ---------------------------------------------------------------------------------------------------------------------
// 3D display: checker on the shown meshes, seams drawn over them.

std::set<const void*>& shownEditors()
{
    static std::set<const void*> s;
    return s;
}

const render::Material& checkerMaterial(int checks)
{
    static int built = 0;
    static render::Material m;
    if (built != checks)
    {
        built = checks;
        auto img = std::make_shared<render::TextureImage>();
        const int size = std::clamp(checks * 32, 256, 2048) / checks * checks;
        img->width = img->height = size;
        img->rgba.resize(size_t(size) * size * 4);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                int cx = x * checks / size, cy = y * checks / size;
                bool line = x % (size / checks) == 0 || y % (size / checks) == 0;
                uint8_t v = line ? 40 : ((cx + cy) % 2 ? 200 : 96);
                uint8_t* px = &img->rgba[(size_t(y) * size + x) * 4];
                px[0] = px[1] = px[2] = v;
                px[3] = 255;
            }
        render::Material mat;
        mat.baseColor = glm::vec3(1.0f);
        mat.roughness = 0.8f;
        mat.baseColorMap = std::move(img);
        mat.flags = render::MatBaseColorMap | render::MatTwoSided;
        mat.keepBaseColor = true;
        m = std::move(mat);
    }
    return m;
}

/** Object-space seam segments of a mesh, two points per edge in edges. */
struct Seams
{
    std::vector<glm::vec3> lines;
    std::vector<geo::Index> edges;
    /** The same lines kept on the GPU, drawn as a wire. */
    std::shared_ptr<const render::RenderMesh> wire;
};

/** Edges marked as seams or where the uv set splits. Cached per mesh. */
const Seams& seamLines(const MeshPtr& mesh, const std::string& set)
{
    struct Entry
    {
        std::weak_ptr<const geo::Mesh> mesh;
        std::string set;
        Seams seams;
    };
    // Never destroyed: at exit the meshes' owners (extension DLLs among them) may already be unloaded.
    static auto& cache = *new std::vector<Entry>();
    for (Entry& e : cache)
        if (e.mesh.lock() == mesh && e.set == set)
            return e.seams;
    std::erase_if(cache, [](const Entry& e) { return e.mesh.expired(); });
    if (cache.size() > 8)
        cache.erase(cache.begin());
    Entry e{mesh, set, {}};
    const geo::Mesh& m = *mesh;
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(set);
    std::span<const bool> seam = m.edgeAttrs().get<bool>(geo::attr::kSeam);
    auto same = [](glm::vec2 a, glm::vec2 b) { return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec2(1e-6f))); };
    std::vector<uint8_t> split(m.edgeCapacity(), 0);
    tbb::parallel_for(tbb::blocked_range<geo::Index>(0, m.edgeCapacity(), 16384), [&](const tbb::blocked_range<geo::Index>& range) {
        for (geo::Index ed = range.begin(); ed != range.end(); ++ed)
        {
            if (!m.edgeValid(ed) || m.isBoundaryEdge(ed))
                continue;
            bool s = !seam.empty() && seam[ed];
            if (!s && !uv.empty() && m.isManifoldEdge(ed))
            {
                geo::Index c = m.edgeCorner(ed), r = m.cornerRadial(c);
                bool sameDir = m.cornerPoint(r) == m.cornerPoint(c);
                glm::vec2 ra = sameDir ? uv[r] : uv[m.cornerNext(r)], rb = sameDir ? uv[m.cornerNext(r)] : uv[r];
                s = !same(ra, uv[c]) || !same(rb, uv[m.cornerNext(c)]);
            }
            split[ed] = s;
        }
    });
    for (geo::Index ed = 0; ed < m.edgeCapacity(); ++ed)
        if (split[ed])
        {
            e.seams.lines.push_back(glm::vec3(m.position(m.edgePoint(ed, 0))));
            e.seams.lines.push_back(glm::vec3(m.position(m.edgePoint(ed, 1))));
            e.seams.edges.push_back(ed);
        }
    auto wire = std::make_shared<render::RenderMesh>();
    wire->edgePoints = e.seams.lines;
    e.seams.wire = std::move(wire);
    cache.push_back(std::move(e));
    return cache.back().seams;
}

void draw3d(render::FrameState& s, OverlayPainter&, const ViewInfo& view)
{
    LILA_ZONE();

    const Settings& st = settings();
    // Both only show while a UV editor does.
    const bool checker = st.checker && !shownEditors().empty();
    const bool seams = st.seams && !shownEditors().empty();
    if (!checker && !seams)
        return;
    scene::Graph& g = scene::Graph::current();
    const std::string set = uv::currentUvSet();
    const auto sculpting = sculpt::active();
    for (NodeId id : shownMeshes())
    {
        scene::Node* n = g.node(id);
        MeshPtr out = n ? n->as<MeshPtr>(scene::MeshNode::kOutMesh) : nullptr;
        // A sculpt proxy stands in for the mesh it sculpts, and the mesh's seams would not follow the strokes.
        if (!out || !scene::effectivelyVisible(g, id) || !isolate::shows(view.isolation, id) || (sculpting && sculpting->node == id))
            continue;
        if (checker)
        {
            int smooth = int(n->as<int64_t>(scene::MeshNode::kSmoothPreview));
            auto rm = renderMeshFor(smoothedFor(out, smooth), false);
            for (render::MeshDraw& d : s.meshes.edit())
                if (d.mesh && (d.mesh == rm || d.mesh->shareVertices == rm))
                {
                    uint32_t slotCount = 1;
                    for (const render::MeshSection& sec : rm->sections)
                        slotCount = std::max(slotCount, sec.material + 1);
                    d.materials.assign(slotCount, checkerMaterial(st.checkerTiling));
                }
        }
        if (seams)
        {
            glm::mat4 w = glm::mat4(n->as<glm::dmat4>(scene::Transform::kWorldMatrix));
            const glm::vec4 color(1.0f, 1.0f, 1.0f, 1.0f);
            s.seamLineWidth = viewportDisplay::hairlineWidth(viewportDisplay::componentThickness().uvSeam, qApp->devicePixelRatio());
            const Seams& lines = seamLines(out, set);
            MeshPtr cage = scene::modifiers::cage(g, id);
            // Visibility indexes the cage, so it only applies while the output keeps the cage's edges.
            const auto vis = isolate::visibility(view.isolation, id, cage);
            const bool clip = !vis->all() && cage && cage->edgeCapacity() == out->edgeCapacity();
            if (!clip)
            {
                render::MeshDraw d;
                d.mesh = lines.wire;
                d.world = w;
                d.flags = render::DrawWire | render::DrawWireDepth;
                d.wireColor = color;
                d.wireWidth = s.seamLineWidth;
                s.meshes.push_back(std::move(d));
                continue;
            }
            for (size_t i = 0; i < lines.edges.size(); ++i)
                if (vis->edge(lines.edges[i]))
                {
                    s.seamLines.push_back({glm::dvec3(w * glm::vec4(lines.lines[2 * i], 1.0f)), color});
                    s.seamLines.push_back({glm::dvec3(w * glm::vec4(lines.lines[2 * i + 1], 1.0f)), color});
                }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 3D brush.

bool isViewport(QWidget* w)
{
    return w && w->inherits("lila::ui::ViewportWidget");
}

class BrushTool3d;

/** Sees F, B and M before the hotkeys do while a viewport has focus, so they can be held to resize the brush. */
class KeyFilter : public QObject
{
public:
    explicit KeyFilter(BrushTool3d* tool) : mTool(tool) {}
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    BrushTool3d* mTool;
};

class BrushTool3d final : public ViewportTool
{
public:
    enum class Adjust
    {
        None,
        Radius,
        Strength
    };

    const char* name() const override { return "uvBrush"; }

    void activate() override
    {
        mFilter = std::make_unique<KeyFilter>(this);
        qApp->installEventFilter(mFilter.get());
        mListener = addListener([] { viewportTools::requestRedraw(); });
        ensureDriver();
    }

    void deactivate() override
    {
        endStroke();
        if (mFilter)
            qApp->removeEventFilter(mFilter.get());
        mFilter.reset();
        removeListener(mListener);
        viewportTools::requestRedraw();
    }

    /** Held-key state from the key filter. Returns true when the key is ours. */
    bool key(int k, Qt::KeyboardModifiers mods, bool down)
    {
        Adjust kind = Adjust::None;
        if (k == Qt::Key_B || (k == Qt::Key_F && !(mods & Qt::ShiftModifier)))
            kind = Adjust::Radius;
        else if (k == Qt::Key_M || (k == Qt::Key_F && (mods & Qt::ShiftModifier)))
            kind = Adjust::Strength;
        if (kind == Adjust::None)
            return false;
        if (down)
        {
            mArmed = kind;
            mArmedKey = k;
            mArmedUsed = false;
            return true;
        }
        if (k != mArmedKey)
            return true;
        // A tapped F still frames the selection, as everywhere else.
        if (k == Qt::Key_F && !mArmedUsed && mAdjusting == Adjust::None)
            QTimer::singleShot(0, [] { actions::run("viewport.frameSelected"); });
        mArmed = Adjust::None;
        mArmedKey = 0;
        return true;
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        mHover = e->position();
        if (mArmed != Adjust::None)
        {
            mAdjusting = mArmed;
            mArmedUsed = true;
            mAdjustStart = e->position();
            mAdjustFrom = mAdjusting == Adjust::Radius ? brush().radius : brush().strength;
            return true;
        }
        Hit h;
        if (!hit(e->position(), view, h))
            return false;
        uv::live::Session* s = uv::live::get(mNode);
        if (!s)
            return true;
        mBrush = (e->modifiers() & Qt::ShiftModifier) ? uv::LiveBrush::Relax : brush().brush;
        if (mBrush == uv::LiveBrush::Grab)
            mBrush = uv::LiveBrush::Relax; // grabbing has no meaning on the surface
        mInvert = (e->modifiers() & Qt::ControlModifier) != 0;
        s->begin("UV " + std::string(uv::liveBrushName(mBrush)) + " Brush");
        s->setStroking(true);
        mStroking = true;
        mStrokeFrom = h.local;
        mLast = e->position();
        dab(h, view, true);
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (mAdjusting != Adjust::None)
        {
            double dx = e->position().x() - mAdjustStart.x();
            if (mAdjusting == Adjust::Radius)
                brush().radius = std::clamp(mAdjustFrom + dx, 2.0, 2000.0);
            else
                brush().strength = std::clamp(mAdjustFrom + dx / 300.0, 0.0, 1.0);
            settingsChanged();
            return true;
        }
        if (mStroking)
        {
            QPointF d = e->position() - mLast;
            double len = std::hypot(d.x(), d.y()), step = std::max(2.0, brush().radius * 0.15);
            int count = int(len / step);
            for (int k = 1; k <= count; ++k)
            {
                Hit h;
                if (hit(mLast + d * (k * step / len), view, h))
                    dab(h, view, false);
            }
            if (count > 0)
                mLast += d * (count * step / len);
            return true;
        }
        viewportTools::requestRedraw();
        return false;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        if (mAdjusting != Adjust::None)
        {
            mAdjusting = Adjust::None;
            return true;
        }
        endStroke();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        switch (e->key())
        {
        case Qt::Key_BracketLeft:
        case Qt::Key_BracketRight:
            brush().radius = std::clamp(brush().radius * (e->key() == Qt::Key_BracketLeft ? 0.8 : 1.25), 2.0, 2000.0);
            settingsChanged();
            return true;
        case Qt::Key_Escape: QTimer::singleShot(0, [] { viewportTools::set(nullptr); }); return true;
        default: return false;
        }
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        const BrushSettings& b = brush();
        retarget();
        scene::Node* n = scene::Graph::current().node(mNode);
        if (!n)
        {
            drawToolStatus(p, view, {.tool = "UV Brush", .prompt = "select a mesh"});
            return;
        }
        std::string_view name = uv::liveBrushName(b.brush);
        drawToolStatus(p, view,
                       {.tool = "UV Brush",
                        .subject = QString::fromStdString(n->name()),
                        .mode = QString::fromUtf8(name.data(), qsizetype(name.size())),
                        .fields = {{"radius", QString("%1 px").arg(int(std::lround(b.radius)))}, {"strength", QString::number(b.strength, 'f', 2)}},
                        .flags = {{"live", liveMode()}},
                        .keys = {{"B-drag", "radius"}, {"M-drag", "strength"}, {"Esc", "exit"}}});
        QPointF at = mAdjusting != Adjust::None ? mAdjustStart : mHover;
        const glm::vec4 rim(0.95f, 0.95f, 0.95f, 1.0f), dim(0.6f, 0.6f, 0.6f, 0.8f);
        Hit h;
        if (hit(at, view, h))
        {
            double r = b.radius * view.unitsPerPixel(h.world);
            glm::dvec3 nrm = h.worldNormal;
            glm::dvec3 ref = std::abs(nrm.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
            glm::dvec3 u = glm::normalize(glm::cross(nrm, ref)), v = glm::cross(nrm, u);
            for (double frac : {1.0, mAdjusting == Adjust::Strength ? b.strength : 0.0})
            {
                if (frac <= 0.0)
                    continue;
                std::vector<QPointF> pts;
                for (int k = 0; k < 48; ++k)
                {
                    double a = 2.0 * kPi * k / 48;
                    if (auto q = view.project(h.world + (u * std::cos(a) + v * std::sin(a)) * r * frac))
                        pts.push_back(*q);
                }
                p.polyline(pts, frac == 1.0 ? 1.5f : 1.0f, frac == 1.0 ? rim : dim, true);
            }
        }
        else
        {
            std::vector<QPointF> pts;
            for (int k = 0; k < 48; ++k)
            {
                double a = 2.0 * kPi * k / 48;
                pts.push_back(at + QPointF(std::cos(a), std::sin(a)) * b.radius);
            }
            p.polyline(pts, 1.0f, dim, true);
        }
    }

private:
    struct Hit
    {
        glm::dvec3 local{0.0}, world{0.0}, worldNormal{0.0, 1.0, 0.0};
    };

    void retarget()
    {
        if (mStroking)
            return;
        std::vector<NodeId> shown = shownMeshes();
        NodeId want = shown.empty() ? scene::kNoNode : shown.front();
        if (want != mNode)
        {
            mNode = want;
            mBvhMesh.reset();
        }
    }

    bool hit(QPointF px, const ViewInfo& view, Hit& out)
    {
        retarget();
        MeshPtr m = cageOf(mNode);
        if (!m)
            return false;
        if (m != mBvhMesh && !mStroking)
        {
            mBvhMesh = m;
            mBvh.build(*m);
        }
        if (mBvh.empty())
            return false;
        glm::dmat4 w = scene::Graph::current().node(mNode)->as<glm::dmat4>(scene::Transform::kWorldMatrix), inv = glm::inverse(w);
        glm::dvec3 o, d;
        view.ray(px, o, d);
        glm::dvec3 lo(inv * glm::dvec4(o, 1.0)), ld(inv * glm::dvec4(d, 0.0));
        geo::BvhHit h;
        if (!mBvh.raycast(lo, ld, h))
            return false;
        out.local = h.point;
        out.world = glm::dvec3(w * glm::dvec4(h.point, 1.0));
        const geo::Mesh& mesh = *mBvhMesh;
        const auto& tris = mBvh.triangles();
        glm::dvec3 a = mesh.position(mesh.cornerPoint(tris.corners[size_t(h.triangle) * 3]));
        glm::dvec3 b = mesh.position(mesh.cornerPoint(tris.corners[size_t(h.triangle) * 3 + 1]));
        glm::dvec3 c = mesh.position(mesh.cornerPoint(tris.corners[size_t(h.triangle) * 3 + 2]));
        glm::dvec3 nrm = glm::transpose(glm::inverse(glm::dmat3(w))) * glm::cross(b - a, c - a);
        if (glm::length(nrm) > 0.0)
            out.worldNormal = glm::normalize(nrm);
        return true;
    }

    void dab(const Hit& h, const ViewInfo& view, bool first)
    {
        uv::live::Session* s = uv::live::get(mNode, false);
        if (!s)
            return;
        glm::dmat4 w = scene::Graph::current().node(mNode)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        double scale = std::max(1e-12, std::cbrt(std::abs(glm::determinant(glm::dmat3(w)))));
        uv::LiveDab d;
        d.brush = mBrush;
        d.surface = true;
        d.center3 = h.local;
        d.lineFrom3 = mStrokeFrom;
        d.radius = brush().radius * view.unitsPerPixel(h.world) / scale;
        d.strength = brush().strength;
        d.invert = mInvert;
        d.first = first;
        bool wake = liveMode();
        s->solver().edit([d, wake](std::vector<uv::LiveIsland>& is) {
            for (uv::LiveIsland& i : is)
                if (i.applyDab(d) && wake)
                    i.wake();
        });
        viewportTools::requestRedraw();
    }

    void endStroke()
    {
        if (!mStroking)
            return;
        mStroking = false;
        if (uv::live::Session* s = uv::live::get(mNode, false))
            s->setStroking(false);
    }

    std::unique_ptr<KeyFilter> mFilter;
    int mListener = 0;
    Adjust mArmed = Adjust::None;
    int mArmedKey = 0;
    bool mArmedUsed = false;
    Adjust mAdjusting = Adjust::None;
    QPointF mAdjustStart;
    double mAdjustFrom = 0.0;

    NodeId mNode = scene::kNoNode;
    MeshPtr mBvhMesh;
    geo::MeshBvh mBvh;
    bool mStroking = false;
    uv::LiveBrush mBrush = uv::LiveBrush::Relax;
    bool mInvert = false;
    glm::dvec3 mStrokeFrom{0.0};
    QPointF mLast, mHover;
};

bool KeyFilter::eventFilter(QObject* obj, QEvent* e)
{
    if (e->type() != QEvent::KeyPress && e->type() != QEvent::KeyRelease && e->type() != QEvent::ShortcutOverride)
        return false;
    QWidget* focus = QApplication::focusWidget();
    if (obj != focus || !isViewport(focus))
        return false;
    auto* k = static_cast<QKeyEvent*>(e);
    if (k->key() != Qt::Key_F && k->key() != Qt::Key_B && k->key() != Qt::Key_M)
        return false;
    if (k->modifiers() & (Qt::ControlModifier | Qt::AltModifier))
        return false;
    if (e->type() == QEvent::ShortcutOverride)
    {
        e->accept();
        return true;
    }
    if (k->isAutoRepeat())
        return true;
    return mTool->key(k->key(), k->modifiers(), e->type() == QEvent::KeyPress);
}
} // namespace

BrushSettings& brush()
{
    return settings().brush;
}

void settingsChanged()
{
    save();
    notify();
}

bool liveMode()
{
    return settings().live;
}

void setLiveMode(bool on)
{
    if (settings().live == on)
        return;
    settings().live = on;
    settingsChanged();
}

double balance()
{
    return settings().balance;
}

void setBalance(double b)
{
    b = std::clamp(b, -1.0, 1.0);
    if (settings().balance == b)
        return;
    settings().balance = b;
    uv::live::options().balance = b;
    uv::live::optionsChanged();
    settingsChanged();
    if (liveMode())
        run(Op::Relax);
}

bool checker3d()
{
    return settings().checker;
}

void setChecker3d(bool on)
{
    settings().checker = on;
    settingsChanged();
    io::uiChanged("uvChecker");
    viewportTools::requestRedraw();
}

int checkerTiling()
{
    return settings().checkerTiling;
}

void setCheckerTiling(int checks)
{
    checks = std::clamp(checks, 1, 256);
    if (settings().checkerTiling == checks)
        return;
    settings().checkerTiling = checks;
    settingsChanged();
    io::uiChanged("uvChecker");
    viewportTools::requestRedraw();
}

bool islandSelect()
{
    return settings().islands;
}

void setIslandSelect(bool on)
{
    if (settings().islands == on)
        return;
    settings().islands = on;
    notify();
}

void setEditorShown(const void* editor, bool shown)
{
    bool changed = shown ? shownEditors().insert(editor).second : shownEditors().erase(editor) > 0;
    if (changed && settings().checker)
        viewportTools::requestRedraw();
}

bool seams3d()
{
    return settings().seams;
}

void setSeams3d(bool on)
{
    settings().seams = on;
    settingsChanged();
    viewportTools::requestRedraw();
}

bool twoColourHeatmap()
{
    return settings().twoColour;
}

void setTwoColourHeatmap(bool on)
{
    settings().twoColour = on;
    settingsChanged();
}

int addListener(std::function<void()> fn)
{
    static int next = 1;
    listeners()[next] = std::move(fn);
    return next++;
}

void removeListener(int id)
{
    listeners().erase(id);
}

std::vector<NodeId> shownMeshes()
{
    return uv::shownMeshes();
}

std::vector<geo::Index> selectedFaces(NodeId node)
{
    ComponentMode mode = scene::components::mode();
    if (mode == ComponentMode::Object)
        return {};
    const auto& ids = scene::components::selected(node, mode);
    MeshPtr m = cageOf(node);
    if (ids.empty() || !m)
        return {};
    return scene::components::convert(*m, mode, ids, ComponentMode::Face);
}

void ensureDriver()
{
    static QPointer<QTimer> timer;
    if (timer)
        return;
    timer = new QTimer(qApp);
    timer->setInterval(16);
    QObject::connect(timer, &QTimer::timeout, [] {
        bool open = uv::live::anyOpen();
        if (uv::live::tickAll() || open)
        {
            notify();
            viewportTools::requestRedraw();
        }
    });
    timer->start();
    // Solver threads stop while the app is still whole, not during static destruction.
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, [] { uv::live::clear(); });
}

void run(Op op)
{
    const char* label = op == Op::Relax ? "UV Relax" : op == Op::Flatten ? "UV Flatten" : "UV Settle";
    for (NodeId node : shownMeshes())
    {
        uv::live::Session* s = uv::live::get(node);
        if (!s || s->islandCount() == 0)
            continue;
        std::vector<int> islands = s->islandsOfFaces(selectedFaces(node));
        s->begin(label);
        s->wake(islands, [op](uv::LiveIsland& is) {
            resetOptions(is);
            if (op == Op::Flatten)
                is.flatten();
            else if (op == Op::Settle)
            {
                uv::LiveOptions o = is.options();
                o.slimSteps = 10;
                o.tolerance = 5e-6;
                is.setOptions(o);
            }
        });
        s->setStroking(false);
    }
    ensureDriver();
}

void settleAfter(NodeId node, const std::vector<geo::Index>& faces, const char* label)
{
    if (!liveMode())
        return;
    uv::live::Session* s = uv::live::get(node);
    if (!s || s->islandCount() == 0)
        return;
    std::vector<int> islands = s->islandsOfFaces(faces);
    if (islands.empty())
        return;
    s->begin(label);
    s->wake(islands, resetOptions);
    s->setStroking(false);
    ensureDriver();
}

nlohmann::json cutOrSew(const char* command, nlohmann::json call)
{
    std::vector<std::pair<NodeId, std::vector<geo::Index>>> touched;
    for (NodeId node : shownMeshes())
        touched.emplace_back(node, selectedFaces(node));
    if (!call.contains("relax"))
        call["relax"] = std::string(command) == "uvCut" || liveMode();
    json r;
    try
    {
        r = ops::execute(command, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        return nullptr;
    }
    for (auto& [node, faces] : touched)
        settleAfter(node, faces, "UV Live Settle");
    return r;
}

void startBrushTool3d()
{
    viewportTools::set(std::make_unique<BrushTool3d>());
}

void install()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    settings();
    viewportHooks::addDrawer(draw3d);
    scene::components::addListener([] {
        if (settings().islands && scene::components::mode() != scene::ComponentMode::Face)
            setIslandSelect(false);
    });
    // Asking for faces while already in face mode (island select is face mode too) means plain faces.
    ops::addExecuteListener([](const ExecuteEvent& e) {
        if (e.done && e.ok && (e.name == "selectMode" || e.name == "convertSelection") && e.call.contains("args") && e.call["args"].size() == 1 && e.call["args"][0] == "face")
            setIslandSelect(false);
    });
    // Per scene; the prefs keep the last values for scenes saved without them.
    io::registerUiSection(
        "uvChecker", [] { return nlohmann::json{{"on", settings().checker}, {"tiling", settings().checkerTiling}}; },
        [](const nlohmann::json& section) {
            if (!section.is_object())
                return;
            settings().checker = section.value("on", settings().checker);
            settings().checkerTiling = std::clamp(section.value("tiling", settings().checkerTiling), 1, 256);
            settingsChanged();
            viewportTools::requestRedraw();
        });

    auto add = [](std::string id, std::string label, std::function<void()> fn, std::string doc, std::string python = {}) {
        actions::add({.id = std::move(id), .label = std::move(label), .category = "UV", .run = std::move(fn), .doc = std::move(doc),
                      .python = std::move(python)});
    };
    add("uv.liveMode", "Live UV Mode", [] { setLiveMode(!liveMode()); },
        "Toggle live mode: dragging uvs steers the solver and the island relaxes around them; cuts, sews and pins re-solve as they happen.");
    add("uv.liveRelax", "Relax UVs (Live)", [] { run(Op::Relax); }, "Relax the selected islands live until they settle (one undo step).",
        ops::toPython("uvLiveRelax", json::object()));
    add("uv.liveFlatten", "Flatten UVs (Live)", [] { run(Op::Flatten); },
        "Flatten the selected islands from scratch, keeping pins, size and placement, then relax them live.",
        ops::toPython("uvLiveRelax", {{"flatten", true}}));
    add("uv.liveSettle", "Settle UVs", [] { run(Op::Settle); }, "Relax the selected islands to convergence and finish with a fold-free SLIM pass.",
        ops::toPython("uvLiveRelax", {{"slim", 10}}));
    add("uv.pin", "Pin UVs", [] {
            try
            {
                ops::execute("uvPin", json::object(), CallSource::UI);
            }
            catch (const CommandError& e)
            {
                log::error("{}", e.what());
            }
        },
        "Pin the selected uvs so the solver keeps them in place.", ops::toPython("uvPin", json::object()));
    add("uv.unpin", "Unpin UVs", [] {
            try
            {
                ops::execute("uvUnpin", json::object(), CallSource::UI);
            }
            catch (const CommandError& e)
            {
                log::error("{}", e.what());
            }
        },
        "Unpin the selected uvs.", ops::toPython("uvUnpin", json::object()));
    add("uv.cut", "Cut UV Edges", [] { cutOrSew("uvCut", json::object()); }, "Cut the uvs along the selected edges and let the cut open.",
        ops::toPython("uvCut", json::object()));
    add("uv.sew", "Sew UV Edges", [] { cutOrSew("uvSew", json{{"move", true}}); },
        "Move and Sew: weld the selected seams, the smaller island moving to meet the other.", ops::toPython("uvSew", json::object()));
    add("uv.weld", "Weld UV Edges", [] { cutOrSew("uvSew", json{{"move", false}}); },
        "Weld the selected seams where they are, both sides meeting halfway.", ops::toPython("uvSew", {{"move", false}}));
    add("uv.brush3d", "UV Brush (3D)", [] { startBrushTool3d(); },
        "Paint the UV editor's brush (relax, flatten, pin, stiffen...) on the mesh in the viewport; B drag sizes it, M drag sets strength.");
    add("uv.checker3d", "UV Checker in 3D", [] { setChecker3d(!checker3d()); }, "Show a checker texture on the meshes in the UV editor.");
    add("uv.seams3d", "UV Seams in 3D", [] { setSeams3d(!seams3d()); }, "Draw uv seams over the meshes in the UV editor.");
    add("uv.includeChildren", "UVs of Child Meshes", [] {
        uv::setIncludeChildren(!uv::includeChildren());
        settingsChanged();
        viewportTools::requestRedraw();
    }, "Toggle whether the uv editor and uv tools include meshes under the selected objects, or only the selected meshes.");
    add("uv.heatmapTwoColour", "Two-Colour Stretch Map", [] { setTwoColourHeatmap(!twoColourHeatmap()); },
        "Show the UV editor's stretch map in two colours instead of grayscale.");
}
} // namespace lila::ui::uvLive
