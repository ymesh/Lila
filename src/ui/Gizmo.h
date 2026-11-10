#pragma once

#include "core/Export.h"
#include "core/ExtensionContext.h"
#include "core/Registry.h"
#include "ui/ViewInfo.h"

#include <QPointF>
#include <QString>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

class QKeyEvent;

namespace lila::ui
{
class OverlayPainter;

/**
 * An on-screen manipulator a viewport drives (Maya's MPxManipContainer). Each viewport owns its own instance, made by
 * the active provider's factory, and talks to it only through this interface: setView() before every call, then hover
 * while the mouse is up, press/drag/release (or cancel) for a drag, keyPress for numeric entry, and draw once a frame.
 *
 * A gizmo that edits the scene should do so with undo off while dragging and run one command on release, so undo and
 * the script echo see a single edit (as the default "lila.trs" manipulator does).
 */
class LILA_UI_API Gizmo
{
public:
    virtual ~Gizmo() = default;

    virtual void setView(const ViewInfo& view) = 0;

    /**
     * Whether it works in a tool mode: "select", "move", "rotate", "scale" (the transform tools), or the name() of the
     * active ViewportTool. A viewport whose active gizmo does not support the current mode falls back to the default
     * provider, so a gizmo only has to handle the modes it adds something to.
     */
    virtual bool supports(std::string_view mode) const = 0;

    /** False when there is nothing to manipulate; nothing is drawn or picked then. */
    virtual bool visible() = 0;
    /** Handles, plus any readout of the drag in progress. */
    virtual void draw(OverlayPainter& p) = 0;

    /** Updates the hovered handle; true when it changed (the view redraws). */
    virtual bool hover(QPointF px) = 0;
    /** True when the press was taken: it starts a drag, or is swallowed so it doesn't fall through to selection. */
    virtual bool press(QPointF px, Qt::MouseButton button, Qt::KeyboardModifiers mods) = 0;
    virtual void drag(QPointF px, Qt::KeyboardModifiers mods) = 0;
    /** Ends the drag and commits it. */
    virtual void release() = 0;
    /** Ends the drag and puts back what it changed. */
    virtual void cancel() = 0;
    /** A wheel step during a drag (+1 up, -1 down); true when the gizmo used it. */
    virtual bool wheel(int steps)
    {
        (void)steps;
        return false;
    }
    virtual bool dragging() const = 0;
    virtual bool hovering() const = 0;
    /** The hovered handle can't act (its channels are locked): the cursor says so. */
    virtual bool hoveringLocked() const { return false; }

    /** Keys while the gizmo has the view: numeric entry, Esc to cancel. True when consumed. */
    virtual bool keyPress(QKeyEvent* e) { (void)e; return false; }
    /** The number typed so far during a drag, empty when none. */
    virtual QString typed() const { return {}; }
    /** The current drag's readout ("X 12.50 cm", "45.0°"), empty when not dragging. */
    virtual QString readout() const { return {}; }
    /** A precision (slowed) drag is in progress. */
    virtual bool precisionDrag() const { return false; }

    /** The viewport moved the cursor by offset to wrap it at the edges; the next drag() position jumps by that much. */
    virtual void warped(QPointF offset) { (void)offset; }
    /** Whether the viewport may wrap the cursor at its edges during this gizmo's drags. */
    virtual bool wrapsCursor() const { return true; }

    /**
     * Whether a right-button press at px is the gizmo's (a handle, or something it drives, under the cursor). When true
     * the viewport gives the press to press() instead of opening its right-button marking menu.
     */
    virtual bool wantsRightButton(QPointF px) { (void)px; return false; }
};

/**
 * Gizmo providers: named factories for gizmos. The active one comes from setActive() (modes call it), else the
 * "general" pref /viewport/gizmo, else the default "lila.trs" (move/rotate/scale). Changing it, or removing the
 * provider in use, replaces every viewport's instance at once (cancelling a drag in progress), so no instance outlives
 * the extension that made it.
 */
namespace gizmos
{
inline constexpr const char* kDefault = "lila.trs";
inline constexpr const char* kPref = "/viewport/gizmo";

struct Provider
{
    std::string id;
    QString label;
    std::function<std::unique_ptr<Gizmo>()> create;
};

/** Holds "lila.trs" from first use. */
LILA_UI_API Registry<Provider>& registry();

/** Throws std::invalid_argument when the id is taken. */
LILA_UI_API const Provider& add(Provider provider);
/** For extensions: owned by ctx and removed when it unloads. Null, with a log message, if the id is taken. */
LILA_UI_API const Provider* add(ext::Context& ctx, Provider provider);
LILA_UI_API bool remove(std::string_view id);
LILA_UI_API const Provider* find(std::string_view id);

/** Makes id the active provider for every viewport; empty goes back to the pref. False (no change) when id is unknown. */
LILA_UI_API bool setActive(std::string_view id);
/** What setActive() last set, empty when the pref decides. */
LILA_UI_API const std::string& activeOverride();
/** The provider in use: the override, else the pref, else kDefault, skipping ids that are not registered. */
LILA_UI_API std::string activeId();

/** A new instance from the provider, or null when it is not registered. */
LILA_UI_API std::unique_ptr<Gizmo> create(std::string_view id);

/**
 * Called when the active provider may have changed, or a provider was removed. Viewports drop instances whose provider
 * is gone right there, before its code can unload. Returns an id for removeListener.
 */
LILA_UI_API int addListener(std::function<void()> fn);
LILA_UI_API void removeListener(int id);
} // namespace gizmos
} // namespace lila::ui
