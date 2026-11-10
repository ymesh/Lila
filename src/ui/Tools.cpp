#include "ui/Tools.h"

#include "ui/Actions.h"
#include "ui/ViewportTool.h"
#include "ui/GizmoMath.h"

#include "core/PrefStore.h"
#include "scene/Components.h"

#include <QMouseEvent>
#include <map>
#include <tuple>

namespace lila::ui::tools
{
namespace
{
Tool gTool = Tool::Select;
bool gPivotEdit = false;
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

bool bridgeOnTap()
{
    if (scene::components::mode() != scene::ComponentMode::Edge)
        return false;
    size_t count = 0;
    for (const auto& [node, ids] : scene::components::selected(scene::ComponentMode::Edge))
        count += ids.size();
    return count >= 2;
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

    actions::add({.id = "tool.pivotEditOn", .label = "Edit Pivot (hold)", .category = "Tools", .run = [] { setPivotEdit(true); },
                  .doc = "While held, the manipulator moves the pivot instead of the object."});
    actions::add({.id = "tool.pivotEditOff", .label = "End Pivot Edit", .category = "Tools", .run = [] { setPivotEdit(false); },
                  .doc = "Returns the manipulator to moving the object."});
    actions::add({.id = "tool.cloneDragOn", .label = "Clone Drag (hold)", .category = "Tools", .run = [] { setCloneDrag(true); },
                  .doc = "While held, dragging a manipulator handle duplicates the selected objects and moves the copies."});
    actions::add({.id = "tool.cloneDragOff", .label = "End Clone Drag", .category = "Tools", .run = [] { setCloneDrag(false); },
                  .doc = "Returns manipulator drags to moving the selected objects themselves."});
    // Y: held, a manipulator drag clones; a tap without one reactivates the last viewport tool.
    actions::add({.id = "tool.cloneKeyDown", .label = "Clone Key Down", .category = "Tools", .run = [] {
                      gCloneKeyUsed = false;
                      setCloneDrag(true);
                  },
                  .doc = "Press half of the clone key: hold and drag a manipulator handle to clone the selected objects."});
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
        for (auto [space, what] : {std::pair{"world", "the world axes"}, {"object", "the object's own axes"}, {"parent", "the parent's axes"}})
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
    // A tap with two or more edges selected in edge mode bridges them instead.
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
                  .doc = "Release half of the soft select key: a tap without a drag bridges two or more selected edges in edge mode, "
                         "otherwise toggles soft select."});
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
