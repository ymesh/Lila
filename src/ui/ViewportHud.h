#pragma once

#include "core/Export.h"

#include "render/FrameState.h"
#include "scene/Graph.h"
#include "ui/Manipulator.h"
#include "ui/Overlay.h"

#include <QRectF>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class QMenu;

namespace lila::scene::isolation
{
struct Set;
}

namespace lila::ui
{
class ViewportWidget;
}

/**
 * Maya's Heads Up Display adapted to Lila: per-viewport readouts stacked in the view's corners so they never overlap.
 * Each viewport panel keeps its own choices in the general prefs under /viewport/hud/panel<N>/.
 */
namespace lila::ui::hud
{
/** A line of an item's readout that can be hidden on its own, from the item's option box. */
struct Part
{
    const char* id; // "verts"; its action is "viewport.hud.<item>.<id>"
    const char* label;
};

struct Item
{
    const char* id; // "polyCount"; its action is "viewport.hud.<id>"
    const char* label;
    const char* doc;
    std::vector<Part> parts = {};
};

/** Every HUD element, in menu order. */
LILA_UI_API const std::vector<Item>& items();

/** The panel's own choice for an item, ignoring the show-all switch. */
LILA_UI_API bool itemOn(int panel, std::string_view item);
/** Turning an item on also turns the show-all switch on, so it never waits invisibly behind it. */
LILA_UI_API void setItemOn(int panel, std::string_view item, bool on);
/** The panel's Show/Hide all switch. */
LILA_UI_API bool allShown(int panel);
LILA_UI_API void setAllShown(int panel, bool on);
/** On and not hidden by the show-all switch: what actually draws. */
LILA_UI_API bool shown(int panel, std::string_view item);
/** Whether a part of an item draws while the item does; every part is on by default. */
LILA_UI_API bool partOn(int panel, std::string_view item, std::string_view part);
LILA_UI_API void setPartOn(int panel, std::string_view item, std::string_view part, bool on);

/** Check state of a "viewport.hud.*" action for a view, nullopt for any other id. */
LILA_UI_API std::optional<bool> actionState(const std::string& id, const ViewportWidget* view);

/** Where the view cube draws in a view that wide: the top-right corner, with the right-hand readouts below it. */
LILA_UI_API QRectF viewCubeRect(double viewWidth);

/** One View Switcher segment: "persp", "front", "right", "top" or "camera", and where it draws. */
struct SwitcherSegment
{
    const char* view;
    QRectF rect;
};
/**
 * The View Switcher's segments in a view of that size, under the view cube when it shows; empty when the view is too
 * small for it.
 */
LILA_UI_API std::vector<SwitcherSegment> viewSwitcherSegments(double viewWidth, double viewHeight, bool belowCube);

/** The HUD menu of a viewport: the show-all switch, then one checkable entry per item, then options. */
LILA_UI_API void fillMenu(QMenu* menu, ViewportWidget* view);

/**
 * Registers "viewport.hud.toggleAll", one "viewport.hud.<id>" per item and one "viewport.hud.<id>.<part>" per part, all
 * acting on active(). Items with parts get an option box listing them.
 */
LILA_UI_API void registerActions(std::function<ViewportWidget*()> active);

struct Context
{
    int panel = 0;
    ViewInfo view;
    /** Whether the view draws the node at all (isolate, LOD level, show filters); null shows everything. */
    std::function<bool(scene::NodeId)> shownNode;
    /** Changes whenever shownNode's answers may have; 0 recounts every frame. */
    uint64_t shownKey = 0;
    /** The view's isolation, for meshes it shows only some faces of; null when not isolating. */
    const scene::isolation::Set* isolation = nullptr;
    /** The world point under the cursor (a surface, else the ground plane), for the georeference readout; nullopt off the view. */
    std::optional<glm::dvec3> cursorWorld;
    bool cursorOnSurface = false;
    double framesPerSecond = 0.0;
    /** GPU time of the last drawn frame and the ambient occlusion part of it; 0 when unknown. */
    double gpuMs = 0.0;
    double aoMs = 0.0;
    /** Receives the rect of every readout drawn, so tool messages can avoid them. */
    std::vector<QRectF>* drawn = nullptr;
};

/**
 * The text readouts: poly count, smooth preview, selection and texel density top left; object details, symmetry and
 * soft select top right; current frame and frame rate bottom right. Returns true while a count is provisional, so the
 * caller redraws shortly for the exact one.
 */
LILA_UI_API bool draw(OverlayPainter& p, const Context& ctx);

/** World axes at the origin, a constant size on screen, letters at their tips. */
LILA_UI_API void drawOriginAxis(render::FrameState& s, OverlayPainter& p, const ViewInfo& view);
} // namespace lila::ui::hud
