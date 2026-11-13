#include "ui/NodeTools.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "gn/Gn.h"
#include "gn/Tool.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "render/FrameState.h"
#include "scene/Graph.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/GraphBackend.h"
#include "ui/Menus.h"
#include "ui/NodeEditor.h"
#include "ui/NodeHandles.h"
#include "ui/Overlay.h"
#include "ui/Theme.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportTool.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QTimer>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <set>

namespace lila::ui::nodeTools
{
namespace
{
using json = nlohmann::json;
using gn::GizmoSpec;
using scene::NodeId;

constexpr double kPickPx = 8.0;
constexpr double kArrowPx = 70.0;
constexpr double kDialPx = 45.0;
constexpr double kRingPx = 60.0;
constexpr double kScaleBoxPx = 52.0;
constexpr int kRingSegments = 64;
/** Pixels of drag that double a Transform Gizmo's scale. */
constexpr double kPxPerScale = 100.0;
/** Past this many preview edges a tool shows no preview wire. */
constexpr size_t kMaxPreviewEdges = 400000;

glm::vec4 neutralColor() { return themeSrgb("manipNeutral", {0.75f, 0.75f, 0.75f, 1.0f}); }
glm::vec4 hotColor() { return themeSrgb("manipActive", {1.0f, 0.85f, 0.25f, 1.0f}); }

glm::vec4 partColor(int axis, bool hot)
{
    if (axis >= 0 && viewportDisplay::accentColors(viewportDisplay::kManipulatorAxisColors))
    {
        glm::vec4 c(viewportDisplay::axisColor(axis), 1.0f);
        return hot ? glm::mix(c, hotColor(), 0.55f) : c;
    }
    return hot ? hotColor() : neutralColor();
}

enum class Tip : uint8_t
{
    None,
    Arrow,
    Box,
    Cross,
    Dot,
};

/** One grabbable piece of a gizmo, as screen polylines. */
struct Part
{
    int id = 0;
    std::vector<QPointF> line;
    bool closed = false;
    int axis = -1;
    Tip tip = Tip::None;
    /** Where the tip sits and which way it points on screen. */
    QPointF tipAt;
    QPointF tipDir{1.0, 0.0};
    /** A dial's current value marker. */
    std::optional<QPointF> marker;
};

glm::dvec3 unitOr(const glm::dvec3& v, const glm::dvec3& fallback) { return glm::length(v) > 1e-12 ? glm::normalize(v) : fallback; }

/** Two unit vectors spanning the plane perpendicular to n. */
void planeBasis(const glm::dvec3& n, glm::dvec3& u, glm::dvec3& v)
{
    u = unitOr(glm::cross(n, std::abs(n.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0)), glm::dvec3(1, 0, 0));
    v = glm::cross(n, u);
}

std::optional<Part> segmentPart(const ViewInfo& view, int id, const glm::dvec3& a, const glm::dvec3& b, int axis, Tip tip)
{
    auto pa = view.project(a), pb = view.project(b);
    if (!pa || !pb)
        return std::nullopt;
    Part p{.id = id, .line = {*pa, *pb}, .axis = axis, .tip = tip, .tipAt = *pb};
    QPointF d = *pb - *pa;
    const double len = std::hypot(d.x(), d.y());
    // Edge-on the arrow collapses to a dot; leave it out rather than flicker.
    if (len < 6.0)
        return std::nullopt;
    p.tipDir = d / len;
    return p;
}

std::optional<Part> ringPart(const ViewInfo& view, int id, const glm::dvec3& centre, const glm::dvec3& normal, double radius, int axis)
{
    glm::dvec3 u, v;
    planeBasis(normal, u, v);
    Part p{.id = id, .closed = true, .axis = axis};
    for (int k = 0; k < kRingSegments; ++k)
    {
        const double a = 2.0 * 3.14159265358979 * k / kRingSegments;
        auto s = view.project(centre + radius * (std::cos(a) * u + std::sin(a) * v));
        if (!s)
            return std::nullopt;
        p.line.push_back(*s);
    }
    return p;
}

/** The parts of one gizmo, ids: Linear 0; Dial 0; Transform 0-2 move, 3-5 rotate, 6-8 scale by axis. */
std::vector<Part> layoutGizmo(const ViewInfo& view, const glm::dmat4& world, const GizmoSpec& s)
{
    std::vector<Part> out;
    const glm::dmat3 linear(world);
    switch (s.kind)
    {
    case GizmoSpec::Kind::Linear:
    {
        const glm::dvec3 base = glm::dvec3(world * glm::dvec4(s.position, 1.0));
        const glm::dvec3 dir = unitOr(linear * s.direction, glm::dvec3(0, 1, 0));
        const Tip tip = s.style == "box" ? Tip::Box : s.style == "cross" ? Tip::Cross : Tip::Arrow;
        if (auto p = segmentPart(view, 0, base, base + dir * kArrowPx * view.unitsPerPixel(base), -1, tip))
            out.push_back(*p);
        break;
    }
    case GizmoSpec::Kind::Dial:
    {
        const glm::dvec3 centre = glm::dvec3(world * glm::dvec4(s.position, 1.0));
        const glm::dvec3 n = unitOr(glm::transpose(glm::inverse(linear)) * s.direction, glm::dvec3(0, 1, 0));
        const double r = kDialPx * s.radius * view.unitsPerPixel(centre);
        if (auto p = ringPart(view, 0, centre, n, r, -1))
        {
            glm::dvec3 u, v;
            planeBasis(n, u, v);
            p->marker = view.project(centre + r * (std::cos(s.value) * u + std::sin(s.value) * v));
            out.push_back(*p);
        }
        break;
    }
    case GizmoSpec::Kind::Transform:
    {
        const glm::dmat4 m = world * s.matrix;
        const glm::dvec3 origin(m[3]);
        const double upp = view.unitsPerPixel(origin);
        for (int a = 0; a < 3; ++a)
        {
            glm::dvec3 e(0.0);
            e[a] = 1.0;
            const glm::dvec3 axis = unitOr(glm::dmat3(m) * e, e);
            if (s.translate)
                if (auto p = segmentPart(view, a, origin, origin + axis * kArrowPx * upp, a, Tip::Arrow))
                    out.push_back(*p);
            if (s.rotate)
                if (auto p = ringPart(view, 3 + a, origin, axis, kRingPx * upp, a))
                    out.push_back(*p);
            if (s.scale)
                if (auto at = view.project(origin + axis * kScaleBoxPx * upp))
                    out.push_back({.id = 6 + a, .line = {*at}, .axis = a, .tip = Tip::Box, .tipAt = *at});
        }
        break;
    }
    }
    return out;
}

double distanceToSegment(QPointF p, QPointF a, QPointF b)
{
    const QPointF ab = b - a;
    const double len2 = QPointF::dotProduct(ab, ab);
    const double t = len2 > 0.0 ? std::clamp(QPointF::dotProduct(p - a, ab) / len2, 0.0, 1.0) : 0.0;
    const QPointF d = p - (a + ab * t);
    return std::hypot(d.x(), d.y());
}

double distanceToPart(QPointF px, const Part& p)
{
    double best = 1e9;
    if (p.line.size() == 1)
        best = std::hypot(px.x() - p.line[0].x(), px.y() - p.line[0].y());
    for (size_t k = 0; k + 1 < p.line.size() + (p.closed ? 1 : 0); ++k)
        best = std::min(best, distanceToSegment(px, p.line[k], p.line[(k + 1) % p.line.size()]));
    if (p.tip != Tip::None)
        best = std::min(best, std::hypot(px.x() - p.tipAt.x(), px.y() - p.tipAt.y()) - 4.0);
    return best;
}

/** The part of a gizmo under px and how far from it, or -1. */
int pickParts(QPointF px, const std::vector<Part>& parts, double& distance)
{
    int hit = -1;
    distance = kPickPx;
    for (const Part& p : parts)
        if (double d = distanceToPart(px, p); d < distance)
        {
            distance = d;
            hit = p.id;
        }
    return hit;
}

void drawParts(OverlayPainter& p, const std::vector<Part>& parts, int hovered, int active)
{
    for (const Part& part : parts)
    {
        const bool hot = part.id == hovered || part.id == active;
        const glm::vec4 c = partColor(part.axis, hot);
        const float w = hot ? 3.0f : 2.0f;
        if (part.line.size() > 1)
            p.polyline(part.line, w, c, part.closed);
        const QPointF d = part.tipDir, side(-d.y(), d.x());
        switch (part.tip)
        {
        case Tip::Arrow: p.triangle(part.tipAt + d * 12.0, part.tipAt + side * 5.0, part.tipAt - side * 5.0, c); break;
        case Tip::Box: p.rect(QRectF(part.tipAt - QPointF(5, 5), QSizeF(10, 10)), c); break;
        case Tip::Cross:
            p.line(part.tipAt - QPointF(6, 6), part.tipAt + QPointF(6, 6), w, c);
            p.line(part.tipAt - QPointF(6, -6), part.tipAt + QPointF(6, -6), w, c);
            break;
        case Tip::Dot:
        case Tip::None: break;
        }
        if (part.marker)
            p.disc(*part.marker, hot ? 5.0f : 4.0f, c);
        // Axis letters keep a Transform Gizmo's axes apart without colour.
        if (part.axis >= 0 && part.tip == Tip::Arrow && !viewportDisplay::accentColors(viewportDisplay::kManipulatorAxisColors))
            p.label(part.tipAt + d * 20.0 - QPointF(4, -4), QString(QChar('X' + part.axis)), c);
    }
}

std::string paramLabel(const std::string& param) { return gn::GInput{.name = param}.displayName(); }

/**
 * A drag on one gizmo part: the value the gizmo's input gets as the cursor moves, from the spec at the press, in the
 * form a modifier attr holds (degrees for an angle input).
 */
class GizmoDrag
{
public:
    GizmoDrag(GizmoSpec spec, int part, const glm::dmat4& world, QPointF press, const ViewInfo& view)
        : mSpec(std::move(spec)), mPart(part), mWorld(world), mPress(press), mLast(press)
    {
        const glm::dmat3 linear(world);
        if (mSpec.kind == GizmoSpec::Kind::Linear || (mSpec.kind == GizmoSpec::Kind::Transform && part < 3) ||
            (mSpec.kind == GizmoSpec::Kind::Transform && part >= 6))
        {
            glm::dvec3 origin, dirObj;
            if (mSpec.kind == GizmoSpec::Kind::Linear)
            {
                origin = mSpec.position;
                dirObj = unitOr(mSpec.direction, glm::dvec3(0, 1, 0));
            }
            else
            {
                origin = glm::dvec3(mSpec.matrix[3]);
                glm::dvec3 e(0.0);
                e[part % 3] = 1.0;
                dirObj = unitOr(glm::dmat3(mSpec.matrix) * e, e);
            }
            mAxisObj = dirObj;
            const glm::dvec3 base(world * glm::dvec4(origin, 1.0));
            const glm::dvec3 dirWorld = linear * dirObj;
            mWorldPerObj = std::max(glm::length(dirWorld), 1e-12);
            const double span = view.unitsPerPixel(base) * 50.0;
            auto a = view.project(base), b = view.project(base + glm::normalize(dirWorld) * span);
            QPointF d = a && b ? *b - *a : QPointF();
            double len = std::hypot(d.x(), d.y());
            if (len < 5.0)
            {
                d = QPointF(1, 0);
                len = 50.0;
            }
            mPxDir = d / len;
            mWorldPerPx = span / len;
        }
        else
        {
            const glm::dvec3 centre = mSpec.kind == GizmoSpec::Kind::Dial ? mSpec.position : glm::dvec3(mSpec.matrix[3]);
            glm::dvec3 axisObj = mSpec.direction;
            if (mSpec.kind == GizmoSpec::Kind::Transform)
            {
                glm::dvec3 e(0.0);
                e[part % 3] = 1.0;
                axisObj = glm::dmat3(mSpec.matrix) * e;
            }
            const glm::dvec3 c(world * glm::dvec4(centre, 1.0));
            mCentre = view.project(c).value_or(press);
            const glm::dvec3 n = unitOr(glm::transpose(glm::inverse(linear)) * axisObj, glm::dvec3(0, 1, 0));
            // Screen angles grow clockwise (y down); a ring facing the viewer turns the other way by the right-hand rule.
            mAngleSign = glm::dot(n, view.eye - c) > 0.0 ? -1.0 : 1.0;
            mLastAngle = std::atan2(press.y() - mCentre.y(), press.x() - mCentre.x());
        }
    }

    /** The new value, as JSON for the input, after moving to px. */
    json move(QPointF px, Qt::KeyboardModifiers mods)
    {
        const double fine = (mods & Qt::ShiftModifier) ? 0.2 : 1.0;
        const bool along = mSpec.kind == GizmoSpec::Kind::Linear || (mSpec.kind == GizmoSpec::Kind::Transform && (mPart < 3 || mPart >= 6));
        if (along)
            mAmount += QPointF::dotProduct(px - mLast, mPxDir) * fine;
        else
        {
            const double a = std::atan2(px.y() - mCentre.y(), px.x() - mCentre.x());
            double d = a - mLastAngle;
            d = std::remainder(d, 2.0 * 3.14159265358979);
            mLastAngle = a;
            mAmount += mAngleSign * d * fine;
        }
        mLast = px;
        return current();
    }

    json current() const
    {
        switch (mSpec.kind)
        {
        case GizmoSpec::Kind::Linear:
        {
            const double objDist = mAmount * mWorldPerPx / mWorldPerObj;
            return toInput(mSpec.value + objDist / std::max(glm::length(mSpec.direction), 1e-12));
        }
        case GizmoSpec::Kind::Dial: return toInput(mSpec.value + mAmount);
        case GizmoSpec::Kind::Transform:
        {
            glm::dmat4 m = mSpec.matrix;
            glm::dvec3 e(0.0);
            e[mPart % 3] = 1.0;
            if (mPart < 3)
                m[3] += glm::dvec4(mAxisObj * (mAmount * mWorldPerPx / mWorldPerObj), 0.0);
            else if (mPart < 6)
                m = m * glm::rotate(glm::dmat4(1.0), mAmount, e);
            else
            {
                glm::dvec3 f(1.0);
                f[mPart % 3] = std::max(0.01, std::exp2(mAmount / kPxPerScale));
                m = m * glm::scale(glm::dmat4(1.0), f);
            }
            json j = json::array();
            for (int k = 0; k < 16; ++k)
                j.push_back(m[k / 4][k % 4]);
            return j;
        }
        }
        return nullptr;
    }

    QString readout() const
    {
        const QString name = QString::fromStdString(paramLabel(mSpec.param));
        const json v = current();
        if (mSpec.kind == GizmoSpec::Kind::Transform)
        {
            const char* what = mPart < 3 ? "Move" : mPart < 6 ? "Rotate" : "Scale";
            QString amount = mPart < 3 ? QString::number(mAmount * mWorldPerPx / mWorldPerObj, 'f', 2)
                             : mPart < 6 ? QString::number(glm::degrees(mAmount), 'f', 1) + QChar(0x00B0)
                                         : QString::number(std::exp2(mAmount / kPxPerScale), 'f', 3);
            return QString("%1 %2 %3  %4").arg(name, what, QString(QChar('X' + mPart % 3)), amount);
        }
        if (mSpec.subtype == "angle")
            return QString("%1 %2%3").arg(name).arg(v.get<double>(), 0, 'f', 1).arg(QChar(0x00B0));
        if (mSpec.paramType == gn::SocketType::Int)
            return QString("%1 %2").arg(name).arg(v.get<int64_t>());
        return QString("%1 %2").arg(name).arg(v.get<double>(), 0, 'g', 4);
    }

    const GizmoSpec& spec() const { return mSpec; }

private:
    json toInput(double graphValue) const
    {
        if (mSpec.subtype == "angle")
            return glm::degrees(graphValue);
        if (mSpec.paramType == gn::SocketType::Int)
            return int64_t(std::llround(graphValue));
        return graphValue;
    }

    GizmoSpec mSpec;
    int mPart;
    glm::dmat4 mWorld;
    QPointF mPress, mLast;
    QPointF mPxDir{1.0, 0.0};
    double mWorldPerPx = 1.0, mWorldPerObj = 1.0;
    glm::dvec3 mAxisObj{0.0, 1.0, 0.0};
    QPointF mCentre;
    double mAngleSign = 1.0, mLastAngle = 0.0;
    double mAmount = 0.0;
};

// --------------------------------------------------------------- modifier gizmos

/** A gn modifier on a mesh and the gizmos its group recorded. */
struct ModifierGizmos
{
    NodeId modifier = scene::kNoNode;
    std::shared_ptr<const gn::GizmoList> gizmos;
};

std::vector<ModifierGizmos> gizmosOn(const scene::Node& mesh)
{
    std::vector<ModifierGizmos> out;
    scene::Graph& g = scene::Graph::current();
    for (NodeId m : scene::modifiers::stack(g, mesh.id()))
        if (const scene::Node* n = g.node(m); n && gn::isGnModifier(*n))
            if (auto list = gn::modifierGizmos(m))
                out.push_back({m, std::move(list)});
    return out;
}

int encodeHandle(size_t modifier, size_t gizmo, int part) { return int(modifier << 16 | gizmo << 4) | part; }

/** Edits to modifier inputs, live with undo off; commit puts the start values back and sets the result by setAttr, one undo step. */
class GizmoPlugEdit
{
public:
    void set(NodeId id, const std::string& attr, const json& value)
    {
        scene::Graph& g = scene::Graph::current();
        scene::Node* n = g.node(id);
        const int index = n ? n->type().attrIndex(attr) : -1;
        if (index < 0)
            return;
        if (!mSaved)
            mSaved = Saved{id, index, g.storedValue({id, index})};
        scene::PlugValue v = mSaved->start;
        if (std::holds_alternative<double>(v) && value.is_number())
            v = value.get<double>();
        else if (std::holds_alternative<int64_t>(v) && value.is_number())
            v = int64_t(std::llround(value.get<double>()));
        else if (std::holds_alternative<glm::dmat4>(v) && value.is_array() && value.size() == 16)
        {
            glm::dmat4 m;
            for (int k = 0; k < 16; ++k)
                m[k / 4][k % 4] = value[size_t(k)].get<double>();
            v = m;
        }
        else
            return;
        UndoStack& u = UndoStack::get();
        const bool was = u.enabled();
        u.setEnabled(false);
        g.setValue({id, index}, std::move(v));
        u.setEnabled(was);
        mPlug = n->path() + "." + attr;
        mValue = value;
        viewportTools::requestRedraw();
    }

    void restore()
    {
        if (!mSaved)
            return;
        scene::Graph& g = scene::Graph::current();
        UndoStack& u = UndoStack::get();
        const bool was = u.enabled();
        u.setEnabled(false);
        if (g.node(mSaved->id))
            g.setValue({mSaved->id, mSaved->attr}, mSaved->start);
        u.setEnabled(was);
        viewportTools::requestRedraw();
    }

    void commit()
    {
        if (!mSaved || mPlug.empty())
            return;
        restore();
        try
        {
            ops::execute("setAttr", {{"args", {mPlug, mValue}}}, CallSource::UI);
        }
        catch (const std::exception& e)
        {
            log::write(LogLevel::Warning, e.what());
        }
    }

private:
    struct Saved
    {
        NodeId id;
        int attr;
        scene::PlugValue start;
    };
    std::optional<Saved> mSaved;
    std::string mPlug;
    json mValue;
};

class ModifierGizmoDrag final : public nodeHandles::Drag
{
public:
    ModifierGizmoDrag(NodeId modifier, GizmoSpec spec, int part, const glm::dmat4& world, QPointF press, const ViewInfo& view)
        : mModifier(modifier), mDrag(std::move(spec), part, world, press, view)
    {
    }

    void move(QPointF px, const ViewInfo&, Qt::KeyboardModifiers mods) override { mEdit.set(mModifier, mDrag.spec().param, mDrag.move(px, mods)); }
    void release(bool moved) override
    {
        if (moved)
            mEdit.commit();
    }
    void cancel() override { mEdit.restore(); }
    QString readout() const override { return mDrag.readout(); }

private:
    NodeId mModifier;
    GizmoDrag mDrag;
    GizmoPlugEdit mEdit;
};

void registerModifierGizmos()
{
    nodeHandles::add({
        .id = "gn.gizmos",
        .nodeType = "mesh",
        .draw =
            [](OverlayPainter& p, const nodeHandles::Context& c, int hovered, int active) {
                const std::vector<ModifierGizmos> mods = gizmosOn(c.node);
                for (size_t m = 0; m < mods.size(); ++m)
                    for (size_t k = 0; k < mods[m].gizmos->size(); ++k)
                    {
                        auto partOf = [&](int h) { return h >= 0 && (h >> 4) == encodeHandle(m, k, 0) >> 4 ? h & 15 : -1; };
                        drawParts(p, layoutGizmo(c.view, c.world, (*mods[m].gizmos)[k]), partOf(hovered), partOf(active));
                    }
            },
        .pick =
            [](QPointF px, const nodeHandles::Context& c) {
                const std::vector<ModifierGizmos> mods = gizmosOn(c.node);
                int best = -1;
                double bestDist = kPickPx;
                for (size_t m = 0; m < mods.size(); ++m)
                    for (size_t k = 0; k < mods[m].gizmos->size(); ++k)
                    {
                        double d = 0.0;
                        if (int part = pickParts(px, layoutGizmo(c.view, c.world, (*mods[m].gizmos)[k]), d); part >= 0 && d < bestDist)
                        {
                            bestDist = d;
                            best = encodeHandle(m, k, part);
                        }
                    }
                return best;
            },
        .begin = [](int handle, QPointF px, const nodeHandles::Context& c) -> std::unique_ptr<nodeHandles::Drag> {
            const std::vector<ModifierGizmos> mods = gizmosOn(c.node);
            const size_t m = size_t(handle >> 16), k = size_t((handle >> 4) & 0xfff);
            if (m >= mods.size() || k >= mods[m].gizmos->size())
                return nullptr;
            return std::make_unique<ModifierGizmoDrag>(mods[m].modifier, (*mods[m].gizmos)[k], handle & 15, c.world, px, c.view);
        },
    });
}

// --------------------------------------------------------------- tool sessions

/** The group a tool action runs, read fresh; null with a log line when it is gone or unreadable. */
std::optional<gn::Group> toolGroup(const std::string& name)
{
    scene::Node* n = gn::findGroupNode(scene::Graph::current(), name);
    if (!n)
        return std::nullopt;
    try
    {
        return gn::readGroup(*n);
    }
    catch (const std::exception& e)
    {
        log::write(LogLevel::Warning, std::string("node tool ") + name + ": " + e.what());
        return std::nullopt;
    }
}

bool hasGizmos(const gn::Group& g)
{
    return std::any_of(g.nodes.begin(), g.nodes.end(), [](const gn::GNode& n) { return gn::isGizmoType(n.type); });
}

json matrixJson(const glm::dmat4& m)
{
    json j = json::array();
    for (int k = 0; k < 16; ++k)
        j.push_back(m[k / 4][k % 4]);
    return j;
}

/** gnRunTool's call for a pointer in a view. */
json runCall(const std::string& group, QPointF px, const ViewInfo& view, const std::map<std::string, json>& values)
{
    glm::dvec3 origin, dir;
    view.ray(px, origin, dir);
    json v = json::object();
    for (const auto& [k, val] : values)
        v[k] = val;
    return {{"args", {group}},
            {"mouse", {px.x(), px.y()}},
            {"region", {view.width, view.height}},
            {"rayOrigin", {origin.x, origin.y, origin.z}},
            {"rayDirection", {dir.x, dir.y, dir.z}},
            {"view", matrixJson(view.view)},
            {"projection", matrixJson(view.proj)},
            {"orthographic", view.ortho},
            {"values", v}};
}

/**
 * A tool group held as a viewport tool: the selected meshes show what it would make as a wire, its gizmos edit its
 * inputs, and a click (or Enter) applies it at the pointer. Esc ends it.
 */
class NodeToolSession final : public ViewportTool
{
public:
    explicit NodeToolSession(std::string group) : mGroupName(std::move(group)) {}

    const char* name() const override { return "Node Tool"; }

    void activate() override
    {
        if (auto g = toolGroup(mGroupName))
        {
            mGroup = std::move(*g);
            for (const gn::GInput& in : mGroup.inputs)
                mValues[in.name] = in.defaultValue;
            mFollowsPointer = gn::readsPointer(mGroup);
        }
        else
            mError = "the group is gone";
        gatherTargets();
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton || (e->modifiers() & Qt::AltModifier))
            return false;
        mView = view;
        mHaveView = true;
        const QPointF px = e->position();
        if (auto hit = pick(px, view))
        {
            const Preview& pv = mPreviews[hit->target];
            mDrag.emplace(pv.gizmos[hit->gizmo], hit->part, pv.world, px, view);
            mActive = hit;
            return true;
        }
        apply(px, view);
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mView = view;
        mHaveView = true;
        mPointer = e->position();
        if (mDrag)
        {
            mValues[mDrag->spec().param] = mDrag->move(mPointer, e->modifiers());
            refreshPreview();
            return true;
        }
        std::optional<Hit> h = pick(mPointer, view);
        const bool changed = h.has_value() != mHover.has_value() || (h && mHover && !(*h == *mHover));
        mHover = h;
        if (mFollowsPointer)
            refreshPreview();
        if (changed || mFollowsPointer)
            viewportTools::requestRedraw();
        return false;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (e->button() != Qt::LeftButton || !mDrag)
            return false;
        mDrag.reset();
        mActive.reset();
        viewportTools::requestRedraw();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo& view) override { return handleKey(*e, &view); }
    bool hotkey(const QKeyEvent& e, std::string_view) override { return handleKey(e, mHaveView ? &mView : nullptr); }

    void draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();

        mView = view;
        mHaveView = true;
        if (mPreviews.empty() && !mTargets.empty() && mError.isEmpty())
            refreshPreview();
        const glm::vec4 wire = hotColor() * glm::vec4(1.0f, 1.0f, 1.0f, 0.85f);
        for (const Preview& pv : mPreviews)
            for (const auto& [a, b] : pv.edges)
            {
                s.linesOnTop.push_back({glm::dvec3(pv.world * glm::dvec4(a, 1.0)), wire});
                s.linesOnTop.push_back({glm::dvec3(pv.world * glm::dvec4(b, 1.0)), wire});
            }
        for (size_t t = 0; t < mPreviews.size(); ++t)
            for (size_t k = 0; k < mPreviews[t].gizmos.size(); ++k)
            {
                auto partOf = [&](const std::optional<Hit>& h) { return h && h->target == t && h->gizmo == k ? h->part : -1; };
                drawParts(p, layoutGizmo(view, mPreviews[t].world, mPreviews[t].gizmos[k]), partOf(mHover), partOf(mActive));
            }
        if (mDrag)
            p.label(mPointer + QPointF(18, -14), mDrag->readout());

        ToolStatus st;
        st.tool = QString::fromStdString(paramLabel(mGroupName));
        st.subject = mTargets.size() == 1 ? QString::fromStdString(targetName(mTargets.front())) : mTargets.empty() ? QString() : QString("%1 meshes").arg(mTargets.size());
        for (const gn::GInput& in : mGroup.inputs)
            if (auto it = mValues.find(in.name); it != mValues.end() && it->second.is_number())
                st.fields.push_back({QString::fromStdString(in.displayName()), QString::number(it->second.get<double>(), 'g', 4)});
        st.prompt = mTargets.empty() ? "select a mesh" : mError.isEmpty() ? "click to apply" : mError;
        st.keys = {{"LMB", "apply"}, {"LMB drag handle", "edit input"}, {"Shift", "fine drag"}, {"Enter", "apply"}, {"Esc", "exit"}};
        drawToolStatus(p, view, st);
    }

private:
    struct Preview
    {
        NodeId node = scene::kNoNode;
        glm::dmat4 world{1.0};
        std::vector<std::pair<glm::dvec3, glm::dvec3>> edges;
        gn::GizmoList gizmos;
    };
    struct Hit
    {
        size_t target = 0, gizmo = 0;
        int part = -1;
        bool operator==(const Hit&) const = default;
    };

    static std::string targetName(NodeId id)
    {
        const scene::Node* n = scene::Graph::current().node(id);
        return n ? n->name() : std::string();
    }

    bool handleKey(const QKeyEvent& e, const ViewInfo* view)
    {
        if (e.key() == Qt::Key_Escape)
        {
            QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
            return true;
        }
        if ((e.key() == Qt::Key_Return || e.key() == Qt::Key_Enter) && view)
        {
            apply(mPointer, *view);
            return true;
        }
        return false;
    }

    void gatherTargets()
    {
        mTargets.clear();
        scene::Graph& g = scene::Graph::current();
        for (NodeId id : g.selection())
            if (const scene::Node* n = g.node(id); n && n->type().isA("mesh"))
                mTargets.push_back(id);
        mPreviews.clear();
    }

    void refreshPreview()
    {
        mPreviews.clear();
        mError.clear();
        scene::Graph& g = scene::Graph::current();
        gn::ToolContext base;
        glm::dvec3 origin(0.0), dir(0.0, 0.0, -1.0);
        if (mHaveView)
        {
            base.mouse = {mPointer.x(), mPointer.y()};
            base.region = {mView.width, mView.height};
            base.view = mView.view;
            base.projection = mView.proj;
            base.orthographic = mView.ortho;
            mView.ray(mPointer, origin, dir);
        }
        for (NodeId id : mTargets)
        {
            const scene::Node* n = g.node(id);
            if (!n)
                continue;
            gn::ToolTarget t = gn::toolTarget(g, id, base, origin, dir);
            try
            {
                gn::ToolResult r = gn::runTool(mGroup, t.mesh, t.selection ? &*t.selection : nullptr, t.ctx, mValues);
                Preview pv{.node = id, .world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix), .gizmos = std::move(r.gizmos)};
                if (r.mesh.numEdges() <= kMaxPreviewEdges)
                    for (geo::Index e = 0; e < r.mesh.edgeCapacity(); ++e)
                        if (r.mesh.edgeValid(e))
                            pv.edges.emplace_back(r.mesh.position(r.mesh.edgePoint(e, 0)), r.mesh.position(r.mesh.edgePoint(e, 1)));
                mPreviews.push_back(std::move(pv));
            }
            catch (const std::exception& e)
            {
                mError = QString::fromStdString(e.what());
            }
        }
        viewportTools::requestRedraw();
    }

    std::optional<Hit> pick(QPointF px, const ViewInfo& view) const
    {
        std::optional<Hit> best;
        double bestDist = kPickPx;
        for (size_t t = 0; t < mPreviews.size(); ++t)
            for (size_t k = 0; k < mPreviews[t].gizmos.size(); ++k)
            {
                double d = 0.0;
                if (int part = pickParts(px, layoutGizmo(view, mPreviews[t].world, mPreviews[t].gizmos[k]), d); part >= 0 && d < bestDist)
                {
                    bestDist = d;
                    best = Hit{t, k, part};
                }
            }
        return best;
    }

    void apply(QPointF px, const ViewInfo& view)
    {
        if (mTargets.empty())
            return;
        try
        {
            ops::execute("gnRunTool", runCall(mGroupName, px, view, mValues), CallSource::UI);
            mError.clear();
        }
        catch (const std::exception& e)
        {
            mError = QString::fromStdString(e.what());
            log::write(LogLevel::Warning, e.what());
        }
        gatherTargets();
        refreshPreview();
    }

    std::string mGroupName;
    gn::Group mGroup;
    std::map<std::string, json> mValues;
    bool mFollowsPointer = false;
    std::vector<NodeId> mTargets;
    std::vector<Preview> mPreviews;
    std::optional<Hit> mHover, mActive;
    std::optional<GizmoDrag> mDrag;
    ViewInfo mView;
    bool mHaveView = false;
    QPointF mPointer;
    QString mError;
};

/** Runs a tool: at once when it has no gizmos and ignores the pointer, else as a viewport tool. */
void startTool(const std::string& group)
{
    std::optional<gn::Group> g = toolGroup(group);
    if (!g)
        return;
    if (!hasGizmos(*g) && !gn::readsPointer(*g))
    {
        actions::runWithOptions("gnRunTool", {{"args", {group}}});
        return;
    }
    viewportTools::set(std::make_unique<NodeToolSession>(group));
}

// --------------------------------------------------------------- actions and menus

constexpr const char* kMenu = "Mesh/Node Tools";
constexpr const char* kToggleAction = "nodeEditor.toggleNodeTool";

std::string actionId(const std::string& group) { return "gnTool." + group; }

/** The geometry-nodes group open in the active Node Editor, or empty. */
std::string editorGroup()
{
    NodeEditor* e = NodeEditor::active();
    if (!e || e->backend().id() != kGeometryNodesBackend)
        return {};
    return e->group().toStdString();
}

std::set<std::string>& registeredTools()
{
    static std::set<std::string> s;
    return s;
}

/** Brings the tool actions and their menu rows in line with the scene's tool groups. */
void syncTools()
{
    std::vector<std::string> now = gn::toolGroups(scene::Graph::current());
    std::set<std::string> wanted(now.begin(), now.end());
    for (auto it = registeredTools().begin(); it != registeredTools().end();)
        if (!wanted.contains(*it))
        {
            actions::remove(actionId(*it));
            menus::removeItem(std::string(kMenu) + "/" + actionId(*it));
            it = registeredTools().erase(it);
        }
        else
            ++it;
    for (const std::string& name : now)
    {
        std::optional<gn::Group> g = toolGroup(name);
        const std::string label = paramLabel(name);
        actions::add({.id = actionId(name),
                      .label = label,
                      .category = "Node Tools",
                      .run = [name] { startTool(name); },
                      .enabled = [] { return actions::hasSelection(); },
                      .doc = g && !g->description.empty() ? g->description : "Run the " + label + " node tool on the selected meshes.",
                      .python = "lila.ops.gnRunTool(\"" + name + "\")",
                      .icon = "tool",
                      .keywords = {"node tool", "tool", "geometry nodes", name}});
        if (registeredTools().insert(name).second)
            menus::addItem({.id = std::string(kMenu) + "/" + actionId(name), .menu = kMenu, .action = actionId(name)});
    }
}

void scheduleSync()
{
    static bool pending = false;
    if (pending)
        return;
    pending = true;
    QTimer::singleShot(0, [] {
        pending = false;
        syncTools();
    });
}

void registerActions()
{
    actions::add({.id = kToggleAction,
                  .label = "Use as Node Tool",
                  .category = "Node Tools",
                  .run =
                      [] {
                          const std::string group = editorGroup();
                          if (group.empty())
                              return;
                          const std::optional<gn::Group> g = toolGroup(group);
                          actions::run(json{{"command", "gnSetTool"}, {"args", {{"args", {group}}, {"tool", !(g && g->tool)}}}});
                      },
                  .enabled = [] { return !editorGroup().empty(); },
                  .doc = "Make the group open in the Node Editor a node tool: it runs on the selected meshes from Mesh > Node Tools, with "
                         "its gizmos as viewport handles. Again to make it a modifier group only.",
                  .python = "lila.ops.gnSetTool(\"MyGroup\")",
                  .checked =
                      [] {
                          const std::string group = editorGroup();
                          const std::optional<gn::Group> g = group.empty() ? std::nullopt : toolGroup(group);
                          return g && g->tool;
                      },
                  .icon = "tool",
                  .keywords = {"node tool", "tool group", "geometry nodes tool", "operator"}});
    menus::addItem({.id = "Mesh/Node Tools/", .menu = "Mesh", .kind = menus::Kind::Submenu, .label = "Node Tools", .after = "Mesh/nodeEditor.showHistory"});
    menus::addItem({.menu = kMenu, .action = kToggleAction});
    menus::addItem({.id = std::string(kMenu) + "/-", .menu = kMenu, .kind = menus::Kind::Separator});

    scene::Graph::current().addListener([](scene::Change c, NodeId id) {
        if (c == scene::Change::Value || c == scene::Change::Renamed)
        {
            const scene::Node* n = scene::Graph::current().node(id);
            if (!n || n->type().name != gn::kGroupNodeType)
                return;
        }
        else if (c == scene::Change::Selection || c == scene::Change::Reparented || c == scene::Change::Connection || c == scene::Change::Attrs)
            return;
        scheduleSync();
    });
}
} // namespace

void registerNodeToolUi()
{
    registerModifierGizmos();
    registerActions();
}
} // namespace lila::ui::nodeTools
