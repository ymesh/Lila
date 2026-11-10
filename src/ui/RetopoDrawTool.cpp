#include "ui/RetopoDrawTool.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Normals.h"
#include "geo/Topology.h"
#include "retopo/RetopoCommands.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "ui/Actions.h"
#include "ui/CursorBadges.h"
#include "ui/GizmoMath.h"
#include "ui/NodeLookup.h"
#include "ui/OptionBox.h"
#include "ui/RetopoPolesTool.h"
#include "ui/ToolColors.h"
#include "ui/TopologyTool.h"
#include "ui/ViewportToolMath.h"

#include <QApplication>
#include <QInputDialog>
#include <QPainter>
#include <QPixmap>
#include <QTimer>

#include <numeric>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>

namespace lila::ui
{
namespace
{
using geo::Index;
using geo::kInvalid;

constexpr double kWeldRadius = 10.0; // px
constexpr const char* kExtrudeAnglePref = "/retopoDraw/extrudeAngle";
constexpr const char* kSnapAnglePref = "/retopoDraw/snapAngle";
constexpr const char* kSnapSpacePref = "/retopoDraw/snapSpace";
constexpr const char* kMergeBadge = "retopo.draw.merge";

// Overlay colours are sRGB; the 3D streams take linear.
const glm::vec4 kDotFill(0.95f, 0.95f, 0.95f, 1.0f);
const glm::vec4 kDotRim(0.08f, 0.08f, 0.08f, 1.0f);
const glm::vec4 kGhostDot(0.6f, 0.6f, 0.6f, 0.8f);
const glm::vec4 kFillPreview(0.55f, 0.55f, 0.55f, 0.5f);
const glm::vec4 kDeleteFace(0.8f, 0.8f, 0.8f, 0.6f);

bool insidePolygon(QPointF p, const std::vector<QPointF>& poly)
{
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if ((poly[i].y() > p.y()) != (poly[j].y() > p.y()) &&
            p.x() < (poly[j].x() - poly[i].x()) * (p.y() - poly[i].y()) / (poly[j].y() - poly[i].y()) + poly[i].x())
            in = !in;
    return in;
}

bool convex(const std::vector<QPointF>& poly)
{
    int sign = 0;
    for (size_t i = 0; i < poly.size(); ++i)
    {
        QPointF a = poly[i], b = poly[(i + 1) % poly.size()], c = poly[(i + 2) % poly.size()];
        double cr = (b.x() - a.x()) * (c.y() - b.y()) - (b.y() - a.y()) * (c.x() - b.x());
        int s = cr > 0.0 ? 1 : cr < 0.0 ? -1 : 0;
        if (s == 0)
            return false;
        if (sign == 0)
            sign = s;
        else if (s != sign)
            return false;
    }
    return true;
}

/** Tab never reaches widgets (focus chain), and the tool needs its release too; this sees keys first. */
class RetopoDrawKeyFilter : public QObject
{
public:
    explicit RetopoDrawKeyFilter(RetopoDrawTool* tool) : mTool(tool) {}

    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() != QEvent::KeyPress && e->type() != QEvent::KeyRelease && e->type() != QEvent::ShortcutOverride)
            return false;
        auto* k = static_cast<QKeyEvent*>(e);
        // The fill preview and delete cursor follow Shift, Ctrl and Alt without waiting for the mouse to move.
        if ((k->key() == Qt::Key_Shift || k->key() == Qt::Key_Control || k->key() == Qt::Key_Alt) && e->type() != QEvent::ShortcutOverride)
            viewportTools::requestRedraw();
        QWidget* focus = QApplication::focusWidget();
        if (!focus || obj != focus || !focus->inherits("lila::ui::ViewportWidget"))
            return false;
        // Shift+Tab arrives as Backtab.
        if (k->key() == Qt::Key_Tab || k->key() == Qt::Key_Backtab)
        {
            if (e->type() == QEvent::ShortcutOverride)
            {
                e->accept();
                return true;
            }
            if (!k->isAutoRepeat())
                mTool->setTabDown(e->type() == QEvent::KeyPress);
            return true;
        }
        if (e->type() == QEvent::KeyPress && k->key() == Qt::Key_Escape && k->modifiers() == Qt::NoModifier)
        {
            // Leaving deletes the tool, so it has to wait until this event is done with it.
            QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
            return true;
        }
        return false;
    }

private:
    RetopoDrawTool* mTool;
};
} // namespace

RetopoDrawTool::RetopoDrawTool(scene::NodeId reference, std::string retopoName) : mReference(reference), mRetopoName(std::move(retopoName)) {}

RetopoDrawTool::~RetopoDrawTool() = default;

void RetopoDrawTool::activate()
{
    mFilter = std::make_unique<RetopoDrawKeyFilter>(this);
    qApp->installEventFilter(mFilter.get());
    cursorBadges::add({kMergeBadge, "arrow-merge", [this](const ViewportState&) { return merging(); }, "Vertex merges on release"});
    refresh();
    log::info("Retopo Draw on {} into {}: click dots, Shift-click to fill or place an angle-snapped dot, Ctrl-drag loops, "
              "Tab-drag extends (Shift: whole loop, Ctrl: up to the extrude angle), Shift-drag relaxes, Ctrl+Shift-click deletes, Esc exits",
              mRefMesh ? scene::Graph::current().node(mReference)->path() : std::string("(no reference)"), mRetopoName);
}

void RetopoDrawTool::deactivate()
{
    cursorBadges::remove(kMergeBadge);
    if (mFilter)
        qApp->removeEventFilter(mFilter.get());
    mFilter.reset();
}

double RetopoDrawTool::extrudeAngle() { return std::clamp(PrefStore::get().value<double>("general", kExtrudeAnglePref, 30.0), 1.0, 179.0); }

void RetopoDrawTool::setExtrudeAngle(double degrees)
{
    PrefStore::get().set("general", kExtrudeAnglePref, std::clamp(degrees, 1.0, 179.0));
    viewportTools::requestRedraw();
}

double RetopoDrawTool::snapAngle() { return std::clamp(PrefStore::get().value<double>("general", kSnapAnglePref, 15.0), 0.1, 180.0); }

void RetopoDrawTool::setSnapAngle(double degrees)
{
    PrefStore::get().set("general", kSnapAnglePref, std::clamp(degrees, 0.1, 180.0));
    viewportTools::requestRedraw();
}

bool RetopoDrawTool::snapObjectSpace() { return PrefStore::get().value<std::string>("general", kSnapSpacePref, "object") != "world"; }

void RetopoDrawTool::setSnapObjectSpace(bool object)
{
    PrefStore::get().set("general", kSnapSpacePref, std::string(object ? "object" : "world"));
    viewportTools::requestRedraw();
}

bool RetopoDrawTool::setReference(scene::NodeId reference)
{
    scene::Graph& g = scene::Graph::current();
    scene::Node* ref = g.node(reference);
    if (!ref || !ref->type().isA("mesh"))
        return false;
    scene::Node* retopo = retopoNode();
    if (retopo && retopo->id() == reference)
    {
        log::warn("Set Retopo Reference: the retopo mesh can't be its own reference");
        return false;
    }
    if (retopo && !run("retopoDrawSetReference", {{"mesh", retopo->path()}, {"reference", ref->path()}}))
        return false;
    mReference = reference;
    refresh();
    log::info("Retopo reference: {}", ref->path());
    viewportTools::requestRedraw();
    return true;
}

void RetopoDrawTool::refresh()
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* ref = g.node(mReference);
    scene::MeshPtr refMesh = ref && ref->type().isA("mesh") ? ref->as<scene::MeshPtr>(scene::MeshNode::kOutMesh) : nullptr;
    glm::dmat4 refWorld = ref ? ref->as<glm::dmat4>(scene::Transform::kWorldMatrix) : glm::dmat4(1.0);
    if (refMesh != mRefMesh || refWorld != mRefWorld)
    {
        mRefMesh = refMesh;
        mRefWorld = refWorld;
        if (refMesh)
        {
            geo::Mesh world = *refMesh;
            world.compact();
            for (glm::dvec3& p : world.positions())
                p = glm::dvec3(refWorld * glm::dvec4(p, 1.0));
            mRefBvh.build(world);
            mRefFaceNormals = geo::faceNormals(world);
        }
        else
        {
            mRefBvh = geo::MeshBvh();
            mRefFaceNormals.clear();
        }
    }
    mRetopo = nullptr;
    mSymmetry.reset();
    if (scene::Node* n = retopoNode())
    {
        mRetopoId = n->id();
        mRetopoName = n->path();
        mRetopo = scene::modifiers::cage(g, n->id());
        mRetopoWorld = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        mSymmetry = scene::symmetrySpec(*n);
        const std::string& dots = n->as<std::string>(scene::MeshNode::kRetopoDrawDots);
        if (dots != mDotsText || mRetopoWorld != mDotsWorld)
        {
            mDotsText = dots;
            mDotsWorld = mRetopoWorld;
            mDots = retopo::retopoDrawDots(*n);
        }
        const std::string& guides = n->as<std::string>(scene::MeshNode::kRetopoGuides);
        if (guides != mGuidesText)
        {
            mGuidesText = guides;
            mGuides = retopo::retopoGuides(*n);
        }
    }
    else
    {
        mDots.clear();
        mDotsText.clear();
        mGuides.clear();
        mGuidesText.clear();
    }
    if (mHover.dot >= int(mDots.size()))
        mHover.dot = -1;
    if (!mRetopo || mAnchorPoint >= mRetopo->pointCapacity() || !mRetopo->pointValid(mAnchorPoint))
        mAnchorPoint = kInvalid;
}

scene::Node* RetopoDrawTool::retopoNode() const
{
    scene::Graph& g = scene::Graph::current();
    if (scene::Node* n = g.node(mRetopoId); n && n->type().isA("mesh"))
        return n;
    scene::Node* n = findNode(mRetopoName);
    return n && n->type().isA("mesh") ? n : nullptr;
}

bool RetopoDrawTool::deleting() const
{
    Qt::KeyboardModifiers m = QGuiApplication::queryKeyboardModifiers();
    // Ctrl+Shift as in the Knife; Ctrl+Alt as in the Topology tool.
    return (m & Qt::ControlModifier) && (m & (Qt::ShiftModifier | Qt::AltModifier)) && (mDrag == Drag::None || mDrag == Drag::DeleteClick);
}

std::optional<QCursor> RetopoDrawTool::cursor() const
{
    return deleting() ? std::optional<QCursor>(deleteCursor()) : std::nullopt;
}

glm::dvec3 RetopoDrawTool::pointWorld(Index p) const { return glm::dvec3(mRetopoWorld * glm::dvec4(mRetopo->position(p), 1.0)); }

bool RetopoDrawTool::surfaceHit(QPointF px, const ViewInfo& view, glm::dvec3& out) const
{
    if (mRefBvh.empty())
        return false;
    glm::dvec3 o, d;
    view.ray(px, o, d);
    geo::BvhHit hit;
    if (!mRefBvh.raycast(o, d, hit))
        return false;
    out = hit.point;
    return true;
}

glm::dvec3 RetopoDrawTool::onSurface(const glm::dvec3& p) const
{
    geo::BvhClosest c;
    return !mRefBvh.empty() && mRefBvh.closestPoint(p, c) ? c.point : p;
}

glm::dvec3 RetopoDrawTool::onSurfaceAlong(const glm::dvec3& p, const glm::dvec3& axis) const
{
    geo::BvhHit up, down;
    bool hitUp = !mRefBvh.empty() && mRefBvh.raycast(p, axis, up);
    bool hitDown = !mRefBvh.empty() && mRefBvh.raycast(p, -axis, down);
    if (hitUp && (!hitDown || up.t < down.t))
        return up.point;
    if (hitDown)
        return down.point;
    return onSurface(p);
}

std::optional<glm::dvec3> RetopoDrawTool::anchorWorld() const
{
    if (!mDots.empty())
        return mDots.back();
    if (mRetopo && mAnchorPoint != kInvalid)
        return pointWorld(mAnchorPoint);
    return std::nullopt;
}

std::optional<std::pair<glm::dvec3, retopo::AngleSnap>> RetopoDrawTool::lineSnap(QPointF px, const ViewInfo& view) const
{
    std::optional<glm::dvec3> anchor = anchorWorld();
    glm::dvec3 hit;
    if (!anchor || !surfaceHit(px, view, hit))
        return std::nullopt;
    glm::dvec3 normal = -view.viewDir(*anchor);
    if (geo::BvhClosest c; mRefBvh.closestPoint(*anchor, c) && c.face < mRefFaceNormals.size())
        normal = mRefFaceNormals[c.face];
    glm::dmat3 frame = snapObjectSpace() ? retopo::rotationFrame(mRefWorld) : glm::dmat3(1.0);
    retopo::AngleSnap snap = retopo::angleSnap(*anchor, hit, normal, frame, snapAngle());
    snap.point = onSurfaceAlong(snap.point, snap.axis);
    return std::pair{*anchor, snap};
}

bool RetopoDrawTool::visible(const glm::dvec3& p, const ViewInfo& view) const
{
    if (mRefBvh.empty())
        return true;
    glm::dvec3 d = p - view.eye;
    double len = glm::length(d);
    // A little slack so points lying on the surface aren't hidden by it.
    double slack = view.unitsPerPixel(p) * 3.0;
    return len <= slack || !mRefBvh.occluded(view.eye, d / len, len - slack);
}

std::vector<std::optional<QPointF>> RetopoDrawTool::projectPoints(const ViewInfo& view) const
{
    std::vector<std::optional<QPointF>> out;
    if (!mRetopo)
        return out;
    out.resize(mRetopo->pointCapacity());
    for (Index p = 0; p < mRetopo->pointCapacity(); ++p)
        if (mRetopo->pointValid(p))
        {
            glm::dvec3 w = mMoved.contains(p) ? mMoved.at(p) : pointWorld(p);
            if (visible(w, view))
                out[p] = view.project(w);
        }
    return out;
}

RetopoDrawTool::Hover RetopoDrawTool::pick(QPointF px, const ViewInfo& view) const
{
    Hover h;
    double best = kPickRadiusPx;
    for (size_t i = 0; i < mDots.size(); ++i)
        if (auto s = view.project(mDots[i]); s && distPx(*s, px) < best)
        {
            best = distPx(*s, px);
            h.dot = int(i);
        }
    if (!mRetopo)
        return h;
    const geo::Mesh& m = *mRetopo;
    auto screen = projectPoints(view);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (screen[p] && distPx(*screen[p], px) < best)
        {
            best = distPx(*screen[p], px);
            h.point = p;
            h.dot = -1;
        }
    if (h.point != kInvalid || h.dot >= 0)
        return h;
    double bestEdge = kPickRadiusPx;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e))
            continue;
        auto a = screen[m.edgePoint(e, 0)], b = screen[m.edgePoint(e, 1)];
        if (!a || !b)
            continue;
        double d = segmentDistPx(px, *a, *b);
        if (d < bestEdge)
        {
            bestEdge = d;
            h.edge = e;
        }
    }
    // The face under the cursor, nearest first, by ray against its fan triangles.
    glm::dvec3 o, d;
    view.ray(px, o, d);
    double bestT = std::numeric_limits<double>::max();
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        std::vector<Index> pts = m.facePoints(f);
        glm::dvec3 a = pointWorld(pts[0]);
        for (size_t k = 1; k + 1 < pts.size(); ++k)
        {
            glm::dvec3 b = pointWorld(pts[k]), c = pointWorld(pts[k + 1]);
            glm::dvec3 e1 = b - a, e2 = c - a, pv = glm::cross(d, e2);
            double det = glm::dot(e1, pv);
            if (std::abs(det) < 1e-18)
                continue;
            glm::dvec3 tv = o - a;
            double u = glm::dot(tv, pv) / det;
            glm::dvec3 qv = glm::cross(tv, e1);
            double v = glm::dot(d, qv) / det, t = glm::dot(e2, qv) / det;
            if (u >= 0.0 && v >= 0.0 && u + v <= 1.0 && t > 0.0 && t < bestT)
            {
                bestT = t;
                h.face = f;
            }
        }
    }
    if (h.face != kInvalid)
    {
        glm::dvec3 hit;
        // Faces behind the reference surface don't count.
        if (surfaceHit(px, view, hit) && glm::length(hit - o) + view.unitsPerPixel(hit) * 4.0 < bestT)
            h.face = kInvalid;
    }
    return h;
}

bool RetopoDrawTool::run(const char* command, const json& call, json* result)
{
    try
    {
        json r = ops::execute(command, call, CallSource::UI);
        if (result)
            *result = std::move(r);
        return true;
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        return false;
    }
}

json RetopoDrawTool::targetCall(json flags) const
{
    flags["mesh"] = mRetopoName;
    if (scene::Node* ref = scene::Graph::current().node(mReference))
        flags["reference"] = ref->path();
    flags["create"] = true;
    return flags;
}

bool RetopoDrawTool::setDots(const std::vector<glm::dvec3>& dots)
{
    json name;
    if (!run("retopoDrawSetDots", targetCall({{"dots", vecListJson(dots)}}), &name))
        return false;
    if (name.is_string())
        mRetopoName = name.get<std::string>();
    refresh();
    return true;
}

void RetopoDrawTool::placeDot(QPointF px, const ViewInfo& view)
{
    glm::dvec3 hit;
    if (!surfaceHit(px, view, hit))
        return;
    // Too close to an existing vertex or dot: that one is already there to use.
    Hover h = pick(px, view);
    if (h.point != kInvalid || h.dot >= 0)
        return;
    if (auto s = symmetrySnap(hit, view.unitsPerPixel(hit) * kWeldRadius))
        hit = onSurface(*s);
    std::vector<glm::dvec3> dots = mDots;
    dots.push_back(hit);
    setDots(dots);
}

std::optional<glm::dvec3> RetopoDrawTool::symmetrySnap(const glm::dvec3& world, double tolerance) const
{
    if (!mSymmetry)
        return std::nullopt;
    const glm::dvec3 local(glm::inverse(mRetopoWorld) * glm::dvec4(world, 1.0));
    // World tolerance in object units: the matrix's average scale.
    const double scale = std::cbrt(std::abs(glm::determinant(glm::dmat3(mRetopoWorld))));
    const glm::dvec3 snapped = mSymmetry->snap(local, tolerance / std::max(scale, 1e-12));
    if (snapped == local)
        return std::nullopt;
    return glm::dvec3(mRetopoWorld * glm::dvec4(snapped, 1.0));
}

std::vector<glm::dvec3> RetopoDrawTool::symmetryImages(const glm::dvec3& world) const
{
    std::vector<glm::dvec3> out;
    if (!mSymmetry)
        return out;
    const glm::dvec3 local(glm::inverse(mRetopoWorld) * glm::dvec4(world, 1.0));
    for (int g = 1; g < mSymmetry->order(); ++g)
        out.push_back(glm::dvec3(mRetopoWorld * glm::dvec4(mSymmetry->apply(g, local), 1.0)));
    return out;
}

void RetopoDrawTool::placeSnappedDot(QPointF px, const ViewInfo& view)
{
    auto snap = lineSnap(px, view);
    if (!snap)
    {
        placeDot(px, view);
        return;
    }
    glm::dvec3 at = snap->second.point;
    if (auto s = view.project(at))
        if (Hover h = pick(*s, view); h.point != kInvalid || h.dot >= 0)
            return;
    if (auto s = symmetrySnap(at, view.unitsPerPixel(at) * kWeldRadius))
        at = onSurface(*s);
    std::vector<glm::dvec3> dots = mDots;
    dots.push_back(at);
    setDots(dots);
}

std::optional<RetopoDrawTool::Fill> RetopoDrawTool::findFill(QPointF px, const ViewInfo& view) const
{
    // Over an existing face there is nothing to fill.
    if (mRetopo && pick(px, view).face != kInvalid)
        return std::nullopt;
    struct Candidate
    {
        QPointF screen;
        glm::dvec3 world;
        int dot;
    };
    std::vector<Candidate> cands;
    for (size_t i = 0; i < mDots.size(); ++i)
        if (auto s = view.project(mDots[i]))
            cands.push_back({*s, mDots[i], int(i)});
    if (mRetopo)
    {
        auto screen = projectPoints(view);
        for (Index p = 0; p < mRetopo->pointCapacity(); ++p)
            if (screen[p])
                cands.push_back({*screen[p], pointWorld(p), -1});
    }
    std::vector<QPointF> screen;
    for (const Candidate& c : cands)
        screen.push_back(c.screen);
    std::vector<int> best = smallestFillAround(px, screen);
    if (best.empty())
        return std::nullopt;
    Fill fill;
    for (int i : best)
    {
        fill.points.push_back(cands[i].world);
        if (cands[i].dot >= 0)
            fill.dots.push_back(cands[i].dot);
    }
    return fill;
}

void RetopoDrawTool::fillAt(QPointF px, const ViewInfo& view)
{
    std::optional<Fill> fill = findFill(px, view);
    if (!fill)
        return;
    std::vector<glm::dvec3> remaining;
    for (int i = 0; i < int(mDots.size()); ++i)
        if (std::find(fill->dots.begin(), fill->dots.end(), i) == fill->dots.end())
            remaining.push_back(mDots[i]);
    double weld = view.unitsPerPixel(fill->points[0]) * 0.5;
    // The face and the dots it used up are one undo step.
    UndoChunk chunk("Retopo Draw Fill");
    json name;
    if (!run("retopoDrawAddFace", targetCall({{"points", vecListJson(fill->points)}, {"weld", weld}}), &name))
        return;
    if (name.is_string())
        mRetopoName = name.get<std::string>();
    if (remaining.size() != mDots.size())
        setDots(remaining);
    refresh();
}

void RetopoDrawTool::deleteAt(QPointF px, const ViewInfo& view)
{
    Hover h = pick(px, view);
    if (h.dot >= 0)
    {
        std::vector<glm::dvec3> dots = mDots;
        dots.erase(dots.begin() + h.dot);
        setDots(dots);
        return;
    }
    if (h.point != kInvalid)
        run("retopoDrawDelete", {{"mesh", mRetopoName}, {"points", {h.point}}});
    else if (h.face != kInvalid)
        run("retopoDrawDelete", {{"mesh", mRetopoName}, {"faces", {h.face}}});
    refresh();
}

void RetopoDrawTool::relaxStep(QPointF px, const ViewInfo& view)
{
    if (!mRetopo)
        return;
    const geo::Mesh& m = *mRetopo;
    auto screen = projectPoints(view);
    auto at = [&](Index p) { return mMoved.contains(p) ? mMoved.at(p) : pointWorld(p); };
    std::map<Index, glm::dvec3> next;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!screen[p] || m.isBoundaryPoint(p))
            continue;
        double d = distPx(*screen[p], px);
        if (d > mBrush)
            continue;
        glm::dvec3 avg(0.0);
        int count = 0;
        m.forEachPointEdge(p, [&](Index e) {
            avg += at(m.edgeOtherPoint(e, p));
            ++count;
        });
        if (count == 0)
            continue;
        double falloff = 1.0 - d / mBrush;
        glm::dvec3 target = glm::mix(at(p), avg / double(count), 0.35 * falloff * (2.0 - falloff));
        next[p] = onSurface(target);
    }
    for (auto& [p, pos] : next)
        mMoved[p] = pos;
}

double RetopoDrawTool::edgeFraction(Index edge, QPointF px, const ViewInfo& view) const
{
    auto a = view.project(pointWorld(mRetopo->edgePoint(edge, 0))), b = view.project(pointWorld(mRetopo->edgePoint(edge, 1)));
    if (!a || !b)
        return 0.5;
    double t = 0.5;
    segmentDistPx(px, *a, *b, &t);
    return std::clamp(t, 0.05, 0.95);
}

void RetopoDrawTool::loopPreview(Index edge, double fraction, std::vector<std::pair<glm::dvec3, glm::dvec3>>& out) const
{
    // Walk the ring both ways, carrying which end of each edge is the "start" side so the fraction lines up.
    const geo::Mesh& m = *mRetopo;
    auto point = [&](Index e, Index startPoint) {
        Index other = m.edgeOtherPoint(e, startPoint);
        return glm::mix(pointWorld(startPoint), pointWorld(other), fraction);
    };
    std::vector<Index> seenFaces;
    for (int dir = 0; dir < 2; ++dir)
    {
        Index e = edge, start = m.edgePoint(edge, 0);
        std::vector<Index> faces;
        m.forEachEdgeCorner(e, [&](Index c) { faces.push_back(m.cornerFace(c)); });
        if (size_t(dir) >= faces.size())
            break;
        Index f = faces[dir];
        for (int guard = 0; guard < 100000 && f != kInvalid; ++guard)
        {
            if (std::find(seenFaces.begin(), seenFaces.end(), f) != seenFaces.end())
                break;
            seenFaces.push_back(f);
            Index opp = geo::quadOppositeEdge(m, e, f);
            if (opp == kInvalid)
                break;
            // The start side continues along the quad's side edge from start.
            Index next = kInvalid;
            m.forEachFaceCorner(f, [&](Index c) {
                Index a = m.cornerPoint(c), b = m.cornerNextPoint(c);
                if (m.cornerEdge(c) == e || m.cornerEdge(c) == opp)
                    return;
                if (a == start)
                    next = b;
                else if (b == start)
                    next = a;
            });
            if (next == kInvalid)
                break;
            out.push_back({point(e, start), point(opp, next)});
            Index nf = m.oppositeFace(opp, f);
            e = opp;
            start = next;
            f = nf;
        }
    }
}

retopo::BorderRun RetopoDrawTool::extrudeRun(Index edge, bool wholeLoop) const
{
    if (!mRetopo || edge == kInvalid || !mRetopo->edgeValid(edge) || !mRetopo->isBoundaryEdge(edge))
        return {};
    return retopo::walkBorder(*mRetopo, edge, wholeLoop ? 180.0 : extrudeAngle());
}

void RetopoDrawTool::beginRunExtrude(Index edge, bool wholeLoop)
{
    mDrag = Drag::ExtendRun;
    mDragEdge = edge;
    mRun = extrudeRun(edge, wholeLoop);
    const geo::Mesh& m = *mRetopo;
    const size_t n = mRun.points.size();
    // Each edge's outward direction: in its face's plane, perpendicular to the edge, away from the face.
    std::vector<glm::dvec3> edgeOut(mRun.edges.size(), glm::dvec3(0.0));
    for (size_t i = 0; i < mRun.edges.size(); ++i)
    {
        glm::dvec3 a = pointWorld(mRun.points[i]), b = pointWorld(mRun.points[(i + 1) % n]);
        Index face = kInvalid;
        m.forEachEdgeCorner(mRun.edges[i], [&](Index c) { face = m.cornerFace(c); });
        if (face == kInvalid)
            continue;
        std::vector<Index> pts = m.facePoints(face);
        glm::dvec3 centre(0.0), normal(0.0);
        for (size_t k = 0; k < pts.size(); ++k)
        {
            glm::dvec3 p = pointWorld(pts[k]), q = pointWorld(pts[(k + 1) % pts.size()]);
            centre += p / double(pts.size());
            normal += glm::cross(p, q);
        }
        glm::dvec3 out = glm::cross(b - a, normal);
        if (glm::dot(out, centre - (a + b) * 0.5) > 0.0)
            out = -out;
        if (double len = glm::length(out); len > 0.0)
            edgeOut[i] = out / len;
        if (mRun.edges[i] == edge)
            mRunPull = edgeOut[i];
    }
    mRunOut.assign(n, glm::dvec3(0.0));
    for (size_t j = 0; j < n; ++j)
    {
        std::vector<size_t> adjacent;
        if (j < mRun.edges.size())
            adjacent.push_back(j);
        if (j > 0 || mRun.closed)
            adjacent.push_back((j + mRun.edges.size() - 1) % mRun.edges.size());
        glm::dvec3 sum(0.0);
        for (size_t i : adjacent)
            sum += edgeOut[i];
        double len = glm::length(sum);
        if (len <= 0.0)
            continue;
        glm::dvec3 dir = sum / len;
        // Longer at corners so the strip keeps its width there.
        mRunOut[j] = dir / std::max(0.5, glm::dot(dir, edgeOut[adjacent[0]]));
    }
    mRunTo.clear();
    for (Index p : mRun.points)
        mRunTo.push_back(pointWorld(p));
}

void RetopoDrawTool::updateRunExtrude(QPointF px, const ViewInfo& view)
{
    glm::dvec3 hit;
    if (!surfaceHit(px, view, hit))
        return;
    double dist = std::max(0.0, glm::dot(hit - mDragStartHit, mRunPull));
    for (size_t j = 0; j < mRun.points.size(); ++j)
        mRunTo[j] = onSurface(pointWorld(mRun.points[j]) + mRunOut[j] * dist);
}

void RetopoDrawTool::drawRun(render::FrameState& s, const retopo::BorderRun& run) const
{
    for (size_t i = 0; i < run.edges.size(); ++i)
    {
        s.linesOnTop.push_back({glm::vec3(pointWorld(run.points[i])), kHotLinear});
        s.linesOnTop.push_back({glm::vec3(pointWorld(run.points[(i + 1) % run.points.size()])), kHotLinear});
    }
}

bool RetopoDrawTool::mousePress(QMouseEvent* e, const ViewInfo& view)
{
    refresh();
    mPressPx = e->position();
    mCursor = e->position();
    mCursorView = view.camera;
    mMods = e->modifiers();
    mMoved.clear();
    mDotTo.reset();
    Hover h = pick(e->position(), view);
    const bool shift = mMods & Qt::ShiftModifier, ctrl = mMods & Qt::ControlModifier;

    if (e->button() == Qt::MiddleButton)
    {
        if (h.point != kInvalid)
        {
            mDrag = Drag::Tweak;
            mDragPoint = h.point;
        }
        else if (h.dot >= 0)
        {
            mDrag = Drag::DotMove;
            mDragDot = h.dot;
        }
        else
            return false;
        return true;
    }
    if (e->button() != Qt::LeftButton)
        return false;
    if (ctrl && (mMods & (Qt::AltModifier | Qt::ShiftModifier)) && !mTabDown)
        mDrag = Drag::DeleteClick;
    else if (mTabDown)
    {
        if (h.edge == kInvalid || !mRetopo->isBoundaryEdge(h.edge))
            return true;
        if (shift || ctrl)
        {
            beginRunExtrude(h.edge, shift);
            surfaceHit(e->position(), view, mDragStartHit);
            return true;
        }
        mDrag = Drag::Extend;
        mDragEdge = h.edge;
        surfaceHit(e->position(), view, mDragStartHit);
        mExtendTo = {pointWorld(mRetopo->edgePoint(h.edge, 0)), pointWorld(mRetopo->edgePoint(h.edge, 1))};
    }
    else if (ctrl)
    {
        if (h.edge == kInvalid)
            return true;
        mDrag = Drag::Loop;
        mDragEdge = h.edge;
        mLoopFraction = edgeFraction(h.edge, e->position(), view);
    }
    else if (shift)
        mDrag = Drag::FillClick;
    else if (h.point != kInvalid)
    {
        mDrag = Drag::Tweak;
        mDragPoint = h.point;
    }
    else if (h.dot >= 0)
    {
        mDrag = Drag::DotMove;
        mDragDot = h.dot;
    }
    else
        mDrag = Drag::DotClick;
    return true;
}

bool RetopoDrawTool::mouseMove(QMouseEvent* e, const ViewInfo& view)
{
    mCursor = e->position();
    mCursorView = view.camera;
    mMods = e->modifiers();
    if (mDrag == Drag::None)
    {
        refresh();
        mHover = pick(e->position(), view);
        if (mHover.point != kInvalid && !(mMods & Qt::ShiftModifier))
            mAnchorPoint = mHover.point;
        if (mMods & Qt::ControlModifier && mHover.edge != kInvalid)
            mLoopFraction = edgeFraction(mHover.edge, e->position(), view);
        viewportTools::requestRedraw();
        return true;
    }
    const bool moved = distPx(e->position(), mPressPx) > 4.0;
    switch (mDrag)
    {
    case Drag::FillClick:
        if (moved)
            mDrag = Drag::Relax;
        break;
    case Drag::Relax: relaxStep(e->position(), view); break;
    case Drag::Tweak:
    {
        glm::dvec3 hit;
        if (!surfaceHit(e->position(), view, hit))
            break;
        mMoved.erase(mDragPoint);
        mMergeTarget = kInvalid;
        auto screen = projectPoints(view);
        double best = kWeldRadius;
        for (Index p = 0; p < screen.size(); ++p)
            if (p != mDragPoint && screen[p] && distPx(*screen[p], e->position()) < best)
            {
                best = distPx(*screen[p], e->position());
                mMergeTarget = p;
            }
        mMoved[mDragPoint] = mMergeTarget != kInvalid ? pointWorld(mMergeTarget) : hit;
        break;
    }
    case Drag::DotMove:
    {
        glm::dvec3 hit;
        if (moved && surfaceHit(e->position(), view, hit))
            mDotTo = hit;
        break;
    }
    case Drag::Loop:
    {
        // Follow the cursor onto whichever edge of the ring it is over.
        Hover h = pick(e->position(), view);
        if (h.edge != kInvalid)
            mDragEdge = h.edge;
        mLoopFraction = edgeFraction(mDragEdge, e->position(), view);
        break;
    }
    case Drag::Extend:
    {
        glm::dvec3 hit;
        if (surfaceHit(e->position(), view, hit))
        {
            glm::dvec3 offset = hit - mDragStartHit;
            mExtendTo = {onSurface(pointWorld(mRetopo->edgePoint(mDragEdge, 0)) + offset),
                         onSurface(pointWorld(mRetopo->edgePoint(mDragEdge, 1)) + offset)};
        }
        break;
    }
    case Drag::ExtendRun: updateRunExtrude(e->position(), view); break;
    default: break;
    }
    viewportTools::requestRedraw();
    return true;
}

bool RetopoDrawTool::mouseRelease(QMouseEvent* e, const ViewInfo& view)
{
    mCursor = e->position();
    commitDrag(view);
    mDrag = Drag::None;
    mMoved.clear();
    mDotTo.reset();
    mDragDot = -1;
    mMergeTarget = kInvalid;
    refresh();
    mHover = pick(e->position(), view);
    return true;
}

void RetopoDrawTool::commitDrag(const ViewInfo& view)
{
    switch (mDrag)
    {
    case Drag::DotClick: placeDot(mCursor, view); break;
    case Drag::FillClick:
        if (findFill(mCursor, view))
            fillAt(mCursor, view);
        else
            placeSnappedDot(mCursor, view);
        break;
    case Drag::DeleteClick: deleteAt(mCursor, view); break;
    case Drag::DotMove:
        if (mDotTo && mDragDot >= 0 && mDragDot < int(mDots.size()))
        {
            glm::dvec3 to = *mDotTo;
            if (auto s = symmetrySnap(to, view.unitsPerPixel(to) * kWeldRadius))
                to = onSurface(*s);
            std::vector<glm::dvec3> dots = mDots;
            dots[mDragDot] = to;
            setDots(dots);
        }
        break;
    case Drag::Loop:
        if (mRetopo && mRetopo->edgeValid(mDragEdge))
            run("retopoDrawInsertLoop", {{"mesh", mRetopoName}, {"edge", mDragEdge}, {"fraction", mLoopFraction}});
        break;
    case Drag::Extend:
        if (distPx(mCursor, mPressPx) > 4.0 && mRetopo && mRetopo->edgeValid(mDragEdge))
        {
            Index a = mRetopo->edgePoint(mDragEdge, 0), b = mRetopo->edgePoint(mDragEdge, 1);
            double weld = view.unitsPerPixel(pointWorld(a)) * 0.5;
            run("retopoDrawAddFace", targetCall({{"points", {vecJson(pointWorld(a)), vecJson(pointWorld(b)), vecJson(mExtendTo[1]), vecJson(mExtendTo[0])}},
                                               {"weld", weld}}));
        }
        break;
    case Drag::ExtendRun:
        if (distPx(mCursor, mPressPx) > 4.0 && mRetopo && mRunTo.size() == mRun.points.size())
        {
            json faces = json::array();
            for (size_t i = 0; i < mRun.edges.size(); ++i)
            {
                size_t j = (i + 1) % mRun.points.size();
                faces.push_back({vecJson(pointWorld(mRun.points[i])), vecJson(pointWorld(mRun.points[j])), vecJson(mRunTo[j]), vecJson(mRunTo[i])});
            }
            double weld = view.unitsPerPixel(pointWorld(mRun.points[0])) * 0.5;
            run("retopoDrawAddFace", targetCall({{"faces", faces}, {"weld", weld}}));
        }
        break;
    case Drag::Tweak:
    case Drag::Relax:
        if (!mMoved.empty())
        {
            json points = json::array(), positions = json::array();
            for (auto& [p, pos] : mMoved)
            {
                points.push_back(p);
                positions.push_back(vecJson(pos));
            }
            double weld = 0.0;
            // Dropped onto another vertex: mouseMove already snapped to it, and the command welds them.
            if (mDrag == Drag::Tweak && mMergeTarget != kInvalid && mRetopo->pointValid(mMergeTarget))
                weld = view.unitsPerPixel(pointWorld(mMergeTarget)) * 0.5;
            run("retopoDrawMove", {{"mesh", mRetopoName}, {"points", points}, {"positions", positions}, {"weld", weld}});
        }
        break;
    default: break;
    }
}

bool RetopoDrawTool::wheel(QWheelEvent* e, const ViewInfo& view)
{
    if (mTabDown && (e->modifiers() & Qt::ControlModifier))
    {
        // 5 degrees a notch.
        setExtrudeAngle(extrudeAngle() + 5.0 * e->angleDelta().y() / 120.0);
        if (mDrag == Drag::ExtendRun && !(e->modifiers() & Qt::ShiftModifier) && mRetopo && mRetopo->edgeValid(mDragEdge))
        {
            beginRunExtrude(mDragEdge, false);
            updateRunExtrude(mCursor, view);
        }
        return true;
    }
    if (!(e->modifiers() & Qt::ShiftModifier))
        return false;
    // Shift turns the wheel's vertical delta horizontal on some platforms.
    int delta = e->angleDelta().y() != 0 ? e->angleDelta().y() : e->angleDelta().x();
    mBrush = std::clamp(mBrush * std::pow(1.1, delta / 120.0), 10.0, 400.0);
    viewportTools::requestRedraw();
    return true;
}

bool RetopoDrawTool::keyPress(QKeyEvent* e, const ViewInfo&)
{
    if (e->key() == Qt::Key_Escape)
    {
        QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
        return true;
    }
    if (e->key() == Qt::Key_Backspace && !mDots.empty())
    {
        std::vector<glm::dvec3> dots = mDots;
        dots.pop_back();
        setDots(dots);
        return true;
    }
    return false;
}

void RetopoDrawTool::removeOwnDraw(render::FrameState& s) const
{
    // The retopo mesh is drawn here, see-through; drop the viewport's opaque draw of it.
    if (!mRetopo)
        return;
    scene::Node* n = findNode(mRetopoName);
    if (!n)
        return;
    scene::MeshPtr out = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    if (!out)
        return;
    geo::Bounds b = out->bounds();
    glm::mat4 world(mRetopoWorld);
    float tol = float(std::max(1e-4, glm::length(b.size()) * 1e-4));
    std::erase_if(s.meshes.edit(), [&](const render::MeshDraw& d) {
        if (!d.mesh || d.world != world)
            return false;
        return glm::length(d.mesh->boundsMin - glm::vec3(b.min)) < tol && glm::length(d.mesh->boundsMax - glm::vec3(b.max)) < tol;
    });
}

void RetopoDrawTool::draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view)
{
    LILA_ZONE();

    refresh();
    removeOwnDraw(s);
    if (mDrag == Drag::None)
        mMods = QGuiApplication::queryKeyboardModifiers();
    const bool here = view.camera == mCursorView;
    const bool shift = mMods & Qt::ShiftModifier, ctrl = mMods & Qt::ControlModifier;
    const bool del = here && deleting();
    auto world = [&](Index i) { return mMoved.contains(i) ? mMoved.at(i) : pointWorld(i); };

    drawRetopoGuides(
        p, view, mGuides, [this](const glm::dvec3& w) { return symmetryImages(w); }, [&](const glm::dvec3& w) { return visible(w, view); });

    if (mRetopo)
    {
        const geo::Mesh& m = *mRetopo;
        for (Index f = 0; f < m.faceCapacity(); ++f)
        {
            if (!m.faceValid(f))
                continue;
            std::vector<Index> pts = m.facePoints(f);
            glm::dvec3 a = world(pts[0]), n(0.0);
            for (size_t k = 1; k + 1 < pts.size(); ++k)
                n += glm::cross(world(pts[k]) - a, world(pts[k + 1]) - a);
            double len = glm::length(n);
            double facing = len > 0.0 ? std::abs(glm::dot(n / len, view.viewDir(a))) : 1.0;
            float g = float(0.04 + 0.2 * facing);
            glm::vec4 c(g, g, g, 0.45f);
            for (size_t k = 1; k + 1 < pts.size(); ++k)
                for (Index q : {pts[0], pts[k], pts[k + 1]})
                    s.tris3d.push_back({glm::vec3(world(q)), c});
        }
        for (Index e = 0; e < m.edgeCapacity(); ++e)
            if (m.edgeValid(e))
            {
                const bool hot = here && e == mHover.edge && mDrag == Drag::None && !shift;
                pushRetopoEdge(s, glm::vec3(world(m.edgePoint(e, 0))), glm::vec3(world(m.edgePoint(e, 1))),
                               hot ? kHotLinear : glm::vec4(0.6f, 0.6f, 0.6f, 1.0f), hot);
            }

        // Edge loop preview while Ctrl is held over an edge, or during the drag.
        Index loopEdge = mDrag == Drag::Loop ? mDragEdge : (here && mDrag == Drag::None && ctrl && !shift && !mTabDown) ? mHover.edge : kInvalid;
        if (loopEdge != kInvalid && m.edgeValid(loopEdge))
        {
            std::vector<std::pair<glm::dvec3, glm::dvec3>> segs;
            loopPreview(loopEdge, mLoopFraction, segs);
            for (auto& [a, b] : segs)
            {
                s.linesOnTop.push_back({glm::vec3(a), kHotLinear});
                s.linesOnTop.push_back({glm::vec3(b), kHotLinear});
            }
        }
        if (mDrag == Drag::Extend && m.edgeValid(mDragEdge))
        {
            glm::dvec3 q[4] = {pointWorld(m.edgePoint(mDragEdge, 0)), pointWorld(m.edgePoint(mDragEdge, 1)), mExtendTo[1], mExtendTo[0]};
            for (int k : {0, 1, 2, 0, 2, 3})
                s.tris3d.push_back({glm::vec3(q[k]), glm::vec4(0.6f, 0.6f, 0.6f, 0.5f)});
            for (int k = 0; k < 4; ++k)
            {
                s.linesOnTop.push_back({glm::vec3(q[k]), kHotLinear});
                s.linesOnTop.push_back({glm::vec3(q[(k + 1) % 4]), kHotLinear});
            }
        }

        // Tab Shift / Tab Ctrl: the edges that would extrude, then the strip while dragging.
        if (here && mDrag == Drag::None && mTabDown && shift != ctrl)
            drawRun(s, extrudeRun(mHover.edge, shift));
        if (mDrag == Drag::ExtendRun && mRunTo.size() == mRun.points.size())
        {
            drawRun(s, mRun);
            for (size_t i = 0; i < mRun.edges.size(); ++i)
            {
                size_t j = (i + 1) % mRun.points.size();
                glm::dvec3 q[4] = {pointWorld(mRun.points[i]), pointWorld(mRun.points[j]), mRunTo[j], mRunTo[i]};
                for (int k : {0, 1, 2, 0, 2, 3})
                    s.tris3d.push_back({glm::vec3(q[k]), glm::vec4(0.6f, 0.6f, 0.6f, 0.5f)});
                for (int k = 1; k < 4; ++k)
                {
                    s.linesOnTop.push_back({glm::vec3(q[k]), kHotLinear});
                    s.linesOnTop.push_back({glm::vec3(q[(k + 1) % 4]), kHotLinear});
                }
            }
        }

        auto screen = projectPoints(view);
        if (mDrag == Drag::Tweak && mMergeTarget < screen.size() && screen[mMergeTarget])
        {
            std::vector<QPointF> ring;
            for (int k = 0; k < 32; ++k)
            {
                double a = k / 32.0 * 6.283185307179586;
                ring.push_back(*screen[mMergeTarget] + QPointF(std::cos(a), std::sin(a)) * 10.0);
            }
            p.polyline(ring, 2.0f, kHotColor, true);
        }
        for (Index i = 0; i < screen.size(); ++i)
            if (screen[i])
            {
                bool hot = (here && i == mHover.point && !del) || (mDrag == Drag::Tweak && i == mDragPoint);
                p.disc(*screen[i], hot ? 6.0f : 4.0f, hot ? kHotColor : kVertexRim, 16);
                p.disc(*screen[i], hot ? 3.5f : 2.5f, hot ? kHotColor : kVertexFill, 12);
            }
    }

    for (size_t i = 0; i < mDots.size(); ++i)
    {
        glm::dvec3 dot = mDrag == Drag::DotMove && int(i) == mDragDot && mDotTo ? *mDotTo : mDots[i];
        for (const glm::dvec3& image : symmetryImages(dot))
            if (auto g = view.project(image); g && visible(image, view))
                p.disc(*g, 3.5f, kGhostDot, 12);
        if (!visible(dot, view))
            continue;
        if (auto sp = view.project(dot))
        {
            bool hot = (here && int(i) == mHover.dot && !del) || (mDrag == Drag::DotMove && int(i) == mDragDot);
            p.disc(*sp, hot ? 6.5f : 5.0f, kDotRim, 16);
            p.disc(*sp, hot ? 5.0f : 3.5f, kDotFill, 16);
        }
    }

    std::optional<Fill> fill;
    if (here && shift && !ctrl && (mDrag == Drag::None || mDrag == Drag::FillClick))
        fill = findFill(mCursor, view);
    if (fill)
    {
        const std::vector<glm::dvec3>& q = fill->points;
        for (size_t k = 1; k + 1 < q.size(); ++k)
            for (const glm::dvec3& v : {q[0], q[k], q[k + 1]})
                s.tris3d.push_back({glm::vec3(v), kFillPreview});
        for (size_t k = 0; k < q.size(); ++k)
        {
            s.linesOnTop.push_back({glm::vec3(q[k]), kHotLinear});
            s.linesOnTop.push_back({glm::vec3(q[(k + 1) % q.size()]), kHotLinear});
        }
    }
    if (here && shift && !ctrl && !mTabDown && !fill && (mDrag == Drag::None || mDrag == Drag::FillClick))
        drawLineSnap(p, view);
    if (del)
        drawDeleteTarget(s, p, view);

    // The relax brush, while relaxing or when Shift has nothing to fill.
    if (here && shift && !ctrl && (mDrag == Drag::Relax || (!fill && (mDrag == Drag::None || mDrag == Drag::FillClick))))
    {
        std::vector<QPointF> circle;
        for (int k = 0; k < 48; ++k)
        {
            double a = k / 48.0 * 6.283185307179586;
            circle.push_back(mCursor + QPointF(std::cos(a), std::sin(a)) * mBrush);
        }
        p.polyline(circle, 1.5f, kTopoBrush, true);
    }
    if (mRefBvh.empty())
        drawToolStatus(p, view, {.tool = "Retopo Draw", .prompt = "select a reference mesh"});
    else if (here && mTabDown && ctrl && !shift)
        drawToolStatus(p, view,
                       {.tool = "Retopo Draw",
                        .mode = "extrude to angle",
                        .fields = {{"angle", QString("%1\u00b0").arg(int(std::lround(extrudeAngle())))}},
                        .keys = {{"wheel", "angle"}}});
    else if (here && mTabDown && shift && !ctrl)
        drawToolStatus(p, view, {.tool = "Retopo Draw", .mode = "extrude edge loop", .keys = {{"drag border", "extrude the loop"}}});
    else
        drawToolStatus(p, view, status(here ? mMods : Qt::NoModifier));
}

ToolStatus RetopoDrawTool::status(Qt::KeyboardModifiers mods) const
{
    const bool shift = mods & Qt::ShiftModifier, ctrl = mods & Qt::ControlModifier;
    ToolStatus st{.tool = "Retopo Draw", .subject = QString::fromStdString(mRetopoName).section('|', -1)};
    if (mSymmetry)
        st.symmetry = ToolSymmetry{true};
    // The keys follow the modifiers held, so each one shows what it adds.
    if (mTabDown)
    {
        st.mode = "extend";
        st.keys = {{"drag border", "extend a strip"}, {"+Shift", "whole loop"}, {"+Ctrl", "to the extrude angle"}};
    }
    else if (ctrl && shift)
    {
        st.mode = "delete";
        st.keys = {{"click", "delete face, vertex or dot"}};
    }
    else if (shift)
    {
        st.mode = "fill";
        st.keys = {{"click", "fill, or a snapped dot"}, {"drag", "relax"}, {"wheel", "brush size"}};
    }
    else if (ctrl)
    {
        st.mode = "loop";
        st.keys = {{"drag edge", "insert loop"}, {"+Shift click", "delete"}};
    }
    else
    {
        st.prompt = mDots.empty() ? "click to place dots" : "Shift-click to fill a face";
        st.keys = {{"Shift-click", "fill"},       {"Shift-drag", "relax"},       {"Ctrl-drag", "edge loop"}, {"Tab-drag", "extend border"},
                   {"drag vertex", "move"},       {"Ctrl+Shift-click", "delete"}, {"Backspace", "last dot"}, {"Esc", "exit"}};
    }
    return st;
}

void RetopoDrawTool::drawLineSnap(OverlayPainter& p, const ViewInfo& view) const
{
    auto snap = lineSnap(mCursor, view);
    if (!snap)
        return;
    const auto& [anchor, s] = *snap;
    // Sampled along the surface, so the dashes follow it over curves.
    std::vector<QPointF> run;
    const int samples = 32;
    for (int i = 0; i <= samples; ++i)
    {
        glm::dvec3 w = i == 0 ? anchor : i == samples ? s.point : onSurfaceAlong(glm::mix(anchor, s.point, i / double(samples)), s.axis);
        std::optional<QPointF> sp = visible(w, view) ? view.project(w) : std::nullopt;
        if (sp)
            run.push_back(*sp);
        if ((!sp || i == samples) && run.size() > 1)
            p.dashedPolyline(run, 1.5f, 6.0f, kHotColor);
        if (!sp)
            run.clear();
    }
    if (auto sp = view.project(s.point); sp && visible(s.point, view))
    {
        std::vector<QPointF> ring;
        for (int k = 0; k < 24; ++k)
        {
            double a = k / 24.0 * 6.283185307179586;
            ring.push_back(*sp + QPointF(std::cos(a), std::sin(a)) * 7.0);
        }
        p.polyline(ring, 1.5f, kHotColor, true);
        p.disc(*sp, 3.5f, kDotFill, 16);
        p.label(*sp + QPointF(10.0, -10.0), QString("%1\u00b0 %2").arg(s.degrees, 0, 'g', 4).arg(snapObjectSpace() ? "object" : "world"));
    }
}

void RetopoDrawTool::drawDeleteTarget(render::FrameState& s, OverlayPainter& p, const ViewInfo& view) const
{
    if (mHover.dot >= 0 && mHover.dot < int(mDots.size()))
    {
        if (auto sp = view.project(mDots[mHover.dot]))
        {
            p.disc(*sp, 8.0f, kHotColor, 16);
            p.disc(*sp, 5.0f, kDotRim, 16);
        }
        return;
    }
    if (!mRetopo)
        return;
    const geo::Mesh& m = *mRetopo;
    // Deleting a vertex takes its faces with it, so those light up too.
    std::vector<Index> faces;
    if (mHover.point != kInvalid && m.pointValid(mHover.point))
    {
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
                if (std::vector<Index> pts = m.facePoints(f); std::find(pts.begin(), pts.end(), mHover.point) != pts.end())
                    faces.push_back(f);
    }
    else if (mHover.face != kInvalid && m.faceValid(mHover.face))
        faces.push_back(mHover.face);
    for (Index f : faces)
    {
        std::vector<Index> pts = m.facePoints(f);
        for (size_t k = 1; k + 1 < pts.size(); ++k)
            for (Index q : {pts[0], pts[k], pts[k + 1]})
                s.tris3d.push_back({glm::vec3(pointWorld(q)), kDeleteFace});
        for (size_t k = 0; k < pts.size(); ++k)
        {
            s.linesOnTop.push_back({glm::vec3(pointWorld(pts[k])), kHotLinear});
            s.linesOnTop.push_back({glm::vec3(pointWorld(pts[(k + 1) % pts.size()])), kHotLinear});
        }
    }
    if (mHover.point != kInvalid && m.pointValid(mHover.point))
        if (auto sp = view.project(pointWorld(mHover.point)))
        {
            p.disc(*sp, 7.0f, kHotColor, 16);
            p.disc(*sp, 4.0f, kVertexFill, 12);
        }
}

std::vector<int> smallestFillAround(QPointF px, const std::vector<QPointF>& screen)
{
    std::vector<int> near(screen.size());
    std::iota(near.begin(), near.end(), 0);
    std::stable_sort(near.begin(), near.end(), [&](int a, int b) { return distPx(screen[a], px) < distPx(screen[b], px); });
    if (near.size() > 7)
        near.resize(7);
    std::vector<int> best;
    double bestPerimeter = std::numeric_limits<double>::max();
    const int n = int(near.size());
    for (int size : {4, 3})
    {
        std::vector<int> pick(size);
        auto tryCombo = [&]() {
            std::vector<int> order;
            for (int i : pick)
                order.push_back(near[i]);
            QPointF c(0, 0);
            for (int i : order)
                c += screen[i] / double(size);
            // Counter-clockwise as seen on screen (y points down), so the face points at the camera.
            std::sort(order.begin(), order.end(), [&](int a, int b) {
                return std::atan2(-(screen[a].y() - c.y()), screen[a].x() - c.x()) < std::atan2(-(screen[b].y() - c.y()), screen[b].x() - c.x());
            });
            std::vector<QPointF> poly;
            for (int i : order)
                poly.push_back(screen[i]);
            if (!convex(poly) || !insidePolygon(px, poly))
                return;
            double per = 0.0;
            for (size_t k = 0; k < poly.size(); ++k)
                per += distPx(poly[k], poly[(k + 1) % poly.size()]);
            if (per < bestPerimeter)
            {
                bestPerimeter = per;
                best = order;
            }
        };
        std::function<void(int, int)> rec = [&](int start, int depth) {
            if (depth == size)
            {
                tryCombo();
                return;
            }
            for (int i = start; i < n; ++i)
            {
                pick[depth] = i;
                rec(i + 1, depth + 1);
            }
        };
        rec(0, 0);
        if (!best.empty())
            break;
    }
    return best;
}

RetopoDrawTool* activeRetopoDraw() { return dynamic_cast<RetopoDrawTool*>(viewportTools::active()); }

namespace
{
/** The reference Set Retopo Reference picks: the first selected mesh that isn't the retopo mesh. */
scene::Node* selectedSurface(const std::string& retopo)
{
    scene::Graph& g = scene::Graph::current();
    for (scene::NodeId id : g.selection())
        if (scene::Node* n = g.node(id); n && n->type().isA("mesh") && n->path() != retopo)
            return n;
    return nullptr;
}

/** Connects the retopo mesh to the reference, creating it (with the reference's symmetry) when it doesn't exist yet. */
bool connectReference(const std::string& retopo, const scene::Node& ref)
{
    try
    {
        ops::execute("retopoDrawSetReference", {{"mesh", retopo}, {"reference", ref.path()}, {"create", true}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        return false;
    }
    log::info("Retopo reference: {}. Retopo mesh: {}", ref.path(), retopo);
    return true;
}

void setReferenceFromSelection()
{
    scene::Graph& g = scene::Graph::current();
    if (RetopoDrawTool* tool = activeRetopoDraw())
    {
        if (scene::Node* ref = selectedSurface(tool->retopoName()))
            tool->setReference(ref->id());
        else
            log::warn("Set Retopo Reference: no mesh selected");
        return;
    }
    if (TopologyTool* topo = activeTopologyTool())
    {
        scene::Node* ref = selectedSurface(topo->retopoName());
        if (!ref)
        {
            log::warn("Set Retopo Reference: no mesh selected");
            return;
        }
        std::string retopo = topo->retopoName();
        TopoMode mode = topo->mode();
        if (connectReference(retopo, *ref))
            viewportTools::set(std::make_unique<TopologyTool>(ref->id(), retopo, mode));
        return;
    }
    // A selected retopo mesh takes the other selected mesh as its reference.
    for (scene::NodeId id : g.selection())
        if (retopo::retopoDrawReference(g, id) != scene::kNoNode)
            if (scene::Node* ref = selectedSurface(g.node(id)->path()))
            {
                connectReference(g.node(id)->path(), *ref);
                return;
            }
    // Otherwise the selected mesh becomes the reference, and its retopo mesh is made now.
    std::optional<retopo::RetopoDrawTargets> t = retopo::resolveRetopoDraw(g, g.selection());
    scene::Node* ref = t ? g.node(t->reference) : nullptr;
    if (!ref)
    {
        log::warn("Set Retopo Reference: no mesh selected");
        return;
    }
    connectReference(t->retopo, *ref);
}

void stepSnapAngle(int dir)
{
    double current = RetopoDrawTool::snapAngle();
    double next = current;
    if (dir > 0)
    {
        for (double v : gizmo::kRotateSteps)
            if (v > current + 1e-9)
            {
                next = v;
                break;
            }
    }
    else
        for (double v : gizmo::kRotateSteps)
            if (v < current - 1e-9)
                next = v;
    RetopoDrawTool::setSnapAngle(next);
    log::info("Retopo Draw snap angle: {}\u00b0", next);
}
} // namespace

void registerRetopoActions()
{
    registerRetopoPolesActions();
    actions::add({
        "retopo.draw",
        "Retopo Draw",
        "Mesh",
        [] {
            scene::Graph& g = scene::Graph::current();
            std::optional<retopo::RetopoDrawTargets> t = retopo::resolveRetopoDraw(g, g.selection());
            if (TopologyTool* topo = activeTopologyTool())
                t = retopo::RetopoDrawTargets{topo->reference(), topo->retopoName()};
            if (!t)
            {
                log::warn("Retopo Draw: select the reference mesh, or a retopo mesh to resume on");
                return;
            }
            viewportTools::set(std::make_unique<RetopoDrawTool>(t->reference, t->retopo));
        },
        nullptr,
        "Draw a new quad mesh over the selected reference, or resume on a selected retopo mesh: click dots, Shift-click to fill "
        "(or, with nothing to fill, place a dot on an angle-snapped line), "
        "Ctrl-drag loops, Tab-drag extends (Tab+Shift: whole loop, Tab+Ctrl: up to the extrude angle), Shift-drag relaxes, MMB-drag tweaks, Ctrl+Shift-click deletes. Esc or Q/W/E/R exits.",
        "lila.ui.run_action(\"retopo.draw\")",
    });
    actions::add({
        .id = "retopo.setReference",
        .label = "Set Retopo Reference",
        .category = "Mesh",
        .run = setReferenceFromSelection,
        .doc = "Makes the selected mesh the reference the retopo tools draw on, and creates its retopo mesh (with the "
               "reference's symmetry) if there isn't one. With a retopo mesh and another mesh selected, that mesh becomes its new reference.",
        .python = "lila.ui.run_action(\"retopo.setReference\")",
    });
    actions::add({
        .id = "retopo.toggleSnapSpace",
        .label = "Retopo Snap in Object Space",
        .category = "Mesh",
        .run = [] { RetopoDrawTool::setSnapObjectSpace(!RetopoDrawTool::snapObjectSpace()); },
        .doc = "Retopo Draw's Shift line snap measures angles from the surface object's axes when on, from the world axes when off.",
        .python = "lila.prefs.set(\"general\", \"/retopoDraw/snapSpace\", \"world\")",
    });
    actions::add({
        .id = "retopo.increaseSnapAngle",
        .label = "Increase Retopo Snap Angle",
        .category = "Mesh",
        .run = [] { stepSnapAngle(1); },
        .doc = "Steps Retopo Draw's Shift line snap angle up to the next common angle.",
        .python = "lila.ui.run_action(\"retopo.increaseSnapAngle\")",
    });
    actions::add({
        .id = "retopo.decreaseSnapAngle",
        .label = "Decrease Retopo Snap Angle",
        .category = "Mesh",
        .run = [] { stepSnapAngle(-1); },
        .doc = "Steps Retopo Draw's Shift line snap angle down to the next common angle.",
        .python = "lila.ui.run_action(\"retopo.decreaseSnapAngle\")",
    });
    optionBox::addCustom("retopo.draw", [] {
        bool ok = false;
        double v = QInputDialog::getDouble(QApplication::activeWindow(), "Retopo Draw Options",
                                           "Extrude angle: Tab+Ctrl drag stops where the border turns more than this",
                                           RetopoDrawTool::extrudeAngle(), 1.0, 179.0, 0, &ok);
        if (ok)
            RetopoDrawTool::setExtrudeAngle(v);
    });
}
} // namespace lila::ui
