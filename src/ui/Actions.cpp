#include "ui/Actions.h"

#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "python/Python.h"
#include "scene/ComponentPick.h"
#include "scene/Duplicate.h"
#include "scene/Graph.h"
#include "scene/Isolation.h"
#include "scene/Modifiers.h"
#include "scene/Relocate.h"
#include "ui/Deletion.h"
#include "ui/Hotkeys.h"
#include "ui/MaterialDraw.h"
#include "ui/OptionBox.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportTool.h"

#include <QApplication>
#include <QCursor>

#include <algorithm>
#include <cctype>
#include <map>
#include <string_view>

namespace lila::ui::actions
{
namespace
{
std::vector<json>& recentList()
{
    static std::vector<json> r;
    return r;
}

json& lastRepeatable()
{
    static json r;
    return r;
}

std::string& lastTool()
{
    static std::string id;
    return id;
}

void runCommand(const char* name, const json& call = json::object())
{
    try
    {
        ops::execute(name, call, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::error("{}", e.what());
    }
}

/** "weightedNormals" -> "Weighted Normals". */
std::string modifierLabel(const std::string& kind)
{
    std::string out;
    for (size_t i = 0; i < kind.size(); ++i)
    {
        char c = kind[i];
        if (i == 0)
            out += char(std::toupper(static_cast<unsigned char>(c)));
        else
        {
            if (std::isupper(static_cast<unsigned char>(c)))
                out += ' ';
            out += c;
        }
    }
    return out;
}
} // namespace

Registry<Action>& registry()
{
    static Registry<Action> r;
    return r;
}

void add(Action a, std::string owner)
{
    std::string id = a.id;
    if (a.doc.empty())
        log::warn("action {} has no doc", id);
    if (!a.command.empty() && !a.run)
        a.run = [command = a.command, call = a.call.is_object() ? a.call : json::object()] { runWithOptions(command, call); };
    registry().put(std::move(id), std::move(a), std::move(owner));
}

uint64_t add(ext::Context& ctx, Action a)
{
    if (const std::string* owner = registry().ownerOf(a.id); owner && *owner != ctx.owner())
    {
        ctx.log(LogLevel::Error, "action '" + a.id + "' already belongs to " + *owner);
        return 0;
    }
    std::string id = a.id;
    add(std::move(a), ctx.owner());
    return ctx.track("action:" + id, [id, own = ctx.owner()] {
        if (const std::string* o = registry().ownerOf(id); o && *o == own)
            registry().remove(id);
    });
}

bool remove(const std::string& id)
{
    return registry().remove(id);
}

void bindCommand(const std::string& id, std::string command, json call)
{
    Action* a = registry().find(id);
    if (!a)
        return;
    a->command = std::move(command);
    a->call = call.is_object() ? std::move(call) : json::object();
    a->run = [command = a->command, c = a->call] { runWithOptions(command, c); };
}

void runWithOptions(const std::string& command, const json& call)
{
    try
    {
        ops::execute(command, ops::options::callWith(command, call), CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::error("{}", e.what());
    }
}

std::string tooltip(const std::string& id)
{
    const Action* a = find(id);
    if (!a)
        return id;
    std::string key = Hotkeys::get().keyFor(id).toStdString();
    std::string tip = a->label + (key.empty() ? "" : "  (" + key + ")");
    return a->doc.empty() ? tip : tip + "\n" + a->doc;
}

const Action* find(const std::string& id)
{
    return registry().find(id);
}

void setEnabledWhen(const std::string& id, std::function<bool()> enabled)
{
    if (Action* a = registry().find(id))
        a->enabled = std::move(enabled);
}

bool isEnabled(const json& target)
{
    if (!target.is_string())
        return true;
    const Action* a = find(target.get<std::string>());
    return !a || !a->enabled || a->enabled();
}

bool hasSelection()
{
    return !scene::Graph::current().selection().empty();
}

std::vector<const Action*> all()
{
    std::vector<const Action*> out;
    for (Action* a : registry().all())
        out.push_back(a);
    return out;
}

namespace
{
Trigger& currentTrigger()
{
    static Trigger t;
    return t;
}

std::map<int, std::function<void(const RunEvent&)>>& runListeners()
{
    static std::map<int, std::function<void(const RunEvent&)>> l;
    return l;
}

int& runDepth()
{
    static int depth = 0;
    return depth;
}

void notifyRun(const json& target, bool done, bool ok)
{
    Trigger t = currentTrigger();
    if (t.via == Via::Button && Hotkeys::fromKey())
        t.via = Via::Key;
    const RunEvent e{target, t, done, ok};
    auto listeners = runListeners();
    for (auto& [id, fn] : listeners)
        fn(e);
}

bool runTarget(const json& target);
} // namespace

ViaScope::ViaScope(Via via, std::string path) : mOuter(std::exchange(currentTrigger(), Trigger{via, std::move(path)}))
{
}

ViaScope::~ViaScope()
{
    currentTrigger() = std::move(mOuter);
}

Via currentVia()
{
    return currentTrigger().via;
}

int addRunListener(std::function<void(const RunEvent&)> fn)
{
    static int next = 1;
    runListeners()[next] = std::move(fn);
    return next++;
}

void removeRunListener(int id)
{
    runListeners().erase(id);
}

RunReport::RunReport(json target) : mTarget(std::move(target)), mTop(runDepth()++ == 0)
{
    if (mTop)
        notifyRun(mTarget, false, false);
}

RunReport::~RunReport()
{
    --runDepth();
    if (mTop)
        notifyRun(mTarget, true, ok);
}

bool run(const json& target)
{
    RunReport report(target);
    report.ok = runTarget(target);
    return report.ok;
}

namespace
{
bool runTarget(const json& target)
{
    if (target.is_string())
    {
        const Action* a = find(target.get<std::string>());
        if (!a)
        {
            log::warn("no action named '{}'", target.get<std::string>());
            return false;
        }
        if (a->enabled && !a->enabled())
            return false;
        // A copy, since running it may remove the action (an extension unloading itself).
        std::function<void()> fn = a->run;
        ViewportTool* toolBefore = viewportTools::active();
        static unsigned toolRecords = 0;
        const unsigned recordsBefore = toolRecords;
        if (fn)
            fn();
        // The innermost action that started the tool, not one that ran it (Repeat Last Tool, a key's release).
        if (ViewportTool* t = viewportTools::active(); t && t != toolBefore && toolRecords == recordsBefore)
        {
            lastTool() = target.get<std::string>();
            ++toolRecords;
        }
        remember(target);
        return true;
    }
    if (!target.is_object())
        return false;
    if (target.contains("command"))
    {
        try
        {
            ops::execute(target["command"].get<std::string>(), target.value("args", json::object()), CallSource::UI);
            remember(target);
            return true;
        }
        catch (const std::exception& e)
        {
            log::error("{}", e.what());
            return false;
        }
    }
    if (target.contains("optionBox") && target["optionBox"].is_string())
    {
        optionBox::open(target["optionBox"].get<std::string>());
        return true;
    }
    if (target.contains("python"))
    {
        bool ok = python::run(target["python"].get<std::string>(), "<hotkey>").ok;
        if (ok)
            remember(target);
        return ok;
    }
    return false;
}
} // namespace

bool repeatable(const json& target)
{
    auto startsWith = [](const std::string& s, std::string_view prefix) { return s.rfind(prefix, 0) == 0; };
    auto commandRepeatable = [&](const std::string& name) {
        return !startsWith(name, "select") && !startsWith(name, "pickWalk") && name != "undo" && name != "redo" && name != "lookThru" &&
               !startsWith(name, "flushUndo") && (!ops::find(name) || ops::find(name)->undoable);
    };
    if (target.is_string())
    {
        const std::string& id = target.get_ref<const std::string&>();
        static const std::vector<std::string_view> prefixes = {
            "select.", "viewport.", "hotbox.", "window.", "tool.", "snap.", "softSelect.", "shelf.", "help.", "file.", "edit.flush",
            "anim.nextFrame", "anim.prevFrame", "anim.nextKey", "anim.prevKey", "anim.play", "anim.stop", "anim.firstFrame",
            "anim.lastFrame", "anim.ghost", "anim.toggleGhosts", "anim.autoKeyToggle", "adjustLastOp.", "outliner.revealSelected",
            "graphEditor.", "nodeEditor.frame", "nodeEditor.selectAll", "nodeEditor.zipTie", "nodeEditor.flowHold", "nodeEditor.toggleFlow", "nodeEditor.toggleMinimap", "nodeEditor.bookmarkGo", "nodeEditor.toggleSelectedWires",
            "nodeEditor.find", "mode.", "picker.frame", "measure.toggleShow"};
        static const std::vector<std::string_view> ids = {"edit.undo", "edit.redo", "edit.repeatLast", "edit.selectAll", "edit.deselect", "edit.invertSelection"};
        if (id == "file.exportSelection")
            return true;
        if (std::ranges::any_of(prefixes, [&](std::string_view p) { return startsWith(id, p); }) || std::ranges::find(ids, id) != ids.end())
            return false;
        // Selection and mode switches under any category: uv.vertexMode, uv.selectStacks, rig.dominantInfluenceSelect.
        std::string last = id.substr(id.rfind('.') + 1);
        std::ranges::transform(last, last.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (last.find("select") != std::string::npos || last.ends_with("mode"))
            return false;
        const Action* a = find(id);
        return !a || a->command.empty() || commandRepeatable(a->command);
    }
    if (!target.is_object())
        return false;
    if (target.contains("command") && target["command"].is_string())
        return commandRepeatable(target["command"].get_ref<const std::string&>());
    return target.contains("python");
}

void remember(const json& target)
{
    if (repeatable(target))
        lastRepeatable() = target;
    // Opening the hotbox is how Recent Commands is reached, so it isn't one of them.
    if (target.is_string() && (target.get<std::string>().rfind("hotbox.", 0) == 0 || target.get<std::string>() == "edit.repeatLast"))
        return;
    std::vector<json>& r = recentList();
    std::erase(r, target);
    r.insert(r.begin(), target);
    if (r.size() > 20)
        r.resize(20);
}

std::optional<json> repeatTarget()
{
    if (lastRepeatable().is_null())
        return std::nullopt;
    return lastRepeatable();
}

std::optional<std::string> lastToolAction()
{
    if (lastTool().empty() || !find(lastTool()))
        return std::nullopt;
    return lastTool();
}

std::string displayLabel(const std::string& id)
{
    if (id == "edit.repeatLast")
        if (auto t = repeatTarget())
            return "Repeat " + labelFor(*t);
    if (id == "tool.repeatLast")
        if (auto t = lastToolAction())
            return "Repeat " + labelFor(*t);
    const Action* a = find(id);
    return a ? a->label : id;
}

std::vector<json> recent()
{
    return recentList();
}

std::string labelFor(const json& target)
{
    if (target.is_string())
    {
        const Action* a = find(target.get<std::string>());
        return a ? a->label : target.get<std::string>();
    }
    if (target.is_object())
    {
        if (target.contains("label"))
            return target["label"].get<std::string>();
        if (target.contains("command"))
            return target["command"].get<std::string>();
        if (target.contains("markingMenu"))
            return target["markingMenu"].get<std::string>() + " menu";
        if (target.contains("optionBox") && target["optionBox"].is_string())
            return labelFor(target["optionBox"]) + " Options";
        if (target.contains("python"))
            return "Python";
    }
    return "?";
}

std::string pythonFor(const json& target)
{
    if (target.is_string())
    {
        const Action* a = find(target.get<std::string>());
        if (a && !a->python.empty())
            return a->python;
        return "lila.ui.run_action(" + json(target.get<std::string>()).dump() + ")";
    }
    if (target.is_object())
    {
        if (target.contains("python"))
            return target["python"].get<std::string>();
        if (target.contains("command"))
            return ops::toPython(target["command"].get<std::string>(), target.value("args", json::object()));
        if (target.contains("markingMenu"))
            return "# opens the '" + target["markingMenu"].get<std::string>() + "' marking menu";
        if (target.contains("optionBox") && target["optionBox"].is_string())
            return "# opens the option box of " + target["optionBox"].get<std::string>();
    }
    return "";
}

void registerBuiltins()
{
    viewportKeys::registerBuiltins();
    auto cmd = [](std::string id, std::string label, std::string category, const char* command, json call, std::string doc) {
        std::string py = ops::toPython(command, call);
        add({.id = std::move(id), .label = std::move(label), .category = std::move(category), .doc = std::move(doc), .python = py,
             .command = command, .call = call});
    };
    cmd("edit.undo", "Undo", "Edit", "undo", json::object(), "Steps back through the undo history.");
    cmd("edit.redo", "Redo", "Edit", "redo", json::object(), "Reapplies the last change you undid.");
    add({.id = "edit.repeatLast", .label = "Repeat Last", .category = "Edit",
         .run = [] {
             if (auto t = repeatTarget())
                 run(*t);
         },
         .enabled = [] { return repeatTarget().has_value(); },
         .doc = "Runs the last command, menu item or shelf button again. Selection, views, tools, windows and undo are not repeated.",
         .python = "lila.ui.run_action(\"edit.repeatLast\")"});
    cmd("edit.flushUndo", "Flush Undo", "Edit", "flushUndo", json::object(), "Clears the undo history so undo cannot go back past this point.");
    cmd("edit.flushRedo", "Flush Redo", "Edit", "flushUndo", {{"which", "future"}}, "Clears the redo steps, keeping the undo history.");
    cmd("edit.flushAll", "Flush All Undo", "Edit", "flushUndo", {{"which", "all"}}, "Clears both the undo and the redo history.");
    // Delete removes components in component mode, objects otherwise.
    add({"edit.delete", "Delete", "Edit", [] {
             auto scene = [] {
                 // Curve points sit in the vertex selection beside mesh vertices but have their own command.
                 bool curvePoints = false, meshComponents = false;
                 if (const scene::ComponentMode mode = scene::components::mode(); mode != scene::ComponentMode::Object)
                     for (const auto& entry : scene::components::selected(mode))
                         if (const scene::Node* n = scene::Graph::current().node(entry.first); n && !entry.second.empty())
                             (n->type().isA("curve") ? curvePoints : meshComponents) = true;
                 if (curvePoints)
                     runCommand("deleteCurvePoints", json::object());
                 if (meshComponents)
                     runCommand("deleteComponents", json::object());
                 if (!curvePoints && !meshComponents)
                     runCommand("delete", json::object());
             };
             if (Hotkeys::fromKey())
                 deletion::run(QApplication::focusWidget(), QApplication::widgetAt(QCursor::pos()), scene);
             else
                 scene();
         },
         nullptr,
         "Removes the selected objects, or the selected components in component mode. The Delete key removes what is selected in "
         "the panel with focus, or under the cursor: bake groups and maps, modifiers, layers, keys, nodes.",
         "delete()  # or deleteComponents() in component mode"});
    cmd("select.hierarchy", "Select Hierarchy", "Select", "selectHierarchy", json::object(), "Adds everything below the selected objects to the selection.");
    cmd("select.pickWalkUp", "Pick Walk Up (Parent)", "Select", "pickWalk", {{"direction", "up"}}, "Moves the selection to each object's parent.");
    cmd("select.pickWalkDown", "Pick Walk Down (Child)", "Select", "pickWalk", {{"direction", "down"}}, "Moves the selection to each object's first child.");
    cmd("select.pickWalkLeft", "Pick Walk Left (Previous Sibling)", "Select", "pickWalk", {{"direction", "left"}},
        "Moves the selection to the object above it in the outliner.");
    cmd("select.pickWalkRight", "Pick Walk Right (Next Sibling)", "Select", "pickWalk", {{"direction", "right"}},
        "Moves the selection to the object below it in the outliner.");
    cmd("edit.group", "Group", "Edit", "group", json::object(), "Parents the selected objects under a new group transform.");
    cmd("edit.duplicate", "Duplicate", "Edit", "duplicate", json::object(), "Copies the selected objects in place and selects the copies.");
    cmd("edit.duplicateWithTransform", "Duplicate with Transform", "Edit", "duplicateWithTransform", json::object(),
        "Duplicates the last copy again, placed the way that copy now sits relative to its source: duplicate, move the copy, "
        "then repeat this to continue the run.");
    cmd("edit.instance", "Instance", "Edit", "instance", json::object(),
        "Makes instances of the selected meshes: copies that share the mesh and its whole modifier stack, so an edit to one changes all "
        "and they export as one mesh. For a copy with modifiers of its own, use Reference Copy.");
    cmd("edit.instanceReference", "Reference Copy", "Edit", "instance", {{"reference", true}},
        "Makes references of the selected meshes: copies that share the base mesh but keep a modifier stack of their own (same shape, "
        "a different bevel). Component edits still reach every copy.");
    cmd("edit.makeUnique", "Make Unique", "Edit", "makeUnique", json::object(),
        "Gives the selected instances and references data of their own, so edits to them stop reaching the others.");
    cmd("edit.convertToInstance", "Convert to Instances", "Edit", "convertToInstance", json::object(),
        "Replaces the selected meshes with instances of the last one selected; with only one selected, every identical mesh in the scene.");
    cmd("select.instances", "Select Instances", "Select", "selectInstances", json::object(),
        "Selects every mesh sharing data with the selected ones: their source, instances and references.");
    cmd("select.instanceSource", "Select Instance Source", "Select", "selectInstanceSource", json::object(),
        "Selects the source of the selected instances and references.");
    setEnabledWhen("edit.duplicateWithTransform", [] { return !scene::duplicate::lastPairs().empty(); });
    for (const char* id : {"edit.instance", "edit.instanceReference", "edit.makeUnique", "edit.convertToInstance", "select.instances", "select.instanceSource"})
        setEnabledWhen(id, hasSelection);
    cmd("modify.centerPivot", "Center Pivot", "Modify", "centerPivot", json::object(), "Moves each selected object's pivot to the center of its bounds.");
    cmd("display.hide", "Hide Selection", "Display", "hide", json::object(), "Hides the selected objects in every viewport.");
    cmd("display.toggleVisibility", "Toggle Visibility", "Display", "toggleVisibility", json::object(),
        "Hides the selected objects that are shown and shows the ones that are hidden.");
    cmd("display.smoothOff", "Smooth Preview Off", "Display", "smoothPreview", {{"level", 0}}, "Draws the selected meshes as their plain cage.");
    cmd("display.smoothCage", "Smooth Preview (1 level)", "Display", "smoothPreview", {{"level", 1}},
        "Previews the selected meshes subdivided once. Display only; the mesh is unchanged.");
    cmd("display.smoothOn", "Smooth Preview", "Display", "smoothPreview", {{"level", 2}},
        "Previews the selected meshes subdivided and smooth. Display only; the mesh is unchanged.");
    cmd("display.show", "Show Selection", "Display", "show", json::object(), "Unhides the selected objects.");
    cmd("display.showAll", "Show All", "Display", "show", {{"all", true}}, "Unhides every hidden object.");
    cmd("display.showLastHidden", "Show Last Hidden", "Display", "showLastHidden", json::object(), "Unhides what the last hide hid.");
    for (const char* id : {"display.hide", "display.show", "display.smoothOff", "display.smoothCage", "display.smoothOn"})
        setEnabledWhen(id, hasSelection);
    cmd("modify.freezeTransforms", "Freeze Transformations", "Modify", "freezeTransforms", json::object(),
        "Bakes the selected objects' transforms into their geometry, leaving translate and rotate at 0 and scale at 1.");
    cmd("modify.resetTransforms", "Reset Transformations", "Modify", "resetTransforms", json::object(),
        "Puts translate, rotate and scale back to their defaults, moving the object.");
    cmd("modify.resetPivot", "Reset Pivot", "Modify", "resetPivot", json::object(), "Moves the pivot back to the object's origin.");
    cmd("modify.zeroPivot", "Zero Pivot", "Modify", "zeroPivot", json::object(), "Moves the pivot to the world origin.");
    cmd("modify.zeroPivotRotation", "Zero Pivot Rotation", "Modify", "zeroPivotRotation", json::object(),
        "Lines the selected objects' pivot axes up with the world, leaving the pivot and the objects where they are.");
    cmd("modify.bakePivot", "Bake Pivot", "Modify", "bakePivot", json::object(),
        "Turns the selected objects onto their pivot's axes without moving them, so they scale along those axes.");
    cmd("modify.matchPivot", "Match Pivot", "Modify", "matchPivot", json::object(),
        "Copies the first selected object's pivot position and orientation onto the other selected objects, without moving them.");
    cmd("modify.matchPivotPosition", "Match Pivot Position", "Modify", "matchPivot", {{"orient", false}},
        "Moves the other selected objects' pivots to the first selected object's pivot, keeping their orientation.");
    cmd("modify.matchPivotOrient", "Match Pivot Orientation", "Modify", "matchPivot", {{"position", false}},
        "Turns the other selected objects' pivots to the first selected object's pivot axes, keeping their position.");
    for (const char* id : {"modify.matchPivot", "modify.matchPivotPosition", "modify.matchPivotOrient"})
        setEnabledWhen(id, [] { return scene::Graph::current().selection().size() >= 2; });
    cmd("modify.orientPivot", "Orient Pivot", "Modify", "orientPivot", json::object(),
        "Turns the object so its X aims from the first picked component to the second, or along a single face, edge or loop, "
        "leaving the mesh where it is. Adjust the axes, up and mode in the popup after.");
    cmd("modify.pinComponentPivot", "Pin Component Pivot", "Modify", "pinComponentPivot", json::object(),
        "Keeps the component manipulator's axes, and its position once moved with Edit Pivot, through selection changes until "
        "reset. Click its label in the viewport to reset.");
    cmd("modify.resetComponentPivot", "Reset Component Pivot", "Modify", "resetComponentPivot", json::object(),
        "Puts the component manipulator back on the selection's centre and the tool's own axes.");
    cmd("modify.relocateScene", "Relocate Scene to Selection", "Modify", "relocateScene", json::object(),
        "Moves the whole scene so the selection's bottom centre becomes the world origin, rigs, deformers and animation included. "
        "Every transform and deformed point is checked after the move; if anything is off, nothing changes. One undo step.");
    cmd("modify.setDisplayOrigin", "Set Display Origin", "Modify", "setDisplayOrigin", {{"from", "bottom"}},
        "Draws the grid and world axes about the selection's bottom centre, and snaps to that grid, without moving anything.");
    cmd("modify.clearDisplayOrigin", "Clear Display Origin", "Modify", "setDisplayOrigin", {{"clear", true}},
        "Puts the grid and world axes back on the world origin.");
    setEnabledWhen("modify.relocateScene", hasSelection);
    setEnabledWhen("modify.setDisplayOrigin", hasSelection);
    setEnabledWhen("modify.clearDisplayOrigin", [] { return scene::relocate::hasDisplayOrigin(); });
    cmd("edit.parent", "Parent", "Edit", "parent", json::object(), "Parents the selected objects under the one selected last.");
    cmd("edit.unparent", "Unparent", "Edit", "parent", {{"world", true}}, "Moves the selected objects to the top of the hierarchy, keeping where they are.");
    cmd("edit.ungroup", "Ungroup", "Edit", "ungroup", json::object(), "Moves the selected groups' children up a level and deletes the groups.");
    cmd("edit.parentChain", "Parent Chain", "Edit", "parent", {{"chain", true}}, "Parents the selected objects into a chain, in selection order.");
    add({"edit.selectAll", "Select All", "Edit", [] {
              try
              {
                  json all = ops::execute("ls", {{"dag", true}});
                  // An isolated view selects only what it shows.
                  if (const scene::isolation::Set* iso = scene::isolation::active())
                  {
                      const scene::Graph& g = scene::Graph::current();
                      json shown = json::array();
                      for (const json& name : all)
                          if (const scene::Node* n = name.is_string() ? g.find(name.get<std::string>()) : nullptr; !n || iso->shows(n->id()))
                              shown.push_back(name);
                      all = std::move(shown);
                  }
                  ops::execute("select", {{"args", all}}, CallSource::UI);
              }
              catch (const std::exception& e)
              {
                  log::error("{}", e.what());
              }
          }, nullptr, "Selects every object in the scene.", "select(ls(dag=True))"});
    cmd("edit.deselect", "Deselect", "Edit", "select", {{"clear", true}}, "Clears the selection.");
    cmd("edit.invertSelection", "Invert Selection", "Edit", "invertSelection", json::object(),
        "Selects what is unselected and deselects the rest: objects, or components of the current kind.");
    cmd("file.new", "New Scene", "File", "newScene", json::object(), "Clears the scene and starts an empty one.");
    cmd("create.locator", "Locator", "Create", "spaceLocator", json::object(), "Creates a cross-hair transform to mark a spot, snap to or parent under.");
    cmd("create.camera", "Camera", "Create", "createNode", {{"args", {"camera"}}},
        "Creates a camera at the origin looking down -Z; look through it from the view's camera menu or by MMB-dragging it from the outliner onto a view.");
    cmd("create.emptyGroup", "Empty Group", "Create", "group", {{"empty", true}}, "Creates an empty transform to group objects under.");
    for (auto [mode, doc] : {std::pair{"object", "Selects and transforms whole objects."},
                             {"vertex", "Picks and edits the selected meshes' vertices."},
                             {"edge", "Picks and edits the selected meshes' edges."},
                             {"face", "Picks and edits the selected meshes' faces."}})
    {
        std::string label = std::string(1, char(std::toupper(mode[0]))) + (mode + 1) + " Mode";
        cmd(std::string("select.") + mode + "Mode", label, "Select", "selectMode", {{"args", {mode}}}, doc);
    }
    cmd("select.grow", "Grow Selection", "Select", "growSelection", json::object(), "Adds the components bordering the selection.");
    cmd("select.loop", "Select Edge Loop", "Select", "selectLoop", json::object(), "Extends the selection along its loops: edges, the edges between selected vertices, or the faces either side of a shared edge.");
    cmd("select.ring", "Select Edge Ring", "Select", "selectLoop", {{"ring", true}}, "Selects the parallel edges around each selected edge's ring, or the vertices of the rings between selected vertices.");
    cmd("select.allComponents", "Select All Components", "Select", "selectAllComponents", json::object(),
        "Selects every component of the current type on the selected meshes.");
    cmd("select.toVertices", "Convert to Vertices", "Select", "convertSelection", {{"args", {"vertex"}}}, "Turns the selection into the vertices it touches.");
    cmd("select.toEdges", "Convert to Edges", "Select", "convertSelection", {{"args", {"edge"}}}, "Turns the selection into the edges it touches.");
    cmd("select.toFaces", "Convert to Faces", "Select", "convertSelection", {{"args", {"face"}}}, "Turns the selection into the faces it touches.");

    cmd("mesh.extrude", "Extrude", "Mesh", "polyExtrude", json::object(), "Pulls the selected faces or edges out, adding side faces.");
    cmd("mesh.inset", "Inset", "Mesh", "polyInset", json::object(), "Shrinks a new face inside each selected face, ringed by quads.");
    cmd("mesh.bevel", "Bevel", "Mesh", "polyBevel", json::object(), "Cuts the selected edges into chamfers or rounded strips, once. The Bevel modifier stays live.");
    cmd("mesh.insertLoop", "Insert Edge Loop", "Mesh", "polyInsertLoop", json::object(),
        "Adds edge loops across the ring of the selected edge. With several, Fraction slides the group and Spacing sets the gap.");
    cmd("mesh.splitRings", "Split Rings", "Mesh", "polySplitRings", json::object(),
        "Subdivides one way: loops across the rings of the selected edges, or across selected faces, curved to the surface.");
    cmd("mesh.offsetLoop", "Offset Edge Loop", "Mesh", "polyOffsetLoop", json::object(),
        "Adds an edge loop either side of the selected edges, parallel to them. Partial loops end on a border or meet at their ends.");
    cmd("mesh.merge", "Merge", "Mesh", "polyMerge", json::object(), "Welds selected vertices that lie within the merge distance.");
    cmd("mesh.mergeCenter", "Merge to Center", "Mesh", "polyMergeCenter", json::object(), "Collapses the selection to a single point at its center.");
    cmd("mesh.collapse", "Collapse", "Mesh", "polyCollapse", json::object(), "Collapses each selected edge or face to a point.");
    cmd("mesh.dissolve", "Dissolve", "Mesh", "polyDissolve", json::object(), "Removes the selected edges or vertices, joining the faces around them.");
    cmd("mesh.bridge", "Bridge", "Mesh", "polyBridge", json::object(), "Joins two selected borders or face sets with a band of faces.");
    cmd("mesh.poke", "Poke", "Mesh", "polyPoke", json::object(), "Splits each selected face into triangles around a new center vertex.");
    cmd("mesh.spin", "Spin Edge", "Mesh", "polySpinEdge", json::object(), "Turns the selected edges to join the next corners of their two faces.");
    cmd("mesh.fillHole", "Fill Hole", "Mesh", "polyFillHole", json::object(), "Caps the selected open borders with a face.");
    cmd("mesh.gridFill", "Grid Fill", "Mesh", "polyGridFill", json::object(), "Rebuilds the selected fans or faces, or fills the selected holes, as a quad grid inside a ring of quads.");
    cmd("mesh.flip", "Flip Normals", "Mesh", "polyFlip", json::object(), "Reverses the selected faces so they point the other way.");
    cmd("mesh.conform", "Conform Normals", "Mesh", "polyConform", json::object(), "Turns faces to agree with their neighbours, so the whole shell faces one way.");
    cmd("mesh.triangulate", "Triangulate", "Mesh", "polyTriangulate", json::object(), "Splits the selected faces into triangles.");
    cmd("mesh.quadrangulate", "Quadrangulate", "Mesh", "polyQuadrangulate", json::object(), "Joins pairs of triangles into quads where they make good ones.");
    cmd("mesh.reduce", "Reduce", "Mesh", "polyReduce", json::object(), "Lowers the polygon count, keeping the shape, UVs and borders.");
    cmd("mesh.subdivide", "Subdivide", "Mesh", "polySubdivide", json::object(), "Splits the selected faces into smaller ones.");
    cmd("mesh.mirror", "Mirror", "Mesh", "polyMirror", json::object(), "Adds a mirrored, welded copy of the mesh, once. The Mirror modifier stays live.");
    cmd("mesh.separate", "Separate", "Mesh", "polySeparate", json::object(), "Splits a mesh into one object per disconnected piece.");
    cmd("mesh.combine", "Combine", "Mesh", "polyCombine", json::object(), "Merges the selected meshes into one object.");
    cmd("mesh.smartJoin", "Smart Join", "Mesh", "smartJoin", json::object(),
        "Joins the two selected meshes (a limb into a body) with a quad bridge, carving a matching hole; the option box previews live and rates the result.");
    cmd("mesh.bakeModifierStack", "Bake Modifier Stack", "Mesh", "bakeModifierStack", json::object(),
        "Bakes the generator and modifier stack into the selected meshes, leaving plain geometry.");
    cmd("mesh.transferAttributes", "Transfer Attributes", "Mesh", "transferAttributes", json::object(),
        "Copies UVs, normals or other attributes from the first selected mesh onto the others.");
    cmd("mesh.softFoliageNormals", "Soft Foliage Normals", "Mesh", "softFoliageNormals", json::object(),
        "Points the normals of leaf cards out from a dome around the mesh (mixed 80% by default), so foliage shades as soft clumps.");
    cmd("mesh.quadRemesh", "Quad Remesh", "Mesh", "quadRemesh", json::object(), "Rebuilds the mesh as evenly sized, flowing quads.");
    cmd("mesh.repair", "Repair Mesh", "Mesh", "meshRepair", json::object(),
        "Fixes coincident points, lamina and zero-area faces, non-manifold geometry and flipped faces; the option box picks the checks.");
    cmd("mesh.selectIssues", "Select Mesh Issues", "Mesh", "meshRepair", {{"mode", "select"}},
        "Selects what Repair Mesh would fix, without changing anything, and logs the counts per check.");
    cmd("mesh.voxelRemesh", "Voxel Remesh", "Mesh", "voxelRemesh", json::object(),
        "Rebuilds the mesh as one closed surface from a voxel grid, closing holes and fusing shells.");
    cmd("mesh.relax", "Relax", "Mesh", "relaxMesh", json::object(), "Evens out the spacing of the selected vertices, keeping the shape.");
    cmd("mesh.booleanDifference", "Boolean Difference", "Mesh", "polyBoolean", {{"operation", "difference"}},
        "Cuts the other selected meshes out of the first.");
    cmd("mesh.booleanUnion", "Boolean Union", "Mesh", "polyBoolean", {{"operation", "union"}},
        "Merges the selected meshes into one solid, removing the parts inside.");
    cmd("mesh.booleanIntersection", "Boolean Intersection", "Mesh", "polyBoolean", {{"operation", "intersection"}},
        "Keeps only the volume the selected meshes share.");
    for (const std::string& kind : scene::modifiers::kinds())
        cmd("modifier." + kind, modifierLabel(kind), "Modifiers", "addModifier", {{"kind", kind}}, scene::modifiers::kindDoc(kind));
    cmd("mesh.symmetryX", "Symmetry X", "Mesh", "setSymmetry", {{"axis", "x"}}, "Mirrors selections and edits across the object's X plane.");
    cmd("mesh.symmetryOff", "Symmetry Off", "Mesh", "setSymmetry", {{"axis", "off"}}, "Stops mirroring selections and edits.");
    cmd("mesh.symmetryPlaneFromSelection", "Symmetry Plane from Selection", "Mesh", "setSymmetryPlane", json::object(),
        "Puts the symmetry plane through the selected centre edges or vertices, on the axis they lie flat across.");
    cmd("mesh.symmetryPlaneFromPivot", "Symmetry Plane at Pivot", "Mesh", "setSymmetryPlane", {{"pivot", true}},
        "Puts the symmetry plane back through the object's pivot.");
    cmd("mesh.symmetryY", "Symmetry Y", "Mesh", "setSymmetry", {{"axis", "y"}}, "Mirrors selections and edits across the object's Y plane.");
    cmd("mesh.symmetryZ", "Symmetry Z", "Mesh", "setSymmetry", {{"axis", "z"}}, "Mirrors selections and edits across the object's Z plane.");
    cmd("mesh.symmetryRadial", "Symmetry Radial", "Mesh", "setSymmetry", {{"axis", "radial"}},
        "Repeats selections and edits round the object's radial axis, radialCount wedges (set in the Attribute Editor or the toolbar's symmetry menu).");
    cmd("mesh.symmetrizeRadial", "Symmetrize Radial", "Mesh", "symmetrizeRadial", json::object(),
        "Rebuilds the mesh exactly radially symmetric from its kept wedge, welded on the seams, and turns radial symmetry on. Model one wedge, then run this.");
    cmd("mesh.toggleSymmetry", "Toggle Symmetry", "Mesh", "toggleSymmetry", json::object(),
        "Turns symmetry on for the selected meshes (on the axis last used), or off when they all have it on.");
    cmd("mesh.symmetryCheck", "Symmetry Check", "Mesh", "symmetryCheck", json::object(),
        "Reports how far the mesh is from symmetric and how many vertices have no mirror partner.");
    cmd("mesh.symmetrizePositive", "Symmetrize (Keep + Side)", "Mesh", "symmetrize", {{"side", "positive"}},
        "Copies the + side of the mesh onto the - side.");
    cmd("mesh.symmetrizeNegative", "Symmetrize (Keep - Side)", "Mesh", "symmetrize", {{"side", "negative"}},
        "Copies the - side of the mesh onto the + side.");
    cmd("mesh.symmetryRebuild", "Rebuild Symmetry", "Mesh", "symmetryRebuild", json::object(),
        "Replaces the other side with a mirror of the kept one (the side of the selection, else the side holding most of the mesh), welded on the plane.");
    cmd("mesh.symmetryRepair", "Re-pair Symmetry", "Mesh", "symmetryRebuild", {{"pairsOnly", true}},
        "Pairs mirror vertices again from their positions, leaving the geometry as it is.");
    cmd("mesh.symmetryRebuildTopological", "Re-pair Symmetry from Edge", "Mesh", "symmetryRebuild", {{"topological", true}},
        "Pairs mirror vertices by topology, outward from the selected center edge. Works on posed or uneven meshes.");
    cmd("mesh.boundingMesh", "Bounding Mesh", "Mesh", "boundingMesh", json::object(),
        "Wraps the selection in a fitted box, sphere, capsule, cylinder or hull, named and parented as its Unreal collision.");
    cmd("mesh.multiBoxBounds", "Multi-Box Bounds", "Mesh", "multiBoxBounds", json::object(),
        "Splits the selected meshes into chunks that conform to boxes and creates one box (or hull) per chunk, named and parented as Unreal collision.");
    cmd("unreal.sendBake", "Send Bake to Unreal", "Unreal", "unrealSendTextures", json::object(),
        "Sends the bake project's textures to the connected Unreal editor, updating them in place, and wires them into the "
        "materials (instances of the unreal pref materialParent when it is set).");
    cmd("bake.assign", "Assign Bake to Materials", "Bake", "bakeAssign", json::object(),
        "Wires the last bake's textures into the materials of the lows they were baked for.");
    cmd("bake.all", "Bake All Projects", "Bake", "bakeAll", json::object(), "Bakes every bake project in the scene, one after another.");
    cmd("bake.allChanged", "Bake Changed Projects", "Bake", "bakeAll", {{"changedOnly", true}},
        "Bakes the bake projects whose inputs changed since their last bake.");
    cmd("mesh.extract", "Extract", "Mesh", "polyExtract", json::object(), "Moves the selected faces into a new object.");
    cmd("mesh.softenEdges", "Soften Normals", "Mesh", "polySoftEdge", {{"angle", 180.0}},
        "Makes the selected edges soft, or every edge of the selected meshes in object mode.");
    cmd("mesh.hardenEdges", "Harden Normals", "Mesh", "polySoftEdge", {{"angle", 0.0}},
        "Makes the selected edges hard, or every edge of the selected meshes in object mode.");
    cmd("mesh.setNormalAngle", "Soften by Angle", "Mesh", "polySoftEdge", {{"angle", 30.0}},
        "Hardens the selected edges whose faces meet sharper than the angle and softens the rest.");
    cmd("mesh.crease", "Crease Edges", "Mesh", "polyCrease", json::object(),
        "Creases the selected edges so the subdivision preview and Subdivide keep them sharp.");
    cmd("mesh.uncrease", "Uncrease Edges", "Mesh", "polyCrease", {{"value", 0.0}}, "Removes the crease from the selected edges.");
    cmd("mesh.bevelWeight", "Bevel Weight", "Mesh", "polyBevelWeight", json::object(),
        "Sets the bevel weight of the selected edges; Auto Bevel in weight mode bevels them.");
    cmd("mesh.duplicateFaces", "Duplicate Faces", "Mesh", "polyDuplicateFaces", json::object(),
        "Copies the selected faces into a separate shell of the same mesh and selects the copies.");
    cmd("mesh.connect", "Connect", "Mesh", "polyConnect", json::object(), "Splits faces with new edges between the selected vertices or edges.");
    cmd("select.shrink", "Shrink Selection", "Select", "shrinkSelection", json::object(), "Drops the components on the selection's border.");
    cmd("select.border", "Select Border Edges", "Select", "selectBorder", json::object(), "Selects the open border edges of the selected meshes.");
    cmd("select.byAngle", "Select by Angle", "Select", "selectByAngle", json::object(), "Selects edges whose faces meet sharper than an angle.");
    actions::add({.id = "select.toggleCameraBased", .label = "Camera-Based Selection", .category = "Select", .run = [] {
                      bool on = PrefStore::get().value<bool>("general", "/select/cameraBased", false);
                      PrefStore::get().set("general", "/select/cameraBased", !on);
                  },
                  .doc = "Component marquee and lasso selection skip vertices, edges and faces hidden behind other surfaces. Off, they "
                         "select through the mesh. Clicks always pick the nearest component.",
                  .python = "lila.prefs.set(\"general\", \"/select/cameraBased\", not lila.prefs.get(\"general\", \"/select/cameraBased\", False))"});
    actions::add({.id = "select.toggleBackfaceCulling", .label = "Backface Culling Selection", .category = "Select", .run = [] {
                      bool on = PrefStore::get().value<bool>("general", "/select/backfaceCulling", false);
                      PrefStore::get().set("general", "/select/backfaceCulling", !on);
                  },
                  .doc = "Clicks and marquees skip components on faces turned away from the camera.",
                  .python = "lila.prefs.set(\"general\", \"/select/backfaceCulling\", not lila.prefs.get(\"general\", \"/select/backfaceCulling\", False))"});
    for (auto [id, label, mode, doc] : {
             std::tuple{"select.edgesByCenter", "Select Edges by Center", scene::ComponentMode::Edge, "A click picks the edge whose midpoint is nearest, not the line under the cursor."},
             {"select.facesByCenter", "Select Faces by Center", scene::ComponentMode::Face, "A click picks the face whose center is nearest, not the face under the cursor."},
         })
    {
        const std::string key = scene::componentPick::selectByCenterKey(mode);
        actions::add({.id = id, .label = label, .category = "Select", .run = [key] {
                          bool on = PrefStore::get().value<bool>("general", key, false);
                          PrefStore::get().set("general", key, !on);
                      },
                      .doc = doc,
                      .python = "lila.prefs.set(\"general\", \"" + key + "\", not lila.prefs.get(\"general\", \"" + key + "\", False))"});
    }
    cmd("select.containedFaces", "To Contained Faces", "Select", "selectContainedFaces", json::object(),
        "Selects the faces enclosed by the selected edges, flood filled up to them and the mesh borders.");

    for (auto [id, label, command, doc] : {
             std::tuple{"create.cube", "Cube", "polyCube", "Creates a box."},
             {"create.sphere", "Sphere", "polySphere", "Creates a UV sphere, quads with triangles at the poles."},
             {"create.cylinder", "Cylinder", "polyCylinder", "Creates a capped cylinder."},
             {"create.capsule", "Capsule", "polyCapsule", "Creates a cylinder with rounded ends."},
             {"create.cone", "Cone", "polyCone", "Creates a cone with a capped base."},
             {"create.plane", "Plane", "polyPlane", "Creates a flat grid of quads."},
             {"create.torus", "Torus", "polyTorus", "Creates a ring-shaped tube."},
             {"create.disc", "Disc", "polyDisc", "Creates a flat round disc."},
             {"create.pipe", "Pipe", "polyPipe", "Creates a hollow tube with wall thickness."},
             {"create.quadSphere", "Quad Sphere", "polyQuadSphere", "Creates an all-quad sphere from a subdivided cube, with no poles."},
             {"create.icoSphere", "Ico Sphere", "polyIcoSphere", "Creates an evenly triangulated sphere from a subdivided icosahedron."},
         })
        cmd(id, label, "Create", command, json::object(), std::string(doc) + " Its size and subdivisions stay editable in the channel box INPUTS.");
    auto display = [](const char* id, const char* label, const char* mode, const char* doc) {
        actions::add({.id = id, .label = label, .category = "Display",
                      .run = [mode] { PrefStore::get().set("general", "/viewport/displayMode", mode); },
                      .doc = doc,
                      .python = std::string("lila.prefs.set(\"general\", \"/viewport/displayMode\", \"") + mode + "\")"});
    };
    display("viewport.wireframe", "Wireframe", "wireframe", "Draws meshes as their edges only.");
    actions::add({.id = "viewport.shaded", .label = "Shaded", .category = "Display",
                  .run =
                      [] {
                          PrefStore::get().set("general", "/viewport/displayMode", "shaded");
                          setMaterialsShown(false);
                      },
                  .doc = "Draws meshes as lit surfaces in flat gray, for form checks.",
                  .python = "lila.prefs.set(\"general\", \"/viewport/displayMode\", \"shaded\")\n"
                            "lila.prefs.set(\"general\", \"/viewport/materials\", False)"});
    actions::add({.id = "viewport.wireOnShaded", .label = "Wireframe on Shaded", .category = "Display", .run = [] {
                      bool on = PrefStore::get().value<bool>("general", "/viewport/wireOnShaded", false);
                      PrefStore::get().set("general", "/viewport/wireOnShaded", !on);
                  },
                  .doc = "Draws mesh edges over the shaded surfaces."});
    actions::add({.id = "viewport.toggleAO", .label = "Ambient Occlusion", .category = "Display", .run = [] {
                      bool on = PrefStore::get().value<bool>("general", "/viewport/ao/enabled", true);
                      PrefStore::get().set("general", "/viewport/ao/enabled", !on);
                  },
                  .doc = "Darkens creases and contact areas, so forms read better.",
                  .python = "lila.prefs.set(\"general\", \"/viewport/ao/enabled\", not lila.prefs.get(\"general\", \"/viewport/ao/enabled\", True))"});

    namespace vd = viewportDisplay;
    auto togglePref = [](const char* id, const char* label, const char* key, bool fallback, const char* doc) {
        std::string k = key;
        std::string fb = fallback ? "True" : "False";
        actions::add({.id = id, .label = label, .category = "Display",
                      .run = [k, fallback] { PrefStore::get().set("general", k, !PrefStore::get().value<bool>("general", k, fallback)); },
                      .doc = doc,
                      .python = "lila.prefs.set(\"general\", \"" + k + "\", not lila.prefs.get(\"general\", \"" + k + "\", " + fb + "))",
                      .checked = [k, fallback] { return PrefStore::get().value<bool>("general", k, fallback); }});
    };
    togglePref("viewport.toggleAA", "Antialiasing", vd::kAaEnabled, true, "Multisample antialiasing in every viewport.");
    togglePref("viewport.toggleRtShadows", "Ray-Traced Shadows", vd::kRtShadows, true,
               "Shadows traced against the scene for up to four lights: exact contacts, soft by each light's size (GPUs with ray tracing).");
    togglePref("viewport.toggleSafeFrames", "Safe Frames", vd::kSafeFrames, false,
               "Action and title safe frames inside the resolution gate (turn the gate on to see them).");
    togglePref("viewport.toggleThirds", "Rule of Thirds", vd::kThirds, false, "Thirds lines inside the resolution gate.");
    togglePref("viewport.togglePostEffects", "Post Effects", vd::kPostEffects, true,
               "The scene's post process grading, bloom and lens effects in every viewport.");
    togglePref("viewport.toggleTAA", "Temporal Antialiasing", vd::kTaa, true,
               "Smooths shading and texture shimmer as the view moves, and supersamples the view while it holds still.");
    togglePref("viewport.toggleGrid", "Grid", vd::kGrid, true, "The ground grid in every viewport.");
    togglePref("viewport.toggleXray", "X-Ray", vd::kXray, false, "Shaded meshes become see-through.");
    togglePref("viewport.toggleBackfaceCulling", "Backface Culling", vd::kBackfaceCulling, false, "Hides the back faces of shaded meshes.");
    togglePref("viewport.toggleKuwahara", "Painterly Filter", vd::kKuwahara, false,
               "Smooths the shaded view into flat, brushy patches that follow its edges (anisotropic Kuwahara). Viewport preview only.");
    togglePref("viewport.toggleOutline", "Outlines", vd::kOutline, false,
               "Draws dark lines where depth or surface direction jumps, like inked edges. Viewport preview only.");
    togglePref("viewport.toggleShadows", "Shadows", vd::kShadows, true, "The soft ground shadow under meshes.");
    togglePref("viewport.toggleKeys", "Keys", kShowKeysPref, true,
               "Key hints on screen: the active tool's keys in its readout and corner list, and the layout's keys in the corner.");
    togglePref("viewport.smoothSubdivisions", "Show Smooth Subdivisions", vd::kSmoothSubdivisions, false,
               "Smooth preview draws the edges of every subdivided face, not just the edges of the mesh.");
    togglePref("viewport.defaultMaterial", "Use Default Material", vd::kDefaultMaterial, false, "Shades every mesh with the plain default material.");
    togglePref("viewport.greyBaseColor", "Grey Base Color", vd::kGreyBaseColor, false,
               "Materials shade with a 50% grey base color, keeping their normal, packed and other maps, opacity and shading model.");
    togglePref("viewport.toggleEnvBackground", "Environment Background", "/viewport/env/background", true, "Shows the HDRI behind the scene.");
    togglePref("viewport.toggleToolbar", "Show Viewport Header", vd::kToolbar, true,
               "The row at the top of each viewport panel: component mode, shading, symmetry, soft select, snapping, overlays and camera.");
    togglePref("viewport.toggleHighlightSelection", "Highlight Selection", vd::kHighlightSelection, true,
               "Selected objects draw highlighted. Turn off to watch a selected mesh change without the highlight in the way; "
               "component modes still show their components.");
    togglePref("viewport.togglePreselectObjects", "Highlight Objects Under Cursor", "/viewport/preselect/objects", false,
               "In object mode, the object under the cursor highlights before it is clicked.");
    togglePref("select.toggleSymmetric", "Symmetric Selection", "/select/symmetric", true,
               "With symmetry on, clicking, dragging and loop picks select the mirror images too.");
    togglePref("viewport.toggleSymmetryLine", "Show Symmetry Line", vd::kSymmetryEdges, true,
               "With symmetry on, the edges on the mirror plane draw in red.");
    cmd("select.symmetrize", "Symmetrize Selection", "Select", "symmetrizeSelection", json::object(),
        "Adds the mirror image of the component selection under the mesh's symmetry.");
    togglePref("viewport.togglePreselectComponents", "Highlight Components Under Cursor", "/viewport/preselect/components", true,
               "In component modes, the vertex, edge or face under the cursor highlights before it is clicked.");
    for (int n : {2, 4, 8})
    {
        std::string label = "Antialiasing " + std::to_string(n) + "x";
        actions::add({.id = "viewport.aaSamples" + std::to_string(n), .label = label, .category = "Display", .run = [n] {
                          PrefStore::get().set("general", vd::kAaSamples, n);
                          PrefStore::get().set("general", vd::kAaMode, "multisample");
                          PrefStore::get().set("general", vd::kAaEnabled, true);
                      },
                      .doc = std::to_string(n) + " samples per pixel. More is smoother and costs more GPU time.",
                      .python = "lila.prefs.set(\"general\", \"/viewport/aa/samples\", " + std::to_string(n) + ")"});
    }
    actions::add({.id = "viewport.aaTemporal", .label = "Temporal Antialiasing Only", .category = "Display", .run = [] {
                      PrefStore::get().set("general", vd::kAaMode, vd::kAaTemporal);
                      PrefStore::get().set("general", vd::kAaEnabled, true);
                  },
                  .doc = "One sample a frame, refined once the view stops. Dense scenes draw about twice as fast while they move.",
                  .python = "lila.prefs.set(\"general\", \"/viewport/aa/mode\", \"temporal\")"});
    for (auto [mode, label, doc] : {std::tuple{"environment", "Environment Lighting", "Lights the scene with the HDRI environment."},
                                    {"studio", "Studio Lighting", "Lights the scene with a fixed studio rig, whatever the HDRI."},
                                    {"sculpt", "Sculpt Lighting",
                                     "One grazing key light over little fill, so surface detail reads while sculpting. Shift+Alt+right-drag turns it."},
                                    {"flat", "Flat Lighting", "Unlit base colors, for checking textures and silhouettes."}})
    {
        std::string m = mode;
        actions::add({.id = "viewport.lighting." + m, .label = label, .category = "Display",
                      .run = [m] { PrefStore::get().set("general", vd::kLighting, m); },
                      .doc = doc,
                      .python = "lila.prefs.set(\"general\", \"/viewport/lighting\", \"" + m + "\")"});
    }
    actions::add({.id = "viewport.resetLight", .label = "Reset Light", .category = "Display", .run = [] { vd::resetLight(); },
                  .doc = "Puts the light back where it starts for the lighting mode in use: the HDRI unturned, or the studio or sculpt rig "
                         "at its default rotation and angle. Shift+Alt+right-click in the viewport does the same.",
                  .python = "lila.ui.run_action(\"viewport.resetLight\")"});
    actions::add({.id = "viewport.toggleSculptLighting", .label = "Sculpt Lighting", .category = "Display", .run = [] {
                      PrefStore& p = PrefStore::get();
                      const std::string mode = p.value<std::string>("general", vd::kLighting, "environment");
                      if (mode == "sculpt")
                          p.set("general", vd::kLighting, p.value<std::string>("general", vd::kLightingBeforeSculpt, "environment"));
                      else
                      {
                          p.set("general", vd::kLightingBeforeSculpt, mode);
                          p.set("general", vd::kLighting, std::string("sculpt"));
                      }
                  },
                  .doc = "Switches the viewport to sculpt lighting, or back to the lighting it had before.",
                  .python = "lila.ui.run_action(\"viewport.toggleSculptLighting\")",
                  .checked = [] { return PrefStore::get().value<std::string>("general", vd::kLighting, "environment") == "sculpt"; }});
    actions::add({.id = "viewport.toggleLighting", .label = "Scene Lights", .category = "Display", .run = [] {
                      PrefStore& p = PrefStore::get();
                      p.set("general", vd::kSceneLights, !p.value<bool>("general", vd::kSceneLights, true));
                  },
                  .doc = "Turns the scene's lights off and on in the viewport, as if hidden. The environment still lights.",
                  .python = "lila.prefs.set(\"general\", \"/viewport/sceneLights\", False)"});
    for (auto [mode, label, doc] : {std::tuple{"off", "Hide Vertex Colors", "Meshes draw without their vertex colors."},
                                    {"shaded", "Vertex Colors (Shaded)", "Multiplies each mesh's current color set into its lit base color."},
                                    {"unlit", "Vertex Colors (Unlit)", "Shows each mesh's current color set flat, without lighting."}})
    {
        std::string m = mode;
        actions::add({.id = "viewport.vertexColors." + m, .label = label, .category = "Display",
                      .run = [m] { PrefStore::get().set("general", vd::kVertexColors, m); },
                      .doc = doc,
                      .python = "lila.prefs.set(\"general\", \"/viewport/vertexColors\", \"" + m + "\")"});
    }
    actions::add({.id = "viewport.toggleVertexColors", .label = "Vertex Colors", .category = "Display", .run = [] {
                      bool on = PrefStore::get().value<std::string>("general", vd::kVertexColors, "off") != "off";
                      PrefStore::get().set("general", vd::kVertexColors, on ? "off" : "shaded");
                  },
                  .doc = "Shows or hides every mesh's current color set (shaded)."});
    for (auto [channel, label, doc] : {std::tuple{"rgb", "All Channels", "Vertex colors show in full color."},
                                       {"r", "Red Channel", "Shows only the red channel, as grayscale."},
                                       {"g", "Green Channel", "Shows only the green channel, as grayscale."},
                                       {"b", "Blue Channel", "Shows only the blue channel, as grayscale."},
                                       {"a", "Alpha Channel", "Shows only the alpha channel, as grayscale."}})
    {
        std::string c = channel;
        actions::add({.id = "viewport.colorChannel." + c, .label = label, .category = "Display",
                      .run = [c] {
                          PrefStore::get().set("general", vd::kColorChannel, c);
                          if (PrefStore::get().value<std::string>("general", vd::kVertexColors, "off") == "off")
                              PrefStore::get().set("general", vd::kVertexColors, "unlit");
                      },
                      .doc = std::string(doc) + " Unreal materials often read single channels as masks.",
                      .python = "lila.prefs.set(\"general\", \"/viewport/colorChannel\", \"" + c + "\")"});
    }
    togglePref("viewport.grayscaleWeights", "Grayscale Weight Display", vd::kGrayscaleWeights, false,
               "Skin weights and the soft select falloff draw as a gray ramp instead of colours.");
    togglePref("viewport.monotone", "Monotone", vd::kMonotone, false,
               "No colour anywhere: the view axes, manipulator axes, time slider keys and graph editor curves all draw in grays. "
               "Off, each follows its own colour preference.");
    {
        namespace sm = scene::selectionMask;
        for (auto [kind, label, doc] : {std::tuple{sm::Kind::Curve, "Select Curves", "Curves can be picked in object mode."},
                                        {sm::Kind::Mesh, "Select Polygon Objects", "Meshes can be picked in object mode."},
                                        {sm::Kind::Joint, "Select Joints", "Joints can be picked in object mode."},
                                        {sm::Kind::Light, "Select Lights", "Lights can be picked in object mode."},
                                        {sm::Kind::Locator, "Select Locators", "Locators can be picked in object mode."},
                                        {sm::Kind::Other, "Select Other Objects", "Cameras, image planes and measurements can be picked in object mode."}})
        {
            sm::Kind k = kind;
            std::string key = sm::prefKey(k);
            actions::add({.id = std::string("select.mask.") + sm::name(k), .label = label, .category = "Select",
                          .run = [k] { sm::setEnabled(k, !sm::enabled(k)); },
                          .doc = std::string(doc) + " Off masks them out of clicks, marquees and the highlight under the cursor.",
                          .python = "lila.prefs.set(\"general\", \"" + key + "\", not lila.prefs.get(\"general\", \"" + key + "\", True))"});
        }
    }
    {
        namespace sm = scene::selectionMask;
        for (auto [kind, label, doc] : {std::tuple{sm::Kind::Curve, "Show Curves", "Curves draw in the viewport."},
                                        {sm::Kind::Mesh, "Show Geometry", "Meshes draw in the viewport."},
                                        {sm::Kind::Joint, "Show Joints", "Joints draw in the viewport."},
                                        {sm::Kind::Light, "Show Lights", "Light icons draw in the viewport."},
                                        {sm::Kind::Locator, "Show Locators", "Locators draw in the viewport."}})
        {
            sm::Kind k = kind;
            std::string key = sm::showKey(k);
            actions::add({.id = std::string("viewport.show.") + sm::name(k), .label = label, .category = "Display",
                          .run = [k] { sm::setShown(k, !sm::shown(k)); },
                          .doc = std::string(doc) + " Off hides them and leaves them unpickable, without changing their visibility.",
                          .python = "lila.prefs.set(\"general\", \"" + key + "\", not lila.prefs.get(\"general\", \"" + key + "\", True))"});
        }
    }
    actions::add({.id = "viewport.resetExposure", .label = "Reset Exposure and Gamma", .category = "Display", .run = [] {
                      PrefStore::get().set("general", vd::kExposure, 0.0);
                      PrefStore::get().set("general", vd::kGamma, 1.0);
                  },
                  .doc = "Puts the viewport exposure back to 0 and gamma to 1."});
}
} // namespace lila::ui::actions
