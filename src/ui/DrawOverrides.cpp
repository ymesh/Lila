#include "ui/DrawOverrides.h"

#include "core/Log.h"
#include "geo/Lasso.h"
#include "scene/Transform.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace lila::ui::drawOverrides
{
namespace
{
/** A locator's three axis lines. */
std::vector<std::pair<glm::dvec3, glm::dvec3>> locatorLines(const glm::dmat4& world, const glm::dvec3& size)
{
    std::vector<std::pair<glm::dvec3, glm::dvec3>> out;
    for (int axis = 0; axis < 3; ++axis)
    {
        glm::dvec4 d(0.0);
        d[axis] = size[axis];
        out.emplace_back(glm::dvec3(world * (glm::dvec4(0, 0, 0, 1) + d)), glm::dvec3(world * (glm::dvec4(0, 0, 0, 1) - d)));
    }
    return out;
}

glm::dvec3 locatorSize(const scene::Node& n)
{
    return n.as<glm::dvec3>(scene::Locator::kLocalScale) * n.as<double>(scene::Locator::kDrawSize);
}

void drawLocator(DrawContext& c)
{
    for (auto& [a, b] : locatorLines(c.world, locatorSize(c.node)))
    {
        c.frame.lines.push_back({glm::dvec3(a), c.color});
        c.frame.lines.push_back({glm::dvec3(b), c.color});
    }
}

std::optional<double> pickLocator(const PickContext& c)
{
    QRectF box = QRectF(c.a, c.b).normalized();
    std::optional<double> best;
    for (auto& [a, b] : locatorLines(c.world, locatorSize(c.node)))
    {
        auto p0 = c.view.project(a), p1 = c.view.project(b);
        if (!p0 || !p1)
            continue;
        if (!c.click)
        {
            if (geo::lasso::segmentTouchesBox({p0->x(), p0->y()}, {p1->x(), p1->y()}, {box.left(), box.top()}, {box.right(), box.bottom()}))
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
    return best;
}

std::map<std::string, const DrawOverride*, std::less<>>& byType()
{
    static std::map<std::string, const DrawOverride*, std::less<>> m;
    return m;
}

uint64_t gVersion = 1;

void rebuildByType(const Registry<DrawOverride>& r)
{
    ++gVersion;
    auto& m = byType();
    m.clear();
    // Ids in order, so of two overrides for one type the first id wins, whatever the registration order.
    for (const DrawOverride* o : r.all())
        m.emplace(o->nodeType, o);
}
} // namespace

Registry<DrawOverride>& registry()
{
    static Registry<DrawOverride>* r = [] {
        // Lila's own, even when an extension's init is what first asks for the registry.
        owners::OwnerScope lila({});
        auto* reg = new Registry<DrawOverride>();
        reg->add("lila.locator", {.id = "lila.locator", .nodeType = "locator", .draw = drawLocator, .pick = pickLocator});
        reg->addListener([reg](RegistryChange, const std::string&, const std::string&) { rebuildByType(*reg); });
        rebuildByType(*reg);
        return reg;
    }();
    return *r;
}

const DrawOverride& add(DrawOverride o)
{
    std::string id = o.id;
    return registry().add(std::move(id), std::move(o));
}

const DrawOverride* add(ext::Context& ctx, DrawOverride o)
{
    std::string id = o.id;
    try
    {
        ctx.add(registry(), "drawOverride", id, std::move(o));
        return registry().find(id);
    }
    catch (const std::exception& e)
    {
        log::error("extension {}: draw override: {}", ctx.owner(), e.what());
        return nullptr;
    }
}

bool remove(std::string_view id) { return registry().remove(id); }

uint64_t version()
{
    registry();
    return gVersion;
}

const DrawOverride* forType(const scene::NodeType& t)
{
    registry();
    const auto& m = byType();
    if (m.empty())
        return nullptr;
    for (const scene::NodeType* type = &t; type; type = type->base)
        if (auto it = m.find(type->name); it != m.end())
            return it->second;
    return nullptr;
}
} // namespace lila::ui::drawOverrides
