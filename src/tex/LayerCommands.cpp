#include "tex/Generators.h"
#include "tex/Layers.h"
#include "tex/MaterialFromMaps.h"
#include "tex/SmartMaterial.h"

#include "bake/Bake.h"
#include "core/ImageIO.h"
#include "core/Log.h"
#include "tex/TextureSet.h"
#include "tex/Plan.h"
#include "core/PackedArray.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "gn/Group.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "tex/NodeDef.h"
#include "tex/PaintStrokes.h"
#include "tex/Ports.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <set>

namespace lila::tex
{
namespace
{
using scene::Node;

scene::Graph& scene() { return scene::Graph::current(); }

std::string positional(const Args& a, size_t i, const char* cmd, const char* what)
{
    if (a.positional.size() <= i || !a.positional[i].is_string())
        throw CommandError(std::string(cmd) + ": " + what + " required");
    return a.positional[i].get<std::string>();
}

/** A set's stack opened for a command: edits go to stack, commit compiles it into the set's graph. */
struct Session
{
    const char* cmd;
    Node* set = nullptr;
    Graph graph;
    SetSettings settings;
    StackState state;

    LayerStack& stack() { return state.stack; }

    /** A fresh layer or mask id. Paint left under an id (a layer undone or removed keeps its buffer) never passes to a new one. */
    std::string newId(char prefix)
    {
        std::set<std::string> painted;
        if (set)
            if (paint::PaintPtr p = paint::readPaint(*set))
                for (const auto& [id, b] : p->buffers)
                    painted.insert(id);
        return state.stack.newId(prefix, painted);
    }

    Layer& layer(const std::string& id)
    {
        Layer* l = state.stack.find(id);
        if (!l)
            throw CommandError(std::string(cmd) + ": no layer " + id);
        return *l;
    }

    json commit()
    {
        StackCompile c;
        try
        {
            c = compileStack(state.stack, settings, graph);
        }
        catch (const std::exception& e)
        {
            throw CommandError(std::string(cmd) + ": " + e.what());
        }
        writeGraph(scene(), *set, c.graph);
        graph = std::move(c.graph);
        return c.warnings;
    }
};

Session open(const Args& a, const char* cmd, bool edit = true)
{
    Session s{cmd};
    const std::string name = positional(a, 0, cmd, "texture set");
    s.set = findSet(scene(), name);
    if (!s.set)
        throw CommandError(std::string(cmd) + ": no texture set named '" + name + "'");
    try
    {
        s.graph = readGraph(*s.set);
        s.settings = readSettings(*s.set);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
    s.state = readStack(s.graph, s.settings);
    if (!edit || !s.state.graphMode)
        return s;
    if (s.state.edited)
        throw CommandError(std::string(cmd) + ": " + name +
                           " was edited as nodes in a way its layer stack can't show; texRebuildLayers rebuilds the graph from the stack "
                           "(dropping those edits)");
    // A graph made by hand joins the stack as its bottom layer.
    json base = json::object();
    Layer l = graphAsLayer(s.graph, s.settings, &base);
    l.id = s.newId('L');
    s.state.stack.layers.insert(s.state.stack.layers.begin(), std::move(l));
    s.state.stack.base = base;
    s.state.graphMode = false;
    s.state.layered = true;
    return s;
}

json flagJson(const Args& a, const char* flag, json fallback)
{
    return a.has(flag) ? a.flags[flag] : fallback;
}

bool contains(const Layer& l, const std::string& id)
{
    if (l.id == id)
        return true;
    return std::any_of(l.children.begin(), l.children.end(), [&](const Layer& c) { return contains(c, id); });
}

/** Where a layer goes: right above another, inside a folder (on top, or at an index), or at an index in the root. */
void insertLayer(Session& s, Layer l, const Args& a)
{
    LayerStack& st = s.stack();
    if (a.has("above") || a.has("below"))
    {
        const std::string ref = a.get<std::string>(a.has("above") ? "above" : "below");
        size_t i = 0;
        std::vector<Layer>* list = st.listOf(ref, &i);
        if (!list)
            throw CommandError(std::string(s.cmd) + ": no layer " + ref);
        list->insert(list->begin() + std::ptrdiff_t(a.has("above") ? i + 1 : i), std::move(l));
        return;
    }
    std::vector<Layer>* list = &st.layers;
    if (a.has("parent"))
    {
        Layer& folder = s.layer(a.get<std::string>("parent"));
        if (folder.kind != LayerKind::Folder)
            throw CommandError(std::string(s.cmd) + ": " + folder.id + " isn't a folder");
        list = &folder.children;
    }
    const int index = a.get<int>("index", -1);
    if (index < 0 || size_t(index) >= list->size())
        list->push_back(std::move(l));
    else
        list->insert(list->begin() + index, std::move(l));
}

void checkLayer(Session& s, const Layer& l)
{
    const std::string cmd = s.cmd;
    switch (l.kind)
    {
    case LayerKind::Instance:
    {
        const Layer* src = s.stack().find(l.source);
        if (!src)
            throw CommandError(cmd + ": an instance needs a source layer (no layer '" + l.source + "')");
        if (src->kind == LayerKind::Folder || src->kind == LayerKind::Instance || src->id == l.id)
            throw CommandError(cmd + ": an instance's source must be a fill, paint, procedural or filter layer");
        break;
    }
    case LayerKind::Filter:
    {
        if (!findNode(l.filter))
            throw CommandError(cmd + ": unknown filter node '" + l.filter + "'");
        Graph probe;
        NodePorts p = portsOf(probe, GNode{"F", l.filter, {}, l.filterValues});
        if (!p.error.empty() || p.inputs.empty() || p.outputs.empty())
            throw CommandError(cmd + ": " + l.filter + " can't be a filter layer (it needs an input and an output)");
        break;
    }
    case LayerKind::Procedural:
        if (!l.graph.is_null())
        {
            try
            {
                Graph::fromJson(l.graph);
            }
            catch (const std::exception& e)
            {
                throw CommandError(cmd + ": " + e.what());
            }
        }
        break;
    default: break;
    }
    const auto anchors = s.stack().anchors();
    if (!l.anchor.empty())
        if (auto it = anchors.find(l.anchor); it != anchors.end() && it->second != l.id)
            throw CommandError(cmd + ": an anchor named " + l.anchor + " is already on " + it->second);
}

void checkMask(Session& s, const Mask& m)
{
    const std::string cmd = s.cmd;
    switch (m.kind)
    {
    case MaskKind::Generator:
    case MaskKind::Filter:
    {
        const NodeDef* d = findNode(m.type);
        if (!d)
            throw CommandError(cmd + ": unknown node type '" + m.type + "'");
        Graph probe;
        NodePorts p = portsOf(probe, GNode{"M", m.type, {}, m.values});
        if (!p.error.empty() || p.outputs.empty() || (m.kind == MaskKind::Filter && p.inputs.empty()))
            throw CommandError(cmd + ": " + m.type + " can't be a " + maskKindName(m.kind) + " mask");
        for (const auto& [port, role] : m.bindings)
        {
            if (!p.input(port))
                throw CommandError(cmd + ": " + m.type + " has no input '" + port + "'");
            if (!role.empty() && role != "none" && !role.starts_with("anchor:") && !findMeshRole(role))
                throw CommandError(cmd + ": '" + role + "' is no mesh map (curvature, ao, thickness, cavity, height, position, worldNormal, id, "
                                                       "uvIslands, vertexColor) or anchor:<name>.<channel>");
        }
        break;
    }
    case MaskKind::Anchor:
        if (!s.stack().anchors().contains(m.anchor))
            throw CommandError(cmd + ": no anchor named '" + m.anchor + "'");
        if (!s.settings.channel(m.channel))
            throw CommandError(cmd + ": the set has no channel " + m.channel);
        break;
    default: break;
    }
}

json defaultMaterial(const SetSettings& s)
{
    json m = json::object();
    for (const char* c : {"baseColor", "roughness", "metallic", "height"})
        if (const Channel* ch = s.channel(c))
            m[c] = channelDefault(c, ch->type);
    return m;
}

json result(json value, const json& warnings)
{
    json out = {{"id", std::move(value)}};
    if (!warnings.empty())
        out["warnings"] = warnings;
    return out;
}

Mask maskFromArgs(Session& s, const Args& a, MaskKind kind, const Layer& l)
{
    Mask m;
    m.kind = kind;
    m.id = s.newId('M');
    m.name = a.get<std::string>("name", "");
    const bool first = std::none_of(l.masks.begin(), l.masks.end(), [](const Mask& x) { return x.enabled; });
    const bool combines = kind == MaskKind::Fill || kind == MaskKind::Generator || kind == MaskKind::Paint || kind == MaskKind::Anchor;
    m.mode = a.get<std::string>("mode", combines && !first ? "multiply" : "normal");
    if (std::find(maskBlendModes().begin(), maskBlendModes().end(), m.mode) == maskBlendModes().end())
        throw CommandError(std::string(s.cmd) + ": unknown mask mode " + m.mode);
    m.opacity = a.get<double>("opacity", 1.0);
    m.invert = a.get<bool>("invert", false);
    m.value = a.get<double>("value", 1.0);
    m.type = a.get<std::string>("type", "");
    m.values = flagJson(a, "values", json::object());
    if (!m.values.is_object())
        throw CommandError(std::string(s.cmd) + ": values must be a dict");
    if (a.has("bindings"))
        m.bindings = a.flags["bindings"].get<std::map<std::string, std::string>>();
    m.image = a.get<std::string>("image", "");
    m.anchor = a.get<std::string>("anchor", "");
    m.channel = a.get<std::string>("channel", "height");
    return m;
}

std::vector<FlagDef> maskFlags()
{
    using T = ArgType;
    return {{"name", "", T::String, nullptr, "Display name."},
            {"mode", "", T::String, nullptr, "How it combines with the masks below: normal (the first default), multiply (later ones), add, subtract, screen, "
                                             "overlay, darken, lighten, difference."},
            {"opacity", "", T::Float, nullptr, "0 to 1.", {}, {}, 0.0, 1.0},
            {"invert", "", T::Bool, nullptr, "Invert it before combining."},
            {"values", "", T::Any, nullptr, "Parameter values by id (generator, filter, levels)."},
            {"bindings", "", T::Any, nullptr, "Generator inputs' sources: {input: mesh map role or 'anchor:<name>.<channel>'}. Unlisted inputs use "
                                              "the role of their own name."},
            {"index", "", T::Int, nullptr, "Position in the mask stack, 0 the bottom; the top when left out.", {}, {}, 0.0}};
}

json addMask(Session& s, Layer& l, Mask m, const Args& a)
{
    checkMask(s, m);
    const int index = a.get<int>("index", -1);
    const std::string id = m.id;
    if (index < 0 || size_t(index) >= l.masks.size())
        l.masks.push_back(std::move(m));
    else
        l.masks.insert(l.masks.begin() + index, std::move(m));
    return result(id, s.commit());
}

/** Applies a JSON merge patch to an object's JSON form; keys can't change ids. */
template <class X> X patched(const X& x, const json& props, const char* cmd)
{
    if (!props.is_object())
        throw CommandError(std::string(cmd) + ": props must be a dict");
    json j = x.toJson();
    const json id = j["id"];
    j.merge_patch(props);
    j["id"] = id;
    try
    {
        return X::fromJson(j);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
}

json applySmart(const Args& a)
{
    Session s = open(a, "texApplySmartMaterial");
    SmartMaterial sm;
    try
    {
        sm = loadSmartMaterial(positional(a, 1, "texApplySmartMaterial", "smart material name or path"));
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string("texApplySmartMaterial: ") + e.what());
    }
    json out = json::object();
    if (sm.kind == "mask")
    {
        if (!a.has("layer"))
            throw CommandError("texApplySmartMaterial: " + sm.name + " is a smart mask; give the layer it goes on (layer=)");
        Layer& l = s.layer(a.get<std::string>("layer"));
        std::vector<Mask> masks = instantiateMasks(sm.masks, s.stack());
        json ids = json::array();
        for (Mask& m : masks)
        {
            checkMask(s, m);
            ids.push_back(m.id);
            l.masks.push_back(std::move(m));
        }
        out["masks"] = ids;
    }
    else
    {
        Layer l;
        try
        {
            l = instantiateLayer(sm.layer, s.stack());
        }
        catch (const std::exception& e)
        {
            throw CommandError(std::string("texApplySmartMaterial: ") + e.what());
        }
        if (a.has("name"))
            l.name = a.get<std::string>("name");
        else if (l.name.empty())
            l.name = sm.name;
        out["layer"] = l.id;
        insertLayer(s, std::move(l), a);
    }
    json warnings = s.commit();
    const std::vector<std::string> maps = externalsUsed(s.graph);
    if (a.get<bool>("addMaps", true))
        out["addedMaps"] = addBakeMaps(scene(), s.settings.bakeProject, maps);
    else
        out["missingMaps"] = missingBakeMaps(scene(), s.settings.bakeProject, maps);
    out["maps"] = maps;
    if (!warnings.empty())
        out["warnings"] = warnings;
    return out;
}

json saveSmart(const Args& a)
{
    Session s = open(a, "texSaveSmartMaterial", false);
    if (s.state.graphMode)
        throw CommandError("texSaveSmartMaterial: the set has no layer stack to save from");
    Layer& l = s.layer(positional(a, 1, "texSaveSmartMaterial", "layer"));
    SmartMaterial sm;
    sm.name = a.get<std::string>("name", l.name.empty() ? l.id : l.name);
    sm.description = a.get<std::string>("description", "");
    if (a.get<bool>("mask", false))
    {
        sm.kind = "mask";
        for (const Mask& m : l.masks)
            sm.masks.push_back(m.toJson());
    }
    else
        sm.layer = l.toJson();
    std::filesystem::path path;
    if (a.positional.size() > 2 && a.positional[2].is_string())
        path = paths::fromUtf8(a.positional[2].get<std::string>());
    else
    {
        std::string file;
        for (char c : sm.name)
            file += std::isalnum(static_cast<unsigned char>(c)) || c == '-' ? c : '_';
        path = PrefStore::get().userDir() / "smartmaterials" / file;
    }
    if (path.extension() != ".lilasmart")
        path += ".lilasmart";
    try
    {
        saveSmartMaterial(sm, path);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string("texSaveSmartMaterial: ") + e.what());
    }
    return paths::toUtf8(path);
}

/** A procedural layer's exposed settings: each param input's default and every node value it targets. Throws CommandError. */
void applyGraphParams(Layer& l, const json& params)
{
    if (!params.is_object())
        throw CommandError("texSetLayer: params is {name: value}");
    if (l.kind != LayerKind::Procedural || !l.graph.is_object())
        throw CommandError("texSetLayer: only procedural layers have params");
    json& inputs = l.graph["inputs"];
    json& nodes = l.graph["nodes"];
    for (const auto& [key, value] : params.items())
    {
        json* in = nullptr;
        if (inputs.is_array())
            for (json& i : inputs)
                if (i.value("name", "") == key && (i.value("kind", "") == "param" || i.contains("targets")))
                    in = &i;
        if (!in)
            throw CommandError("texSetLayer: " + l.id + " has no setting " + key);
        (*in)["default"] = value;
        for (const json& t : in->value("targets", json::array()))
        {
            const std::string target = t.get<std::string>();
            const auto dot = target.find('.');
            if (dot == std::string::npos || !nodes.is_array())
                continue;
            for (json& n : nodes)
                if (n.value("name", "") == target.substr(0, dot))
                    n["values"][target.substr(dot + 1)] = value;
        }
    }
}

// ---------------------------------------------------------------------------------------------------- copy, paste, flatten

/** The paint buffers a layer (and its children) read, by id. */
void paintBuffersOf(const Layer& l, std::set<std::string>& out)
{
    auto take = [&](const json& v) {
        std::string ref = v.is_string() ? v.get<std::string>() : std::string();
        constexpr std::string_view kPrefix = "external:";
        if (ref.starts_with(kPrefix))
            ref = ref.substr(kPrefix.size());
        std::string buffer, channel;
        if (paint::parseExternalName(ref, &buffer, &channel))
            out.insert(buffer);
    };
    if (l.paint.is_object())
        for (const auto& [k, v] : l.paint.items())
            take(v);
    for (const Mask& m : l.masks)
        take(json(m.image));
    for (const Layer& c : l.children)
        paintBuffersOf(c, out);
}

/** Points the layer's (and children's) paint references from old buffer ids to new ones. */
void renamePaint(Layer& l, const std::map<std::string, std::string>& ids)
{
    auto fix = [&](std::string& ref) {
        for (const auto& [from, to] : ids)
            if (ref.find("paint/" + from + "/") != std::string::npos)
                ref.replace(ref.find("paint/" + from + "/"), from.size() + 7, "paint/" + to + "/");
    };
    if (l.paint.is_object())
        for (auto& [k, v] : l.paint.items())
            if (v.is_string())
            {
                std::string s = v.get<std::string>();
                fix(s);
                v = s;
            }
    for (Mask& m : l.masks)
        fix(m.image);
    for (Layer& c : l.children)
        renamePaint(c, ids);
}

/** Packed binary arrays as plain number arrays: text can't hold binary, and the readers take either form. */
void unpackArrays(json& j)
{
    if (packed::is(j))
    {
        const auto& bin = j.get_binary();
        json flat = json::array();
        for (size_t k = 0, n = packed::count(j); k < n; ++k)
            flat.push_back(packed::element(packed::dtype(j), bin.data(), k));
        j = std::move(flat);
    }
    else if (j.is_object() || j.is_array())
        for (json& v : j)
            unpackArrays(v);
}

json copyLayers(const Args& a)
{
    Session s = open(a, "texCopyLayers", false);
    const json ids = a.has("layers") ? a.flags["layers"] : json::array({positional(a, 1, "texCopyLayers", "layer")});
    json layers = json::array();
    std::set<std::string> buffers;
    for (const json& id : ids)
    {
        const Layer& l = s.layer(id.get<std::string>());
        layers.push_back(l.toJson());
        paintBuffersOf(l, buffers);
    }
    const paint::PaintPtr data = paint::readPaint(*s.set);
    auto kept = std::make_shared<paint::PaintData>();
    for (const std::string& b : buffers)
        if (const paint::Buffer* buf = data->buffer(b))
            kept->buffers[b] = *buf;
    kept->images = data->images;
    kept->pruneImages();
    json paintJson = kept->toJson();
    unpackArrays(paintJson);
    const json clip = {{"format", "lilaLayers"}, {"version", 1}, {"layers", layers}, {"paint", paintJson}};
    return clip.dump();
}

json pasteLayers(const Args& a)
{
    Session s = open(a, "texPasteLayers");
    json clip;
    try
    {
        clip = json::parse(positional(a, 1, "texPasteLayers", "clipboard text"));
    }
    catch (const json::exception&)
    {
        throw CommandError("texPasteLayers: the text isn't copied layers");
    }
    if (clip.value("format", std::string()) != "lilaLayers")
        throw CommandError("texPasteLayers: the text isn't copied layers (texCopyLayers makes them)");
    // Paint buffers come along under fresh ids, so the pasted layers paint independently of their originals.
    const scene::DataPtr pd = paint::PaintData::fromJson(clip.value("paint", json::object()));
    const auto* pasted = dynamic_cast<const paint::PaintData*>(pd.get());
    auto data = std::make_shared<paint::PaintData>(*paint::readPaint(*s.set));
    std::map<std::string, std::string> renamed;
    if (pasted)
    {
        for (const auto& [id, buf] : pasted->buffers)
        {
            const std::string fresh = data->uniqueId();
            data->buffers[fresh] = buf;
            renamed[id] = fresh;
        }
        for (const auto& [k, img] : pasted->images)
            data->images[k] = img;
    }
    json out = json::array();
    const json layers = clip.value("layers", json::array());
    for (const json& lj : layers)
    {
        Layer l = instantiateLayer(lj, s.stack());
        renamePaint(l, renamed);
        out.push_back(l.id);
        insertLayer(s, std::move(l), a);
    }
    if (!renamed.empty())
        paint::writePaint(scene(), *s.set, data);
    json r = {{"layers", out}};
    r["warnings"] = s.commit();
    return r;
}

/** The composite of the stack up to and including the top of the given root layers, per channel those layers write, at the first tile. */
std::map<std::string, image::Image> flattenedImages(Session& s, const std::vector<std::string>& ids, std::vector<std::string>* warnings)
{
    if (ids.empty())
        throw CommandError(std::string(s.cmd) + ": give the layers to flatten");
    LayerStack copy = s.stack();
    size_t top = 0;
    std::set<std::string> channels;
    for (const std::string& id : ids)
    {
        size_t at = 0;
        std::vector<Layer>* list = s.stack().listOf(id, &at);
        if (!list)
            throw CommandError(std::string(s.cmd) + ": no layer " + id);
        if (list != &s.stack().layers)
            throw CommandError(std::string(s.cmd) + ": " + id + " is inside a folder; flatten the folder");
        top = std::max(top, at);
        std::function<void(const Layer&)> collect = [&](const Layer& l) {
            for (const Channel& c : s.settings.channels)
                if (l.kind != LayerKind::Folder && channelOn(s.stack(), l, c.name))
                    channels.insert(c.name);
            for (const Layer& c : l.children)
                collect(c);
        };
        collect(*s.stack().find(id));
    }
    for (size_t i = top + 1; i < copy.layers.size(); ++i)
        copy.layers[i].enabled = false;
    const StackCompile c = compileStack(copy, s.settings, s.graph);
    std::vector<std::pair<std::string, PortType>> wanted;
    for (const Channel& ch : s.settings.channels)
        if (channels.contains(ch.name))
            wanted.push_back({ch.name, ch.type});
    if (s.settings.tiles.size() > 1 && warnings)
        warnings->push_back("only the first UDIM tile is flattened; the others keep showing the stack below");
    try
    {
        return evaluateGraphOutputs(*s.set, c.graph, materialRequests(c.graph, wanted), s.settings.tiles.front().udim, 0, 0, {}, warnings);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(s.cmd) + ": " + e.what());
    }
}

std::vector<std::string> idsArg(const Args& a, const char* cmd)
{
    std::vector<std::string> ids;
    if (a.has("layers"))
        for (const json& id : a.flags["layers"])
            ids.push_back(id.get<std::string>());
    else
        ids.push_back(positional(a, 1, cmd, "layer"));
    return ids;
}

/** Writes each image (full coverage) as <folder>/<set>_<prefix>_<channel>.exr; returns the files by channel. */
std::map<std::string, std::string> writeFlattened(const std::map<std::string, image::Image>& images, const std::filesystem::path& folder,
                                                  const std::string& stem)
{
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    std::map<std::string, std::string> files;
    for (const auto& [channel, img] : images)
    {
        std::vector<float> rgba = img.rgba;
        for (size_t i = 3; i < rgba.size(); i += 4)
            rgba[i] = 1.0f;
        const std::filesystem::path f = folder / (stem + "_" + channel + ".exr");
        image::writeExr(f, img.width, img.height, rgba, 32);
        files[channel] = paths::toUtf8(f);
    }
    return files;
}

json flattenLayers(const Args& a)
{
    Session s = open(a, "texFlattenLayers");
    std::vector<std::string> ids = idsArg(a, "texFlattenLayers");
    // Merge Down takes the layer below the one named as well.
    if (a.get<bool>("down", false))
    {
        size_t at = 0;
        std::vector<Layer>* list = s.stack().listOf(ids.front(), &at);
        if (!list || at == 0)
            throw CommandError("texFlattenLayers: nothing below " + ids.front() + " to merge into");
        ids.push_back((*list)[at - 1].id);
    }
    std::vector<std::string> warnings;
    const std::map<std::string, image::Image> images = flattenedImages(s, ids, &warnings);
    Layer merged;
    merged.id = s.newId('L');
    merged.kind = LayerKind::Paint;
    merged.name = a.get<std::string>("name", ids.size() > 1 ? std::string("Merged") : s.layer(ids.front()).name + " flattened");
    const std::filesystem::path folder = bake::resolveFolder(a.get<std::string>("folder", "textures/" + s.set->name() + "/flattened"));
    for (const auto& [channel, file] : writeFlattened(images, folder, s.set->name() + "_" + merged.id))
    {
        merged.paint[channel] = file;
        merged.channels[channel] = ChannelBlend{};
    }
    // The new layer sits where the top one was; the flattened layers go, or stay hidden below it with keep.
    size_t top = 0;
    for (const std::string& id : ids)
    {
        size_t at = 0;
        s.stack().listOf(id, &at);
        top = std::max(top, at);
    }
    std::vector<Layer>& root = s.stack().layers;
    root.insert(root.begin() + std::ptrdiff_t(top + 1), merged);
    const bool keep = a.get<bool>("keep", false);
    for (const std::string& id : ids)
    {
        size_t at = 0;
        std::vector<Layer>* list = s.stack().listOf(id, &at);
        if (!list)
            continue;
        if (keep)
            (*list)[at].enabled = false;
        else
            list->erase(list->begin() + std::ptrdiff_t(at));
    }
    for (const std::string& w : s.commit())
        warnings.push_back(w);
    return {{"layer", merged.id}, {"channels", merged.paint}, {"warnings", warnings}};
}

json exportFlattened(const Args& a)
{
    Session s = open(a, "texExportFlattened", false);
    const std::vector<std::string> ids = idsArg(a, "texExportFlattened");
    std::vector<std::string> warnings;
    const auto images = flattenedImages(s, ids, &warnings);
    const std::filesystem::path folder = bake::resolveFolder(a.get<std::string>("folder", "textures/" + s.set->name()));
    std::map<std::string, std::string> files;
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::string format = a.get<std::string>("format", "png");
    for (const auto& [channel, img] : images)
    {
        const std::filesystem::path f = folder / (s.set->name() + "_" + ids.front() + "_" + channel + "." + format);
        if (format == "exr")
            image::writeExr(f, img.width, img.height, img.rgba, 32);
        else
        {
            // Colour channels go to 8 and 16-bit files as sRGB, data channels as they are.
            const Channel* c = s.settings.channel(channel);
            std::vector<float> rgba = img.rgba;
            if (c && c->type == PortType::Color)
                for (size_t i = 0; i < rgba.size(); ++i)
                    if (i % 4 != 3)
                        rgba[i] = std::clamp(rgba[i], 0.0f, 1.0f) <= 0.0031308f ? rgba[i] * 12.92f
                                                                              : 1.055f * std::pow(std::clamp(rgba[i], 0.0f, 1.0f), 1.0f / 2.4f) - 0.055f;
            image::writePng(f, img.width, img.height, rgba, 16);
        }
        files[channel] = paths::toUtf8(f);
    }
    return {{"files", files}, {"warnings", warnings}};
}

/**
 * Recompiles the stacks of the sets a finished bake feeds: projections take the position map's box and the lows' world
 * matrices at compile, and both change when a moved mesh rebakes.
 */
void recompileAfterBake(const bake::BakeEvent& e)
{
    if (e.kind != bake::BakeEvent::Kind::Finished || e.data.value("cancelled", false) || e.data.contains("error"))
        return;
    scene::Graph& g = scene();
    const Node* project = g.node(e.project);
    if (!project)
        return;
    for (const std::string& name : setNames(g))
    {
        Node* set = findSet(g, name);
        if (!set)
            continue;
        try
        {
            const SetSettings s = readSettings(*set);
            if (s.bakeProject != project->name())
                continue;
            const Graph graph = readGraph(*set);
            const StackState st = readStack(graph, s);
            if (st.graphMode || !st.layered)
                continue;
            const Graph next = compileStack(st.stack, s, graph).graph;
            if (next.toJson() != graph.toJson())
                writeGraph(g, *set, next);
        }
        catch (const std::exception& ex)
        {
            log::warn("{}: couldn't update its layers after {} baked: {}", name, project->name(), ex.what());
        }
    }
}
} // namespace

void registerLayerCommands()
{
    static const int bakeListener = bake::addBakeListener(recompileAfterBake);
    (void)bakeListener;
    maps::registerCommands();
    using T = ArgType;
    ops::registerCommand({
        .name = "texVariants",
        .doc = "A texture set's variants: {active, variants {name: {layer id: override}}}. An override patches the layer's settings "
               "(enabled, opacity, material, params...) and maskOverrides {mask id: patch} its masks.",
        .positionalDoc = "set",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Session s = open(a, "texVariants", false);
            return {{"active", s.stack().variant}, {"variants", s.stack().variants}};
        },
    });
    ops::registerCommand({
        .name = "texAddVariant",
        .doc = "Add a named variant to a texture set and switch to it: from then on layer and mask edits are its overrides, until "
               "texSwitchVariant goes back to the base (name ''). copy= starts it from another variant's overrides.",
        .positionalDoc = "set name",
        .flags = {{"copy", "", T::String, nullptr, "A variant whose overrides it starts with."}},
        .run = [](const Args& a) -> json {
            Session s = open(a, "texAddVariant");
            const std::string name = positional(a, 1, "texAddVariant", "variant name");
            if (s.stack().variants.contains(name))
                throw CommandError("texAddVariant: " + s.set->name() + " already has a variant " + name);
            json start = json::object();
            if (a.has("copy"))
            {
                const std::string from = a.get<std::string>("copy");
                if (!s.stack().variants.contains(from))
                    throw CommandError("texAddVariant: no variant " + from);
                start = s.stack().variants[from];
            }
            s.stack().variants[name] = start;
            s.stack().variant = name;
            s.commit();
            return {{"active", name}};
        },
    });
    ops::registerCommand({
        .name = "texRemoveVariant",
        .doc = "Remove a texture set's variant; the set goes back to the base when it was active.",
        .positionalDoc = "set name",
        .run = [](const Args& a) -> json {
            Session s = open(a, "texRemoveVariant");
            const std::string name = positional(a, 1, "texRemoveVariant", "variant name");
            if (!s.stack().variants.contains(name))
                throw CommandError("texRemoveVariant: no variant " + name);
            s.stack().variants.erase(name);
            if (s.stack().variant == name)
                s.stack().variant.clear();
            s.commit();
            return {{"active", s.stack().variant}};
        },
    });
    ops::registerCommand({
        .name = "texSwitchVariant",
        .doc = "Show a texture set as one of its variants ('' for the base). Layer and mask edits go to the variant shown.",
        .positionalDoc = "set name",
        .run = [](const Args& a) -> json {
            Session s = open(a, "texSwitchVariant");
            const std::string name = a.positional.size() > 1 && a.positional[1].is_string() ? a.positional[1].get<std::string>() : std::string();
            if (!name.empty() && !s.stack().variants.contains(name))
                throw CommandError("texSwitchVariant: no variant " + name);
            s.stack().variant = name;
            s.commit();
            return {{"active", name}};
        },
    });
    ops::registerCommand({
        .name = "texSetVariant",
        .doc = "Set or clear a variant's override for one layer without switching to it: props is merged into the override (as "
               "texSetLayer takes it, plus maskOverrides {mask id: patch}); props null clears the layer's override.",
        .positionalDoc = "set name layer props",
        .run = [](const Args& a) -> json {
            Session s = open(a, "texSetVariant");
            const std::string name = positional(a, 1, "texSetVariant", "variant name");
            const std::string layer = positional(a, 2, "texSetVariant", "layer");
            if (!s.stack().variants.contains(name))
                throw CommandError("texSetVariant: no variant " + name);
            if (!s.stack().find(layer))
                throw CommandError("texSetVariant: no layer " + layer);
            json& v = s.stack().variants[name];
            const json props = a.positional.size() > 3 ? a.positional[3] : json();
            if (props.is_null())
                v.erase(layer);
            else if (!props.is_object())
                throw CommandError("texSetVariant: props must be a dict or null");
            else
                mergeOverride(v[layer], props);
            LayerStack probe = s.stack();
            probe.variant = name;
            try
            {
                (void)withVariant(probe);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("texSetVariant: ") + e.what());
            }
            s.commit();
            return v.contains(layer) ? v[layer] : json();
        },
    });
    ops::registerCommand({
        .name = "texLayerStack",
        .doc = "A texture set's layer stack as JSON: layers bottom first (with each channel's resolved enable, mode and opacity), masks, anchors, "
               "the base values, the mesh maps it reads, and mode: stack, or graph when the graph was edited as nodes beyond what the stack "
               "can show (or was never layered).",
        .positionalDoc = "set",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Session s = open(a, "texLayerStack", false);
            return stackView(s.state, s.settings, s.graph);
        },
    });

    ops::registerCommand({
        .name = "texRebuildLayers",
        .doc = "Rebuild a texture set's graph from its layer stack, dropping node edits the stack can't show.",
        .positionalDoc = "set",
        .run = [](const Args& a) -> json {
            Session s = open(a, "texRebuildLayers", false);
            if (s.state.graphMode && !s.state.edited)
                throw CommandError("texRebuildLayers: the set's graph was never a layer stack");
            return s.commit();
        },
    });

    ops::registerCommand({
        .name = "texAddLayer",
        .doc = "Add a layer to a texture set's stack (on top unless placed). Kinds: fill (material + mask), paint (placeholder material, paint "
               "buffers and a painted mask), procedural (a graph with outputs named after channels, optionally mask), folder, instance (another "
               "layer's values and mask), filter (a filter node applied to the composite below), decal (pictures per channel placed by "
               "projection), text (words drawn in the material's base colour, placed like a decal) and scatter (copies of a pattern "
               "over the uvs). A graph made by hand joins the stack as its bottom layer. Returns {id}.",
        .positionalDoc = "set kind",
        .flags = {{"name", "", T::String, nullptr, "Display name."},
                  {"above", "", T::String, nullptr, "Insert right above this layer (in its folder)."},
                  {"below", "", T::String, nullptr, "Insert right below this layer."},
                  {"parent", "", T::String, nullptr, "Folder to put it in (on top)."},
                  {"index", "", T::Int, nullptr, "Position in the root or parent, 0 the bottom.", {}, {}, 0.0},
                  {"material", "", T::Any, nullptr, "Fill/paint channel values {channel: value}; replaces the default material."},
                  {"graph", "", T::Any, nullptr, "Procedural: the graph."},
                  {"source", "", T::String, nullptr, "Instance: the source layer."},
                  {"filter", "", T::String, nullptr, "Filter: the node type (blur, hsv, levels...)."},
                  {"values", "", T::Any, nullptr, "Filter: its parameter values."},
                  {"channels", "", T::Any, nullptr, "Per-channel {enabled, mode, opacity}."},
                  {"projection", "", T::Any, nullptr, "Fill, procedural, decal and text: layerProjection's values {mode, positionX...} in the object space of mesh (a node path; the bake's first low when unset)."},
                  {"decal", "", T::Any, nullptr, "Decal: {maps {channel: file}, columns, rows, cell}."},
                  {"text", "", T::Any, nullptr, "Text: {text, font, tracking, outline, outlineColor, resolution}."},
                  {"scatter", "", T::Any, nullptr, "Scatter: {pattern, maps, countX, countY, perCell, size, sizeRandom, rotationRandom, hueRandom, seed, avoidOverlap}."}},
        .run = [](const Args& a) -> json {
            Session s = open(a, "texAddLayer");
            Layer l;
            if (!parseLayerKind(positional(a, 1, "texAddLayer", "kind"), &l.kind))
                throw CommandError("texAddLayer: kind is fill, paint, procedural, folder, instance, filter, decal, text or scatter");
            l.id = s.newId('L');
            l.name = a.get<std::string>("name", std::string(layerKindName(l.kind)) + " " + l.id.substr(1));
            if (l.kind == LayerKind::Fill || l.kind == LayerKind::Paint)
                l.material = flagJson(a, "material", defaultMaterial(s.settings));
            else if (l.kind == LayerKind::Decal || l.kind == LayerKind::Text || l.kind == LayerKind::Scatter)
                l.material = flagJson(a, "material", json::object());
            l.projection = flagJson(a, "projection", json::object());
            l.decal = flagJson(a, "decal", json::object());
            l.text = flagJson(a, "text", json{{"text", "Text"}});
            l.scatter = flagJson(a, "scatter", json::object());
            if (!l.material.is_object())
                throw CommandError("texAddLayer: material must be a dict");
            l.graph = flagJson(a, "graph", json());
            l.source = a.get<std::string>("source", "");
            l.filter = a.get<std::string>("filter", "");
            l.filterValues = flagJson(a, "values", json::object());
            if (a.has("channels"))
                l = patched(l, json{{"channels", a.flags["channels"]}}, "texAddLayer");
            // A paint layer points every channel and its mask at its buffers from the start, so a stroke only changes
            // pixels: wiring them on the first stroke would change the graph and re-evaluate the whole stack.
            if (l.kind == LayerKind::Paint)
            {
                for (const Channel& c : s.settings.channels)
                    if (!l.paint.contains(c.name))
                        l.paint[c.name] = paint::imageRef(l.id, c.name);
                if (!l.paint.contains(paint::kMaskChannel))
                    l.paint[paint::kMaskChannel] = paint::imageRef(l.id, paint::kMaskChannel);
            }
            checkLayer(s, l);
            const std::string id = l.id;
            insertLayer(s, std::move(l), a);
            return result(id, s.commit());
        },
    });

    ops::registerCommand({
        .name = "texRemoveLayer",
        .doc = "Remove layers (folders with their contents) from a texture set's stack.",
        .positionalDoc = "set layer...",
        .run = [](const Args& a) -> json {
            Session s = open(a, "texRemoveLayer");
            for (size_t i = 1; i < a.positional.size(); ++i)
            {
                const std::string id = a.positional[i].get<std::string>();
                size_t at = 0;
                std::vector<Layer>* list = s.stack().listOf(id, &at);
                if (!list)
                    throw CommandError("texRemoveLayer: no layer " + id);
                const Layer& gone = (*list)[at];
                std::function<void(const std::vector<Layer>&)> check = [&](const std::vector<Layer>& ls) {
                    for (const Layer& x : ls)
                    {
                        if (x.kind == LayerKind::Instance && contains(gone, x.source) && !contains(gone, x.id))
                            throw CommandError("texRemoveLayer: " + x.id + " is an instance of " + x.source + "; remove or repoint it first");
                        check(x.children);
                    }
                };
                check(s.stack().layers);
                list->erase(list->begin() + std::ptrdiff_t(at));
            }
            return s.commit();
        },
    });

    ops::registerCommand({
        .name = "texMoveLayer",
        .doc = "Move a layer in a texture set's stack: right above or below another layer, into a folder, or to an index.",
        .positionalDoc = "set layer",
        .flags = {{"above", "", T::String, nullptr, "Right above this layer."},
                  {"below", "", T::String, nullptr, "Right below this layer."},
                  {"parent", "", T::String, nullptr, "Into this folder (on top, or at index)."},
                  {"index", "", T::Int, nullptr, "Position in the root or parent, 0 the bottom.", {}, {}, 0.0}},
        .run = [](const Args& a) -> json {
            Session s = open(a, "texMoveLayer");
            const std::string id = positional(a, 1, "texMoveLayer", "layer");
            size_t at = 0;
            std::vector<Layer>* list = s.stack().listOf(id, &at);
            if (!list)
                throw CommandError("texMoveLayer: no layer " + id);
            for (const char* f : {"above", "below", "parent"})
                if (a.has(f) && contains((*list)[at], a.get<std::string>(f)))
                    throw CommandError("texMoveLayer: can't move a layer into or next to itself");
            Layer l = std::move((*list)[at]);
            list->erase(list->begin() + std::ptrdiff_t(at));
            insertLayer(s, std::move(l), a);
            return s.commit();
        },
    });

    ops::registerCommand({
        .name = "texSetLayer",
        .doc = "Change a layer: props is a merge patch over its JSON (texLayerStack shows it): name, enabled, channels {channel: {enabled, mode, "
               "opacity}}, material {channel: value, or null to drop it}, paint {channel or mask: image}, graph, source, filter, filterValues, "
               "anchor, heightBlend {enabled, sharpness, influence}, tiling {scale, rotation, offsetX, offsetY}, masks, kind; params {name: value} "
               "sets a procedural layer's exposed settings (its graph's param inputs and the node values they target). Returns the layer.",
        .positionalDoc = "set layer props",
        .run = [](const Args& a) -> json {
            Session s = open(a, "texSetLayer");
            Layer& l = s.layer(positional(a, 1, "texSetLayer", "layer"));
            if (a.positional.size() < 3)
                throw CommandError("texSetLayer: props required");
            // With a variant active the edit is that variant's override; the base layer stays as it is.
            const std::string variant = s.stack().variant;
            const Layer* shown = &l;
            LayerStack resolved;
            if (!variant.empty())
            {
                resolved = withVariant(s.stack());
                shown = resolved.find(l.id);
            }
            Layer next = patched(*shown, a.positional[2], "texSetLayer");
            const json& props = a.positional[2];
            if (props.is_object() && props.contains("params"))
                applyGraphParams(next, props["params"]);
            if (next.kind != LayerKind::Folder && !next.children.empty())
                throw CommandError("texSetLayer: only folders have children");
            checkLayer(s, next);
            for (const Mask& m : next.masks)
                checkMask(s, m);
            json out = next.toJson();
            if (!variant.empty())
            {
                json patch = props;
                if (patch.contains("params"))
                {
                    patch.erase("params");
                    patch["graph"] = out["graph"];
                }
                patch.erase("id");
                mergeOverride(s.stack().variants[variant][l.id], patch);
            }
            else
                l = std::move(next);
            s.commit();
            return out;
        },
    });

    {
        std::vector<FlagDef> flags = maskFlags();
        flags.push_back({"type", "", T::String, nullptr, "Generator or filter: the node type."});
        flags.push_back({"value", "", T::Float, nullptr, "Fill: the value (default 1)."});
        flags.push_back({"image", "", T::String, nullptr, "Paint: the image."});
        flags.push_back({"anchor", "", T::String, nullptr, "Anchor: its name."});
        flags.push_back({"channel", "", T::String, nullptr, "Anchor: the channel read (default height)."});
        ops::registerCommand({
            .name = "texAddMask",
            .doc = "Add a mask to a layer's mask stack. Kinds: fill (a value), generator (a mask graph over mesh maps), filter (any filter node over the "
                   "masks below), levels, paint (an image), anchor (a channel of the composite at an anchor). Returns {id}.",
            .positionalDoc = "set layer kind",
            .flags = flags,
            .run = [](const Args& a) -> json {
                Session s = open(a, "texAddMask");
                Layer& l = s.layer(positional(a, 1, "texAddMask", "layer"));
                MaskKind kind;
                if (!parseMaskKind(positional(a, 2, "texAddMask", "kind"), &kind))
                    throw CommandError("texAddMask: kind is fill, generator, filter, levels, paint or anchor");
                return addMask(s, l, maskFromArgs(s, a, kind, l), a);
            },
        });
    }

    ops::registerCommand({
        .name = "texAddGenerator",
        .doc = "Add a generator mask to a layer: edge wear, dirt, dust, cavity grime, AO darken, position gradient, thickness, scratches, drips, "
               "rust spread, moss, wetness, paint chips, sun bleach, ID select, UV island random, polygon fill (genEdgeWear, genDirt, ...; "
               "texNodeTypes lists the Generator category). Its inputs bind to the set's mesh maps by name. Returns {id}.",
        .positionalDoc = "set layer type",
        .flags = maskFlags(),
        .run = [](const Args& a) -> json {
            Session s = open(a, "texAddGenerator");
            Layer& l = s.layer(positional(a, 1, "texAddGenerator", "layer"));
            Mask m = maskFromArgs(s, a, MaskKind::Generator, l);
            m.type = positional(a, 2, "texAddGenerator", "generator type");
            return addMask(s, l, std::move(m), a);
        },
    });

    ops::registerCommand({
        .name = "texSetMask",
        .doc = "Change a layer's mask: props is a merge patch over its JSON (name, enabled, mode, opacity, invert, value, type, values, bindings, "
               "image, anchor, channel). Returns the mask.",
        .positionalDoc = "set layer mask props",
        .run = [](const Args& a) -> json {
            Session s = open(a, "texSetMask");
            Layer& l = s.layer(positional(a, 1, "texSetMask", "layer"));
            const std::string id = positional(a, 2, "texSetMask", "mask");
            auto it = std::find_if(l.masks.begin(), l.masks.end(), [&](const Mask& m) { return m.id == id; });
            if (it == l.masks.end())
                throw CommandError("texSetMask: " + l.id + " has no mask " + id);
            if (a.positional.size() < 4)
                throw CommandError("texSetMask: props required");
            const std::string variant = s.stack().variant;
            Mask current = *it;
            if (!variant.empty())
                if (const Layer* r = withVariant(s.stack()).find(l.id))
                    for (const Mask& m : r->masks)
                        if (m.id == id)
                            current = m;
            Mask next = patched(current, a.positional[3], "texSetMask");
            checkMask(s, next);
            json out = next.toJson();
            if (!variant.empty())
            {
                json patch = a.positional[3];
                patch.erase("id");
                mergeOverride(s.stack().variants[variant][l.id]["maskOverrides"][id], patch);
            }
            else
                *it = std::move(next);
            s.commit();
            return out;
        },
    });

    ops::registerCommand({
        .name = "texRemoveMask",
        .doc = "Remove masks from a layer.",
        .positionalDoc = "set layer mask...",
        .run = [](const Args& a) -> json {
            Session s = open(a, "texRemoveMask");
            Layer& l = s.layer(positional(a, 1, "texRemoveMask", "layer"));
            for (size_t i = 2; i < a.positional.size(); ++i)
            {
                const std::string id = a.positional[i].get<std::string>();
                if (std::erase_if(l.masks, [&](const Mask& m) { return m.id == id; }) == 0)
                    throw CommandError("texRemoveMask: " + l.id + " has no mask " + id);
            }
            return s.commit();
        },
    });

    ops::registerCommand({
        .name = "texAddAnchor",
        .doc = "Put an anchor on a layer: a name for the composite right after it, which masks (kind anchor) and generator inputs "
               "(anchor:<name>.<channel>) of layers above read. Returns the name.",
        .positionalDoc = "set layer name",
        .run = [](const Args& a) -> json {
            Session s = open(a, "texAddAnchor");
            Layer& l = s.layer(positional(a, 1, "texAddAnchor", "layer"));
            const std::string name = positional(a, 2, "texAddAnchor", "name");
            if (!gn::validIdentifier(name))
                throw CommandError("texAddAnchor: '" + name + "' isn't a valid name (letters, digits, underscores)");
            Layer next = l;
            next.anchor = name;
            checkLayer(s, next);
            l.anchor = name;
            s.commit();
            return name;
        },
    });

    ops::registerCommand({
        .name = "texApplySmartMaterial",
        .doc = "Apply a smart material (a saved layer, usually a folder) to a texture set, or a smart mask to one of its layers. By name (built-in, extension pack, "
               "user or workspace library) or .lilasmart path. Its generators bind to the set's mesh maps by name; the maps it reads that the "
               "set's bake project doesn't make are added to it. Returns {layer or masks, maps, addedMaps}.",
        .positionalDoc = "set smartMaterial",
        .flags = {{"name", "", T::String, nullptr, "Layer name; the smart material's when left out."},
                  {"above", "", T::String, nullptr, "Insert right above this layer."},
                  {"below", "", T::String, nullptr, "Insert right below this layer."},
                  {"parent", "", T::String, nullptr, "Folder to put it in."},
                  {"index", "", T::Int, nullptr, "Position in the root or parent.", {}, {}, 0.0},
                  {"layer", "", T::String, nullptr, "Smart masks: the layer they go on."},
                  {"addMaps", "", T::Bool, true, "Add mesh maps it needs to the set's bake project (else only report them as missingMaps)."}},
        .run = [](const Args& a) -> json { return applySmart(a); },
    });

    ops::registerCommand({
        .name = "texSaveSmartMaterial",
        .doc = "Save a layer (a folder, usually) as a smart material, or its masks as a smart mask (mask=True). Path optional: the user library "
               "otherwise. Returns the file.",
        .positionalDoc = "set layer [path]",
        .flags = {{"name", "", T::String, nullptr, "Library name; the layer's when left out."},
                  {"description", "", T::String, nullptr, "Shown in the library."},
                  {"mask", "", T::Bool, false, "Save the layer's mask stack as a smart mask."}},
        .undoable = false,
        .run = [](const Args& a) -> json { return saveSmart(a); },
    });

    ops::registerCommand({
        .name = "texSmartMaterials",
        .doc = "The smart materials and masks in the libraries: name, kind, description, tags, tier and file. tags= keeps those with every "
               "tag given.",
        .flags = {{"tags", "", T::List, nullptr, "Only those carrying all of these tags."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::vector<std::string> want = normalizeTags(a.get<std::vector<std::string>>("tags", {}));
            json out = json::array();
            for (const SmartMaterial& m : listSmartMaterials())
            {
                if (!std::ranges::all_of(want, [&](const std::string& t) { return std::ranges::binary_search(m.tags, t); }))
                    continue;
                out.push_back({{"name", m.name}, {"kind", m.kind}, {"description", m.description}, {"tags", m.tags}, {"tier", m.tier},
                               {"file", paths::toUtf8(m.file)}});
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "texSetSmartMaterialTags",
        .doc = "Set, add or remove a smart material's or mask's tags, for you only (built-in ones too). Returns its tags.",
        .positionalDoc = "smartMaterial",
        .flags = {{"tags", "", T::List, nullptr, "Replace its tags with these."},
                  {"add", "", T::List, nullptr, "Tags to add."},
                  {"remove", "", T::List, nullptr, "Tags to remove."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("texSetSmartMaterialTags: give a smart material name");
            SmartMaterial m;
            try
            {
                m = loadSmartMaterial(a.positional[0].get<std::string>());
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("texSetSmartMaterialTags: ") + e.what());
            }
            std::vector<std::string> tags = a.has("tags") ? a.get<std::vector<std::string>>("tags") : m.tags;
            for (const std::string& t : a.get<std::vector<std::string>>("add", {}))
                tags.push_back(t);
            tags = normalizeTags(tags);
            for (const std::string& t : normalizeTags(a.get<std::vector<std::string>>("remove", {})))
                std::erase(tags, t);
            setSmartMaterialTags(m.name, tags);
            return tags;
        },
    });
    ops::registerCommand({
        .name = "texCopyLayers",
        .doc = "Copy layers (with their masks, children and paint) as text for texPasteLayers, into this set or another, in this scene "
               "or another. Returns the text.",
        .positionalDoc = "set [layer]",
        .flags = {{"layers", "", T::List, nullptr, "The layers to copy, bottom first; else the positional one."}},
        .undoable = false,
        .run = copyLayers,
    });
    ops::registerCommand({
        .name = "texPasteLayers",
        .doc = "Paste layers texCopyLayers copied, with fresh layer and mask ids, unique anchor names and their own copies of the paint. "
               "Placed like texAddLayer (above, below, parent, index). Returns {layers, warnings}.",
        .positionalDoc = "set text",
        .flags = {{"above", "", T::String, nullptr, "Insert right above this layer."},
                  {"below", "", T::String, nullptr, "Insert right below this layer."},
                  {"parent", "", T::String, nullptr, "Folder to put them in."},
                  {"index", "", T::Int, nullptr, "Position in the root or parent, 0 the bottom.", {}, {}, 0.0}},
        .run = pasteLayers,
    });
    ops::registerCommand({
        .name = "texFlattenLayers",
        .doc = "Flatten root layers into one paint layer: what the stack shows with them (the stack below them included, every channel "
               "they write, masks baked in) becomes the new layer's pixels, written to folder as EXR. down=True merges the layer into "
               "the one below it. keep=True leaves the originals hidden under it. Returns {layer, channels, warnings}.",
        .positionalDoc = "set [layer]",
        .flags = {{"layers", "", T::List, nullptr, "Root layers to flatten together; else the positional one."},
                  {"down", "", T::Bool, false, "Merge the layer with the one below it."},
                  {"keep", "", T::Bool, false, "Keep the flattened layers, hidden."},
                  {"name", "", T::String, nullptr, "The new layer's name."},
                  {"folder", "", T::String, nullptr, "Where its pixels go; textures/<set>/flattened by default."}},
        .run = flattenLayers,
    });
    ops::registerCommand({
        .name = "texExportFlattened",
        .doc = "Write what flattening root layers would make (the stack with them, every channel they write) as images, leaving the stack "
               "alone. Returns {files, warnings}.",
        .positionalDoc = "set [layer]",
        .flags = {{"layers", "", T::List, nullptr, "Root layers; else the positional one."},
                  {"folder", "", T::String, nullptr, "Where the files go; textures/<set> by default."},
                  {"format", "", T::String, "png", "png (16 bit, colour as sRGB) or exr (32 bit linear).", {"png", "exr"}}},
        .undoable = false,
        .run = exportFlattened,
    });
}
} // namespace lila::tex
