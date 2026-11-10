#include "ui/CurveUi.h"

#include "brush/BrushUi.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "curve/CurveNodes.h"
#include "groom/Groom.h"
#include "groom/GroomNodes.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "ops/Undo.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/OptionBox.h"
#include "ui/UiCommands.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportToolMath.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QSpinBox>
#include <QTimer>

#include <cmath>
#include <memory>

namespace lila::ui
{
namespace
{
using D3 = glm::dvec3;


constexpr const char* kCurveSculptPrefix = "/curveSculpt";

/** What the Sculpt Curves tool remembers between sessions. */
struct CurveSculptSettings
{
    groom::Brush brush = groom::Brush::Comb;
    brush::BrushSettings stroke;
    int addCount = 5;
    double addLength = 5.0;
    int addPoints = 8;
    bool interpolate = true;
    double minDistance = 0.0;
    double density = 1.0;
    bool keepLength = true;
    bool frontOnly = true;
    bool settleWhileGrabbing = true;
};

CurveSculptSettings& curveSculptSettings()
{
    static CurveSculptSettings s = [] {
        CurveSculptSettings c;
        const PrefStore& p = PrefStore::get();
        groom::parseBrush(p.value<std::string>("general", std::string(kCurveSculptPrefix) + "/brush", "comb"), &c.brush);
        c.stroke.strength = 0.5;
        c.stroke.loadPrefs(kCurveSculptPrefix);
        c.stroke.radius = std::clamp(c.stroke.radius, 2.0, 2000.0);
        auto key = [](const char* k) { return std::string(kCurveSculptPrefix) + "/" + k; };
        c.addCount = std::clamp(p.value<int>("general", key("addCount"), c.addCount), 1, 1000);
        c.addLength = std::max(1e-3, p.value<double>("general", key("addLength"), c.addLength));
        c.addPoints = std::clamp(p.value<int>("general", key("addPoints"), c.addPoints), 2, 256);
        c.interpolate = p.value<bool>("general", key("interpolate"), c.interpolate);
        c.minDistance = std::max(0.0, p.value<double>("general", key("minDistance"), c.minDistance));
        c.density = std::max(1e-4, p.value<double>("general", key("density"), c.density));
        c.keepLength = p.value<bool>("general", key("keepLength"), c.keepLength);
        c.frontOnly = p.value<bool>("general", key("frontOnly"), c.frontOnly);
        c.settleWhileGrabbing = p.value<bool>("general", key("settleWhileGrabbing"), c.settleWhileGrabbing);
        return c;
    }();
    return s;
}

void saveSettings()
{
    const CurveSculptSettings& c = curveSculptSettings();
    PrefStore& p = PrefStore::get();
    auto key = [](const char* k) { return std::string(kCurveSculptPrefix) + "/" + k; };
    p.set("general", key("brush"), std::string(groom::brushName(c.brush)));
    c.stroke.savePrefs(kCurveSculptPrefix);
    p.set("general", key("addCount"), c.addCount);
    p.set("general", key("addLength"), c.addLength);
    p.set("general", key("addPoints"), c.addPoints);
    p.set("general", key("interpolate"), c.interpolate);
    p.set("general", key("minDistance"), c.minDistance);
    p.set("general", key("density"), c.density);
    p.set("general", key("keepLength"), c.keepLength);
    p.set("general", key("frontOnly"), c.frontOnly);
    p.set("general", key("settleWhileGrabbing"), c.settleWhileGrabbing);
    syncGroomPalette();
    viewportTools::requestRedraw();
}

QString brushLabel(groom::Brush b)
{
    QString n = groom::brushName(b);
    return n.left(1).toUpper() + n.mid(1);
}

D3 xform(const glm::dmat4& m, const D3& p, double w) { return D3(m * glm::dvec4(p, w)); }

/**
 * Sculpt Curves: brushes for curve nodes holding many curves (hair, fur). 1 to 0 pick comb, smooth, length, puff, add,
 * delete, density, cut, pinch and slide; Tab swaps to the lock brushes (grab, twist, widen, flatten, curl, frizz, clump,
 * split, merge, settle) on the same keys. Shift smooths and Ctrl inverts the next stroke. B-drag or Ctrl+scroll sets the
 * radius, M-drag or Shift+scroll the strength. The stroke plays live on a copy and lands as one undoable command.
 */
class CurveSculptTool final : public ViewportTool, public brush::DabSink
{
public:
    const char* name() const override { return "Sculpt Curves"; }
    // The brush ring and its centre dot are the cursor.
    std::optional<QCursor> cursor() const override { return QCursor(Qt::BlankCursor); }
    bool ownsBrushRadius() const override { return true; }

    void activate() override
    {
        scene::Graph& g = scene::Graph::current();
        std::string mesh;
        for (scene::NodeId id : g.selection())
            if (scene::Node* n = g.node(id))
            {
                if (n->type().isA("curve") && mNode == scene::kNoNode)
                    mNode = id;
                else if (n->type().isA("groom") && mNode == scene::kNoNode)
                    if (scene::Node* guides = groom::guidesOf(g, *n))
                        mNode = guides->id();
                else if (n->type().isA("mesh") && mesh.empty())
                    mesh = n->path();
            }
        if (mNode == scene::kNoNode)
            for (auto& [id, ids] : scene::components::selected(scene::ComponentMode::Vertex))
                if (scene::Node* n = g.node(id); n && n->type().isA("curve"))
                {
                    mNode = id;
                    break;
                }
        // A mesh selected with the curves becomes their surface when they have none.
        if (scene::Node* n = g.node(mNode); n && !mesh.empty() && groom::groomSurfaceOf(*n).empty())
            runUiWithOptions("groomSurface", {{"args", {n->path()}}, {"mesh", mesh}, {"attach", false}});
        mControls.activate({}, {});
        const scene::NodeId* target = &mNode;
        const bool* stroking = &mStroking;
        viewportHooks::setHideHook("lila.curveSculpt", [target, stroking](scene::NodeId id) { return *stroking && id == *target; });
        // On a groom's guides the lock brushes sit with the groom brushes.
        scene::Node* curve = g.node(mNode);
        showGroomPalette(curve && groom::groomOf(g, *curve) ? GroomPalettePage::Groom : GroomPalettePage::Sculpt);
    }

    void deactivate() override
    {
        if (mStroking)
            endStroke();
        mControls.deactivate();
        hideGroomPalette();
        viewportHooks::removeHideHook("lila.curveSculpt");
        viewportTools::requestRedraw();
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (mControls.mousePress(e))
            return true;
        if (e->button() != Qt::LeftButton)
            return false;
        if (!target())
        {
            log::warn("Sculpt Curves: select a curve holding the curves to sculpt");
            return true;
        }
        beginStroke(e, view);
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        mHoverView = view.camera;
        if (mControls.mouseMove(e))
            return true;
        if (mStroking)
        {
            mStroke.move(mControls.input(e));
            return true;
        }
        viewportTools::requestRedraw();
        return false;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (mControls.mouseRelease(e))
            return true;
        if (e->button() != Qt::LeftButton)
            return false;
        if (mStroking)
            endStroke();
        return true;
    }

    bool wheel(QWheelEvent* e, const ViewInfo&) override
    {
        const double steps = e->angleDelta().y() / 120.0;
        brush::BrushSettings& s = curveSculptSettings().stroke;
        if (e->modifiers() & Qt::ControlModifier)
        {
            if (s.radiusUnit == brush::RadiusUnit::Screen)
                s.radius = std::clamp(s.radius * std::pow(1.1, steps), 2.0, 2000.0);
            else
                s.worldRadius = std::clamp(s.worldRadius * std::pow(1.1, steps), 1e-3, 1e5);
            saveSettings();
            return true;
        }
        if (e->modifiers() & Qt::ShiftModifier)
        {
            s.strength = std::clamp(s.strength + 0.05 * steps, 0.0, 1.0);
            saveSettings();
            return true;
        }
        return false;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (mControls.keyPress(e))
            return true;
        return key(e->key(), e->modifiers());
    }

    void draw(render::FrameState& f, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();

        const CurveSculptSettings& st = curveSculptSettings();
        scene::Node* n = target();
        if (mStroking && n)
        {
            const glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            const glm::vec4 col(0.55f, 0.55f, 0.55f, 1.0f);
            for (const curve::Spline& s : mWork)
                for (size_t i = 1; i < s.size(); ++i)
                {
                    f.lines.push_back({glm::vec3(xform(w, s.points[i - 1], 1.0)), col});
                    f.lines.push_back({glm::vec3(xform(w, s.points[i], 1.0)), col});
                }
        }
        const Qt::KeyboardModifiers mods = QApplication::keyboardModifiers();
        const groom::Brush b = mStroking ? mBrush : (mods & Qt::ShiftModifier) ? groom::Brush::Smooth : st.brush;
        const bool invert = mStroking ? mInvert : (mods & Qt::ControlModifier) != 0;
        QString surface = n ? QString::fromStdString(groom::groomSurfaceOf(*n)) : QString();
        drawToolStatus(p, view,
                       {.tool = "Sculpt Curves",
                        .subject = n ? QString::fromStdString(n->name()) : QString(),
                        .mode = brushLabel(b),
                        .fields = {{"radius", st.stroke.radiusUnit == brush::RadiusUnit::Screen ? QString("%1 px").arg(int(std::lround(st.stroke.radius)))
                                                                                                 : QString("%1 cm").arg(st.stroke.worldRadius, 0, 'f', 2)},
                                   {"strength", QString::number(st.stroke.strength, 'f', 2)},
                                   {"surface", surface.isEmpty() ? QString("none") : surface}},
                        .flags = {{"invert", invert}},
                        .prompt = !n ? QString("select a curve") : surface.isEmpty() ? QString("add and density need a surface") : QString(),
                        .symmetry = n && curve::symmetryAxis(*n) >= 0 ? std::optional<ToolSymmetry>(ToolSymmetry{true, QString("XYZ").mid(curve::symmetryAxis(*n), 1)})
                                                                     : std::nullopt});
        if (groomKeysShown())
            drawKeyList(p, QPointF(16.0, view.height - 84.0), curveSculptKeys());
        const QPointF at = mControls.anchor(brush::ui::ringPosition(mStroke, mHover));
        std::optional<brush::ui::CursorSurface> cs;
        brush::SurfaceHit hit;
        if (mHoverView == view.camera && query(QPointF(at), view, hit))
            cs = brush::ui::CursorSurface{hit.center, hit.normal};
        brush::ui::drawCursor(p, view, st.stroke, at, cs,
                              {.strengthRing = mControls.adjusting() == brush::ui::BrushControls::Adjust::Strength, .normalTick = true, .stroke = &mStroke});
    }

    void dab(const brush::Dab& d) override
    {
        scene::Node* n = target();
        if (!n)
            return;
        const glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        // World space, as the command takes them; the comb follows the cursor's travel since the last dab.
        json j = {{"center", vecJson(d.center)},
                  {"normal", vecJson(d.normal)},
                  {"radius", d.radius},
                  {"strength", std::clamp(d.opacity * d.flow, 0.0, 1.0)},
                  {"delta", vecJson(d.direction)},
                  {"view", vecJson(d.viewDir)}};
        mDabs.push_back(j);
        // The preview replays the dab exactly as the command will.
        groom::Dab g;
        const glm::dmat4 inv = glm::inverse(w);
        const glm::dmat3 nrm = glm::transpose(glm::dmat3(w));
        const double scale = std::max(1e-12, std::cbrt(std::abs(glm::determinant(glm::dmat3(w)))));
        // Brushing the mirrored copies edits the guides they mirror, as the command does.
        D3 wc = d.center, wn = d.normal, wd = d.direction, wv = d.viewDir;
        groom::mirrorDabToSource(*n, mWork, wc, wn, wd, wv);
        g.center = xform(inv, wc, 1.0);
        const D3 nn = nrm * wn;
        g.normal = glm::length(nn) > 1e-12 ? glm::normalize(nn) : D3(0, 1, 0);
        g.radius = d.radius / scale;
        g.strength = j["strength"].get<double>();
        g.delta = xform(inv, wd, 0.0);
        const D3 v = xform(inv, wv, 0.0);
        g.viewDir = glm::length(v) > 1e-12 ? glm::normalize(v) : D3(0.0);
        groom::applyDab(mWork, mApply, g, mSurfaceLocal.get(), mRng);
        showLive(false);
        viewportTools::requestRedraw();
    }

private:
    scene::Node* target() const
    {
        scene::Node* n = scene::Graph::current().node(mNode);
        return n && n->type().isA("curve") ? n : nullptr;
    }

    bool key(int k, Qt::KeyboardModifiers)
    {
        if (k == Qt::Key_Escape)
        {
            QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
            return true;
        }
        return false;
    }

    /** The surface (world space) and its copy in the curve's object space, rebuilt when the mesh or either matrix changes. */
    void syncSurface(scene::Node& curve)
    {
        scene::Graph& g = scene::Graph::current();
        const std::string name = groom::groomSurfaceOf(curve);
        scene::Node* mesh = name.empty() ? nullptr : g.find(name);
        if (!mesh || !mesh->type().isA("mesh"))
        {
            mSurfaceWorld.reset();
            mSurfaceLocal.reset();
            mSurfaceKey = {};
            return;
        }
        g.evaluate();
        scene::MeshPtr m = mesh->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        const glm::dmat4 mw = mesh->as<glm::dmat4>(scene::Transform::kWorldMatrix), cw = curve.as<glm::dmat4>(scene::Transform::kWorldMatrix);
        SurfaceKey key{m.get(), mw, cw};
        if (key == mSurfaceKey && mSurfaceWorld)
            return;
        mSurfaceKey = key;
        mSurfaceWorld.reset();
        mSurfaceLocal.reset();
        if (!m || m->numFaces() == 0)
            return;
        auto moved = [&](const glm::dmat4& t) {
            geo::Mesh c = *m;
            for (geo::Index pi = 0; pi < c.pointCapacity(); ++pi)
                if (c.pointValid(pi))
                    c.setPosition(pi, xform(t, c.position(pi), 1.0));
            return c;
        };
        mSurfaceWorld = std::make_unique<groom::Surface>(moved(mw));
        mSurfaceLocal = std::make_unique<groom::Surface>(moved(glm::inverse(cw) * mw));
    }

    /** What the brush is over: the surface, else the nearest curve point under the ring, else the plane through the curves. */
    bool query(QPointF px, const ViewInfo& view, brush::SurfaceHit& out)
    {
        scene::Node* n = target();
        if (!n)
            return false;
        syncSurface(*n);
        D3 o, d;
        view.ray(px, o, d);
        d = glm::normalize(d);
        out.viewDir = d;
        out.eye = view.eye;
        out.right = brush::ui::screenRight(view, glm::dmat4(1.0));
        out.worldToLocal = 1.0;
        if (mSurfaceWorld)
            if (auto h = mSurfaceWorld->raycast(o, d))
            {
                out.center = h->point;
                out.normal = h->normal;
                out.pixelSize = view.unitsPerPixel(h->point);
                return true;
            }
        const glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        const curve::Splines& splines = mStroking ? mWork : curve::shapeSplines(scene::Graph::current(), mNode);
        const double reach = curveSculptSettings().stroke.radius;
        double best = reach;
        std::optional<D3> nearest;
        D3 lo(1e300), hi(-1e300);
        for (const curve::Spline& s : splines)
            for (const D3& p : s.points)
            {
                const D3 wp = xform(w, p, 1.0);
                lo = glm::min(lo, wp);
                hi = glm::max(hi, wp);
                if (auto q = view.project(wp))
                    if (double dd = std::hypot(q->x() - px.x(), q->y() - px.y()); dd < best)
                    {
                        best = dd;
                        nearest = wp;
                    }
            }
        if (lo.x > hi.x)
            return false;
        // Depth of the nearest curve point, else the middle of them all, on the plane facing the view.
        const D3 anchor = nearest ? *nearest : (lo + hi) * 0.5;
        const double den = glm::dot(d, d);
        const double t = glm::dot(anchor - o, d) / den;
        out.center = o + d * t;
        out.normal = -d;
        out.pixelSize = view.unitsPerPixel(out.center);
        return true;
    }

    /**
     * On a groom's guides the stroke shows on the locks as it goes: the guides hold the work in progress with undo off,
     * and get their start value back before the stroke lands as one command.
     */
    void showLive(bool force)
    {
        if (!mLive || (!force && mLiveClock.isValid() && mLiveClock.elapsed() < 33))
            return;
        mLiveClock.start();
        scene::Graph& g = scene::Graph::current();
        UndoStack& u = UndoStack::get();
        const bool was = u.enabled();
        u.setEnabled(false);
        if (scene::Node* n = g.node(mNode))
            groom::storeGuides(g, *n, mWork);
        u.setEnabled(was);
    }

    void endLive()
    {
        if (!mLive)
            return;
        mLive = false;
        UndoStack& u = UndoStack::get();
        const bool was = u.enabled();
        u.setEnabled(false);
        scene::Graph::current().setValue(mLiveAt, mLiveStart);
        u.setEnabled(was);
        mLiveStart = {};
    }

    void beginStroke(const QMouseEvent* e, const ViewInfo& view)
    {
        scene::Node* n = target();
        scene::Graph& g = scene::Graph::current();
        const CurveSculptSettings& st = curveSculptSettings();
        const Qt::KeyboardModifiers mods = e->modifiers();
        mBrush = (mods & Qt::ShiftModifier) ? groom::Brush::Smooth : st.brush;
        mInvert = (mods & Qt::ControlModifier) != 0;
        mSeed = uint32_t(e->timestamp());
        mRng.seed(mSeed);
        mWork = curve::shapeSplines(g, mNode);
        mApply = {};
        mApply.brush = mBrush;
        const brush::Falloff falloff = st.stroke.resolveFalloff();
        mApply.falloff = [falloff](double t) { return falloff.evaluate(t); };
        mApply.strength = st.stroke.strength;
        mApply.invert = mInvert;
        mApply.keepLength = st.keepLength;
        mApply.frontOnly = st.frontOnly;
        mApply.addCount = st.addCount;
        mApply.addLength = st.addLength;
        mApply.addPoints = st.addPoints;
        mApply.interpolate = st.interpolate;
        mApply.minDistance = st.minDistance;
        mApply.density = st.density;
        mApply.settleWhileGrabbing = st.settleWhileGrabbing;
        if (groom::isLockBrush(mBrush))
            groom::lockBrushSettings(g, *n, mWork, glm::inverse(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)), mApply);
        // As the command does: with points of the curve selected, only the curves holding them change.
        const auto& sel = scene::components::selected(mNode, scene::ComponentMode::Vertex);
        if (!sel.empty())
        {
            mApply.mask.assign(mWork.size(), 0);
            for (uint32_t id : sel)
            {
                size_t si, pi;
                curve::PointRole r;
                curve::decodeComponent(id, &si, &pi, &r);
                if (si < mWork.size())
                    mApply.mask[si] = 1;
            }
        }
        syncSurface(*n);
        // Live on a groom's guides: the work goes into the groom's hierarchy, whose start value comes back after.
        scene::Node* groomNode = groom::groomOf(g, *n);
        mLive = groomNode != nullptr;
        if (mLive)
        {
            mLiveAt = {groomNode->id(), groomNode->type().attrIndex("hierarchy"), -1};
            mLiveStart = g.value(mLiveAt);
            mLiveClock.invalidate();
        }
        mDabs = json::array();
        mStroking = true;
        brush::BrushSettings stroke = st.stroke;
        stroke.seed = mSeed;
        const ViewInfo v = view;
        mStroke.begin(stroke, mControls.input(e), [this, v](glm::dvec2 px, brush::SurfaceHit& out) { return query(QPointF(px.x, px.y), v, out); },
                      *this, false);
        viewportTools::requestRedraw();
    }

    void endStroke()
    {
        mStroke.end();
        mStroking = false;
        endLive();
        scene::Node* n = target();
        if (n && !mDabs.empty())
        {
            const CurveSculptSettings& st = curveSculptSettings();
            json call = {{"args", {n->path()}},
                         {"brush", groom::brushName(mBrush)},
                         {"dabs", std::move(mDabs)},
                         {"falloff", st.stroke.falloff},
                         {"strength", st.stroke.strength},
                         {"invert", mInvert},
                         {"keepLength", st.keepLength},
                         {"frontOnly", st.frontOnly},
                         {"addCount", st.addCount},
                         {"addLength", st.addLength},
                         {"addPoints", st.addPoints},
                         {"interpolate", st.interpolate},
                         {"minDistance", st.minDistance},
                         {"density", st.density},
                         {"settleWhileGrabbing", st.settleWhileGrabbing},
                         {"seed", int64_t(mSeed)}};
            try
            {
                // Not echoed: a stroke is hundreds of dabs.
                ops::execute("curveSculptStroke", call, CallSource::Script);
            }
            catch (const CommandError& e)
            {
                log::error("{}", e.what());
            }
        }
        mDabs = json::array();
        mWork.clear();
        viewportTools::requestRedraw();
    }

    struct SurfaceKey
    {
        const geo::Mesh* mesh = nullptr;
        glm::dmat4 meshWorld{0.0}, curveWorld{0.0};
        bool operator==(const SurfaceKey&) const = default;
    };

    brush::ui::BrushControls mControls{[]() -> brush::BrushSettings& { return curveSculptSettings().stroke; }, [] { saveSettings(); }, {}};
    scene::NodeId mNode = scene::kNoNode;
    brush::Stroke mStroke;
    bool mStroking = false;
    groom::Brush mBrush = groom::Brush::Comb;
    bool mInvert = false;
    uint32_t mSeed = 0;
    std::mt19937 mRng;
    groom::Settings mApply;
    curve::Splines mWork;
    json mDabs = json::array();
    bool mLive = false;
    scene::PlugRef mLiveAt;
    scene::PlugValue mLiveStart;
    QElapsedTimer mLiveClock;
    QPointF mHover;
    std::string mHoverView;
    std::unique_ptr<groom::Surface> mSurfaceWorld, mSurfaceLocal;
    SurfaceKey mSurfaceKey;
};

void openOptions()
{
    CurveSculptSettings& st = curveSculptSettings();
    QDialog dlg(QApplication::activeWindow());
    dlg.setWindowTitle("Sculpt Curves Options");
    auto* form = new QFormLayout(&dlg);
    auto* brushBox = new QComboBox(&dlg);
    for (int i = 0; i < int(groom::Brush::Count); ++i)
        brushBox->addItem(brushLabel(groom::Brush(i)));
    brushBox->setCurrentIndex(int(st.brush));
    auto spin = [&](double v, double lo, double hi, double step, const char* suffix) {
        auto* s = new QDoubleSpinBox(&dlg);
        s->setRange(lo, hi);
        s->setSingleStep(step);
        s->setDecimals(3);
        s->setSuffix(suffix);
        s->setValue(v);
        return s;
    };
    auto ispin = [&](int v, int lo, int hi) {
        auto* s = new QSpinBox(&dlg);
        s->setRange(lo, hi);
        s->setValue(v);
        return s;
    };
    auto check = [&](bool v) {
        auto* c = new QCheckBox(&dlg);
        c->setChecked(v);
        return c;
    };
    QDoubleSpinBox* strength = spin(st.stroke.strength, 0.0, 1.0, 0.05, "");
    QComboBox* falloff = new QComboBox(&dlg);
    for (int i = 0; i < int(brush::Shape::Count); ++i)
        falloff->addItem(QString::fromUtf8(brush::shapeName(brush::Shape(i)).data(), qsizetype(brush::shapeName(brush::Shape(i)).size())));
    falloff->setCurrentText(QString::fromStdString(st.stroke.falloff));
    QSpinBox* addCount = ispin(st.addCount, 1, 1000);
    QDoubleSpinBox* addLength = spin(st.addLength, 0.001, 1e4, 0.5, " cm");
    QSpinBox* addPoints = ispin(st.addPoints, 2, 256);
    QCheckBox* interpolate = check(st.interpolate);
    QDoubleSpinBox* minDistance = spin(st.minDistance, 0.0, 1e3, 0.05, " cm");
    QDoubleSpinBox* density = spin(st.density, 0.0001, 1e4, 0.1, " /cm\xc2\xb2");
    QCheckBox* keepLength = check(st.keepLength);
    QCheckBox* frontOnly = check(st.frontOnly);
    QCheckBox* settleGrab = check(st.settleWhileGrabbing);
    form->addRow("Brush", brushBox);
    form->addRow("Strength", strength);
    form->addRow("Falloff", falloff);
    form->addRow("Add: Curves per Dab", addCount);
    form->addRow("Add: Length (no neighbours)", addLength);
    form->addRow("Add: Points", addPoints);
    form->addRow("Add: Shape from Neighbours", interpolate);
    form->addRow("Min Root Distance (0 = auto)", minDistance);
    form->addRow("Density", density);
    form->addRow("Keep Segment Lengths", keepLength);
    form->addRow("Front Only", frontOnly);
    form->addRow("Settle While Grabbing", settleGrab);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted)
        return;
    st.brush = groom::Brush(brushBox->currentIndex());
    st.stroke.strength = strength->value();
    st.stroke.falloff = falloff->currentText().toStdString();
    st.addCount = addCount->value();
    st.addLength = addLength->value();
    st.addPoints = addPoints->value();
    st.interpolate = interpolate->isChecked();
    st.minDistance = minDistance->value();
    st.density = density->value();
    st.keepLength = keepLength->isChecked();
    st.frontOnly = frontOnly->isChecked();
    st.settleWhileGrabbing = settleGrab->isChecked();
    saveSettings();
}
} // namespace

int curveSculptBrush() { return int(curveSculptSettings().brush); }

void setCurveSculptBrush(int brush)
{
    curveSculptSettings().brush = groom::Brush(std::clamp(brush, 0, int(groom::Brush::Count) - 1));
    saveSettings();
}

void resetCurveSculptSettings()
{
    CurveSculptSettings& c = curveSculptSettings();
    const groom::Brush brush = c.brush;
    c = CurveSculptSettings{};
    c.brush = brush;
    c.stroke.strength = 0.5;
    saveSettings();
}

bool curveSculptSettleGrab() { return curveSculptSettings().settleWhileGrabbing; }

void setCurveSculptSettleGrab(bool on)
{
    curveSculptSettings().settleWhileGrabbing = on;
    saveSettings();
}

std::vector<KeyHint> curveSculptKeys()
{
    std::vector<KeyHint> keys = {{"Drag", brushLabel(curveSculptSettings().brush)}, {"Shift+drag", "Smooth"}, {"Ctrl+drag", "Invert"}};
    keys.insert(keys.end(), {{"B+drag", "Radius"}, {"M+drag", "Strength"}, {"[  ]", "Smaller, larger"}, {"Esc", "Exit"}});
    return keys;
}

void registerCurveSculptTool()
{
    if (!viewportTools::registry().find("curve.sculptCurves"))
        viewportTools::add({.id = "curve.sculptCurves", .label = "Sculpt Curves", .icon = "brush", .create = [] {
                                return std::make_unique<CurveSculptTool>();
                            }});
    actions::add({.id = "curve.sculptCurves",
                  .label = "Sculpt Curves",
                  .category = "Curves",
                  .run =
                      [] {
                          // A groom is brushed with the groom brushes.
                          scene::Graph& g = scene::Graph::current();
                          for (scene::NodeId id : g.selection())
                              if (scene::Node* n = g.node(id); n && n->type().isA("groom"))
                              {
                                  viewportTools::activate("groom.brushes");
                                  return;
                              }
                          viewportTools::activate("curve.sculptCurves");
                      },
                  .doc = "Combs, smooths, grows, puffs, adds, deletes, thins, cuts, pinches and slides the selected curve's many curves (hair) "
                         "with brushes; roots stay on its groom surface. Select a mesh too to make it the surface.",
                  .python = "lila.ui.run_action(\"curve.sculptCurves\")"});
    optionBox::addCustom("curve.sculptCurves", openOptions);
    for (const char* b : {"comb", "smooth", "length", "puff", "add", "delete", "density", "cut", "pinch", "slide", "grab", "twist", "widen", "flatten", "curl",
                          "frizz", "clump", "split", "merge", "settle"})
    {
        groom::Brush br;
        groom::parseBrush(b, &br);
        actions::add({.id = std::string("curve.sculptCurves.") + b,
                      .label = "Sculpt Curves: " + brushLabel(br).toStdString(),
                      .category = "Curves",
                      .run =
                          [br] {
                              curveSculptSettings().brush = br;
                              saveSettings();
                              if (!viewportTools::active() || std::string_view(viewportTools::active()->name()) != "Sculpt Curves")
                                  viewportTools::activate("curve.sculptCurves");
                          },
                      .doc = "Sculpt Curves with the " + std::string(b) + " brush.",
                      .python = "lila.ui.run_action(\"curve.sculptCurves." + std::string(b) + "\")"});
    }
    actions::add({.id = "curve.groomSurface",
                  .label = "Set Groom Surface",
                  .category = "Curves",
                  .run =
                      [] {
                          scene::Graph& g = scene::Graph::current();
                          json curves = json::array();
                          std::string mesh;
                          for (scene::NodeId id : g.selection())
                              if (scene::Node* n = g.node(id))
                              {
                                  if (n->type().isA("curve"))
                                      curves.push_back(n->path());
                                  else if (n->type().isA("mesh"))
                                      mesh = n->path();
                              }
                          if (curves.empty() || mesh.empty())
                          {
                              log::error("Set Groom Surface: select the curves and the mesh they grow from");
                              return;
                          }
                          runUiWithOptions("groomSurface", {{"args", curves}, {"mesh", mesh}});
                      },
                  .doc = "Makes the selected mesh the surface the selected curves grow from, moving their roots onto it.",
                  .python = "lila.ops.groomSurface(\"Hair\", mesh=\"Scalp\")",
                  .command = "groomSurface"});
    actions::add({.id = "curve.groomGuides",
                  .label = "Guide Curves from Mesh",
                  .category = "Curves",
                  .doc = "Grows guide curves out of the selected mesh along its normals, spread by area and an optional density color set.",
                  .python = ops::toPython("groomGuides", json::object()),
                  .command = "groomGuides",
                  .call = json::object()});
    actions::add({.id = "curve.hairCards",
                  .label = "Hair Cards",
                  .category = "Curves",
                  .doc = "Builds textured hair cards from the selected curves with live history: per curve, per clump or per guide, tapered, "
                         "facing the scalp, UVs in atlas cells.",
                  .python = ops::toPython("hairCards", json::object()),
                  .command = "hairCards",
                  .call = json::object()});
}
} // namespace lila::ui
