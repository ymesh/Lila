#pragma once

#include "core/Export.h"
#include "core/PrefStore.h"

#include "render/FrameState.h"
#include "scene/Camera.h"

#include <algorithm>

namespace lila::ui::viewportDisplay
{
/**
 * Display prefs (file "general") shared by the Display menu, the viewport toolbar and the Preferences dialog. Every
 * viewport reads them each frame, so changing one redraws all views.
 */
inline constexpr const char* kAaEnabled = "/viewport/aa/enabled";
inline constexpr const char* kAaSamples = "/viewport/aa/samples"; // 1, 2, 4 or 8
/**
 * "multisample" (kAaSamples every frame) or "temporal": one sample a frame with temporal antialiasing, refined over the
 * still frames (kAccumulate) once the view stops. Temporal draws dense scenes about twice as fast while they move.
 */
inline constexpr const char* kAaMode = "/viewport/aa/mode";
inline constexpr const char* kAaTemporal = "temporal";
/** Temporal antialiasing, and how many jittered frames a still view averages after each change. */
inline constexpr const char* kTaa = "/viewport/aa/temporal";
inline constexpr const char* kAccumulate = "/viewport/aa/accumulate";
/** Most frames a second a view draws: render::kFrameCapRefresh (the display's rate, default), 0 for no limit, or fps. */
inline constexpr const char* kFrameCap = "/viewport/frameCap";
/** The cap while another app has focus; 0 keeps the foreground cap. */
inline constexpr const char* kBackgroundFrameCap = "/viewport/backgroundFrameCap";
inline constexpr int kDefaultBackgroundFrameCap = 0;
/** FIFO presentation: waits for each vblank instead of mailbox's newest-frame-wins. */
inline constexpr const char* kVsync = "/viewport/vsync";
/** Inside the resolution gate: broadcast action and title safe frames, and rule-of-thirds lines. */
inline constexpr const char* kSafeFrames = "/viewport/safeFrames";
/** Shadows traced against the scene for up to four lights: exact contacts, soft by each light's size, on GPUs with ray tracing. */
inline constexpr const char* kRtShadows = "/viewport/rtShadows";
inline constexpr const char* kThirds = "/viewport/thirds";
/** The scene's post process grading, bloom and lens effects in viewports (its tonemapper and exposure always apply). */
inline constexpr const char* kPostEffects = "/viewport/post/effects";
inline constexpr const char* kGrid = "/viewport/grid";
inline constexpr const char* kBackfaceCulling = "/viewport/backfaceCulling";
inline constexpr const char* kXray = "/viewport/xray";
inline constexpr const char* kLighting = "/viewport/lighting"; // "environment", "studio", "sculpt" or "flat"
inline constexpr const char* kEnvRotation = "/viewport/env/rotation"; // degrees around the up axis
/** Degrees the studio and sculpt rigs turn around the view axis. */
inline constexpr const char* kRigRotation = "/viewport/rigRotation";
/** Degrees the sculpt rig's key sits off the view direction: 0 lights from the camera, 90 grazes. */
inline constexpr const char* kRigTilt = "/viewport/rigTilt";
inline constexpr double kDefaultRigTilt = 45.0;
/** The lighting mode Sculpt Lighting switches back to. */
inline constexpr const char* kLightingBeforeSculpt = "/viewport/lightingBeforeSculpt";
inline constexpr const char* kSceneLights = "/viewport/sceneLights"; // false: lights and domes light nothing in the viewport
inline constexpr const char* kShadows = "/viewport/shadows";
inline constexpr const char* kDefaultMaterial = "/viewport/defaultMaterial";
/** Materials keep everything but their base colour, which shades 50% grey. */
inline constexpr const char* kGreyBaseColor = "/viewport/greyBaseColor";
inline constexpr const char* kExposure = "/viewport/exposure"; // stops
inline constexpr const char* kGamma = "/viewport/gamma";
inline constexpr const char* kToolbar = "/viewport/toolbar";
/** Panel toolbar items that need something the scene lacks (joints, materials) hide while it lacks it. */
inline constexpr const char* kHideUnusedItems = "/viewport/toolbar/hideUnused";
/** Smooth preview draws the subdivided mesh's own edges; off, only the cage edges, following the smooth surface. */
inline constexpr const char* kSmoothSubdivisions = "/viewport/smoothPreview/subdivisions";
/** Smooth preview keeps normals split along hard edges; off, it shades smooth across them, like Maya. */
inline constexpr const char* kSmoothHardEdges = "/viewport/smoothPreview/hardEdges";
inline constexpr const char* kVertexColors = "/viewport/vertexColors"; // "off", "shaded" (multiplied in) or "unlit"
inline constexpr const char* kColorChannel = "/viewport/colorChannel"; // "rgb", "r", "g", "b" or "a" (grayscale)
/** Weight displays (skin weights, soft select falloff) as a grayscale ramp instead of the colour ramp. */
inline constexpr const char* kGrayscaleWeights = "/viewport/grayscaleWeights";
/**
 * How strongly components show the soft select falloff, 0 to 1: 0 only shades the vertex dots, higher tints the edges by
 * weight and grows the dots with it; above 0.5 the edges draw at the selected edge width.
 */
inline constexpr const char* kSoftSelectEmphasis = "/softSelect/emphasis";
inline constexpr double kSoftSelectEmphasisDefault = 0.5;
/** Objects draw highlighted when selected; off, they draw as if unselected (manipulators and component modes still show). */
inline constexpr const char* kHighlightSelection = "/viewport/highlightSelection";
/**
 * With several objects selected, which take the selected wire colour and which the secondary one (white): "last"
 * (Maya), "allButFirst", "first", or "all".
 */
inline constexpr const char* kSelectionOrderColors = "/viewport/selectionOrderColors";
inline constexpr int kDefaultSamples = 8;
/** Preview-only post over the shaded scene: an anisotropic Kuwahara (painted look) and depth/normal outlines. */
inline constexpr const char* kKuwahara = "/viewport/post/kuwahara";
inline constexpr const char* kKuwaharaRadius = "/viewport/post/kuwaharaRadius"; // device pixels
inline constexpr const char* kOutline = "/viewport/post/outline";
inline constexpr const char* kOutlineWidth = "/viewport/post/outlineWidth"; // device pixels

/** Full monotone: no hue anywhere, overriding every accent toggle below. Off by default. */
inline constexpr const char* kMonotone = "/ui/monotone";
inline bool monotone() { return PrefStore::get().value<bool>("general", kMonotone, false); }
/** Selective colour: the few displays that read better with hue, each on by default. */
inline constexpr const char* kViewAxisColors = "/viewport/viewAxisColors";
inline constexpr const char* kManipulatorAxisColors = "/manipulator/axisColors";
inline constexpr const char* kTimeKeyColors = "/timeline/keyColors";
inline constexpr const char* kGraphAxisColors = "/graphEditor/axisColors";
inline constexpr const char* kGhostColors = "/anim/ghost/colors";
inline constexpr const char* kComponentColors = "/viewport/componentColors";

/** Component thickness in logical pixels: edge line widths and vertex square sizes, unselected, hovered and selected. */
inline constexpr const char* kEdgeThickness = "/viewport/components/edgeThickness";
inline constexpr const char* kEdgeThicknessHover = "/viewport/components/edgeThicknessHover";
inline constexpr const char* kEdgeThicknessSelected = "/viewport/components/edgeThicknessSelected";
inline constexpr const char* kVertexThickness = "/viewport/components/vertexThickness";
inline constexpr const char* kVertexThicknessHover = "/viewport/components/vertexThicknessHover";
inline constexpr const char* kVertexThicknessSelected = "/viewport/components/vertexThicknessSelected";
/** Vertex dots in edge and face modes: the selection's own vertices, which soft select otherwise leaves out. */
inline constexpr const char* kVerticesInEdgeFaceModes = "/viewport/components/verticesInEdgeFaceModes";
/** UV seams drawn over meshes while the UV editor shows them. */
inline constexpr const char* kUvSeamThickness = "/viewport/components/uvSeamThickness";
/** The edges on a mesh's symmetry plane, drawn in red in component modes. */
inline constexpr const char* kSymmetryEdges = "/viewport/components/symmetryEdges";
inline constexpr const char* kSymmetryEdgesObjectMode = "/viewport/components/symmetryEdgesObjectMode";
inline constexpr const char* kSymmetryEdgeThickness = "/viewport/components/symmetryEdgeThickness";

struct ComponentThickness
{
    double edge = 2.0;
    double edgeHover = 3.5;
    double edgeSelected = 3.0;
    double vertex = 4.0;
    double vertexHover = 8.0;
    double vertexSelected = 6.0;
    double uvSeam = 3.0;
    double symmetryEdge = 4.0;
};

LILA_UI_API ComponentThickness componentThickness();

/**
 * Device-pixel width of a hardware line for a logical width. 1 stays a one-pixel hairline at any display scale; the
 * width beyond it scales with the display like the drawn components do.
 */
inline float hairlineWidth(double logical, double dpr) { return float(std::max(1.0, 1.0 + (logical - 1.0) * dpr)); }

/** Curve line and halo widths for the display scale: 2 px curves, 3 px highlighted, each with a 1 px dark rim. */
inline void applyCurveWidths(render::FrameState& s, double dpr)
{
    s.curveWidth = hairlineWidth(2.0, dpr);
    s.curveBoldWidth = hairlineWidth(3.0, dpr);
    s.curveHaloWidth = s.curveWidth + float(2.0 * dpr);
    s.curveHaloBoldWidth = s.curveBoldWidth + float(2.0 * dpr);
}

inline bool accentColors(bool monotone, bool selective) { return selective && !monotone; }
/** Whether the display behind one of the accent toggles above draws in colour right now. */
LILA_UI_API bool accentColors(const char* key);
/** Whether views show the View Switcher where a viewport has made no choice of its own; Puppeteer's modes turn it on. */
LILA_UI_API void setViewSwitcherDefault(bool on);
LILA_UI_API bool viewSwitcherDefault();
/** A display layer's linear colour, as a grey of the same luminance under Monotone. */
LILA_UI_API glm::vec3 layerTint(const glm::vec3& linear);
/** The theme's X, Y and Z colours (Monotone: muted red, green and blue that read on the dark UI greys). sRGB 0..1. */
LILA_UI_API glm::vec3 axisColor(int axis);
/** Key ticks on the time slider, the theme's keyTick (Monotone: Maya's red). sRGB 0..1. */
LILA_UI_API glm::vec3 keyTickColor();

/** Copies the prefs above onto a frame. */
LILA_UI_API void apply(render::FrameState& s);

/** The frame cap in force now: the background cap while another app has focus. render::kFrameCapRefresh, 0 (none) or fps. */
LILA_UI_API int frameCap();
/** Seconds between frames under frameCap(), at the primary screen's refresh rate for kFrameCapRefresh; 0 uncapped. */
LILA_UI_API double frameInterval();

/** Shaded meshes ignore their materials and draw the plain default one. */
LILA_UI_API bool defaultMaterial();
LILA_UI_API bool highlightSelection();
/** Which selected objects draw in the selected wire colour, per kSelectionOrderColors. */
struct SelectionOrderColors
{
    scene::NodeId odd = scene::kNoNode; // the first or last selected, set apart from the rest
    bool oddIsAccent = true;            // the odd one takes the selected colour and the rest white, or the reverse

    /** True when a selected node draws in the secondary colour rather than the selected one. */
    bool other(scene::NodeId id) const { return odd != scene::kNoNode && (id == odd) != oddIsAccent; }
};
LILA_UI_API SelectionOrderColors selectionOrderColors(const scene::Graph& g);

LILA_UI_API render::VertexColorMode vertexColorMode();
LILA_UI_API render::ColorChannel colorChannel();

LILA_UI_API bool grayscaleWeights();
/**
 * Linear colour for a weight of 0..1: black through blue, cyan, green, yellow and red to white (Maya's paint ramp), or a
 * display-encoded gray ramp (0.5 reads mid gray) when grayscale.
 */
LILA_UI_API glm::vec4 weightColor(float w, bool grayscale);
/** Soft select falloff: dark red at the rim through orange to yellow at full weight (Maya's), or gray. Linear. */
LILA_UI_API glm::vec4 falloffColor(float w, bool grayscale);

/** Viewport defaults offered on first run; each is only a starting point for the AO, antialiasing and shadow prefs. */
enum class Quality
{
    Best,     // accurate AO, 8x and temporal antialiasing, shadows
    Balanced, // fast AO, temporal-only antialiasing (kAaMode), shadows
    Fastest,  // no AO, antialiasing or shadows
};
/** kAaMode is temporal. */
LILA_UI_API bool temporalAa();
/**
 * Turns the light the current lighting mode uses by degrees: the HDRI around the up axis, or the studio or sculpt rig
 * around the view. Flat lighting has none, and nothing changes. Kept within -180..180.
 */
LILA_UI_API void turnLight(double degrees);
/** Moves the sculpt rig's key toward grazing (positive) or toward the camera, within 0..90 degrees. Other modes ignore it. */
LILA_UI_API void tiltLight(double degrees);
/** The current mode's light back to its defaults: rotation 0, and the sculpt rig's angle too. */
LILA_UI_API void resetLight();
LILA_UI_API void applyQuality(Quality q);
/** The scene's active post process into s (nothing when it has none); effects turns its grading, bloom and lens effects on. */
LILA_UI_API void applyPostProcess(render::FrameState& s, bool effects);
/**
 * The scene's lights Lila's provider owns into s.lights, in the shader's units, with shadow matrices fit to the shown
 * meshes for directional and spot lights.
 */
LILA_UI_API void applyLights(render::FrameState& s);
/** A camera's depth of field into s, when the camera has it on. */
LILA_UI_API void applyLens(render::FrameState& s, const scene::camera::Lens& lens);
/** The preset this machine's GPU suits: Fastest for integrated or under 4 GB, Balanced under 8 GB, otherwise Best. */
LILA_UI_API Quality recommendedQuality();
} // namespace lila::ui::viewportDisplay
