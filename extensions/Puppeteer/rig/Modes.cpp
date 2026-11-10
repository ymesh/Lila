// Puppeteer's two modes. Rig mode builds characters (joints, IK chains, feet, lookers, spaces); Animate mode poses the
// joints directly: clicking the mesh picks the joint under it, the Spinner moves it, and a Space tap opens the Curve
// Overlay over the viewport while a Space hold is still the hotbox.

#include "Puppeteer.h"
#include "rig/Rig.h"

#include <lila/Events.h>
#include <lila/Poses.h>
#include <lila/Undo.h>
#include <lila/UI.h>
#include <lila/Viewport.h>

#include <QTimer>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace puppeteer
{
namespace
{
// Each mode and its layout are bound: entering the mode shows the layout, and picking the layout enters the mode.
constexpr const char* kRigLayout = "Puppeteer Rig";
constexpr const char* kAnimateLayout = "Puppeteer";
constexpr const char* kShelf = "Puppeteer";
constexpr const char* kBaseSelection = "lila.dominantInfluence";

// Selection -----------------------------------------------------------------------------------------------------------

std::optional<std::vector<std::string>> baseRedirect(const selectionHandlers::Hit& hit)
{
    const selectionHandlers::Handler* base = selectionHandlers::find(kBaseSelection);
    return base && base->redirect ? base->redirect(hit) : std::nullopt;
}

std::optional<selectionHandlers::Highlight> baseHover(const selectionHandlers::Hit& hit)
{
    const selectionHandlers::Handler* base = selectionHandlers::find(kBaseSelection);
    return base && base->hover ? base->hover(hit) : std::nullopt;
}

std::vector<selectionHandlers::Candidate> baseCandidates(const selectionHandlers::Hit& hit)
{
    const selectionHandlers::Handler* base = selectionHandlers::find(kBaseSelection);
    return base && base->candidates ? base->candidates(hit) : std::vector<selectionHandlers::Candidate>{};
}

/** An IK chain stands for its controller (its reverse foot or handle); a joint is always itself, posed FK. */
std::string animateTarget(Graph& g, const std::string& name)
{
    Node* n = named(g, name);
    if (n && n->type().isA(kIkChainType))
    {
        const NodeId goal = ikGoalOf(g, n->id());
        return goal ? g.node(goal)->path() : name;
    }
    return name;
}

/** The RMB joint menu picks as a click would: a chain stands for its controller. */
std::vector<selectionHandlers::Candidate> animateCandidates(const selectionHandlers::Hit& hit)
{
    std::vector<selectionHandlers::Candidate> out = baseCandidates(hit);
    for (selectionHandlers::Candidate& c : out)
        c.name = animateTarget(Graph::current(), c.name);
    return out;
}

std::optional<std::vector<std::string>> animateRedirect(const selectionHandlers::Hit& hit)
{
    Graph& g = Graph::current();
    std::vector<std::string> names;
    if (auto base = baseRedirect(hit))
        names = *base;
    else if (hit.node != kNoNode && g.node(hit.node) && (rig::isJoint(*g.node(hit.node)) || g.node(hit.node)->type().isA(kIkChainType)))
        names = {g.node(hit.node)->path()};
    else
        return std::nullopt;
    for (std::string& n : names)
        n = animateTarget(g, n);
    return names;
}

/**
 * Saves the skeleton's skinned bind pose as the scene's "bind" pose, so it wins over a template's, which only matches
 * skeletons built from that template. The joints go back to how they were; nothing lands on the undo stack.
 */
void captureBindPose(Graph& g, NodeId top, const std::vector<std::string>& meshes)
{
    const std::vector<NodeId> nodes = poses::characterOf(g, top).nodes;
    if (nodes.empty())
        return;
    UndoStack& undo = UndoStack::get();
    const bool was = undo.enabled();
    undo.setEnabled(false);
    const poses::Pose now = poses::capture(g, nodes);
    bool bound = false;
    for (const std::string& mesh : meshes)
    {
        try
        {
            ops::execute("goToBindPose", {{"args", {mesh}}}, CallSource::Script);
            bound = true;
        }
        catch (const std::exception&)
        {
        }
    }
    if (bound)
    {
        g.evaluate();
        try
        {
            ops::execute("savePose", {{"args", {"bind"}}, {"tier", "scene"}, {"nodes", {g.node(top)->path()}}, {"hierarchy", true}},
                         CallSource::Script);
        }
        catch (const std::exception& e)
        {
            log::write(LogLevel::Warning, std::string("Bind pose not saved: ") + e.what());
        }
    }
    poses::ApplyOptions back;
    back.scope = nodes;
    poses::apply(g, now, back);
    g.evaluate();
    undo.setEnabled(was);
}

/**
 * A skeleton being animated becomes a Puppeteer character the first time one of its joints is selected in Animate mode,
 * with its helper joints redirected to their limbs, so Strings, poses and mesh picking work without a Rig step.
 */
void ensureCharacter()
{
    if (modes::current() != kAnimateMode)
        return;
    Graph& g = Graph::current();
    for (NodeId id : g.selection())
    {
        Node* n = g.node(id);
        if (!n || !rig::isJoint(*n) || characterFor(g, id))
            continue;
        NodeId top = id;
        for (NodeId p = g.parent(top); p && g.node(p) && rig::isJoint(*g.node(p)); p = g.parent(p))
            top = p;
        // Only a skeleton that deforms a mesh is something to animate; a lone joint stays a plain joint.
        const std::vector<std::string> meshes = skeletonMeshes(g, top);
        if (meshes.empty())
            return;
        const std::string root = g.node(top)->path();
        // Named after the group holding the skeleton (an imported SKM_Manny) when there is one, else the root joint.
        const NodeId holder = g.parent(top);
        const std::string name = holder && g.node(holder) ? g.node(holder)->name() : g.node(top)->name();
        json selected = json::array();
        for (NodeId s : g.selection())
            if (g.node(s))
                selected.push_back(g.node(s)->path());
        // Hand and foot controllers from the matching rig template, so limbs have something to grab, lock and pin.
        if (ops::find("rigFromTemplate"))
        {
            try
            {
                ops::execute("rigFromTemplate", {{"args", {root}}}, CallSource::UI);
            }
            catch (const std::exception& e)
            {
                log::write(LogLevel::Info, std::string("No template rig for ") + root + ": " + e.what());
            }
        }
        const json made = runCommand("puppeteerCharacter", {{"args", {root}}, {"name", name + "Character"}});
        if (!made.is_string())
            return;
        runCommand("autoSelectAs", {{"args", {root}}});
        captureBindPose(g, top, meshes);
        runCommand("select", {{"args", selected}});
        viewportToast::show(QString("%1 is now a Puppeteer character").arg(QString::fromStdString(name)));
        return;
    }
}

// Registration --------------------------------------------------------------------------------------------------------

json preset(std::vector<std::string> open, std::vector<std::string> raise, std::string menuSet, json size = json::object(),
            json columns = json::array())
{
    json p{{"open", open}, {"raise", raise}, {"size", std::move(size)}};
    if (!columns.empty())
        p["columns"] = std::move(columns);
    return {{"version", 1},
            {"preset", std::move(p)},
            {"menuSet", std::move(menuSet)},
            {"viewport", {{"quad", false}, {"camera", "persp"}}}};
}

void registerModeActions(ext::Context& ctx)
{
    const char* rigCat = "Puppeteer Rig";
    const char* animCat = "Puppeteer Animate";
    actions::add(ctx, {.id = "puppeteer.rig.character", .label = "Create Character", .category = rigCat,
                       .doc = "Make a Puppeteer character for the selected skeleton, finding its meshes, template and poses.",
                       .command = "puppeteerCharacter"});
    actions::add(ctx, {.id = "puppeteer.rig.ikChain", .label = "Create IK Chain", .category = rigCat,
                       .doc = "Select the start joint, then the end effector: an IK chain whose controller poses the joints' rotations.",
                       .command = "puppeteerIkChain"});
    actions::add(ctx, {.id = "puppeteer.rig.reverseFoot", .label = "Create Reverse Foot", .category = rigCat,
                       .doc = "Select an ankle with an IK chain: a sole control with heel, ball and toe roll, twist and bank.",
                       .command = "puppeteerReverseFoot"});
    actions::add(ctx, {.id = "puppeteer.rig.looker", .label = "Create Looker", .category = rigCat,
                       .doc = "Select a joint (a head, an eye): a target it aims at.", .command = "puppeteerLooker"});
    actions::add(ctx, {.id = "puppeteer.rig.space", .label = "Create Spaces", .category = rigCat,
                       .run =
                           [] {
                               Graph& g = Graph::current();
                               std::vector<std::string> sel;
                               for (NodeId id : g.selection())
                                   sel.push_back(g.node(id)->path());
                               if (sel.size() < 2)
                               {
                                   log::write(LogLevel::Warning, "Create Spaces: select the spaces, then the joint last");
                                   return;
                               }
                               json spaces = json::array();
                               for (size_t i = 0; i + 1 < sel.size(); ++i)
                                   spaces.push_back(sel[i]);
                               runCommand("puppeteerSpace", {{"args", {sel.back()}}, {"spaces", spaces}});
                           },
                       .doc = "Select the spaces, then the joint last: the joint can then be keyed in any of them."});

    actions::add(ctx, {.id = "puppeteer.anim.lock", .label = "Lock or Unlock Key", .category = animCat,
                       .doc = "Plant the selected controllers' key at this frame, or free it: a locked foot or hand holds until the next key.",
                       .command = "puppeteerFootLock"});
    actions::add(ctx, {.id = "puppeteer.anim.pin", .label = "Pin or Unpin Joints", .category = animCat,
                       .doc = "Pin the selected joints so full-body moves hold them where they are, or unpin them.",
                       .command = "puppeteerPin"});
    actions::add(ctx, {.id = "puppeteer.anim.matchControllers", .label = "Match Controllers to Pose", .category = animCat,
                       .doc = "Put the selected IK controllers on the joints' current pose.", .command = "ikMatch"});
    actions::add(ctx, {.id = "puppeteer.anim.snapFoot", .label = "Snap Foot to Sole", .category = animCat,
                       .doc = "Put the selected reverse feet on the sole of the pose, roll, twist and bank zeroed.",
                       .command = "puppeteerSnapReverseFoot"});
    actions::add(ctx, {.id = "puppeteer.anim.alignFoot", .label = "Align Foot Level", .category = animCat,
                       .doc = "Level the selected reverse feet, keeping the pose in roll, twist and bank.",
                       .command = "puppeteerSnapReverseFoot", .call = {{"align", true}}});
}

void registerMenus(ext::Context& ctx)
{
    menus::addMenu(ctx, {.id = "Puppeteer", .title = "Puppeteer", .sets = {"Rig", "Animate"}});
    for (const char* a : {"mode.puppeteerRig", "mode.puppeteerAnimate", "", "puppeteer.rig.character", "window.puppeteerStringsRules", "puppeteer.rig.ikChain",
                          "puppeteer.rig.reverseFoot", "puppeteer.rig.looker", "puppeteer.rig.space", "rig.bendBones",
                          "rig.bendBoneCV", "rig.mirrorBoneCVs", "rig.bakeBendBones"})
        menus::addItem(ctx, *a ? menus::Item{.menu = "Puppeteer", .action = a} : menus::Item{.menu = "Puppeteer", .kind = menus::Kind::Separator});
    menus::addItem(ctx, {.menu = "Puppeteer", .kind = menus::Kind::Submenu, .label = "Joints"});
    for (const char* a : {"puppeteer.joint.create", "puppeteer.joint.insert", "puppeteer.joint.parent", "puppeteer.joint.unparent",
                          "puppeteer.joint.orient", "puppeteer.joint.center", "puppeteer.joint.centerInMesh", "puppeteer.joint.mirror"})
        menus::addItem(ctx, {.menu = "Puppeteer/Joints", .action = a});
    menus::addItem(ctx, {.menu = "Puppeteer", .kind = menus::Kind::Separator});
    for (const char* a : {"puppeteer.anim.lock", "puppeteer.anim.pin", "puppeteer.anim.matchControllers", "puppeteer.anim.snapFoot",
                          "puppeteer.anim.alignFoot", "puppeteer.spinner.inheritRotate", "puppeteer.spinner.toonScale",
                          "puppeteer.spinner.autoPose", "puppeteer.curveOverlay"})
        menus::addItem(ctx, {.menu = "Puppeteer", .action = a});
    menus::addItem(ctx, {.menu = "Puppeteer", .kind = menus::Kind::Submenu, .label = "Strings"});
    for (const char* a : {"puppeteer.strings.forward", "puppeteer.strings.backward", "puppeteer.strings.ik", "puppeteer.strings.fullBody",
                          "", "puppeteer.strings.lineOfAction", "window.puppeteerStringsRules", "", "puppeteer.strings.contacts", "puppeteer.strings.onLayer", "puppeteer.strings.stretch", "puppeteer.strings.zero", "puppeteer.strings.restoreLengths",
                          "puppeteer.strings.overshoot", "puppeteer.strings.smooth", "puppeteer.strings.fill", "puppeteer.strings.retime", "puppeteer.strings.trail", "",
                          "puppeteer.strings.breakdownParts", "puppeteer.strings.clearBreakdownParts", "",
                          "puppeteer.strings.glue", "puppeteer.strings.attach", "puppeteer.strings.unglue"})
        menus::addItem(ctx, *a ? menus::Item{.menu = "Puppeteer/Strings", .action = a}
                               : menus::Item{.menu = "Puppeteer/Strings", .kind = menus::Kind::Separator});
    for (const char* a : {"puppeteer.strings.smooth", "puppeteer.strings.fill", "puppeteer.strings.retime"})
        menus::addItem(ctx, {.menu = menus::kTimeSliderRange, .action = a});
}

void registerShelf(ext::Context& ctx)
{
    json items = json::array();
    for (auto [label, iconName, target] : {std::tuple{"Rig", "bone", "mode.puppeteerRig"},
                                           {"Animate", "walk", "mode.puppeteerAnimate"},
                                           {"Character", "user-plus", "puppeteer.rig.character"},
                                           {"Joints", "hierarchy-2", "puppeteer.joint.create"},
                                           {"Orient", "compass", "puppeteer.joint.orient"},
                                           {"Mirror", "flip-horizontal", "puppeteer.joint.mirror"},
                                           {"IK Chain", "vector", "puppeteer.rig.ikChain"},
                                           {"Foot", "shoe", "puppeteer.rig.reverseFoot"},
                                           {"Looker", "eye", "puppeteer.rig.looker"},
                                           {"Spaces", "world", "puppeteer.rig.space"},
                                           {"Lock", "lock", "puppeteer.anim.lock"},
                                           {"Pin", "pin", "puppeteer.anim.pin"},
                                           {"Match", "target", "puppeteer.anim.matchControllers"},
                                           {"Stacker", "list-numbers", "window.puppeteerStacker"},
                                           {"Mixer", "stack-2", "window.puppeteerMixer"},
                                           {"Bank", "movie", "window.puppeteerAnimBank"},
                                           {"Cycles", "repeat", "window.puppeteerCycleMaker"}})
        items.push_back({{"label", label}, {"icon", iconName}, {"target", target}});
    shelves::add(ctx, {kShelf, std::move(items)});
}

void registerModeBundles(ext::Context& ctx)
{
    layouts::add(ctx, kRigLayout,
                 preset({"outliner", "channelBox", "attributeEditor", "displayLayers", "timeSlider"}, {"channelBox"}, "Rig"));
    // Curves are the Space overlay. Clips, poses, timing and layers stack down the left; the scene and its values stay right.
    layouts::add(ctx, kAnimateLayout,
                 preset({"outliner", "channelBox", "attributeEditor", "displayLayers", "puppeteerAnimBank", "poseLibrary", "puppeteerStacker",
                         "puppeteerMixer", "timeSlider"},
                        {"channelBox"}, "Animate",
                        {{"puppeteerAnimBank", {{"width", 300}, {"height", 0.15}}},
                         {"poseLibrary", {{"height", 0.3}}},
                         {"puppeteerStacker", {{"height", 0.3}}},
                         {"outliner", {{"height", 0.55}}}},
                        json::array({{"puppeteerAnimBank", "poseLibrary", "puppeteerStacker", "puppeteerMixer"}})));

    selectionHandlers::add(ctx, {.id = kRigSelection, .label = "Puppeteer Rig: joint under mesh", .redirect = baseRedirect, .hover = baseHover,
                                 .candidates = baseCandidates});
    selectionHandlers::add(ctx, {.id = kAnimateSelection, .label = "Puppeteer Animate: joint under mesh, IK goals for IK chains",
                                 .redirect = animateRedirect, .hover = baseHover, .candidates = animateCandidates});

    const json common = {{"I", {{"press", "puppeteer.spinner.move"}}},
                         {"O", {{"press", "puppeteer.spinner.rotate"}}},
                         {"P", {{"press", "puppeteer.spinner.scale"}}},
                         {"Shift+O", {{"press", "puppeteer.spinner.inheritRotate"}}},
                         {"8", {{"press", "puppeteer.spinner.local"}}},
                         {"9", {{"press", "puppeteer.spinner.world"}}},
                         {"0", {{"press", "puppeteer.spinner.screen"}}},
                         {"`", {{"press", "puppeteer.spinner.summonDown"}, {"release", "puppeteer.spinner.summonUp"}}}};
    json rigKeys = {{"viewport", common}};
    rigKeys["viewport"]["J"] = {{"press", "puppeteer.joint.create"}};
    rigKeys["viewport"]["Shift+J"] = {{"press", "puppeteer.joint.orient"}};
    rigKeys["viewport"]["Alt+M"] = {{"press", "puppeteer.joint.mirror"}};
    json animKeys = {{"viewport", common}};
    animKeys["viewport"]["Space"] = {{"press", "puppeteer.curveOverlay"}};
    // Curve Overlay keys; each does its usual job while the overlay is closed or has nothing to act on.
    animKeys["viewport"]["Shift+F"] = "puppeteer.curveOverlay.frameSelected";
    animKeys["viewport"]["Shift+A"] = "puppeteer.curveOverlay.frameAll";
    animKeys["viewport"]["Delete"] = "puppeteer.curveOverlay.delete";
    animKeys["viewport"]["Backspace"] = "puppeteer.curveOverlay.delete";
    animKeys["viewport"]["Esc"] = "puppeteer.curveOverlay.escape";
    animKeys["viewport"]["Shift+L"] = {{"press", "puppeteer.anim.lock"}};
    animKeys["viewport"]["Shift+P"] = {{"press", "puppeteer.anim.pin"}};
    animKeys["viewport"]["U"] = {{"LMB", {{"markingMenu", "puppeteerStrings"}}}};

    modes::add(ctx, {.id = kRigMode,
                     .label = "Puppeteer Rig",
                     .doc = "Build characters: joints, IK chains, reverse feet, lookers and spaces.",
                     .icon = "bone",
                     .layout = kRigLayout,
                     .hotkeys = rigKeys,
                     .shelf = kShelf,
                     .gizmoProvider = kSpinnerGizmo,
                     .selectionHandler = kRigSelection});
    modes::add(ctx, {.id = kAnimateMode,
                     .label = "Puppeteer Animate",
                     .doc = "Pose the joints directly: click the mesh to pick the joint under it, turn it with the Spinner. "
                            "U + left drag: pin, contacts, stretch and zero. Space tap: the Curve Overlay (hold Shift to edit its keys); Space hold: the hotbox.",
                     .icon = "walk",
                     .layout = kAnimateLayout,
                     .hotkeys = animKeys,
                     .markingMenuSet = kAnimateMode,
                     .shelf = kShelf,
                     .gizmoProvider = kSpinnerGizmo,
                     .selectionHandler = kAnimateSelection,
                     // Animating starts in Rotate, whatever tool was on before.
                     .onEnter =
                         [] {
                             if (actions::find("tool.rotate"))
                                 actions::run("tool.rotate");
                             ensureCharacter();
                         },
                     .onExit = [] { closeCurveOverlay(); }});
}
} // namespace

void registerModes(ext::Context& ctx)
{
    // Shift+right-click while animating: keys and poses rather than modelling tools.
    const json keysMenu = {{"N", {{"label", "Set Key"}, {"action", "anim.setKey"}}},
                           {"S", {{"label", "Delete Keys"}, {"action", "anim.deleteKeys"}}},
                           {"E", {{"label", "Next Key"}, {"action", "anim.nextKey"}}},
                           {"W", {{"label", "Previous Key"}, {"action", "anim.prevKey"}}},
                           {"NE", {{"label", "Mirror Pose"}, {"action", "pose.mirror"}}},
                           {"NW", {{"label", "Zero Pose"}, {"action", "puppeteer.strings.zero"}}},
                           {"SE", {{"label", "Pin or Unpin"}, {"action", "puppeteer.anim.pin"}}},
                           {"SW", {{"label", "Curve Overlay"}, {"action", "puppeteer.curveOverlay"}}},
                           {"overflow", json::array({{{"label", "Key Translate"}, {"action", "anim.setKeyTranslate"}},
                                                     {{"label", "Key Rotate"}, {"action", "anim.setKeyRotate"}},
                                                     {{"label", "Key Scale"}, {"action", "anim.setKeyScale"}},
                                                     {{"separator", true}},
                                                     {{"label", "Lock Key"}, {"action", "puppeteer.anim.lock"}}})}};
    for (const char* name : {"viewportShift", "viewportShiftObject"})
        markingMenus::add(ctx, std::string(kAnimateMode) + "/" + name, keysMenu);
    const events::SubscriptionId sub =
        events::subscribe(events::Event::SelectionChanged, [](const events::EventData&) { QTimer::singleShot(0, [] { ensureCharacter(); }); }, "Puppeteer");
    // Picking a Puppeteer layout enters its mode; any other layout leaves Puppeteer.
    const events::SubscriptionId layoutSub = events::subscribe(
        events::Event::LayoutChanged,
        [](const events::EventData& e) {
            const std::string want = e.name == kAnimateLayout ? kAnimateMode : e.name == kRigLayout ? kRigMode : std::string();
            const std::string& now = modes::current();
            if (!want.empty() && now != want)
                QTimer::singleShot(0, [want] { modes::activate(want); });
            else if (want.empty() && (now == kAnimateMode || now == kRigMode))
                QTimer::singleShot(0, [] { modes::activate(modes::kDefaultMode); });
        },
        "Puppeteer");
    ctx.track("puppeteer:autoCharacter", [sub, layoutSub] {
        events::unsubscribe(sub);
        events::unsubscribe(layoutSub);
    });
    registerModeActions(ctx);
    registerMenus(ctx);
    registerModeBundles(ctx);
    registerShelf(ctx);
}
} // namespace puppeteer
