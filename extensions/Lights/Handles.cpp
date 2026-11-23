#include "Handles.h"

#include <lila/Command.h>
#include <lila/Graph.h>
#include <lila/Lights.h>
#include <lila/Log.h>
#include <lila/Undo.h>
#include <lila/Viewport.h>
#include <lila/ViewportTool.h>

#include <QCursor>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <optional>

#include <glm/gtc/matrix_transform.hpp>

using namespace lila;
using scene::Light;
using scene::NodeId;
using scene::PlugValue;
using scene::lights::Kind;

namespace lights_ext
{
namespace
{
enum Handle
{
    kSwatch,
    kIntensity,
    kSceneLightsOff,
    kRange,
    kOuter,
    kInner,
    kWidth,
    kHeight,
    kCorner,
    kSource,
    kHandleCount,
};

// Glyphs.cpp draws with these sizes.
constexpr double kGlyphSize = 10.0;
constexpr double kGlyphReach = 50.0;

constexpr float kDot = 4.5f;
constexpr double kPickPx = 12.0;
/** Screen pixels of intensity drag per stop (a doubling); Shift drags five times finer. */
constexpr double kPxPerStop = 60.0;

const glm::vec4 kIdle{1.0f, 0.8f, 0.25f, 1.0f};
const glm::vec4 kHot{1.0f, 1.0f, 1.0f, 1.0f};
const glm::vec4 kPlate{0.08f, 0.08f, 0.08f, 0.75f};
const glm::vec4 kPlateHot{0.2f, 0.2f, 0.2f, 0.9f};

/** A world line a handle moves along, and how a pixel of cursor motion maps onto it. */
struct Line
{
    glm::dvec3 origin{0.0};
    glm::dvec3 dir{1, 0, 0}; // world, unit
    double worldPerLocal = 1.0;
    double t = 0.0; // the handle's local distance along it at layout time
};

struct Layout
{
    scene::lights::Settings s;
    std::array<std::optional<QPointF>, kHandleCount> at;
    std::array<Line, kHandleCount> lines;
    /** Handles of a setting at 0 that means "none" (no range cutoff, no source radius): drawn hollow. */
    std::array<bool, kHandleCount> unset{};
    QRectF swatch, intensity;
    /** The Scene Lights off chip beside the intensity, empty while scene lights are on. */
    QRectF sceneLightsOff;
    double reach = 0.0;    // spot: the cone's length
    double pointBody = 0.0; // point: the bulb radius drawn
};

const char* unitSuffix(scene::lights::Units u)
{
    using U = scene::lights::Units;
    switch (u)
    {
    case U::Candela: return " cd";
    case U::Lumens: return " lm";
    case U::Lux: return " lx";
    case U::Nits: return " nt";
    case U::Watts: return " W";
    case U::Unitless: break;
    }
    return "";
}

QString intensityText(const scene::lights::Settings& s) { return QString::number(s.intensity, 'g', 3) + unitSuffix(s.units); }

/**
 * Where a light with no range has faded to 1% of the default environment: the viewport's inverse square in metres, with
 * its unit scale. A range dragged from there changes almost nothing until it comes in further.
 */
double fadeDistance(const scene::lights::Settings& s)
{
    using U = scene::lights::Units;
    const double outer = glm::radians(std::clamp(s.outerConeAngle, 0.1, 90.0));
    double scale = 1.0;
    switch (s.units)
    {
    case U::Candela:
    case U::Nits:
    case U::Lux: scale = 0.01; break;
    case U::Lumens: scale = 0.01 / (s.kind == Kind::Spot ? 2.0 * std::numbers::pi * (1.0 - std::cos(outer)) : 4.0 * std::numbers::pi); break;
    case U::Watts: scale = 0.01 * 683.0 / (4.0 * std::numbers::pi); break;
    case U::Unitless: break;
    }
    const glm::dvec3 c = scene::lights::effectiveColor(s);
    const double brightest = std::max(s.intensity, 0.0) * scale * std::max({c.x, c.y, c.z});
    return std::max(100.0 * std::sqrt(brightest / 0.01), kGlyphReach);
}

double toSrgb(double c)
{
    c = std::clamp(c, 0.0, 1.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

Layout layout(const ui::nodeHandles::Context& c)
{
    Layout l;
    l.s = scene::lights::settings(c.node);
    const scene::lights::Settings& s = l.s;
    const glm::dmat4 m = c.world * glm::scale(glm::dmat4(1.0), c.node.as<glm::dvec3>(scene::Locator::kLocalScale));
    const glm::dmat3 m3(m);
    if (std::abs(glm::determinant(m3)) < 1e-18)
        return l;
    const glm::dmat3 inv = glm::inverse(m3);
    auto world = [&](glm::dvec3 p) { return glm::dvec3(m * glm::dvec4(p, 1.0)); };
    const glm::dvec3 camRight(c.view.view[0][0], c.view.view[1][0], c.view.view[2][0]);
    const glm::dvec3 camUp(c.view.view[0][1], c.view.view[1][1], c.view.view[2][1]);
    // Directions that face the camera, in the light's own space; inPlane keeps them in its XY plane (cone rims, discs).
    auto facing = [&](glm::dvec3 w, bool inPlane) {
        glm::dvec3 v = inv * w;
        if (inPlane)
            v.z = 0.0;
        const double len = glm::length(v);
        return len > 1e-9 ? v / len : glm::dvec3(1, 0, 0);
    };

    // Places handle h at t along a local line, at least minPx from the line's start on screen so handles on a tiny glyph
    // stay apart. Drags are relative, so the push never makes the value jump.
    auto place = [&](Handle h, glm::dvec3 origin, glm::dvec3 dir, double t, double minPx) {
        const glm::dvec3 wd = m3 * dir;
        const double k = glm::length(wd);
        if (k < 1e-12)
            return;
        l.lines[h] = {world(origin), wd / k, k, t};
        auto o = c.view.project(world(origin));
        auto p = c.view.project(world(origin + dir * t));
        if (!o || !p)
            return;
        QPointF d = *p - *o;
        double len = std::hypot(d.x(), d.y());
        if (len < minPx)
        {
            auto q = c.view.project(world(origin) + wd / k * c.view.unitsPerPixel(world(origin)) * 20.0);
            QPointF u = q ? *q - *o : QPointF(1, 0);
            const double ul = std::hypot(u.x(), u.y());
            u = ul > 1e-6 ? u / ul : QPointF(1, 0);
            p = *o + u * minPx;
        }
        l.at[h] = *p;
    };

    const bool noRange = s.range <= 0.0;
    const double range = noRange ? fadeDistance(s) : s.range;
    l.unset[kRange] = noRange;
    l.unset[kSource] = s.sourceRadius <= 0.0;

    switch (s.kind)
    {
    case Kind::Point:
    {
        l.pointBody = s.sourceRadius > 0.0 ? s.sourceRadius : kGlyphSize;
        place(kRange, {}, facing(camRight, false), range, 22.0);
        place(kSource, {}, facing(camUp, false), s.sourceRadius, 10.0);
        break;
    }
    case Kind::Spot:
    {
        l.reach = s.range > 0.0 ? s.range : kGlyphReach * 2.0;
        const glm::dvec3 end{0, 0, -l.reach};
        const glm::dvec3 side = facing(camRight, true);
        place(kRange, {}, {0, 0, -1}, range, 24.0);
        place(kOuter, end, side, l.reach * std::tan(glm::radians(std::clamp(s.outerConeAngle, 0.5, 89.0))), 16.0);
        place(kInner, end, -side, l.reach * std::tan(glm::radians(std::clamp(s.innerConeAngle, 0.0, 89.0))), 16.0);
        break;
    }
    case Kind::Area:
    {
        const double w = std::max(s.width, 1e-3) * 0.5, h = std::max(s.height, 1e-3) * 0.5;
        place(kWidth, {}, {1, 0, 0}, w, 12.0);
        place(kHeight, {}, {0, 1, 0}, h, 12.0);
        place(kCorner, {}, glm::normalize(glm::dvec3(w, h, 0)), std::hypot(w, h), 18.0);
        place(kRange, {}, {0, 0, -1}, range, 24.0);
        break;
    }
    case Kind::Disc:
    {
        const double r = std::max(s.width, 1e-3) * 0.5;
        place(kWidth, {}, facing(camRight, true), r, 12.0);
        place(kRange, {}, {0, 0, -1}, range, 24.0);
        break;
    }
    case Kind::Directional:
    case Kind::Dome: break;
    }

    // The swatch and intensity sit centred below the light, where the manipulator's up axis never points.
    if (auto o = c.view.project(world({})))
    {
        const double w = ui::OverlayPainter::textWidth(intensityText(s)) + 10;
        const double off = ui::sceneLights::on() ? 0.0 : 3 + 16;
        l.swatch = QRectF(o->x() - (14 + 3 + w + off) * 0.5, o->y() + 30, 14, 14);
        l.intensity = QRectF(l.swatch.right() + 3, l.swatch.top() - 1, w, 16);
        l.at[kSwatch] = l.swatch.center();
        l.at[kIntensity] = l.intensity.center();
        if (off > 0.0)
        {
            l.sceneLightsOff = QRectF(l.intensity.right() + 3, l.intensity.top(), 16, 16);
            l.at[kSceneLightsOff] = l.sceneLightsOff.center();
        }
    }
    return l;
}

int pickHandle(QPointF px, const ui::nodeHandles::Context& c)
{
    const Layout l = layout(c);
    if (l.swatch.adjusted(-2, -2, 2, 2).contains(px))
        return kSwatch;
    if (l.intensity.adjusted(-2, -2, 2, 2).contains(px))
        return kIntensity;
    if (l.sceneLightsOff.adjusted(-2, -2, 2, 2).contains(px))
        return kSceneLightsOff;
    int best = -1;
    double bestD = kPickPx;
    for (int h = kRange; h < kHandleCount; ++h)
        if (l.at[h])
            if (double d = std::hypot(px.x() - l.at[h]->x(), px.y() - l.at[h]->y()); d < bestD)
            {
                best = h;
                bestD = d;
            }
    return best;
}

void drawHandles(ui::OverlayPainter& p, const ui::nodeHandles::Context& c, int hovered, int active)
{
    const Layout l = layout(c);
    const int hot = active >= 0 ? active : hovered;
    for (int h = kRange; h < kHandleCount; ++h)
        if (l.at[h])
        {
            const bool on = h == hot;
            const float r = on ? 6.0f : kDot;
            p.disc(*l.at[h], r + 1.5f, {0.0f, 0.0f, 0.0f, 0.6f});
            p.disc(*l.at[h], r, on ? kHot : kIdle);
            if (l.unset[h] && !on)
                p.disc(*l.at[h], r - 2.0f, {0.1f, 0.1f, 0.1f, 1.0f});
        }
    if (l.swatch.isEmpty())
        return;
    const glm::dvec3 col = scene::lights::effectiveColor(l.s) / std::max({l.s.color.x, l.s.color.y, l.s.color.z, 1.0});
    p.rect(l.swatch.adjusted(-1.5, -1.5, 1.5, 1.5), hot == kSwatch ? kHot : glm::vec4(0.0f, 0.0f, 0.0f, 0.7f));
    p.rect(l.swatch, {float(toSrgb(col.x)), float(toSrgb(col.y)), float(toSrgb(col.z)), 1.0f});
    p.rect(l.intensity, hot == kIntensity ? kPlateHot : kPlate);
    p.text(QPointF(l.intensity.left() + 5, l.intensity.bottom() - 4), intensityText(l.s), hot == kIntensity ? kHot : glm::vec4(0.9f, 0.9f, 0.9f, 1.0f));
    if (!l.sceneLightsOff.isEmpty())
    {
        p.rect(l.sceneLightsOff, hot == kSceneLightsOff ? kPlateHot : kPlate);
        p.icon(l.sceneLightsOff.adjusted(1, 1, -1, -1), "sceneLightsOff",
               hot == kSceneLightsOff ? kHot : ui::sceneLights::iconColor());
    }
}

/** Lights in the selection. */
std::vector<NodeId> selectedLights()
{
    std::vector<NodeId> out;
    scene::Graph& g = scene::Graph::current();
    for (NodeId id : g.selection())
        if (const scene::Node* n = g.node(id); n && n->type().isA("light"))
            out.push_back(id);
    return out;
}

/** Edits to light plugs, live with undo off; commit puts the start values back and sets the changed ones by command, as one undo step. */
class PlugEdit
{
public:
    void set(NodeId id, int attr, PlugValue v)
    {
        scene::Graph& g = scene::Graph::current();
        if (!g.node(id))
            return;
        auto it = std::find_if(mPlugs.begin(), mPlugs.end(), [&](const Saved& s) { return s.id == id && s.attr == attr; });
        if (it == mPlugs.end())
            mPlugs.push_back({id, attr, g.storedValue({id, attr})});
        UndoStack& u = UndoStack::get();
        const bool was = u.enabled();
        u.setEnabled(false);
        g.setValue({id, attr}, std::move(v));
        u.setEnabled(was);
        ui::viewportTools::requestRedraw();
    }

    void restore()
    {
        scene::Graph& g = scene::Graph::current();
        UndoStack& u = UndoStack::get();
        const bool was = u.enabled();
        u.setEnabled(false);
        for (const Saved& s : mPlugs)
            if (g.node(s.id))
                g.setValue({s.id, s.attr}, s.start);
        u.setEnabled(was);
        ui::viewportTools::requestRedraw();
    }

    void commit(const char* label)
    {
        scene::Graph& g = scene::Graph::current();
        std::vector<std::pair<std::string, PlugValue>> changed;
        for (const Saved& s : mPlugs)
            if (const scene::Node* n = g.node(s.id))
                if (const PlugValue& now = g.storedValue({s.id, s.attr}); now != s.start)
                    changed.emplace_back(n->path() + "." + n->type().attrs[size_t(s.attr)].name, now);
        restore();
        if (changed.empty())
            return;
        UndoChunk chunk(label);
        for (const auto& [plug, value] : changed)
            try
            {
                ops::execute("setAttr", {{"args", {plug, scene::toJson(value)}}}, CallSource::UI);
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
        PlugValue start;
    };
    std::vector<Saved> mPlugs;
};

/** Dragging a handle on the glyph: the setting moves with the cursor along the handle's line, relative to the press. */
class ShapeDrag final : public ui::nodeHandles::Drag
{
public:
    ShapeDrag(Handle h, QPointF press, const ui::nodeHandles::Context& c) : mHandle(h), mNode(c.node.id()), mPress(press)
    {
        const Layout l = layout(c);
        mStart = l.s;
        mLine = l.lines[h];
        mReach = l.reach;
        mPointBody = l.pointBody;
        // Pixels to world along the line, fixed at the press so a foreshortened line doesn't speed up mid-drag.
        const double span = c.view.unitsPerPixel(mLine.origin) * 50.0;
        auto a = c.view.project(mLine.origin), b = c.view.project(mLine.origin + mLine.dir * span);
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

    void move(QPointF px, const ui::ViewInfo&, Qt::KeyboardModifiers mods) override
    {
        double px2 = QPointF::dotProduct(px - mPress, mPxDir);
        if (mods & Qt::ShiftModifier)
            px2 *= 0.2;
        const double t = mLine.t + px2 * mWorldPerPx / mLine.worldPerLocal;
        const scene::lights::Settings& s = mStart;
        switch (mHandle)
        {
        case kRange:
            // A point light pulled back into its bulb has no cutoff again.
            mEdit.set(mNode, Light::kRange, s.kind == Kind::Point && t < mPointBody ? 0.0 : std::max(0.0, t));
            break;
        case kSource: mEdit.set(mNode, Light::kSourceRadius, std::max(0.0, t)); break;
        case kOuter:
        {
            const double outer = std::clamp(glm::degrees(std::atan2(std::max(t, 0.0), mReach)), 0.5, 89.0);
            mEdit.set(mNode, Light::kOuterConeAngle, outer);
            mEdit.set(mNode, Light::kInnerConeAngle, std::min(s.innerConeAngle, outer));
            break;
        }
        case kInner:
            mEdit.set(mNode, Light::kInnerConeAngle, std::clamp(glm::degrees(std::atan2(std::max(t, 0.0), mReach)), 0.0, s.outerConeAngle));
            break;
        case kWidth: mEdit.set(mNode, Light::kWidth, std::max(0.1, t * 2.0)); break;
        case kHeight: mEdit.set(mNode, Light::kHeight, std::max(0.1, t * 2.0)); break;
        case kCorner:
        {
            const double f = std::max(t, 0.05) / std::max(mLine.t, 1e-6);
            mEdit.set(mNode, Light::kWidth, std::max(0.1, s.width * f));
            mEdit.set(mNode, Light::kHeight, std::max(0.1, s.height * f));
            break;
        }
        default: break;
        }
    }

    void release(bool moved) override
    {
        if (moved)
            mEdit.commit(label());
    }
    void cancel() override { mEdit.restore(); }

    QString readout() const override
    {
        const scene::Node* n = scene::Graph::current().node(mNode);
        if (!n)
            return {};
        const scene::lights::Settings s = scene::lights::settings(*n);
        auto cm = [](double v) { return QString::number(v, 'f', v < 10.0 ? 2 : 1) + " cm"; };
        auto deg = [](double v) { return QString::number(v, 'f', 1) + QChar(0x00B0); };
        switch (mHandle)
        {
        case kRange: return s.range > 0.0 ? "Range " + cm(s.range) : QString("Range: none");
        case kSource: return "Source radius " + cm(s.sourceRadius);
        case kOuter: return "Outer cone " + deg(s.outerConeAngle);
        case kInner: return "Inner cone " + deg(s.innerConeAngle);
        case kWidth: return (s.kind == Kind::Disc ? "Diameter " : "Width ") + cm(s.width);
        case kHeight: return "Height " + cm(s.height);
        case kCorner: return "Size " + QString::number(s.width, 'f', 1) + QString(" x ") + cm(s.height);
        default: return {};
        }
    }

private:
    const char* label() const
    {
        switch (mHandle)
        {
        case kRange: return "Light Range";
        case kSource: return "Light Source Radius";
        case kOuter:
        case kInner: return "Light Cone";
        default: return "Light Size";
        }
    }

    Handle mHandle;
    NodeId mNode;
    QPointF mPress;
    scene::lights::Settings mStart;
    Line mLine;
    double mReach = 0.0, mPointBody = 0.0;
    QPointF mPxDir{1, 0};
    double mWorldPerPx = 1.0;
    PlugEdit mEdit;
};

/** Dragging the intensity sideways scales every selected light's by the same number of stops. */
class IntensityDrag final : public ui::nodeHandles::Drag
{
public:
    explicit IntensityDrag(QPointF press) : mPress(press)
    {
        scene::Graph& g = scene::Graph::current();
        for (NodeId id : selectedLights())
            mStart.emplace_back(id, g.node(id)->as<double>(Light::kIntensity));
    }

    void move(QPointF px, const ui::ViewInfo&, Qt::KeyboardModifiers mods) override
    {
        const double dx = px.x() - mPress.x();
        mStops = dx / (kPxPerStop * ((mods & Qt::ShiftModifier) ? 5.0 : 1.0));
        for (const auto& [id, v] : mStart)
            mEdit.set(id, Light::kIntensity, v > 0.0 ? v * std::exp2(mStops) : std::max(0.0, dx * 0.01));
    }
    void release(bool moved) override
    {
        if (moved)
            mEdit.commit("Light Intensity");
    }
    void cancel() override { mEdit.restore(); }

    QString readout() const override
    {
        if (mStart.empty())
            return {};
        const scene::Node* n = scene::Graph::current().node(mStart.front().first);
        if (!n)
            return {};
        QString text = "Intensity " + intensityText(scene::lights::settings(*n));
        if (mStart.front().second > 0.0)
            text += QString("  %1%2 stops").arg(mStops >= 0 ? "+" : "").arg(mStops, 0, 'f', 1);
        if (mStart.size() > 1)
            text += QString("  (%1 lights)").arg(mStart.size());
        return text;
    }

private:
    QPointF mPress;
    std::vector<std::pair<NodeId, double>> mStart;
    double mStops = 0.0;
    PlugEdit mEdit;
};

/** A click on the swatch opens the colour wheel for every selected light. */
void openColorPopup(NodeId lead)
{
    scene::Graph& g = scene::Graph::current();
    const scene::Node* n = g.node(lead);
    if (!n)
        return;
    std::vector<NodeId> lights = selectedLights();
    if (lights.empty())
        lights.push_back(lead);
    const scene::lights::Settings s = scene::lights::settings(*n);
    auto edit = std::make_shared<PlugEdit>();
    ui::colorPopup::Options o;
    o.value = {.color = s.color, .useTemperature = s.useTemperature, .temperature = s.temperature};
    o.temperature = true;
    o.title = lights.size() > 1 ? QString("%1 lights").arg(lights.size()) : QString::fromStdString(n->name());
    o.changed = [edit, lights](const ui::colorPopup::Value& v) {
        for (NodeId id : lights)
        {
            edit->set(id, Light::kColor, v.color);
            edit->set(id, Light::kUseTemperature, v.useTemperature);
            edit->set(id, Light::kTemperature, v.temperature);
        }
    };
    o.closed = [edit](const ui::colorPopup::Value&, bool accepted) {
        if (accepted)
            edit->commit("Light Colour");
        else
            edit->restore();
    };
    ui::colorPopup::show(QCursor::pos() + QPoint(12, 12), std::move(o));
}

/** A click on the Scene Lights off chip turns scene lights back on. */
class SceneLightsClick final : public ui::nodeHandles::Drag
{
public:
    void move(QPointF, const ui::ViewInfo&, Qt::KeyboardModifiers) override {}
    void release(bool) override { ui::sceneLights::setOn(true); }
    void cancel() override {}
};

class SwatchClick final : public ui::nodeHandles::Drag
{
public:
    explicit SwatchClick(NodeId node) : mNode(node) {}
    void move(QPointF, const ui::ViewInfo&, Qt::KeyboardModifiers) override {}
    void release(bool) override { openColorPopup(mNode); }
    void cancel() override {}

private:
    NodeId mNode;
};

std::unique_ptr<ui::nodeHandles::Drag> beginDrag(int handle, QPointF px, const ui::nodeHandles::Context& c)
{
    switch (handle)
    {
    case kSwatch: return std::make_unique<SwatchClick>(c.node.id());
    case kIntensity: return std::make_unique<IntensityDrag>(px);
    case kSceneLightsOff: return std::make_unique<SceneLightsClick>();
    default: return std::make_unique<ShapeDrag>(Handle(handle), px, c);
    }
}
} // namespace

void addHandles(ext::Context& ctx)
{
    ui::nodeHandles::add(ctx, {.id = "lights.handles", .nodeType = "light", .draw = drawHandles, .pick = pickHandle, .begin = beginDrag});
}
} // namespace lights_ext
