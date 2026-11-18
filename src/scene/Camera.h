#pragma once

#include "core/Export.h"

#include "scene/ImagePlane.h"
#include "scene/RenderSettings.h"
#include "scene/Transform.h"

#include <algorithm>
#include <optional>

namespace lila::scene
{
/**
 * A scene camera, as Maya's camera shape: lens, film back and clipping on a locator-like transform. It looks down its
 * local -Z with +Y up. Film apertures are in inches and the focal length in mm, as Maya shows them; distances in cm.
 */
class Camera : public Locator
{
public:
    enum Attr
    {
        kFocalLength = Locator::kDrawSize + 1,
        kHorizontalFilmAperture,
        kVerticalFilmAperture,
        kFilmFit,
        kNearClipPlane,
        kFarClipPlane,
        kOrthographic,
        kOrthographicWidth,
        kLocked,
        kOverscan,
        kDisplayResolution,
        kDisplayFilmGate,
        kDisplayGateMask,
        kDisplayGateMaskOpacity,
        kBookmarks,
        kDepthOfField,
        kFStop,
        kFocusDistance,
        kApertureBlades,
        kApertureRotation,
        kShutterAngle,
        kAnamorphicSqueeze,
        kPhysicalExposure,
        kIso,
        kPostProcess,
        kCount
    };
};

namespace camera
{
/** The lens as renders and still viewports see it. */
struct Lens
{
    bool depthOfField = false;
    double fStop = 5.6;
    double focusDistance = 500.0; // cm
    int blades = 0;               // 0 is a round aperture
    double bladeRotation = 0.0;   // degrees
    double shutterAngle = 180.0;  // degrees of the frame the shutter is open, for motion blur
    double focalLength = 35.0;    // mm
    double anamorphicSqueeze = 1.0; // above 1 an anamorphic lens: bokeh stretches tall, flare streaks run wide
    bool physicalExposure = false;  // fStop, shutter and ISO set the exposure (EV100)
    double iso = 100.0;
    std::string postProcess;        // a post process node this camera grades with in place of the scene's
    /** Radius of the aperture, cm: the focal length over twice the f-number. */
    double apertureRadius() const { return focalLength / (2.0 * std::max(fStop, 0.1)) * 0.1; }
};
LILA_SCENE_API Lens lens(const Node& cam);

/** The camera's frame from its node and world matrix, as image planes lay out against it. */
LILA_SCENE_API imagePlane::CameraFrame frame(const Node& cam, const glm::dmat4& world);

/** Film back half extents: tangents of the half angles for perspective, cm for orthographic. Before the film fit. */
LILA_SCENE_API glm::dvec2 filmExtents(const Node& cam);
LILA_SCENE_API FilmFit filmFit(const Node& cam);

/** Half extents a view of `aspect` (width over height) shows through the camera, after its film fit. */
LILA_SCENE_API glm::dvec2 viewExtents(const Node& cam, double aspect, double overscan = 1.0, std::optional<double> gateAspect = std::nullopt);

/** Horizontal and vertical angle of view in degrees, as a view of `aspect` shows it. Perspective cameras only. */
LILA_SCENE_API glm::dvec2 angleOfView(const Node& cam, double aspect);

/**
 * Reversed-Z projection in Vulkan clip space (y down, depth 1 at the near plane) for view half extents in the film's
 * units. A far clip of 0 or less is an infinite far plane; orthographic projections always need one.
 */
LILA_SCENE_API glm::dmat4 projection(glm::dvec2 viewExtents, bool ortho, double nearClip, double farClip);
/** The camera's projection for a view of `aspect`, with its own clipping planes. */
LILA_SCENE_API glm::dmat4 projection(const Node& cam, double aspect, double overscan = 1.0, std::optional<double> gateAspect = std::nullopt);

/** World to view: the inverse of the camera's world matrix with its scale and shear taken out. */
LILA_SCENE_API glm::dmat4 viewMatrix(const glm::dmat4& world);

/**
 * World matrix that puts a camera at eye looking at lookAt with up as near its up as it can be, keeping the scale of
 * `world` (the camera's current world matrix).
 */
LILA_SCENE_API glm::dmat4 placedWorld(const glm::dmat4& world, const glm::dvec3& eye, const glm::dvec3& lookAt, const glm::dvec3& up);

/** A camera node's gate display and film fit, as RenderSettings::camera() reports them for it. */
LILA_SCENE_API CameraSettings settings(const Node& cam);
/** Writes the settings onto the node's attributes (undoably when undo is on). */
LILA_SCENE_API void setSettings(Graph& g, NodeId cam, const CameraSettings& c);

/**
 * Moves gate settings that older scenes keyed by a camera node's name in the render settings onto that node, so they
 * follow renames. Built-in view names stay where they are.
 */
LILA_SCENE_API void migrateNamedSettings(Graph& g, RenderSettings& r);
} // namespace camera

/** The camera type. Idempotent; needs the locator type first. */
LILA_SCENE_API void registerCameraTypes();
/** The viewPlace command. Idempotent. */
LILA_SCENE_API void registerCameraCommands();
} // namespace lila::scene
