// Panel Layout maps for the baker: line mask, panel ID and line height, read from each low mesh's own layout.

#include "layout/LayoutNode.h"
#include "layout/Lines.h"
#include "layout/Maps.h"

#include "bake/BakeInputs.h"
#include "bake/Maps.h"
#include "scene/Graph.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_inverse.hpp>

namespace lila::layout
{
namespace
{
/** Each low's resolved layout and its world-to-mesh matrix, by group and low. */
struct Prepared
{
    struct Low
    {
        std::shared_ptr<const Resolved> layout;
        glm::dmat4 toLocal{1.0};
    };
    std::vector<std::vector<Low>> groups;
};

std::shared_ptr<const void> prepare(const bake::MapSettings&, const bake::BakeInputs& in)
{
    auto out = std::make_shared<Prepared>();
    scene::Graph& g = scene::Graph::current();
    for (const bake::GroupInputs& group : in.groups)
    {
        std::vector<Prepared::Low>& lows = out->groups.emplace_back();
        for (const bake::LowInput& low : group.lows)
        {
            Prepared::Low entry;
            if (scene::Node* mesh = g.find(low.soup.name))
                if (scene::Node* lay = findLayoutNode(g, mesh->id()))
                {
                    entry.layout = resolvedLayout(g, lay->id());
                    if (mesh->type().isA("transform"))
                        entry.toLocal = glm::inverse(mesh->as<glm::dmat4>(scene::Transform::kWorldMatrix));
                }
            lows.push_back(std::move(entry));
        }
    }
    return out;
}

const Prepared::Low* lowOf(const bake::MapContext& ctx, uint32_t low)
{
    const auto* p = static_cast<const Prepared*>(ctx.prepared);
    if (!p || ctx.groupIndex >= p->groups.size() || low >= p->groups[ctx.groupIndex].size())
        return nullptr;
    const Prepared::Low& l = p->groups[ctx.groupIndex][low];
    return l.layout ? &l : nullptr;
}

/** Runs fn(layout, mesh-space point, index) for every sample on a low with a layout; the rest are left out. */
template <class F> void eachSample(const bake::MapContext& ctx, std::span<const bake::TexelSample> samples, std::span<uint8_t> valid, F&& fn)
{
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const Prepared::Low* low = lowOf(ctx, samples[i].low);
        valid[i] = low ? 1 : 0;
        if (!low)
            continue;
        const glm::dvec3 world(samples[i].position[0], samples[i].position[1], samples[i].position[2]);
        fn(*low->layout, glm::dvec3(low->toLocal * glm::dvec4(world, 1.0)), i);
    }
}

} // namespace

void registerBakeMaps()
{
    bake::registerMap({.id = "panelLines",
                       .label = "Panel Lines",
                       .suffix = "panellines",
                       .doc = "The low mesh's Panel Layout lines: white on a line, fading to black at its edge. For grime, wear and masks.",
                       .cpu =
                           [](const bake::MapContext& ctx, std::span<const bake::TexelSample> samples, std::span<const bake::HitRecord>,
                              std::span<glm::vec4> out, std::span<uint8_t> valid) {
                               eachSample(ctx, samples, valid, [&](const Resolved& r, const glm::dvec3& p, size_t i) {
                                   const LineSample line = r.lineAt(p, std::max(r.reach(), 1e-6));
                                   double v = 0.0;
                                   if (line.run >= 0)
                                       v = std::clamp(1.0 - line.distance / std::max(1e-9, r.runs()[size_t(line.run)].width * 0.5), 0.0, 1.0);
                                   out[i] = glm::vec4(float(v), float(v), float(v), 1.0f);
                               });
                           },
                       .prepare = prepare});
    bake::registerMap({.id = "panelId",
                       .label = "Panel ID",
                       .suffix = "panelid",
                       .doc = "A flat colour per panel of the low mesh's Panel Layout, black on the lines. For masking panels.",
                       .resolve = bake::Resolve::Exact,
                       .linear = false,
                       .cpu =
                           [](const bake::MapContext& ctx, std::span<const bake::TexelSample> samples, std::span<const bake::HitRecord>,
                              std::span<glm::vec4> out, std::span<uint8_t> valid) {
                               eachSample(ctx, samples, valid, [&](const Resolved& r, const glm::dvec3& p, size_t i) {
                                   const LineSample line = r.lineAt(p, std::max(r.reach(), 1e-6));
                                   const bool onLine = line.run >= 0 && line.distance < r.runs()[size_t(line.run)].width * 0.5;
                                   const PanelSample panel = r.panelAt(p);
                                   if (onLine || panel.panel < 0)
                                   {
                                       out[i] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
                                       return;
                                   }
                                   out[i] = glm::vec4(panelColor(panel.key), 1.0f);
                               });
                           },
                       .prepare = prepare});
    bake::registerMap({.id = "panelHeight",
                       .label = "Panel Line Height",
                       .suffix = "panelheight",
                       .doc = "The grooves of the low mesh's Texture panel lines as a height map: 1 at the surface, 0 at the deepest groove.",
                       .background = {1.0f, 1.0f, 1.0f, 1.0f},
                       .preferredBits = 16,
                       .cpu =
                           [](const bake::MapContext& ctx, std::span<const bake::TexelSample> samples, std::span<const bake::HitRecord>,
                              std::span<glm::vec4> out, std::span<uint8_t> valid) {
                               eachSample(ctx, samples, valid, [&](const Resolved& r, const glm::dvec3& p, size_t i) {
                                   double deepest = 0.0;
                                   for (const ResolvedRun& run : r.runs())
                                       if (run.output == Output::Texture)
                                           deepest = std::max(deepest, std::abs(run.depth));
                                   const double d = deepest > 0.0 ? grooveDepth(r, p, outputBit(Output::Texture)) / deepest : 0.0;
                                   const float v = float(1.0 - std::clamp(d, 0.0, 1.0));
                                   out[i] = glm::vec4(v, v, v, 1.0f);
                               });
                           },
                       .prepare = prepare});
}
} // namespace lila::layout
