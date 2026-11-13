// Line of Action tool: draw a stroke over a character and the body path nearest it bends to the stroke's shape. The stroke
// stays as an editable action curve while the selection holds; dragging it re-poses the path. With a range Shift + dragged
// on the time slider the stroke poses every key in it, and Travel slides the body along the stroke over the range. Shift +
// drag from one joint to another picks that path for the next stroke; Ctrl + wheel turns the plane strokes are drawn on.

#include "Puppeteer.h"
#include "rig/Rig.h"

#include <lila/Anim.h>
#include <lila/UI.h>
#include <lila/ViewportTool.h>

#include <glm/gtc/matrix_transform.hpp>

#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace puppeteer
{
namespace
{
const glm::vec4 kStroke{0.92f, 0.78f, 0.22f, 1.0f};
const glm::vec4 kCurve{0.92f, 0.78f, 0.22f, 0.75f};
const glm::vec4 kGrab{1.0f, 1.0f, 1.0f, 1.0f};
constexpr double kGrabPx = 10.0;

double distanceToSegment(QPointF p, QPointF a, QPointF b)
{
    const QPointF d = b - a;
    const double l2 = d.x() * d.x() + d.y() * d.y();
    const double t = l2 > 1e-9 ? std::clamp(((p.x() - a.x()) * d.x() + (p.y() - a.y()) * d.y()) / l2, 0.0, 1.0) : 0.0;
    const QPointF q = a + t * d - p;
    return std::hypot(q.x(), q.y());
}

/** Where a pixel's ray meets the plane through origin facing normal. */
std::optional<glm::dvec3> onPlane(const ViewInfo& view, QPointF px, const glm::dvec3& origin, const glm::dvec3& normal)
{
    glm::dvec3 o, d;
    view.ray(px, o, d);
    const double den = glm::dot(d, normal);
    if (std::abs(den) < 1e-9)
        return std::nullopt;
    return o + d * (glm::dot(origin - o, normal) / den);
}

class LineOfActionTool final : public ViewportTool
{
public:
    const char* name() const override { return "Line of Action"; }
    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

    void activate() override
    {
        log::write(LogLevel::Info, "Line of Action: draw along the body or a limb; drag the curve to adjust, Shift+drag: pick a path, L: line, "
                                   "T: travel, Ctrl+wheel: turn the plane");
    }
    void deactivate() override { viewportTools::requestRedraw(); }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        dropCurveIfStale();
        mGrab = -1;
        mPicking = e->modifiers() & Qt::ShiftModifier;
        if (!mPicking && !mCurve.empty() && view.camera == mCamera)
            mGrab = nearestCurvePoint(e->position(), view);
        if (mGrab < 0)
        {
            mCurve.clear();
            mScreen = {e->position()};
        }
        mLast = e->position();
        mDragging = true;
        mCamera = view.camera;
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        if (!mDragging)
            return false;
        const QPointF at = e->position();
        if (mGrab >= 0)
            nudge(at, view);
        else if (!mScreen.empty() && std::hypot(at.x() - mScreen.back().x(), at.y() - mScreen.back().y()) >= 3.0)
            mScreen.push_back(at);
        mLast = at;
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton || !mDragging)
            return false;
        mDragging = false;
        if (mPicking)
            pickPath(e->position(), view);
        else if (mGrab >= 0)
            pose();
        else if (mScreen.size() >= 3)
            fromStroke(view);
        mPicking = false;
        mScreen.clear();
        viewportTools::requestRedraw();
        return true;
    }

    bool wheel(QWheelEvent* e, const ViewInfo&) override
    {
        if (!(e->modifiers() & Qt::ControlModifier) || e->angleDelta().y() == 0)
            return false;
        mPlaneTurn = std::fmod(mPlaneTurn + (e->angleDelta().y() > 0 ? 15.0 : -15.0) + 360.0, 360.0);
        viewportTools::requestRedraw();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier))
            return false;
        if (e->key() == Qt::Key_L)
        {
            cycleLine();
            return true;
        }
        if (e->key() == Qt::Key_T)
        {
            mTravel = !mTravel;
            viewportTools::requestRedraw();
            return true;
        }
        if (e->key() == Qt::Key_Escape)
        {
            viewportTools::set(nullptr);
            return true;
        }
        return false;
    }

    bool hotkey(const QKeyEvent& e, std::string_view) override
    {
        // L and T are app hotkeys too; the tool takes them while active.
        return !(e.modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier)) && (e.key() == Qt::Key_L || e.key() == Qt::Key_T) &&
               keyPress(const_cast<QKeyEvent*>(&e), ViewInfo{});
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        dropCurveIfStale();
        if (mDragging && mGrab < 0 && mScreen.size() >= 2)
            p.polyline(mScreen, 2.0f, mPicking ? kGrab : kStroke);
        if (!mCurve.empty())
        {
            std::vector<QPointF> pts;
            for (const glm::dvec3& w : mCurve)
                if (const auto s = view.project(w))
                    pts.push_back(*s);
            p.polyline(pts, 1.5f, kCurve);
            if (mGrab >= 0 && size_t(mGrab) < pts.size())
                p.disc(pts[size_t(mGrab)], 4.0f, kGrab);
        }
        const auto& sel = anim::timeSelection();
        ToolStatus s{.tool = "Line of Action",
                     .mode = QString::fromStdString(!mPath.empty() ? mPath[0] + " to " + mPath[1] : mLine.empty() ? std::string("auto") : mLine),
                     .fields = {{"plane", QString("%1").arg(mPlaneTurn, 0, 'f', 0) + QChar(0x00B0)}},
                     .flags = {{"travel", mTravel}},
                     .prompt = mCurve.empty() ? "draw along the body or a limb" : "drag the curve to adjust, or draw again",
                     .keys = {{"Shift+drag", "pick path"}, {"L", "line"}, {"T", "travel"}, {"Ctrl+wheel", "plane"}, {"Esc", "exit"}}};
        if (sel.active)
            s.fields.push_back({"range", QString("%1-%2").arg(anim::frameOf(sel.start), 0, 'f', 0).arg(anim::frameOf(sel.end), 0, 'f', 0)});
        drawToolStatus(p, view, s);
    }

private:
    /** The character the tool poses: the selection's, else the only one in the scene. */
    NodeId character() const
    {
        Graph& g = Graph::current();
        for (NodeId id : g.selection())
            if (const NodeId c = characterFor(g, id))
                return c;
        NodeId only = kNoNode;
        for (NodeId id : g.nodes())
            if (g.node(id)->type().name == kCharacterType)
            {
                if (only != kNoNode)
                    return kNoNode;
                only = id;
            }
        return only;
    }

    /** The action curve lasts while the selection it was drawn with holds. */
    void dropCurveIfStale()
    {
        if (!mCurve.empty() && Graph::current().selection() != mSelection)
        {
            mCurve.clear();
            viewportTools::requestRedraw();
        }
    }

    int nearestCurvePoint(QPointF px, const ViewInfo& view) const
    {
        int best = -1;
        double bestD = kGrabPx;
        for (size_t i = 0; i < mCurve.size(); ++i)
            if (const auto s = view.project(mCurve[i]))
                if (const double d = std::hypot(s->x() - px.x(), s->y() - px.y()); d < bestD)
                {
                    bestD = d;
                    best = int(i);
                }
        return best;
    }

    /** Moves the grabbed part of the curve with the mouse, easing off over a quarter of its points either side. */
    void nudge(QPointF to, const ViewInfo& view)
    {
        const glm::dvec3 centre = mCurve[size_t(mGrab)], normal = view.viewDir(centre);
        const auto a = onPlane(view, mLast, centre, normal), b = onPlane(view, to, centre, normal);
        if (!a || !b)
            return;
        const glm::dvec3 delta = *b - *a;
        const double radius = std::max(2.0, double(mCurve.size()) / 4.0);
        for (size_t i = 0; i < mCurve.size(); ++i)
        {
            const double t = std::abs(double(i) - double(mGrab)) / radius;
            if (t < 1.0)
                mCurve[i] += delta * (1.0 - t * t * (3.0 - 2.0 * t));
        }
    }

    /** The character's lines, as {name: [top, end]} from its rules. */
    json lines(NodeId c) const
    {
        const json r = runCommand("stringsRules", {{"args", {Graph::current().node(c)->path()}}});
        return r.value("lines", json::object());
    }

    void cycleLine()
    {
        const NodeId c = character();
        std::vector<std::string> names{""};
        const json all = c != kNoNode ? lines(c) : json::object();
        for (const auto& [n, ends] : all.items())
            names.push_back(n);
        const auto it = std::find(names.begin(), names.end(), mLine);
        mLine = it == names.end() || it + 1 == names.end() ? names.front() : *(it + 1);
        mPath.clear();
        viewportTools::requestRedraw();
    }

    /** The character's joint drawn nearest px, within 30 px; empty when none is. */
    std::string jointAt(NodeId character, QPointF px, const ViewInfo& view) const
    {
        Graph& g = Graph::current();
        std::string best;
        double bestD = 30.0;
        for (NodeId j : fullBodyJoints(g, characterRoot(g, character)))
            if (const auto s = view.project(glm::dvec3(worldOf(g, j)[3])))
                if (const double d = std::hypot(s->x() - px.x(), s->y() - px.y()); d < bestD)
                {
                    bestD = d;
                    best = g.node(j)->name();
                }
        return best;
    }

    /** A Shift + drag from one joint to another along the body: that path takes the next strokes, top joint first. */
    void pickPath(QPointF end, const ViewInfo& view)
    {
        Graph& g = Graph::current();
        const NodeId c = character();
        if (c == kNoNode || mScreen.empty())
            return;
        std::string a = jointAt(c, mScreen.front(), view), b = jointAt(c, end, view);
        Node* na = a.empty() ? nullptr : g.find(a);
        Node* nb = b.empty() ? nullptr : g.find(b);
        if (!na || !nb || na == nb)
            return log::write(LogLevel::Warning, "Line of Action: Shift+drag from one joint to another along the body to pick a path");
        if (isUnder(g, na->id(), nb->id()))
            std::swap(a, b);
        else if (!isUnder(g, nb->id(), na->id()))
            return log::write(LogLevel::Warning, "Line of Action: " + a + " and " + b + " aren't on one path down the body; pick a joint and one below it");
        mPath = {a, b};
        mLine.clear();
        mCurve.clear();
    }

    /** The plane strokes are drawn on: facing the view, turned about the world's up by the plane setting. */
    glm::dvec3 planeNormal(const ViewInfo& view, const glm::dvec3& origin) const
    {
        const glm::dvec3 n = view.viewDir(origin);
        return glm::dvec3(glm::rotate(glm::dmat4(1.0), glm::radians(mPlaneTurn), glm::dvec3(0.0, 1.0, 0.0)) * glm::dvec4(n, 0.0));
    }

    /** The stroke onto the action plane through the chosen line's top joint, then the pose. */
    void fromStroke(const ViewInfo& view)
    {
        Graph& g = Graph::current();
        const NodeId c = character();
        if (c == kNoNode)
            return log::write(LogLevel::Warning, "Line of Action: select a character (or a joint in one) first");
        std::string line = mLine;
        glm::dvec3 origin(0.0);
        double best = 1e300;
        const json all = mPath.empty() ? lines(c) : json{{"", mPath}};
        for (const auto& [n, ends] : all.items())
        {
            Node* top = g.find(ends[0].get<std::string>());
            Node* end = g.find(ends[1].get<std::string>());
            if (!top || !end)
                continue;
            const auto a = view.project(glm::dvec3(worldOf(g, top->id())[3])), b = view.project(glm::dvec3(worldOf(g, end->id())[3]));
            if (!a || !b)
                continue;
            double sum = 0.0;
            for (const QPointF& s : mScreen)
                sum += distanceToSegment(s, *a, *b);
            if ((mLine.empty() && sum < best) || n == mLine || !mPath.empty())
            {
                best = mLine.empty() ? sum : -1.0;
                line = n;
                origin = glm::dvec3(worldOf(g, top->id())[3]);
            }
        }
        if (line.empty() && mPath.empty())
            return log::write(LogLevel::Warning, "Line of Action: the character has no lines of action in view");
        const glm::dvec3 normal = planeNormal(view, origin);
        mCurve.clear();
        for (const QPointF& s : mScreen)
            if (const auto w = onPlane(view, s, origin, normal))
                mCurve.push_back(*w);
        mCharacter = c;
        mDrawnLine = line;
        mDrawnPath = mPath;
        pose();
        mSelection = g.selection();
    }

    void pose()
    {
        Graph& g = Graph::current();
        if (mCurve.size() < 2 || !g.node(mCharacter))
            return;
        json stroke = json::array();
        for (const glm::dvec3& p : mCurve)
            stroke.push_back({p.x, p.y, p.z});
        json call = {{"args", {g.node(mCharacter)->path()}}, {"stroke", stroke}, {"travel", mTravel}};
        if (!mDrawnPath.empty())
            call["joints"] = mDrawnPath;
        else
            call["line"] = mDrawnLine;
        if (const auto& sel = anim::timeSelection(); sel.active)
        {
            call["range"] = {sel.start, sel.end};
            call["falloff"] = sel.before();
            if (const std::string layer = stringsLayer(); !layer.empty())
                call["layer"] = layer;
        }
        runCommand("lineOfAction", call);
    }

    std::vector<QPointF> mScreen;
    std::vector<glm::dvec3> mCurve;
    std::vector<NodeId> mSelection;
    std::string mCamera, mLine, mDrawnLine;
    std::vector<std::string> mPath, mDrawnPath; // a picked [top, end] in place of a named line
    double mPlaneTurn = 0.0;                     // degrees the action plane turns about the world's up
    bool mPicking = false;
    NodeId mCharacter = kNoNode;
    QPointF mLast;
    int mGrab = -1;
    bool mDragging = false;
    bool mTravel = false;
};
} // namespace

void registerLineOfActionTool(ext::Context& ctx)
{
    viewportTools::add(ctx, {.id = "puppeteer.lineOfAction", .label = "Line of Action", .icon = "scribble",
                             .create = [] { return std::make_unique<LineOfActionTool>(); }});
    actions::add(ctx, {.id = "puppeteer.strings.lineOfAction",
                       .label = "Line of Action",
                       .category = "Puppeteer Strings",
                       .run = [] { viewportTools::activate("puppeteer.lineOfAction"); },
                       .doc = "Draw a stroke along the body or a limb and it bends to the stroke's shape; drag the curve after to "
                              "adjust. Over a time slider range it poses every key there.",
                       .icon = "scribble",
                       .keywords = {"sketch", "draw pose", "stroke", "curve", "posing", "gesture"}});
}
} // namespace puppeteer
