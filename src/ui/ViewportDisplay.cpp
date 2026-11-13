#include "ui/ViewportDisplay.h"

#include "core/Paths.h"
#include "core/Profile.h"
#include "core/PrefStore.h"
#include "render/CubeLut.h"
#include "render/DisplayOptions.h"
#include "render/Renderer.h"
#include "scene/LightingChannels.h"
#include "scene/Graph.h"
#include "scene/Light.h"
#include "scene/NodeBounds.h"
#include "scene/Transform.h"
#include "scene/PostProcess.h"
#include "scene/Relocate.h"
#include "ui/Environments.h"
#include "ui/Theme.h"
#include "scene/SceneIndex.h"

#include <atomic>
#include <QGuiApplication>
#include <QScreen>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
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
    applyLights(s);
    s.rtShadows = p.value<bool>("general", kRtShadows, true);
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
    if (!PrefStore::get().value<bool>("general", kSceneLights, true))
        return;
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
        s.lights.push_back(d);
    }
}

void applyLens(render::FrameState& s, const scene::camera::Lens& lens)
{
    if (!lens.depthOfField)
        return;
    s.lensRadius = float(lens.apertureRadius());
    s.focusDistance = float(lens.focusDistance);
    s.apertureBlades = lens.blades;
    s.apertureRotation = float(lens.bladeRotation);
}

void applyPostProcess(render::FrameState& s, bool effects)
{
    LILA_ZONE();
    namespace pp = scene::postProcess;
    s.post = {};
    const scene::Graph& g = scene::Graph::current();
    const scene::NodeId id = pp::active(g);
    if (id == scene::kNoNode)
        return;
    const scene::Node& n = *g.node(id);
    const int a = pp::firstAttr();
    auto num = [&](int attr) { return float(n.as<double>(a + attr)); };
    auto vec = [&](int attr) { return glm::vec3(n.as<glm::dvec3>(a + attr)); };
    render::PostSettings& p = s.post;
    s.fog = {};
    p.tonemapper = render::PostSettings::Tonemapper(std::clamp(int(n.as<int64_t>(a + pp::kTonemapper)), 0, 3));
    p.agxLook = int(n.as<int64_t>(a + pp::kAgxLook));
    s.exposure *= std::exp2(num(pp::kExposure));
    p.autoExposure = n.as<bool>(a + pp::kAutoExposure);
    p.exposureCompensation = num(pp::kExposureCompensation);
    p.exposureMin = num(pp::kExposureMin);
    p.exposureMax = std::max(num(pp::kExposureMax), p.exposureMin);
    p.exposureSpeed = num(pp::kExposureSpeed);
    p.effects = effects;
    p.whiteBalance = glm::vec3(pp::whiteBalance(n.as<double>(a + pp::kTemperature), n.as<double>(a + pp::kTint)));
    p.contrast = num(pp::kContrast);
    p.saturation = num(pp::kSaturation);
    p.lift = vec(pp::kLift);
    p.gamma = vec(pp::kGamma);
    p.gain = vec(pp::kGain);
    p.bloom = num(pp::kBloom);
    p.bloomRadius = num(pp::kBloomRadius);
    p.vignette = num(pp::kVignette);
    p.grain = num(pp::kGrain);
    p.chromaticAberration = num(pp::kChromaticAberration);
    p.distortion = num(pp::kDistortion);
    p.sharpen = num(pp::kSharpen);
    p.lutIntensity = num(pp::kLutIntensity);
    if (const std::string lut = n.as<std::string>(a + pp::kLut); !lut.empty())
        p.lut = render::loadCubeLut(lut);
    render::FrameState::Fog& fog = s.fog;
    fog.on = n.as<bool>(a + pp::kFog) && num(pp::kFogDensity) > 0.0f;
    fog.color = vec(pp::kFogColor);
    fog.density = num(pp::kFogDensity) * 0.01f;
    fog.height = num(pp::kFogHeight);
    fog.falloff = num(pp::kFogFalloff) * 0.01f;
    fog.start = num(pp::kFogStart);
    fog.sunScatter = num(pp::kFogSunScatter);
    fog.anisotropy = std::clamp(num(pp::kFogAnisotropy), 0.0f, 0.95f);
}

bool defaultMaterial()
{
    return PrefStore::get().value<bool>("general", kDefaultMaterial, false);
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
