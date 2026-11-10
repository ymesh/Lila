#include "ui/MeasureTool.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/Measure.h"
#include "scene/MeasureMath.h"
#include "scene/MeshNodes.h"
#include "ui/Actions.h"
#include "ui/GeometrySnap.h"
#include "ui/MeshCache.h"
#include "ui/Tools.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportToast.h"
#include "ui/ViewportTool.h"
#include "scene/SceneIndex.h"

#include <QApplication>
#include <QClipboard>
#include <QMainWindow>
#include <QStatusBar>
#include <QTimer>

#include <chrono>
#include <cmath>
#include <numbers>
#include <optional>

namespace lila::ui
{
namespace
{
using scene::NodeId;

const glm::vec4 kColor(0.80f, 0.80f, 0.80f, 1.0f);
const glm::vec4 kSelectedMeasureColor(1.0f, 1.0f, 1.0f, 1.0f);
const glm::vec4 kHudColor(0.92f, 0.92f, 0.92f, 1.0f);
const glm::vec4 kDimColor(0.55f, 0.55f, 0.55f, 1.0f);
constexpr float kTick = 6.0f;
constexpr float kMeasureDash = 5.0f;

PrefStore& prefs() { return PrefStore::get(); }

glm::dvec3 toVec3(const json& j) { return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()}; }

QPointF unit(QPointF d)
{
    double len = std::hypot(d.x(), d.y());
    return len > 1e-9 ? d / len : QPointF(1, 0);
}

/** Liang-Barsky clip to the view plus a margin, so a nearly edge-on line doesn't become a million dashes. */
bool clip(QPointF& a, QPointF& b, const ViewInfo& view)
{
    const double m = 50.0;
    double t0 = 0.0, t1 = 1.0;
    QPointF d = b - a;
    auto edge = [&](double p, double q) {
        if (std::abs(p) < 1e-12)
            return q >= 0.0;
        double r = q / p;
        if (p < 0)
            t0 = std::max(t0, r);
        else
            t1 = std::min(t1, r);
        return t0 <= t1;
    };
    if (!edge(-d.x(), a.x() + m) || !edge(d.x(), view.width + m - a.x()) || !edge(-d.y(), a.y() + m) ||
        !edge(d.y(), view.height + m - a.y()))
        return false;
    QPointF a0 = a;
    a = a0 + d * t0;
    b = a0 + d * t1;
    return true;
}

/** A measurement's 2D picture: dashed and solid segments in widget pixels, plus its label. */
struct Shape
{
    std::vector<std::pair<QPointF, QPointF>> dashed;
    std::vector<std::pair<QPointF, QPointF>> solid;
    QPointF labelAt;
    QString label;
    bool valid = false;

    void draw(OverlayPainter& p, const ViewInfo& view, const glm::vec4& color) const
    {
        for (auto [a, b] : dashed)
            if (clip(a, b, view))
                p.dashedLine(a, b, 1.5f, kMeasureDash, color);
        for (auto [a, b] : solid)
            if (clip(a, b, view))
                p.line(a, b, 1.5f, color);
        if (!label.isEmpty())
            p.label(labelAt - QPointF(OverlayPainter::textWidth(label) / 2.0, 0.0), label, color);
    }

    double distance(QPointF px) const
    {
        auto seg = [&](QPointF a, QPointF b) {
            QPointF d = b - a;
            double len2 = QPointF::dotProduct(d, d);
            double t = len2 > 0 ? std::clamp(QPointF::dotProduct(px - a, d) / len2, 0.0, 1.0) : 0.0;
            QPointF q = a + d * t - px;
            return std::hypot(q.x(), q.y());
        };
        double best = 1e300;
        for (const auto& [a, b] : dashed)
            best = std::min(best, seg(a, b));
        for (const auto& [a, b] : solid)
            best = std::min(best, seg(a, b));
        double w = OverlayPainter::textWidth(label);
        if (!label.isEmpty() && std::abs(px.x() - labelAt.x()) < w / 2 + 5 && px.y() < labelAt.y() + 5 && px.y() > labelAt.y() - 16)
            best = 0.0;
        return best;
    }
};

void addTicks(Shape& s, QPointF a, QPointF b)
{
    QPointF u = unit(b - a), n(-u.y(), u.x());
    s.solid.push_back({a - n * kTick, a + n * kTick});
    s.solid.push_back({b - n * kTick, b + n * kTick});
}

Shape distanceShape(const glm::dvec3& a, const glm::dvec3& b, const QString& label, const ViewInfo& view)
{
    Shape s;
    auto pa = view.project(a), pb = view.project(b);
    if (!pa || !pb)
        return s;
    s.valid = true;
    s.dashed.push_back({*pa, *pb});
    addTicks(s, *pa, *pb);
    QPointF u = unit(*pb - *pa), n(-u.y(), u.x());
    if (n.y() > 0)
        n = -n;
    s.labelAt = (*pa + *pb) / 2.0 + n * 14.0 + QPointF(0, 4);
    s.label = label;
    return s;
}

Shape angleShape(const glm::dvec3& a, const glm::dvec3& v, const glm::dvec3& b, const QString& label, const ViewInfo& view)
{
    Shape s;
    auto pa = view.project(a), pv = view.project(v), pb = view.project(b);
    if (!pa || !pv || !pb)
        return s;
    s.valid = true;
    s.dashed.push_back({*pv, *pa});
    s.dashed.push_back({*pv, *pb});
    glm::dvec3 u = a - v, w = b - v;
    double lu = glm::length(u), lw = glm::length(w);
    if (lu < 1e-9 || lw < 1e-9)
        return s;
    u /= lu;
    w /= lw;
    double theta = std::atan2(glm::length(glm::cross(u, w)), glm::dot(u, w));
    glm::dvec3 perp = w - u * glm::dot(u, w);
    perp = glm::length(perp) > 1e-9 ? glm::normalize(perp) : glm::dvec3(0.0);
    double r = 0.3 * std::min(lu, lw);
    const int n = 24;
    std::optional<QPointF> prev;
    for (int i = 0; i <= n; ++i)
    {
        double t = theta * i / n;
        auto q = view.project(v + (u * std::cos(t) + perp * std::sin(t)) * r);
        if (q && prev)
            s.solid.push_back({*prev, *q});
        prev = q;
    }
    auto mid = view.project(v + (u * std::cos(theta / 2) + perp * std::sin(theta / 2)) * r * 1.6);
    s.labelAt = (mid ? *mid : *pv) + QPointF(0, 4);
    s.label = label;
    return s;
}

Shape circleShape(const glm::dvec3& c, const glm::dvec3& normal, double radius, bool diameter, const QString& label, const ViewInfo& view)
{
    Shape s;
    auto pc = view.project(c);
    if (!pc || radius <= 0.0)
        return s;
    s.valid = true;
    glm::dvec3 nrm = glm::normalize(normal);
    glm::dvec3 ref = std::abs(nrm.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    // The radius line lies along the in-plane direction that reads best on screen: perpendicular to the view.
    glm::dvec3 viewDir = view.viewDir(c);
    glm::dvec3 u = glm::cross(nrm, viewDir);
    if (glm::length(u) < 1e-6)
        u = glm::cross(nrm, ref);
    u = glm::normalize(u);
    glm::dvec3 w = glm::cross(nrm, u);
    const int n = 72;
    std::optional<QPointF> prev;
    for (int i = 0; i <= n; ++i)
    {
        double t = 2.0 * std::numbers::pi * i / n;
        auto q = view.project(c + (u * std::cos(t) + w * std::sin(t)) * radius);
        // Every other segment, for a dashed circle.
        if (q && prev && (i % 2))
            s.solid.push_back({*prev, *q});
        prev = q;
    }
    auto edge = view.project(c + u * radius);
    auto other = view.project(c - u * radius);
    if (edge)
    {
        QPointF from = diameter && other ? *other : *pc;
        s.dashed.push_back({from, *edge});
        addTicks(s, from, *edge);
        QPointF dir = unit(*edge - from), nn(-dir.y(), dir.x());
        if (nn.y() > 0)
            nn = -nn;
        s.labelAt = (from + *edge) / 2.0 + nn * 14.0 + QPointF(0, 4);
    }
    else
        s.labelAt = *pc;
    s.solid.push_back({*pc - QPointF(4, 0), *pc + QPointF(4, 0)});
    s.solid.push_back({*pc - QPointF(0, 4), *pc + QPointF(0, 4)});
    s.label = label;
    return s;
}

glm::dvec3 originOf(const scene::Node& n, int attr) { return glm::dvec3(n.as<glm::dmat4>(attr)[3]); }

Shape shapeOf(const scene::Node& n, const ViewInfo& view)
{
    QString label = QString::fromStdString(scene::measure::label(n));
    if (n.type().isA("distanceMeasure"))
        return distanceShape(n.as<glm::dvec3>(scene::DistanceMeasure::kStartPoint), n.as<glm::dvec3>(scene::DistanceMeasure::kEndPoint), label,
                             view);
    if (n.type().isA("angleMeasure"))
        return angleShape(originOf(n, scene::AngleMeasure::kPoint1Matrix), originOf(n, scene::AngleMeasure::kVertexMatrix),
                          originOf(n, scene::AngleMeasure::kPoint2Matrix), label, view);
    if (n.type().isA("radiusMeasure") && n.as<bool>(scene::RadiusMeasure::kValid))
        return circleShape(n.as<glm::dvec3>(scene::RadiusMeasure::kCenter), n.as<glm::dvec3>(scene::RadiusMeasure::kNormal),
                           n.as<double>(scene::RadiusMeasure::kRadius),
                           n.as<int64_t>(scene::RadiusMeasure::kDisplay) == scene::RadiusMeasure::kShowDiameter, label, view);
    return {};
}

bool drawn(const scene::Node& n) { return scene::measure::isMeasure(n) && scene::effectivelyVisible(scene::Graph::current(), n.id()); }

// ------------------------------------------------------------------- HUD

struct Hud
{
    json result;
    std::chrono::steady_clock::time_point until{};
};

Hud& hud()
{
    static Hud h;
    return h;
}

void showHud(const json& result)
{
    double seconds = std::max(0.5, prefs().value<double>("general", "/measure/hudSeconds", 5.0));
    hud().result = result;
    hud().until = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(seconds * 1000));
    viewportTools::requestRedraw();
    QTimer::singleShot(int(seconds * 1000) + 50, [] { viewportTools::requestRedraw(); });
}

constexpr const char* kCopyIcon = "copy";

struct HudLayout
{
    QStringList lines;
    QPointF at;
    double lineH = 18.0;
    QRectF copy; // the copy button, right of the headline
};

/** Centred above the view's bottom edge, the headline on top. */
HudLayout hudLayout(const json& r, const ViewInfo& view)
{
    HudLayout l;
    l.lines = QString::fromStdString(r.value("report", std::string())).split('\n', Qt::SkipEmptyParts);
    while (l.lines.size() > 10)
        l.lines.removeLast();
    float w = 0.0f;
    for (const QString& line : l.lines)
        w = std::max(w, OverlayPainter::textWidth(line));
    l.at = QPointF(std::round(view.width / 2.0 - w / 2.0), std::round(view.height - 28.0 - l.lineH * (l.lines.size() - 1)));
    const float head = l.lines.isEmpty() ? 0.0f : OverlayPainter::textWidth(l.lines.front());
    l.copy = QRectF(l.at.x() + head + 10.0, l.at.y() - 13.0, 16.0, 16.0);
    return l;
}

bool hudShowing()
{
    return !hud().result.is_null() && std::chrono::steady_clock::now() <= hud().until;
}

void drawHud(OverlayPainter& p, const ViewInfo& view)
{
    Hud& h = hud();
    if (!hudShowing())
        return;
    const json& r = h.result;
    for (const json& seg : r.value("segments", json::array()))
    {
        auto a = view.project(toVec3(seg[0])), b = view.project(toVec3(seg[1]));
        if (!a || !b)
            continue;
        QPointF pa = *a, pb = *b;
        if (clip(pa, pb, view))
            p.dashedLine(pa, pb, 2.0f, kMeasureDash, kHudColor);
    }
    if (r.contains("circle"))
    {
        const json& c = r["circle"];
        Shape s = circleShape(toVec3(c["center"]), toVec3(c["normal"]), c["radius"].get<double>(), true, {}, view);
        s.draw(p, view, kHudColor);
    }

    HudLayout l = hudLayout(r, view);
    for (int i = 0; i < l.lines.size(); ++i)
        p.label(l.at + QPointF(0, l.lineH * i), l.lines[i], i == 0 ? kHudColor : kDimColor);
    p.icon(l.copy, kCopyIcon, kHudColor);
}

void drawMeasures(render::FrameState&, OverlayPainter& p, const ViewInfo& view)
{
    LILA_ZONE();

    p.setShadow(true);
    if (scene::measure::shown())
    {
        scene::Graph& g = scene::Graph::current();
        g.evaluate();
        // Selected last, so it draws over the rest.
        for (int pass = 0; pass < 2; ++pass)
            for (scene::Node* n : scene::sceneIndex::matching(g, "measure", scene::measure::isMeasure))
            {
                const NodeId id = n->id();
                if (!drawn(*n) || g.isSelected(id) != (pass == 1))
                    continue;
                Shape s = shapeOf(*n, view);
                if (s.valid)
                    s.draw(p, view, pass ? kSelectedMeasureColor : kColor);
            }
    }
    drawHud(p, view);
    p.setShadow(false);
}

std::vector<viewportHooks::PickHit> pickMeasures(QPointF a, QPointF b, bool click, const ViewInfo& view)
{
    LILA_ZONE();

    std::vector<viewportHooks::PickHit> hits;
    if (!scene::measure::shown())
        return hits;
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    QRectF box = QRectF(a, b).normalized();
    for (NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!drawn(*n))
            continue;
        Shape s = shapeOf(*n, view);
        if (!s.valid)
            continue;
        if (click)
        {
            if (double d = s.distance(a); d < 6.0)
                hits.push_back({n->path(), d});
        }
        else if (box.contains(s.labelAt))
            hits.push_back({n->path(), 0.0});
    }
    return hits;
}

// ------------------------------------------------------------------- snapping

struct Snap
{
    glm::dvec3 p{0.0};
    NodeId mesh = scene::kNoNode;
    int64_t vertex = -1;
    const char* kind = "plane";
};

double pxDist(QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

/** The point under the cursor: V vertex, C edge, else the surface, else the ground plane; X rounds to the grid. */
Snap snapAt(QPointF px, const ViewInfo& view)
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    unsigned snaps = tools::snaps();
    glm::dvec3 o, d;
    view.ray(px, o, d);

    std::optional<Snap> best;
    double bestPx = 16.0;
    double bestRay = 1e300;
    std::optional<Snap> surface;
    for (NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (geometrySnap::isPointObject(g, id))
        {
            glm::dvec3 w(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
            if (auto s = view.project(w); (snaps & SnapVertex) && s && scene::effectivelyVisible(g, id) && pxDist(*s, px) < bestPx)
            {
                bestPx = pxDist(*s, px);
                best = Snap{w, scene::kNoNode, -1, "object"};
            }
            continue;
        }
        if (!n->type().isA("mesh") || !scene::effectivelyVisible(g, id))
            continue;
        scene::MeshPtr mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        if (!mesh)
            continue;
        const geo::Mesh& m = *mesh;
        glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        auto at = [&](geo::Index p) { return glm::dvec3(world * glm::dvec4(m.position(p), 1.0)); };
        if (snaps & SnapVertex)
            for (geo::Index p = 0; p < m.pointCapacity(); ++p)
            {
                if (!m.pointValid(p))
                    continue;
                glm::dvec3 w = at(p);
                if (auto s = view.project(w); s && pxDist(*s, px) < bestPx)
                {
                    bestPx = pxDist(*s, px);
                    best = Snap{w, id, int64_t(p), "vertex"};
                }
            }
        else if (snaps & SnapEdge)
            for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
            {
                if (!m.edgeValid(e))
                    continue;
                glm::dvec3 wa = at(m.edgePoint(e, 0)), wb = at(m.edgePoint(e, 1));
                auto sa = view.project(wa), sb = view.project(wb);
                if (!sa || !sb)
                    continue;
                QPointF dir = *sb - *sa;
                double len2 = QPointF::dotProduct(dir, dir);
                double t = len2 > 0 ? std::clamp(QPointF::dotProduct(px - *sa, dir) / len2, 0.0, 1.0) : 0.0;
                if (double dd = pxDist(*sa + dir * t, px); dd < bestPx)
                {
                    bestPx = dd;
                    best = Snap{glm::mix(wa, wb, t), id, -1, "edge"};
                }
            }
        if (auto rm = renderMeshFor(mesh))
            if (auto hit = raycast(*rm, world, o, d); hit && hit->distance < bestRay)
            {
                bestRay = hit->distance;
                surface = Snap{o + d * hit->distance, id, -1, "face"};
            }
    }
    Snap out;
    if (best)
        out = *best;
    else if (surface)
        out = *surface;
    else
    {
        glm::dvec3 up = prefs().value<std::string>("general", "/scene/upAxis", "y") == "z" ? glm::dvec3(0, 0, 1) : glm::dvec3(0, 1, 0);
        double denom = glm::dot(d, up);
        double t = std::abs(denom) > 0.1 ? -glm::dot(o, up) / denom : -1.0;
        // A ground plane seen edge-on (front and side views) gives no usable hit: use the plane through the origin facing the view.
        if (t <= 0.0)
            t = -glm::dot(o, d);
        out.p = o + d * (t > 0.0 ? t : 100.0);
    }
    if ((snaps & SnapGrid) && !best)
    {
        double step = prefs().value<double>("general", "/grid/spacing", 10.0);
        for (int i = 0; i < 3; ++i)
            out.p[i] = std::round(out.p[i] / step) * step;
        out.kind = "grid";
        out.vertex = -1;
    }
    return out;
}

// ------------------------------------------------------------------- tool

void runAction(const char* name, json args)
{
    actions::run({{"command", name}, {"args", std::move(args)}});
}

bool attachToMeshes() { return prefs().value<bool>("general", "/measure/attach", false); }

json parentFor(const Snap& s)
{
    if (s.mesh == scene::kNoNode || !attachToMeshes())
        return nullptr;
    scene::Node* n = scene::Graph::current().node(s.mesh);
    return n ? json(n->path()) : json(nullptr);
}

json point(const glm::dvec3& p) { return json::array({p.x, p.y, p.z}); }

/** Click points with V/C/X snapping; distance takes two, angle three (the corner second), radius three or more then Enter. */
class MeasureTool final : public ViewportTool
{
public:
    enum class Kind
    {
        Distance,
        Angle,
        Radius,
    };

    explicit MeasureTool(Kind kind) : mKind(kind) {}

    const char* name() const override
    {
        return mKind == Kind::Distance ? "distanceTool" : mKind == Kind::Angle ? "angleTool" : "radiusTool";
    }

    void activate() override { log::info("{}", hint()); }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        mPicked.push_back(snapAt(e->position(), view));
        if ((mKind == Kind::Distance && mPicked.size() == 2) || (mKind == Kind::Angle && mPicked.size() == 3))
            commit();
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = snapAt(e->position(), view);
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseRelease(QMouseEvent*, const ViewInfo&) override { return true; }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        switch (e->key())
        {
        case Qt::Key_Escape:
            if (!mPicked.empty())
                mPicked.clear();
            else
                // Not from inside our own handler: set() destroys this tool.
                QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
            viewportTools::requestRedraw();
            return true;
        case Qt::Key_Backspace:
            if (!mPicked.empty())
                mPicked.pop_back();
            viewportTools::requestRedraw();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (mKind == Kind::Radius && mPicked.size() >= 3)
                commit();
            viewportTools::requestRedraw();
            return true;
        default:
            return false;
        }
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        drawToolStatus(p, view, toolStatus());
        p.setShadow(true);
        std::vector<glm::dvec3> pts;
        for (const Snap& s : mPicked)
            pts.push_back(s.p);
        if (mHover)
            pts.push_back(mHover->p);
        for (const Snap& s : mPicked)
            if (auto q = view.project(s.p))
                p.disc(*q, 3.5f, kSelectedMeasureColor);
        if (mHover)
            if (auto q = view.project(mHover->p))
            {
                p.rect(QRectF(*q - QPointF(4, 4), QSizeF(8, 8)), {1, 1, 1, 0.35f});
                p.text(*q + QPointF(9, -7), mHover->kind, kDimColor);
            }

        Shape s;
        if (mKind == Kind::Distance && pts.size() == 2)
            s = distanceShape(pts[0], pts[1], QString::fromStdString(scene::measure::formatLength(glm::distance(pts[0], pts[1]))), view);
        else if (mKind == Kind::Angle && pts.size() >= 2)
        {
            if (pts.size() == 2)
                s = distanceShape(pts[0], pts[1], {}, view);
            else
                try
                {
                    double a = scene::measure::angleDegrees(pts[0], pts[1], pts[2]);
                    s = angleShape(pts[0], pts[1], pts[2], QString::fromStdString(scene::measure::formatAngle(a)), view);
                }
                catch (const std::invalid_argument&)
                {
                }
        }
        else if (mKind == Kind::Radius && pts.size() >= 3)
            try
            {
                scene::measure::Circle c = scene::measure::fitCircle(pts);
                s = circleShape(c.center, c.normal, c.radius, false,
                                QString::fromStdString("R " + scene::measure::formatLength(c.radius) + "   \xC3\x98 " +
                                                       scene::measure::formatLength(2.0 * c.radius)),
                                view);
            }
            catch (const std::invalid_argument&)
            {
            }
        if (s.valid)
            s.draw(p, view, kSelectedMeasureColor);
        p.setShadow(false);
    }

private:
    std::string hint() const
    {
        switch (mKind)
        {
        case Kind::Distance:
            return "Distance tool: click start and end (hold V vertex, C edge, X grid), Esc to cancel";
        case Kind::Angle:
            return "Angle tool: click the first arm, the corner, then the second arm; Esc to cancel";
        default:
            return "Radius tool: click 3+ points on the circle, Enter to finish, Backspace to undo a point, Esc to cancel";
        }
    }

    ToolStatus toolStatus() const
    {
        switch (mKind)
        {
        case Kind::Distance:
            return {.tool = "Distance Tool",
                    .prompt = "click start and end",
                    .keys = {{"V", "vertex"}, {"C", "edge"}, {"X", "grid"}, {"Esc", "cancel"}}};
        case Kind::Angle:
            return {.tool = "Angle Tool", .prompt = "click arm, corner, then arm", .keys = {{"Esc", "cancel"}}};
        default:
            return {.tool = "Radius Tool",
                    .prompt = "click 3+ points on the circle",
                    .keys = {{"Enter", "finish"}, {"Backspace", "undo point"}, {"Esc", "cancel"}}};
        }
    }

    void commit()
    {
        std::vector<Snap> picked = std::move(mPicked);
        mPicked.clear();
        if (mKind == Kind::Distance)
            runAction("distanceMeasure",
                       {{"start", point(picked[0].p)}, {"end", point(picked[1].p)}, {"startParent", parentFor(picked[0])},
                        {"endParent", parentFor(picked[1])}});
        else if (mKind == Kind::Angle)
        {
            json pts = json::array(), parents = json::array();
            for (const Snap& s : picked)
            {
                pts.push_back(point(s.p));
                parents.push_back(parentFor(s));
            }
            runAction("angleMeasure", {{"points", pts}, {"parents", parents}});
        }
        else
        {
            // All on vertices of one mesh: fit those vertices live, so the measure follows edits.
            bool live = std::all_of(picked.begin(), picked.end(), [&](const Snap& s) { return s.vertex >= 0 && s.mesh == picked[0].mesh; });
            if (live)
            {
                json verts = json::array();
                for (const Snap& s : picked)
                    verts.push_back(s.vertex);
                runAction("radiusMeasure", {{"mesh", scene::Graph::current().node(picked[0].mesh)->path()}, {"vertices", verts}});
            }
            else
            {
                json pts = json::array();
                for (const Snap& s : picked)
                    pts.push_back(point(s.p));
                runAction("radiusMeasure", {{"points", pts}});
            }
        }
    }

    Kind mKind;
    std::vector<Snap> mPicked;
    std::optional<Snap> mHover;
};

// ------------------------------------------------------------------- measureSelection UI

void status(const QString& text)
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* mw = qobject_cast<QMainWindow*>(w))
        {
            mw->statusBar()->showMessage(text, 8000);
            return;
        }
}

bool clipboardUnits() { return prefs().value<bool>("general", "/measure/clipboardUnits", false); }

std::string formatField(const std::string& field, double v, bool withUnit)
{
    if (field == "angle")
        return scene::measure::formatAngle(v, withUnit);
    if (field == "area")
        return scene::measure::formatArea(v, withUnit);
    return scene::measure::formatLength(v, withUnit);
}

std::optional<json> measure()
{
    try
    {
        return ops::execute("measureSelection", json::object(), CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::warn("{}", e.what());
        status(QString::fromUtf8(e.what()));
        return std::nullopt;
    }
}

void copy(const std::string& text, const json& r)
{
    QApplication::clipboard()->setText(QString::fromStdString(text));
    QString line = QString::fromStdString(r.value("report", std::string())).replace('\n', "   |   ");
    status(line + "   (copied " + QString::fromStdString(text) + ")");
    viewportToast::showActive(text == r.value("report", std::string()) ? QString("Copied the measurements") : "Copied " + QString::fromStdString(text));
}

/** The main number, as the clipboard prefs format it. */
std::string headline(const json& r)
{
    if (std::string main = r.value("main", std::string()); !main.empty() && r.contains(main) && r[main].is_number())
        return formatField(main, r[main].get<double>(), clipboardUnits());
    if (r.contains("value") && r["value"].is_string())
        return r["value"].get<std::string>();
    return r.value("text", std::string());
}

/** measureSelection with its HUD; copies the headline, the full report or nothing per /measure/clipboard. */
void measureSelectionUi()
{
    std::optional<json> r = measure();
    if (!r)
        return;
    showHud(*r);
    std::string mode = prefs().value<std::string>("general", "/measure/clipboard", "value");
    if (mode == "off")
    {
        status(QString::fromStdString(r->value("report", std::string())).replace('\n', "   |   "));
        return;
    }
    copy(mode == "report" ? r->value("report", std::string()) : headline(*r), *r);
}

/** The HUD's copy button: copies the headline, or the full report with Shift, and keeps the HUD up a while longer. */
bool clickHud(QPointF px, const ViewInfo& view)
{
    if (!hudShowing() || !hudLayout(hud().result, view).copy.adjusted(-3, -3, 3, 3).contains(px))
        return false;
    const json r = hud().result;
    copy(QApplication::keyboardModifiers() & Qt::ShiftModifier ? r.value("report", std::string()) : headline(r), r);
    showHud(r);
    return true;
}

/** Copies one named number from measureSelection, trying fields in order (Copy Diameter falls back to 2 * radius...). */
void copyField(const char* what, std::vector<std::string> fields)
{
    std::optional<json> r = measure();
    if (!r)
        return;
    showHud(*r);
    for (const std::string& f : fields)
        if (r->contains(f) && (*r)[f].is_number())
        {
            copy(formatField(f, (*r)[f].get<double>(), clipboardUnits()), *r);
            return;
        }
    std::string msg = std::string("Copy ") + what + ": the selection has no " + what + " (" + r->value("kind", std::string("nothing")) + ")";
    log::warn("{}", msg);
    status(QString::fromStdString(msg));
}

void registerMeasureActions()
{
    auto add = [](const char* id, const char* label, std::function<void()> fn, const char* python = "", const char* doc = "",
                  std::vector<std::string> keywords = {}) {
        actions::add({.id = id, .label = label, .category = "Measure", .run = std::move(fn), .doc = doc, .python = python,
                      .keywords = std::move(keywords)});
    };
    // The keyword also lets tear-off search find the Display > Measure section by "distance".
    add("measure.selection", "Measure Selection", measureSelectionUi, "lila.ops.measureSelection()",
        "Distance, length, angle, circle, area or size of the selection, shown in the viewport and copied to the clipboard.", {"distance"});
    add("measure.copyDistance", "Copy Distance", [] { copyField("distance", {"distance", "length"}); }, "lila.ops.measureSelection()[\"distance\"]",
        "Copies the distance across the selection to the clipboard, in the clipboard units.");
    add("measure.copyDiameter", "Copy Diameter", [] { copyField("diameter", {"diameter"}); }, "lila.ops.measureSelection()[\"diameter\"]",
        "Copies the diameter of the circle through the selection to the clipboard.");
    add("measure.copyLength", "Copy Length", [] { copyField("length", {"length", "distance", "circumference"}); },
        "lila.ops.measureSelection()[\"length\"]", "Copies the total length of the selected edges to the clipboard.");
    add("measure.distanceTool", "Distance Tool", [] { viewportTools::set(std::make_unique<MeasureTool>(MeasureTool::Kind::Distance)); }, "",
        "Click two points in the viewport to place a live distance measurement.");
    add("measure.angleTool", "Angle Tool", [] { viewportTools::set(std::make_unique<MeasureTool>(MeasureTool::Kind::Angle)); }, "",
        "Click the first arm, the corner, then the second arm to place a live angle measurement.");
    add("measure.radiusTool", "Radius Tool", [] { viewportTools::set(std::make_unique<MeasureTool>(MeasureTool::Kind::Radius)); }, "",
        "Click three or more points on a round shape, then Enter, to place a best-fit radius and diameter.");
    add("measure.distanceFromSelection", "Distance Measure from Selection", [] { runAction("distanceMeasure", json::object()); },
        "lila.ops.distanceMeasure()", "Places a distance measurement between the two selected points or objects; it updates as they move.");
    add("measure.angleFromSelection", "Angle Measure from Selection", [] { runAction("angleMeasure", json::object()); }, "lila.ops.angleMeasure()",
        "Places an angle measurement on the three selected points; it updates as they move.");
    add("measure.radiusFromSelection", "Radius Measure from Selection", [] { runAction("radiusMeasure", json::object()); },
        "lila.ops.radiusMeasure()", "Places a best-fit circle through the selected vertices; it updates under edits.");
    add("measure.toggleShow", "Show Measurements", [] {
            prefs().set("general", "/measure/show", !scene::measure::shown());
            viewportTools::requestRedraw();
        },
        "lila.prefs.set(\"general\", \"/measure/show\", False)", "Shows or hides every placed measurement in the viewports.");
}
} // namespace

void registerMeasureUi()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    viewportHooks::addDrawer(drawMeasures);
    viewportHooks::addPicker(pickMeasures);
    overlayIcons::add(kCopyIcon, {.tabler = kCopyIcon});
    viewportHooks::setClickHook("lila.measureHud", clickHud);
    registerMeasureActions();
}
} // namespace lila::ui
