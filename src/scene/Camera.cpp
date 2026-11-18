#include "scene/Camera.h"

#include "ops/Command.h"
#include "scene/AttrBuilders.h"
#include "scene/Graph.h"

#include <algorithm>
#include <cmath>

namespace lila::scene
{
using namespace attrdefs;

namespace
{
std::vector<AttrDef> cameraAttrs()
{
    std::vector<AttrDef> a(Camera::kCount - Camera::kFocalLength);
    auto at = [&](int attr) -> AttrDef& { return a[size_t(attr - Camera::kFocalLength)]; };
    at(Camera::kFocalLength) = floatAttr("focalLength", "fl", 35.0, "Lens focal length, mm.", 2.5, 3500.0);
    at(Camera::kHorizontalFilmAperture) = floatAttr("horizontalFilmAperture", "hfa", 1.41732, "Film back width, inches.", 1e-3);
    at(Camera::kVerticalFilmAperture) = floatAttr("verticalFilmAperture", "vfa", 0.94488, "Film back height, inches.", 1e-3);
    at(Camera::kFilmFit) = {.name = "filmFit", .shortName = "ff", .type = PlugType::Enum, .defaultValue = int64_t(1), .flags = kStoredFlags,
                            .enumNames = {"fill", "horizontal", "vertical", "overscan"},
                            .doc = "How the film gate fits the render resolution."};
    at(Camera::kNearClipPlane) = floatAttr("nearClipPlane", "ncp", 0.1, "Near clipping distance, cm.", 1e-3);
    at(Camera::kFarClipPlane) = floatAttr("farClipPlane", "fcp", 10000.0, "Far clipping distance, cm.", 1e-3);
    at(Camera::kOrthographic) = {.name = "orthographic", .shortName = "o", .type = PlugType::Bool, .defaultValue = false, .flags = kStoredFlags,
                                 .doc = "Parallel projection instead of perspective."};
    at(Camera::kOrthographicWidth) = floatAttr("orthographicWidth", "ow", 30.0, "Width of an orthographic view, cm.", 1e-3);
    at(Camera::kLocked) = {.name = "locked", .shortName = "lc", .type = PlugType::Bool, .defaultValue = false, .flags = kStoredFlags,
                           .doc = "Lock Camera: the camera is not meant to be moved while looking through it."};
    at(Camera::kOverscan) = {.name = "overscan", .shortName = "ovr", .type = PlugType::Float, .defaultValue = 1.0, .flags = kStoredFlags,
                             .doc = "View size relative to the gate; 1.0 fits it exactly.", .minValue = 0.01};
    auto flag = [](std::string name, std::string shortName, bool def, std::string doc) {
        return AttrDef{.name = std::move(name), .shortName = std::move(shortName), .type = PlugType::Bool, .defaultValue = def,
                       .flags = kStoredFlags, .doc = std::move(doc)};
    };
    at(Camera::kDisplayResolution) = flag("displayResolution", "dr", false, "Show the resolution gate when looking through.");
    at(Camera::kDisplayFilmGate) = flag("displayFilmGate", "dfg", false, "Show the film gate when looking through.");
    at(Camera::kDisplayGateMask) = flag("displayGateMask", "dgm", true, "Darken outside the gate.");
    at(Camera::kDisplayGateMaskOpacity) = {.name = "displayGateMaskOpacity", .shortName = "dgo", .type = PlugType::Float,
                                           .defaultValue = 0.7, .flags = kStoredFlags, .doc = "Gate mask opacity, 0 to 1.", .minValue = 0.0,
                                           .maxValue = 1.0};
    at(Camera::kBookmarks) = {.name = "bookmarks", .shortName = "bkm", .type = PlugType::String, .defaultValue = std::string(),
                              .flags = kStoredFlags | AttrHidden,
                              .doc = "Camera bookmarks saved while looking through this camera, as JSON by slot."};
    at(Camera::kDepthOfField) = flag("depthOfField", "dof", false, "Blur what is off the focus distance, in renders and in a still view.");
    at(Camera::kFStop) = floatAttr("fStop", "fs", 5.6, "Aperture f-number: lower blurs more.", 0.5, 64.0);
    at(Camera::kFocusDistance) = floatAttr("focusDistance", "fd", 500.0, "Distance in focus, cm.", 0.1);
    at(Camera::kApertureBlades) = {.name = "apertureBlades", .shortName = "ab", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = kStoredFlags,
                                   .doc = "Blades shaping the bokeh; 0 is round.", .minValue = 0.0, .maxValue = 16.0};
    at(Camera::kApertureRotation) = floatAttr("apertureRotation", "ar", 0.0, "Rotation of the bladed aperture, degrees.");
    at(Camera::kShutterAngle) = floatAttr("shutterAngle", "sa", 180.0, "Motion blur: degrees of each frame the shutter is open (180 is half).", 0.0, 360.0);
    at(Camera::kAnamorphicSqueeze) = floatAttr("anamorphicSqueeze", "asq", 1.0,
                                               "Anamorphic lens squeeze: 1 is spherical; 1.33 to 2 stretch bokeh tall and flare streaks wide.", 1.0, 2.5);
    at(Camera::kPhysicalExposure) = flag("physicalExposure", "pex", false,
                                         "Expose by the camera: fStop, shutterAngle (at the scene frame rate) and iso set the exposure as on a real camera.");
    at(Camera::kIso) = floatAttr("iso", "iso", 100.0, "Physical exposure: film speed.", 1.0, 409600.0);
    at(Camera::kPostProcess) = {.name = "postProcess", .shortName = "ppr", .type = PlugType::String, .defaultValue = std::string(),
                                .flags = kStoredFlags, .doc = "A post process node this camera grades with in place of the scene's, so shots can look different."};
    return a;
}
} // namespace

namespace camera
{
Lens lens(const Node& cam)
{
    Lens l;
    l.depthOfField = cam.as<bool>(Camera::kDepthOfField);
    l.fStop = cam.as<double>(Camera::kFStop);
    l.focusDistance = cam.as<double>(Camera::kFocusDistance);
    l.blades = int(cam.as<int64_t>(Camera::kApertureBlades));
    l.bladeRotation = cam.as<double>(Camera::kApertureRotation);
    l.shutterAngle = cam.as<double>(Camera::kShutterAngle);
    l.focalLength = cam.as<double>(Camera::kFocalLength);
    l.anamorphicSqueeze = cam.as<double>(Camera::kAnamorphicSqueeze);
    l.physicalExposure = cam.as<bool>(Camera::kPhysicalExposure);
    l.iso = cam.as<double>(Camera::kIso);
    l.postProcess = cam.as<std::string>(Camera::kPostProcess);
    return l;
}

imagePlane::CameraFrame frame(const Node& cam, const glm::dmat4& world)
{
    imagePlane::CameraFrame f;
    f.eye = glm::dvec3(world[3]);
    f.right = glm::normalize(glm::dvec3(world[0]));
    f.up = glm::normalize(glm::dvec3(world[1]));
    f.forward = -glm::normalize(glm::dvec3(world[2]));
    f.ortho = cam.as<bool>(Camera::kOrthographic);
    f.film = filmExtents(cam);
    return f;
}

glm::dvec2 filmExtents(const Node& cam)
{
    const double hfa = std::max(cam.as<double>(Camera::kHorizontalFilmAperture), 1e-6);
    const double vfa = std::max(cam.as<double>(Camera::kVerticalFilmAperture), 1e-6);
    if (cam.as<bool>(Camera::kOrthographic))
    {
        const double half = std::max(cam.as<double>(Camera::kOrthographicWidth), 1e-6) * 0.5;
        return {half, half * vfa / hfa};
    }
    // Half the aperture in mm over the focal length: the tangent of the half angle of view.
    const double fl = std::max(cam.as<double>(Camera::kFocalLength), 1e-6);
    return glm::dvec2(hfa, vfa) * (25.4 * 0.5 / fl);
}

FilmFit filmFit(const Node& cam)
{
    return FilmFit(std::clamp<int64_t>(cam.as<int64_t>(Camera::kFilmFit), 0, 3));
}

glm::dvec2 viewExtents(const Node& cam, double aspect, double overscan, std::optional<double> gateAspect)
{
    return fitView(filmExtents(cam), aspect, filmFit(cam), overscan, gateAspect);
}

glm::dvec2 angleOfView(const Node& cam, double aspect)
{
    glm::dvec2 e = viewExtents(cam, aspect);
    return {glm::degrees(2.0 * std::atan(e.x)), glm::degrees(2.0 * std::atan(e.y))};
}

glm::dmat4 projection(glm::dvec2 ext, bool ortho, double nearClip, double farClip)
{
    glm::dmat4 p(0.0);
    if (ortho)
    {
        const double farD = std::max(farClip, nearClip + 1e-6);
        p[0][0] = 1.0 / ext.x;
        p[1][1] = -1.0 / ext.y;
        p[2][2] = 1.0 / (farD - nearClip);
        p[3][2] = farD / (farD - nearClip);
        p[3][3] = 1.0;
        return p;
    }
    p[0][0] = 1.0 / ext.x;
    p[1][1] = -1.0 / ext.y;
    p[2][3] = -1.0;
    if (farClip > nearClip)
    {
        p[2][2] = nearClip / (farClip - nearClip);
        p[3][2] = nearClip * farClip / (farClip - nearClip);
    }
    else
        p[3][2] = nearClip;
    return p;
}

glm::dmat4 projection(const Node& cam, double aspect, double overscan, std::optional<double> gateAspect)
{
    const double nearClip = std::max(cam.as<double>(Camera::kNearClipPlane), 1e-6);
    return projection(viewExtents(cam, aspect, overscan, gateAspect), cam.as<bool>(Camera::kOrthographic), nearClip,
                      cam.as<double>(Camera::kFarClipPlane));
}

glm::dmat4 viewMatrix(const glm::dmat4& world)
{
    glm::dvec3 back = glm::normalize(glm::dvec3(world[2]));
    glm::dvec3 right = glm::normalize(glm::cross(glm::dvec3(world[1]), back));
    glm::dvec3 up = glm::cross(back, right);
    glm::dmat4 m(1.0);
    m[0] = glm::dvec4(right, 0.0);
    m[1] = glm::dvec4(up, 0.0);
    m[2] = glm::dvec4(back, 0.0);
    m[3] = world[3];
    return glm::inverse(m);
}

glm::dmat4 placedWorld(const glm::dmat4& world, const glm::dvec3& eye, const glm::dvec3& lookAt, const glm::dvec3& up)
{
    glm::dvec3 back = eye - lookAt;
    back = glm::length(back) > 1e-12 ? glm::normalize(back) : glm::normalize(glm::dvec3(world[2]));
    glm::dvec3 right = glm::cross(up, back);
    // Looking straight along up: keep the camera's own right instead.
    if (glm::length(right) < 1e-9)
        right = glm::dvec3(world[0]) - back * glm::dot(glm::dvec3(world[0]), back);
    right = glm::normalize(right);
    glm::dvec3 trueUp = glm::cross(back, right);
    glm::dvec3 scale(glm::length(glm::dvec3(world[0])), glm::length(glm::dvec3(world[1])), glm::length(glm::dvec3(world[2])));
    glm::dmat4 m(1.0);
    m[0] = glm::dvec4(right * scale.x, 0.0);
    m[1] = glm::dvec4(trueUp * scale.y, 0.0);
    m[2] = glm::dvec4(back * scale.z, 0.0);
    m[3] = glm::dvec4(eye, 1.0);
    return m;
}
CameraSettings settings(const Node& cam)
{
    CameraSettings c;
    c.filmFit = filmFit(cam);
    c.overscan = std::clamp(cam.as<double>(Camera::kOverscan), 0.01, 100.0);
    c.resolutionGate = cam.as<bool>(Camera::kDisplayResolution);
    c.filmGate = cam.as<bool>(Camera::kDisplayFilmGate);
    c.gateMask = cam.as<bool>(Camera::kDisplayGateMask);
    c.gateMaskOpacity = std::clamp(cam.as<double>(Camera::kDisplayGateMaskOpacity), 0.0, 1.0);
    c.nearClip = cam.as<double>(Camera::kNearClipPlane);
    return c;
}

void setSettings(Graph& g, NodeId cam, const CameraSettings& c)
{
    auto set = [&](int attr, PlugValue v) {
        if (g.node(cam)->value(attr) != v)
            g.setValue({cam, attr, -1}, std::move(v));
    };
    set(Camera::kFilmFit, int64_t(c.filmFit));
    set(Camera::kOverscan, c.overscan);
    set(Camera::kDisplayResolution, c.resolutionGate);
    set(Camera::kDisplayFilmGate, c.filmGate);
    set(Camera::kDisplayGateMask, c.gateMask);
    set(Camera::kDisplayGateMaskOpacity, c.gateMaskOpacity);
    if (c.nearClip > 0.0)
        set(Camera::kNearClipPlane, c.nearClip);
}

void migrateNamedSettings(Graph& g, RenderSettings& r)
{
    for (auto it = r.cameras.begin(); it != r.cameras.end();)
    {
        const Node* n = nullptr;
        try
        {
            n = isViewName(it->first) ? nullptr : g.find(it->first);
        }
        catch (const std::exception&)
        {
        }
        if (n && n->type().isA("camera"))
        {
            setSettings(g, n->id(), it->second);
            it = r.cameras.erase(it);
        }
        else
            ++it;
    }
}
} // namespace camera

void registerCameraTypes()
{
    if (types::find("camera"))
        return;
    NodeType t;
    t.name = "camera";
    t.icon = "camera";
    t.base = types::find("locator");
    t.dag = true;
    t.doc = "A scene camera: lens, film back and clipping, looking down its local -Z.";
    t.ownAttrs = cameraAttrs();
    t.ownAttrs.insert(t.ownAttrs.begin(), Locator::hiddenDrawSize());
    t.create = [] { return std::make_unique<Camera>(); };
    types::registerType(std::move(t));
}

void registerCameraCommands()
{
    using T = ArgType;
    if (ops::find("viewPlace"))
        return;
    ops::registerCommand({
        .name = "viewPlace",
        .doc = "Place a camera node: eye is its world position, lookAt the point it aims at, up the "
               "world direction its top leans to. Flags left out keep the camera's current value; with eye alone it moves "
               "without turning. Fails on a locked camera.",
        .positionalDoc = "camera",
        .flags = {
            {"eye", "eye", T::Vec3, nullptr, "World position, cm."},
            {"lookAt", "la", T::Vec3, nullptr, "World point to aim at, cm."},
            {"up", "up", T::Vec3, nullptr, "World up direction."},
            {"orthographicWidth", "ow", T::Float, nullptr, "Width of an orthographic view, cm.", {}, {}, 1e-3},
        },
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("viewPlace: camera name required");
            Graph& g = Graph::current();
            std::string name = a.positional[0].get<std::string>();
            Node* n = g.find(name);
            if (!n || !n->type().isA("camera"))
                throw CommandError("viewPlace: '" + name + "' is not a camera");
            if (n->as<bool>(Camera::kLocked))
                throw CommandError("viewPlace: camera '" + name + "' is locked");
            g.evaluate();
            auto vec = [&](const char* flag) {
                const json& j = a.flags[flag];
                return glm::dvec3(j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>());
            };
            const glm::dmat4 world = n->as<glm::dmat4>(Transform::kWorldMatrix);
            const imagePlane::CameraFrame f = camera::frame(*n, world);
            glm::dvec3 eye = a.has("eye") ? vec("eye") : f.eye;
            glm::dvec3 lookAt = a.has("lookAt") ? vec("lookAt") : eye + f.forward;
            glm::dvec3 up = a.has("up") ? vec("up") : f.up;
            glm::dmat4 placed = camera::placedWorld(world, eye, lookAt, up);
            Transform::setLocalMatrix(g, n->id(), glm::inverse(n->as<glm::dmat4>(Transform::kParentMatrix)) * placed);
            if (a.has("orthographicWidth"))
                g.setValue({n->id(), Camera::kOrthographicWidth, -1}, std::max(a.get<double>("orthographicWidth"), 1e-3));
            return nullptr;
        },
    });
}
} // namespace lila::scene
