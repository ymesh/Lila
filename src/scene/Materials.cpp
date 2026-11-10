#include "scene/Materials.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Profile.h"
#include "core/Paths.h"
#include "geo/Mesh.h"
#include "scene/AttrBuilders.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <iterator>
#include <map>
#include <set>

namespace lila::scene
{
using namespace attrdefs;

namespace
{
using namespace material;

AttrDef colorAttr(std::string name, glm::dvec3 def, std::string doc)
{
    AttrDef a{.name = name, .type = PlugType::Vec3, .defaultValue = def, .flags = AttrInput | AttrStorable | AttrKeyable, .doc = std::move(doc),
              .minValue = 0.0};
    a.childNames = {name + "R", name + "G", name + "B"};
    return a;
}

AttrDef pathAttr(std::string name, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrFile | AttrImage,
            .doc = std::move(doc)};
}

/** Painterly, Toon and Foliage inputs; each model reads only its own and DefaultLit none of them. */
void stylisedAttrs(std::vector<AttrDef>& a)
{
    a[kShadingModel] = enumAttr("shadingModel", {"defaultLit", "painterly", "toon", "foliage", "unlit", "skin", "cloth", "clearcoat", "hair", "eye"},
                                "Lighting model: defaultLit (Unreal's default lit), painterly (brush strokes and a colour palette), toon "
                                "(stepped ramp with tinted light and shadow), foliage (soft gradient shading, lit through), unlit (the "
                                "base colour as it is, plus emissive, with no lighting), skin (light scattering under the surface), cloth "
                                "(velvet sheen), clearcoat (a glossy layer on top), hair (stretched strand highlights, for hair cards) or "
                                "eye (an iris under a glossy cornea; UVs centred on the pupil).");

    a[kPainterlyStrength] = floatAttr("painterlyStrength", 1.0, "Painterly: scales the whole effect; 0 shades exactly like defaultLit.", 0.0);
    a[kStrokeNormal] = floatAttr("strokeNormal", 0.6, "Painterly: how far the stroke sheet bends the shading normal; 0 is the plain surface.", 0.0, 1.0);
    a[kStrokeScale] = floatAttr("strokeScale", 20.0, "Painterly: size of one stroke sheet tile, cm (UV mode: tiles per UV unit is 100 / this).", 1e-3);
    a[kStrokeLayerRatio] = floatAttr("strokeLayerRatio", 2.7, "Painterly: the second, finer stroke layer is this many times smaller.", 1e-3);
    a[kStrokeMapping] = enumAttr("strokeMapping", {"object", "world", "screen", "uv"},
                                 "Painterly: project strokes triplanar in object or world space, at a constant size on screen, or along the UVs.");
    a[kStrokeMap] = pathAttr("strokeMap", "Painterly: tileable stroke sheet (RG normal tilt, B stroke shade, A per-stroke random). Empty uses the "
                                          "built-in sheet; a built-in's name (strokes_soft, strokes_bristle, strokes_dabs) picks another.");
    a[kPaletteCount] = {.name = "paletteCount", .type = PlugType::Int, .defaultValue = int64_t(3), .flags = AttrInput | AttrStorable,
                        .doc = "Painterly: colours in the palette, 2 to 4: base and shadow, then light, then accent.", .minValue = 2.0, .maxValue = 4.0};
    a[kPaletteShadow] = colorAttr("paletteShadow", glm::dvec3(0.10, 0.12, 0.25), "Painterly: linear colour strokes take on in shadow.");
    a[kPaletteLight] = colorAttr("paletteLight", glm::dvec3(0.95, 0.80, 0.55), "Painterly: linear colour strokes take on in light; tints the specular.");
    a[kPaletteAccent] = colorAttr("paletteAccent", glm::dvec3(0.60, 0.30, 0.35), "Painterly: linear colour a few scattered strokes pick up.");
    a[kPaletteContrast] = floatAttr("paletteContrast", 0.5, "Painterly: 0 blends the palette smoothly across the light, 1 steps it.", 0.0, 1.0);
    a[kPaletteSpread] = floatAttr("paletteSpread", 0.35, "Painterly: how far individual strokes push the palette boundary around.", 0.0);
    a[kSpecularTint] = floatAttr("specularTint", 0.5, "Painterly: tints the specular toward paletteLight.", 0.0, 1.0);
    a[kBoil] = boolAttr("boil", false, "Painterly: shift the strokes a few times a second, like hand-drawn frames. Follows the time slider.");
    a[kBoilFps] = floatAttr("boilFps", 10.0, "Painterly: stroke changes per second while boiling.", 0.0);

    a[kToonShadowColor] = colorAttr("toonShadowColor", glm::dvec3(0.35, 0.38, 0.55), "Toon: multiplies the base colour in shadow.");
    a[kToonLightTint] = colorAttr("toonLightTint", glm::dvec3(1.0, 0.97, 0.92), "Toon: multiplies the base colour in light.");
    a[kToonThreshold] = floatAttr("toonThreshold", 0.0, "Toon: where light turns to shadow, as N.L from -1 to 1.", -1.0, 1.0);
    a[kToonSoftness] = floatAttr("toonSoftness", 0.08, "Toon: width of the light to shadow ramp; 0 is a hard line.", 0.0);
    a[kToonOuterOffset] = floatAttr("toonOuterOffset", 0.45, "Toon: how much deeper into the shadow the second, harder band starts; 0 hides it.", 0.0);
    a[kToonOuterColor] = colorAttr("toonOuterColor", glm::dvec3(0.20, 0.20, 0.35), "Toon: multiplies the base colour in the outer shadow band.");
    a[kRimColor] = colorAttr("rimColor", glm::dvec3(1.0), "Toon: linear rim light colour.");
    a[kRimWidth] = floatAttr("rimWidth", 0.3, "Toon: how far the rim reaches in from the silhouette, 0 to 1.", 0.0, 1.0);
    a[kRimStrength] = floatAttr("rimStrength", 0.4, "Toon: rim light brightness; 0 is off.", 0.0);

    a[kFoliageBottomColor] = colorAttr("foliageBottomColor", glm::dvec3(0.05, 0.12, 0.06), "Foliage: linear colour at the bottom of the object.");
    a[kFoliageTopColor] = colorAttr("foliageTopColor", glm::dvec3(0.35, 0.50, 0.15), "Foliage: linear colour at the top of the object.");
    a[kFoliageGradient] = floatAttr("foliageGradient", 0.6, "Foliage: how much the bottom to top gradient replaces the base colour.", 0.0, 1.0);
    a[kFoliageBottomShadow] = floatAttr("foliageBottomShadow", 0.5, "Foliage: darkens surfaces facing down.", 0.0, 1.0);
    a[kFoliageTopHighlight] = floatAttr("foliageTopHighlight", 0.3, "Foliage: brightens surfaces facing up.", 0.0);
    a[kFoliageTranslucency] = floatAttr("foliageTranslucency", 0.4, "Foliage: light passing through toward the camera from behind.", 0.0);
    a[kFoliageWrap] = floatAttr("foliageWrap", 0.5, "Foliage: wraps the light around the form, 0 to 1, for soft clumps.", 0.0, 1.0);

    a[kSubsurfaceColor] = colorAttr("subsurfaceColor", glm::dvec3(0.9, 0.25, 0.15), "Skin: linear colour of the light scattered under the surface.");
    a[kSubsurfaceAmount] = floatAttr("subsurfaceAmount", 0.8, "Skin: how far light spreads under the surface, 0 to 1.", 0.0, 1.0);
    a[kSheenColor] = colorAttr("sheenColor", glm::dvec3(0.5), "Cloth: linear colour of the sheen at grazing angles.");
    a[kSheenRoughness] = floatAttr("sheenRoughness", 0.5, "Cloth: spread of the sheen, 0 to 1.", 0.0, 1.0);
    a[kClearcoat] = floatAttr("clearcoat", 1.0, "Clearcoat and eye: strength of the glossy top layer, 0 to 1.", 0.0, 1.0);
    a[kClearcoatRoughness] = floatAttr("clearcoatRoughness", 0.05, "Clearcoat and eye: roughness of the top layer.", 0.0, 1.0);
    a[kAnisotropy] = floatAttr("anisotropy", 0.8, "Hair: how far highlights stretch across the strands, 0 to 1.", 0.0, 1.0);
    a[kAnisotropyRotation] = floatAttr("anisotropyRotation", 0.0, "Hair: degrees from the UVs' V direction to the strands.");
    a[kHairShift] = floatAttr("hairShift", 0.1, "Hair: how far the two highlights shift apart along the strand, 0 to 1.", 0.0, 1.0);
    a[kIrisRadius] = floatAttr("irisRadius", 0.15, "Eye: iris radius in UV units, around the UV centre.", 0.0, 0.5);
    a[kIrisDepth] = floatAttr("irisDepth", 0.3, "Eye: how deep the iris sits under the cornea; it shifts with the view.", 0.0, 1.0);
    a[kLimbalRing] = floatAttr("limbalRing", 0.5, "Eye: darkening around the iris's edge, 0 to 1.", 0.0, 1.0);
    a[kBevelRadius] = floatAttr("bevelRadius", 0.0, "Rounds hard edges as they shade, by ray tracing the surfaces within this many cm; 0 is "
                                                    "off. Needs a GPU with ray tracing. Bake a roundedNormal map to keep it for game engines.", 0.0);
}

/** A height map and the normals rebuilt from it, as Unreal's MF_LilaHeightNormal rebuilds them. */
void heightAttrs(std::vector<AttrDef>& a)
{
    a[kHeightMap] = pathAttr("heightMap", "Linear height texture (red channel, 16 bit), 0.5 at the surface. Unreal gets it as HeightMap.");
    a[kNormalFromHeight] = {.name = "normalFromHeight", .type = PlugType::Bool, .defaultValue = false, .flags = AttrInput | AttrStorable,
                            .doc = "Rebuild the shape's normals from the height map and lay the normal map on them as detail (bake "
                                   "detailNormal for it), so steep sides light as walls. Unreal builds it with MF_LilaHeightNormal."};
    a[kHeightDepth] = floatAttr("heightDepth", 5.0, "cm from height 0 to height 1: the relief the height map stands for.", 0.0);
    a[kHeightUvWidth] = floatAttr("heightUvWidth", 100.0, "World size of the UV square along U, cm.", 1e-3);
    a[kHeightUvHeight] = floatAttr("heightUvHeight", 100.0, "World size of the UV square along V, cm.", 1e-3);
}

constexpr int kSampledMaps[] = {kBaseColorMap, kMetallicMap, kRoughnessMap, kNormalMap, kAmbientOcclusionMap, kOrmMap, kEmissiveMap};
static_assert(kCount == kTextureSampling + int(std::size(kSampledMaps)) * 4);

/** wrapU, wrapV, filter and colorSpace of every sampled map, named after it (baseColorMapWrapU). */
void samplingAttrs(std::vector<AttrDef>& a)
{
    for (size_t k = 0; k < std::size(kSampledMaps); ++k)
    {
        const std::string map = a[size_t(kSampledMaps[k])].name;
        const int first = kTextureSampling + int(k) * 4;
        for (int axis = 0; axis < 2; ++axis)
        {
            const char* uv = axis == 0 ? "U" : "V";
            a[size_t(first + axis)] = enumAttr(map + "Wrap" + uv, {"repeat", "clamp", "mirror", "black"},
                                               std::format("What {} shows past its edges along {}: repeat, clamp (the edge texels), "
                                                           "mirror (repeat flipped) or black.",
                                                           map, uv));
            a[size_t(first + axis)].category = "Texture Sampling";
        }
        a[size_t(first + 2)] = enumAttr(map + "Filter", {"linear", "nearest"},
                                        std::format("How {} is filtered: linear (smooth, mipmapped) or nearest (hard texels, for pixel art).", map));
        a[size_t(first + 2)].category = "Texture Sampling";
        a[size_t(first + 3)] = enumAttr(map + "ColorSpace", {"auto", "sRGB", "linear"},
                                        std::format("How {}'s 8- and 16-bit values are read: auto ({}), sRGB, or linear (raw data). "
                                                    "EXR and HDR files are always linear.",
                                                    map, kSampledMaps[k] == kBaseColorMap || kSampledMaps[k] == kEmissiveMap ? "sRGB, as a colour map" : "linear, as a data map"));
        a[size_t(first + 3)].category = "Texture Sampling";
    }
}

std::vector<AttrDef> materialAttrs()
{
    std::vector<AttrDef> a(kCount);
    a[kBaseColor] = colorAttr("baseColor", glm::dvec3(0.5), "Linear base color, used when no base color map is set.");
    a[kBaseColorMap] = pathAttr("baseColorMap", "Base color texture (sRGB unless baseColorMapColorSpace says otherwise); its alpha is the opacity of masked and translucent materials.");
    a[kMetallic] = floatAttr("metallic", 0.0, "0 dielectric, 1 metal.", 0.0, 1.0);
    a[kMetallicMap] = pathAttr("metallicMap", "Linear metallic texture (red channel).");
    a[kSpecular] = floatAttr("specular", 0.5, "Dielectric reflectance; 0.5 is 4%, Unreal's default.", 0.0, 1.0);
    a[kRoughness] = floatAttr("roughness", 0.4, "0 mirror, 1 fully rough.", 0.0, 1.0);
    a[kRoughness].fileDefault = 0.5;
    a[kRoughnessMap] = pathAttr("roughnessMap", "Linear roughness texture (red channel).");
    a[kNormalMap] = pathAttr("normalMap", "Tangent-space normal map.");
    a[kNormalFormat] = enumAttr("normalFormat", {"directx", "opengl"},
                                "Green channel convention: directx (green down, Unreal) or opengl (green up; Flip Green in the editor).");
    a[kAmbientOcclusionMap] = pathAttr("ambientOcclusionMap", "Linear occlusion texture (red channel); darkens ambient light only.");
    a[kOrmMap] = pathAttr("ormMap", "Packed occlusion/roughness/metallic texture, channels per packedLayout. Wins over the separate maps.");
    a[kPackedLayout] = enumAttr("packedLayout", {"orm", "mro"},
                                "Channel order of the packed map: orm (R occlusion, G roughness, B metallic, Unreal) or mro (R metallic, "
                                "G roughness, B occlusion).");
    a[kEmissiveColor] = colorAttr("emissiveColor", glm::dvec3(0.0), "Linear emitted light, used when no emissive map is set.");
    a[kEmissiveMap] = pathAttr("emissiveMap", "Emissive texture (sRGB unless emissiveMapColorSpace says otherwise).");
    a[kEmissiveIntensity] = floatAttr("emissiveIntensity", 1.0, "Scales the emissive color or map.", 0.0);
    a[kOpacity] = floatAttr("opacity", 1.0, "Masked and translucent only; multiplies the base color map's alpha.", 0.0, 1.0);
    a[kOpacityMaskClip] = floatAttr("opacityMaskClipValue", 0.3333, "Masked only: opacity below this is cut away.", 0.0, 1.0);
    a[kBlendMode] = enumAttr("blendMode", {"opaque", "masked", "translucent"}, "How opacity is used.");
    a[kTwoSided] = {.name = "twoSided", .type = PlugType::Bool, .defaultValue = false, .flags = AttrInput | AttrStorable,
                    .doc = "Draw back faces too (with flipped normals). One-sided materials cull them, as in Unreal."};
    a[kUvSet] = {.name = "uvSet", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable,
                 .doc = "UV set the textures use (0 or 1).", .minValue = 0.0, .maxValue = 1.0};
    stylisedAttrs(a);
    heightAttrs(a);
    samplingAttrs(a);
    return a;
}

Node& materialArg(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n || !n->type().isA("material"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a material");
    return *n;
}

geo::Mesh baseMesh(Graph& g, const PlugRef& p)
{
    g.evaluate();
    const MeshPtr& m = std::get<MeshPtr>(g.storedValue(p));
    return m ? *m : geo::Mesh();
}

std::vector<uint32_t> liveFaces(const geo::Mesh& m)
{
    std::vector<uint32_t> out;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            out.push_back(f);
    return out;
}

/** Texture attr for a channel name, -1 when unknown. */
int channelAttr(const std::string& channel)
{
    for (const auto& [name, attr] : materials::textureChannels())
        if (name == channel)
            return attr;
    if (channel == "ao" || channel == "occlusion")
        return kAmbientOcclusionMap;
    if (channel == "mro")
        return kOrmMap;
    return -1;
}

json describe(Graph& g, const Node& n)
{
    json out = {{"name", n.name()}};
    const auto& defs = n.type().attrs;
    for (int i = 0; i < kCount; ++i)
    {
        const AttrDef& d = defs[size_t(i)];
        if (d.type == PlugType::Enum)
        {
            int64_t v = n.as<int64_t>(i);
            out[d.name] = v >= 0 && size_t(v) < d.enumNames.size() ? json(d.enumNames[size_t(v)]) : json(v);
        }
        else
            out[d.name] = toJson(n.value(i));
    }
    out["flipGreen"] = NormalFormat(n.as<int64_t>(kNormalFormat)) == NormalFormat::OpenGL;
    json used = json::array();
    g.evaluate();
    for (NodeId id : g.nodes())
    {
        Node* m = g.node(id);
        if (!m->type().isA("mesh"))
            continue;
        const MeshPtr& mesh = m->as<MeshPtr>(MeshNode::kOutMesh);
        if (!mesh)
            continue;
        for (const auto& [slot, count] : materials::slotUsage(*mesh))
            if (slot == n.name())
                used.push_back({{"object", m->path()}, {"faces", count}});
    }
    out["usedBy"] = std::move(used);
    return out;
}

/** Sets flags of createMaterial that name material attrs. */
void applyFlags(Graph& g, Node& n, const Args& a)
{
    const auto& defs = n.type().attrs;
    for (int i = 0; i < kCount; ++i)
    {
        const AttrDef& d = defs[size_t(i)];
        std::string flag = d.name == "emissiveColor" ? "emissive" : d.name;
        if (!a.has(flag))
            continue;
        try
        {
            g.setValue({n.id(), i, -1}, fromJson(d, a.flags[flag]));
        }
        catch (const std::invalid_argument& e)
        {
            throw CommandError("createMaterial: " + flag + ": " + e.what());
        }
    }
    if (a.has("flipGreen"))
        g.setValue({n.id(), kNormalFormat, -1}, int64_t(a.get<bool>("flipGreen") ? NormalFormat::OpenGL : NormalFormat::DirectX));
}

/** Every mesh the command targets, with the face ids to assign (empty = whole object). */
std::map<NodeId, std::vector<uint32_t>> assignTargets(Graph& g, const Args& a)
{
    std::map<NodeId, std::vector<uint32_t>> out;
    std::vector<std::string> named;
    for (size_t i = 1; i < a.positional.size(); ++i)
    {
        const json& v = a.positional[i];
        if (v.is_string())
            named.push_back(v.get<std::string>());
        else if (v.is_array())
            for (const json& x : v)
                if (x.is_string())
                    named.push_back(x.get<std::string>());
    }

    if (a.has("faces"))
    {
        if (named.size() != 1)
            throw CommandError("assignMaterial: faces= needs exactly one object");
        Node* n = g.find(named[0]);
        if (!n || !n->type().isA("mesh"))
            throw CommandError("assignMaterial: " + named[0] + " is not a mesh");
        std::vector<uint32_t>& ids = out[n->id()];
        for (const json& f : a.flags["faces"])
            ids.push_back(f.get<uint32_t>());
        if (ids.empty())
            out.erase(n->id());
        return out;
    }

    // No objects named: the face selection in face mode, else the selected objects.
    if (named.empty() && components::mode() == ComponentMode::Face)
        for (const auto& [node, ids] : components::selected(ComponentMode::Face))
            if (Node* n = g.node(node); n && n->type().isA("mesh") && !ids.empty())
                out[node] = ids;
    if (!out.empty())
        return out;
    if (named.empty())
        for (NodeId id : g.selection())
            named.push_back(g.node(id)->path());
    for (const std::string& name : named)
    {
        Node* n = g.find(name);
        if (!n)
            throw CommandError("assignMaterial: no object named " + name);
        // A selected group assigns to every mesh under it, like Maya.
        if (n->type().isA("mesh"))
            out[n->id()];
        for (NodeId d : g.descendants(n->id()))
            if (g.node(d)->type().isA("mesh"))
                out[d];
    }
    return out;
}

/** Rewrites one slot name on every mesh's base mesh. */
void renameSlots(Graph& g, const std::string& from, const std::string& to)
{
    LILA_ZONE();

    g.evaluate();
    for (NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n->type().isA("mesh") || n->referenceId())
            continue;
        MeshPtr cage = modifiers::cage(g, id);
        if (!cage || std::ranges::find(cage->materialNames(), from) == cage->materialNames().end())
            continue;
        PlugRef base = modifiers::editPlug(g, id);
        geo::Mesh m = baseMesh(g, base);
        for (std::string& s : m.materialNames())
            if (s == from)
                s = to;
        g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
    }
}
} // namespace

namespace materials
{
Node* find(Graph& g, std::string_view slot)
{
    // Slots hold the material's name, which a DAG node elsewhere may share.
    for (NodeId id : g.findAll(slot))
        if (Node* n = g.node(id); n->type().isA("material"))
            return n;
    return nullptr;
}

std::vector<Node*> all(Graph& g)
{
    std::vector<Node*> out;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n->type().isA("material"))
            out.push_back(n);
    return out;
}

Node* create(Graph& g, std::string_view name)
{
    const NodeType* t = types::find("material");
    if (!t)
        throw CommandError("material type not registered");
    // Exact names: a slot only binds to a node of the same name, so the naming policy must not rewrite it.
    return g.createNode(*t, name.empty() ? std::string_view("M_Material") : name, kNoNode, false);
}

std::string rename(Graph& g, NodeId id, const std::string& name)
{
    Node* n = g.node(id);
    const std::string from = n->name();
    std::string to = g.rename(id, name);
    if (to != from && n->type().isA("material"))
        renameSlots(g, from, to);
    return to;
}

std::array<int, 3> packedChannels(PackedLayout layout)
{
    if (layout == PackedLayout::MRO)
        return {2, 1, 0};
    return {0, 1, 2};
}

std::string guessTextureChannel(std::string_view fileName)
{
    static const std::map<std::string, std::string, std::less<>> suffixes = {
        {"basecolor", "baseColor"}, {"albedo", "baseColor"},  {"diffuse", "baseColor"},  {"diff", "baseColor"},
        {"color", "baseColor"},     {"col", "baseColor"},     {"bc", "baseColor"},       {"d", "baseColor"},
        {"normal", "normal"},       {"normalgl", "normal"},   {"normaldx", "normal"},    {"norm", "normal"},
        {"nrm", "normal"},          {"nor", "normal"},        {"n", "normal"},           {"roughness", "roughness"},
        {"rough", "roughness"},     {"rgh", "roughness"},     {"r", "roughness"},        {"metallic", "metallic"},
        {"metalness", "metallic"},  {"metal", "metallic"},    {"m", "metallic"},         {"ao", "ambientOcclusion"},
        {"occlusion", "ambientOcclusion"}, {"ambientocclusion", "ambientOcclusion"},     {"occ", "ambientOcclusion"},
        {"orm", "orm"},             {"arm", "orm"},           {"crm", "orm"},            {"mro", "mro"},
        {"emissive", "emissive"},   {"emission", "emissive"}, {"emit", "emissive"},      {"e", "emissive"},
    };
    std::string_view stem = fileName.substr(fileName.find_last_of("/\\") + 1);
    stem = stem.substr(0, stem.rfind('.'));
    std::vector<std::string> tokens(1);
    for (char c : stem)
    {
        if (c == '_' || c == '-' || c == '.' || c == ' ')
            tokens.emplace_back();
        else
            tokens.back() += char(std::tolower(static_cast<unsigned char>(c)));
    }
    auto numeric = [](const std::string& t) { return t.empty() || std::ranges::all_of(t, [](char c) { return c >= '0' && c <= '9'; }); };
    while (tokens.size() > 1 && numeric(tokens.back()))
        tokens.pop_back();
    if (tokens.size() < 2)
        return {};
    auto it = suffixes.find(tokens.back());
    return it == suffixes.end() ? std::string() : it->second;
}

const std::vector<std::pair<std::string, int>>& textureChannels()
{
    static const std::vector<std::pair<std::string, int>> c = {
        {"baseColor", kBaseColorMap}, {"metallic", kMetallicMap},  {"roughness", kRoughnessMap},
        {"normal", kNormalMap},       {"ambientOcclusion", kAmbientOcclusionMap}, {"orm", kOrmMap},
        {"emissive", kEmissiveMap},   {"stroke", kStrokeMap},       {"height", kHeightMap},
    };
    return c;
}

std::span<const int> sampledMaps() { return kSampledMaps; }

int samplingAttr(int mapAttr)
{
    for (size_t k = 0; k < std::size(kSampledMaps); ++k)
        if (kSampledMaps[k] == mapAttr)
            return kTextureSampling + int(k) * 4;
    return -1;
}

TextureSampling sampling(const Node& material, int mapAttr)
{
    TextureSampling s;
    const int first = samplingAttr(mapAttr);
    if (first < 0 || !material.type().isA("material"))
        return s;
    auto pick = [&](int attr, int64_t count) { return std::clamp<int64_t>(material.as<int64_t>(attr), 0, count - 1); };
    s.wrapU = TextureWrap(pick(first, 4));
    s.wrapV = TextureWrap(pick(first + 1, 4));
    s.filter = TextureFilter(pick(first + 2, 2));
    s.colorSpace = TextureColorSpace(pick(first + 3, 3));
    return s;
}

void setSampling(Graph& g, NodeId material, int mapAttr, const TextureSampling& s)
{
    const int first = samplingAttr(mapAttr);
    const Node* n = g.node(material);
    if (first < 0 || !n)
        return;
    const int64_t values[4] = {int64_t(s.wrapU), int64_t(s.wrapV), int64_t(s.filter), int64_t(s.colorSpace)};
    for (int k = 0; k < 4; ++k)
        if (n->as<int64_t>(first + k) != values[k])
            g.setValue({material, first + k, -1}, values[k]);
}

const char* wrapName(TextureWrap w)
{
    switch (w)
    {
    case TextureWrap::Clamp: return "clamp";
    case TextureWrap::Mirror: return "mirror";
    case TextureWrap::Black: return "black";
    default: return "repeat";
    }
}

bool colorMap(int mapAttr) { return mapAttr == kBaseColorMap || mapAttr == kEmissiveMap; }

bool srgb(const Node& material, int mapAttr)
{
    switch (sampling(material, mapAttr).colorSpace)
    {
    case TextureColorSpace::Srgb: return true;
    case TextureColorSpace::Linear: return false;
    default: return colorMap(mapAttr);
    }
}

const char* colorSpaceName(TextureColorSpace c)
{
    switch (c)
    {
    case TextureColorSpace::Srgb: return "sRGB";
    case TextureColorSpace::Linear: return "linear";
    default: return "auto";
    }
}

std::optional<bool> srgbFromName(std::string_view name)
{
    std::string n(name);
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (n.empty() || n == "auto")
        return std::nullopt;
    for (const char* lin : {"raw", "linear", "lin_", "non-color", "acescg", "aces2065", "acescct", "data", "utility - xyz"})
        if (n.find(lin) != std::string::npos)
            return false;
    if (n.find("srgb") != std::string::npos || n.find("gamma 2.2") != std::string::npos || n.find("rec.709 (srgb)") != std::string::npos)
        return true;
    return std::nullopt;
}

TextureColorSpace colorSpaceFor(int mapAttr, bool srgb)
{
    if (srgb == colorMap(mapAttr))
        return TextureColorSpace::Auto;
    return srgb ? TextureColorSpace::Srgb : TextureColorSpace::Linear;
}

std::string resolvePath(const std::string& path)
{
    return paths::toUtf8(paths::resolvePath(path));
}

std::vector<std::string> builtinStrokeSheets()
{
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(paths::resourcesDir() / "strokes", ec))
        if (e.path().extension() == ".png")
            out.push_back(paths::toUtf8(e.path().stem()));
    std::sort(out.begin(), out.end());
    return out;
}

std::string strokeSheetPath(const std::string& strokeMap)
{
    const std::filesystem::path dir = paths::resourcesDir() / "strokes";
    if (strokeMap.empty())
        return paths::toUtf8(dir / "strokes_soft.png");
    if (strokeMap.find_first_of("/\\.") == std::string::npos)
        return paths::toUtf8(dir / (strokeMap + ".png"));
    return resolvePath(strokeMap);
}

int32_t slotIndex(geo::Mesh& mesh, const std::string& name)
{
    auto& names = mesh.materialNames();
    auto it = std::ranges::find(names, name);
    if (it != names.end())
        return int32_t(it - names.begin());
    names.push_back(name);
    return int32_t(names.size() - 1);
}

std::string slotName(const geo::Mesh& mesh, int32_t id)
{
    const auto& names = mesh.materialNames();
    if (id >= 0 && size_t(id) < names.size())
        return names[size_t(id)];
    return names.empty() && id == 0 ? std::string(kDefaultSlot) : "Material" + std::to_string(id);
}

void assign(geo::Mesh& mesh, const std::string& slot, const std::vector<uint32_t>& faces)
{
    LILA_ZONE();

    bool hadAttr = mesh.faceAttrs().has(geo::attr::kMaterial);
    if (faces.empty())
    {
        mesh.materialNames() = {slot};
        std::span<int32_t> ids = mesh.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
        std::ranges::fill(ids, 0);
        return;
    }
    // Faces without a slot yet keep the default material under an explicit name.
    if (mesh.materialNames().empty())
        mesh.materialNames().push_back(kDefaultSlot);
    else if (!hadAttr)
        mesh.materialNames().resize(1);
    int32_t id = slotIndex(mesh, slot);
    std::span<int32_t> ids = mesh.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
    for (uint32_t f : faces)
        if (f < ids.size() && mesh.faceValid(f))
            ids[f] = id;
    pruneSlots(mesh);
}

void pruneSlots(geo::Mesh& mesh)
{
    std::span<int32_t> ids = mesh.faceAttrs().get<int32_t>(geo::attr::kMaterial);
    auto& names = mesh.materialNames();
    if (ids.empty())
        return;
    int32_t maxId = 0;
    for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f))
            maxId = std::max(maxId, ids[f]);
    std::vector<uint32_t> count(std::max(size_t(maxId) + 1, names.size()), 0);
    for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f))
            ++count[size_t(std::max(0, ids[f]))];
    std::vector<int32_t> remap(count.size(), 0);
    std::vector<std::string> kept;
    for (size_t i = 0; i < count.size(); ++i)
        if (count[i])
        {
            remap[i] = int32_t(kept.size());
            kept.push_back(slotName(mesh, int32_t(i)));
        }
    for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f))
            ids[f] = remap[size_t(std::max(0, ids[f]))];
    names = std::move(kept);
}

std::vector<std::pair<std::string, uint32_t>> slotUsage(const geo::Mesh& mesh)
{
    std::span<const int32_t> ids = mesh.faceAttrs().get<int32_t>(geo::attr::kMaterial);
    std::vector<uint32_t> count(std::max<size_t>(mesh.materialNames().size(), 1), 0);
    for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        size_t id = ids.empty() ? 0 : size_t(std::max(0, ids[f]));
        if (id >= count.size())
            count.resize(id + 1, 0);
        ++count[id];
    }
    std::vector<std::pair<std::string, uint32_t>> out;
    for (size_t i = 0; i < count.size(); ++i)
        if (count[i] || i < mesh.materialNames().size())
            out.push_back({slotName(mesh, int32_t(i)), count[i]});
    return out;
}

void registerTypes()
{
    if (types::find("material"))
        return;
    NodeType t;
    t.name = "material";
    t.icon = "palette";
    t.ownAttrs = materialAttrs();
    t.create = [] { return std::make_unique<Node>(); };
    types::registerType(std::move(t));
}

void registerCommands()
{
    using T = ArgType;

    CommandDef create{
        .name = "createMaterial",
        .doc = "Create a material (Unreal default lit inputs). Faces use it through a slot of the same name. Returns its name.",
        .positionalDoc = "name",
        .flags = {{"baseColor", "bc", T::Vec3, nullptr, "Linear base color."},
                  {"metallic", "", T::Float, nullptr, "0 to 1.", {}, {}, 0.0, 1.0},
                  {"roughness", "", T::Float, nullptr, "0 to 1.", {}, {}, 0.0, 1.0},
                  {"specular", "", T::Float, nullptr, "Dielectric reflectance, 0.5 default.", {}, {}, 0.0, 1.0},
                  {"emissive", "", T::Vec3, nullptr, "Linear emissive color."},
                  {"emissiveIntensity", "", T::Float, nullptr, "Emissive scale.", {}, {}, 0.0},
                  {"opacity", "", T::Float, nullptr, "Masked/translucent opacity.", {}, {}, 0.0, 1.0},
                  {"opacityMaskClipValue", "", T::Float, nullptr, "Masked cutoff.", {}, {}, 0.0, 1.0},
                  {"blendMode", "", T::Any, nullptr, "opaque, masked or translucent."},
                  {"twoSided", "", T::Bool, nullptr, "Draw back faces."},
                  {"uvSet", "", T::Int, nullptr, "UV set for textures.", {}, {}, 0.0, 1.0},
                  {"normalFormat", "", T::Any, nullptr, "directx (Unreal) or opengl."},
                  {"flipGreen", "", T::Bool, nullptr, "Normal map is OpenGL style (green up); same as normalFormat='opengl'."},
                  {"baseColorMap", "", T::String, nullptr, "Texture path."},
                  {"metallicMap", "", T::String, nullptr, "Texture path."},
                  {"roughnessMap", "", T::String, nullptr, "Texture path."},
                  {"normalMap", "", T::String, nullptr, "Texture path."},
                  {"ambientOcclusionMap", "", T::String, nullptr, "Texture path."},
                  {"ormMap", "", T::String, nullptr, "Packed occlusion/roughness/metallic texture path."},
                  {"packedLayout", "", T::Any, nullptr, "orm or mro: the packed map's channel order."},
                  {"emissiveMap", "", T::String, nullptr, "Texture path."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::string name = !a.positional.empty() && a.positional[0].is_string() ? a.positional[0].get<std::string>() : "M_Material";
            if (!Graph::validName(name))
                throw CommandError("createMaterial: invalid name '" + name + "'");
            Node* n = materials::create(g, name);
            applyFlags(g, *n, a);
            return n->path();
        },
    };
    // The stylised shading models' inputs, as flags named like their attributes.
    const std::vector<AttrDef> attrs = materialAttrs();
    for (size_t i = kShadingModel; i < attrs.size(); ++i)
    {
        const AttrDef& d = attrs[i];
        const T type = d.type == PlugType::Float ? T::Float : d.type == PlugType::Vec3 ? T::Vec3 : d.type == PlugType::Bool ? T::Bool
                       : d.type == PlugType::Int ? T::Int : d.type == PlugType::String ? T::String : T::Any;
        create.flags.push_back({d.name, "", type, nullptr, d.doc, {}, {}, d.minValue, d.maxValue});
    }
    ops::registerCommand(std::move(create));

    ops::registerCommand({
        .name = "assignMaterial",
        .doc = "Assign a material to whole objects, to faces= of one object, or (with no objects) to the face selection in "
               "face mode, else the selected objects. Unused slots are dropped.",
        .positionalDoc = "material [objects...]",
        .flags = {{"faces", "f", T::List, nullptr, "Face ids of the one object given."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("assignMaterial: give a material name");
            std::string mat = a.positional[0].get<std::string>();
            // The default slot may be assigned without a node, to clear an assignment.
            if (mat != materials::kDefaultSlot)
                materialArg(g, mat, "assignMaterial");
            auto targets = assignTargets(g, a);
            if (targets.empty())
                throw CommandError("assignMaterial: select meshes or faces first");
            UndoChunk chunk("Assign Material");
            json out = json::array();
            for (auto& [id, faces] : targets)
            {
                PlugRef base = modifiers::editPlug(g, id);
                geo::Mesh m = baseMesh(g, base);
                materials::assign(m, mat, faces);
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
                out.push_back(g.node(id)->path());
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "listMaterials",
        .doc = "Names of every material; with objects, their slots instead: [{object, slots: [{slot, material, faces}]}].",
        .positionalDoc = "[objects...]",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            json out = json::array();
            if (a.positional.empty())
            {
                for (Node* n : materials::all(g))
                    out.push_back(n->path());
                return out;
            }
            g.evaluate();
            for (const std::string& name : a.objects())
            {
                Node* n = g.find(name);
                if (!n || !n->type().isA("mesh"))
                    throw CommandError("listMaterials: " + name + " is not a mesh");
                json slots = json::array();
                if (const MeshPtr& m = n->as<MeshPtr>(MeshNode::kOutMesh))
                    for (const auto& [slot, count] : materials::slotUsage(*m))
                        slots.push_back({{"slot", slot}, {"material", materials::find(g, slot) != nullptr}, {"faces", count}});
                out.push_back({{"object", n->path()}, {"slots", std::move(slots)}});
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "materialInfo",
        .doc = "Every input of a material plus the meshes using it: {name, baseColor, ..., flipGreen, usedBy: [{object, faces}]}.",
        .positionalDoc = "material",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("materialInfo: give a material name");
            return describe(g, materialArg(g, a.positional[0].get<std::string>(), "materialInfo"));
        },
    });

    ops::registerCommand({
        .name = "selectByMaterial",
        .doc = "Select what a material is assigned to: whole objects when it covers every face of each, otherwise just the "
               "faces it is assigned to. Default also takes faces on slots with no material. faces=true always selects faces, "
               "faces=false always whole objects. Returns the objects.",
        .positionalDoc = "material",
        .flags = {{"faces", "f", T::Bool, nullptr, "Force faces (true) or whole objects (false); unset picks by coverage."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("selectByMaterial: give a material name");
            const std::string arg = a.positional[0].get<std::string>();
            // The default without a node of its name also takes faces whose slot has no material, since they draw with it.
            const bool builtinDefault = arg == materials::kDefaultSlot && !materials::find(g, arg);
            const std::string mat = builtinDefault ? arg : materialArg(g, arg, "selectByMaterial").name();
            auto matches = [&](const std::string& slot) { return slot == mat || (builtinDefault && !materials::find(g, slot)); };
            g.evaluate();
            json objects = json::array();
            std::vector<std::pair<std::string, json>> faces;
            bool partial = false;
            for (NodeId id : g.nodes())
            {
                Node* n = g.node(id);
                const MeshPtr mesh = n->type().isA("mesh") ? n->as<MeshPtr>(MeshNode::kOutMesh) : nullptr;
                if (!mesh)
                    continue;
                std::span<const int32_t> ids = mesh->faceAttrs().get<int32_t>(geo::attr::kMaterial);
                std::map<int32_t, bool> slotHit;
                json hit = json::array();
                // A mesh never assigned anything has no material ids: every face is on slot 0.
                for (size_t f = 0; f < mesh->faceCapacity(); ++f)
                {
                    if (!mesh->faceValid(geo::Index(f)))
                        continue;
                    const int32_t slot = ids.empty() ? 0 : ids[f];
                    auto [it, added] = slotHit.try_emplace(slot);
                    if (added)
                        it->second = matches(materials::slotName(*mesh, slot));
                    if (it->second)
                        hit.push_back(f);
                }
                if (hit.empty())
                    continue;
                partial |= hit.size() < mesh->numFaces();
                objects.push_back(n->path());
                faces.emplace_back(n->path(), std::move(hit));
            }
            ops::execute("select", objects.empty() ? json{{"clear", true}} : json{{"args", objects}});
            if (objects.empty() || !(a.has("faces") ? a.get<bool>("faces") : partial))
            {
                ops::execute("selectMode", {{"args", {"object"}}});
                return objects;
            }
            ops::execute("selectMode", {{"args", {"face"}}});
            bool first = true;
            for (auto& [name, ids] : faces)
            {
                ops::execute("selectComponents", {{"args", {name}}, {"faces", ids}, {"add", !first}});
                first = false;
            }
            return objects;
        },
    });

    ops::registerCommand({
        .name = "setMaterialTexture",
        .doc = "Set or clear (path='') a texture: channel baseColor, metallic, roughness, normal, ambientOcclusion, orm, mro "
               "(the packed map with the MRO layout) or emissive. Stored relative to the workspace root (or the scene "
               "folder without one). wrap, wrapU, wrapV and filter set how it is sampled; given without path they change "
               "only that.",
        .positionalDoc = "material",
        .flags = {{"channel", "c", T::String, nullptr, "Texture channel."},
                  {"path", "p", T::String, nullptr, "Image file; empty clears."},
                  {"wrap", "", T::String, nullptr, "Both wrap modes.", {"repeat", "clamp", "mirror", "black"}},
                  {"wrapU", "", T::String, nullptr, "Wrap along U.", {"repeat", "clamp", "mirror", "black"}},
                  {"wrapV", "", T::String, nullptr, "Wrap along V.", {"repeat", "clamp", "mirror", "black"}},
                  {"filter", "", T::String, nullptr, "linear or nearest.", {"linear", "nearest"}}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("setMaterialTexture: give a material name");
            Node& n = materialArg(g, a.positional[0].get<std::string>(), "setMaterialTexture");
            std::string channel = a.get<std::string>("channel");
            int attr = channelAttr(channel);
            if (attr < 0)
                throw CommandError("setMaterialTexture: unknown channel '" + channel + "'");
            const bool sampled = a.has("wrap") || a.has("wrapU") || a.has("wrapV") || a.has("filter");
            if (a.has("path") || !sampled)
            {
                std::string path = a.has("path") ? a.get<std::string>("path") : std::string();
                std::replace(path.begin(), path.end(), '\\', '/');
                g.setValue({n.id(), attr, -1}, paths::storedPath(paths::fromUtf8(path)));
                if (channel == "mro" && !path.empty())
                    g.setValue({n.id(), kPackedLayout, -1}, int64_t(PackedLayout::MRO));
            }
            if (sampled)
            {
                if (materials::samplingAttr(attr) < 0)
                    throw CommandError("setMaterialTexture: the " + channel + " channel has no sampling settings");
                auto wrap = [&](const char* flag, TextureWrap current) {
                    const std::string v = a.has(flag) ? a.get<std::string>(flag) : a.has("wrap") ? a.get<std::string>("wrap") : "";
                    for (TextureWrap w : {TextureWrap::Repeat, TextureWrap::Clamp, TextureWrap::Mirror, TextureWrap::Black})
                        if (v == materials::wrapName(w))
                            return w;
                    if (!v.empty())
                        throw CommandError("setMaterialTexture: unknown wrap '" + v + "'");
                    return current;
                };
                TextureSampling s = materials::sampling(n, attr);
                s.wrapU = wrap("wrapU", s.wrapU);
                s.wrapV = wrap("wrapV", s.wrapV);
                if (a.has("filter"))
                {
                    const std::string f = a.get<std::string>("filter");
                    if (f != "linear" && f != "nearest")
                        throw CommandError("setMaterialTexture: filter must be linear or nearest");
                    s.filter = f == "nearest" ? TextureFilter::Nearest : TextureFilter::Linear;
                }
                materials::setSampling(g, n.id(), attr, s);
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "clearMaterialTextures",
        .doc = "Clear every texture map on the materials, keeping their values and sampling settings. Returns the "
               "channels cleared.",
        .positionalDoc = "material...",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.empty())
                throw CommandError("clearMaterialTextures: give a material name");
            json cleared = json::array();
            for (const json& name : a.positional)
            {
                if (!name.is_string())
                    throw CommandError("clearMaterialTextures: material names are strings");
                Node& n = materialArg(g, name.get<std::string>(), "clearMaterialTextures");
                for (const auto& [channel, attr] : materials::textureChannels())
                    if (!n.as<std::string>(attr).empty())
                    {
                        g.setValue({n.id(), attr, -1}, std::string());
                        cleared.push_back(channel);
                    }
            }
            return cleared;
        },
    });

    ops::registerCommand({
        .name = "renameMaterial",
        .doc = "Rename a material and every slot bound to it. Returns the new name.",
        .positionalDoc = "material newName",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.size() != 2 || !a.positional[0].is_string() || !a.positional[1].is_string())
                throw CommandError("renameMaterial: give the material and its new name");
            Node& n = materialArg(g, a.positional[0].get<std::string>(), "renameMaterial");
            return rename(g, n.id(), a.positional[1].get<std::string>());
        },
    });
}
} // namespace materials
} // namespace lila::scene
