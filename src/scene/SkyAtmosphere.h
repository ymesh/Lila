#pragma once

#include "core/Export.h"
#include "scene/Node.h"

namespace lila::scene
{
class Graph;

/**
 * A planet's sky: Rayleigh and Mie scattering, ozone and the haze between the camera and what it sees, lit by the sun (a
 * directional light). Unreal's SkyAtmosphere, attribute for attribute, so it sends across unchanged. The first enabled
 * one in the scene applies. Attribute order matches the Attr enum.
 */
namespace skyAtmosphere
{
enum Attr
{
    kEnabled,
    kTransformMode, // planetTopAtAbsoluteWorldOrigin, planetTopAtComponentTransform, planetCenterAtComponentTransform
    kBottomRadius,  // km
    kGroundAlbedo,
    kAtmosphereHeight, // km
    kMultiScatteringFactor,
    kRayleighScatteringScale,
    kRayleighScattering,
    kRayleighExponentialDistribution, // km
    kMieScatteringScale,
    kMieScattering,
    kMieAbsorptionScale,
    kMieAbsorption,
    kMieAnisotropy,
    kMieExponentialDistribution, // km
    kOtherAbsorptionScale,
    kOtherAbsorption,
    kOtherTentTipAltitude, // km
    kOtherTentTipValue,
    kOtherTentWidth, // km
    kSkyLuminanceFactor,
    kAerialPerspectiveViewDistanceScale,
    kAerialPerspectiveStartDepth, // km
    kHeightFogContribution,
    kTransmittanceMinLightElevationAngle, // degrees
    kCount
};

/** The node's own attribute indices start after the base node's; this is the first. */
LILA_SCENE_API int firstAttr();

/** The first enabled sky atmosphere, or kNoNode. */
LILA_SCENE_API NodeId active(const Graph& g);

/** The sky node's world position, cm: where the component transform modes put the planet's ground or centre. */
LILA_SCENE_API glm::dvec3 worldPosition(const Node& sky);

/** The node's uniform world scale. It sizes the planet and its air together, so a scaled-down globe gets an atmosphere in proportion. */
LILA_SCENE_API double planetScale(const Node& sky);

/** An attribute as it applies at the node's scale: km lengths times planetScale, per-km strengths divided by it, the rest as stored. */
LILA_SCENE_API double scaled(const Node& sky, Attr attr);

/** The planet's centre in world cm, from the node's transform mode and world position. */
LILA_SCENE_API glm::dvec3 planetCenter(const Node& sky, bool zUp);

/** The light the sky takes as its sun: the visible directional light with atmosphereSun on, else the first; kNoNode for none. */
LILA_SCENE_API NodeId sunLight(const Graph& g);

void registerTypes();
void registerCommands();
} // namespace skyAtmosphere
} // namespace lila::scene
