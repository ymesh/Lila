#include "blend/BlendInternal.h"

#include "scene/Materials.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <map>

namespace lila::blend
{
namespace
{
using namespace scene::material;
namespace materials = scene::materials;

std::string extensionFor(const uint8_t* d, size_t n)
{
    if (n >= 8 && std::memcmp(d, "\x89PNG", 4) == 0)
        return ".png";
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8)
        return ".jpg";
    if (n >= 4 && std::memcmp(d, "v/1\x01", 4) == 0)
        return ".exr";
    if (n >= 4 && (std::memcmp(d, "II*\0", 4) == 0 || std::memcmp(d, "MM\0*", 4) == 0))
        return ".tif";
    if (n >= 12 && std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WEBP", 4) == 0)
        return ".webp";
    if (n >= 4 && std::memcmp(d, "DDS ", 4) == 0)
        return ".dds";
    if (n >= 10 && std::memcmp(d, "#?RADIANCE", 10) == 0)
        return ".hdr";
    if (n >= 2 && d[0] == 'B' && d[1] == 'M')
        return ".bmp";
    return ".tga";
}

/** Shader node trees, and what feeds each input socket, followed through reroutes and into and out of node groups. */
class Shaders
{
public:
    struct Tree
    {
        std::vector<View> nodes;
        std::map<const uint8_t*, std::pair<View, View>> from; // input socket -> (node, output socket)
    };
    /** Where a walk is: the tree, and the group node it was entered through (none at the top). */
    struct Frame
    {
        const Tree* tree = nullptr;
        View group;
    };
    using Context = std::vector<Frame>;
    /** What feeds an input: a node and its output socket, or nothing, in which case `unlinked` holds the value. */
    struct Source
    {
        View node;
        View socket;
        Context ctx;
        View unlinked;
    };

    const Tree& tree(const View& nt)
    {
        auto [it, added] = mTrees.try_emplace(nt.data());
        if (added)
        {
            it->second.nodes = nt.list("nodes", "bNode");
            for (const View& l : nt.list("links", "bNodeLink"))
                if (!(l.i("flag") & (1 << 3))) // a muted link passes nothing
                    if (const View to = l.deref("tosock"))
                        it->second.from[to.data()] = {l.deref("fromnode"), l.deref("fromsock")};
        }
        return it->second;
    }

    static std::string idname(const View& node) { return node.str("idname"); }
    static View socket(const View& node, const char* list, std::string_view name)
    {
        for (const View& s : node.list(list, "bNodeSocket"))
            if (s.str("identifier") == name)
                return s;
        for (const View& s : node.list(list, "bNodeSocket"))
            if (s.str("name") == name)
                return s;
        return {};
    }
    static View input(const View& node, std::string_view name) { return socket(node, "inputs", name); }

    Source follow(View in, Context ctx)
    {
        for (int guard = 0; in && guard < 64; ++guard)
        {
            const Tree& t = *ctx.back().tree;
            auto it = t.from.find(in.data());
            if (it == t.from.end())
                return {View(), View(), ctx, in};
            const auto [node, out] = it->second;
            const std::string id = idname(node);
            if (id == "NodeReroute")
            {
                const std::vector<View> ins = node.list("inputs", "bNodeSocket");
                in = ins.empty() ? View() : ins.front();
                continue;
            }
            if (id == "NodeGroupInput")
            {
                if (ctx.size() < 2)
                    return {View(), View(), ctx, in};
                const View group = ctx.back().group;
                ctx.pop_back();
                in = input(group, out.str("identifier"));
                continue;
            }
            if (id == "ShaderNodeGroup")
                if (const View inner = node.deref("id"))
                {
                    const Tree& innerTree = tree(inner);
                    View output;
                    for (const View& n : innerTree.nodes)
                        if (idname(n) == "NodeGroupOutput" && (!output || (n.i("flag") & (1 << 6))))
                            output = n;
                    if (output)
                    {
                        ctx.push_back({&innerTree, node});
                        in = input(output, out.str("identifier"));
                        continue;
                    }
                }
            return {node, out, ctx, View()};
        }
        return {View(), View(), ctx, View()};
    }

private:
    std::map<const uint8_t*, Tree> mTrees;
};

double socketFloat(const View& socket, double fallback)
{
    const View v = socket ? socket.deref("default_value") : View();
    if (!v)
        return fallback;
    if (v.has("value") && v.type()->field("value")->count == 1)
        return v.num("value", 0, fallback);
    return fallback;
}

glm::dvec4 socketColor(const View& socket, glm::dvec4 fallback)
{
    const View v = socket ? socket.deref("default_value") : View();
    if (!v || !v.has("value") || v.type()->field("value")->count < 3)
        return fallback;
    return glm::dvec4(v.num("value", 0), v.num("value", 1), v.num("value", 2), v.type()->field("value")->count >= 4 ? v.num("value", 3) : 1.0);
}

/**
 * A Principled Volume, Volume Scatter or Volume Absorption node as Lila's volume model, with Cycles' coefficients:
 * scattering is density times colour, absorption density times (1 - colour) times (1 - absorption colour). False for
 * any other volume shader.
 */
template <class Set, class ShadersT, class Result>
bool importVolume(const View& node, const typename ShadersT::Context& ctx, const std::string& material, Set&& set, ShadersT& shaders, Result& r)
{
    using namespace scene::material;
    const std::string kind = ShadersT::idname(node);
    if (kind != "ShaderNodeVolumePrincipled" && kind != "ShaderNodeVolumeScatter" && kind != "ShaderNodeVolumeAbsorption")
        return false;
    auto value = [&](std::string_view name) {
        const View in = ShadersT::input(node, name);
        const typename ShadersT::Source src = shaders.follow(in, ctx);
        if (src.node)
            r.skipped.push_back(std::format("material '{}': the volume's {} is driven by nodes; its unlinked value is used", material, name));
        return src.unlinked ? src.unlinked : in;
    };
    const glm::dvec3 color = glm::dvec3(socketColor(value("Color"), glm::dvec4(0.5, 0.5, 0.5, 1.0)));
    const double density = std::max(socketFloat(value("Density"), 1.0), 0.0);
    glm::dvec3 scatter = color, absorb(0.0);
    if (kind == "ShaderNodeVolumePrincipled")
    {
        const glm::dvec3 absorption = glm::dvec3(socketColor(value("Absorption Color"), glm::dvec4(0.0, 0.0, 0.0, 1.0)));
        absorb = glm::max(glm::dvec3(1.0) - color, 0.0) * glm::max(glm::dvec3(1.0) - absorption, 0.0);
        const double strength = socketFloat(value("Emission Strength"), 0.0);
        set(kVolumeEmissionColor, glm::dvec3(socketColor(value("Emission Color"), glm::dvec4(1.0))));
        set(kVolumeEmissionStrength, std::max(strength, 0.0));
        if (socketFloat(value("Blackbody Intensity"), 0.0) > 0.0)
            r.skipped.push_back(std::format("material '{}': the volume's blackbody glow has no Lila match; use its emission", material));
    }
    else if (kind == "ShaderNodeVolumeAbsorption")
    {
        scatter = glm::dvec3(0.0);
        absorb = glm::max(glm::dvec3(1.0) - color, 0.0);
    }
    set(kShadingModel, int64_t(ShadingModel::Volume));
    set(kVolumeDensity, density);
    set(kVolumeScatterColor, scatter);
    set(kVolumeAbsorptionColor, absorb);
    if (kind != "ShaderNodeVolumeAbsorption")
        set(kVolumeAnisotropy, std::clamp(socketFloat(value("Anisotropy"), 0.0), -0.95, 0.95));
    return true;
}
} // namespace

std::string Importer::imagePath(const View& image)
{
    if (!image)
        return {};
    if (auto it = imagePaths.find(image.data()); it != imagePaths.end())
        return it->second;
    std::string& out = imagePaths[image.data()];
    const std::string name = image.idName();
    View packed = image.deref("packedfile");
    if (!packed)
        if (const std::vector<View> list = image.list("packedfiles", "ImagePackedFile"); !list.empty())
            packed = list.front().deref("packedfile");
    if (packed)
    {
        const Block* bytes = packed.block("data");
        const size_t size = size_t(std::max(0, packed.i("size")));
        if (!bytes || bytes->size < size)
        {
            r.warnings.push_back(std::format("image '{}': its packed data is damaged", name));
            return out;
        }
        std::string fileName = fs::path(std::u8string(name.begin(), name.end())).filename().string();
        for (char& c : fileName)
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
                c = '_';
        // The extension comes from the bytes: Blender names copies "a.png.001", and the name may not say the format at all.
        const std::string ext = extensionFor(bytes->data, size);
        fs::path target = textureDir / fs::path(std::u8string(fileName.begin(), fileName.end()));
        if (target.extension().string() != ext)
            target += ext;
        std::error_code ec;
        fs::create_directories(textureDir, ec);
        std::ofstream f(target, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes->data), std::streamsize(size));
        if (!f)
        {
            r.warnings.push_back(std::format("image '{}': can't write {}", name, utf8(target)));
            return out;
        }
        out = utf8(target);
        return out;
    }
    switch (image.i("source"))
    {
    case 4: // generated
        r.skipped.push_back(std::format("image '{}': generated in Blender, not saved to a file", name));
        return out;
    case 3:
    case 2: warnOnce("movie:" + name, std::format("image '{}': a movie or sequence; its first file is used", name)); break;
    default: break;
    }
    fs::path p = file.resolvePath(image.str("name"), image.library());
    std::string s = utf8(p);
    if (const size_t at = s.find("<UDIM>"); at != std::string::npos)
    {
        s.replace(at, 6, "1001");
        warnOnce("udim:" + name, std::format("image '{}': UDIM tiles; only tile 1001 is used", name));
    }
    std::error_code ec;
    if (!fs::exists(fs::path(std::u8string(s.begin(), s.end())), ec))
        r.warnings.push_back(std::format("image '{}': {} not found", name, s));
    out = s;
    return out;
}

std::string Importer::preferredUv(const std::vector<std::string>& slots) const
{
    std::string want;
    for (const std::string& slot : slots)
        if (auto it = materialUv.find(slot); it != materialUv.end())
        {
            if (!want.empty() && want != it->second)
                return {};
            want = it->second;
        }
    return want;
}

std::vector<std::string> Importer::materialSlots(const View& object, const View& data)
{
    const int dataCount = data.i("totcol");
    const int objectCount = object.i("totcol");
    const int count = std::max(dataCount, objectCount);
    std::vector<std::string> out;
    if (count <= 0)
        return out;
    const std::vector<View> dataMats = data.pointerArray("mat", size_t(std::max(0, dataCount)));
    const std::vector<View> objectMats = object.pointerArray("mat", size_t(std::max(0, objectCount)));
    const std::span<const uint8_t> bits = object.raw<uint8_t>("matbits");
    for (int i = 0; i < count; ++i)
    {
        const bool onObject = size_t(i) < bits.size() && bits[size_t(i)] != 0;
        View m = onObject ? (size_t(i) < objectMats.size() ? objectMats[size_t(i)] : View())
                          : (size_t(i) < dataMats.size() ? dataMats[size_t(i)] : View());
        out.push_back(m ? material(m) : std::string(materials::kDefaultSlot));
    }
    return out;
}

std::string Importer::material(const View& ma)
{
    if (auto it = materialNames.find(ma.data()); it != materialNames.end())
        return it->second;
    const std::string wanted = nameOf(ma);
    std::string& slot = materialNames[ma.data()];
    if (!opts.materials || !scene::types::find("material"))
        return slot = wanted;
    scene::Node* n = materials::create(g, wanted);
    slot = n->name();
    r.materials.push_back(slot);
    const NodeId id = n->id();
    auto set = [&](int attr, scene::PlugValue v) { g.setValue({id, attr, -1}, std::move(v)); };

    // Without nodes (or with no Principled BSDF to read): the material's viewport colour.
    set(kBaseColor, glm::dvec3(ma.num("r", 0, 0.8), ma.num("g", 0, 0.8), ma.num("b", 0, 0.8)));
    set(kMetallic, std::clamp(ma.num("metallic"), 0.0, 1.0));
    set(kRoughness, std::clamp(ma.num("roughness", 0, 0.5), 0.0, 1.0));
    const View nt = ma.deref("nodetree");
    if (!nt || (file.version() < 500 && ma.has("use_nodes") && !ma.i("use_nodes")))
        return slot;

    Shaders shaders;
    const Shaders::Context top = {{&shaders.tree(nt), View()}};
    View output;
    for (const View& node : top.front().tree->nodes)
        if (Shaders::idname(node) == "ShaderNodeOutputMaterial" && (!output || (node.i("flag") & (1 << 6))))
            output = node;
    if (!output)
        return slot;

    // The BSDF the surface shows: a Principled BSDF, looked for through mix and add shaders and node groups.
    std::vector<std::string> notes;
    struct Found
    {
        View node;
        Shaders::Context ctx;
    };
    Found bsdf, emission, diffuse, cutout;
    bool transparentMix = false;
    std::function<void(const View&, const Shaders::Context&, int)> walk = [&](const View& in, const Shaders::Context& ctx, int depth) {
        if (depth > 8)
            return;
        const Shaders::Source src = shaders.follow(in, ctx);
        if (!src.node)
            return;
        const std::string kind = Shaders::idname(src.node);
        if (kind == "ShaderNodeBsdfPrincipled" && !bsdf.node)
            bsdf = {src.node, src.ctx};
        else if (kind == "ShaderNodeEmission" && !emission.node)
            emission = {src.node, src.ctx};
        else if (kind == "ShaderNodeBsdfDiffuse" && !diffuse.node)
            diffuse = {src.node, src.ctx};
        else if (kind == "ShaderNodeBsdfTransparent")
            transparentMix = true;
        else if (kind == "ShaderNodeMixShader" || kind == "ShaderNodeAddShader")
        {
            std::vector<View> shadersIn;
            for (const View& s : src.node.list("inputs", "bNodeSocket"))
                if (s.str("name") == "Shader" || s.str("identifier").starts_with("Shader"))
                    shadersIn.push_back(s);
            // Mixed with a Transparent BSDF, the factor is the cutout: an image's alpha behind it masks the surface.
            if (kind == "ShaderNodeMixShader" && shadersIn.size() == 2)
                for (const View& s : shadersIn)
                    if (const Shaders::Source t = shaders.follow(s, src.ctx); t.node && Shaders::idname(t.node) == "ShaderNodeBsdfTransparent")
                        cutout = {src.node, src.ctx};
            for (const View& s : shadersIn)
                walk(s, src.ctx, depth + 1);
            if (kind == "ShaderNodeMixShader" && !cutout.node)
                notes.push_back("a Mix Shader (its first BSDF is used)");
        }
        else
        {
            // Shader to RGB toon setups and the like: the shader behind them.
            bool found = false;
            for (const View& s : src.node.list("inputs", "bNodeSocket"))
                if (const Shaders::Source t = shaders.follow(s, src.ctx); t.node)
                {
                    const size_t before = notes.size();
                    walk(s, src.ctx, depth + 1);
                    found = found || bsdf.node || emission.node || diffuse.node;
                    notes.resize(std::min(notes.size(), before));
                }
            if (!found)
                notes.push_back(std::format("a {} surface", kind));
        }
    };
    walk(Shaders::input(output, "Surface"), top, 0);

    // Nothing on the surface, or only a Transparent BSDF (volume-only boxes, holdout cards): Blender draws nothing there.
    if (!bsdf.node && !emission.node && !diffuse.node)
    {
        const Shaders::Source volume = shaders.follow(Shaders::input(output, "Volume"), top);
        const bool volumeOnly = !shaders.follow(Shaders::input(output, "Surface"), top).node && volume.node;
        if (volumeOnly && importVolume(volume.node, volume.ctx, ma.idName(), set, shaders, r))
            return slot;
        if (transparentMix || volumeOnly)
        {
            set(kOpacity, 0.0);
            set(kBlendMode, int64_t(BlendMode::Masked));
            invisibleMaterials.insert(slot);
            r.skipped.push_back(std::format("material '{}': {}; it draws nothing", ma.idName(), volumeOnly ? "a volume shader Lila has no match for" : "only a Transparent BSDF"));
            return slot;
        }
    }

    // The value an input of a found node shows when nothing feeds it (from the group node it came through, if any).
    auto valueSocket = [&](const Found& f, std::string_view name) {
        const View in = Shaders::input(f.node, name);
        const Shaders::Source src = shaders.follow(in, f.ctx);
        return src.unlinked ? src.unlinked : in;
    };

    // An image a socket reads, directly or through the nodes that only pass it on.
    struct Map
    {
        View image;
        View node;
        int channel = -1; // separate color / RGB output: 0 r, 1 g, 2 b
        bool alpha = false;
        std::string uv; // the UV map a UV Map node names, empty for the default
    };
    // A mix's own layer (its A input) before what it mixes in, and factors last: the base image is the one that shows.
    auto colourInputsFirst = [](const View& node) {
        std::vector<View> ins = node.list("inputs", "bNodeSocket");
        auto rank = [](const View& sock) {
            const std::string n = sock.str("name");
            return n == "A" || n == "Color1" ? 0 : n == "Fac" || n == "Factor" ? 2 : 1;
        };
        std::stable_sort(ins.begin(), ins.end(), [&](const View& a, const View& b) { return rank(a) < rank(b); });
        return ins;
    };
    // An Image Texture node: its image, and the UV map a UV Map node feeds it.
    auto imageOf = [&](const Shaders::Source& tex, Map& out) {
        out.image = tex.node.deref("id");
        out.node = tex.node;
        out.alpha = tex.socket && tex.socket.str("name") == "Alpha";
        if (const Shaders::Source vec = shaders.follow(Shaders::input(tex.node, "Vector"), tex.ctx); vec.node)
        {
            const std::string v = Shaders::idname(vec.node);
            if (v == "ShaderNodeUVMap")
            {
                if (const View storage = vec.node.deref("storage"))
                    out.uv = storage.str("uv_map");
            }
            else if (v != "ShaderNodeTexCoord")
                notes.push_back(std::format("texture coordinates from a {}", v));
        }
    };
    auto mapOf = [&](const Found& f, std::string_view inputName, const char* what) -> Map {
        Map out;
        Shaders::Source src = shaders.follow(Shaders::input(f.node, inputName), f.ctx);
        for (int guard = 0; src.node && guard < 8; ++guard)
        {
            const std::string kind = Shaders::idname(src.node);
            if (kind == "ShaderNodeTexImage")
            {
                imageOf(src, out);
                return out;
            }
            if (kind == "ShaderNodeSeparateColor" || kind == "ShaderNodeSeparateRGB" || kind == "ShaderNodeSeparateXYZ")
            {
                const std::string n = src.socket ? src.socket.str("name") : std::string();
                out.channel = n == "Red" || n == "R" || n == "X" ? 0 : n == "Green" || n == "G" || n == "Y" ? 1 : 2;
                src = shaders.follow(Shaders::input(src.node, kind == "ShaderNodeSeparateXYZ" ? "Vector" : kind == "ShaderNodeSeparateRGB" ? "Image" : "Color"), src.ctx);
                continue;
            }
            if (kind == "ShaderNodeNormalMap")
            {
                const Shaders::Source strength = shaders.follow(Shaders::input(src.node, "Strength"), src.ctx);
                if (std::abs(socketFloat(strength.unlinked, 1.0) - 1.0) > 1e-3)
                    notes.push_back("normal map strength other than 1");
                src = shaders.follow(Shaders::input(src.node, "Color"), src.ctx);
                continue;
            }
            // A colour mix showing its base layer is the usual tint or overlay setup; anything else is worth a note.
            if (kind != "ShaderNodeMix" && kind != "ShaderNodeMixRGB")
                notes.push_back(std::format("{} through a {} node (the first image behind it is used as is)", what, kind));
            // Mix, math, ramps and the like: the nearest image behind them, breadth first.
            std::vector<Shaders::Source> level = {src};
            for (int depth = 0; depth < 4 && !level.empty(); ++depth)
            {
                std::vector<Shaders::Source> next;
                for (const Shaders::Source& at : level)
                    for (const View& in : colourInputsFirst(at.node))
                        if (Shaders::Source s2 = shaders.follow(in, at.ctx); s2.node)
                        {
                            if (Shaders::idname(s2.node) == "ShaderNodeTexImage")
                            {
                                imageOf(s2, out);
                                return out;
                            }
                            next.push_back(std::move(s2));
                        }
                level = std::move(next);
            }
            return {};
        }
        return out;
    };
    auto sampling = [&](const Map& m, int attr) {
        TextureSampling s;
        if (m.image)
            if (const View cs = m.image.sub("colorspace_settings"))
                if (const std::optional<bool> srgb = materials::srgbFromName(cs.str("name")))
                    s.colorSpace = materials::colorSpaceFor(attr, *srgb);
        if (const View storage = m.node ? m.node.deref("storage") : View())
        {
            const int ext = storage.i("extension");
            s.wrapU = s.wrapV = ext == 1 ? TextureWrap::Clamp : ext == 2 ? TextureWrap::Black : ext == 3 ? TextureWrap::Mirror : TextureWrap::Repeat;
            s.filter = storage.i("interpolation") == 1 ? TextureFilter::Nearest : TextureFilter::Linear;
        }
        return s;
    };
    auto setMap = [&](int attr, const Map& m) {
        const std::string p = imagePath(m.image);
        if (p.empty())
            return false;
        if (!m.uv.empty() && !materialUv.count(slot))
            materialUv[slot] = m.uv;
        set(attr, p);
        if (const TextureSampling s = sampling(m, attr); !s.isDefault())
            materials::setSampling(g, id, attr, s);
        return true;
    };

    if (bsdf.node)
    {
        const glm::dvec4 base = socketColor(valueSocket(bsdf, "Base Color"), glm::dvec4(0.8, 0.8, 0.8, 1.0));
        set(kBaseColor, glm::dvec3(base));
        const Map baseMap = mapOf(bsdf, "Base Color", "base color");
        if (baseMap.image)
            setMap(kBaseColorMap, baseMap);

        set(kMetallic, std::clamp(socketFloat(valueSocket(bsdf, "Metallic"), 0.0), 0.0, 1.0));
        set(kRoughness, std::clamp(socketFloat(valueSocket(bsdf, "Roughness"), 0.5), 0.0, 1.0));
        const Map metal = mapOf(bsdf, "Metallic", "metallic"), rough = mapOf(bsdf, "Roughness", "roughness");
        // One image with roughness in green and metallic in blue is glTF's packing: Lila's ORM.
        if (metal.image && rough.image && metal.image.data() == rough.image.data() && metal.channel == 2 && rough.channel == 1)
        {
            setMap(kOrmMap, rough);
            set(kPackedLayout, int64_t(PackedLayout::ORM));
        }
        else
        {
            if (metal.image)
                setMap(kMetallicMap, metal);
            if (rough.image)
                setMap(kRoughnessMap, rough);
            if ((metal.image && metal.channel > 0) || (rough.image && rough.channel > 0))
                notes.push_back("a single channel of a packed texture read as a grey map");
        }

        const Map normal = mapOf(bsdf, "Normal", "normal");
        if (normal.image && setMap(kNormalMap, normal))
            set(kNormalFormat, int64_t(NormalFormat::OpenGL));

        const char* emitName = Shaders::input(bsdf.node, "Emission Color") ? "Emission Color" : "Emission";
        const double strength = socketFloat(valueSocket(bsdf, "Emission Strength"), 1.0);
        const glm::dvec4 emit = socketColor(valueSocket(bsdf, emitName), glm::dvec4(0.0));
        const Map emitMap = mapOf(bsdf, emitName, "emission");
        if (emitMap.image && strength > 0.0)
        {
            setMap(kEmissiveMap, emitMap);
            set(kEmissiveIntensity, strength);
        }
        else if (strength > 0.0 && glm::dvec3(emit) != glm::dvec3(0.0))
        {
            set(kEmissiveColor, glm::dvec3(emit));
            set(kEmissiveIntensity, strength);
        }

        const double alpha = socketFloat(valueSocket(bsdf, "Alpha"), 1.0);
        const Map alphaMap = mapOf(bsdf, "Alpha", "alpha");
        const bool blended = ma.i("surface_render_method") == 1 || ma.i("blend_method") == 5;
        if (alphaMap.image || alpha < 1.0 || transparentMix)
        {
            set(kOpacity, alpha);
            if (alphaMap.image && !(baseMap.image && baseMap.image.data() == alphaMap.image.data() && alphaMap.alpha))
                notes.push_back("alpha from an image other than the base color's alpha");
            if (blended)
                set(kBlendMode, int64_t(BlendMode::Translucent));
            else
            {
                set(kBlendMode, int64_t(BlendMode::Masked));
                set(kOpacityMaskClip, ma.has("alpha_threshold") ? ma.num("alpha_threshold", 0, 0.5) : 0.5);
            }
        }

        const double ior = socketFloat(valueSocket(bsdf, "IOR"), 1.5);
        const double level = socketFloat(valueSocket(bsdf, "Specular IOR Level"), 0.5);
        const double f0 = std::pow((ior - 1.0) / (ior + 1.0), 2.0) * level * 2.0;
        set(kSpecular, std::clamp(f0 / 0.08, 0.0, 1.0));

        const double coat = socketFloat(valueSocket(bsdf, "Coat Weight"), 0.0);
        const double sheen = socketFloat(valueSocket(bsdf, "Sheen Weight"), 0.0);
        const double sss = socketFloat(valueSocket(bsdf, "Subsurface Weight"), 0.0);
        const double transmission = socketFloat(valueSocket(bsdf, "Transmission Weight"), 0.0);
        if (coat > 0.0)
        {
            set(kShadingModel, int64_t(ShadingModel::Clearcoat));
            set(kClearcoat, coat);
            set(kClearcoatRoughness, socketFloat(valueSocket(bsdf, "Coat Roughness"), 0.03));
        }
        else if (sss > 0.0)
        {
            set(kShadingModel, int64_t(ShadingModel::Skin));
            set(kSubsurfaceAmount, sss);
            set(kSubsurfaceColor, glm::dvec3(base));
        }
        else if (sheen > 0.0)
        {
            set(kShadingModel, int64_t(ShadingModel::Cloth));
            set(kSheenColor, glm::dvec3(socketColor(valueSocket(bsdf, "Sheen Tint"), glm::dvec4(1.0))));
            set(kSheenRoughness, socketFloat(valueSocket(bsdf, "Sheen Roughness"), 0.5));
        }
        if (transmission > 0.0)
            notes.push_back("transmission (glass)");
    }
    else if (diffuse.node)
    {
        set(kBaseColor, glm::dvec3(socketColor(valueSocket(diffuse, "Color"), glm::dvec4(0.8))));
        if (const Map m = mapOf(diffuse, "Color", "color"); m.image)
            setMap(kBaseColorMap, m);
        set(kMetallic, 0.0);
        set(kRoughness, std::clamp(socketFloat(valueSocket(diffuse, "Roughness"), 0.5), 0.0, 1.0));
    }
    if (emission.node && !bsdf.node)
    {
        const double strength = socketFloat(valueSocket(emission, "Strength"), 1.0);
        const Map m = mapOf(emission, "Color", "emission");
        const glm::dvec3 color(socketColor(valueSocket(emission, "Color"), glm::dvec4(1.0)));
        if (!diffuse.node)
        {
            // An emission-only surface is a flat, unlit colour: Unlit shows the base colour as is.
            set(kShadingModel, int64_t(ShadingModel::Unlit));
            set(kBaseColor, m.image ? glm::dvec3(1.0) : color * strength);
            if (m.image)
                setMap(kBaseColorMap, m);
        }
        else
        {
            if (m.image)
                setMap(kEmissiveMap, m);
            else
                set(kEmissiveColor, color);
            set(kEmissiveIntensity, strength);
        }
    }
    if (cutout.node && (bsdf.node || emission.node || diffuse.node))
    {
        const Shaders::Source fac = shaders.follow(Shaders::input(cutout.node, "Fac"), cutout.ctx);
        const Shaders::Source facAlt = fac.node || fac.unlinked ? fac : shaders.follow(Shaders::input(cutout.node, "Factor"), cutout.ctx);
        if (facAlt.node || socketFloat(facAlt.unlinked, 1.0) < 1.0)
        {
            set(kBlendMode, int64_t(BlendMode::Masked));
            set(kOpacityMaskClip, 0.5);
            set(kTwoSided, true);
        }
    }
    if (ma.has("blend_flag") && !(ma.i("blend_flag") & (1 << 0)))
        set(kTwoSided, true);

    std::sort(notes.begin(), notes.end());
    notes.erase(std::unique(notes.begin(), notes.end()), notes.end());
    if (!notes.empty())
    {
        std::string list;
        for (const std::string& note : notes)
            list += (list.empty() ? "" : "; ") + note;
        r.warnings.push_back(std::format("material '{}': approximated; not converted: {}", ma.idName(), list));
    }
    return slot;
}
} // namespace lila::blend
