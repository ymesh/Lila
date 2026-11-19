#include "Tool.h"
#include "Entities.h"

#include <lila/Components.h>
#include <lila/Graph.h>
#include <lila/Prefs.h>
#include <lila/Tools.h>
#include <lila/Transform.h>
#include <lila/UI.h>
#include <lila/Undo.h>
#include <lila/Viewport.h>
#include <lila/ViewportTool.h>

#include <lila/ViewportRenderers.h>

#include <QKeyEvent>
#include <QTimer>
#include <QMouseEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

namespace blockwork
{
using namespace lila;
using namespace lila::scene;
using namespace lila::ui;
using geo::Index;

namespace
{
constexpr const char* kSettingsPref = "/blockwork/tool";
constexpr int kGridCells = 10;

const char* const kModeIds[] = {"block", "carve", "clip", "extend", "paint", "align", "entity", "prefab"};

json toJson(const Settings& s)
{
    return {{"mode", kModeIds[int(s.mode)]},
            {"shape", shapeNames()[size_t(s.shape)]},
            {"sides", s.options.sides},
            {"radiusToCorners", s.options.radiusToCorners},
            {"skipCaps", s.options.skipCaps},
            {"ngonCaps", s.options.ngonCaps},
            {"steps", s.options.steps},
            {"thickness", s.options.thickness},
            {"suffix", s.suffix},
            {"flip", s.flip},
            {"removeOverlap", s.removeOverlap},
            {"material", s.material},
            {"entityClass", s.entityClass},
            {"prefab", s.prefab},
            {"prefabFollowsNormal", s.prefabFollowsNormal},
            {"droneCamera", s.droneCamera}};
}

Settings fromJson(const json& j)
{
    Settings s;
    if (!j.is_object())
        return s;
    for (int i = 0; i < 8; ++i)
        if (j.value("mode", std::string()) == kModeIds[i])
            s.mode = Mode(i);
    if (auto sh = shapeFromName(j.value("shape", std::string("box"))))
        s.shape = *sh;
    s.options.sides = std::clamp(j.value("sides", 12), 3, 256);
    s.options.radiusToCorners = j.value("radiusToCorners", true);
    s.options.skipCaps = j.value("skipCaps", false);
    s.options.ngonCaps = j.value("ngonCaps", true);
    s.options.steps = std::clamp(j.value("steps", 8), 1, 512);
    s.options.thickness = std::max(0.0, j.value("thickness", 25.0));
    s.suffix = j.value("suffix", std::string());
    s.flip = j.value("flip", false);
    s.removeOverlap = j.value("removeOverlap", false);
    s.material = j.value("material", std::string());
    s.entityClass = j.value("entityClass", std::string("PlayerStart"));
    s.prefab = j.value("prefab", std::string());
    s.prefabFollowsNormal = j.value("prefabFollowsNormal", true);
    s.droneCamera = j.value("droneCamera", false);
    return s;
}

struct Plane
{
    glm::dvec3 point{0.0};
    glm::dvec3 normal{0.0, 1.0, 0.0};
    glm::dvec3 u{1.0, 0.0, 0.0};
    glm::dvec3 v{0.0, 0.0, 1.0};
};

/** A normal within about a degree of a world axis becomes that axis, so grid snapping lines up. */
glm::dvec3 tidyNormal(glm::dvec3 n)
{
    n = glm::normalize(n);
    for (int i = 0; i < 3; ++i)
        if (std::abs(n[i]) > 0.9998)
        {
            glm::dvec3 a(0.0);
            a[i] = n[i] > 0.0 ? 1.0 : -1.0;
            return a;
        }
    return n;
}

Plane makePlane(const glm::dvec3& point, const glm::dvec3& normal)
{
    Plane p;
    p.point = point;
    p.normal = tidyNormal(normal);
    planeBasis(p.normal, p.u, p.v);
    return p;
}

std::optional<glm::dvec3> rayPlane(const ViewInfo& view, QPointF px, const Plane& plane)
{
    glm::dvec3 o, d;
    view.ray(px, o, d);
    const double den = glm::dot(d, plane.normal);
    if (std::abs(den) < 1e-9)
        return std::nullopt;
    const double t = glm::dot(plane.point - o, plane.normal) / den;
    if (t < 0.0 && !view.ortho)
        return std::nullopt;
    return o + d * t;
}

/** The point snapped to the grid lines of the plane's own axes, kept on the plane. */
glm::dvec3 snapOnPlane(const Plane& plane, const glm::dvec3& p, double step)
{
    const double a = snap(glm::dot(p, plane.u), step), b = snap(glm::dot(p, plane.v), step);
    return plane.u * a + plane.v * b + plane.normal * glm::dot(plane.point, plane.normal);
}

/** How far along the line from base along dir the cursor ray passes closest. */
double alongLine(const ViewInfo& view, QPointF px, const glm::dvec3& base, const glm::dvec3& dir)
{
    glm::dvec3 o, d;
    view.ray(px, o, d);
    const glm::dvec3 w = base - o;
    const double b = glm::dot(dir, d), c = glm::dot(d, d), dd = glm::dot(dir, w), e = glm::dot(d, w);
    const double den = c - b * b;
    if (std::abs(den) < 1e-9)
        return 0.0;
    return (b * e - c * dd) / den;
}

/** The plane drawn on when the cursor is over nothing: the ground, or the view's own plane in a side view. */
Plane emptyPlane(const ViewInfo& view)
{
    if (view.ortho)
    {
        const glm::dvec3 forward = -glm::dvec3(view.view[0][2], view.view[1][2], view.view[2][2]);
        const glm::dvec3 n = tidyNormal(-forward);
        if (std::abs(n.y) < 0.9)
            return makePlane(glm::dvec3(0.0), n);
    }
    return makePlane(glm::dvec3(0.0), {0.0, 1.0, 0.0});
}

glm::vec4 accent() { return themeLinear("wirePreselect", {1.0f, 0.75f, 0.2f, 1.0f}); }

void addLine(render::FrameState& s, const glm::dvec3& a, const glm::dvec3& b, const glm::vec4& ca, const glm::vec4& cb)
{
    s.linesOnTop.push_back({a, ca});
    s.linesOnTop.push_back({b, cb});
}

void addEdges(render::FrameState& s, const geo::Mesh& m, const glm::vec4& c)
{
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
            addLine(s, m.position(m.edgePoint(e, 0)), m.position(m.edgePoint(e, 1)), c, c);
}

QString cm(double v) { return QString::number(std::abs(v), 'g', 6); }

std::string pathOf(NodeId id) { return graph().fullPath(id); }

/** Blocks whose world bounds the box overlaps, for the carve preview. */
std::vector<NodeId> blocksIn(const geo::Bounds& box)
{
    Graph& g = graph();
    std::vector<NodeId> out;
    for (NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n || !isBlock(*n))
            continue;
        const MeshPtr& m = n->as<MeshPtr>(MeshNode::kOutMesh);
        if (!m)
            continue;
        const glm::dmat4 w = n->as<glm::dmat4>(Transform::kWorldMatrix);
        geo::Bounds b;
        for (Index p = 0; p < m->pointCapacity(); ++p)
            if (m->pointValid(p))
                b.extend(glm::dvec3(w * glm::dvec4(m->position(p), 1.0)));
        if (b.valid() && b.min.x < box.max.x && b.max.x > box.min.x && b.min.y < box.max.y && b.max.y > box.min.y && b.min.z < box.max.z &&
            b.max.z > box.min.z)
            out.push_back(id);
    }
    return out;
}

geo::Mesh worldOut(NodeId id)
{
    Node* n = graph().node(id);
    const MeshPtr& m = n ? n->as<MeshPtr>(MeshNode::kOutMesh) : MeshPtr();
    if (!m)
        return {};
    geo::Mesh w = *m;
    const glm::dmat4 mat = n->as<glm::dmat4>(Transform::kWorldMatrix);
    for (Index p = 0; p < w.pointCapacity(); ++p)
        if (w.pointValid(p))
            w.setPosition(p, glm::dvec3(mat * glm::dvec4(w.position(p), 1.0)));
    return w;
}

class BlockworkTool final : public ViewportTool
{
public:
    const char* name() const override { return "Blockwork"; }

    void activate() override
    {
        reset();
        panels::show(kToolkitId);
        syncCamera();
    }

    void deactivate() override
    {
        reset();
        // Deferred, so switching modes (off and straight back on) keeps the flight.
        QTimer::singleShot(0, [] { syncCamera(); });
    }

    bool editsWhileFlying() const override { return true; }

    bool cancelStep() override
    {
        if (mStep == Step::Idle && !mDragging && !mPainting)
            return false;
        reset();
        mPainting = false;
        mStroke.reset();
        mPainted.clear();
        return true;
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        mPressAt = e->position();
        hover(e->position(), view, e->modifiers());
        const Settings& s = settings();
        switch (s.mode)
        {
        case Mode::Block:
        case Mode::Carve: pressDraw(e->modifiers()); break;
        case Mode::Prefab:
            if (!settings().prefab.empty())
                run("blockworkPrefabPlace", {{"args", {settings().prefab}}, {"position", {mCursor.x, mCursor.y, mCursor.z}},
                                             {"normal", settings().prefabFollowsNormal ? json{mPlane.normal.x, mPlane.normal.y, mPlane.normal.z} : json{0, 1, 0}},
                                             {"turn", mEntityTurn}});
            break;
        case Mode::Entity:
            if (isPointEntity())
                placeEntity();
            else
                pressDraw(e->modifiers());
            break;
        case Mode::Clip: pressClip(); break;
        case Mode::Extend: pressExtend(); break;
        case Mode::Paint: pressPaint(e->modifiers()); break;
        case Mode::Align: pressAlign(); break;
        }
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        hover(e->position(), view, e->modifiers());
        if (mPainting)
            paintHover();
        viewportTools::requestRedraw();
        return mStep != Step::Idle || mDragging || mPainting;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        hover(e->position(), view, e->modifiers());
        const QPointF moved = e->position() - mPressAt;
        const bool dragged = std::hypot(moved.x(), moved.y()) > 6.0;
        // A press-drag-release draws the footprint in one go; clicks work too.
        if (mStep == Step::Footprint && dragged && footprintSize() > 1e-6)
            finishOutline(mCursor);
        if (mDragging)
            releaseExtend(e->modifiers());
        if (mPainting)
        {
            mPainting = false;
            mStroke.reset();
            mPainted.clear();
        }
        viewportTools::requestRedraw();
        return true;
    }

    bool wheel(QWheelEvent* e, const ViewInfo&) override
    {
        // Plain wheel stays the camera's; Shift+wheel steps sides or stairs while drawing.
        if (!(e->modifiers() & Qt::ShiftModifier) || mStep == Step::Idle)
            return false;
        Settings& s = settings();
        const int step = (e->angleDelta().y() + e->angleDelta().x()) > 0 ? 1 : -1;
        if (s.shape == Shape::Stairs)
            s.options.steps = std::clamp(s.options.steps + step, 1, 512);
        else
            s.options.sides = std::clamp(s.options.sides + step, 3, 256);
        saveSettings();
        syncToolkit();
        viewportTools::requestRedraw();
        return true;
    }

    bool hotkey(const QKeyEvent& e, std::string_view) override { return anyKey(e.key(), e.modifiers()); }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override { return anyKey(e->key(), e->modifiers()); }

    void draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view) override
    {
        mView = view;
        const Settings& set = settings();
        drawGrid(s);
        const glm::vec4 ink = accent();
        if (auto at = view.project(mCursor))
            p.rect(QRectF(at->x() - 3, at->y() - 3, 6, 6), ink);

        if (set.mode == Mode::Block || set.mode == Mode::Carve || (set.mode == Mode::Entity && !isPointEntity()))
            drawShape(s, p, view, ink);
        else if (set.mode == Mode::Clip)
            drawClip(s, ink);
        else if (set.mode == Mode::Extend || set.mode == Mode::Paint)
            drawExtend(s, p, view, ink);
        else if (set.mode == Mode::Align)
            drawAlign(s, ink);
        drawToolStatus(p, view, status());
    }

    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

private:
    enum class Step
    {
        Idle,
        Edge,
        Footprint,
        Height,
        Profile,
        ClipLine,
        ClipReady,
    };

    void reset()
    {
        mStep = Step::Idle;
        mProfile.clear();
        mDragging = false;
        mFixedU = false;
        mClipKeep = 0;
        mAutoDepth = 0.0;
    }

    bool anyKey(int k, Qt::KeyboardModifiers mods) { return alignKey(k, mods) || modeKey(k, mods) || key(k, mods); }

    /**
     * Hammer-style mode letters: B C X G A E F, S leaves the tool. Shapes: 1-3 Box to Cylinder, Ctrl+1-3 Arch to Prism (4 stays wireframe),
     * Shift+B and Ctrl+B step forward and back.
     */
    bool modeKey(int k, Qt::KeyboardModifiers mods)
    {
        if (mods & (Qt::AltModifier | Qt::MetaModifier))
            return false;
        const bool ctrl = mods & Qt::ControlModifier, shift = mods & Qt::ShiftModifier;
        Settings& s = settings();
        const int shapes = int(shapeNames().size());
        if (k == Qt::Key_B && (ctrl != shift))
            return pickShape((int(s.shape) + (shift ? 1 : shapes - 1)) % shapes);
        if (!shift && k >= Qt::Key_1 && k <= Qt::Key_3)
            return pickShape((k - Qt::Key_1) + (ctrl ? int(Shape::Arch) : int(Shape::Box)));
        if (ctrl || shift)
            return false;
        switch (k)
        {
        case Qt::Key_B: switchMode(Mode::Block); return true;
        case Qt::Key_C: switchMode(Mode::Carve); return true;
        case Qt::Key_X: switchMode(Mode::Clip); return true;
        case Qt::Key_G: switchMode(Mode::Extend); return true;
        case Qt::Key_A: switchMode(s.mode == Mode::Paint ? Mode::Align : Mode::Paint); return true;
        case Qt::Key_E: switchMode(Mode::Entity); return true;
        case Qt::Key_F: switchMode(Mode::Prefab); return true;
        case Qt::Key_S:
            viewportTools::set(nullptr);
            viewportTools::requestRedraw();
            return true;
        default: return false;
        }
    }

    bool pickShape(int shape)
    {
        Settings& s = settings();
        s.shape = Shape(shape);
        switchMode(s.mode == Mode::Carve ? Mode::Carve : Mode::Block);
        return true;
    }

    void switchMode(Mode m)
    {
        settings().mode = m;
        saveSettings();
        reset();
        syncToolkit();
        viewportTools::requestRedraw();
    }

    bool key(int k, Qt::KeyboardModifiers mods)
    {
        Settings& s = settings();
        switch (k)
        {
        case Qt::Key_BracketLeft:
        case Qt::Key_BracketRight:
            setGridStep(gridStep() * (k == Qt::Key_BracketLeft ? 0.5 : 2.0));
            syncToolkit();
            viewportTools::requestRedraw();
            return true;
        case Qt::Key_Escape:
            if (mStep != Step::Idle || mDragging)
                reset();
            else
                viewportTools::set(nullptr);
            viewportTools::requestRedraw();
            return true;
        case Qt::Key_Tab:
            if (s.mode == Mode::Clip && mStep == Step::ClipReady)
                mClipKeep = (mClipKeep + 1) % 3;
            else if (s.mode == Mode::Prefab || (s.mode == Mode::Entity && isPointEntity()))
                mEntityTurn = std::fmod(mEntityTurn + ((mods & Qt::ShiftModifier) ? 315.0 : 45.0), 360.0);
            else if (s.mode == Mode::Block || s.mode == Mode::Carve)
            {
                s.options.direction = (s.options.direction + ((mods & Qt::ShiftModifier) ? 3 : 1)) % 4;
                saveSettings();
            }
            viewportTools::requestRedraw();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (mStep == Step::Profile && mProfile.size() >= 3)
                finishOutline(mProfile.back());
            else if (mStep == Step::ClipReady)
                applyClip();
            else
                return false;
            viewportTools::requestRedraw();
            return true;
        case Qt::Key_Backspace:
            if (mStep != Step::Profile || mProfile.empty())
                return false;
            mProfile.pop_back();
            if (mProfile.empty())
                mStep = Step::Idle;
            viewportTools::requestRedraw();
            return true;
        case Qt::Key_J:
            if (mStep != Step::Idle || mLastBuilt.empty() || !graph().find(mLastBuilt))
                return false;
            run("blockworkJoin", {{"args", {mLastBuilt}}});
            mLastBuilt.clear();
            return true;
        default: return false;
        }
    }

    void hover(QPointF px, const ViewInfo& view, Qt::KeyboardModifiers mods)
    {
        mView = view;
        mPx = px;
        const double step = gridStep();
        const Settings& s = settings();
        if (mDragging)
        {
            mDistance = snap(alongLine(view, px, mDragStart, mDragNormal), step);
            return;
        }
        if (mStep == Step::Height)
        {
            const double t = alongLine(view, px, mBase, mPlane.normal);
            mHeight = snap(t, step);
            return;
        }
        if (mStep == Step::Idle)
        {
            // Ctrl keeps drawing on the plane in use, wherever the cursor is.
            if (!(mods & Qt::ControlModifier) || !mHavePlane)
            {
                mHoverFace = {};
                mSurface = kNoNode;
                if (auto hit = surfaceSnap::underCursor(px, view))
                {
                    mPlane = makePlane(hit->position, hit->normal);
                    mSurface = hit->node;
                    if (s.mode == Mode::Extend || s.mode == Mode::Paint || s.mode == Mode::Align)
                        pickFace(*hit);
                }
                else
                    mPlane = emptyPlane(view);
                mHavePlane = true;
            }
        }
        if (auto on = rayPlane(view, px, mPlane))
            mCursor = snapOnPlane(mPlane, *on, step);
        if (mStep == Step::Footprint && !mFixedU)
            mU = mPlane.u, mV = mPlane.v;
        mAutoDepth = (mods & Qt::ShiftModifier) ? 0.0 : carveDepth();
    }

    /** How deep a carve goes to come out the far side of the block it was drawn on, or 0 when it is drawn off a block. */
    double carveDepth() const
    {
        if (settings().mode != Mode::Carve || (mStep != Step::Footprint && mStep != Step::Profile))
            return 0.0;
        Node* n = graph().node(mSurface);
        if (!n || !isBlock(*n))
            return 0.0;
        std::vector<glm::dvec3> points = mProfile;
        if (mStep == Step::Footprint)
        {
            const Frame f = frame();
            points = {f.origin, f.origin + f.u * f.size.x, f.origin + f.u * f.size.x + f.v * f.size.y, f.origin + f.v * f.size.y};
        }
        points.push_back(mCursor);
        glm::dvec3 mid(0.0);
        for (const glm::dvec3& p : points)
            mid += p;
        points.push_back(mid / double(points.size()));
        const double d = depthThrough(worldOut(mSurface), points, -mPlane.normal);
        // A little past the far side, so the cut never leaves a skin on it.
        return d > 0.0 ? d + 0.5 : 0.0;
    }

    /** The outline is done: carve straight through when the depth is known, else ask for the height. */
    void finishOutline(const glm::dvec3& base)
    {
        mBase = base;
        mStep = Step::Height;
        if (mAutoDepth > 0.0)
        {
            mHeight = -mAutoDepth;
            build();
        }
    }

    void pickFace(const surfaceSnap::Hit& hit)
    {
        Node* n = graph().node(hit.node);
        if (!n || !isBlock(*n))
            return;
        geo::Mesh m = worldOut(hit.node);
        const Index f = faceAt(m, hit.position, hit.normal);
        if (f == geo::kInvalid)
            return;
        mHoverFace.node = hit.node;
        mHoverFace.face = f;
        mHoverFace.outline.clear();
        for (Index p : m.facePoints(f))
            mHoverFace.outline.push_back(m.position(p));
        mHoverFace.normal = m.faceNormal(f);
        mHoverFace.point = hit.position;
    }

    double footprintSize() const
    {
        const glm::dvec3 d = mCursor - mA;
        return std::abs(glm::dot(d, mU)) + std::abs(glm::dot(d, mV));
    }

    void pressDraw(Qt::KeyboardModifiers mods)
    {
        const Settings& s = settings();
        switch (mStep)
        {
        case Step::Idle:
            mA = mCursor;
            mU = mPlane.u, mV = mPlane.v;
            mFixedU = false;
            if (s.shape == Shape::Prism)
                mStep = Step::Profile, mProfile = {mA};
            else
                mStep = (mods & Qt::ShiftModifier) ? Step::Edge : Step::Footprint;
            break;
        case Step::Edge:
        {
            const glm::dvec3 d = mCursor - mA;
            if (glm::length(d) < 1e-6)
                break;
            mU = glm::normalize(d);
            mV = acrossAxis(mPlane.normal, mU);
            mEdgeLength = glm::length(d);
            mFixedU = true;
            mStep = Step::Footprint;
            break;
        }
        case Step::Footprint:
            if (footprintSize() < 1e-6 && !mFixedU)
                break;
            finishOutline(mCursor);
            break;
        case Step::Profile:
            if (mProfile.size() >= 3)
                if (auto first = mView.project(mProfile.front()), now = mView.project(mCursor); first && now &&
                    std::hypot(first->x() - now->x(), first->y() - now->y()) < 10.0)
                {
                    finishOutline(mProfile.back());
                    break;
                }
            if (glm::length(mCursor - mProfile.back()) > 1e-6)
                mProfile.push_back(mCursor);
            break;
        case Step::Height: build(); break;
        default: break;
        }
    }

    /** The frame the current drawing describes. */
    Frame frame() const
    {
        Frame f;
        f.origin = mA;
        f.u = mU;
        f.v = mV;
        f.normal = mPlane.normal;
        const glm::dvec3 d = (mStep == Step::Height ? mBase : mCursor) - mA;
        f.size = {mFixedU ? mEdgeLength : glm::dot(d, mU), glm::dot(d, mV), mStep == Step::Height ? mHeight : -mAutoDepth};
        return f;
    }

    ShapeOptions shapeOptions() const
    {
        ShapeOptions o = settings().options;
        // Carving with an arch cuts a doorway.
        o.opening = settings().mode == Mode::Carve;
        for (const glm::dvec3& p : mProfile)
            o.profile.push_back({glm::dot(p - mA, mU), glm::dot(p - mA, mV)});
        return o;
    }

    json blockCall(const Frame& f, bool carve) const
    {
        const Settings& s = settings();
        const ShapeOptions o = shapeOptions();
        json call = {{"shape", shapeNames()[size_t(s.shape)]},
                     {"origin", {f.origin.x, f.origin.y, f.origin.z}},
                     {"size", {f.size.x, f.size.y, f.size.z}},
                     {"normal", {f.normal.x, f.normal.y, f.normal.z}},
                     {"uAxis", {f.u.x, f.u.y, f.u.z}}};
        if (s.shape == Shape::Cylinder || s.shape == Shape::Arch)
            call["sides"] = o.sides;
        if (s.shape == Shape::Cylinder)
            call["radiusToCorners"] = o.radiusToCorners, call["skipCaps"] = o.skipCaps, call["ngonCaps"] = o.ngonCaps;
        if (s.shape == Shape::Stairs)
            call["steps"] = o.steps;
        if (s.shape == Shape::Arch && o.opening)
            call["opening"] = true;
        else if (s.shape == Shape::Arch)
            call["thickness"] = o.thickness;
        if (o.direction)
            call["direction"] = o.direction;
        if (s.shape == Shape::Prism)
        {
            json prof = json::array();
            for (const glm::dvec2& p : o.profile)
                prof.push_back({p.x, p.y});
            call["profile"] = prof;
        }
        if (carve)
        {
            call["name"] = "cutter1";
            call["parent"] = "";
            return call;
        }
        if (!s.suffix.empty())
            call["suffix"] = s.suffix;
        if (s.flip)
            call["flip"] = true;
        if (s.removeOverlap)
            call["removeOverlap"] = true;
        return call;
    }

    bool isPointEntity() const
    {
        const EntityClass* c = findEntityClass(settings().entityClass);
        return !c || c->kind != "volume";
    }

    void placeEntity()
    {
        const Settings& s = settings();
        run("blockworkEntity", {{"class", s.entityClass}, {"position", {mCursor.x, mCursor.y, mCursor.z}}, {"turn", mEntityTurn}});
    }

    void build()
    {
        const Settings& s = settings();
        const Frame f = frame();
        try
        {
            if (s.mode == Mode::Entity)
            {
                // Volumes are boxes along the world axes, filling what was drawn.
                const geo::Bounds b = buildShape(Shape::Box, f, {}).bounds();
                if (b.valid())
                    ops::execute("blockworkEntity", {{"class", s.entityClass}, {"origin", {b.min.x, b.min.y, b.min.z}},
                                                     {"size", {b.max.x - b.min.x, b.max.y - b.min.y, b.max.z - b.min.z}}}, CallSource::UI);
            }
            else if (s.mode == Mode::Carve)
            {
                UndoChunk chunk("Carve");
                const std::string cutter = ops::execute("blockworkBlock", blockCall(f, true), CallSource::UI).get<std::string>();
                ops::execute("blockworkCarve", {{"args", {cutter}}}, CallSource::UI);
            }
            else
                mLastBuilt = ops::execute("blockworkBlock", blockCall(f, false), CallSource::UI).get<std::string>();
        }
        catch (const std::exception& e)
        {
            log::warn("{}", e.what());
        }
        reset();
    }

    void pressClip()
    {
        if (mStep == Step::Idle)
            mA = mCursor, mStep = Step::ClipLine;
        else if (mStep == Step::ClipLine && glm::length(mCursor - mA) > 1e-6)
            mBase = mCursor, mStep = Step::ClipReady;
        else if (mStep == Step::ClipReady)
            applyClip();
    }

    glm::dvec3 clipNormal() const
    {
        const glm::dvec3 end = mStep == Step::ClipReady ? mBase : mCursor;
        const glm::dvec3 n = glm::cross(end - mA, mPlane.normal);
        return glm::length(n) > 1e-9 ? glm::normalize(n) : glm::dvec3(1.0, 0.0, 0.0);
    }

    /** The blocks a clip acts on: the selected ones, or every block the plane crosses. */
    std::vector<std::string> clipTargets() const
    {
        Graph& g = graph();
        std::vector<std::string> out;
        for (NodeId id : g.selection())
            if (Node* n = g.node(id); n && isBlock(*n))
                out.push_back(pathOf(id));
        if (!out.empty())
            return out;
        const glm::dvec3 n = clipNormal();
        for (NodeId id : g.nodes())
        {
            Node* k = g.node(id);
            if (!k || !isBlock(*k))
                continue;
            const geo::Mesh m = worldOut(id);
            bool front = false, back = false;
            for (Index p = 0; p < m.pointCapacity(); ++p)
                if (m.pointValid(p))
                {
                    const double d = glm::dot(m.position(p) - mA, n);
                    front = front || d > 1e-3;
                    back = back || d < -1e-3;
                }
            if (front && back)
                out.push_back(pathOf(id));
        }
        return out;
    }

    void applyClip()
    {
        const std::vector<std::string> targets = clipTargets();
        if (!targets.empty())
        {
            const glm::dvec3 n = clipNormal();
            static const char* const keeps[] = {"front", "back", "both"};
            run("blockworkClip", {{"args", targets}, {"point", {mA.x, mA.y, mA.z}}, {"normal", {n.x, n.y, n.z}}, {"keep", keeps[mClipKeep]}});
        }
        reset();
    }

    void pressExtend()
    {
        if (mHoverFace.node == kNoNode)
            return;
        mDragging = true;
        mDragFace = mHoverFace;
        mDragStart = mHoverFace.point;
        mDragNormal = mHoverFace.normal;
        mDistance = 0.0;
    }

    void releaseExtend(Qt::KeyboardModifiers mods)
    {
        mDragging = false;
        if (std::abs(mDistance) < 1e-6 || !graph().node(mDragFace.node))
            return;
        const char* mode = (mods & Qt::ControlModifier) ? "copy" : (mods & Qt::ShiftModifier) ? "extrude" : "move";
        run("blockworkExtend", {{"args", {pathOf(mDragFace.node)}}, {"faces", {mDragFace.face}}, {"distance", mDistance}, {"mode", mode}});
        mHoverFace = {};
    }

    void run(const char* cmd, const json& call)
    {
        try
        {
            ops::execute(cmd, call, CallSource::UI);
        }
        catch (const std::exception& e)
        {
            log::warn("{}", e.what());
        }
    }

    void pressPaint(Qt::KeyboardModifiers mods)
    {
        if (mHoverFace.node == kNoNode)
            return;
        if (mods & Qt::ControlModifier)
        {
            // Pick: the face's texture becomes what Paint lays down.
            mPickNode = graph().fullPath(mHoverFace.node);
            mPickFace = mHoverFace.face;
            if (Node* n = graph().node(mHoverFace.node))
                if (const MeshPtr& m = n->as<MeshPtr>(MeshNode::kOutMesh))
                    if (std::span<const int32_t> ids = m->faceAttrs().get<int32_t>(geo::attr::kMaterial);
                        mPickFace < ids.size() && ids[mPickFace] >= 0 && size_t(ids[mPickFace]) < m->materialNames().size())
                    {
                        settings().material = m->materialNames()[size_t(ids[mPickFace])];
                        saveSettings();
                    }
            return;
        }
        mPainting = true;
        mStroke.emplace("Paint Texture");
        mPainted.clear();
        paintHover();
    }

    void paintHover()
    {
        if (mHoverFace.node == kNoNode || !mPainted.insert({mHoverFace.node, mHoverFace.face}).second)
            return;
        const std::string block = graph().fullPath(mHoverFace.node);
        if (!mPickNode.empty() && graph().find(mPickNode))
            run("blockworkCopyTexture", {{"args", {block}}, {"source", mPickNode}, {"sourceFace", mPickFace}, {"faces", {mHoverFace.face}}});
        else if (!settings().material.empty())
            run("blockworkMaterial", {{"args", {block}}, {"material", settings().material}, {"faces", {mHoverFace.face}}});
        else
            log::warn("Paint: pick a texture in Block Textures, or Ctrl+click a face to take its texture");
    }

    /** The hovered face's edge nearest the cursor, counted round the face. */
    int nearestEdge() const
    {
        const auto& o = mHoverFace.outline;
        int best = -1;
        double bestD = 1e300;
        for (size_t i = 0; i < o.size(); ++i)
        {
            const glm::dvec3 a = o[i], b = o[(i + 1) % o.size()], ab = b - a;
            const double t = std::clamp(glm::dot(mHoverFace.point - a, ab) / std::max(1e-12, glm::dot(ab, ab)), 0.0, 1.0);
            const double d = glm::length(mHoverFace.point - (a + ab * t));
            if (d < bestD)
                bestD = d, best = int(i);
        }
        return best;
    }

    void pressAlign()
    {
        const int edge = nearestEdge();
        if (mHoverFace.node == kNoNode || edge < 0)
            return;
        mAlignNode = graph().fullPath(mHoverFace.node);
        mAlignFace = mHoverFace.face;
        mAlignEdge = edge;
        mAlignFit = "none";
        applyAlign();
    }

    void applyAlign()
    {
        if (mAlignNode.empty() || !graph().find(mAlignNode))
            return;
        static const char* const sides[] = {"bottom", "left", "top", "right"};
        run("blockworkAlignEdge", {{"args", {mAlignNode}}, {"face", mAlignFace}, {"edge", mAlignEdge}, {"side", sides[mAlignSide]}, {"fit", mAlignFit}});
    }

    /** In Align: arrows pick the texture side laid on the edge, Shift+arrows fit along that axis, Backspace resets. */
    bool alignKey(int k, Qt::KeyboardModifiers mods)
    {
        if (settings().mode != Mode::Align)
            return false;
        const bool fit = mods & Qt::ShiftModifier;
        switch (k)
        {
        case Qt::Key_Down:
        case Qt::Key_Up:
            if (fit)
                mAlignFit = "height";
            else
                mAlignSide = k == Qt::Key_Down ? 0 : 2;
            break;
        case Qt::Key_Left:
        case Qt::Key_Right:
            if (fit)
                mAlignFit = "width";
            else
                mAlignSide = k == Qt::Key_Left ? 1 : 3;
            break;
        case Qt::Key_Backspace:
            if (!mAlignNode.empty() && graph().find(mAlignNode))
                run("blockworkTexture", {{"args", {mAlignNode}}, {"faces", {mAlignFace}}, {"reset", true}});
            viewportTools::requestRedraw();
            return true;
        default: return false;
        }
        applyAlign();
        viewportTools::requestRedraw();
        return true;
    }

    void drawAlign(render::FrameState& s, const glm::vec4& ink) const
    {
        const auto& o = mHoverFace.outline;
        if (mHoverFace.node == kNoNode || o.empty())
            return;
        const glm::vec4 dim(glm::vec3(ink), 0.4f);
        const int edge = nearestEdge();
        for (size_t i = 0; i < o.size(); ++i)
            addLine(s, o[i], o[(i + 1) % o.size()], int(i) == edge ? ink : dim, int(i) == edge ? ink : dim);
    }

    /** Grid lines on the plane under the cursor, fading out from it. */
    void drawGrid(render::FrameState& s) const
    {
        if (!mHavePlane)
            return;
        const double step = gridStep();
        const glm::dvec3 c = mCursor;
        const double reach = step * kGridCells;
        for (int axis = 0; axis < 2; ++axis)
        {
            const glm::dvec3 along = axis ? mPlane.u : mPlane.v, across = axis ? mPlane.v : mPlane.u;
            for (int k = -kGridCells; k <= kGridCells; ++k)
            {
                const float a = 0.35f * (1.0f - float(std::abs(k)) / float(kGridCells + 1));
                const glm::vec4 mid(0.75f, 0.75f, 0.75f, a), end(0.75f, 0.75f, 0.75f, 0.0f);
                const glm::dvec3 o = c + across * (double(k) * step);
                addLine(s, o - along * reach, o, end, mid);
                addLine(s, o, o + along * reach, mid, end);
            }
        }
    }

    void drawShape(render::FrameState& s, OverlayPainter& p, const ViewInfo& view, const glm::vec4& ink)
    {
        const Settings& set = settings();
        if (mStep == Step::Profile)
        {
            for (size_t i = 0; i + 1 < mProfile.size(); ++i)
                addLine(s, mProfile[i], mProfile[i + 1], ink, ink);
            if (!mProfile.empty())
                addLine(s, mProfile.back(), mCursor, ink, ink);
            return;
        }
        if (mStep == Step::Edge)
        {
            addLine(s, mA, mCursor, ink, ink);
            label(p, view, mCursor, cm(glm::length(mCursor - mA)));
            return;
        }
        if (mStep != Step::Footprint && mStep != Step::Height)
            return;
        const Frame f = frame();
        if (mStep == Step::Footprint && mAutoDepth <= 0.0)
        {
            const glm::dvec3 a = f.origin, b = a + f.u * f.size.x, c = b + f.v * f.size.y, d = a + f.v * f.size.y;
            addLine(s, a, b, ink, ink), addLine(s, b, c, ink, ink), addLine(s, c, d, ink, ink), addLine(s, d, a, ink, ink);
            label(p, view, mCursor, cm(f.size.x) + " \xc3\x97 " + cm(f.size.y));
            return;
        }
        const geo::Mesh m = buildShape(set.shape, f, shapeOptions());
        addEdges(s, m, ink);
        QString size = cm(f.size.x) + " \xc3\x97 " + cm(f.size.y) + " \xc3\x97 " + cm(f.size.z);
        if (const ShapeOptions o = shapeOptions(); set.shape == Shape::Arch && !o.opening)
        {
            const double t = archFrame(o.direction % 2 ? f.size.y : f.size.x, f.size.z, o.thickness);
            if (t < o.thickness)
                size += "  frame " + cm(t);
        }
        label(p, view, (mStep == Step::Height ? mBase : mCursor) + f.normal * f.size.z, size);
        if (set.mode == Mode::Carve && !m.empty())
        {
            const glm::vec4 warn = themeLinear("warning", {1.0f, 0.4f, 0.2f, 1.0f});
            for (NodeId id : blocksIn(m.bounds()))
                addEdges(s, worldOut(id), glm::vec4(glm::vec3(warn), 0.6f));
        }
    }

    void drawClip(render::FrameState& s, const glm::vec4& ink) const
    {
        if (mStep == Step::Idle)
            return;
        const glm::dvec3 end = mStep == Step::ClipReady ? mBase : mCursor;
        addLine(s, mA, end, ink, ink);
        if (mStep != Step::ClipReady)
            return;
        // The cut plane as a frame across the line, and an arrow to the side kept.
        const glm::dvec3 along = glm::normalize(end - mA), up = mPlane.normal, n = clipNormal();
        const double len = glm::length(end - mA) + gridStep() * 8.0, tall = gridStep() * 8.0;
        const glm::dvec3 o = mA - along * gridStep() * 4.0;
        const glm::dvec3 q[4] = {o - up * tall, o + along * len - up * tall, o + along * len + up * tall, o + up * tall};
        const glm::vec4 dim(glm::vec3(ink), 0.5f);
        for (int i = 0; i < 4; ++i)
            addLine(s, q[i], q[(i + 1) % 4], dim, dim);
        const glm::dvec3 mid = (mA + end) * 0.5;
        const double arrow = gridStep() * 3.0;
        if (mClipKeep != 1)
            addLine(s, mid, mid + n * arrow, ink, ink);
        if (mClipKeep != 0)
            addLine(s, mid, mid - n * arrow, ink, ink);
    }

    void drawExtend(render::FrameState& s, OverlayPainter& p, const ViewInfo& view, const glm::vec4& ink)
    {
        const HoverFace& f = mDragging ? mDragFace : mHoverFace;
        if (f.node == kNoNode || f.outline.empty())
            return;
        const glm::dvec3 off = mDragging ? f.normal * mDistance : glm::dvec3(0.0);
        for (size_t i = 0; i < f.outline.size(); ++i)
        {
            const glm::dvec3 a = f.outline[i] + off, b = f.outline[(i + 1) % f.outline.size()] + off;
            addLine(s, a, b, ink, ink);
            if (mDragging)
                addLine(s, f.outline[i], a, glm::vec4(glm::vec3(ink), 0.5f), glm::vec4(glm::vec3(ink), 0.5f));
        }
        if (mDragging)
            label(p, view, mDragStart + off, (mDistance < 0 ? "-" : "") + cm(mDistance));
    }

    static void label(OverlayPainter& p, const ViewInfo& view, const glm::dvec3& at, const QString& text)
    {
        if (auto px = view.project(at))
            p.label(*px + QPointF(14, -14), text);
    }

    ToolStatus status() const
    {
        const Settings& s = settings();
        ToolStatus st;
        st.tool = "Blockwork";
        st.mode = (s.mode == Mode::Block || s.mode == Mode::Carve) ? QString("%1 %2").arg(modeLabel(s.mode), shapeLabel(s.shape)) : modeLabel(s.mode);
        st.fields.push_back({"grid", cm(gridStep())});
        if (s.shape == Shape::Cylinder || s.shape == Shape::Arch)
            st.fields.push_back({"sides", QString::number(s.options.sides)});
        if (s.shape == Shape::Stairs)
            st.fields.push_back({"steps", QString::number(s.options.steps)});
        st.keys = {{"[ ]", "grid"}, {"Ctrl", "keep plane"}, {"Esc", mStep == Step::Idle ? "leave" : "cancel"}};
        switch (mStep)
        {
        case Step::Idle:
            st.prompt = s.mode == Mode::Clip     ? "click the cut line's start"
                        : s.mode == Mode::Extend ? "drag a block's face"
                        : s.mode == Mode::Paint  ? (s.material.empty() && mPickNode.empty() ? "Ctrl+click a face to take its texture" : "click or drag over faces")
                        : s.mode == Mode::Align  ? "click near the face edge to line the texture up with"
                        : s.shape == Shape::Prism ? "click the outline's first point"
                                                  : "click a corner";
            if (s.mode == Mode::Paint)
            {
                st.keys.insert(st.keys.begin(), {"Ctrl+click", "take a face's texture"});
                if (!s.material.empty())
                    st.fields.push_back({"texture", QString::fromStdString(s.material)});
            }
            if (s.mode == Mode::Align)
                st.keys.insert(st.keys.begin(), {{"Arrows", "side on the edge"}, {"Shift+arrows", "fit height, width"}, {"Backspace", "reset"}});
            if (s.mode == Mode::Prefab)
            {
                st.mode = QString("Prefab %1").arg(QString::fromStdString(std::filesystem::path(s.prefab).stem().string()));
                st.prompt = s.prefab.empty() ? "pick a prefab in the Prefabs panel" : "click where it stands";
                st.fields.push_back({"turn", QString("%1\xc2\xb0").arg(mEntityTurn)});
                st.keys.insert(st.keys.begin(), {"Tab", "turn 45\xc2\xb0"});
            }
            if (s.mode == Mode::Entity)
            {
                st.mode = QString("Entity %1").arg(QString::fromStdString(s.entityClass));
                st.prompt = isPointEntity() ? "click where it stands" : "click a corner of the volume";
                if (isPointEntity())
                {
                    st.fields.push_back({"turn", QString("%1\xc2\xb0").arg(mEntityTurn)});
                    st.keys.insert(st.keys.begin(), {"Tab", "turn 45\xc2\xb0"});
                }
            }
            if (s.mode == Mode::Block || s.mode == Mode::Carve)
                st.keys.insert(st.keys.begin(), {{"Shift+click", "start an angled edge"}, {"1-3 Ctrl+1-3", "shape"}});
            st.keys.push_back({"B C X G A E F", "modes"});
            if (s.mode == Mode::Extend)
                st.keys.insert(st.keys.begin(), {{"Shift", "extrude"}, {"Ctrl", "copy as a block"}});
            if (!mLastBuilt.empty())
                st.keys.insert(st.keys.begin(), {"J", "join the last block into what it touches"});
            break;
        case Step::Edge: st.prompt = "click the edge's end"; break;
        case Step::Footprint:
            st.prompt = mAutoDepth > 0.0 ? "click the opposite corner to cut through" : "click the opposite corner";
            if (s.mode == Mode::Carve)
                st.keys.insert(st.keys.begin(), {"Shift", "set the depth"});
            break;
        case Step::Height:
            st.prompt = "click the height";
            st.keys.insert(st.keys.begin(), {"Tab", "turn"});
            if (s.shape == Shape::Cylinder || s.shape == Shape::Arch || s.shape == Shape::Stairs)
                st.keys.insert(st.keys.begin(), {"Shift+wheel", s.shape == Shape::Stairs ? "steps" : "sides"});
            break;
        case Step::Profile:
            st.prompt = "click points; click the first or Enter to close";
            st.keys.insert(st.keys.begin(), {"Backspace", "remove last point"});
            break;
        case Step::ClipLine: st.prompt = "click the cut line's end"; break;
        case Step::ClipReady:
        {
            static const char* const keeps[] = {"keep front", "keep back", "keep both"};
            st.prompt = QString("click or Enter to cut: %1").arg(keeps[mClipKeep]);
            st.keys.insert(st.keys.begin(), {"Tab", "side kept"});
            break;
        }
        }
        if (mDragging)
            st.prompt = "release to push the face";
        return st;
    }

    struct HoverFace
    {
        NodeId node = kNoNode;
        Index face = geo::kInvalid;
        std::vector<glm::dvec3> outline;
        glm::dvec3 normal{0.0};
        glm::dvec3 point{0.0};
    };

    ViewInfo mView;
    QPointF mPx, mPressAt;
    Step mStep = Step::Idle;
    Plane mPlane;
    bool mHavePlane = false;
    glm::dvec3 mCursor{0.0}, mA{0.0}, mBase{0.0}, mU{1.0, 0.0, 0.0}, mV{0.0, 0.0, 1.0};
    bool mFixedU = false;
    double mEdgeLength = 0.0, mHeight = 0.0;
    /** Carve's depth through the block it is drawn on; 0 asks for the depth by hand. */
    double mAutoDepth = 0.0;
    NodeId mSurface = kNoNode;
    std::vector<glm::dvec3> mProfile;
    int mClipKeep = 0;
    HoverFace mHoverFace, mDragFace;
    bool mDragging = false;
    glm::dvec3 mDragStart{0.0}, mDragNormal{0.0};
    double mDistance = 0.0;
    std::string mLastBuilt;
    bool mPainting = false;
    std::optional<UndoChunk> mStroke;
    std::set<std::pair<NodeId, Index>> mPainted;
    std::string mPickNode;
    Index mPickFace = 0;
    std::string mAlignNode, mAlignFit = "none";
    Index mAlignFace = 0;
    int mAlignEdge = 0, mAlignSide = 0;
    double mEntityTurn = 0.0;
};
} // namespace

Settings& settings()
{
    static Settings s = fromJson(PrefStore::get().value("general", kSettingsPref));
    return s;
}

void saveSettings() { PrefStore::get().set("general", kSettingsPref, toJson(settings())); }

void syncCamera()
{
    static bool ours = false;
    const bool want = settings().droneCamera && viewportTools::activeId() == kToolId;
    if (want && !drone::activeViewFlying())
        ours = drone::setActiveViewFlying(true);
    else if (!want && ours)
    {
        ours = false;
        drone::setActiveViewFlying(false);
    }
}

const char* modeLabel(Mode m)
{
    static const char* const k[] = {"Block", "Carve", "Clip", "Extend", "Paint", "Align", "Entity", "Prefab"};
    return k[int(m)];
}

const char* shapeLabel(Shape s)
{
    static const char* const k[] = {"Box", "Wedge", "Cylinder", "Arch", "Stairs", "Prism"};
    return k[int(s)];
}

void startTool(Mode mode)
{
    settings().mode = mode;
    saveSettings();
    if (viewportTools::activeId() == kToolId)
    {
        viewportTools::set(nullptr);
    }
    viewportTools::activate(kToolId);
    syncToolkit();
}

void startTool(Shape shape)
{
    Settings& s = settings();
    s.shape = shape;
    if (s.mode != Mode::Block && s.mode != Mode::Carve)
        s.mode = Mode::Block;
    startTool(s.mode);
}

/** Move, Rotate and Scale keep a selection of only blocks on the Blockwork grid. */
static std::optional<SelectionSnap> blockSnap()
{
    Graph& g = graph();
    const std::vector<NodeId>& sel = g.selection();
    if (sel.empty())
        return std::nullopt;
    g.evaluate();
    geo::Bounds box;
    for (NodeId id : sel)
    {
        const Node* n = g.node(id);
        if (!n || !isBlock(*n))
            return std::nullopt;
        box.extend(worldBounds(g, id));
    }
    if (!box.valid())
        return std::nullopt;
    const double step = gridStep();
    return SelectionSnap{.moveStep = step, .absolute = gridAbsolute(), .anchor = box.min, .rotateStep = 15.0, .scaleStep = step, .extent = box.size()};
}

void registerTool(ext::Context& ctx)
{
    viewportTools::add(ctx, {.id = kToolId, .label = "Blockwork", .icon = "box", .create = [] { return std::make_unique<BlockworkTool>(); }});
    const int snap = tools::addSelectionSnap(blockSnap);
    ctx.track("blockwork selection snap", [snap] { tools::removeSelectionSnap(snap); });
}
} // namespace blockwork
