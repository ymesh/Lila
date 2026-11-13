#include "ui/CurveUi.h"
#include "ui/CurveToolsCommon.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "curve/CurveNodes.h"
#include "curve/Pipe.h"
#include "curve/PipeNode.h"
#include "geo/Conform.h"
#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/StepHistory.h"
#include "ui/ViewportTool.h"

#include <glm/gtc/constants.hpp>

#include <array>

namespace lila::ui
{
namespace
{
using namespace curveTools;
using curve::Spline;

const std::array<const char*, 5> kPipePresets = {"", "pipe", "hose", "cable", "conduit"};

/** The selected face region's border (or a selected closed edge loop): where a pipe starts and which way it leaves. */
struct StartLoop
{
    std::vector<D3> points;
    D3 centre{0.0};
    D3 normal{0.0};
    double radius = 0.0;
};

std::optional<StartLoop> selectedLoop()
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    for (scene::ComponentMode m : {scene::ComponentMode::Face, scene::ComponentMode::Edge})
        for (auto& [id, ids] : scene::components::selected(m))
        {
            scene::Node* n = g.node(id);
            if (!n || !n->type().isA("mesh") || ids.empty())
                continue;
            scene::MeshPtr cage = scene::modifiers::cage(g, id);
            if (!cage)
                continue;
            geo::ShapeSelection sel = m == scene::ComponentMode::Face ? geo::shapeOfFaces(*cage, ids) : geo::shapeOfEdges(*cage, ids);
            for (const geo::EdgePath& c : sel.chains)
            {
                if (!c.closed || c.points.size() < 3)
                    continue;
                StartLoop out;
                glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
                for (geo::Index p : c.points)
                    out.points.push_back(D3(w * glm::dvec4(cage->position(p), 1.0)));
                for (const D3& p : out.points)
                    out.centre += p;
                out.centre /= double(out.points.size());
                glm::dmat3 nm = glm::transpose(glm::inverse(glm::dmat3(w)));
                if (m == scene::ComponentMode::Face)
                    for (geo::Index f : ids)
                        out.normal += nm * cage->faceAreaVector(f);
                else
                {
                    for (size_t k = 0; k < out.points.size(); ++k)
                        out.normal += glm::cross(out.points[k], out.points[(k + 1) % out.points.size()]);
                    // A loop's winding says nothing about the mesh: point away from the faces beside it.
                    D3 beside(0.0);
                    for (geo::Index e : ids)
                        cage->forEachEdgeCorner(e, [&](geo::Index c) {
                            beside += D3(w * glm::dvec4(cage->faceCentroid(cage->cornerFace(c)), 1.0)) - out.centre;
                        });
                    if (glm::dot(out.normal, beside) > 0.0)
                        out.normal = -out.normal;
                }
                if (glm::length(out.normal) < 1e-12)
                    continue;
                out.normal = glm::normalize(out.normal);
                for (const D3& p : out.points)
                    out.radius += glm::distance(p, out.centre) / double(out.points.size());
                return out;
            }
        }
    return std::nullopt;
}

/**
 * Pipe: draw a path like the Bezier pen (click corners, drag smooth points), on the construction plane or along mesh
 * surfaces, and see the pipe as you go. B-drag or Ctrl+scroll sets the radius of the next point, so the pipe can swell
 * and narrow along its length. Enter builds it with live history; with nothing drawn, Enter pipes the selected curves.
 * A selected face or closed loop is the start: the pipe leaves its centre along its normal, from the loop itself.
 * While the tool is active, undo, redo and Backspace step through the placed points and never reach the scene.
 */
class PipeTool final : public ViewportTool
{
public:
    const char* name() const override { return "Pipe"; }
    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }
    bool ownsBrushRadius() const override { return true; }

    void activate() override
    {
        mRadius = prefD("radius", 5.0);
        mSides = int(prefD("sides", 12.0));
        mWall = prefD("wall", 0.0);
        mPreset = int(prefD("preset", 0.0)) % int(kPipePresets.size());
        mFromLoop = prefB("fromLoop", true);
        mAttach = prefB("attach", false);
        mSurface = prefB("surface", true);
        mStart = selectedLoop();
        if (mStart && mFromLoop)
            mRadius = startRadius();
        restart();
        log::info("Pipe: click and drag to draw the path, B-drag or Ctrl+scroll sets the next point's radius, +/- sides, Enter builds it");
    }

    void deactivate() override { viewportTools::requestRedraw(); }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (tools::brushRadiusDrag(e, false))
        {
            mRadiusDrag = true;
            mPressPx = e->position();
            mStartRadius = mRadius;
            return true;
        }
        if (e->button() != Qt::LeftButton)
            return false;
        D3 normal(0.0);
        auto p = placeAt(e->position(), view, &normal);
        if (!p)
            return true;
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
        mSpline.radius.push_back(mRadius);
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
        if (mRadiusDrag)
        {
            setRadius(mStartRadius * std::exp((e->position().x() - mPressPx.x()) * 0.01));
            return true;
        }
        if (mDragging)
        {
            size_t i = mDragging - 1;
            if (distPx(e->position(), mPressPx) < 3.0)
                return true;
            D3 a = mSpline.points[i], o, d;
            view.ray(e->position(), o, d);
            double den = glm::dot(d, mDragNormal);
            if (std::abs(den) > 1e-9)
            {
                D3 w = o + d * (glm::dot(a - o, mDragNormal) / den);
                bool brk = e->modifiers() & Qt::ControlModifier;
                mSpline.handleR[i] = w;
                mSpline.typeR[i] = brk ? curve::HandleType::Free : curve::HandleType::Aligned;
                if (!brk)
                {
                    mSpline.handleL[i] = a - (w - a);
                    mSpline.typeL[i] = curve::HandleType::Aligned;
                }
                else if (mSpline.typeL[i] == curve::HandleType::Vector)
                    mSpline.typeL[i] = curve::HandleType::Free;
                curve::updateHandles(mSpline);
            }
            viewportTools::requestRedraw();
            return true;
        }
        mHover = placeAt(e->position(), view, nullptr);
        viewportTools::requestRedraw();
        return false;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (mRadiusDrag && (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton))
        {
            mRadiusDrag = false;
            return true;
        }
        if (e->button() != Qt::LeftButton)
            return false;
        mDragging = 0;
        viewportTools::requestRedraw();
        return true;
    }

    bool wheel(QWheelEvent* e, const ViewInfo&) override
    {
        double steps = e->angleDelta().y() / 120.0;
        if (e->modifiers() & Qt::ControlModifier)
        {
            setRadius(mRadius * std::pow(1.1, steps));
            return true;
        }
        if (e->modifiers() & Qt::ShiftModifier)
        {
            setSides(mSides + (steps > 0 ? 1 : -1));
            return true;
        }
        return false;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        switch (e->key())
        {
        case Qt::Key_Return:
        case Qt::Key_Enter: finish(); return true;
        case Qt::Key_Plus:
        case Qt::Key_Equal: setSides(mSides + 1); return true;
        case Qt::Key_Minus:
        case Qt::Key_Underscore: setSides(mSides - 1); return true;
        case Qt::Key_P:
            mPreset = (mPreset + 1) % int(kPipePresets.size());
            setPref("preset", mPreset);
            if (json v = curve::pipePreset(kPipePresets[size_t(mPreset)]); !v.is_null())
            {
                setRadius(v["radius"].get<double>());
                setSides(v["sides"].get<int>());
                mWall = v["wallThickness"].get<double>();
                setPref("wall", mWall);
            }
            break;
        case Qt::Key_T:
            mWall = mWall > 0.0 ? 0.0 : std::max(0.05, mRadius * 0.1);
            setPref("wall", mWall);
            break;
        case Qt::Key_F:
            mFromLoop = !mFromLoop;
            setPref("fromLoop", mFromLoop);
            if (mStart && mFromLoop)
                mRadius = startRadius();
            if (mStart && mSpline.size())
                mSpline.radius[0] = startRadius();
            break;
        case Qt::Key_A:
            mAttach = !mAttach;
            setPref("attach", mAttach);
            break;
        case Qt::Key_Y: prefs().set("general", "/curveTools/symmetry", (symmetryPref() + 1) % 4); break;
        case Qt::Key_S:
            mSurface = !mSurface;
            setPref("surface", mSurface);
            break;
        case Qt::Key_L: mPlacer.toggleLock(!mPlacer.lockEnabled()); break;
        case Qt::Key_Backspace: stepBack(); break;
        case Qt::Key_Escape:
            if (mSpline.size() <= (mStart ? 1u : 0u))
                exitTool();
            restart();
            break;
        default: return false;
        }
        viewportTools::requestRedraw();
        return true;
    }

    bool hotkey(const QKeyEvent& e, std::string_view action) override
    {
        switch (mHistory.route(e.key(), action))
        {
        case StepKey::Undo: stepBack(); return true;
        case StepKey::Redo: stepForward(); return true;
        case StepKey::None: break;
        }
        // The tool's letters (S, L, T, F, A) are app hotkeys too; it takes them while active.
        return plainKey(e) && keyPress(const_cast<QKeyEvent*>(&e), ViewInfo{});
    }

    void draw(render::FrameState& f, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();

        Spline preview = mSpline;
        if (mHover && !mDragging && view.camera == mHoverView && preview.size())
        {
            preview.points.push_back(*mHover);
            preview.handleL.push_back(*mHover);
            preview.handleR.push_back(*mHover);
            preview.typeL.push_back(curve::HandleType::Vector);
            preview.typeR.push_back(curve::HandleType::Vector);
            preview.radius.push_back(mRadius);
            curve::updateHandles(preview);
        }
        if (preview.size() >= 2)
        {
            shapeStart(preview);
            drawSymmetric(f, preview, kGhost);
            curve::PipeOptions o;
            o.radius = toScales(preview);
            o.sides = mSides;
            o.wall = mWall;
            if (mStart && mFromLoop)
                o.startLoop = mStart->points;
            curve::PipeResult r = curve::makePipe(preview, o);
            const geo::Mesh& m = r.mesh;
            for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
                if (m.edgeValid(e))
                {
                    f.lines.push_back({glm::dvec3(m.position(m.edgePoint(e, 0))), kPreview});
                    f.lines.push_back({glm::dvec3(m.position(m.edgePoint(e, 1))), kPreview});
                }
            if (!r.warnings.empty())
                drawToolMessage(p, view, QString::fromStdString("Pipe: " + r.warnings.front()));
        }
        for (size_t i = 0; i < mSpline.size(); ++i)
        {
            for (const D3* h : {&mSpline.handleL[i], &mSpline.handleR[i]})
                if (glm::distance(*h, mSpline.points[i]) > 1e-9)
                {
                    f.linesOnTop.push_back({glm::dvec3(mSpline.points[i]), kGhost});
                    f.linesOnTop.push_back({glm::dvec3(*h), kGhost});
                }
            dot(p, view, mSpline.points[i]);
        }
        if (mHover && view.camera == mHoverView)
        {
            dot(p, view, *mHover, 3.0f, kGhost);
            if (auto c = view.project(*mHover))
            {
                double px = mRadius / std::max(view.unitsPerPixel(*mHover), 1e-9);
                std::vector<QPointF> ring;
                for (int k = 0; k < 48; ++k)
                {
                    double a = glm::two_pi<double>() * k / 48.0;
                    ring.push_back(*c + QPointF(std::cos(a), std::sin(a)) * px);
                }
                p.polyline(ring, 1.5f, kPreview, true);
            }
        }
        const char* preset = kPipePresets[size_t(mPreset)];
        drawToolStatus(p, view,
                       {.tool = "Pipe",
                        .mode = *preset ? QString(preset) : QString("no preset"),
                        .fields = {{"radius", QString("%1 cm").arg(mRadius, 0, 'f', 2)},
                                   {"sides", QString::number(mSides)},
                                   {"wall", mWall > 0.0 ? QString("%1 cm").arg(mWall, 0, 'f', 2) : QString("solid")}},
                        .flags = {{"surfaces", mSurface}, {"from loop", mFromLoop}, {"attach", mAttach}},
                        .prompt = mStart ? QString("from the selected loop") : QString(),
                        .keys = {{"B-drag", "radius"}, {"+/-", "sides"}, {"P", "preset"}, {"T", "wall"}, {"S", "surfaces"}, {"F", "from loop"},
                                 {"A", "attach"}, {"Y", "symmetry"}, {"Z", "undo point"}, {"Shift+Z", "redo point"}, {"Enter", "build"},
                                 {"Esc", "exit"}},
                        .symmetry = curveSymmetry()});
    }

private:
    static constexpr const char* kPrefs = "/pipeTool/";
    static double prefD(const char* key, double def) { return prefs().value<double>("general", std::string(kPrefs) + key, def); }
    static bool prefB(const char* key, bool def) { return prefs().value<bool>("general", std::string(kPrefs) + key, def); }
    static void setPref(const char* key, const json& v) { prefs().set("general", std::string(kPrefs) + key, v); }

    /** Points go on mesh surfaces (lifted by the radius so the pipe rests on them) or where the curve tools put them. */
    std::optional<D3> placeAt(QPointF px, const ViewInfo& view, D3* normalOut)
    {
        mPlacer.onSurface(mSurface, mRadius);
        return mPlacer.place(px, view, normalOut);
    }

    /** From a start loop, the first handle points out along its normal so the pipe leaves square to the face. */
    void shapeStart(Spline& s) const
    {
        if (!mStart || s.size() < 2)
            return;
        double len = glm::distance(s.points[0], s.points[1]) / 3.0;
        s.handleR[0] = s.points[0] + mStart->normal * len;
        s.handleL[0] = s.points[0] - mStart->normal * len;
        s.typeL[0] = s.typeR[0] = curve::HandleType::Free;
    }

    /**
     * Turns the drawn radii (cm per point) into the pipe's radius and the path's per-point scale of it, so the built
     * node's radius scales the whole pipe. A uniform pipe keeps no per-point values.
     */
    static double toScales(Spline& s)
    {
        const double base = std::max(s.radius.front(), 1e-4);
        bool uniform = true;
        for (double& r : s.radius)
        {
            uniform = uniform && std::abs(r - base) < 1e-9;
            r /= base;
        }
        if (uniform)
            s.radius.clear();
        return base;
    }

    /** A start loop is the first ring, so with from-loop on the path starts at its size. */
    double startRadius() const { return mStart && mFromLoop ? std::max(mStart->radius, 1e-4) : mRadius; }

    /** A placed point and its radius: what undo takes off the path and redo puts back. */
    struct Step
    {
        D3 point, handleL, handleR;
        curve::HandleType typeL, typeR;
        double radius;
    };

    void stepBack()
    {
        if (mSpline.size() <= (mStart ? 1u : 0u))
            return;
        const size_t i = mSpline.size() - 1;
        mHistory.undone({mSpline.points[i], mSpline.handleL[i], mSpline.handleR[i], mSpline.typeL[i], mSpline.typeR[i], mSpline.radius[i]});
        mSpline.erasePoint(i);
        mDragging = 0;
        if (mSpline.size())
            curve::updateHandles(mSpline);
        else
            mPlacer.unlock();
    }

    void stepForward()
    {
        std::optional<Step> st = mHistory.take();
        if (!st)
            return;
        if (mSpline.size() == 0)
        {
            mSpline.kind = curve::Kind::Bezier;
            mPlacer.lockTo(st->point);
        }
        mSpline.points.push_back(st->point);
        mSpline.handleL.push_back(st->handleL);
        mSpline.handleR.push_back(st->handleR);
        mSpline.typeL.push_back(st->typeL);
        mSpline.typeR.push_back(st->typeR);
        mSpline.radius.push_back(st->radius);
        curve::updateHandles(mSpline);
    }

    void restart()
    {
        mSpline = {};
        mHistory.clear();
        mPlacer.unlock();
        mDragging = 0;
        if (!mStart)
            return;
        mSpline.kind = curve::Kind::Bezier;
        mSpline.points = {mStart->centre};
        mSpline.handleL = mSpline.handleR = {mStart->centre};
        mSpline.typeL = mSpline.typeR = {curve::HandleType::Free};
        mSpline.radius = {startRadius()};
        mPlacer.lockTo(mStart->centre);
    }

    void setRadius(double r)
    {
        mRadius = std::clamp(r, 0.01, 1e5);
        setPref("radius", mRadius);
        viewportTools::requestRedraw();
    }

    void setSides(int n)
    {
        mSides = std::clamp(n, 3, 128);
        setPref("sides", mSides);
        viewportTools::requestRedraw();
    }

    void finish()
    {
        json call = {{"radius", mRadius}, {"sides", mSides}, {"wallThickness", mWall}};
        if (*kPipePresets[size_t(mPreset)])
            call["preset"] = kPipePresets[size_t(mPreset)];
        if (mSpline.size() < 2)
        {
            if (mSpline.size() <= (mStart ? 1u : 0u))
                actions::runWithOptions("pipe", call);
            restart();
            viewportTools::requestRedraw();
            return;
        }
        Spline s = mSpline;
        shapeStart(s);
        call["radius"] = toScales(s);
        call["splines"] = json::array({curve::splineToJson(s)});
        call["symmetry"] = symmetryName(symmetryPref());
        if (mStart && mFromLoop)
        {
            call["fromLoop"] = true;
            call["attach"] = mAttach;
        }
        actions::runWithOptions("pipe", call);
        // The loop is used up (an attached pipe replaces it), so the next pipe starts free.
        mStart.reset();
        restart();
        viewportTools::requestRedraw();
    }

    Spline mSpline;
    StepHistory<Step> mHistory;
    size_t mDragging = 0;
    bool mRadiusDrag = false;
    QPointF mPressPx;
    double mStartRadius = 5.0;
    D3 mDragNormal{0, 1, 0};
    std::optional<D3> mHover;
    std::string mHoverView;
    Placer mPlacer;
    std::optional<StartLoop> mStart;
    double mRadius = 5.0, mWall = 0.0;
    int mSides = 12, mPreset = 0;
    bool mFromLoop = true, mAttach = false, mSurface = true;
};
} // namespace

void registerPipeTool()
{
    if (!viewportTools::registry().find("curve.pipeTool"))
        viewportTools::add({.id = "curve.pipeTool", .label = "Pipe Tool", .icon = "route-2", .create = [] { return std::make_unique<PipeTool>(); }});
    actions::add({.id = "curve.pipeTool",
                  .label = "Pipe Tool",
                  .category = "Curves",
                  .run = [] { viewportTools::activate("curve.pipeTool"); },
                  .doc = "Draws a pipe: click and drag a path (on surfaces too), set the radius with B-drag or Ctrl+scroll and the sides with "
                         "+/-, Enter builds it. Starts from a selected face or loop; with nothing drawn, Enter pipes the selected curves.",
                  .python = "lila.ui.run_action(\"curve.pipeTool\")"});
    std::string py = ops::toPython("pipe", json::object());
    actions::add({.id = "curve.pipe",
                  .label = "Pipe from Curves",
                  .category = "Curves",
                  .doc = "Turns the selected curves into pipes with live history: radius, wall, bends, couplings, bolts and corrugation in the "
                         "option box and the attribute editor.",
                  .python = py,
                  .command = "pipe",
                  .call = json::object()});
    for (const char* preset : {"pipe", "hose", "cable", "conduit"})
    {
        std::string label = std::string(1, char(std::toupper(preset[0]))) + (preset + 1);
        actions::add({.id = std::string("curve.pipePreset") + label,
                      .label = label + " Preset",
                      .category = "Curves",
                      .doc = "Applies the " + std::string(preset) + " preset to the selected pipes.",
                      .python = ops::toPython("pipePreset", {{"preset", preset}}),
                      .command = "pipePreset",
                      .call = {{"preset", preset}}});
    }
}
} // namespace lila::ui
