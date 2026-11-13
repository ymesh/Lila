#include "ui/Ghosting.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "scene/Ghosts.h"
#include "ui/Actions.h"
#include "ui/Menus.h"
#include "ui/MeshCache.h"
#include "ui/UiCommands.h"
#include "ui/UiCommon.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportHooks.h"

#include <QColor>
#include <QMenu>

#include <algorithm>
#include <cmath>

namespace lila::ui
{
namespace
{
namespace gh = scene::ghosts;

glm::vec3 prefColor(const char* key, const char* fallback)
{
    QColor c(QString::fromStdString(PrefStore::get().value<std::string>("general", key, fallback)));
    if (!c.isValid())
        c = QColor(fallback);
    return {srgbToLinear(float(c.redF())), srgbToLinear(float(c.greenF())), srgbToLinear(float(c.blueF()))};
}

void drawGhosts(render::FrameState& s, OverlayPainter&, const ViewInfo&)
{
    LILA_ZONE();

    const gh::Settings st = gh::Settings::fromPrefs();
    if (!st.enabled)
        return;
    std::vector<gh::Ghost> ghosts = gh::cache().ghosts(st);
    if (ghosts.empty())
        return;
    const bool tinted = viewportDisplay::accentColors(viewportDisplay::kGhostColors);
    // Without accent colours, before and after still differ: the past darker, the future lighter.
    const glm::vec3 before = tinted ? prefColor(gh::kBeforeColor, gh::kDefaultBeforeColor) : glm::vec3(0.18f);
    const glm::vec3 after = tinted ? prefColor(gh::kAfterColor, gh::kDefaultAfterColor) : glm::vec3(0.55f);
    for (const gh::Ghost& g : ghosts)
    {
        const int count = std::max(1, g.count);
        // The nearest ghost is at full opacity, the farthest at half; across a range, the falloff fades them further.
        const float fade = 1.0f - 0.5f * float(std::abs(g.offset) - 1) / float(std::max(1, count - 1));
        const float alpha = float(st.opacity) * fade * float(0.35 + 0.65 * g.weight);
        const glm::vec3 color = g.offset < 0 ? before : after;
        const bool meshes = st.mesh && !g.frame->meshes.empty();
        if (meshes)
            for (const gh::MeshGhost& m : g.frame->meshes)
            {
                auto rm = renderMeshFor(m.mesh);
                if (!rm)
                    continue;
                render::MeshDraw d;
                d.world = m.world;
                uint32_t slotCount = 1;
                for (const render::MeshSection& sec : rm->sections)
                    slotCount = std::max(slotCount, sec.material + 1);
                render::Material mat;
                mat.baseColor = color;
                mat.opacity = alpha;
                mat.roughness = 0.8f;
                mat.specular = 0.2f;
                mat.flags = render::MatTranslucent | render::MatTwoSided;
                d.materials.assign(slotCount, mat);
                d.mesh = std::move(rm);
                d.flags = render::DrawShaded;
                s.meshes.push_back(std::move(d));
            }
        if (st.skeleton || !meshes)
            for (const auto& [from, to] : g.frame->bones)
            {
                s.linesOnTop.push_back({glm::dvec3(from), glm::vec4(color, std::min(1.0f, alpha * 2.0f))});
                s.linesOnTop.push_back({glm::dvec3(to), glm::vec4(color, std::min(1.0f, alpha * 2.0f))});
            }
    }
}
std::string prefText(const char* key, const char* fallback) { return PrefStore::get().value<std::string>("general", key, fallback); }
} // namespace

std::optional<bool> ghostActionState(const std::string& id)
{
    if (id == "anim.toggleGhosts")
        return generalPref(gh::kEnabled, false);
    if (id.starts_with("anim.ghostMode."))
        return prefText(gh::kMode, "keys") == id.substr(15);
    if (id.starts_with("anim.ghostShow."))
        return prefText(gh::kShow, "mesh") == id.substr(15);
    if (id == "anim.ghostRange")
        return generalPref(gh::kRange, true);
    return std::nullopt;
}

void addGhostingMenu(const std::string& menu)
{
    menus::addSubmenu(menu, "Ghosting", [](QMenu* m) {
        QObject::connect(m, &QMenu::aboutToShow, m, [m] {
            for (QAction* qa : m->actions())
                if (std::optional<bool> on = ghostActionState(qa->data().toString().toStdString()))
                {
                    qa->setCheckable(true);
                    qa->setChecked(*on);
                }
        });
    });
    menus::addActions(menu + "/Ghosting", {"anim.toggleGhosts", "", "anim.ghostMode.keys", "anim.ghostMode.frames", "", "anim.ghostShow.mesh",
                                           "anim.ghostShow.skeleton", "anim.ghostShow.both", "", "anim.ghostRange", "anim.refreshGhosts"});
}

void registerGhostingUi()
{
    viewportHooks::addDrawer(drawGhosts);

    auto add = [](std::string id, std::string label, std::function<void()> run, std::string doc, std::string python) {
        actions::add({.id = std::move(id), .label = std::move(label), .category = "Animation", .run = std::move(run), .doc = std::move(doc),
                      .python = std::move(python)});
    };
    add("anim.toggleGhosts", "Ghosting", [] { runUi("ghosting", {{"enable", !generalPref(gh::kEnabled, false)}}); },
        "Show the selected characters faintly at the keys (or frames) around the current time.", "ghosting(enable=True)");
    add("anim.ghostMode.keys", "Ghost at Keys", [] { runUi("ghosting", {{"mode", "keys"}, {"enable", true}}); },
        "Ghosts sit at the previous and next keys.", "ghosting(mode=\"keys\")");
    add("anim.ghostMode.frames", "Ghost Every N Frames", [] { runUi("ghosting", {{"mode", "frames"}, {"enable", true}}); },
        "Ghosts sit every step frames before and after the current time.", "ghosting(mode=\"frames\")");
    add("anim.ghostShow.mesh", "Ghost Meshes", [] { runUi("ghosting", {{"show", "mesh"}, {"enable", true}}); },
        "Ghosts show the evaluated (skinned) meshes; skeletons without a mesh still show their bones.", "ghosting(show=\"mesh\")");
    add("anim.ghostShow.skeleton", "Ghost Skeletons", [] { runUi("ghosting", {{"show", "skeleton"}, {"enable", true}}); },
        "Ghosts show only the bones.", "ghosting(show=\"skeleton\")");
    add("anim.ghostShow.both", "Ghost Meshes and Skeletons", [] { runUi("ghosting", {{"show", "both"}, {"enable", true}}); },
        "Ghosts show meshes with their bones.", "ghosting(show=\"both\")");
    actions::add({.id = "anim.ghostRange",
                  .label = "Ghosts Across Range",
                  .category = "Animation",
                  .run = [] { runUi("ghosting", {{"range", !generalPref(gh::kRange, true)}, {"enable", true}}); },
                  .doc = "While the time slider has a range selected, the ghosts spread across it and its falloff, fading with the falloff.",
                  .python = "ghosting(range=True)",
                  .checked = [] { return generalPref(gh::kRange, true); },
                  .keywords = {"onion skin", "range", "ghost"}});
    add("anim.refreshGhosts", "Refresh Ghosts", [] { runUi("ghosting", {{"refresh", true}}); },
        "Recapture the ghosts, after edits that aren't keys (a static channel, say).", "ghosting(refresh=True)");
}
} // namespace lila::ui
