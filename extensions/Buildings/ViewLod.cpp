// Building meshes whose lod is auto show near, hlod or far by their distance from the camera of the view being drawn.
// The node reads what this sets; a change reruns that one modifier.

#include "BuildingsScene.h"

#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Prefs.h>
#include <lila/Profile.h>
#include <lila/Viewport.h>
#include <lila/ViewportTool.h>

#include <chrono>

using namespace lila;
using namespace lila::ui;

namespace buildings
{
namespace
{
/** Nearest distance from p to the box, cm. */
double distanceTo(const Vec3& p, const Vec3& lo, const Vec3& hi) { return glm::length(glm::max(glm::max(lo - p, p - hi), Vec3(0.0))); }

void followCamera(render::FrameState&, OverlayPainter&, const ViewInfo& view)
{
    static auto last = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(300))
        return;
    last = now;
    LILA_ZONE();
    PrefStore& prefs = PrefStore::get();
    const double nearTo = prefs.value<double>("general", "/buildings/lodNear", 40000.0);
    const double farFrom = prefs.value<double>("general", "/buildings/lodFar", 200000.0);
    scene::Graph& g = scene::Graph::current();
    static std::vector<std::shared_ptr<const Published>> all;
    allPublished(all);
    for (const std::shared_ptr<const Published>& p : all)
    {
        if (!p->autoLod || p->lo.x > p->hi.x)
            continue;
        const double d = distanceTo(view.eye, p->lo, p->hi);
        const char* lod = d < nearTo ? "near" : d < farFrom ? "hlod" : "far";
        if (setViewLod(p->modifier, lod))
            gn::rerunModifier(g, scene::NodeId(p->modifier));
    }
    all.clear();
}
} // namespace

void registerViewLod(lila::ext::Context& ctx) { viewportHooks::addDrawer(ctx, "buildings.viewLod", followCamera); }
} // namespace buildings
