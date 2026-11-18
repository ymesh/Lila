#pragma once

#include "core/Export.h"
#include "scene/Node.h"

#include <string>
#include <vector>

namespace lila::scene
{
class Graph;

/**
 * A post process: how the scene's light becomes the picture. Tonemapper, exposure, white balance and grading, bloom,
 * lens effects and a LUT, applied by the viewport (unless its post effects are off) and by renders. The first enabled
 * one in the scene is the one used. Attribute order matches the Attr enum.
 */
namespace postProcess
{
enum Attr
{
    kEnabled,
    kTonemapper, // neutral, agx, aces, linear
    kAgxLook,    // none, punchy, golden
    kExposure,   // stops
    kAutoExposure,
    kExposureCompensation,
    kExposureMin,
    kExposureMax,
    kExposureSpeed,
    kTemperature, // kelvin; 6500 leaves white alone
    kTint,
    kContrast,
    kSaturation,
    kLift,
    kGamma,
    kGain,
    kOffset,
    kLiftMaster,
    kGammaMaster,
    kGainMaster,
    kOffsetMaster,
    kShadows,
    kMidtones,
    kHighlights,
    kShadowsRange,
    kHighlightsRange,
    kCurveMaster, // grading curves, in render::grade::Curve order
    kCurveRed,
    kCurveGreen,
    kCurveBlue,
    kCurveHueVsSat,
    kCurveHueVsHue,
    kCurveLumVsSat,
    kBloom,
    kBloomRadius,
    kVignette,
    kGrain,
    kChromaticAberration,
    kDistortion,
    kSharpen,
    kLut,
    kLutIntensity,
    kFog,
    kFogColor,
    kFogDensity,
    kFogHeight,
    kFogFalloff,
    kFogStart,
    kFogSunScatter,
    kFogAnisotropy,
    kLensFlare,
    kFlareThreshold,
    kFlareGhosts,
    kFlareGhostSpacing,
    kFlareChroma,
    kFlareHalo,
    kFlareHaloRadius,
    kFlareStarburst,
    kFlareStreakLength,
    kAnamorphicStreaks,
    kAnamorphicTint,
    kLensDirt,
    kLensDirtIntensity,
    kHalation,
    kHalationThreshold,
    kHalationTint,
    kGrainSize,
    kGrainColor,
    kGrainResponse,
    kLetterboxAspect,
    kLetterboxOpacity,
    kVolume,         // global, box, sphere
    kVolumeCenter,   // cm
    kVolumeSize,     // box: full size, cm; sphere: x is the radius
    kVolumeBlend,    // cm outside the volume over which it fades out
    kVolumePriority, // higher wins where volumes overlap
    kCount
};

/** Where a camera at eye sits against a volume post process: 1 inside, falling to 0 at its blend distance outside. */
LILA_SCENE_API double volumeWeight(const Node& post, const glm::dvec3& eye);

/** The lens presets lensPreset applies, Lila's own names: spherical, anamorphic, vintage and clean glass. */
LILA_SCENE_API const std::vector<std::string>& lensPresetNames();

/** The node's own attribute indices start after the base node's; this is the first. */
LILA_SCENE_API int firstAttr();

/** The first enabled post process node, or kNoNode. */
LILA_SCENE_API NodeId active(const Graph& g);

/** Per-channel gains that turn a light of `kelvin` white, then shift green against magenta by tint (-1..1). */
LILA_SCENE_API glm::dvec3 whiteBalance(double kelvin, double tint);

void registerTypes();
void registerCommands();
} // namespace postProcess
} // namespace lila::scene
