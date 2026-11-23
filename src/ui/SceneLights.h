#pragma once

#include "core/Export.h"

#include <QPointF>
#include <QRectF>

#include <glm/vec4.hpp>

class QWidget;

namespace lila::ui
{
class OverlayPainter;
class ViewportWidget;

/**
 * The viewport's Scene Lights switch (/viewport/sceneLights) and the places that show it is off: a badge at the top of
 * every view and a note on lights in the Attribute Editor, both only while the scene has a visible light to miss.
 */
namespace sceneLights
{
/** Whether lights and domes light the viewport. */
LILA_UI_API bool on();
LILA_UI_API void setOn(bool on);

/** Scene lights are off and the scene has a visible light, so the view looks unlit by them. */
LILA_UI_API bool offWithLights();

/** Where the badge sits in view, under any banners above it; empty while it is hidden. width is what is drawn into. */
LILA_UI_API QRectF badgeRect(const ViewportWidget* view, double width);
/** Draws the badge, hot while the cursor is over it. Returns its rect, empty when hidden. */
QRectF drawBadge(OverlayPainter& p, const ViewportWidget* view, double width);
/** Turns scene lights back on when at is on the badge. Returns whether it was. */
LILA_UI_API bool clickBadge(const ViewportWidget* view, QPointF at);

/** The badge icon's linear tint, the theme's warning colour; light handles mark the off state with it too. */
LILA_UI_API glm::vec4 iconColor();

/** A warning strip with a Turn On button, for a light's Attribute Editor; it hides itself while scene lights are on. */
LILA_UI_API QWidget* attributeNote(QWidget* parent);

/** viewport.toggleLighting and the badge's icon. */
LILA_UI_API void registerActions();
} // namespace sceneLights
} // namespace lila::ui
