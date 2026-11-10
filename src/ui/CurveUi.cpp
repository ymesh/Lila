#include "ui/CurveUi.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "curve/CurveNodes.h"
#include "curve/Tessellate.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/CurveToolsCommon.h"
#include "ui/DrawOverrides.h"
#include "ui/SketchUi.h"
#include "ui/Theme.h"
#include "ui/UiCommands.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportToolMath.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <unordered_map>
#include <cmath>
#include <map>
#include <set>

namespace lila::ui
{
namespace
{
using curve::CurveShape;

// Linear for 3D lines; sRGB for the 2D overlay.
const glm::vec4 kCurve(0.392f, 0.392f, 0.392f, 1.0f); // Monotone's "curve" token
const glm::vec4 kWireDefault(0.02f, 0.02f, 0.02f, 1.0f);
const glm::vec4 kHalo(0.008f, 0.008f, 0.008f, 1.0f);
const glm::vec4 kHull(0.45f, 0.45f, 0.45f, 1.0f);
const glm::vec4 kComb(0.35f, 0.35f, 0.35f, 1.0f);
const glm::vec4 kPointRim(0.05f, 0.05f, 0.05f, 1.0f);
const glm::vec4 kPoint(0.62f, 0.62f, 0.62f, 1.0f);
const glm::vec4 kPointSelected(1.0f, 1.0f, 1.0f, 1.0f);

/** Tessellations by node, reused while the node's splines and display settings stay the same. */
struct Cached
{
    const void* data = nullptr;
    int64_t mode = -1, segments = -1;
    std::vector<curve::Polyline> lines;
};

std::map<scene::NodeId, Cached>& curveCache()
{
    static std::map<scene::NodeId, Cached> c;
    return c;
}

const std::vector<curve::Polyline>& linesOf(const scene::Node& n)
{
    const scene::PlugValue& v = n.value(CurveShape::kLocal);
    const scene::DataPtr* d = std::get_if<scene::DataPtr>(&v);
    Cached& c = curveCache()[n.id()];
    int64_t mode = n.as<int64_t>(CurveShape::kTessMode), seg = n.as<int64_t>(CurveShape::kTessSegments);
    const void* key = d ? d->get() : nullptr;
    if (c.data == key && c.mode == mode && c.segments == seg)
        return c.lines;
    c.data = key;
    c.mode = mode;
    c.segments = seg;
    c.lines.clear();
    curve::TessOptions o;
    o.mode = curve::TessMode(std::clamp<int64_t>(mode, 0, 2));
    o.perSpan = int(std::max<int64_t>(1, seg));
    o.chordError = 0.01;
    o.maxAngle = 0.07;
    for (const curve::Spline& s : curve::splinesOf(v))
        c.lines.push_back(curve::tessellate(s, o));
    return c.lines;
}

/** The curve's symmetry copies, tessellated in world space; empty with symmetry off. */
const std::vector<curve::Polyline>& mirrorLinesOf(const scene::Node& n)
{
    static std::map<scene::NodeId, Cached> mirrors;
    const scene::PlugValue& v = n.value(CurveShape::kWorldSpace);
    const scene::DataPtr* d = std::get_if<scene::DataPtr>(&v);
    Cached& c = mirrors[n.id()];
    int64_t mode = n.as<int64_t>(CurveShape::kTessMode), seg = n.as<int64_t>(CurveShape::kTessSegments);
    const void* key = d ? d->get() : nullptr;
    if (c.data == key && c.mode == mode && c.segments == seg)
        return c.lines;
    c.data = key;
    c.mode = mode;
    c.segments = seg;
    c.lines.clear();
    if (curve::symmetryAxis(n) < 0)
        return c.lines;
    curve::TessOptions o;
    o.mode = curve::TessMode(std::clamp<int64_t>(mode, 0, 2));
    o.perSpan = int(std::max<int64_t>(1, seg));
    o.chordError = 0.01;
    o.maxAngle = 0.07;
    const curve::Splines& all = curve::splinesOf(v);
    for (size_t k = curve::ownSplineCount(n); k < all.size(); ++k)
        c.lines.push_back(curve::tessellate(all[k], o));
    return c.lines;
}

/** The soft select falloff colour for the overlay painter, which takes sRGB. */
glm::vec4 softFill(float weight, bool grayscale)
{
    const glm::vec4 c = viewportDisplay::falloffColor(weight, grayscale);
    auto enc = [](float v) { return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f; };
    return {enc(c.r), enc(c.g), enc(c.b), c.a};
}

/** The node is being edited: vertex mode with it selected or holding selected points. */
bool editing(const scene::Node& n)
{
    if (scene::components::mode() != scene::ComponentMode::Vertex)
        return false;
    scene::Graph& g = scene::Graph::current();
    return g.isSelected(n.id()) || !scene::components::selected(n.id(), scene::ComponentMode::Vertex).empty();
}

void line(render::FrameState& f, const glm::dvec3& a, const glm::dvec3& b, const glm::vec4& c, bool onTop)
{
    auto& out = onTop ? f.linesOnTop : f.lines;
    out.push_back({glm::vec3(a), c});
    out.push_back({glm::vec3(b), c});
}

/** The curve's own line colour: the theme's curve colour unless a highlight or display layer gives another. */
glm::vec4 curveColor(const drawOverrides::DrawContext& c)
{
    if (c.highlight == drawOverrides::Highlight::None && c.color == themeLinear("wireframe", kWireDefault))
        return themeLinear("curve", kCurve);
    return c.color;
}

/** Hair: a curve showing a groom's strands. */
bool strandCurves(const scene::Node& n)
{
    const std::optional<scene::PlugRef> from = scene::Graph::current().source({n.id(), CurveShape::kCreate, -1});
    if (!from)
        return false;
    const scene::Node* src = scene::Graph::current().node(from->node);
    return src && src->type().isA("groom") && from->attr == src->type().attrIndex("outStrands");
}

/** A strand's width where the graph gives none, scene units: GroomLab's Width node default, about a human hair in cm. */
constexpr float kDefaultStrandWidth = 0.007f;

uint64_t mix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    return x ^ (x >> 33);
}

/** A node's strands on the GPU, with their bounds for the distance thinning. */
struct StrandEntry
{
    const void* data = nullptr;
    std::shared_ptr<const render::StrandCloud> cloud;
    /** What the layout was made for: each strand's point count and id. */
    uint64_t layoutSignature = 0;
    glm::dvec3 centre{0.0};
    double radius = 0.0;
};

/** The strands' layout identity: their point counts and ids, so moving strands keep it. */
uint64_t layoutSignature(const curve::Splines& splines)
{
    uint64_t h = mix64(splines.size());
    for (const curve::Spline& s : splines)
    {
        const auto id = s.attrs.find("id");
        h = mix64(h ^ (uint64_t(s.size()) * 0x9e3779b97f4a7c15ull) ^ (id == s.attrs.end() ? 0 : uint64_t(id->second)));
    }
    return h;
}

/**
 * Segments and strands for the renderer. Each strand's rank spreads its clump's strands evenly over 0 to 1, so thinning
 * keeps every clump, each with the same share of its strands.
 */
std::shared_ptr<const render::StrandLayout> strandLayout(const curve::Splines& splines)
{
    auto layout = std::make_shared<render::StrandLayout>();
    std::map<double, std::vector<size_t>> clumps;
    for (size_t k = 0; k < splines.size(); ++k)
    {
        const auto it = splines[k].attrs.find("clumpId");
        clumps[it == splines[k].attrs.end() ? -1.0 - double(k % 64) : it->second].push_back(k);
    }
    std::vector<float> rank(splines.size(), 0.0f);
    for (auto& [clump, members] : clumps)
    {
        auto key = [&](size_t k) {
            const auto it = splines[k].attrs.find("id");
            return mix64(it == splines[k].attrs.end() ? uint64_t(k) : uint64_t(it->second));
        };
        std::sort(members.begin(), members.end(), [&](size_t a, size_t b) { return key(a) < key(b); });
        for (size_t i = 0; i < members.size(); ++i)
            rank[members[i]] = (float(i) + float(key(members[i]) % 1000) / 1000.0f) / float(members.size());
    }
    constexpr uint32_t kHair = 0xff2f4c6eu; // sRGB (110, 76, 47)
    uint32_t first = 0;
    for (size_t k = 0; k < splines.size(); ++k)
    {
        const uint32_t count = uint32_t(splines[k].size());
        if (count >= 2)
        {
            const uint32_t strand = uint32_t(layout->strands.size());
            layout->strands.push_back({first, count, glm::floatBitsToUint(rank[k]), kHair});
            for (uint32_t i = 0; i + 1 < count; ++i)
                layout->segments.push_back({first + i, strand});
            first += count;
        }
    }
    layout->points = first;
    return layout;
}

/** The strands for the renderer: their points each time the curve's data changes, the layout while it holds. */
const StrandEntry& strandEntry(const scene::Node& n, const curve::Splines& splines)
{
    static std::unordered_map<scene::NodeId, StrandEntry> cache;
    const scene::PlugValue& v = n.value(CurveShape::kLocal);
    const auto* d = std::get_if<scene::DataPtr>(&v);
    const void* data = d ? d->get() : nullptr;
    StrandEntry& e = cache[n.id()];
    if (e.cloud && e.data == data)
        return e;
    e.data = data;
    auto cloud = std::make_shared<render::StrandCloud>();
    const uint64_t signature = layoutSignature(splines);
    cloud->layout = e.cloud && signature == e.layoutSignature ? e.cloud->layout : strandLayout(splines);
    e.layoutSignature = signature;
    cloud->points.reserve(cloud->layout->points);
    glm::dvec3 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (const curve::Spline& sp : splines)
    {
        if (sp.size() < 2)
            continue;
        // GroomLab's strands carry width 1 until a Width node gives them widths in scene units.
        const auto wi = sp.pointAttrs.find("width");
        static const std::vector<double> none;
        const bool unset = wi == sp.pointAttrs.end() || std::all_of(wi->second.begin(), wi->second.end(), [](double x) { return x == 1.0; });
        const std::vector<double>& w = unset ? none : wi->second;
        for (size_t i = 0; i < sp.size(); ++i)
        {
            const glm::dvec3& p = sp.points[i];
            cloud->points.emplace_back(glm::vec3(p), i < w.size() ? float(w[i]) : kDefaultStrandWidth);
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
    }
    e.centre = 0.5 * (lo + hi);
    e.radius = 0.5 * glm::length(hi - lo);
    e.cloud = std::move(cloud);
    return e;
}

/** The strands for the renderer, thinned and widened for how big they are on screen. */
render::StrandDraw strandDraw(const scene::Node& n, const curve::Splines& splines, const glm::dmat4& world, const ViewInfo& view)
{
    const StrandEntry& e = strandEntry(n, splines);
    render::StrandDraw d;
    d.cloud = e.cloud;
    d.world = glm::mat4(world);
    const glm::dvec3 centre = xf(world, e.centre);
    const double scale = std::cbrt(std::abs(glm::determinant(glm::dmat3(world))));
    const double pixels = e.radius * scale / std::max(1e-9, view.unitsPerPixel(centre));
    // About a strand per two pixels of the groom's disc on screen; fewer drawn, each wider, so the coverage holds.
    const double wanted = 1.5 * pixels * pixels;
    d.keep = float(std::clamp(wanted / double(std::max<size_t>(1, e.cloud->layout->strands.size())), 0.02, 1.0));
    d.widthScale = std::min(1.0f / d.keep, 8.0f);
    return d;
}

void drawStrands(drawOverrides::DrawContext& c, const curve::Splines& splines)
{
    render::StrandDraw d = strandDraw(c.node, splines, c.world, c.view);
    if (c.highlight != drawOverrides::Highlight::None)
        d.tint = glm::vec3(c.color);
    c.frame.strands.push_back(std::move(d));
}

void drawCurve(drawOverrides::DrawContext& c)
{
    LILA_ZONE();

    const scene::Node& n = c.node;
    const curve::Splines& splines = curve::splinesOf(n.value(CurveShape::kLocal));
    bool edit = editing(n);
    if (!edit && strandCurves(n))
    {
        drawStrands(c, splines);
        return;
    }
    using HL = drawOverrides::Highlight;
    bool bold = c.highlight == HL::Selected || c.highlight == HL::Preselected;
    auto& halo = bold ? c.frame.curveHaloBold : c.frame.curveHalo;
    auto& body = bold ? c.frame.curveLinesBold : c.frame.curveLines;
    const glm::vec4 color = curveColor(c);
    // Fully constrained sketch geometry takes its own colour unless a highlight or layer colour says otherwise.
    const sketchUi::Display* sketch = color == themeLinear("curve", kCurve) ? sketchUi::display(n) : nullptr;
    const glm::vec4 fixedColor = sketchUi::constrainedColor();
    const std::vector<curve::Polyline>& lines = linesOf(n);
    for (size_t li = 0; li < lines.size(); ++li)
    {
        const curve::Polyline& pl = lines[li];
        size_t segs = pl.closed ? pl.size() : (pl.size() ? pl.size() - 1 : 0);
        for (size_t i = 0; i < segs; ++i)
        {
            glm::vec3 a(xf(c.world, pl.points[i])), b(xf(c.world, pl.points[(i + 1) % pl.size()]));
            const glm::vec4& col = sketch && sketchUi::spanFixed(*sketch, splines, li, pl.params[i]) ? fixedColor : color;
            halo.push_back({a, kHalo});
            halo.push_back({b, kHalo});
            body.push_back({a, col});
            body.push_back({b, col});
        }
    }
    for (const curve::Polyline& pl : mirrorLinesOf(n))
    {
        size_t segs = pl.closed ? pl.size() : (pl.size() ? pl.size() - 1 : 0);
        for (size_t i = 0; i < segs; ++i)
        {
            glm::vec3 a(pl.points[i]), b(pl.points[(i + 1) % pl.size()]);
            halo.push_back({a, kHalo});
            halo.push_back({b, kHalo});
            body.push_back({a, color});
            body.push_back({b, color});
        }
    }
    if (n.as<bool>(CurveShape::kDisplayComb))
    {
        double scale = n.as<double>(CurveShape::kCombScale);
        int samples = int(std::max<int64_t>(1, n.as<int64_t>(CurveShape::kCombSamples)));
        for (const curve::Spline& s : splines)
        {
            std::vector<curve::CombTooth> teeth = curve::curvatureComb(s, samples, scale);
            for (size_t i = 0; i < teeth.size(); ++i)
            {
                line(c.frame, xf(c.world, teeth[i].base), xf(c.world, teeth[i].tip), kComb, false);
                if (i + 1 < teeth.size())
                    line(c.frame, xf(c.world, teeth[i].tip), xf(c.world, teeth[i + 1].tip), kComb, false);
            }
        }
    }
    if (!edit)
        return;
    const auto& sel = scene::components::selected(n.id(), scene::ComponentMode::Vertex);
    std::set<uint32_t> picked(sel.begin(), sel.end());
    bool hull = n.as<bool>(CurveShape::kDisplayHull), points = n.as<bool>(CurveShape::kDisplayPoints);
    for (size_t si = 0; si < splines.size(); ++si)
    {
        const curve::Spline& s = splines[si];
        if (hull && s.kind == curve::Kind::Nurbs)
            for (size_t i = 0; i + 1 < s.size() + (s.closed ? 1 : 0); ++i)
                line(c.frame, xf(c.world, s.points[i]), xf(c.world, s.points[(i + 1) % s.size()]), kHull, true);
        if (hull && s.kind == curve::Kind::Bezier && s.handleL.size() == s.size())
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (s.closed || i > 0)
                    line(c.frame, xf(c.world, s.points[i]), xf(c.world, s.handleL[i]), kHull, true);
                if (s.closed || i + 1 < s.size())
                    line(c.frame, xf(c.world, s.points[i]), xf(c.world, s.handleR[i]), kHull, true);
            }
    }
    if (!points)
        return;
    const std::vector<std::pair<uint32_t, glm::dvec3>> positions = curve::componentPositions(splines);
    // Soft selection shades the points it will pull, as it does mesh vertices.
    std::map<uint32_t, float> softWeight;
    if (!picked.empty() && PrefStore::get().value<bool>("general", "/softSelect/enabled", false))
    {
        std::vector<glm::dvec3> worldPts;
        std::vector<char> on;
        std::vector<uint32_t> ids;
        for (auto& [id, p] : positions)
            if (curve::PointRole(id & 3) != curve::PointRole::Edit || picked.contains(id))
            {
                ids.push_back(id);
                worldPts.push_back(xf(c.world, p));
                on.push_back(picked.contains(id) ? 1 : 0);
            }
        const std::vector<float> w = scene::softWeights(worldPts, on);
        for (size_t k = 0; k < ids.size(); ++k)
            if (!on[k] && w[k] > 0.0f)
                softWeight[ids[k]] = w[k];
    }
    const bool grayWeights = viewportDisplay::grayscaleWeights();
    for (auto& [id, p] : positions)
    {
        size_t si, pi;
        curve::PointRole role;
        curve::decodeComponent(id, &si, &pi, &role);
        const curve::Spline& s = splines[si];
        // Open ends' outer handles shape nothing, so they aren't offered.
        if ((role == curve::PointRole::HandleL && !s.closed && pi == 0) || (role == curve::PointRole::HandleR && !s.closed && pi + 1 == s.size()))
            continue;
        auto q = c.view.project(xf(c.world, p));
        if (!q)
            continue;
        bool on = picked.contains(id);
        const auto soft = softWeight.find(id);
        const glm::vec4 fill = on ? kPointSelected : soft != softWeight.end() ? softFill(soft->second, grayWeights) : kPoint;
        switch (role)
        {
        case curve::PointRole::Point:
            c.painter.disc(*q, on ? 5.0f : 4.5f, kPointRim, 16);
            c.painter.disc(*q, on ? 3.5f : 3.0f, fill, 16);
            break;
        case curve::PointRole::HandleL:
        case curve::PointRole::HandleR:
            c.painter.rect(QRectF(*q - QPointF(3.5, 3.5), QSizeF(7, 7)), kPointRim);
            c.painter.rect(QRectF(*q - QPointF(2.0, 2.0), QSizeF(4, 4)), fill);
            break;
        case curve::PointRole::Edit:
            c.painter.line(*q - QPointF(4.5, 4.5), *q + QPointF(4.5, 4.5), on ? 4.5f : 3.5f, kPointRim);
            c.painter.line(*q - QPointF(4.5, -4.5), *q + QPointF(4.5, -4.5), on ? 4.5f : 3.5f, kPointRim);
            c.painter.line(*q - QPointF(4, 4), *q + QPointF(4, 4), on ? 2.5f : 1.5f, fill);
            c.painter.line(*q - QPointF(4, -4), *q + QPointF(4, -4), on ? 2.5f : 1.5f, fill);
            break;
        }
    }
}

std::optional<double> pickCurve(const drawOverrides::PickContext& c)
{
    QRectF box = QRectF(c.a, c.b).normalized();
    std::optional<double> best;
    for (const curve::Polyline& pl : linesOf(c.node))
    {
        size_t segs = pl.closed ? pl.size() : (pl.size() ? pl.size() - 1 : 0);
        for (size_t i = 0; i < segs; ++i)
        {
            auto p0 = c.view.project(xf(c.world, pl.points[i])), p1 = c.view.project(xf(c.world, pl.points[(i + 1) % pl.size()]));
            if (!p0 || !p1)
                continue;
            if (!c.click)
            {
                if (box.contains(*p0) || box.contains(*p1))
                    return 0.0;
                continue;
            }
            QPointF d = *p1 - *p0;
            double len2 = QPointF::dotProduct(d, d);
            double t = len2 > 0 ? std::clamp(QPointF::dotProduct(c.a - *p0, d) / len2, 0.0, 1.0) : 0.0;
            QPointF q = *p0 + d * t - c.a;
            double dist = std::hypot(q.x(), q.y());
            if (!best || dist < *best)
                best = dist;
        }
    }
    return best;
}

void addCurveCommand(std::string id, std::string label, const char* command, json call, std::string doc)
{
    std::string py = ops::toPython(command, call);
    actions::add({.id = std::move(id), .label = std::move(label), .category = "Curves", .doc = std::move(doc), .python = py, .command = command,
                  .call = std::move(call)});
}

/** The symmetry of the last selected curve, as the curveSymmetry axis names it, or empty with none selected. */
std::string selectedCurveSymmetry()
{
    scene::Graph& g = scene::Graph::current();
    for (auto it = g.selection().rbegin(); it != g.selection().rend(); ++it)
        if (scene::Node* n = g.node(*it); n && n->type().isA("curve"))
        {
            const int axis = curve::symmetryAxis(*n);
            return axis < 0 ? "off" : axis == 0 ? "x" : axis == 1 ? "y" : "z";
        }
    return {};
}

void registerSymmetryActions()
{
    for (const char* axis : {"off", "x", "y", "z"})
    {
        const std::string id = std::string("curve.symmetry") + char(std::toupper(axis[0])) + (axis + 1);
        actions::add({.id = id,
                      .label = std::string(axis) == "off" ? "Symmetry Off" : std::string("Symmetry ") + char(std::toupper(axis[0])),
                      .category = "Curves",
                      .doc = std::string(axis) == "off" ? "Stops mirroring the selected curves."
                                                        : std::string("Mirrors the selected curves live across the world plane facing ") + axis +
                                                              ": the copy follows every edit, and pipes and sweeps built from them get both sides.",
                      .python = ops::toPython("curveSymmetry", {{"axis", axis}}),
                      .command = "curveSymmetry",
                      .call = {{"axis", axis}},
                      .checked = [a = std::string(axis)] { return selectedCurveSymmetry() == a; }});
        actions::setEnabledWhen(id, [] { return !selectedCurveSymmetry().empty(); });
        const int pref = std::string(axis) == "off" ? 0 : axis[0] - 'x' + 1;
        actions::add({.id = std::string("curve.drawSymmetry") + char(std::toupper(axis[0])) + (axis + 1),
                      .label = std::string(axis) == "off" ? "Draw Without Symmetry" : std::string("Draw with Symmetry ") + char(std::toupper(axis[0])),
                      .category = "Curves",
                      .run = [pref] { curveTools::prefs().set("general", "/curveTools/symmetry", pref); },
                      .doc = "Curves the draw tools make (EZ, pen, CV, EP, pencil, Catmull-Rom, kappa, pipe) are mirrored this way; Y in a draw tool cycles it.",
                      .python = "lila.prefs.set(\"general\", \"/curveTools/symmetry\", " + std::to_string(pref) + ")",
                      .checked = [pref] { return curveTools::symmetryPref() == pref; }});
    }
    addCurveCommand("curve.bakeSymmetry", "Bake Symmetry", "bakeCurveSymmetry", json::object(),
        "Turns the selected curves' symmetry copies into curves of their own, so each side can be edited apart.");
    actions::setEnabledWhen("curve.bakeSymmetry", [] {
        const std::string s = selectedCurveSymmetry();
        return !s.empty() && s != "off";
    });
}

void registerCurveActions()
{
    registerSymmetryActions();
    addCurveCommand("curve.circle", "Circle", "circle", json::object(), "Creates an exact circle (a rational NURBS).");
    addCurveCommand("curve.fromEdges", "Edges to Curve", "polyEdgeToCurve", json::object(),
        "Makes a curve from each chain of the selected mesh edges, or the borders of the selected faces.");
    addCurveCommand("curve.extrude", "Sweep", "extrude", json::object(),
        "Sweeps a circle, rectangle or a second selected curve along the first selected curve into quads, live.");
    addCurveCommand("curve.revolve", "Revolve", "revolve", json::object(), "Revolves the selected profile curve around an axis into quads, live.");
    addCurveCommand("curve.loft", "Loft", "loft", json::object(), "Lofts quads through the selected curves in selection order, live.");
    addCurveCommand("curve.patchFill", "Patch Fill", "patchFill", json::object(), "Fills four selected boundary curves with a quad patch, live.");
    addCurveCommand("curve.fillet", "Fillet Corners", "filletCurve", json::object(), "Rounds, chamfers or G2-blends the selected curves' corners.");
    addCurveCommand("curve.offset", "Offset", "offsetCurve", json::object(), "Offsets the selected curves within their plane into new curves.");
    addCurveCommand("curve.rebuild", "Rebuild", "rebuildCurve", json::object(), "Rebuilds the selected curves with even spans, corners kept.");
    addCurveCommand("curve.fit", "Fit", "fitCurve", json::object(), "Refits the selected curves as Beziers with as few points as the tolerance allows.");
    actions::add({.id = "curve.project",
                  .label = "Project onto Mesh",
                  .category = "Curves",
                  .run =
                      [] {
                          scene::Graph& g = scene::Graph::current();
                          json curves = json::array();
                          std::string mesh;
                          for (scene::NodeId id : g.selection())
                              if (scene::Node* n = g.node(id))
                              {
                                  if (n->type().isA("curve"))
                                      curves.push_back(n->path());
                                  else if (n->type().isA("mesh"))
                                      mesh = n->path();
                              }
                          if (curves.empty() || mesh.empty())
                          {
                              log::error("Project onto Mesh: select curves and the mesh to project onto");
                              return;
                          }
                          actions::runWithOptions("projectCurve", {{"args", curves}, {"mesh", mesh}});
                      },
                  .doc = "Projects the selected curves onto the selected mesh (closest point, or along the direction in the option box), live.",
                  .python = "lila.ops.projectCurve(\"Curve\", mesh=\"Mesh\")",
                  .command = "projectCurve"});
    addCurveCommand("curve.keepOnSurface", "Keep on Surface", "conformCurve", json::object(),
        "Keeps the selected curves on the selected mesh: their points stay on it however they or the mesh move (Make Live).");
    addCurveCommand("curve.releaseFromSurface", "Release from Surface", "conformCurve", {{"release", true}},
        "Lets the selected curves off the surface they were kept on, leaving them where they are.");
    addCurveCommand("curve.sendToUnreal", "Send Curve to Unreal", "unrealSendCurve", json::object(),
        "Sends the selected curves to the connected Unreal editor as spline actors (or landscape splines), resent on every save.");
    addCurveCommand("curve.attach", "Attach", "attachCurve", json::object(), "Joins the second selected curve onto the first at their nearest ends.");
    addCurveCommand("curve.detach", "Detach", "detachCurve", json::object(), "Splits the curve at the selected points.");
    addCurveCommand("curve.reverse", "Reverse Direction", "reverseCurve", json::object(), "Reverses the selected curves' direction.");
    addCurveCommand("curve.controls", "Curve Controls", "curveControls", json::object(),
        "Builds a control on each point of the selected curves (and on Bezier tangents, if asked) that the curve follows live.");
    addCurveCommand("curve.removeControls", "Remove Curve Controls", "removeCurveControls", json::object(),
        "Deletes the selected curves' controls, keeping the shape they gave them.");
    addCurveCommand("curve.close", "Close", "closeCurve", json::object(), "Closes the selected open curves.");
    addCurveCommand("curve.open", "Open", "openCurve", json::object(), "Opens the selected closed curves at their start.");
    addCurveCommand("curve.deletePoints", "Delete Points", "deleteCurvePoints", json::object(), "Deletes the selected curve points.");
    for (const char* t : {"auto", "vector", "aligned", "free"})
    {
        std::string label = std::string(1, char(std::toupper(t[0]))) + (t + 1);
        addCurveCommand(std::string("curve.handle") + label, label + " Handles", "setCurveHandleType", {{"type", t}},
            "Sets the selected Bezier points' handles to " + std::string(t) + ".");
    }
    addCurveCommand("curve.bend", "Bend", "bendCurve", json::object(), "Bends the selected curves (or selected points) around an axis by an angle.");
    addCurveCommand("curve.curl", "Curl", "curlCurve", json::object(), "Coils the selected curves around themselves.");
    addCurveCommand("curve.scaleCurvature", "Scale Curvature", "scaleCurvature", json::object(),
        "Flattens (below 1) or exaggerates (above 1) the selected curves' bends, keeping their length.");
    addCurveCommand("curve.smooth", "Smooth", "smoothCurve", json::object(), "Smooths the selected curves, or only the selected points.");
    addCurveCommand("curve.evenSpacing", "Even Spacing", "rebuildCurve", {{"keepPointCount", true}},
        "Spreads the selected curves' points evenly along them, keeping their kind, point count and corners.");
    addCurveCommand("curve.deformAlongCurve", "Deform Along Curve", "deformAlongCurve", json::object(),
        "Bends the selected meshes along the selected curve with a live modifier.");
    addCurveCommand("curve.arrayAlongCurve", "Array Along Curve", "arrayAlongCurve", json::object(),
        "Repeats the selected meshes along the selected curve with a live modifier.");
    addCurveCommand("curve.instanceAlongCurve", "Instance Along Curve", "arrayAlongCurve", {{"output", "instances"}},
        "Places instances of the selected meshes along the selected curve, under a group.");
    addCurveCommand("curve.flowAlongSurface", "Flow Along Surface", "flowAlongSurface", json::object(),
        "Flows the selected meshes onto the surface selected last (its UVs, or its selected quad patch), live.");
    addCurveCommand("curve.toBezier", "Convert to Bezier", "convertCurve", {{"type", "bezier"}}, "Converts the selected curves to Beziers.");
    addCurveCommand("curve.toNurbs", "Convert to NURBS", "convertCurve", {{"type", "nurbs"}}, "Converts the selected curves to NURBS (exact).");
    addCurveCommand("curve.toPoly", "Convert to Poly", "convertCurve", {{"type", "poly"}}, "Converts the selected curves to poly lines.");
    actions::add({.id = "curve.toggleComb",
                  .label = "Curvature Comb",
                  .category = "Curves",
                  .run =
                      [] {
                          scene::Graph& g = scene::Graph::current();
                          for (scene::NodeId id : g.selection())
                              if (scene::Node* n = g.node(id); n && n->type().isA("curve"))
                                  runUiWithOptions("setAttr", {{"args", {n->path() + ".displayComb", !n->as<bool>(CurveShape::kDisplayComb)}}});
                      },
                  .doc = "Shows or hides the selected curves' curvature combs.",
                  .python = "lila.ops.setAttr(\"Curve.displayComb\", True)"});
}
} // namespace

bool drawStrandsFor(render::FrameState& frame, const scene::Node& n, const glm::dmat4& world, const ViewInfo& view)
{
    if (!strandCurves(n))
        return false;
    const curve::Splines& splines = curve::splinesOf(n.value(CurveShape::kLocal));
    frame.strands.push_back(strandDraw(n, splines, world, view));
    return true;
}

void registerCurveUi()
{
    if (!drawOverrides::registry().find("lila.curve"))
        drawOverrides::add({.id = "lila.curve", .nodeType = "curve", .draw = drawCurve, .pick = pickCurve});
    registerCurveActions();
    registerCurveTools();
    registerEzCurveTool();
    registerCurvePointTool();
    registerPipeTool();
    registerCableTool();
    registerPipeLevelTools();
    registerSectionTool();
    registerStrapTool();
    registerVehicleCurveUi();
    registerSketchTool();
    registerCurveSculptTool();
    registerGroomUi();
    registerPlanarCurveUi();
}
} // namespace lila::ui
