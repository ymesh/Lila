#include "scene/PostProcess.h"

#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/SceneIndex.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace lila::scene::postProcess
{
namespace
{
constexpr uint32_t kStored = AttrInput | AttrStorable;

AttrDef number(std::string name, std::string shortName, double def, std::string doc, std::optional<double> lo = {},
               std::optional<double> hi = {})
{
    return {.name = std::move(name), .shortName = std::move(shortName), .type = PlugType::Float, .defaultValue = def,
            .flags = kStored | AttrKeyable, .doc = std::move(doc), .minValue = lo, .maxValue = hi};
}

AttrDef color(std::string name, std::string shortName, double def, std::string doc)
{
    AttrDef a{.name = name, .shortName = shortName, .type = PlugType::Vec3, .defaultValue = glm::dvec3(def),
              .flags = kStored | AttrKeyable | AttrColor, .doc = std::move(doc), .minValue = 0.0};
    for (int i = 0; i < 3; ++i)
    {
        a.childNames[i] = name + "RGB"[i];
        a.childShort[i] = shortName + "rgb"[i];
    }
    return a;
}

std::vector<AttrDef> attrs()
{
    std::vector<AttrDef> a(kCount);
    a[kEnabled] = {.name = "enabled", .shortName = "en", .type = PlugType::Bool, .defaultValue = true, .flags = kStored,
                   .doc = "Use this post process. The first enabled one in the scene applies."};
    a[kTonemapper] = {.name = "tonemapper", .shortName = "tm", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = kStored,
                      .enumNames = {"neutral", "agx", "aces", "linear"},
                      .doc = "Neutral keeps base colours true (the viewport's own); AgX rolls bright colours off to white like film; "
                             "ACES matches Unreal's filmic look; linear clips."};
    a[kAgxLook] = {.name = "agxLook", .shortName = "agl", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = kStored,
                   .enumNames = {"none", "punchy", "golden"}, .doc = "AgX only: a look on top, more contrast and saturation, or warm."};
    a[kExposure] = number("exposure", "ex", 0.0, "Stops on top of the viewport's exposure.", -16.0, 16.0);
    a[kAutoExposure] = {.name = "autoExposure", .shortName = "ae", .type = PlugType::Bool, .defaultValue = false, .flags = kStored,
                        .doc = "Meter the picture each frame and expose its average to middle grey, easing toward it like an eye."};
    a[kExposureCompensation] = number("exposureCompensation", "ec", 0.0, "Auto exposure: stops brighter or darker than metered.", -10.0, 10.0);
    a[kExposureMin] = number("exposureMin", "emn", -6.0, "Auto exposure: the most it may darken, stops.", -16.0, 16.0);
    a[kExposureMax] = number("exposureMax", "emx", 6.0, "Auto exposure: the most it may brighten, stops.", -16.0, 16.0);
    a[kExposureSpeed] = number("exposureSpeed", "es", 3.0, "Auto exposure: how fast it adapts, per second; 0 jumps at once.", 0.0, 100.0);
    a[kTemperature] = number("temperature", "wt", 6500.0, "White balance: the light colour that shows as white, kelvin.", 1500.0, 15000.0);
    a[kTint] = number("tint", "wtn", 0.0, "White balance: green (negative) against magenta (positive).", -1.0, 1.0);
    a[kContrast] = number("contrast", "ct", 1.0, "Around middle grey; 1 leaves it.", 0.0, 4.0);
    a[kSaturation] = number("saturation", "sat", 1.0, "0 is grey, 1 leaves it.", 0.0, 4.0);
    a[kLift] = color("lift", "lf", 0.0, "Raises the shadows of the graded picture, per channel.");
    a[kLift].minValue = -1.0;
    a[kGamma] = color("gamma", "gm", 1.0, "Brightens (above 1) or darkens the mid-tones, per channel.");
    a[kGain] = color("gain", "gn", 1.0, "Scales the highlights, per channel.");
    a[kOffset] = color("offset", "ofs", 0.0, "Adds to every level of the graded picture, per channel.");
    a[kOffset].minValue = -1.0;
    a[kLiftMaster] = number("liftMaster", "lfm", 0.0, "Raises (or lowers) the shadows of every channel together.", -1.0, 1.0);
    a[kGammaMaster] = number("gammaMaster", "gmm", 1.0, "Brightens (above 1) or darkens the mid-tones of every channel together.", 0.1, 4.0);
    a[kGainMaster] = number("gainMaster", "gnm", 1.0, "Scales the highlights of every channel together.", 0.0, 4.0);
    a[kOffsetMaster] = number("offsetMaster", "ofm", 0.0, "Adds to every level of every channel together.", -1.0, 1.0);
    a[kShadows] = color("shadows", "shd", 1.0, "Tonal balance: a colour gain on the shadows, below shadowsRange.");
    a[kMidtones] = color("midtones", "mid", 1.0, "Tonal balance: a colour gain on the mid-tones, between the ranges.");
    a[kHighlights] = color("highlights", "hil", 1.0, "Tonal balance: a colour gain on the highlights, above highlightsRange.");
    a[kShadowsRange] = number("shadowsRange", "shr", 0.33, "Tonal balance: luminance where shadows end and mid-tones take over, 0 to 1.", 0.0, 1.0);
    a[kHighlightsRange] = number("highlightsRange", "hir", 0.66, "Tonal balance: luminance where highlights begin, 0 to 1.", 0.0, 1.0);
    const char* curves[][3] = {
        {"curveMaster", "cvm", "Tone curve on every channel: points x y from 0 to 1."},
        {"curveRed", "cvr", "Tone curve on red, after the master curve."},
        {"curveGreen", "cvg", "Tone curve on green, after the master curve."},
        {"curveBlue", "cvb", "Tone curve on blue, after the master curve."},
        {"curveHueVsSat", "chs", "Saturation by hue: 0.5 leaves it, 0 greys, 1 doubles."},
        {"curveHueVsHue", "chh", "Hue shift by hue: 0.5 leaves it, 0 and 1 turn half way round."},
        {"curveLumVsSat", "cls", "Saturation by luminance: 0.5 leaves it, 0 greys, 1 doubles."},
    };
    for (int i = 0; i < 7; ++i)
        a[kCurveMaster + i] = {.name = curves[i][0], .shortName = curves[i][1], .type = PlugType::String, .defaultValue = std::string(),
                               .flags = kStored | AttrCurve, .doc = curves[i][2]};
    a[kBloom] = number("bloom", "bl", 0.0, "Glow around bright light, 0 to 1. A few hundredths is subtle.", 0.0, 1.0);
    a[kBloomRadius] = number("bloomRadius", "blr", 1.0, "Spread of the glow.", 0.1, 4.0);
    a[kVignette] = number("vignette", "vg", 0.0, "Darkens toward the corners, 0 to 1.", 0.0, 1.0);
    a[kGrain] = number("grain", "gr", 0.0, "Film grain, 0 to 1.", 0.0, 1.0);
    a[kChromaticAberration] = number("chromaticAberration", "ca", 0.0, "Colour fringes toward the corners, pixels.", 0.0, 40.0);
    a[kDistortion] = number("distortion", "ds", 0.0, "Lens distortion: negative bulges (barrel), positive pinches.", -1.0, 1.0);
    a[kSharpen] = number("sharpen", "sh", 0.0, "Crisper detail, 0 to 2.", 0.0, 2.0);
    a[kLut] = {.name = "lut", .shortName = "lut", .type = PlugType::String, .defaultValue = std::string(), .flags = kStored | AttrFile,
               .doc = "A colour lookup table (.cube) applied to the finished picture."};
    a[kLutIntensity] = number("lutIntensity", "li", 1.0, "How much of the LUT, 0 to 1.", 0.0, 1.0);
    a[kFog] = {.name = "fog", .shortName = "fg", .type = PlugType::Bool, .defaultValue = false, .flags = kStored,
               .doc = "Height fog: thicker low down, lit by the sun (the first directional light)."};
    a[kFogColor] = color("fogColor", "fgc", 0.0, "Fog: linear colour of its own glow, scaled by the environment intensity.");
    a[kFogColor].defaultValue = glm::dvec3(0.6, 0.7, 0.8);
    a[kFogDensity] = number("fogDensity", "fgd", 0.02, "Fog: thickness at its height, per metre.", 0.0, 10.0);
    a[kFogHeight] = number("fogHeight", "fgh", 0.0, "Fog: height of the given density, cm; it thins above.");
    a[kFogFalloff] = number("fogFalloff", "fgf", 0.2, "Fog: how fast it thins going up, per metre.", 0.0, 100.0);
    a[kFogStart] = number("fogStart", "fgs", 0.0, "Fog: none nearer the camera than this, cm.", 0.0);
    a[kFogSunScatter] = number("fogSunScatter", "fgss", 0.5, "Fog: how much the sun lights it.", 0.0, 10.0);
    a[kFogAnisotropy] = number("fogAnisotropy", "fga", 0.6, "Fog: how much brighter it glows toward the sun, 0 to 0.95.", 0.0, 0.95);
    a[kLensFlare] = number("lensFlare", "lfl", 0.0, "Lens flare from bright parts of the picture: ghosts, halo and starburst, 0 to 1. 0 is off.", 0.0, 1.0);
    a[kFlareThreshold] = number("flareThreshold", "flt", 1.5, "Lens flare: how bright light must be to flare, in the exposed picture (1 is white).", 0.0, 100.0);
    a[kFlareGhosts] = number("flareGhosts", "flg", 4.0, "Lens flare: ghost images of bright light mirrored through the centre, 0 to 8.", 0.0, 8.0);
    a[kFlareGhostSpacing] = number("flareGhostSpacing", "fgsp", 0.35, "Lens flare: how far apart the ghosts sit along the line through the centre.", 0.05, 1.0);
    a[kFlareChroma] = number("flareChroma", "flc", 0.015, "Lens flare: colour fringing of ghosts and halo.", 0.0, 0.1);
    a[kFlareHalo] = number("flareHalo", "flh", 0.3, "Lens flare: a ring of light round the centre, 0 to 1.", 0.0, 1.0);
    a[kFlareHaloRadius] = number("flareHaloRadius", "flhr", 0.45, "Lens flare: the halo's radius, as a share of the picture.", 0.05, 1.0);
    a[kFlareStarburst] = number("flareStarburst", "fls", 0.4, "Lens flare: star rays from bright points, one pair per aperture blade (the camera's), 0 to 1.", 0.0, 1.0);
    a[kFlareStreakLength] = number("flareStreakLength", "flsl", 0.2, "Lens flare: length of the star rays, as a share of the picture.", 0.01, 1.0);
    a[kAnamorphicStreaks] = number("anamorphicStreaks", "ans", 0.0, "Long horizontal streaks from bright light, as anamorphic lenses draw, 0 to 1.", 0.0, 1.0);
    a[kAnamorphicTint] = color("anamorphicTint", "ant", 1.0, "Colour of the anamorphic streaks.");
    a[kAnamorphicTint].defaultValue = glm::dvec3(0.45, 0.65, 1.0);
    a[kLensDirt] = {.name = "lensDirt", .shortName = "ldt", .type = PlugType::String, .defaultValue = std::string(), .flags = kStored | AttrFile | AttrImage,
                    .doc = "Smudges on the lens lit by bloom and flare: an image, or a built-in (dust, smudge, streaks, rain)."};
    a[kLensDirtIntensity] = number("lensDirtIntensity", "ldi", 0.5, "How much the lens dirt lights up, 0 to 4.", 0.0, 4.0);
    a[kHalation] = number("halation", "hal", 0.0, "Film halation: a warm glow bleeding round bright edges, 0 to 1.", 0.0, 1.0);
    a[kHalationThreshold] = number("halationThreshold", "halt", 1.0, "Halation: how bright light must be to bleed, in the exposed picture.", 0.0, 50.0);
    a[kHalationTint] = color("halationTint", "halc", 1.0, "Halation: the glow's colour; film's is a deep red-orange.");
    a[kHalationTint].defaultValue = glm::dvec3(1.0, 0.25, 0.08);
    a[kGrainSize] = number("grainSize", "grs", 1.5, "Film grain: size of a grain, pixels.", 0.5, 8.0);
    a[kGrainColor] = number("grainColor", "grc", 0.3, "Film grain: 0 is the same in every channel (mono), 1 differs per channel.", 0.0, 1.0);
    a[kGrainResponse] = number("grainResponse", "grr", 0.7, "Film grain: 0 even everywhere, 1 strongest in the mid-tones and faint in shadows and highlights.", 0.0, 1.0);
    a[kLetterboxAspect] = number("letterboxAspect", "lba", 0.0, "Bars matting the picture to this width over height (2.39 is scope); 0 is none. Renders include them.", 0.0, 10.0);
    a[kLetterboxOpacity] = number("letterboxOpacity", "lbo", 1.0, "How dark the letterbox bars are, 0 to 1.", 0.0, 1.0);
    a[kVolume] = {.name = "volume", .shortName = "vol", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = kStored,
                  .enumNames = {"global", "box", "sphere"},
                  .doc = "Global applies everywhere; box and sphere only while the camera is inside (fading over volumeBlend), "
                         "over the global one."};
    a[kVolumeCenter] = color("volumeCenter", "vcn", 0.0, "Volume: the centre, cm.");
    a[kVolumeCenter].flags &= ~uint32_t(AttrColor);
    a[kVolumeCenter].minValue.reset();
    a[kVolumeSize] = color("volumeSize", "vsz", 500.0, "Volume: a box's full size, cm; a sphere's radius in x.");
    a[kVolumeSize].flags &= ~uint32_t(AttrColor);
    for (int attr : {int(kVolumeCenter), int(kVolumeSize)})
        for (int i = 0; i < 3; ++i)
        {
            a[attr].childNames[i] = a[attr].name + "XYZ"[i];
            a[attr].childShort[i] = a[attr].shortName + "xyz"[i];
        }
    a[kVolumeBlend] = number("volumeBlend", "vbl", 100.0, "Volume: distance outside it over which it fades out, cm.", 0.0);
    a[kVolumePriority] = number("volumePriority", "vpr", 0.0, "Volume: where volumes overlap, the higher priority applies over the lower.");
    return a;
}

/** Linear sRGB of a blackbody at `kelvin`, scaled to a peak of 1 (Tanner Helland's fit). */
glm::dvec3 blackbody(double kelvin)
{
    const double t = std::clamp(kelvin, 1000.0, 40000.0) / 100.0;
    double r = t <= 66.0 ? 255.0 : 329.698727446 * std::pow(t - 60.0, -0.1332047592);
    double g = t <= 66.0 ? 99.4708025861 * std::log(t) - 161.1195681661 : 288.1221695283 * std::pow(t - 60.0, -0.0755148492);
    double b = t >= 66.0 ? 255.0 : t <= 19.0 ? 0.0 : 138.5177312231 * std::log(t - 10.0) - 305.0447927307;
    auto lin = [](double c) {
        c = std::clamp(c / 255.0, 0.0, 1.0);
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    glm::dvec3 rgb(lin(r), lin(g), lin(b));
    return rgb / std::max({rgb.r, rgb.g, rgb.b, 1e-6});
}
} // namespace

const std::vector<std::string>& lensPresetNames()
{
    static const std::vector<std::string> names = {"Spherical Prime", "Anamorphic Scope", "Vintage Glass", "Clean Coated"};
    return names;
}

int firstAttr()
{
    const NodeType* t = types::find("postProcess");
    return t ? int(t->attrs.size()) - kCount : 0;
}

NodeId active(const Graph& g)
{
    const int first = firstAttr();
    // Volumes apply over this one where the camera is; the global one is the first enabled that is no volume.
    for (const Node* n : sceneIndex::ofType(g, "postProcess"))
        if (n->as<bool>(first + kEnabled) && n->as<int64_t>(first + kVolume) == 0)
            return n->id();
    return kNoNode;
}

double volumeWeight(const Node& post, const glm::dvec3& eye)
{
    const int first = firstAttr();
    const int64_t mode = post.as<int64_t>(first + kVolume);
    if (mode == 0)
        return 1.0;
    const glm::dvec3 c = post.as<glm::dvec3>(first + kVolumeCenter), size = post.as<glm::dvec3>(first + kVolumeSize);
    double outside = 0.0;
    if (mode == 1)
    {
        const glm::dvec3 d = glm::max(glm::abs(eye - c) - glm::abs(size) * 0.5, glm::dvec3(0.0));
        outside = glm::length(d);
    }
    else
        outside = std::max(glm::length(eye - c) - std::abs(size.x), 0.0);
    const double blend = std::max(post.as<double>(first + kVolumeBlend), 0.0);
    if (outside <= 0.0)
        return 1.0;
    return blend > 0.0 ? std::clamp(1.0 - outside / blend, 0.0, 1.0) : 0.0;
}

glm::dvec3 whiteBalance(double kelvin, double tint)
{
    // Gains that map the chosen light to the white of 6500 K, keeping luminance.
    const glm::dvec3 light = blackbody(kelvin), white = blackbody(6500.0);
    glm::dvec3 gains = white / glm::max(light, glm::dvec3(1e-6));
    gains.g *= 1.0 - 0.25 * std::clamp(tint, -1.0, 1.0);
    const double luma = glm::dot(gains * light, glm::dvec3(0.2126, 0.7152, 0.0722)) / glm::dot(white, glm::dvec3(0.2126, 0.7152, 0.0722));
    return gains / std::max(luma, 1e-6);
}

void registerTypes()
{
    if (types::find("postProcess"))
        return;
    NodeType t;
    t.name = "postProcess";
    t.icon = "adjustments-horizontal";
    t.doc = "How the scene's light becomes the picture: tonemapper, exposure, white balance, grading, bloom, lens effects and a LUT. "
            "The viewport and renders use the first enabled one.";
    t.ownAttrs = attrs();
    t.create = [] { return std::make_unique<Node>(); };
    types::registerType(std::move(t));
}

void registerCommands()
{
    using T = ArgType;
    if (ops::find("postProcess"))
        return;
    ops::registerCommand({
        .name = "lensPreset",
        .doc = "Set a post process's lens flare, streaks and dirt to a lens type: Spherical Prime (round glass, a few ghosts and a "
               "star), Anamorphic Scope (long blue streaks, oval ghosts), Vintage Glass (warm ghosts, strong halo, dust), Clean Coated "
               "(faint ghosts, no streaks). With no name, lists them.",
        .positionalDoc = "preset",
        .flags = {{"node", "n", T::String, nullptr, "The post process; the active one (created if needed) when left out."},
                  {"camera", "cam", T::String, nullptr, "Also set this camera's anamorphicSqueeze and apertureBlades to match the lens."}},
        .run = [](const Args& a) -> json {
            json names = lensPresetNames();
            if (a.positional.empty())
                return names;
            const std::string preset = a.positional[0].get<std::string>();
            struct Lens
            {
                double flare, threshold, ghosts, spacing, chroma, halo, haloRadius, starburst, streak, anamorphic;
                glm::dvec3 tint;
                const char* dirt;
                double dirtIntensity, squeeze;
                int blades;
            };
            static const std::map<std::string, Lens> lenses = {
                {"Spherical Prime", {0.4, 1.5, 4, 0.35, 0.015, 0.25, 0.45, 0.4, 0.2, 0.0, {0.45, 0.65, 1.0}, "", 0.5, 1.0, 6}},
                {"Anamorphic Scope", {0.35, 1.2, 3, 0.45, 0.02, 0.15, 0.5, 0.0, 0.2, 0.8, {0.35, 0.6, 1.0}, "streaks", 0.4, 2.0, 0}},
                {"Vintage Glass", {0.6, 1.0, 6, 0.3, 0.035, 0.6, 0.4, 0.25, 0.15, 0.0, {1.0, 0.75, 0.5}, "dust", 0.8, 1.0, 5}},
                {"Clean Coated", {0.15, 2.5, 2, 0.4, 0.005, 0.05, 0.45, 0.15, 0.12, 0.0, {0.45, 0.65, 1.0}, "", 0.0, 1.0, 9}},
            };
            auto it = lenses.find(preset);
            if (it == lenses.end())
                throw CommandError("lensPreset: no lens named " + preset);
            const Lens& l = it->second;
            Graph& g = Graph::current();
            std::string node = a.get<std::string>("node", "");
            if (node.empty())
            {
                const NodeId id = active(g);
                node = id != kNoNode ? g.node(id)->name()
                                     : ops::execute("createNode", {{"args", {"postProcess"}}, {"name", "postProcess"}}).get<std::string>();
            }
            auto set = [&](const std::string& plug, json value) { ops::execute("setAttr", {{"args", {plug, value}}}); };
            set(node + ".lensFlare", l.flare);
            set(node + ".flareThreshold", l.threshold);
            set(node + ".flareGhosts", l.ghosts);
            set(node + ".flareGhostSpacing", l.spacing);
            set(node + ".flareChroma", l.chroma);
            set(node + ".flareHalo", l.halo);
            set(node + ".flareHaloRadius", l.haloRadius);
            set(node + ".flareStarburst", l.starburst);
            set(node + ".flareStreakLength", l.streak);
            set(node + ".anamorphicStreaks", l.anamorphic);
            set(node + ".anamorphicTint", json::array({l.tint.r, l.tint.g, l.tint.b}));
            set(node + ".lensDirt", std::string(l.dirt));
            set(node + ".lensDirtIntensity", l.dirtIntensity);
            if (const std::string cam = a.get<std::string>("camera", ""); !cam.empty())
            {
                set(cam + ".anamorphicSqueeze", l.squeeze);
                set(cam + ".apertureBlades", l.blades);
            }
            return names;
        },
    });
    ops::registerCommand({
        .name = "postProcess",
        .doc = "Create a post process node, or with no new one needed, return the active one. Returns its name.",
        .flags = {
            {"name", "n", T::String, nullptr, "Name; made unique."},
            {"existing", "e", T::Bool, false, "Return the active post process if there is one instead of creating another."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.get<bool>("existing", false))
                if (NodeId id = active(g); id != kNoNode)
                    return g.node(id)->name();
            return ops::execute("createNode", {{"args", {"postProcess"}}, {"name", a.get<std::string>("name", "postProcess")}});
        },
    });
}
} // namespace lila::scene::postProcess
