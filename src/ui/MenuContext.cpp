#include "ui/MenuContext.h"

#include "core/Events.h"
#include "scene/Graph.h"
#include "sculpt/SculptSession.h"
#include "ui/Actions.h"

#include <QApplication>
#include <QDockWidget>
#include <QMainWindow>
#include <QPointer>

#include <cctype>
#include <map>

namespace lila::ui::menuContext
{
namespace
{
struct State
{
    std::string layoutSet = "Model";
    std::string pinned;
    std::string tool;
    std::string editor;
    std::string selection;
    std::string shown;
    std::vector<std::function<void()>> listeners;
};

State& state()
{
    static State s;
    return s;
}

void notify()
{
    State& s = state();
    const std::string now = current();
    if (now == s.shown)
        return;
    s.shown = now;
    for (const auto& fn : s.listeners)
        fn();
}

/** A selection of joints only, outside the layouts that already show the rigging menus. */
std::string setForSelection()
{
    const scene::Graph& g = scene::Graph::current();
    const auto& sel = g.selection();
    if (sel.empty())
        return {};
    for (scene::NodeId id : sel)
    {
        const scene::Node* n = g.node(id);
        if (!n || n->type().name != "joint")
            return {};
    }
    return "Rig";
}

std::string dockOf(QWidget* w)
{
    for (; w; w = w->parentWidget())
        if (auto* dock = qobject_cast<QDockWidget*>(w))
            return dock->objectName().toStdString();
    return {};
}
} // namespace

const std::vector<std::string>& sets()
{
    static const std::vector<std::string> all = {"Model", "Sculpt", "UV", "Texture", "Rig", "Animate", "Level",
                                                 "Cloth", "Groom", "Effects", "Print", "Script"};
    return all;
}

std::string setForTool(const std::string& tool)
{
    static const std::map<std::string, std::string> kTools = {
        {"sculpt", "Sculpt"},          {"Clay", "Sculpt"},           {"Clay Ring", "Sculpt"},
        {"Sculpt Terrain", "Level"},   {"stagehandKitPlace", "Level"}, {"stagehandPush", "Level"},
        {"Physics Live", "Level"},     {"Ruin Brush", "Level"},      {"Pipe", "Level"},
        {"Pipe Route", "Level"},       {"texturePaint", "Texture"},  {"bakePaint", "Texture"},
        {"vertexPaint", "Texture"},    {"uvBrush", "UV"},            {"weightPaint", "Rig"},
        {"jointTool", "Rig"},          {"Grab Cloth", "Cloth"},      {"Draw Panel", "Cloth"},
        {"Draw Hair", "Groom"},        {"Retopo Draw", "Model"},     {"Retopo Poles", "Model"},
        {"Topology", "Model"},
    };
    auto it = kTools.find(tool);
    return it == kTools.end() ? std::string() : it->second;
}

std::string setForDock(const std::string& dock)
{
    static const std::map<std::string, std::string> kDocks = {
        {"uvEditor", "UV"},
        {"graphEditor", "Animate"},
        {"timeSlider", "Animate"},
        {"animLayers", "Animate"},
        {"poseLibrary", "Animate"},
        {"controlPicker", "Animate"},
        {"offsets", "Animate"},
        {"retargeter", "Animate"},
        {"puppeteerStacker", "Animate"},
        {"puppeteerMixer", "Animate"},
        {"puppeteerAnimBank", "Animate"},
        {"puppeteerCycleMaker", "Animate"},
        {"baker", "Texture"},
        {"textureSets", "Texture"},
        {"textureGraph", "Texture"},
        {"texture2d", "Texture"},
        {"materialEditor", "Texture"},
        {"materialBrowser", "Texture"},
        {"trimSheetEditor", "Texture"},
        {"scriptEditor", "Script"},
        {"scriptOutput", "Script"},
        {"terminal", "Script"},
        {"apiReference", "Script"},
        {"tailorPatternEditor", "Cloth"},
        {"groomPalette", "Groom"},
        {"levelToolkit", "Level"},
        {"stagehand.library", "Level"},
        {"terrain.sculptPalette", "Level"},
        {"clayPalette", "Sculpt"},
        {"modelingToolkit", "Model"},
        {"topologyToolkit", "Model"},
        {"curveToolkit", "Model"},
        {"modifierStack", "Model"},
        {"rigTemplatesPanel", "Rig"},
        {"morphTargets", "Rig"},
        {"drivenChannels", "Rig"},
    };
    auto it = kDocks.find(dock);
    return it == kDocks.end() ? std::string() : it->second;
}

std::string current()
{
    const State& s = state();
    if (!s.pinned.empty())
        return s.pinned;
    if (!s.tool.empty())
        return s.tool;
    // Sculpt mode outlasts the brush: Move or Select while sculpting keeps the sculpt menus.
    if (sculpt::active())
        return "Sculpt";
    if (!s.editor.empty())
        return s.editor;
    // A joint selection means rigging, except where the layout already shows the rigging menus.
    if (!s.selection.empty() && s.layoutSet != "Animate" && s.layoutSet != "Rig")
        return s.selection;
    return s.layoutSet;
}

void setLayoutSet(const std::string& set)
{
    State& s = state();
    s.layoutSet = set.empty() ? "Model" : set;
    s.pinned.clear();
    s.editor.clear();
    notify();
}

std::string layoutSet()
{
    return state().layoutSet;
}

void pin(const std::string& set)
{
    state().pinned = set;
    notify();
}

void unpin()
{
    state().pinned.clear();
    notify();
}

std::string pinned()
{
    return state().pinned;
}

void onChanged(std::function<void()> fn)
{
    state().listeners.push_back(std::move(fn));
}

void install(QMainWindow* window)
{
    events::subscribe(events::Event::ViewportToolChanged, [](const events::EventData& e) {
        state().tool = setForTool(e.name);
        notify();
    });
    sculpt::addListener([] { notify(); });
    events::subscribe(events::Event::SelectionChanged, [](const events::EventData&) {
        state().selection = setForSelection();
        notify();
    });
    QPointer<QMainWindow> main(window);
    QObject::connect(qApp, &QApplication::focusChanged, window, [main](QWidget*, QWidget* now) {
        if (!now || !main || now->window() != main)
            return;
        // Focus in a panel names its set; focus anywhere else that is not a panel (the viewport) clears it.
        const std::string dock = dockOf(now);
        state().editor = dock.empty() ? std::string() : setForDock(dock);
        notify();
    });

    // F3 to F7 pin the common sets; pressing the pinned set's key again goes back to following the context.
    for (const std::string& set : sets())
    {
        std::string id = "menuSet.";
        for (char c : set)
            id += char(std::tolower(static_cast<unsigned char>(c)));
        actions::add({.id = id,
                      .label = set + " Menus",
                      .category = "Windows",
                      .run = [set] { pinned() == set ? unpin() : pin(set); },
                      .enabled = {},
                      .doc = "Shows the " + set + " menus on the menu bar until pressed again or a layout is applied. The hotbox and "
                             "Ctrl+T reach every menu whatever the menu bar shows.",
                      .checked = [set] { return pinned() == set; }});
    }
    actions::add({.id = "menuSet.follow",
                  .label = "Menus Follow Context",
                  .category = "Windows",
                  .run = [] { unpin(); },
                  .enabled = {},
                  .doc = "Unpins the menu set: the menu bar follows the active tool, the focused panel and the selection.",
                  .checked = [] { return pinned().empty(); }});
    state().shown = current();
}
} // namespace lila::ui::menuContext
