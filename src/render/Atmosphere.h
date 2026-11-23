#pragma once

#include "core/Export.h"

#include "render/Environment.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

namespace lila::render
{
/**
 * A planet's atmosphere, in Unreal's SkyAtmosphere terms so the values carry over unchanged. Lengths in km, scattering
 * and absorption coefficients per km (scale times colour, as Unreal multiplies them).
 */
struct AtmosphereSettings
{
    double bottomRadius = 6360.0;
    double atmosphereHeight = 60.0;
    glm::vec3 groundAlbedo{0.401978f, 0.401978f, 0.401978f};
    float multiScattering = 1.0f;
    glm::vec3 rayleighScattering = 0.0331f * glm::vec3(0.175287f, 0.409607f, 1.0f);
    float rayleighScaleHeight = 8.0f;
    glm::vec3 mieScattering = glm::vec3(0.003996f);
    glm::vec3 mieAbsorption = glm::vec3(0.000444f);
    float mieAnisotropy = 0.8f;
    float mieScaleHeight = 1.2f;
    /** Ozone in the default: a tent of density peaking at tipAltitude, falling to 0 width either side. */
    glm::vec3 otherAbsorption = 0.001881f * glm::vec3(0.345561f, 1.0f, 0.045188f);
    float otherTipAltitude = 25.0f;
    float otherTipValue = 1.0f;
    float otherWidth = 15.0f;
    glm::vec3 skyLuminanceFactor{1.0f};
    float aerialDistanceScale = 1.0f;
    float aerialStartDepth = 0.1f; // km
    /** Lowest sun elevation (degrees) the ground's sunlight is attenuated for; below it the sun keeps that colour. */
    float minLightElevation = -90.0f;

    double topRadius() const { return bottomRadius + atmosphereHeight; }
    /** 1 for Earth-deep air and deeper, less for a shallower one: scales the fixed small distances so a miniature planet keeps them in proportion. */
    double lengthUnit() const { return std::min(atmosphereHeight / 60.0, 1.0); }
    bool operator==(const AtmosphereSettings&) const = default;
};

/** Precomputed tables (Hillaire 2020): transmittance to the top of the atmosphere and the multiple scattering term. */
struct AtmosphereLuts
{
    static constexpr int kTransmittanceW = 256, kTransmittanceH = 64, kMultiScatter = 32;
    std::vector<glm::vec3> transmittance; // u: view zenith, v: altitude (Bruneton's mapping)
    std::vector<glm::vec3> multiScatter;  // u: cos sun zenith from -1 to 1, v: altitude
};

LILA_UI_API AtmosphereLuts buildAtmosphereLuts(const AtmosphereSettings& a);

/** Transmittance from a point r km from the planet's centre to the top of the atmosphere along a ray at cos zenith mu. */
LILA_UI_API glm::vec3 atmosphereTransmittance(const AtmosphereSettings& a, const AtmosphereLuts& luts, double r, double mu);

/**
 * Light the atmosphere scatters toward pos (km from the planet's centre) from direction dir (unit, pointing away from
 * the viewer), lit by a sun toward sunDir of illuminance 1. Includes the lit ground where the ray hits the planet; the
 * sun's own disk is left out. Samples along the ray: more for a smoother result.
 */
LILA_UI_API glm::vec3 skyRadiance(const AtmosphereSettings& a, const AtmosphereLuts& luts, const glm::dvec3& pos, const glm::dvec3& dir,
                                  const glm::dvec3& sunDir, int samples = 32);

/** Sunlight reaching pos through the atmosphere, as a share per channel; the planet's shadow gives 0. */
LILA_UI_API glm::vec3 sunTransmittance(const AtmosphereSettings& a, const AtmosphereLuts& luts, const glm::dvec3& pos, const glm::dvec3& sunDir);

/**
 * What the viewport and path tracer draw with: the settings, where the planet is and the sun that lights it. World axes
 * are the scene's, so up is +Y, or +Z when zUp; positions are world cm.
 */
struct AtmosphereFrame
{
    AtmosphereSettings settings;
    glm::dvec3 planetCenter{0.0, -6360.0e5, 0.0}; // world cm
    glm::dvec3 sunDir{0.0, 1.0, 0.0};             // toward the sun
    glm::vec3 sunRadiance{1.0f};                  // the sun light's radiance above the atmosphere, shader units
    float sunAngularRadius = 0.004675f;           // radians, Unreal's default sun disk
    bool zUp = false;
};

/** Camera position in km from the planet's centre, kept above the ground so a camera below sea level still sees sky. */
LILA_UI_API glm::dvec3 atmosphereCameraKm(const AtmosphereFrame& f, const glm::dvec3& eyeWorld);

/**
 * The key a baked sky goes by on the renderer's environment path: its settings, sun and the camera height it was seen
 * from, rounded so small camera moves reuse it. isAtmosphereKey tells such paths from files.
 */
LILA_UI_API std::string atmosphereEnvironmentKey(const AtmosphereFrame& f, const glm::dvec3& eyeWorld);
LILA_UI_API bool isAtmosphereKey(const std::string& path);
LILA_UI_API std::optional<AtmosphereFrame> parseAtmosphereKey(const std::string& path, glm::dvec3* eyeWorld = nullptr);

/** The sky as image-based lighting, without the sun's disk (the sun light adds the sun), in the environment's Y-up axes. */
LILA_UI_API EnvironmentData buildAtmosphereEnvironment(const AtmosphereFrame& f, const glm::dvec3& eyeWorld);

/** The atmosphere as the shaders read it: GpuAtmosphere followed by the two tables, float4 per texel. */
struct GpuAtmosphere
{
    glm::vec4 radii;       // bottom, top (km), multiple scattering factor, 1
    glm::vec4 rayleigh;    // rgb scattering per km, w scale height km
    glm::vec4 mieScatter;  // rgb, w scale height km
    glm::vec4 mieAbsorb;   // rgb, w anisotropy
    glm::vec4 ozone;       // rgb absorption per km, w tip altitude km
    glm::vec4 ozoneShape;  // x tip value, y width km, z sin of the lowest sun elevation, w unused
    glm::vec4 ground;      // rgb albedo, w cos of the sun disk's angular radius
    glm::vec4 skyFactor;   // rgb sky luminance factor, w aerial perspective distance scale
    glm::vec4 camera;      // xyz the camera in km from the planet's centre, world axes; w aerial perspective start, km
    glm::vec4 sunDir;      // xyz toward the sun, w 1 when zUp
    glm::vec4 sunRadiance; // rgb the sun light above the atmosphere, w unused
    glm::vec4 pad[5];
};
static_assert(sizeof(GpuAtmosphere) == 256, "matches GpuAtmosphere in atmosphere_common.slang");

/** The block the shaders read: header then transmittance then multiple scattering, as float4. */
LILA_UI_API std::vector<glm::vec4> packAtmosphere(const AtmosphereFrame& f, const AtmosphereLuts& luts, const glm::dvec3& eyeWorld);

/** Aerial perspective froxels: kAerialSize x kAerialSize screen cells by kAerialSlices depth slices. */
constexpr int kAerialSize = 32, kAerialSlices = 32;
/** The view distance the last slice reaches, km. */
constexpr float kAerialDepthKm = 96.0f;
} // namespace lila::render
