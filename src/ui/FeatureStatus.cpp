#include "ui/FeatureStatus.h"

#include "extensions/ExtensionManager.h"
#include "ui/Actions.h"
#include "ui/DroneCamera.h"
#include "ui/Menus.h"
#include "ui/Overlay.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportWidget.h"

#include <QDesktopServices>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace lila::ui::featureStatus
{
namespace
{
/** Action id to the menu paths that list it, rebuilt after the menu items change. */
struct MenuIndex
{
    std::multimap<std::string, std::string, std::less<>> paths;
    bool dirty = true;
    bool listening = false;
};

MenuIndex& menuIndex()
{
    static MenuIndex index;
    return index;
}

const features::Feature* ofListingMenu(const std::string& actionId)
{
    MenuIndex& index = menuIndex();
    if (!index.listening)
    {
        index.listening = true;
        menus::itemRegistry().addListener([](RegistryChange, const std::string&, const std::string&) { menuIndex().dirty = true; });
    }
    if (index.dirty)
    {
        index.dirty = false;
        index.paths.clear();
        for (const menus::Item* item : menus::itemRegistry().all())
            if (item->kind == menus::Kind::Action)
                index.paths.emplace(item->action, item->menu);
    }
    auto [from, to] = index.paths.equal_range(actionId);
    for (auto it = from; it != to; ++it)
        if (const features::Feature* f = features::ofMenu(it->second))
            return f;
    return nullptr;
}

std::string ownerOf(const std::string& actionId)
{
    const std::string* owner = actions::registry().ownerOf(actionId);
    return owner ? *owner : std::string();
}

/** Ids of the not-ready features an action of theirs ran for this session. */
std::set<std::string>& used()
{
    static std::set<std::string> ids;
    return ids;
}

bool loaded(const features::Feature& f)
{
    return std::ranges::any_of(f.extensions, [](const std::string& e) { return extensions::ExtensionManager::get().isLoaded(e); });
}
} // namespace

const features::Feature* ofAction(const std::string& actionId)
{
    if (const features::Feature* f = features::ofAction(actionId, ownerOf(actionId)))
        return f;
    return ofListingMenu(actionId);
}

bool marked(features::Status s)
{
    return s == features::Status::InDevelopment || s == features::Status::NotReady;
}

std::string label(const std::string& actionId)
{
    const features::Feature* f = ofAction(actionId);
    return f && marked(f->status) ? features::label(f->status) : "";
}

features::Status rowStatus(const std::string& actionId, const std::string& path)
{
    const features::Feature* f = features::ofAction(actionId, ownerOf(actionId));
    if (!f)
        f = features::ofMenu(path);
    return f ? f->status : features::Status::Ready;
}

void openRoadmap()
{
    QDesktopServices::openUrl(QUrl(features::kRoadmapUrl));
}

QStringList inUse()
{
    QStringList out;
    for (const std::string& id : used())
        if (const features::Feature* f = features::find(id); f && f->status == features::Status::NotReady && loaded(*f))
            out << QString::fromStdString(f->title);
    return out;
}

void draw(OverlayPainter& p, const ViewportWidget* view, double width)
{
    const QStringList names = inUse();
    if (names.isEmpty())
        return;
    const QString title = names.join(", ");
    const QString status = features::label(features::Status::NotReady);
    const double height = 30.0, pad = 10.0, icon = 16.0, gap = 8.0;
    const double titleW = OverlayPainter::textWidth(title), statusW = OverlayPainter::textWidth(status);
    const double total = pad + icon + gap + titleW + gap * 1.5 + statusW + pad;
    // The Drone Camera's banner has the top of a flying view.
    const double top = drone::active(view) ? 46.0 : 10.0;
    const QRectF bar(std::round(width * 0.5 - total * 0.5), top, std::round(total), height);
    p.rect(bar, {0.08f, 0.08f, 0.09f, 0.85f});
    const glm::vec4 text(0.95f, 0.95f, 0.95f, 1.0f), dim(0.62f, 0.62f, 0.62f, 1.0f);
    const double baseline = bar.center().y() + 5.0;
    double x = bar.left() + pad;
    p.icon(QRectF(x, bar.center().y() - icon * 0.5, icon, icon), "featureNotReady", text);
    x += icon + gap;
    p.text({x, baseline}, title, text);
    x += titleW + gap * 1.5;
    p.text({x, baseline}, status, dim);
}

void registerActions()
{
    overlayIcons::add("featureNotReady", {.tabler = "alert-triangle"});
    actions::add({.id = "help.roadmap",
                  .label = "Roadmap",
                  .category = "Help",
                  .run = [] { openRoadmap(); },
                  .doc = "Opens Lila's roadmap in the browser: what is ready for feedback, what is in development and what is planned.",
                  .icon = "map-2",
                  .keywords = {"status", "ready", "alpha", "development", "planned"}});
    actions::addRunListener([](const actions::RunEvent& e) {
        if (e.done || !e.target.is_string())
            return;
        const features::Feature* f = ofAction(e.target.get<std::string>());
        if (f && f->status == features::Status::NotReady && used().insert(f->id).second)
            viewportTools::requestRedraw();
    });
}
} // namespace lila::ui::featureStatus
