#include "ui/CurveUi.h"
#include "ui/CurveToolsCommon.h"

#include "core/Log.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/LastOp.h"
#include "ui/Actions.h"
#include "ui/SurfaceSnap.h"
#include "ui/ViewportTool.h"

#include <QTimer>

#include <algorithm>
#include <map>

namespace lila::ui
{
namespace
{
using namespace curveTools;
namespace lo = scene::lastOp;

constexpr double kEzDragPx = 4.0;

struct EzEnd
{
    D3 point{0.0}, normal{0.0, 1.0, 0.0};
    scene::NodeId on = scene::kNoNode;
};

json ezVec(const D3& v) { return json::array({v.x, v.y, v.z}); }

/**
 * EZ Curve: click two points for a curve between them that hugs the meshes it lands on and stays out of the ones it
 * crosses. Until the next curve, dragging reshapes it: up and down bends, sideways twists, Shift sags, Ctrl lifts.
 */
class EzCurveTool final : public ViewportTool
{
public:
    const char* name() const override { return "EZ Curve"; }
    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

    void activate() override
    {
        mSurface.load();
        mPlacer.onSurface(true, mSurface.offset());
        mPlacer.toggleLock(false);
        prefs().set("general", "/curveTools/symmetry", 0);
        log::info("EZ Curve: click two points; then drag to bend, twist (sideways), sag (Shift) or lift (Ctrl)");
    }

    void deactivate() override
    {
        if (mDragging)
            lo::endDrag();
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        if (!mStart && adjusting())
        {
            mPress = e->position();
            mPressMods = e->modifiers();
            mPressed = true;
            return true;
        }
        place(e->position(), view);
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        if (mPressed)
        {
            if (!mDragging && distPx(mPress, e->position()) >= kEzDragPx)
                beginDrag();
            if (mDragging)
                drag(e->position());
            return true;
        }
        mHover = mPlacer.place(e->position(), view);
        mHoverView = view.camera;
        viewportTools::requestRedraw();
        return false;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton || !mPressed)
            return false;
        mPressed = false;
        if (mDragging)
        {
            mDragging = false;
            lo::endDrag();
            if (const lo::Op* op = lo::current())
                log::write(LogLevel::Echo, ops::toPython(op->command, op->call) + "\n");
        }
        else
            place(mPress, view);
        viewportTools::requestRedraw();
        return true;
    }

    bool wheel(QWheelEvent* e, const ViewInfo&) override
    {
        if (!adjusting() || !(e->modifiers() & Qt::ShiftModifier))
            return false;
        const int steps = e->angleDelta().y() > 0 ? 1 : -1;
        adjust({{"points", std::clamp(value("points").get<int>() + steps, 3, 128)}});
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (toolKey(e->key()))
            return true;
        switch (e->key())
        {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            mMade = false;
            viewportTools::requestRedraw();
            return true;
        case Qt::Key_Escape:
            if (!mStart && !adjusting())
                exitTool();
            mStart.reset();
            mMade = false;
            viewportTools::requestRedraw();
            return true;
        default: return false;
        }
    }

    bool hotkey(const QKeyEvent& e, std::string_view) override { return plainKey(e) && toolKey(e.key()); }

    void draw(render::FrameState& f, OverlayPainter& p, const ViewInfo& view) override
    {
        drawSymmetryPlane(f, view);
        if (mStart)
        {
            dot(p, view, mStart->point);
            if (mHover && view.camera == mHoverView)
            {
                f.linesOnTop.push_back({glm::dvec3(mStart->point), kGhost});
                f.linesOnTop.push_back({glm::dvec3(*mHover), kGhost});
            }
        }
        if (mHover && view.camera == mHoverView)
            dot(p, view, *mHover, 3.0f, kGhost);
        if (mDragging)
            p.label(mLastPx + QPointF(18, -14), dragLabel());
        if (mStart)
            drawToolStatus(p, view, {.tool = "EZ Curve", .prompt = "click the end point", .keys = {{"Esc", "cancel"}}});
        else if (adjusting())
            drawToolStatus(p, view,
                           {.tool = "EZ Curve",
                            .fields = {{"points", QString::number(value("points").get<int>())}},
                            .flags = {{"follow surface", value("follow").get<bool>()}, {"keep out of meshes", value("clearance").get<bool>()}},
                            .keys = {{"Drag up/down", "bend"}, {"Drag sideways", "twist"}, {"Shift", "sag"}, {"Ctrl", "lift"}, {"Shift+scroll", "points"},
                                     {"F", "follow surface"}, {"O", "keep out of meshes"}, {"Y", "symmetry"}, {"R", "reset"}, {"Enter", "done"},
                                     {"Click", "next curve"}},
                            .symmetry = curveSymmetry()});
        else
            drawToolStatus(p, view,
                           {.tool = "EZ Curve",
                            .prompt = "click the start point",
                            .keys = {{"Y", "symmetry"}, {"Esc", "exit"}},
                            .symmetry = curveSymmetry()});
    }

private:
    /** True while the curve this tool made is the last op and can still be reshaped. */
    bool adjusting() const
    {
        const lo::Op* op = lo::current();
        return mMade && op && op->command == "ezCurve" && lo::reachable();
    }

    json value(const char* flag) const
    {
        const lo::Op* op = lo::current();
        if (op)
            for (const FlagDef* f : lo::editableFlags("ezCurve"))
                if (f->name == flag)
                    return lo::flagValue(*op, *f);
        return 0;
    }

    void place(QPointF px, const ViewInfo& view)
    {
        EzEnd end;
        auto p = mPlacer.place(px, view, &end.normal, &end.on);
        if (!p)
            return;
        end.point = *p;
        if (!mStart)
        {
            mStart = end;
            mMade = false;
            viewportTools::requestRedraw();
            return;
        }
        create(*std::exchange(mStart, std::nullopt), end);
    }

    /** Adapts to the meshes the ends sit on, and to any the straight line between them passes through. */
    void create(const EzEnd& a, const EzEnd& b)
    {
        if (glm::distance(a.point, b.point) < 1e-6)
            return;
        scene::Graph& g = scene::Graph::current();
        std::vector<scene::NodeId> meshes;
        auto add = [&](scene::NodeId id) {
            if (id != scene::kNoNode && std::ranges::find(meshes, id) == meshes.end())
                meshes.push_back(id);
        };
        add(a.on);
        add(b.on);
        // Rays both ways, inset from the ends so the meshes they sit on don't hit at once.
        const D3 chord = b.point - a.point;
        for (auto [from, dir] : {std::pair{a.point + chord * 0.01, chord}, std::pair{b.point - chord * 0.01, -chord}})
            if (auto hit = surfaceSnap::raycast(from, dir); hit && glm::dot(hit->position - from, dir) < glm::dot(dir, dir) * 0.98)
                add(hit->node);
        json names = json::array();
        for (scene::NodeId id : meshes)
            if (const scene::Node* n = g.node(id))
                names.push_back(n->path());
        json call = {{"start", ezVec(a.point)}, {"end", ezVec(b.point)}, {"follow", a.on != scene::kNoNode && b.on != scene::kNoNode},
                     {"lift", mSurface.offset()}, {"symmetry", symmetryName(symmetryPref())}};
        if (a.on != scene::kNoNode)
            call["startNormal"] = ezVec(a.normal);
        if (b.on != scene::kNoNode)
            call["endNormal"] = ezVec(b.normal);
        if (!names.empty())
            call["mesh"] = names;
        try
        {
            ops::execute("ezCurve", call, CallSource::UI);
            mMade = true;
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
        viewportTools::requestRedraw();
    }

    void beginDrag()
    {
        mDragging = true;
        lo::endDrag();
        for (const char* f : {"bend", "twist", "sag", "lift"})
            mFrom[f] = value(f).get<double>();
    }

    void drag(QPointF px)
    {
        mLastPx = px;
        const double dx = px.x() - mPress.x(), dy = mPress.y() - px.y();
        json flags;
        if (mPressMods & Qt::ControlModifier)
            flags["lift"] = std::max(0.0, mFrom["lift"] + dy * 0.05);
        else if (mPressMods & Qt::ShiftModifier)
            flags["sag"] = mFrom["sag"] + dy * 0.004;
        else
        {
            flags["bend"] = mFrom["bend"] + dy * 0.004;
            flags["twist"] = mFrom["twist"] + dx * 0.5;
        }
        try
        {
            lo::rerun(flags, CallSource::Script, true);
        }
        catch (const CommandError& e)
        {
            log::warn("ezCurve: {}", e.what());
        }
        viewportTools::requestRedraw();
    }

    QString dragLabel() const
    {
        if (mPressMods & Qt::ControlModifier)
            return QString("Lift %1 cm").arg(value("lift").get<double>(), 0, 'f', 2);
        if (mPressMods & Qt::ShiftModifier)
            return QString("Sag %1").arg(value("sag").get<double>(), 0, 'f', 3);
        return QString("Bend %1  Twist %2°").arg(value("bend").get<double>(), 0, 'f', 3).arg(value("twist").get<double>(), 0, 'f', 1);
    }

    /** One undo step of its own, shown in the adjust popup like any other change. */
    void adjust(const json& flags)
    {
        try
        {
            lo::rerun(flags, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::warn("ezCurve: {}", e.what());
        }
        viewportTools::requestRedraw();
    }

    /** F, O and R, which the app also binds, taken ahead of its hotkeys while there is a curve to reshape. */
    bool toolKey(int key)
    {
        if (key == Qt::Key_Y && !mDragging)
        {
            prefs().set("general", "/curveTools/symmetry", (symmetryPref() + 1) % 4);
            if (adjusting())
                adjust({{"symmetry", symmetryName(symmetryPref())}});
            viewportTools::requestRedraw();
            return true;
        }
        if (!adjusting() || mDragging)
            return false;
        switch (key)
        {
        case Qt::Key_F: adjust({{"follow", !value("follow").get<bool>()}}); return true;
        case Qt::Key_O: adjust({{"clearance", !value("clearance").get<bool>()}}); return true;
        case Qt::Key_R: adjust({{"bend", 0.0}, {"twist", 0.0}, {"sag", 0.0}}); return true;
        default: return false;
        }
    }

    std::optional<EzEnd> mStart;
    std::optional<D3> mHover;
    std::string mHoverView;
    bool mMade = false;
    bool mPressed = false;
    bool mDragging = false;
    QPointF mPress, mLastPx;
    Qt::KeyboardModifiers mPressMods;
    std::map<std::string, double> mFrom;
    Placer mPlacer;
    SurfaceDraw mSurface;
};
} // namespace

void registerEzCurveTool()
{
    if (!viewportTools::registry().find("curve.ezCurve"))
        viewportTools::add({.id = "curve.ezCurve", .label = "EZ Curve", .icon = "line", .create = [] { return std::make_unique<EzCurveTool>(); }});
    actions::add({.id = "curve.ezCurve", .label = "EZ Curve", .category = "Curves", .run = [] { viewportTools::activate("curve.ezCurve"); },
                  .doc = "Click two points for a curve between them that follows the surface they sit on and stays out of meshes it crosses. "
                         "Then drag to bend it (up and down), twist it (sideways), sag it (Shift) or lift it off the surface (Ctrl).",
                  .python = "lila.ui.run_action(\"curve.ezCurve\")"});
}
} // namespace lila::ui
