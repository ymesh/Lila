#include "ui/Tools.h"

#include "ui/Actions.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportToast.h"
#include "ui/GizmoMath.h"

#include "core/PrefStore.h"
#include "geo/Bridge.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Modifiers.h"

#include <QMouseEvent>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <tuple>

namespace lila::ui::tools
{
namespace
{
Tool gTool = Tool::Select;
bool gPivotEdit = false;
bool gPivotKeyWasOn = false;
std::chrono::steady_clock::time_point gPivotKeyDown;
constexpr std::chrono::milliseconds kPivotTapTime{250};
bool gCloneDrag = false;
bool gCloneKeyUsed = false;
unsigned gHandleResets = 0;
unsigned gSnaps = SnapNone;
std::map<int, std::function<void()>> gListeners;
int gNextListener = 1;

void notify()
{
    auto copy = gListeners;
    for (auto& [id, fn] : copy)
        fn();
}

/** A B tap bridges only when the pref allows it and the selected edges hold a pair of borders polyBridge would join. */
bool bridgeOnTap()
{
    if (scene::components::mode() != scene::ComponentMode::Edge || !PrefStore::get().value<bool>("general", "/softSelect/bridgeOnTap", false))
        return false;
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    for (const auto& [node, ids] : scene::components::selected(scene::ComponentMode::Edge))
        if (const scene::Node* n = g.node(node); n && n->type().isA("mesh") && ids.size() >= 2)
            if (scene::MeshPtr cage = scene::modifiers::cage(g, node); cage && !geo::planBridges(*cage, ids).empty())
                return true;
    return false;
}
} // namespace

Tool current() { return gTool; }

unsigned handleResets() { return gHandleResets; }

void setCurrent(Tool t)
{
    if (t == gTool)
        return;
    gTool = t;
    notify();
}

const char* name(Tool t)
{
    switch (t)
    {
    case Tool::Select: return "select";
    case Tool::Move: return "move";
    case Tool::Rotate: return "rotate";
    case Tool::Scale: return "scale";
    }
    return "";
}

bool pivotEdit() { return gPivotEdit; }

void setPivotEdit(bool on)
{
    if (on == gPivotEdit)
        return;
    gPivotEdit = on;
    notify();
}

bool cloneDrag() { return gCloneDrag; }

bool cloneShiftSwapped() { return PrefStore::get().value<bool>("general", "/manipulator/swapCloneShift", false); }

void setCloneDrag(bool on)
{
    if (on == gCloneDrag)
        return;
    gCloneDrag = on;
    notify();
}

void markCloneDragUsed() { gCloneKeyUsed = true; }

unsigned snaps() { return gSnaps; }

void setSnap(SnapFlags flag, bool on)
{
    unsigned next = on ? (gSnaps | flag) : (gSnaps & ~unsigned(flag));
    if (next == gSnaps)
        return;
    gSnaps = next;
    notify();
}

const char* snapKey(Tool t)
{
    return t == Tool::Rotate ? "rotate" : t == Tool::Scale ? "scale" : "move";
}

IncrementSnap incrementSnap(Tool t)
{
    const PrefStore& p = PrefStore::get();
    std::string base = std::string("/manipulator/snap/") + snapKey(t);
    double fallback = t == Tool::Rotate ? gizmo::kDefaultRotateStep : t == Tool::Scale ? gizmo::kDefaultScaleStep : gizmo::kDefaultMoveStep;
    IncrementSnap s;
    s.enabled = p.value<bool>("general", base + "/enabled", false);
    s.step = p.value<double>("general", base + "/step", fallback);
    s.absolute = t == Tool::Move && p.value<bool>("general", base + "/absolute", false);
    if (t == Tool::Rotate && (gSnaps & SnapRotateHold))
        s.enabled = !s.enabled;
    s.enabled = s.enabled && s.step > 0.0;
    return s;
}

static std::map<int, std::function<std::optional<SelectionSnap>()>> gSelectionSnaps;
static int gNextSelectionSnap = 1;

int addSelectionSnap(std::function<std::optional<SelectionSnap>()> fn)
{
    gSelectionSnaps[gNextSelectionSnap] = std::move(fn);
    return gNextSelectionSnap++;
}

void removeSelectionSnap(int id) { gSelectionSnaps.erase(id); }

std::optional<SelectionSnap> selectionSnap()
{
    if (gSnaps & SnapRotateHold)
        return std::nullopt;
    for (const auto& [id, fn] : gSelectionSnaps)
        if (std::optional<SelectionSnap> s = fn())
            return s;
    return std::nullopt;
}

int addListener(std::function<void()> fn)
{
    gListeners[gNextListener] = std::move(fn);
    return gNextListener++;
}

void removeListener(int id) { gListeners.erase(id); }

void changed() { notify(); }

static bool gSoftAdjust = false;
static bool gSoftKeyUsed = false;

bool softSelectAdjusting() { return gSoftAdjust; }
void markSoftSelectAdjusted() { gSoftKeyUsed = true; }

bool brushRadiusDrag(const QMouseEvent* e, bool armed)
{
    if (!(armed || gSoftAdjust) || (e->button() != Qt::LeftButton && e->button() != Qt::MiddleButton))
        return false;
    gSoftKeyUsed = true;
    return true;
}

void registerActions()
{
    auto tool = [](const char* id, const char* label, Tool t, const char* doc) {
        actions::add({.id = id, .label = label, .category = "Tools",
                      .run =
                          [t] {
                              // Picking a transform tool leaves any viewport tool (retopo draw, sculpt...), as in Maya.
                              viewportTools::set(nullptr);
                              if (t == current())
                              {
                                  ++gHandleResets;
                                  notify();
                              }
                              setCurrent(t);
                          },
                      .doc = doc,
                      .python = std::string("lila.ui.set_tool(\"") + name(t) + "\")"});
    };
    tool("tool.select", "Select Tool", Tool::Select, "Click or drag a marquee to select, with no manipulator.");
    tool("tool.move", "Move Tool", Tool::Move, "Drag the arrows, planes or center to move the selection.");
    tool("tool.rotate", "Rotate Tool", Tool::Rotate, "Drag the rings to rotate the selection, or the outer ring to turn it in screen space.");
    tool("tool.scale", "Scale Tool", Tool::Scale, "Drag the handles to scale the selection along an axis, a plane or uniformly from the center.");

    actions::add({.id = "tool.pivotEditOn", .label = "Edit Pivot (tap or hold)", .category = "Tools", .run = [] {
                      gPivotKeyWasOn = gPivotEdit;
                      gPivotKeyDown = std::chrono::steady_clock::now();
                      setPivotEdit(true);
                  },
                  .doc = "Press half of the pivot key: a tap toggles pivot editing, holding edits the pivot until release."});
    actions::add({.id = "tool.pivotEditOff", .label = "End Pivot Edit", .category = "Tools", .run = [] {
                      const bool tap = std::chrono::steady_clock::now() - gPivotKeyDown < kPivotTapTime;
                      setPivotEdit(tap && !gPivotKeyWasOn);
                  },
                  .doc = "Release half of the pivot key: after a hold it returns the manipulator to moving the object; after a tap it leaves pivot editing toggled."});
    actions::add({.id = "tool.cloneDragOn", .label = "Clone Drag (hold)", .category = "Tools", .run = [] { setCloneDrag(true); },
                  .doc = "While held, dragging a manipulator handle duplicates the selected objects and moves the copies; on faces and edges it extrudes them and moves the extruded ones.",
                  .keywords = {"extrude drag", "shift extrude", "duplicate drag", "alt drag"}});
    actions::add({.id = "tool.cloneDragOff", .label = "End Clone Drag", .category = "Tools", .run = [] { setCloneDrag(false); },
                  .doc = "Returns manipulator drags to moving the selected objects themselves."});
    // Y: held, a manipulator drag clones; a tap without one reactivates the last viewport tool.
    actions::add({.id = "tool.cloneKeyDown", .label = "Clone Key Down", .category = "Tools", .run = [] {
                      gCloneKeyUsed = false;
                      setCloneDrag(true);
                  },
                  .doc = "Press half of the clone key: hold and drag a manipulator handle to clone the selected objects, or extrude the selected faces or edges."});
    actions::add({.id = "tool.cloneKeyUp", .label = "Clone Key Up", .category = "Tools", .run = [] {
                      setCloneDrag(false);
                      if (!gCloneKeyUsed)
                          actions::run("tool.repeatLast");
                  },
                  .doc = "Release half of the clone key: a tap without a clone drag runs Repeat Last Tool."});
    actions::add({.id = "tool.repeatLast", .label = "Repeat Last Tool", .category = "Tools",
                  .run = [] {
                      if (auto id = actions::lastToolAction())
                          actions::run(*id);
                  },
                  .enabled = [] { return actions::lastToolAction().has_value(); },
                  .doc = "Reactivates the last viewport tool, such as the Distance Tool, ready for a new use.",
                  .python = "lila.ui.run_action(\"tool.repeatLast\")",
                  .keywords = {"reactivate", "again", "last tool"}});
    actions::add({.id = "tool.pivotEditToggle", .label = "Toggle Pivot Edit", .category = "Tools", .run = [] { setPivotEdit(!pivotEdit()); },
                  .doc = "Switches the manipulator between moving the object and moving its pivot.", .checked = [] { return pivotEdit(); }});

    auto spaceIs = [](std::string key, std::string space) -> std::function<bool()> {
        return [key, space] { return PrefStore::get().value<std::string>("general", key, "world") == space; };
    };
    for (const char* manip : {"move", "rotate"})
        for (auto [space, what] : {std::pair{"world", "the world axes"}, {"object", "the object's own axes"}, {"parent", "the parent's axes"},
                                   {"custom", "the Custom axis angles in the Modeling Toolkit's Move section"}})
        {
            std::string key = std::string("/manipulator/") + manip + "Space";
            std::string label = std::string(1, char(std::toupper(space[0]))) + (space + 1);
            actions::add({.id = std::string("manip.") + manip + "." + space, .label = label, .category = "Tools",
                          .run = [key, space] { PrefStore::get().set("general", key, space); },
                          .doc = std::string("Aligns the ") + manip + " manipulator to " + what + ".",
                          .python = "lila.prefs.set(\"general\", \"" + key + "\", \"" + space + "\")",
                          .checked = spaceIs(key, space)});
        }
    for (auto [space, label, what] : {std::tuple{"world", "World", "the world axes"}, {"object", "Object", "the object's own axes"},
                                      {"component", "Component", "the selected components' average normal and tangent"}})
        actions::add({.id = std::string("manip.scale.") + space, .label = label, .category = "Tools",
                      .run = [space] { PrefStore::get().set("general", "/manipulator/scaleSpace", space); },
                      .doc = std::string("Scales components along ") + what + ". Objects always scale on their own axes.",
                      .python = std::string("lila.prefs.set(\"general\", \"/manipulator/scaleSpace\", \"") + space + "\")",
                      .checked = spaceIs("/manipulator/scaleSpace", space)});
    actions::add({.id = "manip.rotate.component", .label = "Component", .category = "Tools",
                  .run = [] { PrefStore::get().set("general", "/manipulator/rotateSpace", "component"); },
                  .doc = "Rotates components on their average normal and tangent (on a curve, its tangent frame). Objects rotate on "
                         "their own axes.",
                  .python = "lila.prefs.set(\"general\", \"/manipulator/rotateSpace\", \"component\")",
                  .checked = spaceIs("/manipulator/rotateSpace", "component")});
    actions::add({.id = "manip.rotate.gimbal", .label = "Gimbal", .category = "Tools",
                  .run = [] { PrefStore::get().set("general", "/manipulator/rotateSpace", "gimbal"); },
                  .doc = "Each ring turns one rotate channel about its own Euler axis, in the object's rotate order. Two rings "
                         "lining up shows gimbal lock.",
                  .python = "lila.prefs.set(\"general\", \"/manipulator/rotateSpace\", \"gimbal\")",
                  .checked = spaceIs("/manipulator/rotateSpace", "gimbal"),
                  .keywords = {"euler", "gimbal lock", "local"}});
    actions::add({.id = "manip.move.component", .label = "Component", .category = "Tools",
                  .run = [] { PrefStore::get().set("general", "/manipulator/moveSpace", "component"); },
                  .doc = "Moves components together along their average normal and tangent (on a curve, its tangent frame). Objects move "
                         "on their own axes.",
                  .python = "lila.prefs.set(\"general\", \"/manipulator/moveSpace\", \"component\")",
                  .checked = spaceIs("/manipulator/moveSpace", "component")});
    actions::add({.id = "manip.move.normal", .label = "Normal", .category = "Tools",
                  .run = [] { PrefStore::get().set("general", "/manipulator/moveSpace", "normal"); },
                  .doc = "Moves each component along its own frame: N along its vertex normal (a face selection inflates), U and V along "
                         "its surface tangents (U follows the uvs, or the selected edges in edge mode). Curve points move along the curve: U its "
                         "tangent (a handle slides along itself), N across it within its plane, V out of the plane. Objects move on their "
                         "own axes.",
                  .python = "lila.prefs.set(\"general\", \"/manipulator/moveSpace\", \"normal\")",
                  .checked = spaceIs("/manipulator/moveSpace", "normal")});

    // Scale has no Parent space, so Parent leaves it alone.
    auto setAllSpaces = [](const std::string& space) {
        PrefStore& prefs = PrefStore::get();
        prefs.set("general", "/manipulator/moveSpace", space);
        prefs.set("general", "/manipulator/rotateSpace", space);
        if (space != "parent")
            prefs.set("general", "/manipulator/scaleSpace", space);
    };
    auto allSpacesIs = [](std::string space) -> std::function<bool()> {
        return [space] {
            const PrefStore& prefs = PrefStore::get();
            return prefs.value<std::string>("general", "/manipulator/moveSpace", "world") == space &&
                   prefs.value<std::string>("general", "/manipulator/rotateSpace", "world") == space &&
                   (space == "parent" || prefs.value<std::string>("general", "/manipulator/scaleSpace", "world") == space);
        };
    };
    for (auto [space, label, what] : {std::tuple{"world", "All World", "the world axes"}, {"object", "All Object", "the object's own axes"},
                                      {"parent", "All Parent", "the parent's axes (scale has no parent space and keeps its own)"}})
    {
        std::string py = std::string("for m in (\"move\", \"rotate\"") + (std::string(space) == "parent" ? "" : ", \"scale\"") +
                         "):\n    lila.prefs.set(\"general\", f\"/manipulator/{m}Space\", \"" + space + "\")";
        actions::add({.id = std::string("manip.all.") + space, .label = label, .category = "Tools",
                      .run = [setAllSpaces, space] { setAllSpaces(space); },
                      .doc = std::string("Aligns the move, rotate and scale manipulators to ") + what + ".",
                      .python = py,
                      .checked = allSpacesIs(space),
                      .keywords = {"manipulator space", "all manipulators", "transform space"}});
    }
    actions::add({.id = "manip.all.cycle", .label = "Cycle Manipulator Space", .category = "Tools",
                  .run =
                      [setAllSpaces] {
                          const std::string cur = PrefStore::get().value<std::string>("general", "/manipulator/moveSpace", "world");
                          const char* next = cur == "world" ? "object" : cur == "object" ? "parent" : "world";
                          setAllSpaces(next);
                          const QString label = QString(next);
                          viewportToast::showActive(label.left(1).toUpper() + label.mid(1) + " Space", 1200, viewportToast::Kind::Info,
                                                    "manip.space");
                      },
                  .doc = "Steps the move, rotate and scale manipulators through World, Object and Parent space, starting from the move "
                         "manipulator's current space.",
                  .keywords = {"manipulator space", "toggle space", "world object parent", "transform space"}});

    auto resizeManip = [](double factor) {
        double size = PrefStore::get().value<double>("general", "/manipulator/size", 100.0);
        // Round to the Preferences field's 5 px step, always moving at least one step.
        double next = std::round(size * factor / 5.0) * 5.0;
        if (next == size)
            next += factor > 1.0 ? 5.0 : -5.0;
        next = std::clamp(next, 30.0, 400.0);
        PrefStore::get().set("general", "/manipulator/size", next);
        viewportToast::showActive(QString("Manipulator Size %1%").arg(qRound(next)), 1200, viewportToast::Kind::Info, "manip.size");
        viewportTools::requestRedraw();
    };
    actions::add({.id = "manip.sizeUp", .label = "Larger Manipulator", .category = "Tools", .run = [resizeManip] { resizeManip(1.15); },
                  .doc = "Draws the move, rotate and scale manipulators larger on screen.",
                  .python = "lila.ui.run_action(\"manip.sizeUp\")",
                  .keywords = {"gizmo size", "handle size", "manipulator size", "bigger", "increase"}});
    actions::add({.id = "manip.sizeDown", .label = "Smaller Manipulator", .category = "Tools", .run = [resizeManip] { resizeManip(1.0 / 1.15); },
                  .doc = "Draws the move, rotate and scale manipulators smaller on screen.",
                  .python = "lila.ui.run_action(\"manip.sizeDown\")",
                  .keywords = {"gizmo size", "handle size", "manipulator size", "decrease"}});

    auto snap = [](const char* key, const char* label, SnapFlags f, const char* doc) {
        actions::add({.id = std::string("snap.") + key + "On", .label = label, .category = "Snapping", .run = [f] { setSnap(f, true); },
                      .doc = doc});
        actions::add({.id = std::string("snap.") + key + "Off", .label = std::string(label) + " Off", .category = "Snapping",
                      .run = [f] { setSnap(f, false); }, .doc = "Ends the held snap when the key is released."});
    };
    snap("grid", "Snap to Grid (hold)", SnapGrid, "While held, moves snap to grid points.");

    actions::add({.id = "softSelect.toggle", .label = "Soft Select", .category = "Tools", .run = [] {
                      bool on = PrefStore::get().value<bool>("general", "/softSelect/enabled", false);
                      PrefStore::get().set("general", "/softSelect/enabled", !on);
                  },
                  .doc = "Edits fall off smoothly to the components around the selection, within the soft select radius.",
                  .python = "lila.prefs.set(\"general\", \"/softSelect/enabled\", True)",
                  .checked = [] { return PrefStore::get().value<bool>("general", "/softSelect/enabled", false); }});
    actions::add({.id = "softSelect.adjustOn", .label = "Adjust Soft Select Radius (hold)", .category = "Tools", .run = [] {
                      gSoftAdjust = true;
                      notify();
                  },
                  .doc = "While held, dragging in the viewport sets the soft select radius."});
    actions::add({.id = "softSelect.adjustOff", .label = "Stop Adjusting Soft Select", .category = "Tools", .run = [] {
                      gSoftAdjust = false;
                      notify();
                  },
                  .doc = "Ends the radius drag started by Adjust Soft Select Radius."});
    // Maya's B: a tap toggles soft select, holding it while dragging with LMB (or MMB, from zero) sets the radius.
    // A tap on a bridgeable edge selection bridges it instead, unless the bridgeOnTap pref is off.
    actions::add({.id = "softSelect.keyDown", .label = "Soft Select Key Down", .category = "Tools", .run = [] {
                      gSoftAdjust = true;
                      gSoftKeyUsed = false;
                      notify();
                  },
                  .doc = "Press half of the soft select key: hold and drag to set the radius."});
    actions::add({.id = "softSelect.keyUp", .label = "Soft Select Key Up", .category = "Tools", .run = [] {
                      gSoftAdjust = false;
                      if (!gSoftKeyUsed && bridgeOnTap())
                          actions::run("mesh.bridge");
                      else if (!gSoftKeyUsed)
                          PrefStore::get().set("general", "/softSelect/enabled", !PrefStore::get().value<bool>("general", "/softSelect/enabled", false));
                      notify();
                  },
                  .doc = "Release half of the soft select key: a tap without a drag toggles soft select, or with the Key tap bridges two "
                         "borders pref on, bridges a selection of two borders in edge mode."});
    snap("edge", "Snap to Edge (hold)", SnapEdge, "While held, moves snap onto the nearest edge under the cursor.");
    snap("vertex", "Snap to Vertex (hold)", SnapVertex, "While held, moves snap to the nearest vertex, locator or joint under the cursor.");
    snap("surface", "Snap to Surface (hold)", SnapSurface, "While held, moves slide along the mesh surface under the cursor.");
    snap("rotateHold", "Toggle Rotate Snap (hold)", SnapRotateHold, "While held, flips rotate snapping on or off.");

    for (Tool t : {Tool::Move, Tool::Rotate, Tool::Scale})
    {
        std::string key = std::string("/manipulator/snap/") + snapKey(t) + "/enabled";
        std::string label = std::string(t == Tool::Move ? "Move" : t == Tool::Rotate ? "Rotate" : "Scale") + " Snapping";
        actions::add({.id = std::string("snap.") + snapKey(t) + "Increment", .label = label, .category = "Snapping",
                      .run = [key] { PrefStore::get().set("general", key, !PrefStore::get().value<bool>("general", key, false)); },
                      .doc = t == Tool::Rotate ? "Rotation steps by a fixed angle." : t == Tool::Move ? "Moves step by a fixed distance."
                                                                                                       : "Scaling steps by a fixed amount.",
                      .python = "lila.prefs.set(\"general\", \"" + key + "\", True)"});
    }
}
} // namespace lila::ui::tools
