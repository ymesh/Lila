#pragma once

#include "core/Export.h"

#include <functional>

class QMouseEvent;
class QWheelEvent;

namespace lila::ui
{
class OverlayPainter;
class ViewportWidget;

/**
 * Drone Camera: a view-only fly mode for moving through large scenes, with Unreal's viewport fly keys and mouse. WASD,
 * the arrows or the number pad move, E/Q rise and fall, Z/C zoom while held, R/F change speed, Esc leaves. Mouse as in
 * Unreal: left drag moves along the ground and turns, right drag looks, middle (or left with right) drag pans, the wheel
 * moves forward and right drag with the wheel changes speed. Nothing in the scene can be edited while it is on; Alt with
 * the mouse still tumbles, tracks and dollies. The whole flight is one camera undo step.
 */
namespace drone
{
LILA_UI_API bool active(const ViewportWidget* view);
/** Starts flying in view, switching an orthographic or look-through view to perspective first (and saying so). */
LILA_UI_API void enter(ViewportWidget* view);
LILA_UI_API void leave(ViewportWidget* view);
/**
 * On or off. Run from a key, a tap toggles and holding the key flies only while it is down; a key bound to it is shown
 * in the view's banner.
 */
LILA_UI_API void toggle(ViewportWidget* view);

/** Flight speed in cm per second, shared by every view and remembered between sessions. */
LILA_UI_API double speed();
LILA_UI_API void setSpeed(double cmPerSecond);

/** Input the view hands over first while view is flying; true when the flight took it. */
bool mousePress(ViewportWidget* view, QMouseEvent* e);
bool mouseMove(ViewportWidget* view, QMouseEvent* e);
bool mouseRelease(ViewportWidget* view, QMouseEvent* e);
bool wheel(ViewportWidget* view, QWheelEvent* e);
/** The banner at the top of a flying view, with its close button, and a frame round the view. */
void draw(OverlayPainter& p, ViewportWidget* view);
/** Sets view's cursor while it flies; false when it isn't flying. */
bool applyCursor(ViewportWidget* view);

/** view.drone and the menu entries; active gives the view the action targets. */
LILA_UI_API void registerActions(std::function<ViewportWidget*()> active);
} // namespace drone
} // namespace lila::ui
