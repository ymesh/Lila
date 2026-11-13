#include "ui/CurveUi.h"
#include "ui/CurveToolsCommon.h"
#include "ui/SketchUi.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "curve/CurveNodes.h"
#include "curve/SketchNode.h"
#include "curve/Tessellate.h"
#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/NumberExpr.h"
#include "ui/Theme.h"
#include "ui/ViewportTool.h"

#include <glm/gtc/constants.hpp>

#include <QApplication>
#include <QElapsedTimer>

#include <cctype>
#include <cmath>
#include <map>
#include <optional>

namespace lila::ui
{
namespace
{
using namespace curveTools;
namespace sk = curve::sketch;
using D2 = glm::dvec2;
using curve::Spline;
using curve::Splines;

constexpr double kPickPx = kHandleGrabPx;
constexpr double kInferPx = 10.0;
constexpr float kButton = 26.0f;

const glm::vec4 kFree(0.92f, 0.92f, 0.92f, 1.0f);
const glm::vec4 kPlate(0.08f, 0.08f, 0.08f, 0.88f);
const glm::vec4 kPlateSelected(0.93f, 0.93f, 0.93f, 0.95f);
const glm::vec4 kInk(0.9f, 0.9f, 0.9f, 1.0f);
const glm::vec4 kInkDark(0.08f, 0.08f, 0.08f, 1.0f);

glm::vec4 conflictInk() { return themeSrgb("statusError", glm::vec4(1.0f, 0.36f, 0.36f, 1.0f)); }
glm::vec4 accentInk() { return themeSrgb("accent", glm::vec4(0.92f, 0.92f, 0.92f, 1.0f)); }

bool autoConstraints() { return prefs().value<bool>("general", "/sketch/autoConstraints", true); }
void setAutoConstraints(bool on) { prefs().set("general", "/sketch/autoConstraints", on); }

enum class SketchMode
{
    Select,
    Line,
    Arc,
    Circle,
};

const char* modeName(SketchMode m)
{
    switch (m)
    {
    case SketchMode::Select: return "Select";
    case SketchMode::Line: return "Line";
    case SketchMode::Arc: return "Arc";
    case SketchMode::Circle: return "Circle";
    }
    return "";
}

/** A placed point while drawing, with what was inferred for it (refs to it use spline kNew). */
struct Placed
{
    D2 at{0.0};
    std::vector<sk::Constraint> constraints;
};

/** What a palette button does. */
struct Button
{
    enum Kind
    {
        Draw,
        Constrain,
        Dimension,
        Auto,
    } kind;
    SketchMode mode = SketchMode::Select;
    sk::Type type = sk::Type::Coincident;
    std::string key;
    QRectF rect;
};

/** Constraint keys while the tool is active (plain letters, no modifiers). */
const std::map<int, sk::Type>& constraintKeys()
{
    static const std::map<int, sk::Type> k = {
        {Qt::Key_C, sk::Type::Coincident}, {Qt::Key_H, sk::Type::Horizontal}, {Qt::Key_V, sk::Type::Vertical},
        {Qt::Key_P, sk::Type::Parallel},   {Qt::Key_Q, sk::Type::Perpendicular}, {Qt::Key_T, sk::Type::Tangent},
        {Qt::Key_E, sk::Type::Equal},      {Qt::Key_O, sk::Type::Concentric}, {Qt::Key_M, sk::Type::Midpoint},
        {Qt::Key_Y, sk::Type::Symmetric},  {Qt::Key_F, sk::Type::Fixed},      {Qt::Key_N, sk::Type::PointOnCurve},
        {Qt::Key_K, sk::Type::Collinear},
    };
    return k;
}

std::string keyOf(sk::Type t)
{
    for (auto& [key, type] : constraintKeys())
        if (type == t)
            return std::string(1, char(key));
    return {};
}

void runSketchCommand(const char* cmd, const json& call, QString* error)
{
    try
    {
        ops::execute(cmd, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        if (error)
            *error = QString::fromStdString(e.what());
    }
}

/**
 * Sketch constraints: draw lines, arcs and circles on the sketch plane with auto constraints, select points, lines and
 * curves to constrain or dimension, drag points with the constraints holding, and edit dimensions in place. Works on the
 * selected curve, or a new one when nothing is selected. Constraint glyphs sit beside the geometry; click one to select
 * it and Delete removes it.
 */
class SketchTool final : public ViewportTool
{
public:
    const char* name() const override { return "Sketch"; }
    std::optional<QCursor> cursor() const override { return mMode == SketchMode::Select ? std::nullopt : std::optional(QCursor(Qt::CrossCursor)); }

    void activate() override
    {
        scene::Graph& g = scene::Graph::current();
        for (scene::NodeId id : g.selection())
            if (scene::Node* n = g.node(id); n && n->type().isA("curve"))
            {
                mNode = id;
                break;
            }
        if (mNode == scene::kNoNode)
            for (auto& [id, ids] : scene::components::selected(scene::ComponentMode::Vertex))
                if (scene::Node* n = g.node(id); n && n->type().isA("curve"))
                {
                    mNode = id;
                    break;
                }
        mMode = mNode == scene::kNoNode ? SketchMode::Line : SketchMode::Select;
        log::info("Sketch: L line, A arc, R circle, S select; select geometry then a constraint key or D to dimension; double-click a "
                  "dimension to type its value");
    }

    void deactivate() override { viewportTools::requestRedraw(); }

    /** Applies a constraint to the selection; what the palette, keys and actions call. */
    void constrain(sk::Type t)
    {
        refresh();
        if (sk::isDimension(t))
        {
            dimension(t);
            return;
        }
        if (mNode == scene::kNoNode || mSel.empty())
        {
            flash(QString("%1: select points, lines or curves first").arg(sketchUi::label(t)));
            return;
        }
        json refs = json::array();
        for (const sk::Ref& r : mSel)
            refs.push_back(sk::refName(r));
        json call = planeArgs();
        call.update({{"args", {nodePath()}}, {"type", sk::typeName(t)}, {"refs", refs}});
        QString err;
        runSketchCommand("sketchConstrain", call, &err);
        if (err.isEmpty())
            mSel.clear();
        else
            flash(err);
        viewportTools::requestRedraw();
    }

    /** Dimensions the selection: by type, or the one that fits it (length, radius or diameter, distance, angle). */
    void dimension(std::optional<sk::Type> type = std::nullopt)
    {
        refresh();
        if (mNode == scene::kNoNode || mSel.empty())
        {
            flash("Dimension: select a line, circle, two points, a point and a line, or two lines");
            return;
        }
        sk::Type t = type.value_or(smartDimension());
        json refs = json::array();
        for (const sk::Ref& r : mSel)
            refs.push_back(sk::refName(r));
        try
        {
            json call = planeArgs();
            call.update({{"args", {nodePath()}}, {"type", sk::typeName(t)}, {"refs", refs}});
            json r = ops::execute("sketchDimension", call, CallSource::UI);
            mSel.clear();
            // Fusion style: a new dimension opens for typing straight away.
            startEdit(r["id"].get<uint32_t>());
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
            flash(QString::fromStdString(e.what()));
        }
        viewportTools::requestRedraw();
    }

    void toggleAuto()
    {
        setAutoConstraints(!autoConstraints());
        flash(QString("Auto constraints %1").arg(autoConstraints() ? "on" : "off"));
        viewportTools::requestRedraw();
    }

    void setMode(SketchMode m)
    {
        cancelDrawing();
        mMode = m;
        viewportTools::requestRedraw();
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        mView = view;
        refresh();
        if (e->button() != Qt::LeftButton)
            return false;
        const QPointF px = e->position();
        if (mEditId)
            commitEdit();
        if (int b = buttonAt(px); b >= 0)
        {
            press(mButtons[size_t(b)]);
            return true;
        }
        const bool dbl = mClick.isValid() && mClick.elapsed() < QApplication::doubleClickInterval() && distPx(px, mClickPx) < 5.0;
        mClick.start();
        mClickPx = px;
        if (mMode != SketchMode::Select)
        {
            place(px, view);
            return true;
        }
        Pick pick = pickAt(px, view, true);
        if (pick.kind == Pick::Label && dbl)
        {
            startEdit(pick.id);
            return true;
        }
        const bool add = e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
        if (pick.kind == Pick::Glyph || pick.kind == Pick::Label)
        {
            if (!add)
            {
                mSel.clear();
                mSelIds.clear();
            }
            toggle(mSelIds, pick.id);
        }
        else if (pick.kind == Pick::Geometry)
        {
            if (!add && std::ranges::find(mSel, pick.ref) == mSel.end())
            {
                mSel.clear();
                mSelIds.clear();
            }
            if (add)
                toggle(mSel, pick.ref);
            else if (std::ranges::find(mSel, pick.ref) == mSel.end())
                mSel.push_back(pick.ref);
            // A press on geometry may become a drag.
            mPressRef = pick.ref;
            mPressPx = px;
            mPressPlane = planeAt(px, view);
        }
        else if (pick.kind == Pick::OtherCurve)
        {
            mNode = pick.node;
            mSel.clear();
            mSelIds.clear();
            scene::Graph::current().select({mNode});
        }
        else if (!add)
        {
            mSel.clear();
            mSelIds.clear();
        }
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mView = view;
        mHover = e->position();
        mHoverView = view.camera;
        refresh();
        if (mPressRef && (e->buttons() & Qt::LeftButton))
        {
            if (!mDragging && distPx(e->position(), mPressPx) > 3.0)
                mDragging = true;
            if (mDragging)
                dragTo(e->position(), view);
            viewportTools::requestRedraw();
            return true;
        }
        if (mMode != SketchMode::Select)
            mInference = infer(e->position(), view);
        viewportTools::requestRedraw();
        return false;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo& view) override
    {
        mView = view;
        if (e->button() != Qt::LeftButton)
            return false;
        if (mDragging)
            commitDrag();
        mDragging = false;
        mPressRef.reset();
        mPreview.clear();
        viewportTools::requestRedraw();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo& view) override
    {
        mView = view;
        if (mEditId)
            return editKey(*e);
        if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
            return false;
        return toolKey(e->key(), e->modifiers());
    }

    bool hotkey(const QKeyEvent& e, std::string_view action) override
    {
        if (mEditId)
            return editKey(e);
        // Undo while drawing takes back the last placed point, not a scene step.
        if (action == "edit.undo" && !mPlaced.empty())
        {
            mPlaced.pop_back();
            viewportTools::requestRedraw();
            return true;
        }
        if (e.modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
            return false;
        return toolKey(e.key(), e.modifiers());
    }

    void draw(render::FrameState& f, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();

        mView = view;
        refresh();
        // Hover is picked against last frame's glyphs and labels, before they are laid out again.
        const Pick hover = mMode == SketchMode::Select && !mDragging && mHoverView == view.camera ? pickAt(mHover, view) : Pick{};
        mGlyphs.clear();
        mLabels.clear();
        const sketchUi::Display* disp = nodeOk() ? sketchUi::display(*node()) : nullptr;
        drawGeometry(f, p, view, disp, hover);
        if (disp)
        {
            drawGlyphs(p, view, *disp);
            drawDimensions(p, view, *disp);
        }
        drawPreview(f, p, view);
        drawPalette(p, view);
        drawMessage(p, view, disp);
    }

private:
    struct Pick
    {
        enum Kind
        {
            None,
            Geometry,
            Glyph,
            Label,
            OtherCurve,
        } kind = None;
        sk::Ref ref;
        uint32_t id = 0;
        scene::NodeId node = scene::kNoNode;
    };

    struct GlyphAt
    {
        uint32_t id;
        sk::Type type;
        QPointF at;
    };

    struct LabelAt
    {
        uint32_t id;
        QRectF rect;
    };

    // Target and coordinates

    scene::Node* node() const { return scene::Graph::current().node(mNode); }
    bool nodeOk() const
    {
        scene::Node* n = node();
        return n && n->type().isA("curve");
    }
    std::string nodePath() const { return nodeOk() ? node()->path() : std::string(); }

    /** The plane the tool shows, for commands on a curve that has no sketch yet (so theirs matches). */
    json planeArgs() const
    {
        if (nodeOk() && curve::hasSketch(*node()))
            return json::object();
        const D3 n = mPlane.normal();
        return {{"planeNormal", {n.x, n.y, n.z}}, {"planeOrigin", {mPlane.origin.x, mPlane.origin.y, mPlane.origin.z}}};
    }

    /** Re-reads the target's splines, world matrix and plane. */
    void refresh()
    {
        if (!nodeOk())
        {
            mNode = scene::kNoNode;
            mSplines.clear();
            mWorld = glm::dmat4(1.0);
            if (!mPlaneLocked)
            {
                auto [origin, normal] = mPlacer.plane(mView);
                mPlane = sk::planeFrom(origin, normal);
            }
            return;
        }
        scene::Graph& g = scene::Graph::current();
        mSplines = curve::shapeSplines(g, mNode);
        mWorld = node()->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        const std::string& text = node()->as<std::string>(node()->type().attrIndex("sketch"));
        if (text != mSketchText || !mPlaneLocked)
        {
            mSketchText = text;
            if (curve::hasSketch(*node()))
                mPlane = curve::sketchOf(*node()).plane;
            else
            {
                const glm::dmat4 inv = glm::inverse(mWorld);
                const D3 normal = glm::dvec3(inv * glm::dvec4(mPlacer.plane(mView).second, 0.0));
                mPlane = sk::fitPlane(mSplines, glm::length(normal) > 1e-12 ? glm::normalize(normal) : D3(0, 1, 0));
            }
            mPlaneLocked = true;
        }
    }

    D3 toWorld(const D3& p) const { return D3(mWorld * glm::dvec4(p, 1.0)); }
    std::optional<QPointF> screen(const D3& local, const ViewInfo& view) const { return view.project(toWorld(local)); }
    std::optional<QPointF> screen2(const D2& p, const ViewInfo& view) const { return screen(mPlane.to3d(p), view); }

    /** The sketch-plane point under a pixel. */
    std::optional<D2> planeAt(QPointF px, const ViewInfo& view) const
    {
        D3 o, d;
        view.ray(px, o, d);
        const glm::dmat4 inv = glm::inverse(mWorld);
        const D3 lo = D3(inv * glm::dvec4(o, 1.0)), ld = D3(inv * glm::dvec4(d, 0.0));
        const D3 n = mPlane.normal();
        const double den = glm::dot(ld, n);
        if (std::abs(den) < 1e-12)
            return std::nullopt;
        const double t = glm::dot(mPlane.origin - lo, n) / den;
        if (!view.ortho && t < 0.0)
            return std::nullopt;
        return mPlane.to2d(lo + ld * t);
    }

    /** Plane units per screen pixel around a plane point. */
    double unitsPerPixel(const D2& at, const ViewInfo& view) const
    {
        const double scale = std::max(1e-12, std::cbrt(std::abs(glm::determinant(glm::dmat3(mWorld)))));
        return view.unitsPerPixel(toWorld(mPlane.to3d(at))) / scale;
    }

    // Drawing new geometry

    /** The splines with the line being drawn as the last one, so inference sees its own points. */
    Splines withChain() const
    {
        Splines s = mSplines;
        if (mMode == SketchMode::Line && mPlaced.size() >= 2)
        {
            Spline c;
            c.kind = curve::Kind::Poly;
            for (const Placed& pl : mPlaced)
                c.points.push_back(mPlane.to3d(pl.at));
            c.normalize();
            s.push_back(c);
        }
        return s;
    }

    std::optional<sk::Inference> infer(QPointF px, const ViewInfo& view) const
    {
        std::optional<D2> at = planeAt(px, view);
        if (!at)
            return std::nullopt;
        if (!autoConstraints() || QApplication::keyboardModifiers() & Qt::AltModifier)
            return sk::Inference{*at, {}};
        const double tol = kInferPx * unitsPerPixel(*at, view);
        std::optional<D2> prev;
        if (mMode == SketchMode::Line && !mPlaced.empty())
            prev = mPlaced.back().at;
        sk::Inference inf = sk::infer(withChain(), mPlane, *at, prev, tol);
        // A chain with one point has no spline yet: closing onto it isn't possible, but snapping to it is.
        if (mMode == SketchMode::Line && mPlaced.size() == 1 && inf.constraints.empty() && glm::distance(mPlaced[0].at, *at) < tol)
            inf.at = mPlaced[0].at;
        return inf;
    }

    void place(QPointF px, const ViewInfo& view)
    {
        std::optional<sk::Inference> inf = infer(px, view);
        if (!inf)
            return;
        Placed p{inf->at, inf->constraints};
        // The first point of a new sketch fixes its plane, whatever the view does next.
        if (!nodeOk())
            mPlaneLocked = true;
        switch (mMode)
        {
        case SketchMode::Line:
        {
            const int chain = int(mSplines.size());
            // Clicking the chain's first point closes it.
            for (const sk::Constraint& c : p.constraints)
                if (c.type == sk::Type::Coincident && c.refs.size() == 2 && c.refs[1].spline == chain && c.refs[1].index == 0 && mPlaced.size() >= 3)
                {
                    mClosing = p.constraints;
                    commitLine(true);
                    return;
                }
            if (!mPlaced.empty() && glm::distance(mPlaced.back().at, p.at) < 1e-9)
                return;
            mPlaced.push_back(std::move(p));
            break;
        }
        case SketchMode::Circle:
            mPlaced.push_back(std::move(p));
            if (mPlaced.size() == 2)
                commitCircle();
            break;
        case SketchMode::Arc:
            mPlaced.push_back(std::move(p));
            if (mPlaced.size() == 3)
                commitArc();
            break;
        default: break;
        }
        viewportTools::requestRedraw();
    }

    /** Constraint refs from a placed point to the new spline: kNew point is point i, kNew line the leg ending there. */
    static json mapped(const sk::Constraint& c, int spline, int pointIndex, bool centre)
    {
        json refs = json::array();
        sk::Type type = c.type;
        for (sk::Ref r : c.refs)
        {
            if (r.spline == sk::kNew)
            {
                if (r.kind == sk::RefKind::Line)
                    r = {sk::RefKind::Line, spline, pointIndex - 1};
                else
                    r = centre ? sk::Ref{sk::RefKind::Center, spline, 0} : sk::Ref{sk::RefKind::Point, spline, pointIndex};
            }
            refs.push_back(sk::refName(r));
        }
        return {{"type", sk::typeName(type)}, {"refs", refs}};
    }

    void commit(const Spline& s, const json& constraints)
    {
        json call = planeArgs();
        call.update({{"args", {nodePath()}}, {"splines", json::array({curve::splineToJson(s)})}, {"constraints", constraints}});
        try
        {
            json r = ops::execute("sketchAdd", call, CallSource::UI);
            if (scene::Node* n = scene::Graph::current().find(r["curve"].get<std::string>()))
                mNode = n->id();
            mPlaneLocked = false;
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
            flash(QString::fromStdString(e.what()));
        }
        refresh();
    }

    void commitLine(bool closed)
    {
        if (mPlaced.size() < 2)
        {
            mPlaced.clear();
            return;
        }
        const int si = int(mSplines.size());
        Spline s;
        s.kind = curve::Kind::Poly;
        s.closed = closed && mPlaced.size() >= 3;
        json cons = json::array();
        for (size_t i = 0; i < mPlaced.size(); ++i)
        {
            s.points.push_back(mPlane.to3d(mPlaced[i].at));
            for (const sk::Constraint& c : mPlaced[i].constraints)
            {
                // Inferred against the chain itself: refs to the drawn spline already use its index.
                if (c.type == sk::Type::Coincident && c.refs[1].spline == si && size_t(c.refs[1].index) == i)
                    continue;
                if (c.refs[0].kind == sk::RefKind::Line && i == 0)
                    continue;
                cons.push_back(mapped(c, si, int(i), false));
            }
        }
        if (s.closed)
        {
            // The closing leg ends on point 0, so what the closing click inferred for its line goes to the last leg.
            for (const sk::Constraint& c : mClosing)
                if (c.refs.size() == 1 && c.refs[0].kind == sk::RefKind::Line)
                    cons.push_back({{"type", sk::typeName(c.type)}, {"refs", {sk::refName({sk::RefKind::Line, si, int(mPlaced.size()) - 1})}}});
        }
        s.normalize();
        commit(s, cons);
        mPlaced.clear();
        mClosing.clear();
    }

    void commitCircle()
    {
        const D2 c = mPlaced[0].at;
        const double r = glm::distance(c, mPlaced[1].at);
        if (r < 1e-6)
        {
            mPlaced.pop_back();
            return;
        }
        const int si = int(mSplines.size());
        Spline s = curve::circle(mPlane.to3d(c), mPlane.normal(), r);
        json cons = json::array();
        for (const sk::Constraint& k : mPlaced[0].constraints)
        {
            // A centre on a centre is concentric; on a point, coincident; on a curve, point on curve.
            if (k.type == sk::Type::Coincident && k.refs[1].kind == sk::RefKind::Center)
                cons.push_back({{"type", "concentric"}, {"refs", {sk::refName({sk::RefKind::Curve, si, 0}), sk::refName({sk::RefKind::Curve, k.refs[1].spline, 0})}}});
            else
                cons.push_back(mapped(k, si, 0, true));
        }
        for (const sk::Constraint& k : mPlaced[1].constraints)
            if (k.type == sk::Type::Coincident && k.refs[1].isPointLike())
                cons.push_back({{"type", "pointOnCurve"}, {"refs", {sk::refName(k.refs[1]), sk::refName({sk::RefKind::Curve, si, 0})}}});
        commit(s, cons);
        mPlaced.clear();
    }

    /** The line end a placed point snapped onto, and the direction leaving the line there (for tangent arcs). */
    std::optional<std::pair<sk::Ref, D2>> lineEndAt(const Placed& p) const
    {
        for (const sk::Constraint& c : p.constraints)
        {
            if (c.type != sk::Type::Coincident || c.refs[1].kind != sk::RefKind::Point)
                continue;
            const sk::Ref end = sk::normalized(mSplines, c.refs[1]);
            if (end.spline < 0 || size_t(end.spline) >= mSplines.size())
                continue;
            const Spline& sp = mSplines[size_t(end.spline)];
            if (sk::circleOf(sp, mPlane).shape != sk::Shape::Points || sp.closed || sp.size() < 2)
                continue;
            if (end.index != 0 && size_t(end.index) != sp.size() - 1)
                continue;
            const bool atStart = end.index == 0;
            const D2 a = mPlane.to2d(sp.points[atStart ? 1 : sp.size() - 2]), b = mPlane.to2d(sp.points[atStart ? 0 : sp.size() - 1]);
            if (glm::length(b - a) < 1e-12)
                continue;
            if (sp.kind != curve::Kind::Poly && !(sp.kind == curve::Kind::Nurbs && sp.degree == 1))
                continue;
            return std::pair{sk::Ref{sk::RefKind::Line, end.spline, atStart ? 0 : int(sp.size()) - 2}, glm::normalize(b - a)};
        }
        return std::nullopt;
    }

    /** The arc through start and end bulging through `through`, snapped tangent to a line it starts on when nearly so. */
    struct ArcShape
    {
        D2 center{0.0};
        double radius = 0.0, start = 0.0, sweep = 0.0;
        std::optional<sk::Ref> tangentTo;
        bool ok = false;
    };

    ArcShape arcFrom(const D2& s, const D2& e, const D2& through) const
    {
        ArcShape a;
        auto fromCenter = [&](const D2& c, bool ccw) {
            a.center = c;
            a.radius = glm::distance(c, s);
            a.start = std::atan2(s.y - c.y, s.x - c.x);
            double end = std::atan2(e.y - c.y, e.x - c.x);
            double sw = end - a.start;
            if (ccw)
                while (sw <= 0.0)
                    sw += glm::two_pi<double>();
            else
                while (sw >= 0.0)
                    sw -= glm::two_pi<double>();
            a.sweep = sw;
            a.ok = a.radius > 1e-9 && std::abs(sw) > 1e-6;
        };
        const D2 m = through;
        const double d = 2.0 * (s.x * (m.y - e.y) + m.x * (e.y - s.y) + e.x * (s.y - m.y));
        if (std::abs(d) > 1e-12)
        {
            const double s2 = glm::dot(s, s), m2 = glm::dot(m, m), e2 = glm::dot(e, e);
            const D2 c((s2 * (m.y - e.y) + m2 * (e.y - s.y) + e2 * (s.y - m.y)) / d, (s2 * (e.x - m.x) + m2 * (s.x - e.x) + e2 * (m.x - s.x)) / d);
            const double cr = (m.x - s.x) * (e.y - s.y) - (m.y - s.y) * (e.x - s.x);
            fromCenter(c, cr < 0.0);
        }
        // Starting on a line end and leaving nearly along it: the tangent arc instead.
        if (auto le = lineEndAt(mPlaced[0]); le && autoConstraints())
        {
            const D2 dir = le->second;
            const D2 se = e - s;
            const D2 nrm(-dir.y, dir.x);
            const double h = glm::dot(se, nrm);
            const D2 startTangent = a.ok ? glm::normalize(D2(-(s.y - a.center.y), s.x - a.center.x)) * (a.sweep > 0 ? 1.0 : -1.0) : dir;
            const double angle = std::acos(std::clamp(glm::dot(startTangent, dir), -1.0, 1.0));
            if (std::abs(h) > 1e-9 && (!a.ok || angle < glm::radians(15.0)))
            {
                const double r = glm::dot(se, se) / (2.0 * h);
                fromCenter(s + nrm * r, r > 0.0);
                a.tangentTo = le->first;
            }
        }
        return a;
    }

    void commitArc()
    {
        ArcShape a = arcFrom(mPlaced[0].at, mPlaced[1].at, mPlaced[2].at);
        if (!a.ok)
        {
            mPlaced.pop_back();
            flash("Arc: the three points are in a line");
            return;
        }
        const int si = int(mSplines.size());
        const D3 n = mPlane.normal();
        const D3 start = mPlane.to3d(a.center + a.radius * D2(std::cos(a.start), std::sin(a.start)));
        Spline s = curve::arc(mPlane.to3d(a.center), a.sweep > 0.0 ? n : -n, start, std::abs(a.sweep));
        json cons = json::array();
        for (const sk::Constraint& k : mPlaced[0].constraints)
            cons.push_back(mapped(k, si, 0, false));
        for (const sk::Constraint& k : mPlaced[1].constraints)
        {
            json j = mapped(k, si, 0, false);
            for (json& r : j["refs"])
                if (r == sk::refName({sk::RefKind::Point, si, 0}))
                    r = "p" + std::to_string(si) + ".end";
            cons.push_back(j);
        }
        if (a.tangentTo)
            cons.push_back({{"type", "tangent"}, {"refs", {sk::refName(*a.tangentTo), sk::refName({sk::RefKind::Curve, si, 0})}}});
        commit(s, cons);
        mPlaced.clear();
    }

    void cancelDrawing()
    {
        mPlaced.clear();
        mClosing.clear();
        mInference.reset();
        if (!nodeOk())
            mPlaneLocked = false;
    }

    // Selecting and dragging

    /** Screen polyline of a spline span (or the whole spline when span < 0). */
    std::vector<QPointF> screenSpan(const Spline& sp, int span, const ViewInfo& view) const
    {
        std::vector<QPointF> out;
        const curve::Polyline pl = curve::tessellate(sp);
        for (size_t i = 0; i < pl.size(); ++i)
        {
            if (span >= 0 && (pl.params[i] < span - 1e-9 || pl.params[i] > span + 1 + 1e-9))
                continue;
            if (auto q = screen(pl.points[i], view))
                out.push_back(*q);
        }
        if (span < 0 && pl.closed && !out.empty())
            out.push_back(out.front());
        if (span >= 0 && pl.closed && size_t(span) + 1 == sp.size() && !pl.points.empty())
            if (auto q = screen(pl.points.front(), view))
                out.push_back(*q);
        return out;
    }

    static double distToPolyline(QPointF p, const std::vector<QPointF>& pts)
    {
        double best = 1e300;
        for (size_t i = 0; i + 1 < pts.size(); ++i)
        {
            const QPointF d = pts[i + 1] - pts[i];
            const double len2 = QPointF::dotProduct(d, d);
            const double t = len2 > 0.0 ? std::clamp(QPointF::dotProduct(p - pts[i], d) / len2, 0.0, 1.0) : 0.0;
            best = std::min(best, distPx(p, pts[i] + d * t));
        }
        return best;
    }

    /** Does the spline offer its legs as lines (poly, Bezier, degree 1 NURBS) rather than only as a whole curve? */
    bool hasLines(size_t si) const
    {
        const Spline& sp = mSplines[si];
        if (sk::circleOf(sp, mPlane).shape != sk::Shape::Points)
            return false;
        return sp.kind != curve::Kind::Nurbs || sp.degree == 1;
    }

    /** What is under px; others also finds other curves (costly, so only on a click). */
    Pick pickAt(QPointF px, const ViewInfo& view, bool others = false) const
    {
        Pick out;
        for (const LabelAt& l : mLabels)
            if (l.rect.adjusted(-3, -3, 3, 3).contains(px))
                return {Pick::Label, {}, l.id};
        double best = kPickPx;
        for (const GlyphAt& g : mGlyphs)
            if (distPx(g.at, px) < best)
            {
                best = distPx(g.at, px);
                out = {Pick::Glyph, {}, g.id};
            }
        if (out.kind != Pick::None)
            return out;
        best = kPickPx;
        for (auto& [ref, at] : pointRefs())
            if (auto q = screen(at, view); q && distPx(*q, px) < best)
            {
                best = distPx(*q, px);
                out = {Pick::Geometry, ref};
            }
        if (out.kind != Pick::None)
            return out;
        best = kPickPx * 0.75;
        for (size_t si = 0; si < mSplines.size(); ++si)
        {
            if (hasLines(si))
            {
                const Spline& sp = mSplines[si];
                const size_t legs = sp.closed ? sp.size() : sp.size() - 1;
                for (size_t i = 0; i < legs; ++i)
                {
                    std::vector<QPointF> pts;
                    if (sp.kind == curve::Kind::Bezier)
                        pts = screenSpan(sp, int(i), view);
                    else
                        for (const D3& q : {sp.points[i], sp.points[(i + 1) % sp.size()]})
                            if (auto s = screen(q, view))
                                pts.push_back(*s);
                    if (double d = distToPolyline(px, pts); d < best)
                    {
                        best = d;
                        out = {Pick::Geometry, {sk::RefKind::Line, int(si), int(i)}};
                    }
                }
            }
            else if (double d = distToPolyline(px, screenSpan(mSplines[si], -1, view)); d < best)
            {
                best = d;
                out = {Pick::Geometry, {sk::RefKind::Curve, int(si), 0}};
            }
        }
        if (out.kind != Pick::None || !others)
            return out;
        // Another curve: clicking it makes it the sketch.
        scene::Graph& g = scene::Graph::current();
        best = kPickPx;
        for (scene::NodeId id : g.nodes())
        {
            scene::Node* n = g.node(id);
            if (id == mNode || !n || !n->type().isA("curve") || !scene::effectivelyVisible(g, id))
                continue;
            const glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            for (const Spline& sp : curve::shapeSplines(g, id))
            {
                const curve::Polyline pl = curve::tessellate(sp);
                std::vector<QPointF> pts;
                for (const D3& q : pl.points)
                    if (auto s = view.project(D3(w * glm::dvec4(q, 1.0))))
                        pts.push_back(*s);
                if (pl.closed && !pts.empty())
                    pts.push_back(pts.front());
                if (double d = distToPolyline(px, pts); d < best)
                {
                    best = d;
                    out = {Pick::OtherCurve, {}, 0, id};
                }
            }
        }
        return out;
    }

    /** Every point a constraint can name, with where it is (object space). */
    std::vector<std::pair<sk::Ref, D3>> pointRefs() const
    {
        std::vector<std::pair<sk::Ref, D3>> out;
        for (size_t si = 0; si < mSplines.size(); ++si)
        {
            const Spline& sp = mSplines[si];
            const sk::CircleFit cf = sk::circleOf(sp, mPlane);
            if (cf.shape != sk::Shape::Points)
            {
                out.push_back({{sk::RefKind::Center, int(si), 0}, mPlane.to3d(cf.center, cf.height)});
                if (cf.shape == sk::Shape::Arc)
                {
                    out.push_back({{sk::RefKind::Point, int(si), 0}, curve::pointAt(sp, 0.0)});
                    out.push_back({{sk::RefKind::Point, int(si), -1}, curve::pointAt(sp, curve::spanCount(sp))});
                }
                continue;
            }
            for (size_t i = 0; i < sp.size(); ++i)
                out.push_back({{sk::RefKind::Point, int(si), int(i)}, sp.points[i]});
        }
        return out;
    }

    /** The drags a press-and-drag on ref asks for, by a plane offset (or to a plane point for a circle's radius). */
    std::vector<sk::Drag> dragsFor(const sk::Ref& ref, const D2& from, const D2& to) const
    {
        std::vector<sk::Drag> out;
        const D3 delta = mPlane.u * (to.x - from.x) + mPlane.v * (to.y - from.y);
        auto move = [&](const sk::Ref& r) { out.push_back({r, sk::refPosition(mSplines, mPlane, r) + delta}); };
        if (ref.isPointLike())
            move(ref);
        else if (ref.kind == sk::RefKind::Line)
        {
            const Spline& sp = mSplines[size_t(ref.spline)];
            move({sk::RefKind::Point, ref.spline, ref.index});
            move({sk::RefKind::Point, ref.spline, int((size_t(ref.index) + 1) % sp.size())});
        }
        else if (sk::circleOf(mSplines[size_t(ref.spline)], mPlane).shape != sk::Shape::Points)
            out.push_back({ref, mPlane.to3d(to)});
        else
            for (size_t i = 0; i < mSplines[size_t(ref.spline)].size(); ++i)
                move({sk::RefKind::Point, ref.spline, int(i)});
        return out;
    }

    void dragTo(QPointF px, const ViewInfo& view)
    {
        std::optional<D2> now = planeAt(px, view);
        if (!now || !mPressPlane || !mPressRef)
            return;
        mDragNow = *now;
        sk::SolveOptions o;
        o.findConflicts = false;
        o.maxIterations = 30;
        o.drags = dragsFor(*mPressRef, *mPressPlane, *now);
        mPreview = mSplines;
        sk::Sketch k = nodeOk() ? curve::sketchOf(*node()) : sk::Sketch{};
        k.plane = mPlane;
        sk::solve(mPreview, k, o);
    }

    void commitDrag()
    {
        if (!mPressRef || !mPressPlane || !nodeOk())
            return;
        std::vector<sk::Drag> drags = dragsFor(*mPressRef, *mPressPlane, mDragNow);
        json call = planeArgs();
        call["args"] = {nodePath()};
        if (mPressRef->kind == sk::RefKind::Curve && drags.size() == 1 && drags[0].point.kind == sk::RefKind::Curve)
        {
            call["point"] = sk::refName(drags[0].point);
            call["to"] = {drags[0].to.x, drags[0].to.y, drags[0].to.z};
        }
        else
        {
            json refs = json::array();
            for (const sk::Drag& d : drags)
                refs.push_back(sk::refName(d.point));
            const D3 by = mPlane.u * (mDragNow.x - mPressPlane->x) + mPlane.v * (mDragNow.y - mPressPlane->y);
            call["points"] = refs;
            call["by"] = {by.x, by.y, by.z};
        }
        runSketchCommand("sketchDrag", call, nullptr);
    }

    template <class V, class T> static void toggle(V& list, const T& v)
    {
        if (auto it = std::ranges::find(list, v); it != list.end())
            list.erase(it);
        else
            list.push_back(v);
    }

    /** The dimension that fits the selection. */
    sk::Type smartDimension() const
    {
        if (mSel.size() == 1)
        {
            const sk::Ref& r = mSel[0];
            if (r.kind == sk::RefKind::Line)
                return sk::Type::Length;
            if (r.kind == sk::RefKind::Curve)
                return sk::circleOf(mSplines[size_t(r.spline)], mPlane).shape == sk::Shape::Circle ? sk::Type::Diameter : sk::Type::Radius;
        }
        if (mSel.size() == 2 && mSel[0].kind == sk::RefKind::Line && mSel[1].kind == sk::RefKind::Line)
        {
            const Spline& a = mSplines[size_t(mSel[0].spline)];
            const Spline& b = mSplines[size_t(mSel[1].spline)];
            const D2 da = mPlane.to2d(a.points[(size_t(mSel[0].index) + 1) % a.size()]) - mPlane.to2d(a.points[size_t(mSel[0].index)]);
            const D2 db = mPlane.to2d(b.points[(size_t(mSel[1].index) + 1) % b.size()]) - mPlane.to2d(b.points[size_t(mSel[1].index)]);
            const double cross = std::abs(da.x * db.y - da.y * db.x) / std::max(glm::length(da) * glm::length(db), 1e-12);
            return cross < 1e-3 ? sk::Type::Distance : sk::Type::Angle;
        }
        return sk::Type::Distance;
    }

    // Dimension editing

    void startEdit(uint32_t id)
    {
        const sketchUi::Display* d = nodeOk() ? sketchUi::display(*node()) : nullptr;
        const sk::Constraint* c = d ? d->sketch.find(id) : nullptr;
        if (!c || !sk::isDimension(c->type))
            return;
        mEditId = id;
        mEditText = c->expr.empty() ? QString::number(c->value, 'g', 10) : QString::fromStdString(c->expr);
        mEditFresh = true;
        viewportTools::requestRedraw();
    }

    void commitEdit()
    {
        const uint32_t id = mEditId;
        mEditId = 0;
        const sketchUi::Display* d = nodeOk() ? sketchUi::display(*node()) : nullptr;
        const sk::Constraint* c = d ? d->sketch.find(id) : nullptr;
        if (!c)
            return;
        const QString text = mEditText.trimmed();
        const QString was = c->expr.empty() ? QString::number(c->value, 'g', 10) : QString::fromStdString(c->expr);
        if (text.isEmpty() || text == was)
            return;
        json call = {{"args", {nodePath()}}, {"id", id}};
        // A plain or relative number sets the value; anything else is kept as a live expression.
        if (std::optional<double> v = evalExpression(text, c->value))
            call["value"] = *v;
        else
            call["expr"] = text.toStdString();
        QString err;
        runSketchCommand("sketchEditDimension", call, &err);
        if (!err.isEmpty())
            flash(err);
        viewportTools::requestRedraw();
    }

    bool editKey(const QKeyEvent& e)
    {
        switch (e.key())
        {
        case Qt::Key_Return:
        case Qt::Key_Enter: commitEdit(); break;
        case Qt::Key_Escape: mEditId = 0; break;
        case Qt::Key_Backspace:
            if (mEditFresh)
                mEditText.clear();
            else
                mEditText.chop(1);
            break;
        default:
            if (e.text().isEmpty() || !e.text()[0].isPrint())
                return true;
            // The first key replaces the old value, as in a selected field.
            if (mEditFresh)
                mEditText.clear();
            mEditText += e.text();
            break;
        }
        mEditFresh = false;
        viewportTools::requestRedraw();
        return true;
    }

    // Keys and palette

    bool toolKey(int key, Qt::KeyboardModifiers mods)
    {
        const bool shift = mods & Qt::ShiftModifier;
        if (auto it = constraintKeys().find(key); it != constraintKeys().end() && !shift && mMode == SketchMode::Select)
        {
            constrain(it->second);
            return true;
        }
        switch (key)
        {
        case Qt::Key_L: setMode(SketchMode::Line); return true;
        case Qt::Key_A: setMode(SketchMode::Arc); return true;
        case Qt::Key_R: setMode(SketchMode::Circle); return true;
        case Qt::Key_S: setMode(SketchMode::Select); return true;
        case Qt::Key_D:
            if (mMode != SketchMode::Select)
                return false;
            dimension(shift ? std::optional(sk::Type::HorizontalDistance) : std::nullopt);
            return true;
        case Qt::Key_I: toggleAuto(); return true;
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            if (!mPlaced.empty())
            {
                mPlaced.pop_back();
                viewportTools::requestRedraw();
                return true;
            }
            if (!mSelIds.empty())
            {
                json ids = json::array();
                for (uint32_t id : mSelIds)
                    ids.push_back(id);
                QString err;
                runSketchCommand("sketchDelete", {{"args", {nodePath()}}, {"ids", ids}}, &err);
                mSelIds.clear();
                if (!err.isEmpty())
                    flash(err);
                viewportTools::requestRedraw();
                return true;
            }
            // Taken even with nothing to remove, so Delete never falls through to deleting the whole curve.
            flash("Delete: click constraint glyphs or dimensions to select them first");
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (mMode == SketchMode::Line)
                commitLine(false);
            viewportTools::requestRedraw();
            return true;
        case Qt::Key_Escape:
            if (!mPlaced.empty())
                commitOrCancel();
            else if (mMode != SketchMode::Select && nodeOk())
                setMode(SketchMode::Select);
            else if (!mSel.empty() || !mSelIds.empty())
            {
                mSel.clear();
                mSelIds.clear();
            }
            else
                exitTool();
            viewportTools::requestRedraw();
            return true;
        default: return false;
        }
    }

    /** Esc while drawing a line keeps what was drawn; an unfinished arc or circle is dropped. */
    void commitOrCancel()
    {
        if (mMode == SketchMode::Line && mPlaced.size() >= 2)
            commitLine(false);
        cancelDrawing();
    }

    void press(const Button& b)
    {
        switch (b.kind)
        {
        case Button::Draw: setMode(b.mode); break;
        case Button::Constrain: constrain(b.type); break;
        case Button::Dimension: dimension(); break;
        case Button::Auto: toggleAuto(); break;
        }
    }

    int buttonAt(QPointF px) const
    {
        for (size_t i = 0; i < mButtons.size(); ++i)
            if (mButtons[i].rect.contains(px))
                return int(i);
        return -1;
    }

    void flash(const QString& text)
    {
        mFlash = text;
        mFlashClock.start();
        viewportTools::requestRedraw();
    }

    // Drawing

    void drawSplineLines(render::FrameState& f, const Spline& s, const glm::vec4& color, bool onTop) const
    {
        if (!curve::isValid(s))
            return;
        const curve::Polyline pl = curve::tessellate(s);
        const size_t segs = pl.closed ? pl.size() : (pl.size() ? pl.size() - 1 : 0);
        auto& out = onTop ? f.linesOnTop : f.lines;
        for (size_t i = 0; i < segs; ++i)
        {
            out.push_back({glm::dvec3(toWorld(pl.points[i])), color});
            out.push_back({glm::dvec3(toWorld(pl.points[(i + 1) % pl.size()])), color});
        }
    }

    void drawGeometry(render::FrameState&, OverlayPainter& p, const ViewInfo& view, const sketchUi::Display* disp, const Pick& hover)
    {
        if (!nodeOk())
            return;
        const Splines& s = mPreview.empty() ? mSplines : mPreview;
        auto highlighted = [&](const sk::Ref& r) {
            if (std::ranges::find(mSel, r) != mSel.end())
                return 2;
            if (hover.kind == Pick::Geometry && hover.ref == r)
                return 1;
            if (hover.kind == Pick::Glyph || hover.kind == Pick::Label)
                if (const sk::Constraint* c = disp ? disp->sketch.find(hover.id) : nullptr)
                    if (std::ranges::find(c->refs, r) != c->refs.end())
                        return 1;
            for (uint32_t id : mSelIds)
                if (const sk::Constraint* c = disp ? disp->sketch.find(id) : nullptr)
                    if (std::ranges::find(c->refs, r) != c->refs.end())
                        return 1;
            return 0;
        };
        // Lines and curves: the preview while dragging, highlights for selected and hovered ones.
        for (size_t si = 0; si < s.size(); ++si)
        {
            const Spline& sp = s[si];
            const bool lines = si < mSplines.size() && hasLines(si);
            const size_t legs = lines ? (sp.closed ? sp.size() : sp.size() - 1) : 1;
            for (size_t i = 0; i < legs; ++i)
            {
                const sk::Ref ref = lines ? sk::Ref{sk::RefKind::Line, int(si), int(i)} : sk::Ref{sk::RefKind::Curve, int(si), 0};
                const int hl = highlighted(ref);
                if (!hl)
                    continue;
                std::vector<QPointF> pts = lines ? screenSpan(sp, int(i), view) : screenSpan(sp, -1, view);
                if (lines && sp.kind != curve::Kind::Bezier)
                {
                    pts.clear();
                    for (const D3& q : {sp.points[i], sp.points[(i + 1) % sp.size()]})
                        if (auto sq = screen(q, view))
                            pts.push_back(*sq);
                }
                p.polyline(pts, 3.0f, hl == 2 ? accentInk() : glm::vec4(0.8f, 0.8f, 0.8f, 1.0f));
            }
        }
        // Points: filled dark when the sketch pins them, light when they can still move.
        for (auto& [ref, at] : pointRefs())
        {
            auto q = screen(mPreview.empty() ? at : sk::refPosition(s, mPlane, ref), view);
            if (!q)
                continue;
            bool fixed = false;
            if (disp)
            {
                const sk::Ref n = sk::normalized(mSplines, ref);
                const size_t si = size_t(ref.spline);
                fixed = ref.kind == sk::RefKind::Center ? si < disp->status.splineFixed.size() && disp->status.splineFixed[si]
                                                        : disp->status.pointIsFixed(si, size_t(n.index));
            }
            const int hl = highlighted(ref);
            const glm::vec4 fill = hl == 2 ? accentInk() : fixed ? sketchUi::constrainedInk() : kFree;
            if (ref.kind == sk::RefKind::Center)
            {
                p.line(*q - QPointF(5, 0), *q + QPointF(5, 0), 3.0f, kRim);
                p.line(*q - QPointF(0, 5), *q + QPointF(0, 5), 3.0f, kRim);
                p.line(*q - QPointF(4, 0), *q + QPointF(4, 0), 1.5f, fill);
                p.line(*q - QPointF(0, 4), *q + QPointF(0, 4), 1.5f, fill);
            }
            else
            {
                p.disc(*q, hl ? 5.5f : 4.5f, hl == 1 ? accentInk() : kRim, 16);
                p.disc(*q, 3.0f, fill, 16);
            }
        }
    }

    /** Where constraint glyphs go: beside their geometry, stacked when several share a spot. */
    void drawGlyphs(OverlayPainter& p, const ViewInfo& view, const sketchUi::Display& d)
    {
        std::map<std::pair<int, int>, int> stack;
        const QPointF mouse = mHover;
        for (const sk::Constraint& c : d.sketch.constraints)
        {
            if (sk::isDimension(c.type))
                continue;
            bool valid = true;
            for (const sk::Ref& r : c.refs)
                valid = valid && sk::refValid(mSplines, mPlane, r);
            if (!valid)
                continue;
            // One glyph per line or curve the constraint ties (Fusion style); point constraints sit at the point.
            std::vector<sk::Ref> at;
            switch (c.type)
            {
            case sk::Type::Parallel:
            case sk::Type::Perpendicular:
            case sk::Type::Equal:
            case sk::Type::Collinear:
            case sk::Type::Tangent: at = c.refs.size() == 1 ? c.refs : std::vector<sk::Ref>{c.refs[0], c.refs[1]}; break;
            case sk::Type::Symmetric: at = {c.refs[0], c.refs[1]}; break;
            default: at = {c.refs[0]}; break;
            }
            for (const sk::Ref& r : at)
            {
                const D3 pos = sk::refPosition(mSplines, mPlane, r);
                auto q = screen(pos, view);
                if (!q)
                    continue;
                QPointF off(12.0, -12.0);
                if (r.kind == sk::RefKind::Line)
                {
                    const Spline& sp = mSplines[size_t(r.spline)];
                    auto a = screen(sp.points[size_t(r.index)], view), b = screen(sp.points[(size_t(r.index) + 1) % sp.size()], view);
                    if (a && b && distPx(*a, *b) > 1e-6)
                    {
                        const QPointF dir = (*b - *a) / distPx(*a, *b);
                        off = QPointF(-dir.y(), dir.x()) * 13.0;
                    }
                }
                const auto key = std::pair{int(std::lround(q->x() / 6.0)), int(std::lround(q->y() / 6.0))};
                const int k = stack[key]++;
                const QPointF along = off.manhattanLength() > 0.0 ? QPointF(-off.y(), off.x()) / std::hypot(off.x(), off.y()) : QPointF(1, 0);
                const QPointF g = *q + off + along * (17.0 * k);
                const bool sel = std::ranges::find(mSelIds, c.id) != mSelIds.end();
                const bool conflict = std::ranges::find(d.conflicting, c.id) != d.conflicting.end();
                const bool hot = distPx(g, mouse) < kPickPx && mHoverView == view.camera;
                p.rect(QRectF(g - QPointF(8, 8), QSizeF(16, 16)), sel ? kPlateSelected : hot ? glm::vec4(0.25f, 0.25f, 0.25f, 0.95f) : kPlate);
                sketchUi::drawGlyph(p, c.type, g, conflict ? conflictInk() : sel ? kInkDark : kInk, 11.0f);
                mGlyphs.push_back({c.id, c.type, g});
            }
        }
    }

    QString dimensionText(const sk::Constraint& c, double value) const
    {
        QString v = QString::number(value, 'f', 2);
        switch (c.type)
        {
        case sk::Type::Angle: v += QChar(0x00B0); break;
        case sk::Type::Radius: v = "R" + v; break;
        case sk::Type::Diameter: v = QChar(0x00D8) + v; break;
        default: break;
        }
        if (!c.expr.empty())
            v = QString::fromStdString(c.name) + " = " + QString::fromStdString(c.expr) + "  (" + v + ")";
        return v;
    }

    void arrow(OverlayPainter& p, QPointF tip, QPointF from, const glm::vec4& col) const
    {
        const double len = distPx(tip, from);
        if (len < 1e-6)
            return;
        const QPointF d = (tip - from) / len, n(-d.y(), d.x());
        p.triangle(tip, tip - d * 8.0 + n * 3.0, tip - d * 8.0 - n * 3.0, col);
    }

    void drawDimensions(OverlayPainter& p, const ViewInfo& view, const sketchUi::Display& d)
    {
        std::map<uint32_t, double> values;
        try
        {
            values = sk::dimensionValues(d.sketch);
        }
        catch (const std::exception&)
        {
        }
        for (const sk::Constraint& c : d.sketch.constraints)
        {
            if (!sk::isDimension(c.type))
                continue;
            bool valid = true;
            for (const sk::Ref& r : c.refs)
                valid = valid && sk::refValid(mSplines, mPlane, r);
            if (!valid)
                continue;
            const bool conflict = std::ranges::find(d.conflicting, c.id) != d.conflicting.end();
            const glm::vec4 col = conflict ? conflictInk() : glm::vec4(0.85f, 0.85f, 0.85f, 1.0f);
            const double value = values.count(c.id) ? values[c.id] : c.value;
            QPointF labelAt;
            auto pos = [&](const sk::Ref& r) { return screen(sk::refPosition(mSplines, mPlane, r), view); };
            auto lineEnds = [&](const sk::Ref& r) {
                const Spline& sp = mSplines[size_t(r.spline)];
                return std::pair{screen(sp.points[size_t(r.index)], view), screen(sp.points[(size_t(r.index) + 1) % sp.size()], view)};
            };
            std::optional<QPointF> a, b;
            switch (c.type)
            {
            case sk::Type::Radius:
            case sk::Type::Diameter:
            {
                const sk::CircleFit cf = sk::circleOf(mSplines[size_t(c.refs[0].spline)], mPlane);
                const double ang = cf.shape == sk::Shape::Arc ? cf.start + cf.sweep * 0.5 : glm::quarter_pi<double>();
                const D2 dir(std::cos(ang), std::sin(ang));
                auto ctr = screen2(cf.center, view), rim = screen2(cf.center + dir * cf.radius, view);
                auto opposite = screen2(cf.center - dir * cf.radius, view);
                if (!ctr || !rim)
                    continue;
                const QPointF from = c.type == sk::Type::Diameter && opposite ? *opposite : *ctr;
                p.line(from, *rim, 1.5f, col);
                arrow(p, *rim, from, col);
                if (c.type == sk::Type::Diameter && opposite)
                    arrow(p, *opposite, *rim, col);
                const double l = std::max(distPx(*rim, from), 1e-6);
                labelAt = *rim + (*rim - from) / l * 22.0;
                break;
            }
            case sk::Type::Angle:
            {
                auto [p0, p1] = lineEnds(c.refs[0]);
                auto [q0, q1] = lineEnds(c.refs[1]);
                if (!p0 || !p1 || !q0 || !q1)
                    continue;
                // The vertex: where the lines cross on screen, else between them.
                const QPointF r = *p1 - *p0, s = *q1 - *q0;
                const double den = r.x() * s.y() - r.y() * s.x();
                QPointF v = (*p0 + *p1 + *q0 + *q1) / 4.0;
                if (std::abs(den) > 1e-9)
                {
                    const double t = ((q0->x() - p0->x()) * s.y() - (q0->y() - p0->y()) * s.x()) / den;
                    v = *p0 + r * t;
                }
                auto farEnd = [&](QPointF e0, QPointF e1) { return distPx(e0, v) > distPx(e1, v) ? e0 : e1; };
                const QPointF da = farEnd(*p0, *p1) - v, db = farEnd(*q0, *q1) - v;
                double a0 = std::atan2(da.y(), da.x()), a1 = std::atan2(db.y(), db.x());
                double sw = a1 - a0;
                while (sw > glm::pi<double>())
                    sw -= glm::two_pi<double>();
                while (sw < -glm::pi<double>())
                    sw += glm::two_pi<double>();
                std::vector<QPointF> arcPts;
                for (int k = 0; k <= 16; ++k)
                {
                    const double t = a0 + sw * k / 16.0;
                    arcPts.push_back(v + QPointF(std::cos(t), std::sin(t)) * 34.0);
                }
                p.polyline(arcPts, 1.5f, col);
                const double mid = a0 + sw * 0.5;
                labelAt = v + QPointF(std::cos(mid), std::sin(mid)) * 52.0;
                break;
            }
            default:
            {
                // Linear dimensions: between two points, a point and its foot on a line, or along a line.
                if (c.refs.size() == 1)
                    std::tie(a, b) = lineEnds(c.refs[0]);
                else if (c.refs[0].isPointLike() && c.refs[1].isPointLike())
                {
                    a = pos(c.refs[0]);
                    b = pos(c.refs[1]);
                }
                else
                {
                    // From the point (or the second line's middle) to its foot on the line.
                    const bool fromPoint = c.refs[0].isPointLike();
                    const sk::Ref ln = fromPoint ? c.refs[1] : c.refs[0];
                    auto ends2 = [&](const sk::Ref& r) {
                        const Spline& sp = mSplines[size_t(r.spline)];
                        return std::pair{mPlane.to2d(sp.points[size_t(r.index)]), mPlane.to2d(sp.points[(size_t(r.index) + 1) % sp.size()])};
                    };
                    const auto [l0, l1] = ends2(ln);
                    const D2 from = fromPoint ? mPlane.to2d(sk::refPosition(mSplines, mPlane, c.refs[0])) : (ends2(c.refs[1]).first + ends2(c.refs[1]).second) * 0.5;
                    const D2 dir = glm::length(l1 - l0) > 1e-12 ? glm::normalize(l1 - l0) : D2(1, 0);
                    a = screen2(from, view);
                    b = screen2(l0 + dir * glm::dot(from - l0, dir), view);
                }
                if (c.type == sk::Type::HorizontalDistance || c.type == sk::Type::VerticalDistance)
                {
                    // Measure along the plane axis: the second end slides onto the axis line through the first.
                    D2 pa(0.0), pb(0.0);
                    if (c.refs.size() == 1)
                    {
                        const Spline& sp = mSplines[size_t(c.refs[0].spline)];
                        pa = mPlane.to2d(sp.points[size_t(c.refs[0].index)]);
                        pb = mPlane.to2d(sp.points[(size_t(c.refs[0].index) + 1) % sp.size()]);
                    }
                    else
                    {
                        pa = mPlane.to2d(sk::refPosition(mSplines, mPlane, c.refs[0]));
                        pb = mPlane.to2d(sk::refPosition(mSplines, mPlane, c.refs[1]));
                    }
                    const D2 corner = c.type == sk::Type::HorizontalDistance ? D2(pb.x, pa.y) : D2(pa.x, pb.y);
                    auto sb = screen2(pb, view), sc = screen2(corner, view);
                    if (sb && sc)
                        p.dashedLine(*sb, *sc, 1.0f, 4.0f, col);
                    a = screen2(pa, view);
                    b = sc;
                }
                if (!a || !b)
                    continue;
                const double len = distPx(*a, *b);
                const QPointF dir = len > 1e-6 ? (*b - *a) / len : QPointF(1, 0);
                const QPointF n(-dir.y(), dir.x());
                const double offset = c.refs.size() == 1 ? 22.0 : 0.0;
                const QPointF a2 = *a + n * offset, b2 = *b + n * offset;
                if (offset > 0.0)
                {
                    p.line(*a, a2 + n * 4.0, 1.0f, col);
                    p.line(*b, b2 + n * 4.0, 1.0f, col);
                }
                p.line(a2, b2, 1.5f, col);
                arrow(p, a2, b2, col);
                arrow(p, b2, a2, col);
                labelAt = (a2 + b2) * 0.5 + n * 12.0;
                break;
            }
            }
            const bool editing = mEditId == c.id;
            QString text = editing ? mEditText + "|" : dimensionText(c, value);
            const float w = OverlayPainter::textWidth(text) + 10.0f;
            QRectF rect(labelAt - QPointF(w * 0.5, 10.0), QSizeF(w, 18.0));
            const bool sel = std::ranges::find(mSelIds, c.id) != mSelIds.end();
            p.rect(rect, editing ? glm::vec4(0.96f, 0.96f, 0.96f, 1.0f) : sel ? kPlateSelected : kPlate);
            p.text(QPointF(rect.center().x(), rect.bottom() - 5.0), text, editing || sel ? kInkDark : (conflict ? conflictInk() : kInk), OverlayPainter::Center);
            mLabels.push_back({c.id, rect});
        }
    }

    void drawPreview(render::FrameState& f, OverlayPainter& p, const ViewInfo& view)
    {
        if (!mPreview.empty())
            for (const Spline& s : mPreview)
                drawSplineLines(f, s, kPreview, true);
        if (mMode == SketchMode::Select)
            return;
        const bool hoverHere = mHoverView == view.camera && mInference;
        std::vector<D2> pts;
        for (const Placed& pl : mPlaced)
            pts.push_back(pl.at);
        const std::optional<D2> cursor = hoverHere ? std::optional(mInference->at) : std::nullopt;
        auto line2 = [&](const D2& a, const D2& b, const glm::vec4& col) {
            f.linesOnTop.push_back({glm::dvec3(toWorld(mPlane.to3d(a))), col});
            f.linesOnTop.push_back({glm::dvec3(toWorld(mPlane.to3d(b))), col});
        };
        if (mMode == SketchMode::Line)
        {
            for (size_t i = 1; i < pts.size(); ++i)
                line2(pts[i - 1], pts[i], kPreview);
            if (cursor && !pts.empty())
                line2(pts.back(), *cursor, kPreview);
        }
        else if (mMode == SketchMode::Circle && !pts.empty() && cursor)
        {
            Spline c = curve::circle(mPlane.to3d(pts[0]), mPlane.normal(), std::max(glm::distance(pts[0], *cursor), 1e-6));
            drawSplineLines(f, c, kPreview, true);
        }
        else if (mMode == SketchMode::Arc && cursor)
        {
            if (pts.size() == 1)
                line2(pts[0], *cursor, kGhost);
            if (pts.size() == 2)
            {
                ArcShape a = arcFrom(pts[0], pts[1], *cursor);
                if (a.ok)
                {
                    const D3 n = mPlane.normal();
                    const D3 st = mPlane.to3d(a.center + a.radius * D2(std::cos(a.start), std::sin(a.start)));
                    drawSplineLines(f, curve::arc(mPlane.to3d(a.center), a.sweep > 0 ? n : -n, st, std::abs(a.sweep)), kPreview, true);
                    if (a.tangentTo)
                        if (auto q = screen2(pts[0], view))
                        {
                            p.rect(QRectF(*q + QPointF(10, 6), QSizeF(16, 16)), kPlate);
                            sketchUi::drawGlyph(p, sk::Type::Tangent, *q + QPointF(18, 14), kInk, 11.0f);
                        }
                }
            }
        }
        for (const D2& q : pts)
            if (auto s = screen2(q, view))
                dot(p, view, toWorld(mPlane.to3d(q)));
        if (!cursor)
            return;
        auto at = screen2(*cursor, view);
        if (!at)
            return;
        dot(p, view, toWorld(mPlane.to3d(*cursor)), 3.0f, kGhost);
        // Inferred constraints: their glyphs by the cursor, and a dashed guide for horizontal and vertical.
        float x = 14.0f;
        for (const sk::Constraint& c : mInference->constraints)
        {
            if ((c.type == sk::Type::Horizontal || c.type == sk::Type::Vertical) && !pts.empty())
                if (auto from = screen2(pts.back(), view))
                    p.dashedLine(*from, *at, 1.0f, 5.0f, kInk);
            p.rect(QRectF(*at + QPointF(x - 8.0, 6.0), QSizeF(16, 16)), kPlate);
            sketchUi::drawGlyph(p, c.type, *at + QPointF(x, 14.0), kInk, 11.0f);
            x += 18.0f;
        }
    }

    void drawPalette(OverlayPainter& p, const ViewInfo& view)
    {
        mButtons.clear();
        auto add = [&](Button b) { mButtons.push_back(b); };
        add({Button::Draw, SketchMode::Select, {}, "S"});
        add({Button::Draw, SketchMode::Line, {}, "L"});
        add({Button::Draw, SketchMode::Arc, {}, "A"});
        add({Button::Draw, SketchMode::Circle, {}, "R"});
        for (sk::Type t : {sk::Type::Coincident, sk::Type::Horizontal, sk::Type::Vertical, sk::Type::Parallel, sk::Type::Perpendicular,
                           sk::Type::Tangent, sk::Type::Equal, sk::Type::Concentric, sk::Type::Midpoint, sk::Type::Symmetric, sk::Type::Collinear,
                           sk::Type::PointOnCurve, sk::Type::Fixed})
            add({Button::Constrain, SketchMode::Select, t, keyOf(t)});
        add({Button::Dimension, SketchMode::Select, sk::Type::Distance, "D"});
        add({Button::Auto, SketchMode::Select, {}, "I"});
        const double gap = 3.0, groupGap = 10.0;
        const double total = mButtons.size() * (kButton + gap) + 3 * groupGap;
        double x = view.width * 0.5 - total * 0.5;
        const double y = view.height - kButton - 14.0;
        const int hot = mHoverView == view.camera ? buttonAt(mHover) : -1;
        for (size_t i = 0; i < mButtons.size(); ++i)
        {
            Button& b = mButtons[i];
            if (i == 4 || i == 17 || i == 18)
                x += groupGap;
            b.rect = QRectF(x, y, kButton, kButton);
            x += kButton + gap;
            const bool on = (b.kind == Button::Draw && b.mode == mMode) || (b.kind == Button::Auto && autoConstraints());
            p.rect(b.rect, on ? kPlateSelected : int(i) == hot ? glm::vec4(0.28f, 0.28f, 0.28f, 0.95f) : kPlate);
            const glm::vec4 ink = on ? kInkDark : kInk;
            const QPointF c = b.rect.center();
            switch (b.kind)
            {
            case Button::Draw:
                if (b.mode == SketchMode::Select)
                {
                    p.triangle(c + QPointF(-5, -7), c + QPointF(-5, 6), c + QPointF(5, 2), ink);
                }
                else if (b.mode == SketchMode::Line)
                {
                    p.line(c + QPointF(-7, 6), c + QPointF(7, -6), 1.6f, ink);
                    p.disc(c + QPointF(-7, 6), 2.2f, ink, 8);
                    p.disc(c + QPointF(7, -6), 2.2f, ink, 8);
                }
                else
                {
                    std::vector<QPointF> ring;
                    const int n = b.mode == SketchMode::Arc ? 12 : 24;
                    for (int k = 0; k <= n; ++k)
                    {
                        const double a = b.mode == SketchMode::Arc ? glm::pi<double>() * (1.0 + double(k) / n) : glm::two_pi<double>() * k / n;
                        ring.push_back(c + QPointF(std::cos(a), std::sin(a) + (b.mode == SketchMode::Arc ? 0.4 : 0.0)) * 7.5);
                    }
                    p.polyline(ring, 1.6f, ink);
                }
                break;
            case Button::Constrain: sketchUi::drawGlyph(p, b.type, c, ink, 14.0f); break;
            case Button::Dimension: sketchUi::drawGlyph(p, sk::Type::Distance, c, ink, 14.0f); break;
            case Button::Auto:
                p.text(QPointF(c.x(), c.y() + 4.0), "A", ink, OverlayPainter::Center);
                p.disc(c + QPointF(6, -6), 2.5f, ink, 8);
                break;
            }
        }
        if (hot >= 0)
        {
            const Button& b = mButtons[size_t(hot)];
            QString what = b.kind == Button::Draw        ? QString(modeName(b.mode))
                           : b.kind == Button::Constrain ? QString(sketchUi::label(b.type))
                           : b.kind == Button::Dimension ? QString("Dimension (Shift+D horizontal)")
                                                         : QString("Auto Constraints while drawing");
            what += "  [" + QString::fromStdString(b.key) + "]";
            p.label(QPointF(b.rect.center().x() - OverlayPainter::textWidth(what) * 0.5, b.rect.top() - 10.0), what);
        }
    }

    void drawMessage(OverlayPainter& p, const ViewInfo& view, const sketchUi::Display* disp)
    {
        ToolStatus status{.tool = "Sketch", .mode = QString(modeName(mMode)).toLower()};
        if (!nodeOk())
            status.subject = "new sketch on the construction plane";
        else
        {
            status.subject = QString::fromStdString(node()->name());
            if (!disp)
                status.fields = {{"constraints", "none"}};
            else if (!disp->ok)
                status.fields = {{"conflict", QString("%1 constraints").arg(disp->conflicting.size())}};
            else
                status.fields = {{"freedom", disp->status.dof == 0 ? QString("fully constrained") : QString("%1 dof").arg(disp->status.dof)}};
        }
        status.flags = {{"auto constraints", autoConstraints()}};
        switch (mMode)
        {
        case SketchMode::Select:
            status.prompt = "click to select, drag to move";
            status.keys = {{"Shift+click", "add"},
                           {"C H V P Q T E O M Y K N F", "constrain"},
                           {"D", "dimension"},
                           {"double-click dimension", "type value"},
                           {"Delete", "remove glyphs"}};
            break;
        case SketchMode::Line:
            status.prompt = "click points, the first closes";
            status.keys = {{"Enter", "finish"}, {"Backspace", "undo point"}};
            break;
        case SketchMode::Arc:
            status.prompt = "click start, end, then the bulge";
            status.keys = {{"from line end", "tangent"}};
            break;
        case SketchMode::Circle: status.prompt = "click the centre, then the radius"; break;
        }
        status.keys.insert(status.keys.end(), {{"I", "auto constraints"}, {"Alt", "suppress"}, {"L/A/R/S", "mode"}, {"Esc", "back"}});
        drawToolStatus(p, view, status);
        if (!mFlash.isEmpty() && mFlashClock.isValid() && mFlashClock.elapsed() < 5000)
            drawToolMessage(p, view, mFlash, conflictInk());
    }

    scene::NodeId mNode = scene::kNoNode;
    Splines mSplines;
    glm::dmat4 mWorld{1.0};
    sk::Plane mPlane;
    std::string mSketchText;
    bool mPlaneLocked = false;
    Placer mPlacer;
    ViewInfo mView;

    SketchMode mMode = SketchMode::Select;
    std::vector<sk::Ref> mSel;
    std::vector<uint32_t> mSelIds;

    std::vector<Placed> mPlaced;
    std::vector<sk::Constraint> mClosing;
    std::optional<sk::Inference> mInference;

    std::optional<sk::Ref> mPressRef;
    QPointF mPressPx;
    std::optional<D2> mPressPlane;
    D2 mDragNow{0.0};
    bool mDragging = false;
    Splines mPreview;

    uint32_t mEditId = 0;
    QString mEditText;
    bool mEditFresh = false;

    QPointF mHover;
    std::string mHoverView;
    QElapsedTimer mClick;
    QPointF mClickPx;

    std::vector<GlyphAt> mGlyphs;
    std::vector<LabelAt> mLabels;
    std::vector<Button> mButtons;
    QString mFlash;
    QElapsedTimer mFlashClock;
};

SketchTool* activeSketchTool()
{
    ViewportTool* t = viewportTools::active();
    return t && std::string_view(t->name()) == "Sketch" ? static_cast<SketchTool*>(t) : nullptr;
}

/** A constraint from an action: through the tool when it is active, else on the selected curve points. */
void constrainAction(sk::Type t)
{
    if (SketchTool* tool = activeSketchTool())
    {
        tool->constrain(t);
        return;
    }
    runSketchCommand(sk::isDimension(t) ? "sketchDimension" : "sketchConstrain", {{"type", sk::typeName(t)}}, nullptr);
}
} // namespace

void registerSketchTool()
{
    if (!viewportTools::registry().find("curve.sketchTool"))
        viewportTools::add({.id = "curve.sketchTool", .label = "Sketch Constraints", .icon = "ruler-measure", .create = [] {
                                return std::make_unique<SketchTool>();
                            }});
    actions::add({.id = "curve.sketchTool",
                  .label = "Sketch Constraints",
                  .category = "Curves",
                  .run = [] { viewportTools::activate("curve.sketchTool"); },
                  .doc = "Constrains and dimensions the selected curve like a CAD sketch (or draws a new one): lines, arcs and circles with "
                         "auto constraints, glyphs to select and delete, drag solving and editable dimensions.",
                  .python = "lila.ui.run_action(\"curve.sketchTool\")"});
    for (int i = 0; i < int(sk::Type::Count); ++i)
    {
        const sk::Type t = sk::Type(i);
        const std::string name = sk::typeName(t);
        const std::string id = "curve.sketch" + std::string(1, char(std::toupper(name[0]))) + name.substr(1);
        actions::add({.id = id,
                      .label = std::string("Sketch ") + sketchUi::label(t),
                      .category = "Curves",
                      .run = [t] { constrainAction(t); },
                      .doc = std::string("Adds a ") + sketchUi::label(t) + (sk::isDimension(t) ? " dimension" : " constraint") +
                             " to the sketch selection (or the selected curve points).",
                      .python = ops::toPython(sk::isDimension(t) ? "sketchDimension" : "sketchConstrain", {{"type", name}, {"refs", {"p0.0"}}})});
    }
    actions::add({.id = "curve.sketchCombine",
                  .label = "Combine into Sketch",
                  .category = "Curves",
                  .doc = "Merges the selected curves into the first so sketch constraints can tie them together.",
                  .python = ops::toPython("sketchCombine", json::object()),
                  .command = "sketchCombine",
                  .call = json::object()});
    actions::add({.id = "curve.sketchSolve",
                  .label = "Solve Sketch",
                  .category = "Curves",
                  .doc = "Solves the selected curve's sketch constraints again after editing it by other means.",
                  .python = ops::toPython("sketchSolve", json::object()),
                  .command = "sketchSolve",
                  .call = json::object()});
    actions::add({.id = "curve.sketchClear",
                  .label = "Clear Sketch Constraints",
                  .category = "Curves",
                  .doc = "Removes every sketch constraint and dimension from the selected curve.",
                  .python = ops::toPython("sketchClear", json::object()),
                  .command = "sketchClear",
                  .call = json::object()});
}
} // namespace lila::ui
