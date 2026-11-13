#include "ui/ArrayTool.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "ops/Command.h"
#include "scene/ArrayGroup.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"
#include "scene/TransformOps.h"
#include "ui/Actions.h"
#include "ui/MeshCache.h"
#include "ui/ToolColors.h"
#include "ui/ViewportToolMath.h"

#include <glm/gtc/matrix_transform.hpp>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <format>

namespace lila::ui
{
namespace
{
using scene::NodeId;

constexpr double kHandleRadius = 10.0; // px
constexpr double kClickSlop = 4.0;     // px
constexpr double kCountReach = 18.0;   // px past the last copy, so a count handle never sits on a spacing handle
constexpr int kMaxCount = scene::arrays::kMaxCount;

const glm::vec4 kIdle(0.72f, 0.72f, 0.72f, 1.0f);
const glm::vec4 kGuide(0.85f, 0.85f, 0.85f, 0.8f);
const glm::vec4 kPreviewWire(0.55f, 0.55f, 0.55f, 1.0f);

const char* arrayModeName(geo::ArrayMode m)
{
    switch (m)
    {
    case geo::ArrayMode::Radial: return "radial";
    case geo::ArrayMode::Grid: return "grid";
    case geo::ArrayMode::Linear: break;
    }
    return "linear";
}

/** The parameter along the line (p, unit u) of its closest point to the ray (o, unit d). */
double lineParam(const glm::dvec3& p, const glm::dvec3& u, const glm::dvec3& o, const glm::dvec3& d)
{
    const glm::dvec3 w = p - o;
    const double b = glm::dot(u, d), denom = 1.0 - b * b;
    if (denom < 1e-9)
        return 0.0;
    return (b * glm::dot(d, w) - glm::dot(u, w)) / denom;
}

/** Degrees from `from` to `to` about unit `axis`, right handed, in (-180, 180]. */
double angleAbout(const glm::dvec3& from, const glm::dvec3& to, const glm::dvec3& axis)
{
    return glm::degrees(std::atan2(glm::dot(glm::cross(from, to), axis), glm::dot(from, to)));
}

glm::dvec3 rotateAbout(const glm::dvec3& v, const glm::dvec3& axis, double degrees)
{
    return glm::dvec3(glm::rotate(glm::dmat4(1.0), glm::radians(degrees), axis) * glm::dvec4(v, 0.0));
}

QString cm(double v) { return QString::number(v, 'f', std::abs(v) < 10.0 ? 2 : 1) + " cm"; }
} // namespace

bool ArrayTool::begin()
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    std::vector<NodeId> ids;
    for (NodeId id : g.selection())
        if (scene::Node* n = g.node(id); n && n->type().isA("transform"))
            ids.push_back(id);
    mSources = scene::xformops::topLevel(g, ids);
    if (mSources.empty())
        return false;
    NodeId parent = g.parent(mSources.front());
    for (NodeId s : mSources)
        if (g.parent(s) != parent)
            parent = scene::kNoNode;
    mParentWorld = parent != scene::kNoNode ? g.node(parent)->as<glm::dmat4>(scene::Transform::kWorldMatrix) : glm::dmat4(1.0);
    mPivot = glm::dvec3(0.0);
    for (NodeId s : mSources)
        mPivot += scene::xformops::worldPivot(g, s);
    mPivot /= double(mSources.size());
    mSettings.center = mPivot;
    mNormal = scene::sceneUp();
    mSettings.axis = 0;
    for (int i = 1; i < 3; ++i)
        if (std::abs(glm::dot(worldAxis(i), mNormal)) > std::abs(glm::dot(worldAxis(mSettings.axis), mNormal)))
            mSettings.axis = i;
    mSettings.gridCount = glm::ivec3(3);
    mSettings.gridCount[mSettings.axis] = 1;
    return true;
}

void ArrayTool::setMode(geo::ArrayMode mode)
{
    if (mode == geo::ArrayMode::Radial && !mRadialSet)
    {
        mRadialSet = true;
        mSettings.count = 8;
    }
    if (mode != geo::ArrayMode::Radial)
        mSettings.center = mPivot;
    mSettings.mode = mode;
    mPlaced = true;
    viewportTools::requestRedraw();
}

void ArrayTool::setCount(int count)
{
    if (mSettings.mode == geo::ArrayMode::Grid)
        mSettings.gridCount[planeAxes().first] = std::clamp(count, 1, kMaxCount);
    else
        mSettings.count = std::clamp(count, 1, kMaxCount);
    viewportTools::requestRedraw();
}

glm::dmat3 ArrayTool::axes() const { return glm::dmat3(mParentWorld); }

glm::dmat4 ArrayTool::arrayWorld() const
{
    const glm::dvec3 local(glm::inverse(mParentWorld) * glm::dvec4(mSettings.center, 1.0));
    return mParentWorld * glm::translate(glm::dmat4(1.0), local);
}

glm::dvec3 ArrayTool::worldAxis(int local) const
{
    const glm::dvec3 a = axes()[local];
    const double len = glm::length(a);
    return len > 1e-12 ? a / len : glm::dvec3(local == 0, local == 1, local == 2);
}

std::vector<glm::dmat4> ArrayTool::copyTransforms() const
{
    geo::ArrayLayout l;
    l.mode = mSettings.mode;
    l.count = std::clamp(mSettings.count, 1, kMaxCount);
    l.gridCount = glm::clamp(mSettings.gridCount, glm::ivec3(1), glm::ivec3(kMaxCount));
    while (int64_t(l.gridCount.x) * l.gridCount.y * l.gridCount.z > scene::arrays::kMaxTotal)
        l.gridCount = glm::max(l.gridCount / 2, glm::ivec3(1));
    l.offset = l.mode == geo::ArrayMode::Grid ? mSettings.spacing : l.mode == geo::ArrayMode::Linear ? mSettings.offset : glm::dvec3(0.0);
    l.axis = mSettings.axis;
    l.angle = mSettings.angle;
    l.rise = mSettings.rise;
    std::vector<glm::dmat4> out = geo::arrayCopyTransforms(l);
    const glm::dmat4 w = arrayWorld(), wi = glm::inverse(w);
    for (size_t k = 1; k < out.size(); ++k)
        out[k] = w * out[k] * wi;
    return out;
}

glm::dvec3 ArrayTool::copyPivot(int k) const
{
    const std::vector<glm::dmat4> xf = copyTransforms();
    return glm::dvec3(xf[size_t(std::clamp(k, 0, int(xf.size()) - 1))] * glm::dvec4(mPivot, 1.0));
}

std::pair<int, int> ArrayTool::planeAxes() const
{
    const int n = mSettings.axis;
    return {n == 0 ? 1 : 0, n == 2 ? 1 : 2};
}

glm::dvec3 ArrayTool::gridPivot(int a, int b) const
{
    auto [ia, ib] = planeAxes();
    glm::dvec3 cell(0.0);
    cell[ia] = a;
    cell[ib] = b;
    return mPivot + axes() * (mSettings.spacing * cell);
}

glm::dvec3 ArrayTool::ringStart() const
{
    const glm::dvec3 axis = worldAxis(mSettings.axis);
    glm::dvec3 v = mPivot - mSettings.center;
    v -= axis * glm::dot(v, axis);
    // A source on the axis has no radius to turn about; the handles still need somewhere to sit.
    if (glm::length(v) < 1e-6)
        return worldAxis(planeAxes().first) * 50.0;
    return v;
}

double ArrayTool::ringRadius() const { return glm::length(ringStart()); }

std::optional<glm::dvec3> ArrayTool::planeHit(QPointF px, const ViewInfo& view) const
{
    glm::dvec3 o, d;
    view.ray(px, o, d);
    const double dn = glm::dot(d, mNormal);
    if (std::abs(dn) < 1e-9)
        return std::nullopt;
    return o + d * (glm::dot(mPivot - o, mNormal) / dn);
}

void ArrayTool::choosePlane(QPointF px, const ViewInfo& view)
{
    glm::dvec3 o, d;
    view.ray(px, o, d);
    int best = 0;
    for (int i = 1; i < 3; ++i)
        if (std::abs(d[i]) > std::abs(d[best]))
            best = i;
    mNormal = glm::dvec3(0.0);
    mNormal[best] = 1.0;
    int axis = 0;
    for (int i = 1; i < 3; ++i)
        if (std::abs(glm::dot(worldAxis(i), mNormal)) > std::abs(glm::dot(worldAxis(axis), mNormal)))
            axis = i;
    if (axis != mSettings.axis)
    {
        mSettings.gridCount[mSettings.axis] = mSettings.gridCount[axis];
        mSettings.gridCount[axis] = 1;
        mSettings.axis = axis;
    }
}

void ArrayTool::placeFrom(const glm::dvec3& from, const glm::dvec3& to)
{
    const glm::dvec3 local = glm::inverse(axes()) * (to - from);
    if (glm::length(local) < 1e-6)
        return;
    if (mSettings.mode == geo::ArrayMode::Grid)
    {
        auto [a, b] = planeAxes();
        mSettings.spacing[a] = local[a];
        mSettings.spacing[b] = local[b];
        // A drag along one axis makes square cells.
        if (std::abs(local[b]) < 0.2 * std::abs(local[a]))
            mSettings.spacing[b] = std::abs(local[a]);
        else if (std::abs(local[a]) < 0.2 * std::abs(local[b]))
            mSettings.spacing[a] = std::abs(local[b]);
    }
    else
    {
        mSettings.mode = geo::ArrayMode::Linear;
        mSettings.offset = local;
    }
    mSettings.center = mPivot;
    mPlaced = true;
}

std::optional<QPointF> ArrayTool::handlePos(Handle h, const ViewInfo& view) const
{
    auto beyond = [&](const glm::dvec3& last, const glm::dvec3& before) -> std::optional<QPointF> {
        std::optional<QPointF> a = view.project(last), b = view.project(before);
        if (!a)
            return std::nullopt;
        QPointF dir = b ? *a - *b : QPointF(1, 0);
        const double len = std::hypot(dir.x(), dir.y());
        dir = len > 1e-6 ? dir / len : QPointF(1, 0);
        return *a + dir * kCountReach;
    };
    if (!mPlaced)
        return std::nullopt;
    switch (mSettings.mode)
    {
    case geo::ArrayMode::Linear:
        if (h == Handle::Spacing)
            return view.project(copyPivot(1));
        if (h == Handle::Count)
            return beyond(copyPivot(mSettings.count - 1), mPivot);
        return std::nullopt;
    case geo::ArrayMode::Grid:
    {
        auto [a, b] = planeAxes();
        const int na = mSettings.gridCount[a], nb = mSettings.gridCount[b];
        if (h == Handle::Spacing)
            return view.project(gridPivot(1, 0));
        if (h == Handle::SpacingB)
            return view.project(gridPivot(0, 1));
        if (h == Handle::Count)
            return beyond(gridPivot(std::max(na - 1, 1), 0), mPivot);
        if (h == Handle::CountB)
            return beyond(gridPivot(0, std::max(nb - 1, 1)), mPivot);
        return std::nullopt;
    }
    case geo::ArrayMode::Radial:
    {
        const glm::dvec3 axis = worldAxis(mSettings.axis);
        geo::ArrayLayout l;
        l.mode = geo::ArrayMode::Radial;
        l.count = mSettings.count;
        l.angle = mSettings.angle;
        const double step = geo::arrayStepAngle(l);
        const int n = std::max(mSettings.count, 1);
        if (h == Handle::Step)
            return view.project(mSettings.center + rotateAbout(ringStart(), axis, step) + axis * mSettings.rise);
        if (h == Handle::Angle)
            return view.project(mSettings.center + rotateAbout(ringStart() * 1.25, axis, mSettings.angle) + axis * (mSettings.rise * (n - 1)));
        if (h == Handle::Rise)
            return view.project(mSettings.center + axis * (mSettings.rise * (n - 1)));
        return std::nullopt;
    }
    }
    return std::nullopt;
}

ArrayTool::Handle ArrayTool::handleAt(QPointF px, const ViewInfo& view) const
{
    Handle best = Handle::None;
    double bestD = kHandleRadius;
    for (Handle h : {Handle::Spacing, Handle::Count, Handle::SpacingB, Handle::CountB, Handle::Angle, Handle::Step, Handle::Rise})
        if (std::optional<QPointF> at = handlePos(h, view); at && distPx(*at, px) < bestD)
        {
            bestD = distPx(*at, px);
            best = h;
        }
    return best;
}

bool ArrayTool::press(QPointF px, const ViewInfo& view)
{
    mDownPx = px;
    mMoved = false;
    mHandle = handleAt(px, view);
    if (mHandle != Handle::None)
    {
        mDrag = Drag::Handle;
        if (mHandle == Handle::Angle)
            mDragAngle = mSettings.angle;
        return true;
    }
    choosePlane(px, view);
    std::optional<glm::dvec3> hit = planeHit(px, view);
    if (!hit)
        return false;
    mDownWorld = *hit;
    mDrag = Drag::Place;
    return true;
}

void ArrayTool::drag(QPointF px, const ViewInfo& view)
{
    if (distPx(px, mDownPx) > kClickSlop)
        mMoved = true;
    if (mDrag == Drag::Handle)
        dragHandle(px, view);
    else if (mDrag == Drag::Place && mMoved)
        if (std::optional<glm::dvec3> hit = planeHit(px, view))
            placeFrom(mDownWorld, *hit);
    viewportTools::requestRedraw();
}

void ArrayTool::release(QPointF px, const ViewInfo& view)
{
    if (mDrag == Drag::Place && !mMoved)
    {
        // A click places a ring's centre.
        mSettings.center = mDownWorld;
        const bool first = !mRadialSet;
        mRadialSet = true;
        if (first || mSettings.mode != geo::ArrayMode::Radial)
            mSettings.count = std::max(mSettings.count, first ? 8 : 2);
        mSettings.mode = geo::ArrayMode::Radial;
        mPlaced = true;
    }
    else
        drag(px, view);
    mDrag = Drag::None;
    mHandle = Handle::None;
    viewportTools::requestRedraw();
}

void ArrayTool::dragHandle(QPointF px, const ViewInfo& view)
{
    glm::dvec3 o, d;
    view.ray(px, o, d);
    const glm::dmat3 toLocal = glm::inverse(axes());
    auto along = [&](const glm::dvec3& from, const glm::dvec3& dirWorld) { return lineParam(from, dirWorld, o, d); };
    switch (mHandle)
    {
    case Handle::Spacing:
    case Handle::Count:
    case Handle::SpacingB:
    case Handle::CountB:
    {
        if (mSettings.mode == geo::ArrayMode::Linear)
        {
            const glm::dvec3 step = axes() * mSettings.offset;
            const double len = glm::length(step);
            if (len < 1e-9)
                return;
            const glm::dvec3 dir = step / len;
            const double t = along(mPivot, dir);
            if (mHandle == Handle::Spacing)
            {
                if (std::abs(t) > 1e-3)
                    mSettings.offset = toLocal * (dir * t);
            }
            else
                mSettings.count = std::clamp(int(std::lround(t / len)) + 1, 2, kMaxCount);
            return;
        }
        auto [a, b] = planeAxes();
        const bool second = mHandle == Handle::SpacingB || mHandle == Handle::CountB;
        const int axis = second ? b : a;
        const glm::dvec3 dir = worldAxis(axis);
        const double t = along(mPivot, dir);
        const double scale = glm::length(axes()[axis]);
        if (mHandle == Handle::Spacing || mHandle == Handle::SpacingB)
        {
            if (std::abs(t) > 1e-3)
                mSettings.spacing[axis] = t / std::max(scale, 1e-12);
        }
        else
        {
            const double cell = std::abs(mSettings.spacing[axis]) * scale;
            if (cell > 1e-9)
                mSettings.gridCount[axis] = std::clamp(int(std::lround(std::abs(t) / cell)) + 1, 1, kMaxCount);
        }
        return;
    }
    case Handle::Step:
    case Handle::Angle:
    {
        const glm::dvec3 axis = worldAxis(mSettings.axis);
        const double dn = glm::dot(d, axis);
        if (std::abs(dn) < 1e-9)
            return;
        glm::dvec3 v = o + d * (glm::dot(mSettings.center - o, axis) / dn) - mSettings.center;
        v -= axis * glm::dot(v, axis);
        if (glm::length(v) < 1e-9)
            return;
        double a = angleAbout(ringStart(), v, axis);
        if (mHandle == Handle::Step)
        {
            if (a <= 0.0)
                a += 360.0;
            const bool ring = std::abs(std::abs(mSettings.angle) - 360.0) < 1e-6;
            const int n = ring ? int(std::lround(360.0 / a)) : int(std::lround(mSettings.angle / a)) + 1;
            mSettings.count = std::clamp(n, 2, kMaxCount);
            return;
        }
        // Unwrapped against the last value so the arc can sweep past 180 and on to a full turn.
        double next = a;
        while (next - mDragAngle > 180.0)
            next -= 360.0;
        while (next - mDragAngle < -180.0)
            next += 360.0;
        mDragAngle = next;
        double angle = std::clamp(std::round(next), 1.0, 360.0);
        if (QGuiApplication::queryKeyboardModifiers() & Qt::ControlModifier)
            angle = std::clamp(std::round(angle / 15.0) * 15.0, 15.0, 360.0);
        if (angle > 355.0)
            angle = 360.0;
        mSettings.angle = angle;
        return;
    }
    case Handle::Rise:
    {
        const int n = std::max(mSettings.count, 2);
        mSettings.rise = along(mSettings.center, worldAxis(mSettings.axis)) / double(n - 1);
        return;
    }
    case Handle::None: return;
    }
}

bool ArrayTool::key(int key)
{
    switch (key)
    {
    case Qt::Key_1: setMode(geo::ArrayMode::Linear); return true;
    case Qt::Key_2: setMode(geo::ArrayMode::Radial); return true;
    case Qt::Key_3: setMode(geo::ArrayMode::Grid); return true;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
    case Qt::Key_Up:
        setCount((mSettings.mode == geo::ArrayMode::Grid ? mSettings.gridCount[planeAxes().first] : mSettings.count) + 1);
        return true;
    case Qt::Key_Minus:
    case Qt::Key_Down:
        setCount((mSettings.mode == geo::ArrayMode::Grid ? mSettings.gridCount[planeAxes().first] : mSettings.count) - 1);
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        finish();
        exit();
        return true;
    case Qt::Key_Escape:
        if (mDrag != Drag::None)
        {
            mDrag = Drag::None;
            mHandle = Handle::None;
        }
        else
            exit();
        viewportTools::requestRedraw();
        return true;
    default: return false;
    }
}

json ArrayTool::call() const
{
    scene::Graph& g = scene::Graph::current();
    json args = json::array();
    for (NodeId s : mSources)
        if (scene::Node* n = g.node(s))
            args.push_back(n->fullPath());
    json c = {{"args", args}, {"edit", false}, {"mode", arrayModeName(mSettings.mode)}, {"center", vecJson(mSettings.center)}};
    switch (mSettings.mode)
    {
    case geo::ArrayMode::Linear:
        c["count"] = mSettings.count;
        c["offset"] = vecJson(mSettings.offset);
        break;
    case geo::ArrayMode::Radial:
        c["count"] = mSettings.count;
        c["axis"] = std::string(1, char('x' + mSettings.axis));
        c["angle"] = mSettings.angle;
        c["angleMode"] = "total";
        c["rise"] = mSettings.rise;
        break;
    case geo::ArrayMode::Grid:
        c["count"] = mSettings.gridCount.x;
        c["countY"] = mSettings.gridCount.y;
        c["countZ"] = mSettings.gridCount.z;
        c["spacing"] = vecJson(mSettings.spacing);
        break;
    }
    return c;
}

std::string ArrayTool::finish()
{
    if (mSources.empty())
        return {};
    try
    {
        json made = ops::execute("array", call(), CallSource::UI);
        mSources.clear();
        return made.get<std::string>();
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
    return {};
}

void ArrayTool::exit()
{
    // Not from inside our own handler: set() destroys this tool.
    if (QCoreApplication::instance())
        QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
}

bool ArrayTool::mousePress(QMouseEvent* e, const ViewInfo& view)
{
    // The right button keeps its marking menus.
    if (e->button() != Qt::LeftButton)
        return false;
    return press(e->position(), view);
}

bool ArrayTool::mouseMove(QMouseEvent* e, const ViewInfo& view)
{
    mHoverView = view.camera;
    if (mDrag != Drag::None)
        drag(e->position(), view);
    else if (Handle h = handleAt(e->position(), view); h != mHover)
    {
        mHover = h;
        viewportTools::requestRedraw();
    }
    return mDrag != Drag::None;
}

bool ArrayTool::mouseRelease(QMouseEvent* e, const ViewInfo& view)
{
    if (e->button() != Qt::LeftButton || mDrag == Drag::None)
        return false;
    release(e->position(), view);
    return true;
}

bool ArrayTool::keyPress(QKeyEvent* e, const ViewInfo&) { return key(e->key()); }

bool ArrayTool::hotkey(const QKeyEvent& e, std::string_view)
{
    // The tool's own keys win over whatever they are bound to while it runs.
    if (e.modifiers() & (Qt::ControlModifier | Qt::AltModifier))
        return false;
    return key(e.key());
}

void ArrayTool::draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view)
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    std::erase_if(mSources, [&](NodeId id) { return !g.node(id); });
    if (mSources.empty())
        return;
    const std::vector<glm::dmat4> xf = copyTransforms();
    if (xf.size() > 1)
        for (NodeId src : mSources)
        {
            std::vector<NodeId> meshes{src};
            for (NodeId d : g.descendants(src))
                meshes.push_back(d);
            for (NodeId id : meshes)
            {
                scene::Node* n = g.node(id);
                if (!n || !n->type().isA("mesh") || !scene::effectivelyVisible(g, id))
                    continue;
                auto rm = renderMeshFor(n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh));
                if (!rm)
                    continue;
                const glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix), wi = glm::inverse(w);
                auto placements = std::make_shared<std::vector<glm::mat4>>();
                placements->reserve(xf.size() - 1);
                for (size_t k = 1; k < xf.size(); ++k)
                    placements->push_back(glm::mat4(wi * xf[k] * w));
                render::MeshDraw d;
                d.mesh = std::move(rm);
                d.world = w;
                d.instances = std::move(placements);
                d.flags = render::DrawShaded | render::DrawWire;
                d.wireColor = kPreviewWire;
                s.meshes.push_back(std::move(d));
            }
        }

    auto pt = [&](const glm::dvec3& w) { return view.project(w); };
    if (mPlaced)
    {
        std::vector<QPointF> guide;
        switch (mSettings.mode)
        {
        case geo::ArrayMode::Linear:
            for (int k = 0; k < std::max(mSettings.count, 1); ++k)
                if (auto q = pt(copyPivot(k)))
                    guide.push_back(*q);
            p.dashedPolyline(guide, 1.5f, 6.0f, kGuide);
            break;
        case geo::ArrayMode::Grid:
        {
            auto [a, b] = planeAxes();
            const int na = mSettings.gridCount[a], nb = mSettings.gridCount[b];
            for (auto [i, j] : {std::pair{0, 0}, {std::max(na - 1, 1), 0}, {std::max(na - 1, 1), std::max(nb - 1, 1)}, {0, std::max(nb - 1, 1)}})
                if (auto q = pt(gridPivot(i, j)))
                    guide.push_back(*q);
            p.dashedPolyline(guide, 1.5f, 6.0f, kGuide, true);
            break;
        }
        case geo::ArrayMode::Radial:
        {
            const glm::dvec3 axis = worldAxis(mSettings.axis);
            const int n = std::max(mSettings.count, 1);
            const int segments = std::max(8, int(std::abs(mSettings.angle) / 5.0));
            for (int i = 0; i <= segments; ++i)
            {
                const double t = double(i) / segments;
                if (auto q = pt(mSettings.center + rotateAbout(ringStart(), axis, mSettings.angle * t) + axis * (mSettings.rise * (n - 1) * t)))
                    guide.push_back(*q);
            }
            p.dashedPolyline(guide, 1.5f, 6.0f, kGuide);
            if (auto c = pt(mSettings.center), top = pt(mSettings.center + axis * (mSettings.rise * (n - 1))); c && top)
                p.line(*c, *top, 1.5f, kGuide);
            if (auto c = pt(mSettings.center))
            {
                p.line(*c - QPointF(6, 0), *c + QPointF(6, 0), 1.5f, kGuide);
                p.line(*c - QPointF(0, 6), *c + QPointF(0, 6), 1.5f, kGuide);
            }
            break;
        }
        }
    }

    auto label = [&](Handle h) -> QString {
        auto [a, b] = planeAxes();
        switch (h)
        {
        case Handle::Spacing:
            return mSettings.mode == geo::ArrayMode::Linear ? "Spacing " + cm(glm::length(axes() * mSettings.offset))
                                                            : "Spacing " + cm(mSettings.spacing[a] * glm::length(axes()[a]));
        case Handle::SpacingB: return "Spacing " + cm(mSettings.spacing[b] * glm::length(axes()[b]));
        case Handle::Count:
            return QString("%1 copies").arg(mSettings.mode == geo::ArrayMode::Grid ? mSettings.gridCount[a] : mSettings.count);
        case Handle::CountB: return QString("%1 copies").arg(mSettings.gridCount[b]);
        case Handle::Angle: return QString("Angle %1°").arg(mSettings.angle, 0, 'f', 0);
        case Handle::Step: return QString("%1 copies").arg(mSettings.count);
        case Handle::Rise: return "Rise " + cm(mSettings.rise);
        case Handle::None: break;
        }
        return {};
    };
    const bool here = mHoverView.empty() || mHoverView == view.camera;
    for (Handle h : {Handle::Spacing, Handle::Count, Handle::SpacingB, Handle::CountB, Handle::Angle, Handle::Step, Handle::Rise})
        if (std::optional<QPointF> at = handlePos(h, view))
        {
            const bool hot = h == mHandle || (here && mDrag == Drag::None && h == mHover);
            p.disc(*at, 7.0f, kHandleRim, 16);
            p.disc(*at, 5.0f, hot ? kHotColor : kIdle, 16);
            p.label(*at + QPointF(12, -10), label(h), hot ? kHotColor : kIdle);
        }

    if (mPlaced)
    {
        static const char* const kModes[] = {"row", "ring", "grid"};
        drawToolStatus(p, view,
                       {.tool = "Array",
                        .mode = kModes[int(mSettings.mode)],
                        .prompt = "drag the handles",
                        .keys = {{"1", "row"}, {"2", "ring"}, {"3", "grid"}, {"+/-", "copies"}, {"Click", "new centre"}, {"Drag", "new direction"},
                                 {"Enter", "make"}, {"Esc", "cancel"}}});
    }
    else
        drawToolStatus(p, view,
                       {.tool = "Array",
                        .prompt = "drag a direction or click a centre",
                        .keys = {{"Drag", "row"}, {"Click", "ring"}, {"3+drag", "grid"}, {"Esc", "cancel"}}});
}

void registerArrayToolActions()
{
    actions::add({
        .id = "edit.array",
        .label = "Array",
        .category = "Edit",
        .run =
            [] {
                auto tool = std::make_unique<ArrayTool>();
                if (!tool->begin())
                {
                    log::warn("Array: select the objects to array first");
                    return;
                }
                viewportTools::set(std::move(tool));
            },
        .doc = "Repeat the selected objects as a live array: drag a direction for a row, click a centre for a ring, or press 3 "
               "for a grid, then drag the handles for spacing, count, angle and rise. Enter makes the array, Esc cancels.",
        .python = "lila.nodes.array(*lila.nodes.selected(), mode=\"radial\", count=8, center=(0, 0, 0))",
    });
    actions::add({
        .id = "edit.bakeArray",
        .label = "Bake Array",
        .category = "Edit",
        .doc = "Turns the selected arrays into plain groups of real objects, instances of the sources, where the copies were.",
        .command = "bakeArray",
        .call = json::object(),
    });
}
} // namespace lila::ui
