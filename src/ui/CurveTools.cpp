#include "ui/CurveUi.h"
#include "ui/CurveToolsCommon.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "curve/CurveNodes.h"
#include "curve/Fit.h"
#include "curve/Tessellate.h"
#include "geo/Mesh.h"
#include "math/MathCore.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/Isolation.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/GeometrySnap.h"
#include "ui/Isolate.h"
#include "ui/StepHistory.h"
#include "ui/SurfaceSnap.h"
#include "ui/Tools.h"
#include "ui/ViewportTool.h"

#include <QGuiApplication>
#include <QTimer>

#include <cmath>
#include <functional>

namespace lila::ui
{
namespace curveTools
{
std::optional<D3> snapToGeometry(QPointF px, const ViewInfo& view, unsigned snaps)
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    geometrySnap::Nearest nearest(px, view, kSnapPx);
    const scene::isolation::Set* iso = scene::isolation::active();
    for (scene::NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (n->type().isA("curve"))
        {
            geometrySnap::addCurve(nearest, g, id, snaps);
            continue;
        }
        if (geometrySnap::addPointObject(nearest, g, id, snaps))
            continue;
        if (!n->type().isA("mesh") || !scene::effectivelyVisible(g, id) || !isolate::shows(iso, id))
            continue;
        scene::MeshPtr m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        if (!m)
            continue;
        const auto vis = isolate::visibility(iso, id, m);
        glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        auto at = [&](const D3& p) { return D3(w * glm::dvec4(p, 1.0)); };
        if (snaps & SnapVertex)
            for (geo::Index p = 0; p < m->pointCapacity(); ++p)
                if (m->pointValid(p) && vis->point(p))
                    nearest.point(at(m->position(p)));
        if (snaps & SnapEdge)
            for (geo::Index e = 0; e < m->edgeCapacity(); ++e)
                if (m->edgeValid(e) && vis->edge(e))
                    nearest.segment(at(m->position(m->edgePoint(e, 0))), at(m->position(m->edgePoint(e, 1))));
    }
    return nearest.hit();
}

void drawSpline(render::FrameState& f, const curve::Spline& s, const glm::vec4& color)
{
    if (!curve::isValid(s))
        return;
    curve::Polyline pl = curve::tessellate(s);
    size_t segs = pl.closed ? pl.size() : (pl.size() ? pl.size() - 1 : 0);
    for (size_t i = 0; i < segs; ++i)
    {
        f.linesOnTop.push_back({glm::dvec3(pl.points[i]), color});
        f.linesOnTop.push_back({glm::dvec3(pl.points[(i + 1) % pl.size()]), color});
    }
}

void drawSymmetric(render::FrameState& f, const curve::Spline& s, const glm::vec4& color)
{
    drawSpline(f, s, color);
    if (int axis = symmetryPref())
    {
        glm::dmat4 flip(1.0);
        flip[axis - 1][axis - 1] = -1.0;
        curve::Spline m = s;
        curve::transform(m, flip);
        drawSpline(f, m, color);
    }
}

void drawSymmetryPlane(render::FrameState& f, const ViewInfo& view)
{
    const int axis = symmetryPref();
    if (!axis)
        return;
    const int u = axis % 3, v = (axis + 1) % 3;
    // Centred under the eye and sized by its distance, so the plane fills the view from anywhere.
    D3 c = view.eye;
    c[axis - 1] = 0.0;
    const double r = std::max(1000.0, glm::length(view.eye) * 4.0);
    auto corner = [&](double su, double sv) {
        D3 p = c;
        p[u] += su * r;
        p[v] += sv * r;
        return glm::vec3(p);
    };
    const glm::vec4 red(1.0f, 0.1f, 0.1f, 0.07f);
    for (const glm::vec3& p : {corner(-1, -1), corner(1, -1), corner(1, 1), corner(-1, -1), corner(1, 1), corner(-1, 1)})
        f.tris3d.push_back({p, red});
}

void dot(OverlayPainter& p, const ViewInfo& view, const D3& w, float r, const glm::vec4& fill)
{
    if (auto q = view.project(w))
    {
        p.disc(*q, r + 1.5f, kRim, 16);
        p.disc(*q, r, fill, 16);
    }
}

void exitTool()
{
    // Not from inside the tool's own handler: set() destroys the tool.
    QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
}
} // namespace curveTools

namespace
{
using namespace curveTools;
using curve::Spline;

int degreePref() { return std::clamp(prefs().value<int>("general", "/curveTools/degree", 3), 1, 7); }

/** Creates the drawn curve; when the tool keeps curves on surfaces, it stays live on the mesh most of its points landed on. */
void createCurve(const Spline& s, const SurfaceDraw* surface = nullptr)
{
    std::string why;
    if (!curve::isValid(s, &why))
    {
        log::warn("curve: {}", why);
        return;
    }
    try
    {
        std::optional<scene::NodeId> keepOn = surface ? surface->conformTarget() : std::nullopt;
        const scene::Node* mesh = keepOn ? scene::Graph::current().node(*keepOn) : nullptr;
        UndoChunk chunk("Curve");
        json made = ops::execute("curve", {{"splines", json::array({curve::splineToJson(s)})}}, CallSource::UI);
        if (mesh && made.is_string())
            ops::execute("conformCurve", {{"args", {made}}, {"mesh", mesh->path()}, {"offset", surface->offset()}}, CallSource::UI);
        if (int axis = symmetryPref(); axis && made.is_string())
            ops::execute("curveSymmetry", {{"args", {made}}, {"axis", symmetryName(axis)}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

/** Click to place points; the builder turns them into a spline (CV, EP, Catmull-Rom, kappa). Enter finishes, clicking the first point closes. */
class ClickCurveTool final : public ViewportTool
{
public:
    using Builder = std::function<Spline(const std::vector<D3>&, bool closed)>;

    ClickCurveTool(const char* name, const char* help, Builder build) : mName(name), mHelp(help), mBuild(std::move(build)) {}

    const char* name() const override { return mName; }
    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }
    void activate() override
    {
        mSurface.load();
        mSurface.apply(mPlacer);
        log::info("{}: {}", mName, mHelp);
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        if (mPts.size() >= 3)
            if (auto first = view.project(mPts.front()); first && distPx(*first, e->position()) < kClosePx)
            {
                finish(true);
                return true;
            }
        scene::NodeId on = scene::kNoNode;
        if (auto p = mPlacer.place(e->position(), view, nullptr, &on))
        {
            if (mPts.empty())
                mPlacer.lockTo(*p);
            mPts.push_back(*p);
            mSurface.landed(on);
            mHistory.clear();
        }
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = mPlacer.place(e->position(), view);
        mHoverView = view.camera;
        viewportTools::requestRedraw();
        return false;
    }

    bool wheel(QWheelEvent* e, const ViewInfo&) override
    {
        if (!mSurface.wheel(e))
            return false;
        mSurface.apply(mPlacer);
        viewportTools::requestRedraw();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (toolKey(e->key()))
            return true;
        switch (e->key())
        {
        case Qt::Key_Return:
        case Qt::Key_Enter: finish(false); return true;
        case Qt::Key_Backspace:
            mHistory.undo(mPts);
            relock();
            viewportTools::requestRedraw();
            return true;
        case Qt::Key_Escape:
            if (mPts.empty())
                exitTool();
            mPts.clear();
            mHistory.clear();
            mSurface.clearHits();
            mPlacer.unlock();
            viewportTools::requestRedraw();
            return true;
        default: return false;
        }
    }

    bool hotkey(const QKeyEvent& e, std::string_view action) override
    {
        if (plainKey(e) && toolKey(e.key()))
            return true;
        switch (mHistory.route(e.key(), action))
        {
        case StepKey::Undo: mHistory.undo(mPts); break;
        case StepKey::Redo: mHistory.redo(mPts); break;
        case StepKey::None: return false;
        }
        relock();
        return true;
    }

    void draw(render::FrameState& f, OverlayPainter& p, const ViewInfo& view) override
    {
        std::vector<D3> pts = mPts;
        if (mHover && view.camera == mHoverView)
            pts.push_back(*mHover);
        if (pts.size() >= 2)
            drawSymmetric(f, mBuild(pts, false), kPreview);
        for (size_t i = 1; i < mPts.size(); ++i)
        {
            f.linesOnTop.push_back({glm::dvec3(mPts[i - 1]), kGhost});
            f.linesOnTop.push_back({glm::dvec3(mPts[i]), kGhost});
        }
        for (const D3& q : mPts)
            dot(p, view, q);
        if (mHover && view.camera == mHoverView)
            dot(p, view, *mHover, 3.0f, kGhost);
        ToolStatus status{.tool = mName,
                          .flags = {{"planar lock", mPlacer.lockEnabled()}},
                          .prompt = mHelp,
                          .keys = {{"Enter", "finish"}, {"First point", "close"}, {"Z", "undo point"}, {"Shift+Z", "redo point"}, {"L", "planar lock"}}};
        mSurface.addStatus(status);
        status.keys.push_back({"Esc", "exit"});
        drawToolStatus(p, view, status);
    }

private:
    /** S, K and L, which the app also binds (set key, lasso), so they come here before its hotkeys take them. */
    bool toolKey(int key)
    {
        if (mSurface.key(key))
            mSurface.apply(mPlacer);
        else if (key == Qt::Key_L)
        {
            mPlacer.toggleLock(!mPlacer.lockEnabled());
            log::info("{}: planar lock {}", mName, mPlacer.lockEnabled() ? "on" : "off");
        }
        else
            return false;
        viewportTools::requestRedraw();
        return true;
    }

    void finish(bool closed)
    {
        if (mPts.size() >= 2)
            createCurve(mBuild(mPts, closed), &mSurface);
        mPts.clear();
        mHistory.clear();
        mSurface.clearHits();
        mPlacer.unlock();
        viewportTools::requestRedraw();
    }

    const char* mName;
    const char* mHelp;
    /** The planar lock follows the first point through undo and redo. */
    void relock()
    {
        if (mPts.empty())
            mPlacer.unlock();
        else
            mPlacer.lockTo(mPts.front());
    }

    Builder mBuild;
    std::vector<D3> mPts;
    StepHistory<D3> mHistory;
    std::optional<D3> mHover;
    std::string mHoverView;
    Placer mPlacer;
    SurfaceDraw mSurface;
};

/** Bezier pen: click for corners, drag to pull out smooth handles, Ctrl while dragging to break them. */
class PenTool final : public ViewportTool
{
public:
    const char* name() const override { return "Bezier Pen"; }
    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }
    void activate() override
    {
        mSurface.load();
        mSurface.apply(mPlacer);
        log::info("Bezier Pen: click for corners, drag for smooth points, Ctrl while dragging breaks the handle");
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        if (mSpline.size() >= 2)
            if (auto first = view.project(mSpline.points.front()); first && distPx(*first, e->position()) < kClosePx)
            {
                mSpline.closed = true;
                mDragging = 0;
                mClosingDrag = true;
                mPressPx = e->position();
                return true;
            }
        D3 normal(0.0);
        scene::NodeId on = scene::kNoNode;
        auto p = mPlacer.place(e->position(), view, &normal, &on);
        if (!p)
            return true;
        mSurface.landed(on);
        if (mSpline.size() == 0)
        {
            mSpline.kind = curve::Kind::Bezier;
            mPlacer.lockTo(*p);
        }
        mSpline.points.push_back(*p);
        mSpline.handleL.push_back(*p);
        mSpline.handleR.push_back(*p);
        mSpline.typeL.push_back(curve::HandleType::Vector);
        mSpline.typeR.push_back(curve::HandleType::Vector);
        curve::updateHandles(mSpline);
        mHistory.clear();
        mDragging = mSpline.size();
        mDragNormal = glm::length(normal) > 0.5 ? normal : mPlacer.plane(view).second;
        mPressPx = e->position();
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHoverView = view.camera;
        if (mClosingDrag || mDragging)
        {
            size_t i = mClosingDrag ? 0 : mDragging - 1;
            if (distPx(e->position(), mPressPx) < 3.0)
                return true;
            D3 a = mSpline.points[i];
            D3 o, d;
            view.ray(e->position(), o, d);
            double den = glm::dot(d, mDragNormal);
            if (std::abs(den) > 1e-9)
            {
                D3 w = o + d * (glm::dot(a - o, mDragNormal) / den);
                bool brk = e->modifiers() & Qt::ControlModifier;
                // Dragging back onto the first point to close pulls its incoming handle, mirrored to keep the curve heading on.
                if (mClosingDrag)
                {
                    mSpline.handleL[i] = a - (w - a);
                    mSpline.typeL[i] = brk ? curve::HandleType::Free : curve::HandleType::Aligned;
                    if (!brk)
                    {
                        mSpline.handleR[i] = w;
                        mSpline.typeR[i] = curve::HandleType::Aligned;
                    }
                }
                else
                {
                    mSpline.handleR[i] = w;
                    mSpline.typeR[i] = brk ? curve::HandleType::Free : curve::HandleType::Aligned;
                    if (!brk)
                    {
                        mSpline.handleL[i] = a - (w - a);
                        mSpline.typeL[i] = curve::HandleType::Aligned;
                    }
                    else if (mSpline.typeL[i] == curve::HandleType::Vector)
                        mSpline.typeL[i] = curve::HandleType::Free;
                }
                curve::updateHandles(mSpline);
            }
            viewportTools::requestRedraw();
            return true;
        }
        mHover = mPlacer.place(e->position(), view);
        viewportTools::requestRedraw();
        return false;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        bool closing = mClosingDrag;
        mDragging = 0;
        mClosingDrag = false;
        if (closing)
            finish();
        viewportTools::requestRedraw();
        return true;
    }

    bool wheel(QWheelEvent* e, const ViewInfo&) override
    {
        if (!mSurface.wheel(e))
            return false;
        mSurface.apply(mPlacer);
        viewportTools::requestRedraw();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (toolKey(e->key()))
            return true;
        switch (e->key())
        {
        case Qt::Key_Return:
        case Qt::Key_Enter: finish(); return true;
        case Qt::Key_Backspace:
            stepBack();
            viewportTools::requestRedraw();
            return true;
        case Qt::Key_Escape:
            if (mSpline.size() == 0)
                exitTool();
            mSpline = {};
            mHistory.clear();
            mSurface.clearHits();
            mPlacer.unlock();
            viewportTools::requestRedraw();
            return true;
        default: return false;
        }
    }

    bool hotkey(const QKeyEvent& e, std::string_view action) override
    {
        if (plainKey(e) && toolKey(e.key()))
            return true;
        switch (mHistory.route(e.key(), action))
        {
        case StepKey::Undo: stepBack(); return true;
        case StepKey::Redo: stepForward(); return true;
        case StepKey::None: return false;
        }
        return false;
    }

    void draw(render::FrameState& f, OverlayPainter& p, const ViewInfo& view) override
    {
        Spline preview = mSpline;
        if (mHover && !mDragging && !mClosingDrag && view.camera == mHoverView && preview.size() && !preview.closed)
        {
            preview.points.push_back(*mHover);
            preview.handleL.push_back(*mHover);
            preview.handleR.push_back(*mHover);
            preview.typeL.push_back(curve::HandleType::Vector);
            preview.typeR.push_back(curve::HandleType::Vector);
            curve::updateHandles(preview);
        }
        if (preview.size() >= 2)
            drawSymmetric(f, preview, kPreview);
        for (size_t i = 0; i < mSpline.size(); ++i)
        {
            for (const D3* h : {&mSpline.handleL[i], &mSpline.handleR[i]})
                if (glm::distance(*h, mSpline.points[i]) > 1e-9)
                {
                    f.linesOnTop.push_back({glm::dvec3(mSpline.points[i]), kGhost});
                    f.linesOnTop.push_back({glm::dvec3(*h), kGhost});
                    if (auto q = view.project(*h))
                        p.rect(QRectF(*q - QPointF(3, 3), QSizeF(6, 6)), kDot);
                }
            dot(p, view, mSpline.points[i]);
        }
        ToolStatus status{.tool = "Bezier Pen",
                          .flags = {{"planar lock", mPlacer.lockEnabled()}},
                          .prompt = "click a corner, drag a smooth point",
                          .keys = {{"Ctrl", "break handle"}, {"Enter", "finish"}, {"First point", "close"}, {"Z", "undo point"}, {"Shift+Z", "redo point"},
                                   {"L", "planar lock"}}};
        mSurface.addStatus(status);
        status.keys.push_back({"Esc", "exit"});
        drawToolStatus(p, view, status);
    }

private:
    /** S, K and L, ahead of the app's hotkeys on them. */
    bool toolKey(int key)
    {
        if (mSurface.key(key))
            mSurface.apply(mPlacer);
        else if (key == Qt::Key_L)
            mPlacer.toggleLock(!mPlacer.lockEnabled());
        else
            return false;
        viewportTools::requestRedraw();
        return true;
    }

    void finish()
    {
        if (mSpline.size() >= 2)
            createCurve(mSpline, &mSurface);
        mSpline = {};
        mHistory.clear();
        mSurface.clearHits();
        mPlacer.unlock();
        viewportTools::requestRedraw();
    }

    /** An anchor with its handles, as undo takes it off the spline. */
    struct Anchor
    {
        D3 point, handleL, handleR;
        curve::HandleType typeL, typeR;
    };

    void stepBack()
    {
        if (!mSpline.size())
            return;
        const size_t i = mSpline.size() - 1;
        mHistory.undone({mSpline.points[i], mSpline.handleL[i], mSpline.handleR[i], mSpline.typeL[i], mSpline.typeR[i]});
        mSpline.erasePoint(i);
        mDragging = 0;
        mClosingDrag = false;
        if (mSpline.size())
            curve::updateHandles(mSpline);
        else
            mPlacer.unlock();
    }

    void stepForward()
    {
        std::optional<Anchor> a = mHistory.take();
        if (!a)
            return;
        if (mSpline.size() == 0)
        {
            mSpline.kind = curve::Kind::Bezier;
            mPlacer.lockTo(a->point);
        }
        mSpline.points.push_back(a->point);
        mSpline.handleL.push_back(a->handleL);
        mSpline.handleR.push_back(a->handleR);
        mSpline.typeL.push_back(a->typeL);
        mSpline.typeR.push_back(a->typeR);
        curve::updateHandles(mSpline);
    }

    Spline mSpline;
    StepHistory<Anchor> mHistory;
    size_t mDragging = 0; // 1-based index of the anchor whose handles are being pulled
    bool mClosingDrag = false;
    QPointF mPressPx;
    D3 mDragNormal{0, 1, 0};
    std::optional<D3> mHover;
    std::string mHoverView;
    Placer mPlacer;
    SurfaceDraw mSurface;
};

/** Pencil: draw a stroke, get a Bezier fitted to it (corners kept). Ending on the start closes it. */
class PencilTool final : public ViewportTool
{
public:
    const char* name() const override { return "Pencil Curve"; }
    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }
    void activate() override
    {
        mSurface.load();
        mSurface.apply(mPlacer);
        log::info("Pencil Curve: draw a stroke; it is fitted with a Bezier, corners kept");
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        mStroke.clear();
        mPx.clear();
        mSurface.clearHits();
        scene::NodeId on = scene::kNoNode;
        if (auto p = mPlacer.place(e->position(), view, nullptr, &on))
        {
            mPlacer.lockTo(*p);
            mStroke.push_back(*p);
            mPx.push_back(e->position());
            mUpp = view.unitsPerPixel(*p);
            mSurface.landed(on);
        }
        mDrawing = true;
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        if (!mDrawing)
            return false;
        if (!mPx.empty() && distPx(mPx.back(), e->position()) < 3.0)
            return true;
        scene::NodeId on = scene::kNoNode;
        if (auto p = mPlacer.place(e->position(), view, nullptr, &on))
        {
            mStroke.push_back(*p);
            mPx.push_back(e->position());
            mSurface.landed(on);
        }
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (e->button() != Qt::LeftButton || !mDrawing)
            return false;
        mDrawing = false;
        if (mStroke.size() >= 2)
        {
            bool closed = mStroke.size() > 6 && distPx(mPx.front(), mPx.back()) < 12.0;
            double tolPx = std::max(0.5, prefs().value<double>("general", "/curveTools/pencilTolerance", 2.0));
            createCurve(curve::fitBezier(mStroke, closed, tolPx * mUpp), &mSurface);
        }
        mStroke.clear();
        mPx.clear();
        mPlacer.unlock();
        viewportTools::requestRedraw();
        return true;
    }

    bool wheel(QWheelEvent* e, const ViewInfo&) override
    {
        if (!mSurface.wheel(e))
            return false;
        mSurface.apply(mPlacer);
        viewportTools::requestRedraw();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (toolKey(e->key()))
            return true;
        if (e->key() != Qt::Key_Escape)
            return false;
        exitTool();
        return true;
    }

    bool hotkey(const QKeyEvent& e, std::string_view) override { return plainKey(e) && toolKey(e.key()); }

    void draw(render::FrameState& f, OverlayPainter& p, const ViewInfo& view) override
    {
        for (size_t i = 1; i < mStroke.size(); ++i)
        {
            f.linesOnTop.push_back({glm::dvec3(mStroke[i - 1]), kPreview});
            f.linesOnTop.push_back({glm::dvec3(mStroke[i]), kPreview});
            if (symmetryPref())
            {
                f.linesOnTop.push_back({glm::dvec3(mirrored(mStroke[i - 1])), kPreview});
                f.linesOnTop.push_back({glm::dvec3(mirrored(mStroke[i])), kPreview});
            }
        }
        ToolStatus status{.tool = "Pencil Curve", .prompt = "draw a stroke, end on its start to close"};
        mSurface.addStatus(status);
        status.keys.push_back({"Esc", "exit"});
        drawToolStatus(p, view, status);
    }

private:
    /** S and K, ahead of the app's hotkeys on them. */
    bool toolKey(int key)
    {
        if (!mSurface.key(key))
            return false;
        mSurface.apply(mPlacer);
        viewportTools::requestRedraw();
        return true;
    }

    std::vector<D3> mStroke;
    std::vector<QPointF> mPx;
    double mUpp = 0.1;
    bool mDrawing = false;
    Placer mPlacer;
    SurfaceDraw mSurface;
};

Spline cvBuild(const std::vector<D3>& pts, bool closed)
{
    Spline s;
    s.kind = curve::Kind::Nurbs;
    s.points = pts;
    s.closed = closed && pts.size() >= 3;
    s.degree = std::min(degreePref(), std::max(1, int(pts.size()) - (s.closed ? 0 : 1)));
    if (s.closed)
        s.degree = std::min(s.degree, int(pts.size()));
    return s;
}

void tool(std::string id, QString label, std::string icon, std::string doc, std::function<std::unique_ptr<ViewportTool>()> make)
{
    if (!viewportTools::registry().find(id))
        viewportTools::add({.id = id, .label = label, .icon = icon, .create = make});
    actions::add({.id = id, .label = label.toStdString(), .category = "Curves", .run = [id] { viewportTools::activate(id); }, .doc = std::move(doc),
                  .python = "lila.ui.run_action(\"" + id + "\")"});
}
} // namespace

void registerCurveTools()
{
    tool("curve.bezierPen", "Bezier Pen", "vector-bezier", "Draws a Bezier curve: click for corners, drag for smooth points, Ctrl while dragging breaks the handle.",
         [] { return std::make_unique<PenTool>(); });
    tool("curve.cvTool", "CV Curve", "vector-bezier-2", "Draws a NURBS curve by placing its control points (degree from /curveTools/degree).",
         [] { return std::make_unique<ClickCurveTool>("CV Curve", "click to place control points", cvBuild); });
    tool("curve.epTool", "EP Curve", "vector-spline", "Draws a NURBS curve through the points you place (its edit points).", [] {
        return std::make_unique<ClickCurveTool>("EP Curve", "click points the curve passes through",
                                                [](const std::vector<D3>& p, bool c) { return curve::interpolate(p, degreePref(), c); });
    });
    tool("curve.pencil", "Pencil Curve", "scribble", "Draws a stroke and fits a Bezier to it, corners kept.", [] { return std::make_unique<PencilTool>(); });
    tool("curve.catmullRom", "Catmull-Rom Curve", "route", "Draws a smooth curve through the points you place (centripetal Catmull-Rom, no loops or cusps).", [] {
        return std::make_unique<ClickCurveTool>("Catmull-Rom Curve", "click points the curve passes through",
                                                [](const std::vector<D3>& p, bool c) { return curve::catmullRom(p, c); });
    });
    actions::add({.id = "curve.drawOnSurfaces",
                  .label = "Draw on Surfaces",
                  .category = "Curves",
                  .run = [] { curveTools::prefs().set("general", "/curveTools/onSurface", !curveTools::SurfaceDraw::onPref()); },
                  .doc = "The curve draw tools place points on the mesh under the cursor (S in the tool), lifted by the surface offset "
                         "(Ctrl+scroll in the tool).",
                  .python = "lila.prefs.set(\"general\", \"/curveTools/onSurface\", True)",
                  .checked = [] { return curveTools::SurfaceDraw::onPref(); }});
    actions::add({.id = "curve.keepDrawnOnSurface",
                  .label = "Keep Drawn Curves on Surface",
                  .category = "Curves",
                  .run = [] { curveTools::prefs().set("general", "/curveTools/conform", !curveTools::SurfaceDraw::conformPref()); },
                  .doc = "Curves drawn on a surface stay live on it, so moving their points later slides them along it (K in the tool).",
                  .python = "lila.prefs.set(\"general\", \"/curveTools/conform\", True)",
                  .checked = [] { return curveTools::SurfaceDraw::conformPref(); }});
    tool("curve.kappa", "Kappa Curve", "ease-in-out", "Draws a curve whose tightest bends land exactly on the points you place (kappa curve).", [] {
        return std::make_unique<ClickCurveTool>("Kappa Curve", "click where the curve bends most",
                                                [](const std::vector<D3>& p, bool c) { return curve::kappaCurve(p, c); });
    });
}
} // namespace lila::ui
