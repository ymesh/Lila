#include "ui/CurveUi.h"
#include "ui/CurvePick.h"
#include "ui/CurveToolsCommon.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "curve/Blend.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportToolMath.h"

#include <memory>

namespace lila::ui
{
namespace
{
using namespace curveTools;
using curvePick::SceneCurve;
using curve::Continuity;
using curve::Geometry;

const glm::vec4 kHandle(1.0f, 0.78f, 0.3f, 1.0f);
const glm::vec4 kCombColor(0.55f, 0.75f, 1.0f, 1.0f);
constexpr double kEndPx = 20.0;
constexpr double kHandlePx = kHandleGrabPx;

/** One end of the blend: a curve's end, a point along a curve, or a point on a mesh border edge. */
struct Pick
{
    bool border = false;
    std::string path;
    size_t spline = 0;
    curve::Spline s;
    int end = 1; // 0 start, 1 end, 2 at u
    double u = 0.0;
    std::shared_ptr<geo::Mesh> mesh;
    geo::Index edge = geo::kInvalid;
    double t = 0.5;
    D3 point{0.0};
    bool flip = false;
    Continuity continuity = Continuity::G2;
    double bulge = 1.0;
};

/**
 * Blend Curve: click two curve ends (or points along curves, or a mesh border edge) and a blend bridges them, with its
 * curvature comb. Drag the handle dots to set each end's bulge; 0 to 3 set the continuity (G0 to G3) of the end nearest
 * the cursor and F flips it; Enter builds it with live history, so it follows the curves when they move.
 */
class BlendCurveTool final : public ViewportTool
{
public:
    const char* name() const override { return "Blend Curve"; }
    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

    void activate() override
    {
        mCurves = curvePick::visibleCurves();
        mComb = prefs().value<bool>("general", "/blendCurve/comb", true);
        pickSelection();
        log::info("Blend Curve: click two curve ends (or a curve and a mesh border), drag the dots for bulge, 0-3 continuity, Enter builds it");
    }

    void deactivate() override { viewportTools::requestRedraw(); }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        if (mPicks.size() == 2)
        {
            if (std::optional<int> h = handleAt(e->position(), view))
            {
                mDrag = *h;
                return true;
            }
            return true;
        }
        if (std::optional<Pick> p = candidate(e->position(), view))
        {
            Pick q = *p;
            q.continuity = defaultContinuity();
            mPicks.push_back(std::move(q));
            rebuild();
        }
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mCursor = e->position();
        mHoverView = view.camera;
        if (mDrag >= 0 && mBlend)
        {
            dragBulge(e->position(), view);
            return true;
        }
        mCandidate = mPicks.size() < 2 ? candidate(e->position(), view) : std::nullopt;
        viewportTools::requestRedraw();
        return false;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (e->button() != Qt::LeftButton || mDrag < 0)
            return false;
        mDrag = -1;
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo& view) override
    {
        int k = e->key();
        if (k >= Qt::Key_0 && k <= Qt::Key_3)
        {
            if (Pick* p = nearestPick(view))
            {
                p->continuity = Continuity(k - Qt::Key_0);
                prefs().set("general", "/blendCurve/continuity", k - Qt::Key_0);
                rebuild();
            }
            viewportTools::requestRedraw();
            return true;
        }
        switch (k)
        {
        case Qt::Key_F:
            if (Pick* p = nearestPick(view))
            {
                p->flip = !p->flip;
                rebuild();
            }
            break;
        case Qt::Key_C:
            mComb = !mComb;
            prefs().set("general", "/blendCurve/comb", mComb);
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter: commit(); break;
        case Qt::Key_Backspace:
            if (!mPicks.empty())
                mPicks.pop_back();
            rebuild();
            break;
        case Qt::Key_Escape:
            if (mPicks.empty())
            {
                exitTool();
                return true;
            }
            mPicks.clear();
            rebuild();
            break;
        default: return false;
        }
        viewportTools::requestRedraw();
        return true;
    }

    void draw(render::FrameState& f, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();

        for (const Pick& pk : mPicks)
            dot(p, view, pk.point, 4.5f, kDot);
        if (mCandidate && view.camera == mHoverView)
            dot(p, view, mCandidate->point, 3.5f, kGhost);
        if (mBlend)
        {
            drawSpline(f, *mBlend, kPreview);
            if (mComb)
                drawComb(f);
            for (int side = 0; side < 2; ++side)
                if (std::optional<D3> h = handlePoint(side))
                {
                    f.linesOnTop.push_back({glm::dvec3(mPicks[size_t(side)].point), kHandle});
                    f.linesOnTop.push_back({glm::dvec3(*h), kHandle});
                    dot(p, view, *h, 4.0f, kHandle);
                }
        }
        ToolStatus status{.tool = "Blend Curve", .flags = {{"comb", mComb}}};
        if (mPicks.size() < 2)
            status.prompt = mPicks.empty() ? "click a curve end, curve point or border edge" : "click the second end";
        else if (mError.isEmpty())
            status.fields = {{"first", curve::continuityName(mPicks[0].continuity)},
                             {"first bulge", QString::number(mPicks[0].bulge, 'f', 2)},
                             {"second", curve::continuityName(mPicks[1].continuity)},
                             {"second bulge", QString::number(mPicks[1].bulge, 'f', 2)}};
        status.keys = {{"Dot-drag", "bulge"}, {"0-3", "nearer end continuity"}, {"F", "flip"}, {"C", "comb"}, {"Enter", "build"},
                       {"Backspace", "undo pick"}, {"Esc", mPicks.empty() ? "exit" : "clear"}};
        drawToolStatus(p, view, status);
        if (mPicks.size() >= 2 && !mError.isEmpty())
            drawToolMessage(p, view, mError);
    }

private:
    Continuity defaultContinuity() const { return Continuity(std::clamp(prefs().value<int>("general", "/blendCurve/continuity", 2), 0, 3)); }

    /** What a click would pick: a curve end within reach, else the point along the curve, else a mesh border edge. */
    std::optional<Pick> candidate(QPointF px, const ViewInfo& view) const
    {
        if (auto h = curvePick::nearestCurve(mCurves, px, view, kSnapPx))
        {
            const SceneCurve& c = mCurves[h->curve];
            Pick p;
            p.path = c.path;
            p.spline = c.spline;
            p.s = c.s;
            p.end = 2;
            p.u = h->u;
            p.point = h->point;
            if (!c.s.closed)
                for (int e = 0; e < 2; ++e)
                {
                    D3 at = curve::pointAt(c.s, e ? curve::spanCount(c.s) : 0.0);
                    if (auto q = view.project(at); q && distPx(*q, px) < kEndPx)
                    {
                        p.end = e;
                        p.point = at;
                        break;
                    }
                }
            return p;
        }
        if (auto b = curvePick::nearestBorder(px, view, kSnapPx))
        {
            std::optional<geo::Mesh> m = curvePick::worldMesh(b->mesh);
            if (!m)
                return std::nullopt;
            Pick p;
            p.border = true;
            p.path = b->path;
            p.mesh = std::make_shared<geo::Mesh>(std::move(*m));
            p.edge = b->edge;
            p.t = b->t;
            p.point = b->point;
            return p;
        }
        return std::nullopt;
    }

    /** Two selected curves start the tool with their nearest ends picked. */
    void pickSelection()
    {
        scene::Graph& g = scene::Graph::current();
        std::vector<const SceneCurve*> sel;
        for (scene::NodeId id : g.selection())
            for (const SceneCurve& c : mCurves)
                if (c.node == id && c.spline == 0 && !c.s.closed)
                    sel.push_back(&c);
        if (sel.size() != 2)
            return;
        double best = std::numeric_limits<double>::max();
        int ea = 1, eb = 0;
        for (int a = 0; a < 2; ++a)
            for (int b = 0; b < 2; ++b)
            {
                double d = glm::distance(curve::pointAt(sel[0]->s, a ? curve::spanCount(sel[0]->s) : 0.0),
                                         curve::pointAt(sel[1]->s, b ? curve::spanCount(sel[1]->s) : 0.0));
                if (d < best)
                {
                    best = d;
                    ea = a;
                    eb = b;
                }
            }
        for (int k = 0; k < 2; ++k)
        {
            Pick p;
            p.path = sel[size_t(k)]->path;
            p.s = sel[size_t(k)]->s;
            p.end = k == 0 ? ea : eb;
            p.point = curve::pointAt(p.s, p.end ? curve::spanCount(p.s) : 0.0);
            p.continuity = defaultContinuity();
            mPicks.push_back(std::move(p));
        }
        rebuild();
    }

    Geometry geometryOf(const Pick& p, const D3& toward) const
    {
        Geometry g;
        if (p.border)
        {
            std::optional<Geometry> b = curve::borderGeometry(*p.mesh, p.edge, p.t);
            g = b ? *b : Geometry{p.point};
        }
        else if (p.end < 2)
            g = curve::endGeometry(p.s, p.end == 1);
        else
            g = curve::facing(curve::geometryAt(p.s, p.u, true), toward - p.point);
        return p.flip ? curve::reversed(g) : g;
    }

    void rebuild()
    {
        mBlend.reset();
        mError.clear();
        if (mPicks.size() < 2)
            return;
        if (mPicks[0].border && mPicks[1].border)
        {
            mError = "one end must be a curve";
            return;
        }
        try
        {
            curve::BlendEnd a{geometryOf(mPicks[0], mPicks[1].point), mPicks[0].continuity, mPicks[0].bulge};
            curve::BlendEnd b{geometryOf(mPicks[1], mPicks[0].point), mPicks[1].continuity, mPicks[1].bulge};
            mBlend = curve::blendCurve(a, b);
        }
        catch (const std::exception& e)
        {
            mError = QString::fromStdString(e.what());
        }
    }

    int degree() const { return !mBlend ? 0 : mBlend->kind == curve::Kind::Nurbs ? mBlend->degree : mBlend->kind == curve::Kind::Bezier ? 3 : 1; }

    /** The control point next to an end: where its bulge handle sits. */
    std::optional<D3> handlePoint(int side) const
    {
        if (!mBlend || mPicks[size_t(side)].continuity == Continuity::G0)
            return std::nullopt;
        const curve::Spline& s = *mBlend;
        if (s.kind == curve::Kind::Bezier)
            return side == 0 ? s.handleR[0] : s.handleL[1];
        if (s.kind == curve::Kind::Nurbs && s.size() >= 3)
            return side == 0 ? s.points[1] : s.points[s.size() - 2];
        return std::nullopt;
    }

    std::optional<int> handleAt(QPointF px, const ViewInfo& view) const
    {
        for (int side = 0; side < 2; ++side)
            if (std::optional<D3> h = handlePoint(side))
                if (auto q = view.project(*h); q && distPx(*q, px) < kHandlePx)
                    return side;
        return std::nullopt;
    }

    Pick* nearestPick(const ViewInfo& view)
    {
        Pick* best = nullptr;
        double bd = std::numeric_limits<double>::max();
        for (int side = 0; side < int(mPicks.size()); ++side)
        {
            std::vector<D3> spots = {mPicks[size_t(side)].point};
            if (std::optional<D3> h = handlePoint(side))
                spots.push_back(*h);
            for (const D3& s : spots)
                if (auto q = view.project(s); q && distPx(*q, mCursor) < bd)
                {
                    bd = distPx(*q, mCursor);
                    best = &mPicks[size_t(side)];
                }
        }
        return best;
    }

    /** Slides the dragged handle along its end's tangent: bulge is its distance there in gaps, times the degree. */
    void dragBulge(QPointF px, const ViewInfo& view)
    {
        Pick& p = mPicks[size_t(mDrag)];
        Geometry g = geometryOf(p, mPicks[size_t(1 - mDrag)].point);
        double chord = glm::distance(mPicks[0].point, mPicks[1].point);
        if (glm::length(g.t) < 0.5 || chord < 1e-9)
            return;
        D3 o, d;
        view.ray(px, o, d);
        D3 r = p.point - o;
        double a = glm::dot(g.t, g.t), b = glm::dot(g.t, d), c = glm::dot(d, d), e = glm::dot(g.t, r), f2 = glm::dot(d, r);
        double den = a * c - b * b;
        if (std::abs(den) < 1e-12)
            return;
        double along = (b * f2 - c * e) / den;
        p.bulge = std::clamp(along * degree() / chord, 0.01, 10.0);
        rebuild();
        viewportTools::requestRedraw();
    }

    void drawComb(render::FrameState& f) const
    {
        const double chord = glm::distance(mPicks[0].point, mPicks[1].point);
        double most = 0.0;
        for (const curve::CombTooth& t : curve::curvatureComb(*mBlend, 24, 1.0))
            most = std::max(most, t.curvature);
        if (most < 1e-12)
            return;
        std::vector<curve::CombTooth> teeth = curve::curvatureComb(*mBlend, 24, 0.3 * chord / most);
        for (size_t i = 0; i < teeth.size(); ++i)
        {
            f.linesOnTop.push_back({glm::dvec3(teeth[i].base), kCombColor});
            f.linesOnTop.push_back({glm::dvec3(teeth[i].tip), kCombColor});
            if (i + 1 < teeth.size())
            {
                f.linesOnTop.push_back({glm::dvec3(teeth[i].tip), kCombColor});
                f.linesOnTop.push_back({glm::dvec3(teeth[i + 1].tip), kCombColor});
            }
        }
    }

    /** Builds the blend with the blendCurve command (live history); a border end always goes second. */
    void commit()
    {
        if (!mBlend || mPicks.size() < 2)
            return;
        const Pick* a = &mPicks[0];
        const Pick* b = &mPicks[1];
        bool swapped = a->border;
        if (swapped)
            std::swap(a, b);
        if (a->spline != 0 || (!b->border && b->spline != 0))
        {
            log::warn("Blend Curve: only a curve's first spline can be blended with history");
            return;
        }
        json call = {{"args", json::array({a->path})}};
        auto ends = [&](const Pick& p, const char* side) {
            std::string s(side);
            call["continuity" + s] = curve::continuityName(p.continuity);
            call["bulge" + s] = p.bulge;
            call["flip" + s] = p.flip;
            if (p.border)
                return;
            if (p.end == 2)
                call["parameter" + s] = p.u;
            else
                call["end" + s] = p.end == 1 ? "end" : "start";
        };
        ends(*a, "A");
        ends(*b, "B");
        if (b->border)
        {
            call["mesh"] = b->path;
            call["edge"] = b->edge;
            call["edgeT"] = b->t;
        }
        else
            call["args"].push_back(b->path);
        try
        {
            ops::execute("blendCurve", call, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
            return;
        }
        mPicks.clear();
        mBlend.reset();
        mCurves = curvePick::visibleCurves();
    }

    std::vector<SceneCurve> mCurves;
    std::vector<Pick> mPicks;
    std::optional<Pick> mCandidate;
    std::optional<curve::Spline> mBlend;
    QString mError;
    QPointF mCursor;
    std::string mHoverView;
    int mDrag = -1;
    bool mComb = true;
};
} // namespace

void registerBlendCurveTool()
{
    if (!viewportTools::registry().find("curve.blendTool"))
        viewportTools::add({.id = "curve.blendTool", .label = "Blend Curve", .icon = "vector-bezier-arc", .create = [] { return std::make_unique<BlendCurveTool>(); }});
    actions::add({.id = "curve.blendTool",
                  .label = "Blend Curve Tool",
                  .category = "Curves",
                  .run = [] { viewportTools::activate("curve.blendTool"); },
                  .doc = "Bridges two curve ends (or points along curves, or a curve and a mesh border) with a G0 to G3 blend: drag the dots for "
                         "bulge, 0-3 set continuity, Enter builds it live.",
                  .python = "lila.ui.run_action(\"curve.blendTool\")"});
}
} // namespace lila::ui
