// World Remix in the UI: Level > Open World, the Level Toolkit's Open World section, the viewport overlay (sources,
// regions where they were and where they go, seam bands, pins, cut road ends and rivers) and the edit mode that drags
// regions and pins.

#include "OpenWorld.h"
#include "RemixNode.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Node.h>
#include <lila/Transform.h>
#include <lila/Viewport.h>

#include <QMouseEvent>

#include <cmath>
#include <optional>

using namespace lila;
using namespace lila::ui;

namespace openworld
{
namespace
{
/** Edit mode: region and pin handles take left drags. */
bool gEdit = false;

/** A drag in progress: the remix, what is dragged and how far. */
struct Drag
{
    uint64_t modifier = 0;
    std::string region;
    std::string pin;
    bool rotate = false;
    Vec2 grab{0.0};
    double height = 0.0;
    Vec2 delta{0.0};
    double angle = 0.0;
    Vec2 centre{0.0};
    double startAngle = 0.0;
};
std::optional<Drag> gDrag;
/** The handle under the cursor in Edit mode ("region:<id>" or "pin:<id>"), drawn larger. */
std::string gHover;

const glm::vec4 kBright{0.95f, 0.95f, 0.95f, 0.95f};
const glm::vec4 kDim{0.75f, 0.75f, 0.75f, 0.55f};
const glm::vec4 kFaint{0.7f, 0.7f, 0.7f, 0.3f};
const glm::vec4 kWarn{1.0f, 0.62f, 0.2f, 1.0f};

struct Placed
{
    uint64_t modifier = 0;
    Published p;
    glm::dmat4 world{1.0};
    std::string mesh;
};

std::vector<Placed> remixes()
{
    std::vector<Placed> out;
    scene::Graph& g = scene::Graph::current();
    for (auto& [id, p] : publishedRemixes())
    {
        scene::Node* mod = g.node(scene::NodeId(id));
        if (!mod || !p.result)
            continue;
        Placed r{id, p, glm::dmat4(1.0), {}};
        const scene::NodeId mesh = scene::modifiers::meshOf(g, mod->id());
        if (scene::Node* m = mesh != scene::kNoNode ? g.node(mesh) : nullptr)
        {
            r.world = m->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            r.mesh = m->path();
        }
        out.push_back(std::move(r));
    }
    return out;
}

/** The remix the selection names, else the only one. */
std::optional<Placed> current()
{
    std::vector<Placed> all = remixes();
    scene::Graph& g = scene::Graph::current();
    for (scene::NodeId id : g.selection())
        for (Placed& r : all)
            if (scene::Node* n = g.node(id); n && (n->path() == r.mesh || id == scene::NodeId(r.modifier)))
                return r;
    if (all.size() == 1)
        return all[0];
    return std::nullopt;
}

glm::dvec3 worldOf(const Placed& r, Vec2 p, double lift = 0.0)
{
    const Heightfield& h = r.p.before ? r.p.result->before : r.p.result->after;
    return glm::dvec3(r.world * glm::dvec4(p.x, h.heightAt(p.x, p.y) + lift, p.y, 1.0));
}

std::vector<QPointF> project(const Placed& r, const Polygon& poly, const ViewInfo& view, double step)
{
    std::vector<QPointF> out;
    for (size_t i = 0; i < poly.size(); ++i)
    {
        const Vec2 a = poly[i], b = poly[(i + 1) % poly.size()];
        const int n = std::clamp(int(glm::length(b - a) / std::max(step, 1.0)), 1, 64);
        for (int k = 0; k < n; ++k)
            if (auto s = view.project(worldOf(r, a + (b - a) * (double(k) / n), 100.0)))
                out.push_back(*s);
    }
    return out;
}

/** The outline pushed out by d along each corner's bisector: the seam band's edge. */
Polygon offsetOutline(const Polygon& poly, double d)
{
    Polygon out;
    const double sign = area(poly) > 0.0 ? -1.0 : 1.0;
    for (size_t i = 0; i < poly.size(); ++i)
    {
        const Vec2 prev = poly[(i + poly.size() - 1) % poly.size()], p = poly[i], next = poly[(i + 1) % poly.size()];
        auto normal = [&](Vec2 a, Vec2 b) {
            const Vec2 t = glm::normalize(b - a);
            return Vec2(t.y, -t.x) * sign;
        };
        Vec2 n = normal(prev, p) + normal(p, next);
        n = glm::length(n) > 1e-9 ? glm::normalize(n) : normal(p, next);
        out.push_back(p + n * d);
    }
    return out;
}

Region dragged(const Region& r)
{
    Region out = r;
    if (gDrag && gDrag->region == r.id)
    {
        out.translate += gDrag->delta;
        out.angle += gDrag->angle;
    }
    return out;
}

void drawRemix(OverlayPainter& p, const ViewInfo& view, const Placed& r)
{
    const RemixSpec& spec = r.p.spec;
    const RemixResult& res = *r.p.result;
    const double step = res.after.cell * 4.0;

    for (const Polygon& f : res.footprints)
        if (!f.empty())
            p.dashedPolyline(project(r, f, view, step), 1.0f, 8.0f, kFaint, true);

    for (const Region& reg0 : spec.regions)
    {
        const Region reg = dragged(reg0);
        const Polygon target = reg.target();
        const bool gone = reg.op == RegionOp::Delete || reg.op == RegionOp::Sea;
        const bool moved = !(reg.translate == Vec2(0.0) && reg.angle == 0.0) && !stays(reg.op);
        if (moved || gone)
            p.dashedPolyline(project(r, reg.polygon, view, step), 1.5f, 6.0f, kDim, true);
        if (r.p.before)
            continue;
        if (!gone)
            p.polyline(project(r, target, view, step), 2.0f, kBright, true);
        if (pastes(reg.op) || punches(reg.op))
        {
            const double band = spec.seamOf(reg);
            p.dashedPolyline(project(r, offsetOutline(target, band), view, step), 1.0f, 3.0f, kFaint, true);
            p.dashedPolyline(project(r, offsetOutline(target, -band), view, step), 1.0f, 3.0f, kFaint, true);
        }
        const Vec2 from = centroid(reg.polygon), to = centroid(target);
        auto a = view.project(worldOf(r, from, 100.0));
        auto b = view.project(worldOf(r, to, 100.0));
        if (moved && a && b)
            p.dashedLine(*a, *b, 1.5f, 5.0f, kDim);
        if (b)
        {
            const bool handle = gEdit && (reg.op == RegionOp::Cut || reg.op == RegionOp::Move || reg.op == RegionOp::Duplicate);
            if (handle)
                p.smoothDisc(*b, (gDrag && gDrag->region == reg.id) || gHover == "region:" + reg.id ? 7.0 : 5.0, kBright);
            p.label(*b + QPointF(9, -9), QString::fromStdString(reg.id + ": " + regionOpNames()[size_t(reg.op)]));
        }
    }

    for (const Pin& pin : spec.pins)
    {
        Vec2 to = pin.to;
        if (gDrag && gDrag->pin == pin.id)
            to += gDrag->delta;
        auto a = view.project(worldOf(r, pin.at, 100.0));
        auto b = view.project(worldOf(r, r.p.before ? pin.at : to, 100.0));
        if (!b)
            continue;
        if (a && glm::length(to - pin.at) > 1e-6 && !r.p.before)
            p.line(*a, *b, 1.5f, kDim);
        p.smoothDisc(*b, (gDrag && gDrag->pin == pin.id) || (gEdit && gHover == "pin:" + pin.id) ? 6.0 : 4.0, kBright);
        if (glm::length(to - pin.at) < 1e-6)
            p.arc(*b, 8.0, 0.0, 360.0, 1.5f, kBright);
    }

    if (r.p.before)
        return;
    for (const RemixResult::Line& l : res.lines)
    {
        if (!l.added)
            continue;
        std::vector<QPointF> pts;
        for (Vec2 v : l.points)
            if (auto s = view.project(worldOf(r, v, 100.0)))
                pts.push_back(*s);
        p.polyline(pts, 2.0f, kDim);
    }
    for (const CutEnd& e : res.roadEnds)
        if (auto s = view.project(worldOf(r, e.at, 100.0)))
            p.smoothDisc(*s, e.joined ? 2.5 : 5.0, e.joined ? kDim : kWarn);
    for (const CutEnd& e : res.coastEnds)
        if (!e.joined)
            if (auto s = view.project(worldOf(r, e.at, 100.0)))
                p.smoothDisc(*s, 5.0, kWarn);
    for (const RiverState& s : res.rivers)
        if (!s.drains && s.drainedBefore)
            if (auto q = view.project(worldOf(r, s.end, 100.0)))
            {
                p.arc(*q, 7.0, 0.0, 360.0, 2.0f, kWarn);
                p.label(*q + QPointF(10, 4), "No drain");
            }
}

void drawAll(render::FrameState&, OverlayPainter& p, const ViewInfo& view)
{
    for (const Placed& r : remixes())
    {
        if (!r.p.spec.view.value("overlay", true))
            continue;
        drawRemix(p, view, r);
        if (gEdit || r.p.before)
        {
            const QString mode = QString::fromStdString(std::string(r.p.before ? "Before" : "After") + (gEdit ? ", Edit" : ""));
            p.label(QPointF(12, 44), "Remix  " + mode);
        }
    }
}

/** Where a pixel's ray meets the horizontal plane at height (world Y), on the remix's map plane. */
std::optional<Vec2> mapAt(const Placed& r, QPointF px, const ViewInfo& view, double height)
{
    glm::dvec3 o, d;
    view.ray(px, o, d);
    const glm::dmat4 inv = glm::inverse(r.world);
    const glm::dvec3 lo = glm::dvec3(inv * glm::dvec4(o, 1.0)), ld = glm::dvec3(inv * glm::dvec4(d, 0.0));
    if (std::abs(ld.y) < 1e-9)
        return std::nullopt;
    const double t = (height - lo.y) / ld.y;
    if (t <= 0.0)
        return std::nullopt;
    const glm::dvec3 hit = lo + ld * t;
    return Vec2(hit.x, hit.z);
}

double angleOf(Vec2 v) { return std::atan2(-v.y, v.x) * 57.29577951308232; }

/** The region or pin handle nearest px within 12 pixels, as the drag it would start. */
std::optional<Drag> handleAt(QPointF px, const ViewInfo& view, bool rotate)
{
    double best = 12.0;
    std::optional<Drag> hit;
    for (const Placed& r : remixes())
    {
        if (r.p.before || !r.p.spec.view.value("overlay", true))
            continue;
        const Heightfield& h = r.p.result->after;
        for (const Region& reg : r.p.spec.regions)
        {
            if (!(reg.op == RegionOp::Cut || reg.op == RegionOp::Move || reg.op == RegionOp::Duplicate))
                continue;
            const Vec2 c = centroid(reg.target());
            if (auto s = view.project(worldOf(r, c, 100.0)); s && std::hypot(s->x() - px.x(), s->y() - px.y()) < best)
            {
                best = std::hypot(s->x() - px.x(), s->y() - px.y());
                hit = Drag{r.modifier, reg.id, {}, rotate, c, h.heightAt(c.x, c.y) + 100.0, Vec2(0.0), 0.0, c, 0.0};
            }
        }
        for (const Pin& pin : r.p.spec.pins)
            if (auto s = view.project(worldOf(r, pin.to, 100.0)); s && std::hypot(s->x() - px.x(), s->y() - px.y()) < best)
            {
                best = std::hypot(s->x() - px.x(), s->y() - px.y());
                hit = Drag{r.modifier, {}, pin.id, false, pin.to, h.heightAt(pin.to.x, pin.to.y) + 100.0, Vec2(0.0), 0.0, pin.to, 0.0};
            }
    }
    return hit;
}

bool hoverDrag(QPointF px, const ViewInfo& view)
{
    std::string now;
    if (gEdit)
        if (const std::optional<Drag> h = handleAt(px, view, false))
            now = h->region.empty() ? "pin:" + h->pin : "region:" + h->region;
    if (now == gHover)
        return false;
    gHover = now;
    return true;
}

bool pressDrag(QMouseEvent* e, const ViewInfo& view)
{
    if (!gEdit || e->button() != Qt::LeftButton || (e->modifiers() & (Qt::AltModifier | Qt::ShiftModifier)))
        return false;
    const QPointF px = e->position();
    std::optional<Drag> hit = handleAt(px, view, bool(e->modifiers() & Qt::ControlModifier));
    if (!hit)
        return false;
    for (const Placed& r : remixes())
        if (r.modifier == hit->modifier)
            if (auto at = mapAt(r, px, view, hit->height))
            {
                hit->grab = *at;
                hit->startAngle = angleOf(*at - hit->centre);
            }
    gDrag = hit;
    return true;
}

void moveDrag(QMouseEvent* e, const ViewInfo& view)
{
    if (!gDrag)
        return;
    for (const Placed& r : remixes())
        if (r.modifier == gDrag->modifier)
            if (auto at = mapAt(r, e->position(), view, gDrag->height))
            {
                if (gDrag->rotate)
                    gDrag->angle = angleOf(*at - gDrag->centre) - gDrag->startAngle;
                else
                    gDrag->delta = *at - gDrag->grab;
            }
}

void releaseDrag(QMouseEvent*, const ViewInfo&)
{
    if (!gDrag)
        return;
    const Drag d = *gDrag;
    gDrag.reset();
    scene::Graph& g = scene::Graph::current();
    scene::Node* mod = g.node(scene::NodeId(d.modifier));
    Published p;
    if (!mod || !publishedRemix(d.modifier, &p))
        return;
    try
    {
        if (!d.region.empty())
        {
            const Region* r = p.spec.region(d.region);
            if (!r || (d.delta == Vec2(0.0) && d.angle == 0.0))
                return;
            const Vec2 t = r->translate + d.delta;
            ops::execute("remixAddRegion", {{"args", {mod->path()}}, {"id", d.region}, {"translate", {t.x, t.y}}, {"angle", r->angle + d.angle}}, CallSource::UI);
        }
        else
        {
            for (const Pin& pin : p.spec.pins)
                if (pin.id == d.pin && d.delta != Vec2(0.0))
                {
                    const Vec2 to = pin.to + d.delta;
                    ops::execute("remixPin", {{"args", {mod->path()}}, {"id", d.pin}, {"to", {to.x, to.y}}}, CallSource::UI);
                }
        }
    }
    catch (const std::exception& e)
    {
        log::warn("{}", e.what());
    }
}

void warnOnThrow(const std::function<void()>& f)
{
    try
    {
        f();
    }
    catch (const std::exception& e)
    {
        log::write(LogLevel::Warning, e.what());
    }
}

std::string remixName()
{
    const std::optional<Placed> r = current();
    if (!r)
        throw std::runtime_error("select a remix (make one with Create Remix)");
    return r->mesh;
}

const std::vector<std::string> kKeywords = {"remix", "real world", "osm", "dem", "warp", "relocate", "compose map", "world composer", "gis"};
} // namespace

void registerRemixUi(ext::Context& ctx)
{
    auto add = [&](Action a) {
        a.keywords.insert(a.keywords.end(), kKeywords.begin(), kKeywords.end());
        actions::add(ctx, std::move(a));
    };
    add({.id = "openworld.remixCreate",
         .label = "Create Remix",
         .category = "Open World",
         .doc = "Make a World Remix: real places composed onto one map as a live terrain.",
         .python = "remixCreate(name='World', sources=[...])",
         .command = "remixCreate",
         .call = nlohmann::json::object(),
         .icon = "world"});
    add({.id = "openworld.remixSource",
         .label = "Add Source",
         .category = "Open World",
         .run = [] { warnOnThrow([] { actions::runWithOptions("remixSource", {{"args", {remixName()}}}); }); },
         .doc = "Add a real place to the selected remix: elevation, OSM and land cover files, or a bbox to fetch.",
         .python = "remixSource('World', id='coast', dem='dem/coast.tif', osm='osm/coast.json')",
         .icon = "map-plus"});
    add({.id = "openworld.remixRegion",
         .label = "Add Region",
         .category = "Open World",
         .run =
             [] {
                 warnOnThrow([] {
                     nlohmann::json call = {{"args", {remixName()}}};
                     scene::Graph& g = scene::Graph::current();
                     for (scene::NodeId id : g.selection())
                         if (scene::Node* n = g.node(id); n && n->type().isA("curve"))
                             call["curve"] = n->path();
                     actions::runWithOptions("remixAddRegion", call);
                 });
             },
         .doc = "Turn the selected closed curve into a remix region: cut, move, keep, duplicate or delete.",
         .python = "remixAddRegion('World', curve='Outline', op='cut', translate=[-100000, 0])",
         .icon = "polygon"});
    add({.id = "openworld.remixPin",
         .label = "Add Pin",
         .category = "Open World",
         .run = [] { warnOnThrow([] { actions::runWithOptions("remixPin", {{"args", {remixName()}}}); }); },
         .doc = "Pin a point of the base map, still or moved; the land around follows without folding.",
         .python = "remixPin('World', at=[0, 0], to=[-20000, 0])",
         .icon = "pin"});
    add({.id = "openworld.remixEdit",
         .label = "Edit Remix",
         .category = "Open World",
         .run = [] { gEdit = !gEdit; },
         .doc = "Drag region and pin handles in the viewport: drag moves, Ctrl+drag turns a region.",
         .checked = [] { return gEdit; },
         .icon = "drag-drop"});
    add({.id = "openworld.remixBefore",
         .label = "Before Remix",
         .category = "Open World",
         .run =
             [] {
                 warnOnThrow([] {
                     const std::optional<Placed> r = current();
                     if (r)
                         ops::execute("remixView", {{"args", {r->mesh}}, {"before", !r->p.before}}, CallSource::UI);
                 });
             },
         .doc = "Show the base map, sources placed with nothing moved, to compare with the remix.",
         .checked = [] {
             const std::optional<Placed> r = current();
             return r && r->p.before;
         },
         .icon = "arrows-diff"});
    add({.id = "openworld.remixOverlay",
         .label = "Remix Overlay",
         .category = "Open World",
         .run =
             [] {
                 warnOnThrow([] {
                     const std::optional<Placed> r = current();
                     if (r)
                         ops::execute("remixView", {{"args", {r->mesh}}, {"overlay", !r->p.spec.view.value("overlay", true)}}, CallSource::UI);
                 });
             },
         .doc = "Show regions, pins, seams, cut road ends and rivers over the remix.",
         .checked = [] {
             const std::optional<Placed> r = current();
             return r && r->p.spec.view.value("overlay", true);
         },
         .icon = "layers-intersect"});
    add({.id = "openworld.remixApply",
         .label = "Apply Remix",
         .category = "Open World",
         .run = [] { warnOnThrow([] { actions::runWithOptions("remixApply", {{"args", {remixName()}}}); }); },
         .doc = "Write the remixed OSM, heights and spec, and put the features on the terrain.",
         .python = "remixApply('World')",
         .icon = "check"});
    add({.id = "openworld.remixCheck",
         .label = "Check Remix",
         .category = "Open World",
         .run =
             [] {
                 warnOnThrow([] {
                     const nlohmann::json c = ops::execute("remixCheck", {{"args", {remixName()}}}, CallSource::UI);
                     if (c.value("ok", false))
                         log::info("remixCheck: no issues");
                     else
                         for (const nlohmann::json& i : c["issues"])
                             log::warn("remixCheck: {} at [{:.0f}, {:.0f}]: {}", i["kind"].get<std::string>(), i["at"][0].get<double>(),
                                       i["at"][1].get<double>(), i["what"].get<std::string>());
                 });
             },
         .doc = "List what the remix broke: rivers that no longer drain, dangling roads, overlapping regions, folds.",
         .python = "remixCheck('World')",
         .icon = "list-check"});

    addTools(ctx,
             {"openworld.remixCreate", "openworld.remixSource", "openworld.remixRegion", "openworld.remixPin", "", "openworld.remixEdit",
              "openworld.remixBefore", "openworld.remixOverlay", "", "openworld.remixApply", "openworld.remixCheck"},
             {{.action = "openworld.remixCreate", .icon = "world", .text = "Create", .shelf = "Remix", .group = "World Remix"},
              {.action = "openworld.remixRegion", .icon = "polygon", .text = "Region", .group = "World Remix"},
              {.action = "openworld.remixEdit", .icon = "drag-drop", .text = "Edit", .group = "World Remix"},
              {.action = "openworld.remixApply", .icon = "check", .text = "Apply", .group = "World Remix"},
              {.action = "openworld.remixCheck", .icon = "list-check", .text = "Check", .group = "World Remix"},
              {.action = "openworld.remixBefore", .icon = "arrows-diff", .text = "Before", .group = "World Remix"},
              {.action = "openworld.remixSource", .text = "Add Source", .rare = true},
              {.action = "openworld.remixPin", .text = "Add Pin", .rare = true},
              {.action = "openworld.remixOverlay", .text = "Overlay", .rare = true}});

    viewportHooks::addDrawer(ctx, "openworld.remix", drawAll);
    viewOverlays::add(ctx, {.id = "openworld.remix",
                            .label = "World Remix",
                            .doc = "A world remix's sources, regions, seams, pins and repairs, for remixes whose Remix Overlay is on.",
                            .drawers = {"openworld.remix"},
                            .keywords = {"remix", "regions", "seams"}});
    viewportHooks::setDragHook("openworld.remix", {.press = pressDrag, .move = moveDrag, .release = releaseDrag, .hover = hoverDrag});
    ctx.track("dragHook:openworld.remix", [] { viewportHooks::removeDragHook("openworld.remix"); });
}
} // namespace openworld
