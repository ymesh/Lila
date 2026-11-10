#pragma once

#include <lila/Extension.h>
#include <lila/Strings.h>

/**
 * Puppeteer: character animation done on the joints themselves. Each subsystem registers through its own function so
 * the entry point (Puppeteer.cpp) reads as the list of what the extension adds.
 */
namespace puppeteer
{
inline constexpr const char* kCharacterType = "puppeteerCharacter";
// Rig elements are base Lila's; Puppeteer gives them its UI.
inline constexpr const char* kIkChainType = "ikChain";
inline constexpr const char* kReverseFootType = "reverseFoot";
inline constexpr const char* kLookerType = "looker";
// Older Puppeteer scenes' types, loaded only to be migrated.
inline constexpr const char* kLegacyIkChainType = "puppeteerIkChain";
inline constexpr const char* kLegacyReverseFootType = "puppeteerReverseFoot";
inline constexpr const char* kLegacyAlignType = "puppeteerAlign";
inline constexpr const char* kLegacyLookerType = "puppeteerLooker";
inline constexpr const char* kSpaceType = "puppeteerSpace";

inline constexpr const char* kRigMode = "puppeteerRig";
inline constexpr const char* kAnimateMode = "puppeteerAnimate";
inline constexpr const char* kSpinnerGizmo = "puppeteer.spinner";
inline constexpr const char* kRigSelection = "puppeteer.rig";
inline constexpr const char* kAnimateSelection = "puppeteer.animate";

/** Prefs file the extension's settings live in. */
inline constexpr const char* kPrefs = "puppeteer";

/** The character node type and its commands. */
bool registerCharacter(lila::ext::Context& ctx);
/** Puppeteer's rig element commands over base Lila's, and the migration of older scenes' elements. */
bool registerRigElements(lila::ext::Context& ctx);
/** IK/reverse foot controller commands: pole, roll/twist/bank, foot lock. */
void registerControllers(lila::ext::Context& ctx);
/** How reverse foot controllers look and pick in the viewport. */
void registerControllerDrawing(lila::ext::Context& ctx);
/** Pins for full-body posing. */
void registerFullBody(lila::ext::Context& ctx);
/** Strings: stringsPose, the mode drags pose in, its actions and marking menu (rig/Strings.cpp). */
void registerStrings(lila::ext::Context& ctx);
/** The Strings rules on a character's Attribute Editor and the actions behind its buttons (gizmo/StringsUi.cpp). */
void registerStringsUi(lila::ext::Context& ctx);
void registerLineOfActionTool(lila::ext::Context& ctx);
/** The Breakdown Parts brush (gizmo/BreakdownPartsTool.cpp). */
void registerBreakdownPartsTool(lila::ext::Context& ctx);
/** The Strings Rules panel (gizmo/RulesEditor.cpp). */
void registerRulesEditor(lila::ext::Context& ctx);
/** The Strings mode Spinner drags pose in. */
lila::scene::strings::Mode stringsMode();
void setStringsMode(lila::scene::strings::Mode m);
/** How far drags may stretch bones: 1 with Stretch off. */
double stringsStretch();
/** The additive layer drags across a time range key onto: StringsEdits while that option is on, else empty (the base). */
std::string stringsLayer();
inline constexpr const char* kStringsLayer = "StringsEdits";
/** Whether node is glued or attached to anything. */
bool stringsRelated(lila::scene::NodeId node);
/** Each glue or attach set's members, for drawing; attach sets lead with their last. */
std::vector<std::vector<lila::scene::NodeId>> stringsRelations();
/** Joint tools: commands and actions. */
void registerJointTools(lila::ext::Context& ctx);
/** The Spinner's commands (rotate with inherit, toon scale, aim moves, breakdown), usable without the UI. */
void registerSpinnerOps(lila::ext::Context& ctx);
/** The Spinner gizmo provider, its overlay icons and actions. */
void registerSpinner(lila::ext::Context& ctx);
/** Rig and Animate modes: layouts, hotkey layers, selection handlers, menus. */
void registerModes(lila::ext::Context& ctx);
/** The Curve Overlay: its viewport layer, actions and curveOverlay command (anim/CurveOverlay.cpp). */
void registerCurveOverlay(lila::ext::Context& ctx);
void closeCurveOverlay();
/** Anim proxies: the prefs page, actions, and showing proxies with the modes (rig/AnimProxy.cpp). */
void registerAnimProxy(lila::ext::Context& ctx);
/** Animate-mode panels (anim/). */
void registerAnimPanels(lila::ext::Context& ctx);
} // namespace puppeteer
