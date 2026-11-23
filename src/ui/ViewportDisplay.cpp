#include "ui/ViewportDisplay.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "core/PrefStore.h"
#include "render/CubeLut.h"
#include "render/Grade.h"
#include "render/Ocio.h"
#include "render/DisplayOptions.h"
#include "render/Renderer.h"
#include "scene/LightingChannels.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/Light.h"
#include "scene/Materials.h"
#include "scene/NodeBounds.h"
#include "scene/Transform.h"
#include "scene/PostProcess.h"
#include "scene/SkyAtmosphere.h"
#include "render/Atmosphere.h"
#include "scene/ColorManagement.h"
#include "scene/Relocate.h"
#include "ui/Environments.h"
#include "ui/Theme.h"
#include "ui/VolumeDisplay.h"
#include "scene/SceneIndex.h"

#include <atomic>
#include <mutex>
#include <QGuiApplication>
#include <QImage>
#include <QScreen>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <cmath>

namespace lila::ui::viewportDisplay
{
void apply(render::FrameState& s)
{
    const PrefStore& p = PrefStore::get();
    const bool aa = p.value<bool>("general", kAaEnabled, true);
    const bool temporal = aa && temporalAa();
    s.msaaSamples = temporal ? 1 : render::msaaSamples(aa, p.value<int>("general", kAaSamples, kDefaultSamples), 0xFFu);
    s.taa = temporal || p.value<bool>("general", kTaa, true);
    s.accumulate = std::clamp(p.value<int>("general", kAccumulate, 16), 1, 256);
    s.frameCap = frameCap();
    s.vsync = p.value<bool>("general", kVsync, false);
    s.grid = p.value<bool>("general", kGrid, true);
    s.backfaceCulling = p.value<bool>("general", kBackfaceCulling, false);
    s.xray = p.value<bool>("general", kXray, false);
    s.greyBaseColor = p.value<bool>("general", kGreyBaseColor, false);
    s.lighting = render::lightingFromName(p.value<std::string>("general", kLighting, "environment"));
    s.rigRotation = p.value<float>("general", kRigRotation, 0.0f);
    s.rigTilt = p.value<float>("general", kRigTilt, float(kDefaultRigTilt));
    if (!p.value<bool>("general", kShadows, true))
        s.groundShadow = 0.0f;
    s.exposure = render::exposureScale(p.value<double>("general", kExposure, 0.0));
    s.gamma = p.value<float>("general", kGamma, 1.0f);
    s.kuwahara = p.value<bool>("general", kKuwahara, false);
    s.kuwaharaRadius = p.value<float>("general", kKuwaharaRadius, 4.0f);
    s.outline = p.value<bool>("general", kOutline, false);
    s.outlineWidth = p.value<float>("general", kOutlineWidth, 1.0f);
    s.gridColor = glm::vec3(themeLinear("grid", glm::vec4(s.gridColor, 1.0f)));
    s.gridAxisX = glm::vec3(themeLinear("gridAxisX", glm::vec4(s.gridAxisX, 1.0f)));
    s.gridAxisZ = glm::vec3(themeLinear("gridAxisZ", glm::vec4(s.gridAxisZ, 1.0f)));
    s.gridOrigin = scene::relocate::displayOrigin();
    s.wireSelected = themeLinear("wireSelected", s.wireSelected);
    s.wireSelectedOther = themeLinear("wireSelectedOther", s.wireSelectedOther);
    s.wirePreselect = themeLinear("wirePreselect", s.wirePreselect);
    s.wireUnderSelected = themeLinear("wireUnderSelected", s.wireUnderSelected);
    s.wireDefault = themeLinear("wireframe", s.wireDefault);
    applyPostProcess(s, p.value<bool>("general", kPostEffects, true));
    applyColorManagement(s);
    applyLights(s);
    volumeDisplay::apply(s);
    s.rtShadows = p.value<bool>("general", kRtShadows, true);
    const std::string reflections = p.value<std::string>("general", kReflections, "off");
    s.reflections = reflections == "rayTraced"     ? render::FrameState::Reflections::RayTraced
                    : reflections == "screenSpace" ? render::FrameState::Reflections::ScreenSpace
                                                   : render::FrameState::Reflections::Off;
    s.reflectionCutoff = std::clamp(p.value<float>("general", kReflectionCutoff, 0.6f), 0.05f, 1.0f);
    s.viewportDof = p.value<bool>("general", kViewportDof, true);
    s.viewportMotionBlur = p.value<bool>("general", kViewportMotionBlur, false);
}

int frameCap()
{
    const PrefStore& p = PrefStore::get();
    const int cap = std::max(p.value<int>("general", kFrameCap, render::kFrameCapRefresh), render::kFrameCapRefresh);
    const int background = std::max(p.value<int>("general", kBackgroundFrameCap, kDefaultBackgroundFrameCap), 0);
    if (background == 0 || QGuiApplication::applicationState() == Qt::ApplicationActive)
        return cap;
    // A foreground cap already below the background one stays.
    return cap > 0 && cap < background ? cap : background;
}

double frameInterval()
{
    const int cap = frameCap();
    if (cap > 0)
        return 1.0 / cap;
    if (cap == render::kFrameCapRefresh)
        if (QScreen* screen = QGuiApplication::primaryScreen(); screen && screen->refreshRate() > 1.0)
            return 1.0 / screen->refreshRate();
    return cap == 0 ? 0.0 : 1.0 / 60.0;
}

namespace
{
/**
 * Scale from a light's intensity to the shader's radiance. Unitless is used as is (1 at 1 m matches the default
 * environment's light); physical units come down by the same factor, as nothing in the viewport is calibrated to them.
 */
float unitScale(const scene::lights::Settings& l)
{
    using U = scene::lights::Units;
    constexpr double kPhysical = 0.01;
    const double outer = glm::radians(std::clamp(l.outerConeAngle, 0.1, 90.0));
    switch (l.units)
    {
    case U::Candela:
    case U::Nits:
        return float(kPhysical);
    case U::Lux:
        return float(kPhysical);
    case U::Lumens:
        // Candela from lumens over the solid angle the light spreads into.
        return float(kPhysical / (l.kind == scene::lights::Kind::Spot ? 2.0 * glm::pi<double>() * (1.0 - std::cos(outer)) : 4.0 * glm::pi<double>()));
    case U::Watts:
        return float(kPhysical * 683.0 / (4.0 * glm::pi<double>()));
    default:
        return 1.0f;
    }
}

/** Reversed-Z: depth 1 at the near plane, 0 at the far, as Lila's own projections. */
glm::dmat4 reverseDepth(const glm::dmat4& zeroToOne)
{
    glm::dmat4 r(1.0);
    r[2][2] = -1.0;
    r[3][2] = 1.0;
    return r * zeroToOne;
}

/**
 * A dome light takes over the environment: its image (else the viewport's), turned with the dome about the up axis, at
 * its intensity times its colour's brightness. Only one dome lights the scene.
 */
void applyDome(render::FrameState& s, const scene::lights::Settings& l, const glm::dmat4& world)
{
    if (!l.texture.empty())
    {
        // Lila's own HDRIs are stored by file name, the rest as scene paths.
        s.envPath = environments::resolve(l.texture);
        if (std::filesystem::path(paths::fromUtf8(s.envPath)).is_relative())
            s.envPath = paths::toUtf8(paths::resolvePath(l.texture));
    }
    const glm::dvec3 x = glm::dvec3(world[0]);
    const double yaw = s.zUp ? std::atan2(x.y, x.x) : std::atan2(-x.z, x.x);
    s.envRotation += float(glm::degrees(yaw));
    const glm::dvec3 c = scene::lights::effectiveColor(l);
    const double brightness = 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z;
    s.envIntensity *= float(std::max(l.intensity, 0.0) * unitScale(l) * brightness);
}
} // namespace

/** The sky atmosphere's settings as the renderer takes them: Unreal's scales times colours. */
static render::AtmosphereSettings atmosphereSettings(const scene::Node& sky)
{
    namespace sa = scene::skyAtmosphere;
    const int a = sa::firstAttr();
    auto num = [&](sa::Attr attr) { return sa::scaled(sky, attr); };
    auto vec = [&](int attr) { return glm::vec3(sky.as<glm::dvec3>(a + attr)); };
    render::AtmosphereSettings r;
    r.bottomRadius = num(sa::kBottomRadius);
    r.atmosphereHeight = num(sa::kAtmosphereHeight);
    r.groundAlbedo = vec(sa::kGroundAlbedo);
    r.multiScattering = float(num(sa::kMultiScatteringFactor));
    r.rayleighScattering = vec(sa::kRayleighScattering) * float(num(sa::kRayleighScatteringScale));
    r.rayleighScaleHeight = float(num(sa::kRayleighExponentialDistribution));
    r.mieScattering = vec(sa::kMieScattering) * float(num(sa::kMieScatteringScale));
    r.mieAbsorption = vec(sa::kMieAbsorption) * float(num(sa::kMieAbsorptionScale));
    r.mieAnisotropy = float(num(sa::kMieAnisotropy));
    r.mieScaleHeight = float(num(sa::kMieExponentialDistribution));
    r.otherAbsorption = vec(sa::kOtherAbsorption) * float(num(sa::kOtherAbsorptionScale));
    r.otherTipAltitude = float(num(sa::kOtherTentTipAltitude));
    r.otherTipValue = float(num(sa::kOtherTentTipValue));
    r.otherWidth = float(num(sa::kOtherTentWidth));
    r.skyLuminanceFactor = vec(sa::kSkyLuminanceFactor);
    r.aerialDistanceScale = float(num(sa::kAerialPerspectiveViewDistanceScale));
    r.aerialStartDepth = float(num(sa::kAerialPerspectiveStartDepth));
    r.minLightElevation = float(num(sa::kTransmittanceMinLightElevationAngle));
    return r;
}

/** Tables for the CPU's look-ups (the sun's colour, the fog's), kept while the settings stay the same. */
static const render::AtmosphereLuts& cpuAtmosphereLuts(const render::AtmosphereSettings& settings)
{
    static std::mutex mutex;
    static std::optional<render::AtmosphereSettings> key;
    static render::AtmosphereLuts luts;
    std::lock_guard lock(mutex);
    if (key != settings)
    {
        luts = render::buildAtmosphereLuts(settings);
        key = settings;
    }
    return luts;
}

/**
 * The scene's sky atmosphere into the frame: the sun's light coloured by the air it passes through, the sky baked as
 * the environment in place of the viewport's, and height fog tinted by the sky.
 */
static void applyAtmosphere(render::FrameState& s, scene::NodeId sunId, int sunIndex)
{
    namespace sa = scene::skyAtmosphere;
    const scene::Graph& g = scene::Graph::current();
    const scene::NodeId id = sa::active(g);
    if (id == scene::kNoNode)
        return;
    const scene::Node& sky = *g.node(id);
    render::AtmosphereFrame f;
    f.settings = atmosphereSettings(sky);
    f.zUp = s.zUp;
    f.planetCenter = sa::planetCenter(sky, s.zUp);
    // Unlit without a sun, as in Unreal: the sky stays black.
    f.sunRadiance = glm::vec3(0.0f);
    f.sunDir = s.zUp ? glm::dvec3(0.0, 0.0, 1.0) : glm::dvec3(0.0, 1.0, 0.0);
    if (sunId != scene::kNoNode)
    {
        const scene::Node& sun = *g.node(sunId);
        f.sunDir = glm::normalize(glm::dvec3(sun.as<glm::dmat4>(scene::Transform::kWorldMatrix)[2]));
        const double angle = sun.as<double>(scene::Light::kSourceRadius);
        f.sunAngularRadius = float(glm::radians(angle > 0.0 ? std::min(angle, 10.0) : 0.2678));
    }
    if (sunIndex >= 0)
        f.sunRadiance = s.lights[size_t(sunIndex)].radiance;
    const glm::dvec3 eye = s.eye + s.renderOrigin;
    const render::AtmosphereLuts& luts = cpuAtmosphereLuts(f.settings);
    // Sunlight on the scene has crossed the air down to the ground beneath the camera.
    const glm::dvec3 cam = render::atmosphereCameraKm(f, eye);
    const glm::dvec3 ground = cam / glm::length(cam) * (f.settings.bottomRadius + 0.001);
    if (sunIndex >= 0)
        s.lights[size_t(sunIndex)].radiance *= render::sunTransmittance(f.settings, luts, ground, f.sunDir);
    if (s.fog.on)
    {
        const double contribution = std::clamp(sky.as<double>(sa::firstAttr() + sa::kHeightFogContribution), 0.0, 1.0);
        const glm::dvec3 up = cam / glm::length(cam);
        glm::dvec3 side = glm::cross(up, f.sunDir);
        side = glm::length(side) > 1e-6 ? glm::normalize(side) : glm::dvec3(1.0, 0.0, 0.0);
        const glm::vec3 horizon = render::skyRadiance(f.settings, luts, ground, side, f.sunDir, 16) * f.sunRadiance;
        s.fog.color = glm::mix(s.fog.color, horizon, float(contribution));
    }
    s.envPath = render::atmosphereEnvironmentKey(f, eye);
    s.envRotation = 0.0f;
    s.envIntensity = 1.0f;
    s.envLightSaturation = 1.0f;
    s.atmosphere = f;
}

void applyLights(render::FrameState& s)
{
    LILA_ZONE();
    namespace lights = scene::lights;
    s.lights.clear();
    const scene::Graph& cg = scene::Graph::current();
    scene::Graph& g = const_cast<scene::Graph&>(cg);
    // The visible meshes' bounds, for fitting directional shadows; kept until the scene changes.
    static geo::Bounds sceneBounds;
    static std::pair<uint64_t, uint64_t> boundsKey{0, 0};
    auto bounds = [&]() -> const geo::Bounds& {
        if (const std::pair key(g.structure(), scene::sceneIndex::passStamp(g, "mesh")); boundsKey != key)
        {
            boundsKey = key;
            sceneBounds = {};
            const scene::sceneIndex::Hierarchy& hierarchy = scene::sceneIndex::hierarchy(g, true);
            for (const scene::Node* n : scene::sceneIndex::ofType(g, "mesh"))
                if (hierarchy.visible(n->id()))
                    if (geo::Bounds b = scene::worldBounds(g, n->id()); b.valid())
                    {
                        sceneBounds.extend(b.min);
                        sceneBounds.extend(b.max);
                    }
        }
        return sceneBounds;
    };
    s.atmosphere.reset();
    const scene::NodeId sunId = scene::skyAtmosphere::active(g) != scene::kNoNode ? scene::skyAtmosphere::sunLight(g) : scene::kNoNode;
    int sunIndex = -1;
    if (!PrefStore::get().value<bool>("general", kSceneLights, true))
    {
        applyAtmosphere(s, scene::kNoNode, -1);
        return;
    }
    bool domeDone = false;
    for (const scene::Node* n : scene::sceneIndex::ofType(g, "light"))
    {
        const scene::NodeId id = n->id();
        if (!scene::effectivelyVisible(g, id, true))
            continue;
        const lights::Provider* owner = lights::providerFor(*n);
        if (!owner || owner->id != lights::kLilaProvider)
            continue;
        const lights::Settings l = lights::worldSettings(*n);
        const glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        if (l.kind == lights::Kind::Dome)
        {
            if (!domeDone)
                applyDome(s, l, world);
            domeDone = true;
            continue;
        }
        if (l.intensity <= 0.0)
            continue;
        render::LightDraw d;
        d.kind = l.kind == lights::Kind::Spot          ? render::LightDraw::Spot
                 : l.kind == lights::Kind::Directional ? render::LightDraw::Directional
                 : l.kind == lights::Kind::Area        ? render::LightDraw::Area
                 : l.kind == lights::Kind::Disc        ? render::LightDraw::Disc
                                                       : render::LightDraw::Point;
        d.lightingChannels = scene::lightingChannels::mask(*n);
        d.position = glm::dvec3(world[3]);
        d.direction = glm::vec3(glm::normalize(-glm::dvec3(world[2])));
        d.right = glm::vec3(glm::normalize(glm::dvec3(world[0])));
        d.radiance = glm::vec3(lights::effectiveColor(l) * l.intensity) * unitScale(l);
        d.range = float(std::max(l.range, 0.0));
        d.cosOuter = float(std::cos(glm::radians(std::clamp(l.outerConeAngle, 0.1, 90.0))));
        d.cosInner = float(std::cos(glm::radians(std::clamp(std::min(l.innerConeAngle, l.outerConeAngle - 0.1), 0.0, 90.0))));
        d.width = float(std::max(l.width, 0.0));
        d.height = float(std::max(l.kind == lights::Kind::Disc ? l.width : l.height, 0.0));
        d.radius = float(std::max(l.sourceRadius, 0.0));
        if (d.kind == render::LightDraw::Area || d.kind == render::LightDraw::Disc)
            d.radius = std::max(d.radius, 0.5f * std::sqrt(std::max(d.width * d.height, 0.0f)));

        d.castsShadows = l.castShadows;
        d.lensFlare = n->as<bool>(scene::Light::kLensFlare);
        d.lensFlareIntensity = float(n->as<double>(scene::Light::kLensFlareIntensity));
        d.lensFlareScale = float(n->as<double>(scene::Light::kLensFlareScale));
        const geo::Bounds& b = bounds();
        if (l.castShadows && b.valid() && (d.kind == render::LightDraw::Directional || d.kind == render::LightDraw::Spot))
        {
            const glm::dvec3 centre = b.center();
            const double radius = std::max(glm::length(b.size()) * 0.5, 1.0);
            const glm::dvec3 dir = glm::dvec3(d.direction);
            const glm::dvec3 up = std::abs(dir.y) < 0.99 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
            if (d.kind == render::LightDraw::Directional)
            {
                const glm::dmat4 view = glm::lookAt(centre - dir * radius * 2.0, centre, up);
                d.shadowViewProj = reverseDepth(glm::orthoRH_ZO(-radius, radius, -radius, radius, radius * 0.5, radius * 3.5)) * view;
                d.shadowTexel = float(2.0 * radius / 2048.0);
                const double angle = glm::radians(l.sourceRadius > 0.0 ? std::min(l.sourceRadius, 10.0) : 0.27);
                d.shadowDepth = glm::vec4(float(radius * 0.5), float(radius * 3.5), float(std::tan(angle)), float(0.5 / radius));
            }
            else
            {
                const glm::dvec3 eye = d.position;
                const double farPlane = glm::length(centre - eye) + radius;
                const double fov = 2.0 * glm::radians(std::clamp(l.outerConeAngle, 1.0, 89.0));
                const glm::dmat4 view = glm::lookAt(eye, eye + dir, up);
                d.shadowViewProj = reverseDepth(glm::perspectiveRH_ZO(fov, 1.0, std::max(farPlane * 0.001, 0.5), farPlane)) * view;
                d.shadowTexel = float(2.0 * std::tan(fov * 0.5) * glm::length(centre - eye) / 2048.0);
                d.shadowDepth = glm::vec4(float(std::max(farPlane * 0.001, 0.5)), float(farPlane), d.radius, float(0.5 / std::tan(fov * 0.5)));
            }
            d.shadows = true;
        }
        if (id == sunId)
            sunIndex = int(s.lights.size());
        s.lights.push_back(d);
    }
    applyAtmosphere(s, sunId, sunIndex);
}

void applyLens(render::FrameState& s, const scene::camera::Lens& lens)
{
    // A camera's own post process replaces the scene's for its shots.
    if (!lens.postProcess.empty())
        if (const scene::Node* post = scene::Graph::current().find(lens.postProcess); post && post->type().isA("postProcess"))
        {
            applyPostProcess(s, PrefStore::get().value<bool>("general", kPostEffects, true), post->id());
            applyColorManagement(s);
        }
    if (lens.physicalExposure)
    {
        // EV100 against the default camera's (f/5.6, a 180 degree shutter at 24 fps, ISO 100), so that camera keeps Lila's
        // exposure and every stop of aperture, shutter or ISO moves it by a stop.
        const double fps = std::max(scene::anim::playback().fps, 1.0);
        const double time = std::max(lens.shutterAngle, 1.0) / 360.0 / fps;
        const double ev = std::log2(lens.fStop * lens.fStop / time) - std::log2(std::max(lens.iso, 1.0) / 100.0);
        const double reference = std::log2(5.6 * 5.6 / (0.5 / 24.0));
        s.exposure *= float(std::exp2(reference - ev));
    }
    // The lens shapes flare (ray count, streak stretch) whether or not it blurs.
    s.post.flare.blades = lens.blades;
    s.post.flare.bladeRotation = float(lens.bladeRotation);
    s.post.flare.squeeze = float(std::max(lens.anamorphicSqueeze, 1.0));
    s.shutter = float(std::clamp(lens.shutterAngle, 0.0, 360.0) / 360.0);
    s.anamorphicSqueeze = s.post.flare.squeeze;
    if (!lens.depthOfField)
        return;
    s.lensRadius = float(lens.apertureRadius());
    s.focusDistance = float(lens.focusDistance);
    s.apertureBlades = lens.blades;
    s.apertureRotation = float(lens.bladeRotation);
}

/** A lens dirt value as a file: a built-in by name, else a path. */
static std::string lensDirtPath(const std::string& name)
{
    for (const char* builtin : {"dust", "smudge", "streaks", "rain"})
        if (name == builtin)
            return paths::toUtf8(paths::resourcesDir() / "lensdirt" / (name + ".png"));
    return scene::materials::resolvePath(name);
}

static std::shared_ptr<const render::PostImage> lensDirt(const std::string& name)
{
    // Read once per file and change, so the frame never waits on a decode.
    static std::mutex mutex;
    static std::map<std::string, std::pair<std::filesystem::file_time_type, std::shared_ptr<const render::PostImage>>> cache;
    const std::string path = lensDirtPath(name);
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(paths::fromUtf8(path), ec);
    std::lock_guard lock(mutex);
    if (auto it = cache.find(path); it != cache.end() && it->second.first == stamp)
        return it->second.second;
    std::shared_ptr<const render::PostImage> out;
    QImage img(QString::fromStdString(path));
    if (img.isNull())
        log::warn("lens dirt: cannot read {}", path);
    else
    {
        img = img.convertToFormat(QImage::Format_RGBA8888);
        if (img.width() > 2048)
            img = img.scaledToWidth(2048, Qt::SmoothTransformation);
        auto p = std::make_shared<render::PostImage>();
        p->width = img.width();
        p->height = img.height();
        p->rgba.resize(size_t(p->width) * p->height * 4);
        for (int y = 0; y < p->height; ++y)
            std::memcpy(p->rgba.data() + size_t(y) * p->width * 4, img.constScanLine(y), size_t(p->width) * 4);
        out = std::move(p);
    }
    cache[path] = {stamp, out};
    return out;
}

void applyPostProcess(render::FrameState& s, bool effects, scene::NodeId cameraPost)
{
    LILA_ZONE();
    namespace pp = scene::postProcess;
    const scene::Graph& g = scene::Graph::current();
    const float exposureBefore = s.exposure / std::max(s.postExposure, 1e-12f);
    s.post = {};
    s.exposure = exposureBefore;
    s.postExposure = 1.0f;
    const int a = pp::firstAttr();
    const scene::Node* camNode = cameraPost != scene::kNoNode ? g.node(cameraPost) : nullptr;
    const scene::NodeId id = camNode && camNode->type().isA("postProcess") ? cameraPost : pp::active(g);
    // Volumes the camera stands in blend over the base, lowest priority first; numbers blend, switches flip half way.
    std::vector<std::pair<double, const scene::Node*>> volumes;
    for (const scene::Node* v : scene::sceneIndex::ofType(g, "postProcess"))
        if (v->id() != id && v->as<bool>(a + pp::kEnabled) && v->as<int64_t>(a + pp::kVolume) != 0)
            if (const double w = pp::volumeWeight(*v, s.eye + s.renderOrigin); w > 0.0)
                volumes.emplace_back(w, v);
    if (id == scene::kNoNode && volumes.empty())
        return;
    // Typed parameters, not auto: with auto, x.second->as<double> is a dependent name that needs "->template as<double>".
    // MSVC accepts it without, Clang rejects it.
    std::ranges::sort(volumes, [&](const std::pair<double, const scene::Node*>& x, const std::pair<double, const scene::Node*>& y) { return x.second->as<double>(a + pp::kVolumePriority) < y.second->as<double>(a + pp::kVolumePriority); });
    const scene::Node& base = id != scene::kNoNode ? *g.node(id) : *volumes.front().second;
    std::vector<scene::PlugValue> vals(pp::kCount);
    for (int i = 0; i < pp::kCount; ++i)
        vals[size_t(i)] = base.value(a + i);
    for (const auto& [w, v] : volumes)
        for (int i = pp::kTonemapper; i < pp::kVolume; ++i)
        {
            const scene::PlugValue other = v->value(a + i);
            scene::PlugValue& mine = vals[size_t(i)];
            if (const double* x = std::get_if<double>(&mine); x && std::holds_alternative<double>(other))
                mine = *x + (std::get<double>(other) - *x) * w;
            else if (const glm::dvec3* xv = std::get_if<glm::dvec3>(&mine); xv && std::holds_alternative<glm::dvec3>(other))
                mine = *xv + (std::get<glm::dvec3>(other) - *xv) * w;
            else if (w >= 0.5)
                mine = other;
        }
    auto valF = [&](int attr) { return std::get<double>(vals[size_t(attr)]); };
    auto valB = [&](int attr) { return std::get<bool>(vals[size_t(attr)]); };
    auto valI = [&](int attr) { return std::get<int64_t>(vals[size_t(attr)]); };
    auto valS = [&](int attr) { return std::get<std::string>(vals[size_t(attr)]); };
    auto valV = [&](int attr) { return std::get<glm::dvec3>(vals[size_t(attr)]); };
    auto num = [&](int attr) { return float(valF(attr)); };
    auto vec = [&](int attr) { return glm::vec3(valV(attr)); };
    render::PostSettings& p = s.post;
    s.fog = {};
    p.tonemapper = render::PostSettings::Tonemapper(std::clamp(int(valI(pp::kTonemapper)), 0, 3));
    p.agxLook = int(valI(pp::kAgxLook));
    s.postExposure = std::exp2(num(pp::kExposure));
    s.exposure *= s.postExposure;
    p.autoExposure = valB(pp::kAutoExposure);
    p.exposureCompensation = num(pp::kExposureCompensation);
    p.exposureMin = num(pp::kExposureMin);
    p.exposureMax = std::max(num(pp::kExposureMax), p.exposureMin);
    p.exposureSpeed = num(pp::kExposureSpeed);
    p.effects = effects;
    p.whiteBalance = glm::vec3(pp::whiteBalance(valF(pp::kTemperature), valF(pp::kTint)));
    p.contrast = num(pp::kContrast);
    p.saturation = num(pp::kSaturation);
    p.lift = vec(pp::kLift);
    p.gamma = vec(pp::kGamma);
    p.gain = vec(pp::kGain);
    p.offset = vec(pp::kOffset);
    p.liftMaster = num(pp::kLiftMaster);
    p.gammaMaster = num(pp::kGammaMaster);
    p.gainMaster = num(pp::kGainMaster);
    p.offsetMaster = num(pp::kOffsetMaster);
    p.shadows = vec(pp::kShadows);
    p.midtones = vec(pp::kMidtones);
    p.highlights = vec(pp::kHighlights);
    p.shadowsRange = num(pp::kShadowsRange);
    p.highlightsRange = std::max(num(pp::kHighlightsRange), p.shadowsRange);
    std::array<std::string, render::grade::kCurveCount> curves;
    for (int i = 0; i < render::grade::kCurveCount; ++i)
        curves[size_t(i)] = valS(pp::kCurveMaster + i);
    p.curves = render::grade::curves(curves);
    p.bloom = num(pp::kBloom);
    p.bloomRadius = num(pp::kBloomRadius);
    p.vignette = num(pp::kVignette);
    p.grain = num(pp::kGrain);
    p.chromaticAberration = num(pp::kChromaticAberration);
    p.distortion = num(pp::kDistortion);
    p.sharpen = num(pp::kSharpen);
    p.lutIntensity = num(pp::kLutIntensity);
    if (const std::string lut = valS(pp::kLut); !lut.empty())
        p.lut = render::loadCubeLut(lut);
    render::LensFlare& flare = p.flare;
    flare.intensity = num(pp::kLensFlare);
    flare.threshold = num(pp::kFlareThreshold);
    flare.ghosts = num(pp::kFlareGhosts);
    flare.ghostSpacing = num(pp::kFlareGhostSpacing);
    flare.chroma = num(pp::kFlareChroma);
    flare.halo = num(pp::kFlareHalo);
    flare.haloRadius = num(pp::kFlareHaloRadius);
    flare.starburst = num(pp::kFlareStarburst);
    flare.streakLength = num(pp::kFlareStreakLength);
    flare.anamorphic = num(pp::kAnamorphicStreaks);
    flare.anamorphicTint = vec(pp::kAnamorphicTint);
    flare.dirtIntensity = num(pp::kLensDirtIntensity);
    if (const std::string dirt = valS(pp::kLensDirt); !dirt.empty() && flare.dirtIntensity > 0.0f)
        flare.dirt = lensDirt(dirt);
    render::FrameState::Fog& fog = s.fog;
    fog.on = valB(pp::kFog) && num(pp::kFogDensity) > 0.0f;
    fog.color = vec(pp::kFogColor);
    fog.density = num(pp::kFogDensity) * 0.01f;
    fog.height = num(pp::kFogHeight);
    fog.falloff = num(pp::kFogFalloff) * 0.01f;
    fog.start = num(pp::kFogStart);
    fog.sunScatter = num(pp::kFogSunScatter);
    fog.anisotropy = std::clamp(num(pp::kFogAnisotropy), 0.0f, 0.95f);
    p.halation = num(pp::kHalation);
    p.halationThreshold = num(pp::kHalationThreshold);
    p.halationTint = vec(pp::kHalationTint);
    p.grainSize = num(pp::kGrainSize);
    p.grainColor = num(pp::kGrainColor);
    p.grainResponse = num(pp::kGrainResponse);
    p.letterboxAspect = num(pp::kLetterboxAspect);
    p.letterboxOpacity = num(pp::kLetterboxOpacity);
}

namespace
{
std::mutex gColorNoteMutex;
std::string gColorNote;
} // namespace

void applyColorManagement(render::FrameState& s)
{
    LILA_ZONE();
    namespace cm = scene::colorManagement;
    const cm::Settings c = cm::settings(scene::Graph::current(), PrefStore::get().value<std::string>("general", cm::kConfigPref, ""));
    s.post.view = nullptr;
    std::string note;
    if (c.ocio)
    {
        std::string err;
        s.post.view = render::ocio::viewTransform(c.config, c.display, c.view, c.look, &err);
        if (!s.post.view)
            note = "OCIO failed (" + err + "): Lila's tonemapper";
    }
    std::lock_guard lock(gColorNoteMutex);
    gColorNote = std::move(note);
}

std::string colorNote()
{
    std::lock_guard lock(gColorNoteMutex);
    return gColorNote;
}

bool defaultMaterial()
{
    return PrefStore::get().value<bool>("general", kDefaultMaterial, false);
}

bool randomColors()
{
    return PrefStore::get().value<bool>("general", kRandomColors, false);
}

RandomPalette randomPalette()
{
    return PrefStore::get().value<std::string>("general", kRandomColorsPalette, "contrast") == "pastel" ? RandomPalette::Pastel : RandomPalette::Contrast;
}

void setRandomPalette(RandomPalette palette)
{
    PrefStore::get().set("general", kRandomColorsPalette, std::string(palette == RandomPalette::Pastel ? "pastel" : "contrast"));
}

int randomSeed()
{
    return PrefStore::get().value<int>("general", kRandomColorsSeed, 0);
}

glm::vec3 randomColor(std::string_view name, RandomPalette palette, int seed)
{
    uint64_t h = 1469598103934665603ull ^ (uint64_t(uint32_t(seed)) * 0x9e3779b97f4a7c15ull); // FNV-1a
    for (unsigned char ch : name)
        h = (h ^ ch) * 1099511628211ull;
    // Finalise so names differing in one character (pCube1, pCube2) land far apart.
    h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ull;
    h = (h ^ (h >> 27)) * 0x94d049bb133111ebull;
    h ^= h >> 31;
    auto unit = [h](int shift) { return float((h >> shift) & 0xffff) / 65535.0f; };
    float hue = unit(0) * 6.0f, sat = 0.35f + 0.3f * unit(16), val = 0.7f + 0.2f * unit(32);
    if (palette == RandomPalette::Pastel)
    {
        sat = 0.12f + 0.18f * unit(16);
        val = 0.85f + 0.1f * unit(32);
    }
    const float f = hue - std::floor(hue), p = val * (1.0f - sat), q = val * (1.0f - sat * f), t = val * (1.0f - sat * (1.0f - f));
    glm::vec3 rgb;
    switch (int(hue) % 6)
    {
    case 0: rgb = {val, t, p}; break;
    case 1: rgb = {q, val, p}; break;
    case 2: rgb = {p, val, t}; break;
    case 3: rgb = {p, q, val}; break;
    case 4: rgb = {t, p, val}; break;
    default: rgb = {val, p, q}; break;
    }
    // HSV is picked in display space; draws take linear colour.
    return glm::pow(rgb, glm::vec3(2.2f));
}

bool highlightSelection()
{
    return PrefStore::get().value<bool>("general", kHighlightSelection, true);
}

SelectionOrderColors selectionOrderColors(const scene::Graph& g)
{
    const std::vector<scene::NodeId>& sel = g.selection();
    if (sel.size() < 2)
        return {};
    const std::string mode = PrefStore::get().value<std::string>("general", kSelectionOrderColors, "last");
    if (mode == "last")
        return {sel.back(), true};
    if (mode == "allButFirst")
        return {sel.front(), false};
    if (mode == "first")
        return {sel.front(), true};
    return {};
}

ComponentThickness componentThickness()
{
    const PrefStore& p = PrefStore::get();
    const ComponentThickness d;
    auto get = [&](const char* key, double fallback) { return std::max(0.0, p.value<double>("general", key, fallback)); };
    return {get(kEdgeThickness, d.edge),     get(kEdgeThicknessHover, d.edgeHover),   get(kEdgeThicknessSelected, d.edgeSelected),
            get(kVertexThickness, d.vertex), get(kVertexThicknessHover, d.vertexHover), get(kVertexThicknessSelected, d.vertexSelected),
            get(kUvSeamThickness, d.uvSeam), get(kSymmetryEdgeThickness, d.symmetryEdge)};
}

bool accentColors(const char* key)
{
    const PrefStore& p = PrefStore::get();
    return accentColors(p.value<bool>("general", kMonotone, false), p.value<bool>("general", key, true));
}

glm::vec3 layerTint(const glm::vec3& linear)
{
    if (!PrefStore::get().value<bool>("general", kMonotone, false))
        return linear;
    return glm::vec3(glm::dot(linear, glm::vec3(0.2126f, 0.7152f, 0.0722f)));
}

glm::vec3 axisColor(int axis)
{
    static constexpr glm::vec3 kAxes[3] = {{0.85f, 0.40f, 0.40f}, {0.50f, 0.80f, 0.42f}, {0.42f, 0.56f, 0.90f}};
    static constexpr const char* kTokens[3] = {"axisX", "axisY", "axisZ"};
    int i = std::clamp(axis, 0, 2);
    return glm::vec3(themeSrgb(kTokens[i], glm::vec4(kAxes[i], 1.0f)));
}

glm::vec3 keyTickColor()
{
    return glm::vec3(themeSrgb("keyTick", glm::vec4(0.86f, 0.36f, 0.36f, 1.0f)));
}

render::VertexColorMode vertexColorMode()
{
    std::string m = PrefStore::get().value<std::string>("general", kVertexColors, "off");
    return m == "shaded" ? render::VertexColorMode::Shaded : m == "unlit" ? render::VertexColorMode::Unlit : render::VertexColorMode::Off;
}

render::ColorChannel colorChannel()
{
    std::string c = PrefStore::get().value<std::string>("general", kColorChannel, "rgb");
    return c == "r" ? render::ColorChannel::R
           : c == "g" ? render::ColorChannel::G
           : c == "b" ? render::ColorChannel::B
           : c == "a" ? render::ColorChannel::A
                      : render::ColorChannel::Rgb;
}

namespace
{
float toLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

/** Piecewise-linear ramp through sRGB stops, returned linear. */
glm::vec4 ramp(std::initializer_list<glm::vec3> stops, float w)
{
    const glm::vec3* s = stops.begin();
    float x = std::clamp(w, 0.0f, 1.0f) * float(stops.size() - 1);
    size_t i = std::min(size_t(x), stops.size() - 2);
    glm::vec3 c = glm::mix(s[i], s[i + 1], x - float(i));
    return {toLinear(c.r), toLinear(c.g), toLinear(c.b), 1.0f};
}
} // namespace

bool grayscaleWeights()
{
    return PrefStore::get().value<bool>("general", kGrayscaleWeights, false);
}

glm::vec4 weightColor(float w, bool grayscale)
{
    if (grayscale)
    {
        float v = toLinear(std::clamp(w, 0.0f, 1.0f));
        return {v, v, v, 1.0f};
    }
    return ramp({{0.0f, 0.0f, 0.0f}, {0.1f, 0.15f, 0.85f}, {0.0f, 0.75f, 0.9f}, {0.1f, 0.85f, 0.2f}, {0.95f, 0.9f, 0.1f}, {0.95f, 0.2f, 0.1f},
                 {1.0f, 1.0f, 1.0f}},
                w);
}

glm::vec4 falloffColor(float w, bool grayscale)
{
    if (grayscale)
    {
        float v = toLinear(std::clamp(w, 0.0f, 1.0f));
        return {v, v, v, 1.0f};
    }
    return ramp({{0.25f, 0.0f, 0.0f}, {0.8f, 0.1f, 0.0f}, {1.0f, 0.5f, 0.0f}, {1.0f, 1.0f, 0.0f}}, w);
}

void applyQuality(Quality q)
{
    PrefStore& p = PrefStore::get();
    p.set("general", "/viewport/ao/enabled", q != Quality::Fastest);
    p.set("general", "/viewport/ao/mode", q == Quality::Best ? "accurate" : "fast");
    p.set("general", kAaEnabled, q != Quality::Fastest);
    p.set("general", kAaSamples, 8);
    p.set("general", kAaMode, q == Quality::Balanced ? kAaTemporal : "multisample");
    p.set("general", kTaa, q != Quality::Fastest);
    p.set("general", kShadows, q != Quality::Fastest);
}

bool temporalAa()
{
    return PrefStore::get().value<std::string>("general", kAaMode, "multisample") == kAaTemporal;
}

void turnLight(double degrees)
{
    PrefStore& p = PrefStore::get();
    const std::string mode = p.value<std::string>("general", kLighting, "environment");
    if (mode == "flat")
        return;
    const char* pref = mode == "environment" ? kEnvRotation : kRigRotation;
    p.set("general", pref, std::remainder(p.value<double>("general", pref, 0.0) + degrees, 360.0));
}

void resetLight()
{
    PrefStore& p = PrefStore::get();
    const std::string mode = p.value<std::string>("general", kLighting, "environment");
    if (mode == "environment")
        p.set("general", kEnvRotation, 0.0);
    else
    {
        p.set("general", kRigRotation, 0.0);
        p.set("general", kRigTilt, kDefaultRigTilt);
    }
}

void tiltLight(double degrees)
{
    PrefStore& p = PrefStore::get();
    if (p.value<std::string>("general", kLighting, "environment") != "sculpt")
        return;
    p.set("general", kRigTilt, std::clamp(p.value<double>("general", kRigTilt, kDefaultRigTilt) + degrees, 0.0, 90.0));
}

Quality recommendedQuality()
{
    const render::Renderer::GpuInfo gpu = render::Renderer::get().gpuInfo();
    constexpr uint64_t kGiB = 1ull << 30;
    if (gpu.name.empty())
        return Quality::Best;
    if (!gpu.discrete || gpu.vramBytes < 4 * kGiB)
        return Quality::Fastest;
    return gpu.vramBytes < 8 * kGiB ? Quality::Balanced : Quality::Best;
}

namespace
{
std::atomic<bool> gViewSwitcherDefault{false};
}

void setViewSwitcherDefault(bool on)
{
    gViewSwitcherDefault = on;
}

bool viewSwitcherDefault()
{
    return gViewSwitcherDefault;
}
} // namespace lila::ui::viewportDisplay
