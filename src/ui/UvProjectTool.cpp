#include "ui/UvProjectTool.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "scene/Graph.h"
#include "scene/LastOp.h"
#include "ui/Actions.h"
#include "ui/CameraBookmarks.h"
#include "ui/GizmoMath.h"
#include "ui/ToolColors.h"
#include "ui/Tools.h"
#include "ui/UiCommon.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportWidget.h"
#include "ui/ViewportToolMath.h"
#include "uv/UvCommands.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QTimer>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <optional>

namespace lila::ui
{
namespace
{
namespace lo = scene::lastOp;
using uv::Projection;

constexpr const char* kProjectCommand = "uvProject";
constexpr const char* kToolName = "uvProject";
const glm::vec4 kDim(0.55f, 0.55f, 0.55f, 0.8f);
constexpr double kPick = kHandleGrabPx;

std::optional<glm::dvec3> gViewDir;
std::optional<glm::dvec3> gViewEye;

bool sessionLive()
{
    const lo::Op* op = lo::current();
    return op && op->command == kProjectCommand && lo::reachable() && !uv::lastProjections().empty() &&
           uv::lastProjections().front().type != Projection::Camera;
}

double pixelDistance(QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

/** The first projected mesh's frame, placed in the world. */
struct ProjectionShape
{
    uv::ProjectionRecord rec;
    glm::dmat3 w3{1.0};
    glm::dmat4 inv{1.0};

    explicit ProjectionShape(const uv::ProjectionRecord& r) : rec(r), w3(r.world), inv(glm::inverse(r.world)) {}
    const uv::ProjectFrame& fr() const { return rec.frame; }
    /** A point in frame coordinates (object units) to world. */
    glm::dvec3 world(const glm::dvec3& l) const { return glm::dvec3(rec.world * glm::dvec4(fr().center + fr().axes * l, 1.0)); }
    /** A frame direction to world, unnormalized: its length is world units per object unit. */
    glm::dvec3 dir(const glm::dvec3& l) const { return w3 * (fr().axes * l); }
    glm::dvec3 center() const { return world(glm::dvec3(0.0)); }
    /** A world point to frame coordinates. */
    glm::dvec3 local(const glm::dvec3& p) const { return glm::transpose(fr().axes) * (glm::dvec3(inv * glm::dvec4(p, 1.0)) - fr().center); }
    double seam() const { return fr().seamAngle; }
    glm::dvec3 around(double angle, double r, double z) const { return {r * std::cos(angle), r * std::sin(angle), z}; }
};

enum class HandleKind
{
    None,
    Center,
    Ring,   // rotate about axis
    Size,   // width, height or depth along axis, from sign's side
    Radius,
    Seam,
};

struct Handle
{
    HandleKind kind = HandleKind::None;
    int axis = 0;
    int sign = 1;
    bool operator==(const Handle&) const = default;
};

struct Marker
{
    Handle h;
    glm::dvec3 local;
};

/** The square and disc handles of a shape, in frame coordinates. */
std::vector<Marker> markers(const ProjectionShape& s)
{
    const uv::ProjectFrame& f = s.fr();
    std::vector<Marker> out;
    switch (s.rec.type)
    {
    case Projection::Planar:
        for (int sg : {1, -1})
        {
            out.push_back({{HandleKind::Size, 0, sg}, {sg * f.width * 0.5, 0, 0}});
            out.push_back({{HandleKind::Size, 1, sg}, {0, sg * f.height * 0.5, 0}});
        }
        break;
    case Projection::Box:
        for (int sg : {1, -1})
        {
            out.push_back({{HandleKind::Size, 0, sg}, {sg * f.width * 0.5, 0, 0}});
            out.push_back({{HandleKind::Size, 1, sg}, {0, sg * f.height * 0.5, 0}});
            out.push_back({{HandleKind::Size, 2, sg}, {0, 0, sg * f.depth * 0.5}});
        }
        break;
    case Projection::Cylindrical:
        for (int sg : {1, -1})
            out.push_back({{HandleKind::Size, 2, sg}, {0, 0, sg * f.height * 0.5}});
        out.push_back({{HandleKind::Radius}, s.around(s.seam() + glm::half_pi<double>(), f.radius, 0.0)});
        out.push_back({{HandleKind::Seam}, s.around(s.seam(), f.radius, f.height * 0.5)});
        break;
    case Projection::Spherical:
        out.push_back({{HandleKind::Radius}, s.around(s.seam() + glm::half_pi<double>(), f.radius, 0.0)});
        out.push_back({{HandleKind::Seam}, s.around(s.seam(), f.radius, 0.0)});
        break;
    default: break;
    }
    return out;
}

const char* sizeFlag(Projection t, int axis)
{
    if (t == Projection::Cylindrical)
        return "height";
    return axis == 0 ? "width" : axis == 1 ? "height" : "depth";
}

/** Draws a world polyline, broken where it goes behind the camera. */
void wire(OverlayPainter& p, const ViewInfo& view, const std::vector<glm::dvec3>& pts, float width, const glm::vec4& color, bool closed = false)
{
    std::vector<QPointF> run;
    auto flush = [&] {
        if (run.size() > 1)
            p.polyline(run, width, color);
        run.clear();
    };
    for (size_t i = 0; i < pts.size() + (closed ? 1 : 0); ++i)
    {
        if (auto q = view.project(pts[i % pts.size()]))
            run.push_back(*q);
        else
            flush();
    }
    flush();
}

std::vector<glm::dvec3> projectionCircle(const ProjectionShape& s, double r, double z, int n = 64)
{
    std::vector<glm::dvec3> out;
    for (int i = 0; i < n; ++i)
        out.push_back(s.world(s.around(glm::two_pi<double>() * i / n, r, z)));
    return out;
}

/** A world ring around c in the plane perpendicular to n. */
std::vector<glm::dvec3> ringAround(const glm::dvec3& c, const glm::dvec3& n, double r, int count = 64)
{
    glm::dvec3 a = glm::normalize(glm::cross(n, std::abs(n.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0)));
    glm::dvec3 b = glm::cross(n, a);
    std::vector<glm::dvec3> out;
    for (int i = 0; i < count; ++i)
    {
        double t = glm::two_pi<double>() * i / count;
        out.push_back(c + (a * std::cos(t) + b * std::sin(t)) * r);
    }
    return out;
}

class UvProjectTool final : public ViewportTool
{
public:
    const char* name() const override { return kToolName; }

    void deactivate() override
    {
        if (mDrag)
            finishDrag();
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        std::optional<ProjectionShape> s = shape();
        Handle h = s ? pick(*s, e->position(), view) : Handle{};
        if (h.kind == HandleKind::None)
        {
            // A click away from the handles ends the session and goes on to select as usual; nothing touches this after.
            viewportTools::set(nullptr);
            return false;
        }
        beginDrag(*s, h, e, view);
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        if (!mDrag)
        {
            std::optional<ProjectionShape> s = shape();
            Handle h = s ? pick(*s, e->position(), view) : Handle{};
            if (!(h == mHover))
            {
                mHover = h;
                viewportTools::requestRedraw();
            }
            return false;
        }
        dragTo(e->position(), e->modifiers(), view);
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (e->button() != Qt::LeftButton || !mDrag)
            return false;
        finishDrag();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        switch (e->key())
        {
        case Qt::Key_Escape:
            if (mDrag)
            {
                cancelDrag();
                return true;
            }
            [[fallthrough]];
        case Qt::Key_Return:
        case Qt::Key_Enter: QTimer::singleShot(0, [] { endSession(); }); return true;
        default: return false;
        }
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();

        std::optional<ProjectionShape> s = shape();
        if (!s)
        {
            if (!mDrag)
                QTimer::singleShot(0, [] { endSession(); });
            return;
        }
        Handle hot = mDrag ? mDrag->h : mHover;
        auto colorOf = [&](const Handle& h) { return h == hot ? kHotColor : kHandleGray; };
        drawShape(*s, p, view, hot);

        glm::dvec3 c = s->center();
        double upp = view.unitsPerPixel(c);
        for (int i = 0; i < 3; ++i)
        {
            Handle h{HandleKind::Ring, i};
            glm::vec4 col(viewportDisplay::axisColor(i), h == hot ? 1.0f : 0.55f);
            wire(p, view, ringAround(c, glm::normalize(s->dir(axisVector(i))), ringPixels() * upp), h == hot ? 3.0f : 1.5f, col, true);
        }
        if (auto cp = view.project(c))
            square(p, *cp, colorOf({HandleKind::Center}));
        for (const Marker& m : markers(*s))
            if (auto q = view.project(s->world(m.local)))
            {
                if (m.h.kind == HandleKind::Seam)
                    p.disc(*q, m.h == hot ? 7.0f : 5.5f, colorOf(m.h));
                else
                    square(p, *q, colorOf(m.h));
            }

        if (mDrag)
            p.label(mDrag->lastPx + QPointF(18, -14), mDrag->readout);
        else if (hot.kind != HandleKind::None)
            if (auto at = handleScreen(*s, hot, view))
                p.text(*at + QPointF(14, 4), hint(s->rec.type, hot), kHandleGray);
        drawToolStatus(p, view, status(*s));
    }

private:
    struct Drag
    {
        Handle h;
        ProjectionShape start;
        json base;
        QPointF startPx, lastPx;
        glm::dvec3 startHit{0.0};
        double startParam = 0.0;
        double angle = 0.0; // accumulated ring turn, radians
        double lastAngle = 0.0;
        json last;
        QString readout;
    };

    static glm::dvec3 axisVector(int i)
    {
        glm::dvec3 v(0.0);
        v[i] = 1.0;
        return v;
    }

    static double ringPixels() { return handlePixels() * 0.75; }

    static void endSession()
    {
        if (ViewportTool* t = viewportTools::active(); t && std::string_view(t->name()) == kToolName)
            viewportTools::set(nullptr);
    }

    static std::optional<ProjectionShape> shape()
    {
        if (!sessionLive() || !scene::Graph::current().node(uv::lastProjections().front().node))
            return std::nullopt;
        return ProjectionShape(uv::lastProjections().front());
    }

    static void square(OverlayPainter& p, QPointF at, const glm::vec4& c)
    {
        QRectF box(at.x() - 5, at.y() - 5, 10, 10);
        std::vector<QPointF> pts = {box.topLeft(), box.topRight(), box.bottomRight(), box.bottomLeft()};
        p.fill(pts, {c.r, c.g, c.b, c == kHotColor ? 0.6f : 0.25f});
        p.polyline(pts, c == kHotColor ? 2.0f : 1.5f, c, true);
    }

    static QString hint(Projection t, const Handle& h)
    {
        switch (h.kind)
        {
        case HandleKind::Center: return "move";
        case HandleKind::Ring: return "rotate";
        case HandleKind::Size: return sizeFlag(t, h.axis);
        case HandleKind::Radius: return "radius";
        case HandleKind::Seam: return "seam";
        default: return {};
        }
    }

    ToolStatus status(const ProjectionShape& s) const
    {
        const uv::ProjectFrame& f = s.fr();
        const lo::Op* op = lo::current();
        auto flag = [&](const char* name, double fallback) {
            return op && op->call.contains(name) && op->call[name].is_number() ? op->call[name].get<double>() : fallback;
        };
        auto num = [](double v, int decimals = 2) { return QString::number(v, 'f', decimals); };
        auto seam = [&] { return ToolField{"seam", QString("%1°").arg(glm::degrees(f.seamAngle), 0, 'f', 1)}; };
        ToolStatus st{.tool = "UV Projection"};
        switch (s.rec.type)
        {
        case Projection::Planar:
            st.mode = "planar";
            st.fields = {{"size", QString("%1 x %2").arg(num(f.width), num(f.height))}};
            break;
        case Projection::Box:
            st.mode = "box";
            st.fields = {{"size", QString("%1 x %2 x %3").arg(num(f.width), num(f.height), num(f.depth))}};
            break;
        case Projection::Cylindrical:
            st.mode = "cylindrical";
            st.fields = {{"radius", num(f.radius)}, {"height", num(f.height)}, seam()};
            break;
        case Projection::Spherical:
            st.mode = "spherical";
            st.fields = {{"radius", num(f.radius)}, seam()};
            break;
        default: break;
        }
        auto scaleOffset = [&](const char* scale, const char* offset) {
            const double o = flag(offset, 0.0);
            return QString("x%1 %2%3").arg(num(flag(scale, 1.0)), o < 0 ? "" : "+", num(o));
        };
        st.fields.push_back({"U", scaleOffset("scaleU", "offsetU")});
        st.fields.push_back({"V", scaleOffset("scaleV", "offsetV")});
        st.prompt = "drag the handles";
        st.keys = {{"Shift", "scale U/V"}, {"Ctrl+centre", "offset U/V"}, {"Enter", "finish"}};
        return st;
    }

    void drawShape(const ProjectionShape& s, OverlayPainter& p, const ViewInfo& view, const Handle& hot) const
    {
        const uv::ProjectFrame& f = s.fr();
        auto edge = [&](const glm::dvec3& a, const glm::dvec3& b, const glm::vec4& c, float w = 1.5f) { wire(p, view, {s.world(a), s.world(b)}, w, c); };
        switch (s.rec.type)
        {
        case Projection::Planar:
        {
            double x = f.width * 0.5, y = f.height * 0.5;
            wire(p, view, {s.world({-x, -y, 0}), s.world({x, -y, 0}), s.world({x, y, 0}), s.world({-x, y, 0})}, 1.5f, kHandleGray, true);
            // The uv origin corner, and the normal the projection looks along.
            edge({-x, -y, 0}, {-x + f.width * 0.15, -y, 0}, kHotColor, 2.5f);
            edge({-x, -y, 0}, {-x, -y + f.height * 0.15, 0}, kHotColor, 2.5f);
            edge({0, 0, 0}, {0, 0, std::max(f.width, f.height) * 0.2}, kDim);
            break;
        }
        case Projection::Box:
        {
            glm::dvec3 h(f.width * 0.5, f.height * 0.5, f.depth * 0.5);
            for (int i = 0; i < 3; ++i)
                for (int a : {-1, 1})
                    for (int b : {-1, 1})
                    {
                        glm::dvec3 p0(0.0), p1(0.0);
                        int j = (i + 1) % 3, k = (i + 2) % 3;
                        p0[i] = -h[i], p1[i] = h[i];
                        p0[j] = p1[j] = a * h[j];
                        p0[k] = p1[k] = b * h[k];
                        edge(p0, p1, kHandleGray);
                    }
            break;
        }
        case Projection::Cylindrical:
        {
            double z = f.height * 0.5;
            wire(p, view, projectionCircle(s, f.radius, z), 1.5f, kHandleGray, true);
            wire(p, view, projectionCircle(s, f.radius, -z), 1.5f, kHandleGray, true);
            for (int i = 1; i < 8; ++i)
            {
                double a = s.seam() + glm::two_pi<double>() * i / 8.0;
                edge(s.around(a, f.radius, -z), s.around(a, f.radius, z), kDim, 1.0f);
            }
            edge(s.around(s.seam(), f.radius, -z), s.around(s.seam(), f.radius, z), hot.kind == HandleKind::Seam ? kHotColor : kHandleGray, 2.5f);
            edge({0, 0, -z}, {0, 0, z}, kDim, 1.0f);
            break;
        }
        case Projection::Spherical:
        {
            double r = f.radius;
            wire(p, view, projectionCircle(s, r, 0.0), 1.5f, kHandleGray, true);
            for (double a : {s.seam(), s.seam() + glm::half_pi<double>()})
            {
                std::vector<glm::dvec3> m;
                for (int i = 0; i <= 32; ++i)
                {
                    double t = -glm::half_pi<double>() + glm::pi<double>() * i / 32.0;
                    m.push_back(s.world(s.around(a, r * std::cos(t), r * std::sin(t))));
                }
                bool seam = a == s.seam();
                wire(p, view, m, seam ? 2.5f : 1.0f, seam ? (hot.kind == HandleKind::Seam ? kHotColor : kHandleGray) : kDim);
                if (!seam)
                {
                    for (glm::dvec3& q : m)
                        q = s.world(glm::dvec3(-1, -1, 1) * s.local(q));
                    wire(p, view, m, 1.0f, kDim);
                }
            }
            edge({0, 0, r}, {0, 0, r * 1.25}, kHandleGray, 2.0f);
            break;
        }
        default: break;
        }
    }

    std::optional<QPointF> handleScreen(const ProjectionShape& s, const Handle& h, const ViewInfo& view) const
    {
        if (h.kind == HandleKind::Center || h.kind == HandleKind::Ring)
            return view.project(s.center());
        for (const Marker& m : markers(s))
            if (m.h == h)
                return view.project(s.world(m.local));
        return std::nullopt;
    }

    Handle pick(const ProjectionShape& s, QPointF px, const ViewInfo& view) const
    {
        Handle best;
        double bestD = kPick;
        for (const Marker& m : markers(s))
            if (auto q = view.project(s.world(m.local)); q && pixelDistance(*q, px) <= bestD)
                bestD = pixelDistance(*q, px), best = m.h;
        glm::dvec3 c = s.center();
        if (auto q = view.project(c); q && pixelDistance(*q, px) <= std::min(bestD + 0.01, kPick))
            return Handle{HandleKind::Center};
        if (best.kind != HandleKind::None)
            return best;
        double upp = view.unitsPerPixel(c);
        double ringD = 6.0;
        for (int i = 0; i < 3; ++i)
        {
            std::vector<glm::dvec3> pts = ringAround(c, glm::normalize(s.dir(axisVector(i))), ringPixels() * upp);
            for (size_t k = 0; k < pts.size(); ++k)
            {
                auto a = view.project(pts[k]), b = view.project(pts[(k + 1) % pts.size()]);
                if (a && b && segmentDistPx(px, *a, *b) < ringD)
                    ringD = segmentDistPx(px, *a, *b), best = {HandleKind::Ring, i};
            }
        }
        return best;
    }

    /** The full frame as flags, so the re-run reproduces it exactly whatever the fit would do. */
    static json frameFlags(const ProjectionShape& s)
    {
        const uv::ProjectFrame& f = s.fr();
        return {{"center", vecJson(f.center)},
                {"rotation", vecJson(uv::frameEuler(f.axes))},
                {"width", f.width},
                {"height", f.height},
                {"depth", f.depth},
                {"radius", f.radius},
                {"seamAngle", glm::degrees(f.seamAngle)},
                {"scaleU", s.rec.scale.x},
                {"scaleV", s.rec.scale.y},
                {"offsetU", s.rec.offset.x},
                {"offsetV", s.rec.offset.y}};
    }

    void beginDrag(const ProjectionShape& s, const Handle& h, QMouseEvent* e, const ViewInfo& view)
    {
        lo::endDrag();
        Drag d{.h = h, .start = s, .base = frameFlags(s)};
        d.startPx = d.lastPx = e->position();
        glm::dvec3 o, dir;
        view.ray(e->position(), o, dir);
        glm::dvec3 c = s.center();
        switch (h.kind)
        {
        case HandleKind::Center: gizmo::rayPlane(o, dir, c, view.viewDir(c), d.startHit); break;
        case HandleKind::Ring:
        {
            glm::dvec3 n = glm::normalize(s.dir(axisVector(h.axis))), hit;
            if (gizmo::rayPlane(o, dir, c, n, hit))
                d.lastAngle = ringAngle(n, hit - c);
            break;
        }
        default: break;
        }
        d.readout = hint(s.rec.type, h);
        d.last = d.base;
        mDrag = std::move(d);
    }

    static double ringAngle(const glm::dvec3& n, const glm::dvec3& r)
    {
        glm::dvec3 a = glm::normalize(glm::cross(n, std::abs(n.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0)));
        glm::dvec3 b = glm::cross(n, a);
        return std::atan2(glm::dot(r, b), glm::dot(r, a));
    }

    static double snapDegrees(double deg)
    {
        IncrementSnap inc = tools::incrementSnap(Tool::Rotate);
        return inc.enabled ? gizmo::snapValue(deg, inc.step) : deg;
    }

    void dragTo(QPointF px, Qt::KeyboardModifiers mods, const ViewInfo& view)
    {
        Drag& d = *mDrag;
        d.lastPx = px;
        const ProjectionShape& s = d.start;
        const uv::ProjectFrame& f = s.fr();
        glm::dvec3 o, dir;
        view.ray(px, o, dir);
        glm::dvec3 c = s.center();
        bool shift = mods & Qt::ShiftModifier;
        json flags = d.base;
        double tiny = 1e-6 * std::max({f.width, f.height, f.depth, f.radius, 1e-6});
        auto lengthAlong = [&](const glm::dvec3& from, const glm::dvec3& localDir) {
            glm::dvec3 w = s.dir(localDir);
            double len = glm::length(w);
            return len > 0.0 ? gizmo::lineParam(from, w / len, o, dir) / len : 0.0;
        };
        auto scaleChannel = [&](const char* channel, double from, double to) {
            double v = d.base[channel].get<double>() * (from > 0.0 ? to / from : 1.0);
            flags[channel] = v;
            d.readout = QString("%1 x%2").arg(channel[5] == 'U' ? "U scale" : "V scale").arg(v, 0, 'f', 3);
        };

        switch (d.h.kind)
        {
        case HandleKind::Center:
        {
            if (mods & Qt::ControlModifier)
            {
                QPointF delta = px - d.startPx;
                double u = d.base["offsetU"].get<double>() + delta.x() / 250.0, v = d.base["offsetV"].get<double>() - delta.y() / 250.0;
                flags["offsetU"] = u, flags["offsetV"] = v;
                d.readout = QString("U/V offset %1, %2").arg(u, 0, 'f', 3).arg(v, 0, 'f', 3);
                break;
            }
            glm::dvec3 hit;
            if (!gizmo::rayPlane(o, dir, c, view.viewDir(c), hit))
                return;
            glm::dvec3 moved = glm::dvec3(s.inv * glm::dvec4(c + hit - d.startHit, 1.0));
            flags["center"] = vecJson(moved);
            d.readout = QString("centre %1, %2, %3").arg(moved.x, 0, 'f', 2).arg(moved.y, 0, 'f', 2).arg(moved.z, 0, 'f', 2);
            break;
        }
        case HandleKind::Ring:
        {
            glm::dvec3 n = glm::normalize(s.dir(axisVector(d.h.axis))), hit;
            if (!gizmo::rayPlane(o, dir, c, n, hit))
                return;
            double a = ringAngle(n, hit - c);
            d.angle += std::remainder(a - d.lastAngle, glm::two_pi<double>());
            d.lastAngle = a;
            double deg = snapDegrees(glm::degrees(d.angle));
            // The world turn about the world axis is the same turn about the frame axis in object space when the
            // object's scale is uniform; a mirrored object turns the other way.
            double sign = glm::determinant(s.w3) < 0.0 ? -1.0 : 1.0;
            glm::dmat3 turn(glm::rotate(glm::dmat4(1.0), glm::radians(deg) * sign, f.axes[d.h.axis]));
            flags["rotation"] = vecJson(uv::frameEuler(turn * f.axes));
            d.readout = QString("rotate %1°").arg(deg, 0, 'f', 1);
            break;
        }
        case HandleKind::Size:
        {
            const double from = s.rec.type == Projection::Cylindrical || d.h.axis == 1 ? f.height : d.h.axis == 0 ? f.width : f.depth;
            // Sizes stay centred on the frame, as the handle on either side pulls both.
            double size = 2.0 * std::max(d.h.sign * lengthAlong(c, axisVector(d.h.axis)), tiny);
            if (shift)
                scaleChannel(d.h.axis == 0 ? "scaleU" : "scaleV", from, size);
            else
            {
                const char* name = sizeFlag(s.rec.type, d.h.axis);
                flags[name] = size;
                d.readout = QString("%1 %2").arg(name).arg(size, 0, 'f', 2);
            }
            break;
        }
        case HandleKind::Radius:
        {
            double r = std::max(lengthAlong(c, s.around(s.seam() + glm::half_pi<double>(), 1.0, 0.0)), tiny);
            if (shift)
                scaleChannel("scaleU", f.radius, r);
            else
            {
                flags["radius"] = r;
                d.readout = QString("radius %1").arg(r, 0, 'f', 2);
            }
            break;
        }
        case HandleKind::Seam:
        {
            glm::dvec3 capCenter = s.world({0, 0, s.rec.type == Projection::Cylindrical ? f.height * 0.5 : 0.0}), hit;
            glm::dvec3 n = s.dir(axisVector(2));
            if (glm::length(n) <= 0.0 || !gizmo::rayPlane(o, dir, capCenter, glm::normalize(n), hit))
                return;
            glm::dvec3 l = s.local(hit);
            if (glm::length(glm::dvec2(l)) <= tiny)
                return;
            double deg = snapDegrees(glm::degrees(std::atan2(l.y, l.x)));
            flags["seamAngle"] = deg;
            d.readout = QString("seam %1°").arg(deg, 0, 'f', 1);
            break;
        }
        default: return;
        }
        if (flags == d.last)
            return;
        try
        {
            lo::rerun(flags, CallSource::Script, true);
            d.last = flags;
        }
        catch (const CommandError& e)
        {
            log::warn("uvProject: {}", e.what());
        }
        viewportTools::requestRedraw();
    }

    void finishDrag()
    {
        mDrag.reset();
        lo::endDrag();
        if (const lo::Op* op = lo::current(); op && op->command == kProjectCommand)
            log::write(LogLevel::Echo, ops::toPython(op->command, op->call) + "\n");
        viewportTools::requestRedraw();
    }

    void cancelDrag()
    {
        mDrag.reset();
        lo::endDrag(false);
        viewportTools::requestRedraw();
    }

    std::optional<Drag> mDrag;
    Handle mHover;
};

void onLastOp(lo::Change change)
{
    bool ours = viewportTools::active() && std::string_view(viewportTools::active()->name()) == kToolName;
    if ((change == lo::Change::Recorded || change == lo::Change::Rerun || change == lo::Change::Restored) && sessionLive())
    {
        if (!ours)
            QTimer::singleShot(0, [] {
                ViewportTool* t = viewportTools::active();
                if (sessionLive() && !(t && std::string_view(t->name()) == kToolName))
                    viewportTools::set(std::make_unique<UvProjectTool>());
            });
        return;
    }
    if (ours && !sessionLive())
        QTimer::singleShot(0, [] {
            ViewportTool* t = viewportTools::active();
            if (t && std::string_view(t->name()) == kToolName && !sessionLive())
                viewportTools::set(nullptr);
        });
}
} // namespace

void installUvProjectTool()
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;
    lo::install();
    lo::addListener(onLastOp);
    // The seam goes behind the last perspective view drawn (any view when there is none).
    viewportHooks::addDrawer("lila.uvProject.view", [](render::FrameState&, OverlayPainter&, const ViewInfo& view) {
        if (view.ortho && gViewDir)
            return;
        glm::dvec3 forward = -glm::dvec3(view.view[0][2], view.view[1][2], view.view[2][2]);
        if (glm::length(forward) > 1e-12)
            gViewDir = glm::normalize(forward);
        if (!view.ortho)
            gViewEye = glm::dvec3(glm::inverse(glm::dmat4(view.view))[3]);
    });
    uv::projectionViewProvider() = [] { return gViewDir; };
    uv::projectionCameraProvider() = []() -> std::optional<glm::dmat4> {
        ViewportWidget* v = cameraBookmarks::activeView();
        if (!v)
            return std::nullopt;
        return v->viewInfo().viewProj();
    };
    actions::add({.id = "uv.seamView",
                  .label = "Add Seam View",
                  .category = "UV",
                  .run =
                      [] {
                          json call = json::object();
                          if (gViewEye)
                              call["position"] = {gViewEye->x, gViewEye->y, gViewEye->z};
                          try
                          {
                              ops::execute("uvSeamView", call, CallSource::UI);
                          }
                          catch (const CommandError& e)
                          {
                              log::warn("{}", e.what());
                          }
                      },
                  .doc = "A locator where the camera is now, that Auto UV keeps seams out of sight of. Move it and run Auto UV again.",
                  .python = "lila.ops.uvSeamView()"});
}
} // namespace lila::ui
