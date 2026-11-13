#pragma once

#include "core/Export.h"

#include "scene/RenderSettings.h"

namespace lila::scene
{
class Node;
}

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>

namespace lila::ui
{
/** Largest camera distance and orthographic width, cm: far enough to frame a continent from orbit. */
inline constexpr double kMaxCameraDistance = 1e10;

/** Maya-style tumble camera around a center of interest. */
struct LILA_UI_API Camera
{
    glm::dvec3 target{0.0};
    double distance = 120.0; // cm; a default primitive (10 cm) fills a comfortable part of the view
    double yaw = 45.0;    // degrees around world up
    double pitch = -27.0; // degrees, negative looks down
    double focalLength = 35.0;     // mm
    double horizontalAperture = 36.0; // mm, matches Maya's default film back
    double verticalAperture = 24.0;   // mm
    double nearClip = 0.1;
    /** Perspective near clip follows the distance to the center of interest instead of nearClip. */
    bool autoNear = false;
    bool zUp = false;

    /** Orthographic views (top/front/side) look along a fixed axis; `orthoWidth` is the visible width in cm. */
    bool ortho = false;
    double orthoWidth = 160.0;
    glm::dvec3 orthoForward{0, 0, -1};
    glm::dvec3 orthoUp{0, 1, 0};

    scene::FilmFit filmFit = scene::FilmFit::Fill;
    double overscan = 1.0;
    /** Render width over height while the resolution gate shows; the projection then fits the whole gate in view. */
    std::optional<double> gateAspect;

    /**
     * Looking through a camera node: the orientation is `rotation` (columns right, up, back) instead of yaw and pitch,
     * so a rolled camera stays rolled, and the far plane is the node's.
     */
    bool free = false;
    glm::dmat3 rotation{1.0};
    std::optional<double> farClip;

    /** The near clip the projection uses: nearClip, or the automatic one. */
    double nearPlane() const;
    glm::dvec3 up() const { return zUp ? glm::dvec3(0, 0, 1) : glm::dvec3(0, 1, 0); }
    /** Up vector of the view itself (differs from world up in top/bottom views). */
    glm::dvec3 viewUp() const { return free ? rotation[1] : ortho ? orthoUp : up(); }

    /**
     * Takes the pose and lens of a camera node with that world matrix. The distance stays as the center of interest,
     * since camera nodes have none of their own.
     */
    void lookThrough(const scene::Node& cam, const glm::dmat4& world);
    /** Back to an orbit camera with the default lens, keeping target and distance. */
    void leaveLookThrough();

    /** "persp", "top", "bottom", "front", "back", "left" or "right". Ortho views keep the current target. */
    void setView(const std::string& name);
    glm::dvec3 forward() const;
    glm::dvec3 right() const;
    glm::dvec3 eye() const { return target - forward() * distance; }

    glm::dmat4 view() const;
    /** Film back half extents: tangents for perspective, world units for orthographic. */
    glm::dvec2 filmExtents() const;
    /** Half extents of the view for a viewport aspect, in the film's units, after the film fit and overscan. */
    glm::dvec2 viewExtents(double aspect) const;
    /** Gate half extents in the film's units, when the camera has a gate aspect. */
    std::optional<glm::dvec2> gateExtents() const;

    /** Reversed-Z, infinite far plane, Vulkan clip space. */
    glm::dmat4 proj(double aspect) const;

    void tumble(double dxDeg, double dyDeg);
    /** Tumbles around pivot instead of the target: the eye swings round it and the target follows, without a jump. */
    void tumbleAbout(const glm::dvec3& pivot, double dxDeg, double dyDeg);
    /** Pan by a fraction of the view width/height. */
    void track(double dxFrac, double dyFrac, double aspect);
    void dolly(double factor);
    void frame(const glm::dvec3& center, double radius, double aspect);

    /** The navigation pose (target, distance, yaw, pitch, ortho width, focal length) as scenes save it per view. */
    nlohmann::json poseJson() const;
    /** Takes what poseJson wrote; missing or malformed keys keep their current values. */
    void setPose(const nlohmann::json& j);
};
} // namespace lila::ui
