#include "tex/PainterImport.h"

#include "bake/Naming.h"
#include "brush/Alphas.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "ops/Command.h"
#include "tex/Layers.h"
#include "tex/Ports.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <optional>
#include <set>

namespace lila::tex::painter
{
namespace fs = std::filesystem;

namespace
{
constexpr uint64_t kAllChannels = ~0ull;

// Painter's channel bits (its engine's channel order) and blending mode numbers (BlendingMode_* in its shader API).
enum : int
{
    BitBaseColor = 0,
    BitHeight = 1,
    BitOpacity = 3,
    BitEmissive = 4,
    BitGlossiness = 6,
    BitRoughness = 7,
    BitMetallic = 13,
    BitNormal = 22,
    BitAo = 23,
};

const char* channelName(int bit)
{
    static const char* names[] = {"base color", "height", "specular", "opacity", "emissive", "displacement", "glossiness",
                                  "roughness", "anisotropy level", "anisotropy angle", "transmissive", "reflection", "IOR",
                                  "metallic", "user 0", "user 1", "user 2", "user 3", "user 4", "user 5", "user 6", "user 7",
                                  "normal", "ambient occlusion", "diffuse", "specular level", "blending mask", "translucency",
                                  "scattering", "scatter color", "sheen opacity", "sheen roughness", "sheen color", "coat opacity",
                                  "coat color", "coat roughness", "coat specular level", "coat normal"};
    return bit >= 0 && bit < int(std::size(names)) ? names[bit] : "unknown";
}

/** The Lila channel a Painter channel bit writes; empty when Lila has none. Glossiness writes roughness inverted. */
std::string lilaChannel(int bit)
{
    switch (bit)
    {
    case BitBaseColor: return "baseColor";
    case BitHeight: return "height";
    case BitOpacity: return "opacity";
    case BitEmissive: return "emissive";
    case BitGlossiness:
    case BitRoughness: return "roughness";
    case BitMetallic: return "metallic";
    case BitNormal: return "normal";
    case BitAo: return "ao";
    default: return {};
    }
}

bool colorChannel(const std::string& c)
{
    return c == "baseColor" || c == "emissive" || c == "normal";
}

std::vector<int> bits(uint64_t mask)
{
    std::vector<int> out;
    for (int i = 0; i < 64; ++i)
        if (mask >> i & 1)
            out.push_back(i);
    return out;
}

const char* modeName(int m)
{
    static const char* names[] = {"Disable",    "Replace",     "Normal",     "Darken",       "Multiply",    "Color Burn",  "Linear Burn",
                                  "Lighten",    "Screen",      "Color Dodge", "Linear Dodge", "Overlay",     "Soft Light",  "Hard Light",
                                  "Vivid Light", "Linear Light", "Pin Light",  "Difference",   "Exclusion",   "Tint",        "Saturation",
                                  "Color",      "Value",       "Divide",     "Inverse Divide", "Passthrough", "Normal Map Combine",
                                  "Normal Map Detail", "Normal Map Inverse Detail", "Subtract", "Inverse Subtract", "Signed Addition"};
    return m >= 0 && m < int(std::size(names)) ? names[m] : "unknown";
}

std::string str(const json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

double num(const json& j, const char* key, double fallback)
{
    auto it = j.find(key);
    if (it == j.end())
        return fallback;
    if (it->is_number())
        return it->get<double>();
    if (it->is_boolean())
        return it->get<bool>() ? 1.0 : 0.0;
    return fallback;
}

uint64_t mask64(const json& j, const char* key)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_number())
        return 0;
    return it->is_number_unsigned() ? it->get<uint64_t>() : uint64_t(it->get<int64_t>());
}

const json& items(const json& j, const char* key)
{
    static const json empty = json::array();
    auto it = j.find(key);
    if (it == j.end() || !it->is_object())
        return empty;
    auto i = it->find("items");
    return i != it->end() && i->is_array() ? *i : empty;
}

/** "/mg_curvature/mg_curvature?version=..." -> "mg_curvature". */
std::string resourceId(const std::string& url)
{
    std::string s = url.substr(0, url.find('?'));
    if (!s.empty() && s.front() == '/')
        s.erase(0, 1);
    return s.substr(0, s.find('/'));
}

/** A procedural's parameter values by identifier. */
struct Tweaks
{
    std::map<std::string, json> v;

    explicit Tweaks(const json& procedural)
    {
        auto it = procedural.find("tweaks");
        if (it != procedural.end() && it->is_array())
            for (const json& t : *it)
                if (t.is_object())
                    v[str(t, "identifier")] = t.value("value", json());
    }
    double get(const std::string& k, double fallback) const
    {
        auto it = v.find(k);
        if (it == v.end())
            return fallback;
        if (it->second.is_number())
            return it->second.get<double>();
        if (it->second.is_boolean())
            return it->second.get<bool>() ? 1.0 : 0.0;
        return fallback;
    }
    bool flag(const std::string& k) const { return get(k, 0.0) != 0.0; }
};

/** A Lila node standing in for a Painter procedural. */
struct NodeSpec
{
    std::string type;
    json values = json::object();
    bool invert = false;
    /** Comes across only approximately (Painter's graph and Lila's node differ). */
    bool approx = true;
};

int clampInt(double v, int lo, int hi)
{
    return std::clamp(int(std::lround(v)), lo, hi);
}

double clamp01(double v)
{
    return std::clamp(v, 0.0, 1.0);
}

/** Noises and patterns, by Painter resource name. scale multiplies the pattern's tiling. */
std::optional<NodeSpec> textureSpec(const std::string& id, const Tweaks& t, double scale)
{
    auto has = [&](const char* s) { return id.find(s) != std::string::npos; };
    const int seed = clampInt(t.get("$randomseed", 0.0), 0, 1 << 30);
    auto tiles = [&](double fallback) { return clampInt(t.get("scale", t.get("Scale", fallback)) * scale, 1, 64); };
    NodeSpec s;
    s.invert = t.flag("invert") || t.flag("Invert");
    if (has("perlin"))
        s = {"perlin", {{"scale", tiles(4)}, {"seed", seed}}, s.invert};
    else if (has("clouds"))
        s = {"clouds", {{"scale", tiles(3)}, {"seed", seed}}, s.invert};
    else if (has("cells"))
        s = {"cells", {{"scale", tiles(6)}, {"seed", seed}}, s.invert};
    else if (has("crystal"))
        s = {"crystal", {{"scale", tiles(5)}, {"seed", seed}}, s.invert};
    else if (has("worley"))
        s = {"worley", {{"scale", tiles(4)}, {"seed", seed}}, s.invert};
    else if (has("weave") || has("fabric"))
        s = {"weave", json::object(), s.invert};
    else if (has("tile_generator") || has("tiles"))
        s = {"tiles", {{"seed", seed}}, s.invert};
    else if (has("wood") || has("fibers") || has("stripes") || has("brush_wave"))
        s = {"directional", {{"angle", 90.0}, {"seed", seed}}, s.invert};
    else if (has("creases") || has("scratch"))
        s = {"scratches", {{"seed", seed}}, s.invert};
    else if (has("grunge") || has("spots") || has("dirt") || has("rust") || has("leaks") || has("concrete") || has("marble") ||
             has("noise") || has("mesh_"))
        s = {"dirt", {{"scale", tiles(4)}, {"seed", seed}}, s.invert};
    else
        return std::nullopt;
    return s;
}

/** The output a stand-in node's value comes from: its mask, else out, else its first. */
std::string firstOutput(const std::string& type)
{
    const NodePorts ports = portsOf(Graph{}, GNode{"probe", type, {}, json::object()});
    if (ports.outputs.empty())
        return "out";
    for (const char* id : {"mask", "out"})
        if (ports.output(id))
            return id;
    return ports.outputs.front().id;
}

/** Painter's balance (0.5 neutral, higher shows more) as a maskRemap-style level around base. */
double levelFrom(double balance, double base, double range)
{
    return clamp01(base + (0.5 - balance) * range);
}

double softnessFrom(double contrast, double base)
{
    return std::max(0.01, base * (1.0 - clamp01(contrast)));
}

/** Painter's mesh-map generators (mg_*) and the light generator, as Lila generators. */
std::optional<NodeSpec> generatorSpec(const std::string& id, const Tweaks& t)
{
    const bool globalInvert = t.flag("global_invert");
    const double gb = t.get("global_balance", 0.5), gc = t.get("global_contrast", 0.0);
    auto curvature = [&](double balance, double contrast, bool invert) {
        return NodeSpec{"genEdgeWear", {{"breakup", 0.0}, {"level", levelFrom(balance, 0.3, 0.6)}, {"softness", softnessFrom(contrast, 0.25)}}, invert};
    };
    auto ao = [&](double balance, double contrast, bool invert) {
        return NodeSpec{"genAoDarken", {{"level", levelFrom(balance, 0.4, 0.8)}, {"softness", softnessFrom(contrast, 0.8)}}, invert};
    };
    auto position = [&](bool invert) {
        const char* axis = t.get("position_direction_rightleft_opacity", 0.0) > t.get("position_direction_topbottom_opacity", 1.0) ? "x"
                           : t.get("position_direction_frontback_opacity", 0.0) > t.get("position_direction_topbottom_opacity", 1.0) ? "z"
                                                                                                                                     : "y";
        return NodeSpec{"genPositionGradient", {{"axis", axis}, {"invert", invert}}, false};
    };
    if (id == "mg_curvature")
        return curvature(gb, gc, globalInvert != t.flag("curvature_invert"));
    if (id == "mg_ambient_occlusion")
        return ao(gb, gc, globalInvert != t.flag("ao_invert"));
    if (id == "mg_position")
        return position(globalInvert != t.flag("position_invert"));
    if (id == "mg_dirt")
        return NodeSpec{"genDirt", {{"level", levelFrom(t.get("dirt_level", 0.5), 0.35, 0.6)}, {"softness", softnessFrom(t.get("dirt_contrast", 0.0), 0.4)}}, t.flag("invert")};
    if (id.starts_with("mg_") && (id.find("edge") != std::string::npos || id.find("damage") != std::string::npos))
        return NodeSpec{"genEdgeWear", json::object(), t.flag("invert")};
    if (id == "mg_dust")
        return NodeSpec{"genDust", json::object(), t.flag("invert")};
    if (id == "mg_drips" || id == "mg_leaks")
        return NodeSpec{"genDrips", json::object(), t.flag("invert")};
    if (id == "mg_scratches")
        return NodeSpec{"genScratches", json::object(), t.flag("invert")};
    if (id == "mg_thickness")
        return NodeSpec{"genThickness", json::object(), t.flag("invert")};
    if (id == "light" || id == "mg_light")
    {
        const double azimuth = t.get("Horizontal_Angle", 0.0) * 360.0;
        const double elevation = std::clamp(-t.get("Vertical_Angle", -0.25) * 180.0, 0.0, 90.0);
        return NodeSpec{"genSunBleach", {{"azimuth", azimuth}, {"elevation", elevation}}, t.flag("invert")};
    }
    if (id == "mg_mask_editor" || id == "mg_mask_builder")
    {
        // The strongest part stands in for the whole mix.
        struct Part
        {
            const char* key;
            double opacity;
        };
        const Part parts[] = {{"curvature", t.get("curvature_opacity", 0.0)}, {"ao", t.get("ao_opacity", 0.0)},
                              {"ws", t.get("ws_opacity", 0.0)},               {"position", t.get("position_opacity", 0.0)},
                              {"texture", t.get("texture_opacity", 0.0)}};
        const Part* best = std::max_element(std::begin(parts), std::end(parts), [](const Part& a, const Part& b) { return a.opacity < b.opacity; });
        const std::string k = best->opacity > 0.0 ? best->key : "curvature";
        const bool inv = globalInvert != t.flag(k + "_invert");
        if (k == "curvature")
            return curvature(t.get("curvature_balance", gb), std::max(gc, t.get("curvature_contrast", 0.0)), inv);
        if (k == "ao")
            return ao(t.get("ao_balance", gb), std::max(gc, t.get("ao_contrast", 0.0)), inv);
        if (k == "ws")
            return NodeSpec{"genDust", json::object(), inv};
        if (k == "position")
            return position(inv);
        return NodeSpec{"dirt", json::object(), inv};
    }
    return std::nullopt;
}

/** Filters, by Painter resource name. Empty type: Painter's filter changes nothing Lila would see. */
std::optional<NodeSpec> filterSpec(const std::string& id, const Tweaks& t)
{
    const double intensity = t.get("intensity", 1.0);
    if (id == "blur" || id == "fx_blur")
        return NodeSpec{"blur", {{"radius", std::clamp(intensity * 0.004, 0.0005, 0.2)}}};
    if (id == "blur_slope")
        return NodeSpec{"slopeBlur", {{"intensity", std::clamp(intensity * 0.01 / std::max(1.0, t.get("intensity_divider", 1.0)), 0.0, 0.2)}}};
    if (id == "blur_directional")
        return NodeSpec{"directionalBlur", {{"angle", t.get("mblurangle", 0.0) * 360.0}, {"length", std::clamp(intensity * 0.01, 0.0, 0.3)}}};
    if (id == "warp")
        return NodeSpec{"warp", {{"intensity", intensity / std::max(1.0, t.get("intensity_divider", 1.0))}}};
    if (id == "sharpen" || id == "fx_sharpen")
        return NodeSpec{"sharpen", {{"amount", std::clamp(intensity, 0.0, 4.0)}}};
    if (id == "invert")
        return t.flag("invert") ? std::optional<NodeSpec>(NodeSpec{"invert", json::object(), false, false}) : std::optional<NodeSpec>(NodeSpec{});
    if (id == "contrast_luminosity")
    {
        const double c = std::clamp(t.get("Contrast", 0.0), -1.0, 1.0) * 0.5, l = std::clamp(t.get("Luminosity", 0.0), -1.0, 1.0) * 0.5;
        return NodeSpec{"levels", {{"inLow", std::max(0.0, c)}, {"inHigh", 1.0 - std::max(0.0, c)}, {"outLow", clamp01(std::max(0.0, -c) + l)}, {"outHigh", clamp01(1.0 - std::max(0.0, -c) + l)}}};
    }
    if (id == "height_adjust")
    {
        const double off = t.get("offset", 0.0) * 0.5, mul = t.get("height_multiplier", 1.0);
        return NodeSpec{"levels", {{"outLow", clamp01(0.5 - 0.5 * mul + off)}, {"outHigh", clamp01(0.5 + 0.5 * mul + off)}}};
    }
    if (id == "advanced_tri-planar" || id == "tri-planar" || id == "triplanar")
        return NodeSpec{};
    return std::nullopt;
}

class Converter
{
public:
    Converter(const SpsmFile& file, fs::path imageDir, Conversion& out) : mFile(file), mImageDir(std::move(imageDir)), mOut(out) {}

    Layer root(const json& preset, const std::string& name)
    {
        const json* group = preset.contains("layerGroup") ? &preset["layerGroup"] : nullptr;
        if (!group || !group->is_object())
            throw std::runtime_error("the preset has no layers");
        Layer l = layer(*group);
        l.name = name;
        return l;
    }

    /** A Painter smart mask's effects as a Lila mask stack. */
    std::vector<Mask> smartMask(const json& preset)
    {
        std::vector<const json*> effects;
        flatten(items(preset, "actionsStack"), effects);
        std::vector<Mask> out;
        for (const json* e : effects)
            if (std::optional<Mask> m = mask(*e))
                out.push_back(std::move(*m));
        if (out.empty())
            throw std::runtime_error("none of the smart mask's effects can come across");
        return out;
    }

private:
    void approx(const std::string& what) { ++mOut.approximated[what]; }
    void drop(const std::string& what) { ++mOut.dropped[what]; }

    std::string newId(char prefix) { return std::string(1, prefix) + std::to_string(++mNext); }

    /** Painter's per-channel blending as Lila channel settings. */
    std::map<std::string, ChannelBlend> blends(const json& pcb)
    {
        std::map<std::string, ChannelBlend> out;
        if (!pcb.is_array())
            return out;
        for (const json& b : pcb)
        {
            if (!b.is_object())
                continue;
            const int mode = int(num(b, "blendingMode", 2));
            const double opacity = clamp01(num(b, "opacity", 1.0));
            for (int bit : bits(mask64(b, "channelTypes")))
            {
                const std::string ch = lilaChannel(bit);
                if (ch.empty() || (bit == BitGlossiness && out.contains("roughness")))
                    continue;
                ChannelBlend cb;
                cb.opacity = opacity;
                cb.enabled = mode != 0;
                cb.mode = lilaMode(mode, ch);
                out[ch] = cb;
            }
        }
        return out;
    }

    /** "" keeps the channel's default (rnm for normals, addSigned for heights). */
    std::string lilaMode(int mode, const std::string& ch)
    {
        if (ch == "normal")
        {
            if (mode == 2 || mode == 1)
                return "normal";
            if (mode != 26 && mode != 27 && mode != 28 && mode != 25 && mode != 0)
                approx(std::string("normal channel blending ") + modeName(mode) + " as RNM");
            return {};
        }
        if (ch == "height")
        {
            switch (mode)
            {
            case 10:
            case 31: return {};
            case 1:
            case 2:
            case 25: return "normal";
            case 29: return "subtract";
            default: break;
            }
        }
        switch (mode)
        {
        case 0:
        case 1:
        case 2: return "normal";
        case 25: return "normal";
        case 3: return "darken";
        case 4: return "multiply";
        case 5: return "colorBurn";
        case 6: return "linearBurn";
        case 7: return "lighten";
        case 8: return "screen";
        case 9: return "colorDodge";
        case 10: return "add";
        case 11: return "overlay";
        case 12: return "softLight";
        case 13: return "hardLight";
        case 14: return "vividLight";
        case 15: return "linearLight";
        case 16: return "pinLight";
        case 17: return "difference";
        case 18: return "exclusion";
        case 20: return "saturation";
        case 21: return "color";
        case 22: return "value";
        case 23: return "divide";
        case 29: return "subtract";
        case 31: return "addSigned";
        case 19: approx("Tint blending as Color"); return "color";
        case 24: approx("Inverse Divide blending as Divide"); return "divide";
        case 30: approx("Inverse Subtract blending as Subtract"); return "subtract";
        default: approx(std::string(modeName(mode)) + " blending as Normal"); return "normal";
        }
    }

    std::string maskMode(int mode)
    {
        switch (mode)
        {
        case 1:
        case 2:
        case 25: return "normal";
        case 3: return "darken";
        case 4: return "multiply";
        case 7: return "lighten";
        case 8: return "screen";
        case 10: return "add";
        case 11: return "overlay";
        case 17: return "difference";
        case 29: return "subtract";
        default: approx(std::string("mask blending ") + modeName(mode) + " as Normal"); return "normal";
        }
    }

    /** The effects of a stack, groups opened up, disabled ones and empty groups left out. */
    void flatten(const json& list, std::vector<const json*>& out)
    {
        for (const json& a : list)
        {
            if (!a.is_object())
                continue;
            if (str(a, "@") == "DataActionGroup")
            {
                if (a.value("enabled", true))
                    flatten(items(a, "subStack"), out);
                continue;
            }
            out.push_back(&a);
        }
    }

    Layer layer(const json& l)
    {
        const std::string cls = str(l, "@");
        Layer out;
        out.name = str(l, "label");
        out.enabled = l.value("enabled", true);
        if (cls == "DataLayerGroup")
        {
            out.kind = LayerKind::Folder;
            out.id = newId('L');
            for (const json& c : items(l, "subStack"))
                if (c.is_object())
                    out.children.push_back(layer(c));
            groupBlend(out, blends(l.value("perChannelBlending", json())));
        }
        else
        {
            std::vector<const json*> effects;
            flatten(items(l, "actions"), effects);
            std::vector<Layer> parts;
            for (const json* e : effects)
                if (std::optional<Layer> p = content(*e, out.name))
                    parts.push_back(std::move(*p));
            const auto lb = blends(l.value("perChannelBlending", json()));
            if (parts.size() == 1 && parts[0].kind != LayerKind::Filter)
            {
                const std::string name = out.name;
                const bool enabled = out.enabled;
                out = std::move(parts[0]);
                out.name = name;
                out.enabled = enabled;
                combine(out, lb);
            }
            else
            {
                out.kind = LayerKind::Folder;
                out.id = newId('L');
                for (Layer& p : parts)
                    combine(p, lb);
                out.children = std::move(parts);
            }
        }
        masks(l, out);
        return out;
    }

    /** The channels a fill or procedural layer writes; null for kinds that take every channel. */
    static std::optional<std::set<std::string>> writes(const Layer& l)
    {
        std::set<std::string> out;
        if (l.kind == LayerKind::Fill)
            for (const auto& [ch, v] : l.material.items())
                out.insert(ch);
        else if (l.kind == LayerKind::Procedural && l.graph.is_object())
            for (const json& o : l.graph.value("outputs", json::array()))
                out.insert(o.value("name", ""));
        else
            return std::nullopt;
        return out;
    }

    /** Channel settings only for channels the layer writes: a setting turns a channel on, which would write a default. */
    static void prune(Layer& l)
    {
        if (const auto w = writes(l))
            std::erase_if(l.channels, [&](const auto& kv) { return !w->contains(kv.first); });
    }

    /** A Painter layer's blending over its content's: the layer's mode where the content's is normal, opacities multiplied. */
    static void combine(Layer& part, const std::map<std::string, ChannelBlend>& lb)
    {
        const auto w = writes(part);
        for (const auto& [ch, b] : lb)
        {
            if (w && !w->contains(ch))
                continue;
            auto it = part.channels.find(ch);
            if (it == part.channels.end())
            {
                part.channels[ch] = b;
                continue;
            }
            it->second.opacity *= b.opacity;
            it->second.enabled = it->second.enabled && b.enabled;
            if (it->second.mode.empty() || it->second.mode == "normal")
                it->second.mode = b.mode;
        }
    }

    /** A Painter group's blending: Lila folders have no channel settings, so it goes to the children, an even opacity to a mask. */
    void groupBlend(Layer& folder, const std::map<std::string, ChannelBlend>& gb)
    {
        std::set<double> opacities;
        bool modes = false;
        for (const auto& [ch, b] : gb)
        {
            if (b.enabled)
                opacities.insert(b.opacity);
            modes = modes || (!b.mode.empty() && b.mode != "normal");
        }
        if (modes)
        {
            approx("group blending modes, given to the group's layers");
            for (Layer& c : folder.children)
                combine(c, gb);
            return;
        }
        if (opacities.size() == 1 && *opacities.begin() < 1.0)
        {
            Mask m;
            m.id = newId('M');
            m.kind = MaskKind::Fill;
            m.name = "Opacity";
            m.value = *opacities.begin();
            m.mode = "multiply";
            folder.masks.push_back(m);
        }
        else if (opacities.size() > 1)
            approx("group opacity that differs by channel");
    }

    std::optional<Layer> content(const json& e, const std::string& layerName)
    {
        const std::string cls = str(e, "@");
        if (!e.value("enabled", true))
            return std::nullopt;
        if (cls == "DataActionFill")
            return fill(e, layerName);
        if (cls == "DataActionFilterProcedural")
        {
            const json& proc = e.value("procedural", json::object());
            const std::string id = resourceId(str(proc, "urlToSbsRes"));
            std::optional<NodeSpec> f = filterSpec(id, Tweaks(proc));
            if (!f)
            {
                drop("filter " + id);
                return std::nullopt;
            }
            if (f->type.empty())
            {
                approx("filter " + id + " (left out: it only changes projection)");
                return std::nullopt;
            }
            if (f->approx)
                approx("filter " + id + " as " + f->type);
            return filterLayer(e, f->type, f->values);
        }
        if (cls == "DataActionFilterLevels")
            return filterLayer(e, "levels", levelsValues(e.value("levels", json::object())));
        if (cls == "DataActionPaint")
            drop("painted strokes");
        else if (cls == "DataActionAnchor")
            drop("anchor points");
        else
            drop(std::string("effect ") + (cls.starts_with("DataAction") ? cls.substr(10) : cls));
        return std::nullopt;
    }

    Layer filterLayer(const json& e, const std::string& type, const json& values)
    {
        Layer l;
        l.id = newId('L');
        l.kind = LayerKind::Filter;
        l.name = str(e, "label").empty() ? type : str(e, "label");
        l.filter = type;
        l.filterValues = values;
        const uint64_t on = mask64(e, "channelTypes");
        if (on != kAllChannels && on != 0)
        {
            std::set<std::string> chs;
            for (int b : bits(on))
                if (std::string c = lilaChannel(b); !c.empty())
                    chs.insert(c);
            for (const char* c : {"baseColor", "roughness", "metallic", "normal", "height", "ao", "emissive", "opacity"})
                if (!chs.contains(c))
                    l.channels[c].enabled = false;
        }
        return l;
    }

    static json levelsValues(const json& lv)
    {
        auto first = [&](const char* k, double fallback) {
            auto it = lv.find(k);
            return it != lv.end() && it->is_array() && !it->empty() && (*it)[0].is_number() ? (*it)[0].get<double>() : fallback;
        };
        return {{"inLow", first("inputMinimum", 0.0)}, {"inHigh", first("inputMaximum", 1.0)}, {"gamma", first("gamma", 1.0)},
                {"outLow", first("outputMinimum", 0.0)}, {"outHigh", first("outputMaximum", 1.0)}};
    }

    double tiling(const json& e)
    {
        const json& t = e.value("uvTransformation", json::object());
        auto it = t.is_object() ? t.find("scaleFactors") : t.end();
        if (t.is_object() && it != t.end() && it->is_array() && !it->empty() && (*it)[0].is_number())
            return std::max(0.01, (*it)[0].get<double>());
        return 1.0;
    }

    /** The image file an embedded bitmap resource lands in; empty (and reported) when the file doesn't embed it. */
    std::string image(const std::string& url)
    {
        const auto v = url.find("version=");
        const std::string key = v == std::string::npos ? std::string() : url.substr(v + 8);
        const std::string label = resourceId(url);
        auto it = key.empty() ? mFile.resources.end() : mFile.resources.find(key);
        if (it == mFile.resources.end())
        {
            drop("image " + label + " (not in the file)");
            return {};
        }
        if (auto done = mImages.find(key); done != mImages.end())
            return done->second;
        try
        {
            image::Image img = decodeImage(it->second);
            const fs::path path = mImageDir / (bake::fileSafe(label) + "_" + key.substr(0, 8) + ".png");
            mOut.images[path] = std::move(img);
            return mImages[key] = paths::toUtf8(path);
        }
        catch (const std::exception& e)
        {
            drop("image " + label + " (" + e.what() + ")");
            return mImages[key] = std::string();
        }
    }

    /** A fill effect as a fill layer, or a procedural layer when any channel isn't a plain value. */
    std::optional<Layer> fill(const json& e, const std::string& layerName)
    {
        const uint64_t active = e.contains("channelTypes") ? mask64(e, "channelTypes") : kAllChannels;
        const double scale = tiling(e);
        json material = json::object();
        struct Source
        {
            std::string channel;
            std::string nodeType;
            json values;
            std::string output = "out";
            bool invertGloss = false;
            bool invert = false;
        };
        std::vector<Source> nodes;
        std::set<std::string> explicitRough;
        const json& sources = e.value("sources", json::array());
        for (const json& s : sources)
            if (s.is_object())
                for (int b : bits(mask64(s, "channelTypes")))
                    if (b == BitRoughness)
                        explicitRough.insert("roughness");

        for (const json& s : sources)
        {
            if (!s.is_object())
                continue;
            const std::string cls = str(s, "@");
            const std::vector<int> written = bits(mask64(s, "channelTypes") & active);
            // A source on nearly every channel is a blanket assignment, not data meant for each of them.
            const bool blanket = written.size() > 20;
            for (int b : written)
            {
                const std::string ch = lilaChannel(b);
                if (ch.empty())
                {
                    if (!blanket && mDroppedChannels.insert(b).second)
                        drop(std::string("channel ") + channelName(b));
                    continue;
                }
                const bool gloss = b == BitGlossiness;
                if (gloss && explicitRough.contains("roughness"))
                    continue;
                if (cls == "DataSourceUniform")
                {
                    const json& c = s.value("color", json::array({0.0, 0.0, 0.0}));
                    auto at = [&](size_t i) { return c.is_array() && c.size() > i && c[i].is_number() ? c[i].get<double>() : 0.0; };
                    if (ch == "normal")
                    {
                        if (std::abs(at(0) - 0.5) > 0.01 || std::abs(at(1) - 0.5) > 0.01 || at(2) < 0.99)
                            material[ch] = json::array({at(0), at(1), at(2), 1.0});
                    }
                    else if (colorChannel(ch))
                        material[ch] = json::array({at(0), at(1), at(2), 1.0});
                    else if (ch == "height")
                        material[ch] = clamp01(0.5 + 0.5 * at(0));
                    else
                        material[ch] = gloss ? 1.0 - at(0) : at(0);
                }
                else if (cls == "DataSourceBitmap")
                {
                    const std::string url = str(s.value("bitmap", json::object()), "urlToBitmapRes");
                    if (url.starts_with("resourcealias:"))
                    {
                        const std::string alias = url.substr(14);
                        static const std::map<std::string, std::string> mesh = {{"Curvature", "meshCurvature"}, {"AmbientOcclusion", "meshAo"},
                                                                                {"Thickness", "meshThickness"}, {"Height", "meshHeight"}};
                        auto m = mesh.find(alias);
                        if (m == mesh.end())
                            drop("mesh map " + alias + " as a channel value");
                        else
                            nodes.push_back({ch, m->second, json::object(), "out", gloss});
                        continue;
                    }
                    const std::string file = image(url);
                    if (!file.empty())
                        nodes.push_back({ch, "image", {{"file", file}, {"tiles", clampInt(scale, 1, 64)}}, "out", gloss});
                }
                else if (cls == "DataSourceProcedural")
                {
                    const json& proc = s.value("procedural", json::object());
                    const std::string id = resourceId(str(proc, "urlToSbsRes"));
                    const Tweaks t(proc);
                    std::optional<NodeSpec> spec = textureSpec(id, t, scale);
                    if (!spec)
                        spec = generatorSpec(id, t);
                    if (!spec)
                    {
                        drop("Substance graph " + id + " (in " + channelName(b) + ")");
                        continue;
                    }
                    approx(id + " as " + spec->type);
                    nodes.push_back({ch, spec->type, spec->values, firstOutput(spec->type), gloss, spec->invert});
                }
                else
                    drop("fill source " + cls);
            }
        }
        if (material.empty() && nodes.empty())
            return std::nullopt;

        Layer l;
        l.id = newId('L');
        l.name = str(e, "label").empty() ? layerName : str(e, "label");
        l.channels = blends(e.value("perChannelBlending", json()));
        if (nodes.empty())
        {
            l.kind = LayerKind::Fill;
            l.material = material;
            prune(l);
            return l;
        }

        // Procedural: a constant per plain value, the stand-in nodes for the rest.
        l.kind = LayerKind::Procedural;
        json g = {{"name", l.id + "_content"}, {"inputs", json::array()}, {"outputs", json::array()}, {"links", json::array()}};
        json gn = json::array({{{"name", "GroupInput"}, {"type", "GroupInput"}, {"position", {-600, 0}}},
                               {{"name", "GroupOutput"}, {"type", "GroupOutput"}, {"position", {400, 0}}}});
        double y = 0;
        std::set<std::string> outs;
        auto output = [&](const std::string& ch, const std::string& node, const std::string& socket) {
            if (!outs.insert(ch).second)
                return;
            g["outputs"].push_back({{"name", ch}, {"type", colorChannel(ch) ? "color" : "gray"}});
            g["links"].push_back({node, socket, "GroupOutput", ch});
        };
        for (const Source& s : nodes)
        {
            std::string node = "N_" + s.channel;
            gn.push_back({{"name", node}, {"type", s.nodeType}, {"position", {-300, y}}, {"values", s.values}});
            std::string socket = s.output;
            if (s.invertGloss != s.invert)
            {
                gn.push_back({{"name", node + "_inv"}, {"type", "invert"}, {"position", {-100, y}}, {"values", json::object()}});
                g["links"].push_back({node, socket, node + "_inv", "input"});
                node += "_inv";
                socket = "out";
            }
            output(s.channel, node, socket);
            y += 140;
        }
        for (const auto& [ch, v] : material.items())
        {
            if (outs.contains(ch))
                continue;
            const std::string node = "C_" + ch;
            json values = v.is_array() ? json{{"color", v}} : json{{"value", v}};
            gn.push_back({{"name", node}, {"type", v.is_array() ? "color" : "constant"}, {"position", {-300, y}}, {"values", values}});
            output(ch, node, "out");
            y += 140;
        }
        g["nodes"] = gn;
        l.graph = g;
        prune(l);
        return l;
    }

    void masks(const json& l, Layer& out)
    {
        if (!l.value("maskEnabled", true))
            return;
        std::vector<const json*> effects;
        flatten(items(l, "maskActions"), effects);
        if (effects.empty())
            return;
        std::vector<Mask> ms;
        for (const json* e : effects)
            if (std::optional<Mask> m = mask(*e))
                ms.push_back(std::move(*m));
        const bool white = num(l, "maskInitial", 1.0) != 0.0;
        // Lila starts a mask stack on black, Painter on its initial colour; a stack that converted to nothing keeps that colour.
        if (white || ms.empty())
        {
            Mask base;
            base.id = newId('M');
            base.kind = MaskKind::Fill;
            base.value = white ? 1.0 : 0.0;
            ms.insert(ms.begin(), base);
        }
        for (Mask& m : ms)
            out.masks.push_back(std::move(m));
    }

    std::optional<Mask> mask(const json& e)
    {
        const std::string cls = str(e, "@");
        Mask m;
        m.id = newId('M');
        m.enabled = e.value("enabled", true);
        m.name = str(e, "label");
        if (const json& pcb = e.value("perChannelBlending", json()); pcb.is_array() && !pcb.empty() && pcb[0].is_object())
        {
            m.mode = maskMode(int(num(pcb[0], "blendingMode", 2)));
            m.opacity = clamp01(num(pcb[0], "opacity", 1.0));
            if (num(pcb[0], "blendingMode", 2) == 0)
                m.enabled = false;
        }
        if (cls == "DataActionFill")
        {
            const json& sources = e.value("sources", json::array());
            const json* s = nullptr;
            for (const json& c : sources)
                if (c.is_object())
                {
                    s = &c;
                    break;
                }
            if (!s)
                return std::nullopt;
            const std::string sc = str(*s, "@");
            if (sc == "DataSourceUniform")
            {
                m.kind = MaskKind::Fill;
                const json& c = s->value("color", json::array({1.0}));
                m.value = c.is_array() && !c.empty() && c[0].is_number() ? c[0].get<double>() : 1.0;
                return m;
            }
            if (sc == "DataSourceBitmap")
            {
                const std::string url = str(s->value("bitmap", json::object()), "urlToBitmapRes");
                if (url.starts_with("resourcealias:"))
                {
                    static const std::map<std::string, std::string> mesh = {{"Curvature", "meshCurvature"}, {"AmbientOcclusion", "meshAo"},
                                                                            {"Thickness", "meshThickness"}, {"Height", "meshHeight"}};
                    auto it = mesh.find(url.substr(14));
                    if (it == mesh.end())
                    {
                        drop("mesh map " + url.substr(14) + " as a mask");
                        return std::nullopt;
                    }
                    m.kind = MaskKind::Generator;
                    m.type = it->second;
                    return m;
                }
                const std::string file = image(url);
                if (file.empty())
                    return std::nullopt;
                m.kind = MaskKind::Paint;
                m.image = file;
                return m;
            }
            if (sc == "DataSourceProcedural")
            {
                const json& proc = s->value("procedural", json::object());
                const std::string id = resourceId(str(proc, "urlToSbsRes"));
                const Tweaks t(proc);
                if (id == "shape")
                {
                    // Painter's default shape (a full square) is how its fill masks start.
                    m.kind = MaskKind::Fill;
                    m.value = 1.0;
                    return m;
                }
                std::optional<NodeSpec> spec = generatorSpec(id, t);
                if (!spec)
                    spec = textureSpec(id, t, tiling(e));
                if (!spec)
                {
                    drop("Substance graph " + id + " (in a mask)");
                    return std::nullopt;
                }
                if (spec->approx)
                    approx(id + " as " + spec->type);
                m.kind = MaskKind::Generator;
                m.type = spec->type;
                m.values = spec->values;
                m.invert = spec->invert;
                return m;
            }
            drop("mask source " + sc);
            return std::nullopt;
        }
        if (cls == "DataActionFilterProcedural")
        {
            const json& proc = e.value("procedural", json::object());
            const std::string id = resourceId(str(proc, "urlToSbsRes"));
            std::optional<NodeSpec> f = filterSpec(id, Tweaks(proc));
            if (!f)
            {
                drop("mask filter " + id);
                return std::nullopt;
            }
            if (f->type.empty())
                return std::nullopt;
            if (f->approx)
                approx("mask filter " + id + " as " + f->type);
            if (f->type == "levels")
            {
                m.kind = MaskKind::Levels;
                m.values = f->values;
                return m;
            }
            m.kind = MaskKind::Filter;
            m.type = f->type;
            m.values = f->values;
            m.mode = "normal";
            return m;
        }
        if (cls == "DataActionFilterLevels")
        {
            m.kind = MaskKind::Levels;
            m.values = levelsValues(e.value("levels", json::object()));
            m.mode = "normal";
            return m;
        }
        if (cls == "DataActionPaint")
            drop("painted strokes (in a mask)");
        else if (cls == "DataActionAnchor")
            drop("anchor points");
        else
            drop(std::string("mask effect ") + (cls.starts_with("DataAction") ? cls.substr(10) : cls));
        return std::nullopt;
    }

    const SpsmFile& mFile;
    fs::path mImageDir;
    Conversion& mOut;
    int mNext = 0;
    std::map<std::string, std::string> mImages;
    std::set<int> mDroppedChannels;
};

/** Tags guessed from a material's name, plus imported. */
std::vector<std::string> tagsFromName(const std::string& name)
{
    std::string n = name;
    std::ranges::transform(n, n.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
    static const std::vector<std::pair<const char*, std::vector<const char*>>> words = {
        {"metal", {"metal", "iron", "steel", "gold", "bronze", "copper", "brass", "silver", "chrome", "armor", "armour"}},
        {"wood", {"wood", "walnut", "maple", "mahogany", "oak", "pine", "plank", "bark"}},
        {"stone", {"stone", "rock", "cliff", "marble", "granite", "cobble"}},
        {"brick", {"brick"}},
        {"concrete", {"concrete", "cement", "plaster"}},
        {"ground", {"dirt", "mud", "soil", "ground", "gravel"}},
        {"sand", {"sand"}},
        {"snow", {"snow", "ice", "frost"}},
        {"foliage", {"moss", "grass", "leaf", "leaves"}},
        {"fabric", {"cloth", "cotton", "denim", "wool", "silk", "polyester", "plaid", "kevlar", "camo", "fabric", "linen", "velvet"}},
        {"leather", {"leather", "snakeskin", "crocodile", "suede"}},
        {"skin", {"skin", "flesh"}},
        {"plastic", {"plastic"}},
        {"rubber", {"rubber"}},
        {"crystal", {"crystal", "gem"}},
        {"paint", {"paint", "painted"}},
        {"stylized", {"stylized", "stylised", "handpainted", "toon", "cartoon"}},
    };
    // Whole words (and their plurals) only, so "painter" isn't paint.
    std::vector<std::string> nameWords;
    std::string w;
    for (char c : n + " ")
    {
        if (std::isalnum(static_cast<unsigned char>(c)))
            w.push_back(c);
        else if (!w.empty())
        {
            nameWords.push_back(w);
            w.clear();
        }
    }
    auto has = [&](const std::string& k) {
        return std::ranges::any_of(nameWords, [&](const std::string& x) { return x == k || x == k + "s" || x == k + "es"; });
    };
    std::vector<std::string> out = {"imported"};
    for (const auto& [tag, keys] : words)
        if (std::ranges::any_of(keys, [&](const char* k) { return has(k); }))
            out.push_back(tag);
    return out;
}

json countsJson(const std::map<std::string, int>& m)
{
    json a = json::array();
    std::vector<std::pair<std::string, int>> v(m.begin(), m.end());
    std::ranges::stable_sort(v, [](const auto& x, const auto& y) { return x.second > y.second; });
    for (const auto& [what, n] : v)
        a.push_back(n > 1 ? what + " x" + std::to_string(n) : what);
    return a;
}

std::string lowerExt(const fs::path& p)
{
    std::string ext = p.extension().string();
    std::ranges::transform(ext, ext.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
    return ext;
}

/** The words of a Painter alpha's resource name ("alpha_dirt_06" -> dirt), for finding a library alpha like it. */
std::vector<std::string> alphaWords(const std::string& id)
{
    std::vector<std::string> out;
    std::string w;
    auto flush = [&] {
        static const std::set<std::string> filler = {"alpha", "brush", "shape", "special", "maker", "offset"};
        if (w.size() > 2 && !filler.contains(w))
            out.push_back(w);
        w.clear();
    };
    for (const char c : id)
    {
        if (std::isupper(static_cast<unsigned char>(c)) && !w.empty())
            flush();
        if (std::isalpha(static_cast<unsigned char>(c)))
            w.push_back(char(std::tolower(static_cast<unsigned char>(c))));
        else
            flush();
    }
    flush();
    return out;
}

/** A brush preset from Painter's stamp settings; its tip becomes the library alpha sharing a word with Painter's, else round. */
brushes::Preset brushPreset(const json& preset, const std::string& name, Conversion& out)
{
    const json& paint = preset.value("preset", json::object());
    const json& b = paint.value("brush", json::object());
    const json& em = b.value("stampsEmitter", json::object());
    constexpr double kDeg = 57.29577951308232;
    json brush = {{"flow", clamp01(num(b, "flow", 1.0))},
                  // Painter spaces dabs by the tip's diameter, Lila by its radius.
                  {"spacing", std::clamp(num(em, "spacing", 0.1) * 2.0, 0.01, 4.0)},
                  {"jitter",
                   {{"position", std::clamp(num(em, "positionJitter", 0.0) * 2.0, 0.0, 4.0)},
                    {"size", clamp01(num(b, "brushSizeJitter", 0.0))},
                    {"opacity", clamp01(num(b, "flowJitter", 0.0))},
                    {"angle", std::clamp(num(em, "rotationJitter", 0.0) * kDeg, 0.0, 180.0)}}},
                  {"angle", -num(em, "rotation", 0.0) * kDeg},
                  {"followStroke", em.value("followPath", false)},
                  {"pressureFlow", em.value("pressureFlowEnabled", false)},
                  {"pressureSize", em.value("pressureSizeEnabled", false)}};
    std::string source;
    for (const json& s : paint.value("sourceTransparent", json::array()))
        if (s.is_object())
        {
            source = s.contains("procedural") ? resourceId(str(s["procedural"], "urlToSbsRes"))
                                              : resourceId(str(s.value("bitmap", json::object()), "urlToBitmapRes"));
            break;
        }
    const std::vector<std::string> words = alphaWords(source);
    if (!source.empty() && std::ranges::find(words, "round") == words.end())
    {
        std::vector<const brush::alphas::Entry*> matches;
        const std::vector<brush::alphas::Entry> lib = brush::alphas::library();
        // Painter's words first, then what Lila's library calls the same thing.
        static const std::map<std::string, std::vector<std::string>> kin = {
            {"dirt", {"grunge", "grime", "stain", "speck", "noise"}}, {"ground", {"mud", "dirt", "crack", "gravel"}},
            {"tire", {"tread", "track", "grip"}},                    {"hatched", {"hatch", "stripe", "line"}},
            {"inky", {"ink", "splat", "blot"}},                      {"cracked", {"crack"}},
            {"wall", {"plaster", "concrete", "stone"}},              {"wood", {"grain", "bark"}}};
        std::vector<std::string> search = words;
        for (const std::string& w : words)
            if (auto it = kin.find(w); it != kin.end())
                search.insert(search.end(), it->second.begin(), it->second.end());
        for (const std::string& w : search)
        {
            for (const brush::alphas::Entry& e : lib)
            {
                std::string hay = e.name + " " + e.category;
                std::ranges::transform(hay, hay.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
                if (hay.find(w) != std::string::npos)
                    matches.push_back(&e);
            }
            if (!matches.empty())
                break;
        }
        if (matches.empty())
            ++out.dropped["brush tip " + source + " (no library alpha like it, so round)"];
        else
        {
            const brush::alphas::Entry* pick = matches[std::hash<std::string>{}(name) % matches.size()];
            brush["alpha"] = pick->ref;
            ++out.approximated["brush tip " + source + " as alpha " + pick->name];
        }
    }
    brushes::Preset p;
    p.id = "Imported/" + bake::fileSafe(name);
    p.name = name;
    p.category = "Imported";
    p.doc = "Converted from Substance 3D Painter.";
    p.brush = brush;
    p.payload = {{"kind", "paint"}};
    return p;
}

bool isPainterExt(const std::string& ext)
{
    return ext == ".spsm" || ext == ".spmsk" || ext == ".sppr";
}

/** The Painter files a path names: itself, or every one under a folder (brush presets sit in category subfolders). */
std::vector<fs::path> painterFiles(const fs::path& p)
{
    std::vector<fs::path> out;
    std::error_code ec;
    if (fs::is_directory(p, ec))
    {
        for (const auto& e : fs::recursive_directory_iterator(p, ec))
            if (e.is_regular_file(ec) && isPainterExt(lowerExt(e.path())))
                out.push_back(e.path());
        std::ranges::sort(out);
    }
    else
        out.push_back(p);
    return out;
}
} // namespace

Conversion convertSpsm(const SpsmFile& file, const std::string& name, const fs::path& imageDir)
{
    Conversion out;
    Converter c(file, imageDir, out);
    // Painter keeps a preset in several format versions under one uuid: the newest of each stands.
    std::map<std::string, size_t> newest;
    std::vector<size_t> order;
    for (size_t i = 0; i < file.presets.size(); ++i)
    {
        const json& p = file.presets[i];
        const std::string uuid = p.is_object() ? str(p, "uuid") : std::string();
        const std::string key = uuid.empty() ? "#" + std::to_string(i) : uuid;
        auto [it, fresh] = newest.try_emplace(key, i);
        if (fresh)
            order.push_back(i);
        else if (num(p, "preset_dataversion", 0) > num(file.presets[it->second], "preset_dataversion", 0))
        {
            std::ranges::replace(order, it->second, i);
            it->second = i;
        }
    }
    std::set<std::string> used;
    for (size_t i : order)
    {
        std::string n = name;
        for (int k = 2; !used.insert(n).second; ++k)
            n = name + " " + std::to_string(k);
        const std::string cls = str(file.presets[i], "@");
        if (cls == "DataToolPreset")
        {
            out.brushes.push_back(brushPreset(file.presets[i], n, out));
            continue;
        }
        SmartMaterial m;
        m.name = n;
        m.description = "Converted from Substance 3D Painter.";
        m.tags = normalizeTags(tagsFromName(n));
        if (cls == "DataSmartMask")
        {
            m.kind = "mask";
            m.masks = json::array();
            for (const Mask& mk : c.smartMask(file.presets[i]))
                m.masks.push_back(mk.toJson());
            out.materials.push_back(std::move(m));
            continue;
        }
        m.kind = "material";
        m.layer = c.root(file.presets[i], n).toJson();
        m.description = "Converted from Substance 3D Painter.";
        m.tags = normalizeTags(tagsFromName(n));
        out.materials.push_back(std::move(m));
    }
    return out;
}

bool isPainterFile(const fs::path& file)
{
    return isPainterExt(lowerExt(file));
}

void registerPainterCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "texImportPainterSmartMaterials",
        .doc = "Convert Substance 3D Painter smart materials (.spsm), smart masks (.spmsk) and brush presets (.sppr), or folders of them: "
               "smart materials and masks as .lilasmart beside their embedded images, brush presets into Imported in your brushes. "
               "Painter's generators, noises and filters become their nearest Lila nodes and brush tips the nearest library alpha; "
               "custom Substance graphs and painted strokes can't come across. Returns per file {file, materials, brushes, approximated, "
               "dropped, error}.",
        .positionalDoc = "file|folder [file|folder...]",
        .flags = {{"folder", "", T::String, nullptr, "Where they go; your smart materials folder by default."},
                  {"brushFolder", "", T::String, nullptr, "Where brush presets go; Imported in your brushes folder by default."},
                  {"overwrite", "", T::Bool, true, "Replace ones of the same name there."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty())
                throw CommandError("texImportPainterSmartMaterials: give at least one Painter file or folder");
            const fs::path dest = a.has("folder") ? paths::fromUtf8(a.get<std::string>("folder")) : PrefStore::get().userDir() / "smartmaterials";
            const bool overwrite = a.get<bool>("overwrite", true);
            std::vector<fs::path> files;
            for (const json& p : a.positional)
            {
                if (!p.is_string())
                    throw CommandError("texImportPainterSmartMaterials: paths are strings");
                for (fs::path& f : painterFiles(paths::fromUtf8(p.get<std::string>())))
                    files.push_back(std::move(f));
            }
            if (files.empty())
                throw CommandError("texImportPainterSmartMaterials: no .spsm, .spmsk or .sppr files there");
            const fs::path brushDest = a.has("brushFolder") ? paths::fromUtf8(a.get<std::string>("brushFolder")) : brushes::tierDir("user") / "Imported";
            bool anyBrush = false;
            std::error_code ec;
            fs::create_directories(dest, ec);
            json out = json::array();
            for (const fs::path& f : files)
            {
                json r = {{"file", paths::toUtf8(f)}, {"materials", json::array()}, {"brushes", json::array()}};
                try
                {
                    const std::string name = paths::toUtf8(f.stem());
                    const std::string stem = bake::fileSafe(name);
                    Conversion c = convertSpsm(readSpsm(f), name, dest / (stem + ".images"));
                    std::vector<fs::path> targets;
                    for (size_t i = 0; i < c.materials.size(); ++i)
                    {
                        const fs::path target = dest / (bake::fileSafe(c.materials[i].name) + ".lilasmart");
                        if (!overwrite && fs::exists(target, ec))
                            throw std::runtime_error(paths::toUtf8(target.filename()) + " already exists");
                        targets.push_back(target);
                    }
                    for (const auto& [path, img] : c.images)
                    {
                        fs::create_directories(path.parent_path(), ec);
                        image::write(path, img, 8);
                    }
                    for (size_t i = 0; i < c.materials.size(); ++i)
                    {
                        saveSmartMaterial(c.materials[i], targets[i]);
                        r["materials"].push_back({{"name", c.materials[i].name}, {"kind", c.materials[i].kind}, {"file", paths::toUtf8(targets[i])}});
                    }
                    for (const brushes::Preset& p : c.brushes)
                    {
                        const fs::path target = brushDest / (bake::fileSafe(p.name) + ".lilabrush");
                        if (!overwrite && fs::exists(target, ec))
                            throw std::runtime_error(paths::toUtf8(target.filename()) + " already exists");
                        fs::create_directories(brushDest, ec);
                        std::ofstream o(target, std::ios::binary);
                        if (!o)
                            throw std::runtime_error("can't write " + paths::toUtf8(target));
                        o << p.toJson().dump(2) << '\n';
                        r["brushes"].push_back({{"name", p.name}, {"file", paths::toUtf8(target)}});
                        anyBrush = true;
                    }
                    r["approximated"] = countsJson(c.approximated);
                    r["dropped"] = countsJson(c.dropped);
                }
                catch (const std::exception& e)
                {
                    r["error"] = e.what();
                }
                out.push_back(std::move(r));
            }
            if (anyBrush)
                brushes::reload();
            return out;
        },
    });
}
} // namespace lila::tex::painter
