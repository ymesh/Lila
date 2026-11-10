#pragma once

#include "core/Export.h"
#include "core/ExtensionContext.h"
#include "core/Registry.h"

#include "render/FrameState.h"
#include "ui/Manipulator.h"
#include "ui/Overlay.h"

#include <QCursor>
#include <QRectF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lila::ui
{
/**
 * An interactive viewport tool (weight paint, retopo draw, sculpt, knife...). While one is active the viewport offers it
 * every mouse and key event first, except Alt navigation (Ctrl+Alt clicks do reach it); returning true consumes the event. draw() adds to the
 * frame: 3D lines and meshes into `s`, 2D overlay into `p`.
 */
class ViewportTool
{
public:
    virtual ~ViewportTool() = default;
    virtual const char* name() const = 0;

    virtual void activate() {}
    virtual void deactivate() {}

    virtual bool mousePress(QMouseEvent* e, const ViewInfo& view) { (void)e, (void)view; return false; }
    virtual bool mouseMove(QMouseEvent* e, const ViewInfo& view) { (void)e, (void)view; return false; }
    virtual bool mouseRelease(QMouseEvent* e, const ViewInfo& view) { (void)e, (void)view; return false; }
    virtual bool wheel(QWheelEvent* e, const ViewInfo& view) { (void)e, (void)view; return false; }
    virtual bool keyPress(QKeyEvent* e, const ViewInfo& view) { (void)e, (void)view; return false; }
    /**
     * A key bound to a hotkey, offered to the tool before the hotkeys run action (the action id its press runs, or empty),
     * wherever the focus is. True takes the key and nothing else runs, e.g. edit.undo stepping back a point placed but not
     * yet committed. Keys no hotkey binds go to keyPress instead.
     */
    virtual bool hotkey(const QKeyEvent& e, std::string_view action) { (void)e, (void)action; return false; }

    virtual void draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view) { (void)s, (void)p, (void)view; }
    /** The viewport's cursor while this tool is active; nullopt is the arrow. */
    virtual std::optional<QCursor> cursor() const { return std::nullopt; }
    /** B + LMB/MMB drag sets this tool's brush radius rather than soft select's. */
    virtual bool ownsBrushRadius() const { return false; }
    /**
     * The marking menu a right-click with this tool active opens instead of the viewport's, for base "viewport",
     * "viewportShift" or "viewportCtrl"; empty keeps the viewport's.
     */
    virtual std::string markingMenu(std::string_view base) const { (void)base; return {}; }
    /** False draws the selected mesh without its selection wire while this tool is active. */
    virtual bool showsSelection(scene::NodeId mesh) const { (void)mesh; return true; }
};

/** An X: the cursor while a modifier click deletes what is under it. */
LILA_UI_API const QCursor& deleteCursor();

/** A key and what it does, for drawKeyList and ToolStatus. */
struct KeyHint
{
    QString keys;
    QString what;
};

/** A named value in a ToolStatus: "radius" "175 px". */
struct ToolField
{
    QString label;
    QString value;
};

/** A toggle in a ToolStatus, always shown so the line keeps its shape: lit when on, dim when off. */
struct ToolFlag
{
    QString label;
    bool on = false;
};

/** Symmetry as a tool applies it, drawn on its own at the top centre of the view. */
struct ToolSymmetry
{
    bool on = false;
    /** The axis or kind when on, e.g. "X", "Object X", "Radial 6"; empty shows just "on". */
    QString axis;
};

/**
 * A tool's readout along the top of the view: what the tool is working on and with, then how to drive it. Each part
 * keeps the widest width it has shown for this tool, so the readout holds still while values change.
 */
struct ToolStatus
{
    /** The tool's name, e.g. "Sculpt". Also what the remembered widths are kept under. */
    QString tool;
    /** What it works on, usually a node name. */
    QString subject;
    /** The brush or mode, shown first among the settings. */
    QString mode;
    std::vector<ToolField> fields;
    std::vector<ToolFlag> flags;
    /** What to do next, in a few words: "select a mesh", "click the start, then the end". */
    QString prompt;
    /** Shown in the corner key list while the tool runs; list what the keys held now do, so it follows context. */
    std::vector<KeyHint> keys;
    /** Set by tools that mirror their edits; shown above the readout, not in it. */
    std::optional<ToolSymmetry> symmetry;
};

/** Queues a tool's readout for flushToolMessages. */
LILA_UI_API void drawToolStatus(OverlayPainter& p, const ViewInfo& view, const ToolStatus& status);
/** A one-off line under the tool readout, for warnings and notices. Queued and drawn by flushToolMessages once the HUD is placed. */
LILA_UI_API void drawToolMessage(OverlayPainter& p, const ViewInfo& view, const QString& text, const glm::vec4& color = {0.95f, 0.95f, 0.95f, 1.0f});
/** The selected meshes' symmetry, shown at the top centre when no tool reports its own. */
LILA_UI_API void drawSelectionSymmetry(const ToolSymmetry& symmetry);
/** Draws the symmetry indicator, the queued readouts and messages, moved down past any of the occupied HUD rects they would cover. */
LILA_UI_API void flushToolMessages(OverlayPainter& p, const ViewInfo& view, const std::vector<QRectF>& occupied);
/** Height drawKeyList gives the hints. */
LILA_UI_API double keyListHeight(size_t rows);
/** The width of the widest key, for laying the list out. */
LILA_UI_API double keyListWidth(const std::vector<KeyHint>& hints);
/** Keys in the accent colour with what they do beside them, one per line, ending just above bottomLeft. Nothing while keys are hidden. */
LILA_UI_API void drawKeyList(OverlayPainter& p, QPointF bottomLeft, const std::vector<KeyHint>& hints, float alpha = 1.0f);

/** Key hints on screen (tool readouts and the corner key lists); the Keys toggle in Overlays and Help. */
inline constexpr const char* kShowKeysPref = "/viewport/showKeys";
LILA_UI_API bool keysShown();

/**
 * Key hints for a layout, listed in the viewport's bottom-left corner while it is current and the active tool lists none
 * of its own. Extensions add theirs for their own layouts the same way.
 */
namespace viewportKeys
{
using Provider = std::function<std::vector<KeyHint>()>;
LILA_UI_API void add(const std::string& layout, Provider provider);
LILA_UI_API std::vector<KeyHint> forLayout(const std::string& layout);
/** Appends the action's current hotkey and what, or nothing when the action has no key bound. */
LILA_UI_API void addAction(std::vector<KeyHint>& out, const std::string& actionId, const QString& what);
/** Lila's own layouts' keys. */
LILA_UI_API void registerBuiltins();
} // namespace viewportKeys

namespace viewportTools
{
/**
 * A registered viewport tool: what activate(id) makes. Extensions register their tools this way rather than calling
 * set() with their own instance, so unloading the extension can end its tool first.
 */
struct ToolDef
{
    std::string id;
    QString label;
    /** A Tabler icon name for toolbars and menus; may be empty. */
    std::string icon;
    std::function<std::unique_ptr<ViewportTool>()> create;
};

LILA_UI_API Registry<ToolDef>& registry();
/** Throws std::invalid_argument when the id is taken. */
LILA_UI_API const ToolDef& add(ToolDef def);
/** For extensions: owned by ctx and removed when it unloads. Null, with a log message, if the id is taken. */
LILA_UI_API const ToolDef* add(ext::Context& ctx, ToolDef def);
/** Removing the definition of the active tool ends the tool first. */
LILA_UI_API bool remove(std::string_view id);

/** Makes the tool registered as id active. False when there is no such tool, or it made none. */
LILA_UI_API bool activate(std::string_view id);
/** The registered id of the active tool; empty when there is none, or it was set() directly. */
LILA_UI_API const std::string& activeId();

/** Replaces the active tool (deactivating the old one). nullptr goes back to select/move/rotate/scale. */
LILA_UI_API void set(std::unique_ptr<ViewportTool> tool);
LILA_UI_API ViewportTool* active();
/** Asks every viewport to redraw, e.g. after a brush stroke. */
LILA_UI_API void requestRedraw();

/**
 * A mesh drawn with the preselection highlight whatever the preselection pref, for tools that pick objects (the node
 * picker's eyedropper). kNoNode for none.
 */
LILA_UI_API void setHoverHighlight(scene::NodeId id);
LILA_UI_API scene::NodeId hoverHighlight();

LILA_UI_API int addListener(std::function<void()> fn);
LILA_UI_API void removeListener(int id);

/** The "general" pref: tumbling orbits the point a brush last touched instead of the center of interest. On by default. */
inline constexpr const char* kOrbitStrokePref = "/viewport/orbitLastStroke";
/** The "general" pref: undoing a brush stroke puts the tumble point back where it was before it. On by default. */
inline constexpr const char* kOrbitUndoPref = "/viewport/orbitUndo";
/** Where a brush tool last pressed, dragged or released on the surface, in world space; cleared when the tool changes. */
LILA_UI_API void setOrbitPoint(std::optional<glm::dvec3> point);
LILA_UI_API std::optional<glm::dvec3> orbitPoint();
} // namespace viewportTools
} // namespace lila::ui
