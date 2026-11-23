#include "ui/SceneLights.h"

#include "core/PrefStore.h"
#include "scene/Graph.h"
#include "scene/SceneIndex.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/Controls.h"
#include "ui/DroneCamera.h"
#include "ui/FeatureStatus.h"
#include "ui/Hotkeys.h"
#include "ui/Overlay.h"
#include "ui/Theme.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportHud.h"
#include "ui/ViewportToast.h"
#include "ui/ViewportWidget.h"

#include <QCursor>
#include <QHBoxLayout>
#include <QPointer>
#include <QToolButton>

#include <algorithm>
#include <cmath>

namespace lila::ui::sceneLights
{
namespace
{
constexpr const char* kActionId = "viewport.toggleLighting";
constexpr double kHeight = 26.0, kPad = 10.0, kIcon = 16.0, kGap = 8.0;
const QString kTitle = "Scene Lights Off";
const QString kTurnOn = "Turn On";

struct Layout
{
    QRectF bar;
    bool title = true;
    bool turnOn = true;
};

/** Centred over the view, kept clear of the view cube, shedding "Turn On" and then the title when the view is narrow. */
Layout layout(const ViewportWidget* view, double width)
{
    double top = drone::active(view) ? 46.0 : 10.0;
    if (!featureStatus::inUse().isEmpty())
        top += 36.0;
    double right = width - 8.0;
    if (view && hud::shown(view->hudPanel(), "viewCube"))
        if (const QRectF cube = hud::viewCubeRect(width); cube.bottom() > top)
            right = std::min(right, cube.left() - 8.0);
    const double room = right - 8.0;
    const double titleW = OverlayPainter::textWidth(kTitle), turnOnW = OverlayPainter::textWidth(kTurnOn);
    Layout l;
    double total = kPad + kIcon + kGap + titleW + kGap * 1.5 + turnOnW + kPad;
    if (total > room)
    {
        l.turnOn = false;
        total = kPad + kIcon + kGap + titleW + kPad;
    }
    if (total > room)
    {
        l.title = false;
        total = kPad + kIcon + kPad;
    }
    const double left = std::max(8.0, std::min(std::round(width * 0.5 - total * 0.5), right - total));
    l.bar = QRectF(std::round(left), top, std::round(total), kHeight);
    return l;
}
} // namespace

bool on()
{
    return PrefStore::get().value<bool>("general", viewportDisplay::kSceneLights, true);
}

void setOn(bool on)
{
    PrefStore::get().set("general", viewportDisplay::kSceneLights, on);
}

bool offWithLights()
{
    if (on())
        return false;
    const scene::Graph& g = scene::Graph::current();
    for (const scene::Node* n : scene::sceneIndex::ofType(g, "light"))
        if (scene::effectivelyVisible(g, n->id(), true))
            return true;
    return false;
}

QRectF badgeRect(const ViewportWidget* view, double width)
{
    return offWithLights() ? layout(view, width).bar : QRectF();
}

QRectF drawBadge(OverlayPainter& p, const ViewportWidget* view, double width)
{
    if (!offWithLights())
        return {};
    const Layout l = layout(view, width);
    const QRectF bar = l.bar;
    const bool hot = view && bar.contains(view->mapFromGlobal(QCursor::pos().toPointF()));
    p.rect(bar, hot ? glm::vec4(0.2f, 0.2f, 0.21f, 0.92f) : glm::vec4(0.08f, 0.08f, 0.09f, 0.85f));
    const glm::vec4 text(0.95f, 0.95f, 0.95f, 1.0f), dim(0.62f, 0.62f, 0.62f, 1.0f);
    const double baseline = bar.center().y() + 5.0;
    double x = bar.left() + kPad;
    p.icon(QRectF(x, bar.center().y() - kIcon * 0.5, kIcon, kIcon), "sceneLightsOff", iconColor());
    x += kIcon + kGap;
    if (l.title)
        p.text({x, baseline}, kTitle, text);
    x += OverlayPainter::textWidth(kTitle) + kGap * 1.5;
    if (l.turnOn)
        p.text({x, baseline}, kTurnOn, hot ? text : dim);
    return bar;
}

bool clickBadge(const ViewportWidget* view, QPointF at)
{
    if (!badgeRect(view, view->width()).contains(at))
        return false;
    setOn(true);
    return true;
}

glm::vec4 iconColor()
{
    return themeLinear("issueWarning", {0.9f, 0.55f, 0.1f, 1.0f});
}

QWidget* attributeNote(QWidget* parent)
{
    QWidget* banner = controls::warningBanner(parent);
    banner->setToolTip("Lights and domes light nothing in the viewport while Scene Lights is off. The environment still lights.");
    auto* turnOn = controls::push(kActionId, banner, kTurnOn);
    static_cast<QHBoxLayout*>(banner->layout())->addWidget(turnOn, 0, Qt::AlignVCenter);
    auto sync = [b = QPointer<QWidget>(banner)] {
        if (b)
            controls::setWarning(b, on() ? QString() : QString("Scene Lights off in the viewport"));
    };
    sync();
    const int listener = PrefStore::get().onChanged([banner, sync](const std::string& file) {
        if (file == "general")
            QMetaObject::invokeMethod(banner, sync, Qt::QueuedConnection);
    });
    QObject::connect(banner, &QObject::destroyed, [listener] { PrefStore::get().removeOnChanged(listener); });
    return banner;
}

void registerActions()
{
    overlayIcons::add("sceneLightsOff", {.tabler = "bulb-off"});
    const std::string key = viewportDisplay::kSceneLights;
    actions::add({.id = kActionId,
                  .label = "Scene Lights",
                  .category = "Display",
                  .run =
                      [] {
                          setOn(!on());
                          // A stray key press is the usual way lights go dark, so the key says what it did.
                          if (Hotkeys::fromKey())
                          {
                              const QString k = Hotkeys::get().keyFor(kActionId);
                              viewportToast::showActive(QString(on() ? "Scene Lights On" : "Scene Lights Off") + (k.isEmpty() ? "" : " (" + k + ")"));
                          }
                      },
                  .doc = "Turns the scene's lights off and on in the viewport, as if hidden. The environment still lights.",
                  .python = "lila.prefs.set(\"general\", \"" + key + "\", not lila.prefs.get(\"general\", \"" + key + "\", True))",
                  .checked = [] { return on(); },
                  .keywords = {"use all lights", "default lighting", "lights off", "unlit", "lights not working"}});
}
} // namespace lila::ui::sceneLights
