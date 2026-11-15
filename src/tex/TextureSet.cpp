#include "tex/TextureSet.h"

#include "scene/Node.h"
#include "tex/PageStore.h"
#include "tex/PaintStrokes.h"

#include <algorithm>
#include <stdexcept>

namespace lila::tex
{
using scene::Node;
using scene::NodeId;

namespace
{

int attr(const Node& n, const char* name) { return n.type().attrIndex(name); }

class TextureSetNode final : public Node
{
};

bool knownBits(int b) { return b == 8 || b == 16 || b == 32; }
} // namespace

const Channel* SetSettings::channel(std::string_view name) const
{
    for (const Channel& c : channels)
        if (c.name == name)
            return &c;
    return nullptr;
}

std::pair<int, int> SetSettings::tileSize(const Tile& t) const { return {t.width > 0 ? t.width : width, t.height > 0 ? t.height : height}; }

json SetSettings::toJson() const
{
    json ch = json::array();
    for (const Channel& c : channels)
        ch.push_back({{"name", c.name}, {"type", portTypeName(c.type)}, {"bits", c.bits}});
    json tl = json::array();
    for (const Tile& t : tiles)
    {
        json o = {{"udim", t.udim}};
        if (t.width > 0)
            o["width"] = t.width;
        if (t.height > 0)
            o["height"] = t.height;
        tl.push_back(o);
    }
    return {{"channels", ch},         {"width", width},           {"height", height},           {"tiles", tl},
            {"normalConvention", normalConvention}, {"bakeProject", bakeProject}, {"exportPreset", exportPreset}, {"material", material},
            {"heightDepth", heightDepth},   {"uvWidth", uvWidth},       {"uvHeight", uvHeight},       {"normalFromHeight", normalFromHeight},
            {"normalFade", normalFade},     {"heightSource", heightSource}};
}

SetSettings SetSettings::fromJson(const json& j)
{
    if (!j.is_object())
        throw std::invalid_argument("texture set settings must be an object");
    SetSettings s = makeDefault();
    try
    {
        if (j.contains("channels"))
        {
            s.channels.clear();
            for (const json& c : j["channels"])
            {
                Channel ch;
                ch.name = c.at("name").get<std::string>();
                if (!gn::validIdentifier(ch.name))
                    throw std::invalid_argument("channel name '" + ch.name + "' isn't an identifier");
                if (!parsePortType(c.value("type", "gray"), &ch.type) || ch.type == PortType::Material)
                    throw std::invalid_argument("channel " + ch.name + ": bad type " + c.value("type", ""));
                ch.bits = c.value("bits", 8);
                if (!knownBits(ch.bits))
                    throw std::invalid_argument("channel " + ch.name + ": bits must be 8, 16 or 32");
                if (s.channel(ch.name))
                    throw std::invalid_argument("channel " + ch.name + " is listed twice");
                s.channels.push_back(ch);
            }
        }
        s.width = j.value("width", s.width);
        s.height = j.value("height", s.height);
        if (s.width < 1 || s.height < 1 || s.width > 32768 || s.height > 32768)
            throw std::invalid_argument("resolution must be 1 to 32768");
        if (j.contains("tiles"))
        {
            s.tiles.clear();
            for (const json& t : j["tiles"])
            {
                Tile tile;
                tile.udim = t.is_number() ? t.get<int>() : t.value("udim", 1001);
                if (t.is_object())
                {
                    tile.width = t.value("width", 0);
                    tile.height = t.value("height", 0);
                }
                if (tile.udim < 1001 || tile.udim > 1999)
                    throw std::invalid_argument("UDIM " + std::to_string(tile.udim) + " is out of range");
                s.tiles.push_back(tile);
            }
            if (s.tiles.empty())
                s.tiles.push_back({});
        }
        s.normalConvention = j.value("normalConvention", s.normalConvention);
        if (s.normalConvention != "opengl" && s.normalConvention != "directx")
            throw std::invalid_argument("normalConvention must be opengl or directx");
        s.bakeProject = j.value("bakeProject", s.bakeProject);
        s.exportPreset = j.value("exportPreset", s.exportPreset);
        s.material = j.value("material", s.material);
        s.heightDepth = j.value("heightDepth", s.heightDepth);
        s.uvWidth = j.value("uvWidth", s.uvWidth);
        s.uvHeight = j.value("uvHeight", s.uvHeight);
        if (!(s.heightDepth >= 0.0) || !(s.uvWidth > 0.0) || !(s.uvHeight > 0.0))
            throw std::invalid_argument("heightDepth must be 0 or more, uvWidth and uvHeight more than 0");
        s.normalFromHeight = j.value("normalFromHeight", s.normalFromHeight);
        s.normalFade = std::clamp(j.value("normalFade", s.normalFade), 0.0, 90.0);
        s.heightSource = j.value("heightSource", s.heightSource);
        if (s.heightSource != "channel" && s.heightSource != "mesh")
            throw std::invalid_argument("heightSource must be channel or mesh");
    }
    catch (const json::exception& e)
    {
        throw std::invalid_argument(std::string("bad texture set settings: ") + e.what());
    }
    return s;
}

SetSettings SetSettings::makeDefault()
{
    SetSettings s;
    s.channels = {{"baseColor", PortType::Color, 8}, {"roughness", PortType::Gray, 8}, {"metallic", PortType::Gray, 8}, {"normal", PortType::Color, 8},
                  {"height", PortType::Gray, 16},    {"ao", PortType::Gray, 8},         {"emissive", PortType::Color, 8}};
    return s;
}

void registerTypes()
{
    if (scene::types::find(kTextureSetType))
        return;
    scene::NodeType t;
    t.name = kTextureSetType;
    t.icon = "texture";
    t.doc = "A texture set: the texture graph and the channels, tiles and export preset its maps are made with.";
    t.ownAttrs = {{.name = "graph", .type = scene::PlugType::String, .defaultValue = std::string(),
                   .flags = scene::AttrInput | scene::AttrStorable | scene::AttrHidden, .doc = "Texture graph, JSON."},
                  {.name = "settings", .type = scene::PlugType::String, .defaultValue = std::string(),
                   .flags = scene::AttrInput | scene::AttrStorable | scene::AttrHidden, .doc = "Channels, tiles, normal convention, bake project, export preset; JSON."},
                  {.name = paint::kPaintAttr, .type = scene::PlugType::Data, .defaultValue = scene::DataPtr(),
                   .flags = scene::AttrInput | scene::AttrStorable | scene::AttrHidden, .doc = "Paint buffers: stroke records and flattened pixels."}};
    paint::registerDataKind();
    store::install();
    t.create = [] { return std::make_unique<TextureSetNode>(); };
    scene::types::registerType(std::move(t));
}

Node* findSet(scene::Graph& g, std::string_view name)
{
    for (NodeId id : g.findAll(name))
        if (Node* n = g.node(id); n->type().name == kTextureSetType)
            return n;
    return nullptr;
}

std::vector<std::string> setNames(scene::Graph& g)
{
    std::vector<std::string> out;
    for (NodeId id : g.nodes())
        if (g.node(id)->type().name == kTextureSetType)
            out.push_back(g.node(id)->name());
    return out;
}

std::string setForMaterial(scene::Graph& g, std::string_view material)
{
    for (const std::string& name : setNames(g))
        try
        {
            if (Node* n = findSet(g, name); n && readSettings(*n).material == material)
                return name;
        }
        catch (const std::exception&)
        {
        }
    return {};
}

std::string defaultSetName(std::string_view material)
{
    for (std::string_view prefix : {"MI_", "M_"})
        if (material.size() > prefix.size() && material.starts_with(prefix))
        {
            material.remove_prefix(prefix.size());
            break;
        }
    return std::string(material) + "Tex";
}

Graph readGraph(const Node& set)
{
    const std::string& def = set.as<std::string>(attr(set, "graph"));
    if (def.empty())
        return Graph::makeMaterial(set.name());
    try
    {
        return Graph::fromJson(json::parse(def));
    }
    catch (const json::exception& e)
    {
        throw std::invalid_argument(set.path() + ": " + e.what());
    }
}

SetSettings readSettings(const Node& set)
{
    const std::string& def = set.as<std::string>(attr(set, "settings"));
    if (def.empty())
        return SetSettings::makeDefault();
    try
    {
        return SetSettings::fromJson(json::parse(def));
    }
    catch (const json::exception& e)
    {
        throw std::invalid_argument(set.path() + ": " + e.what());
    }
}

void writeGraph(scene::Graph& g, Node& set, const Graph& graph)
{
    if (graph.ties.empty() && graph.frames.empty())
    {
        g.setValue({set.id(), attr(set, "graph"), -1}, graph.toJson().dump());
        return;
    }
    Graph pruned = graph;
    gn::pruneTies(pruned.ties, pruned.links);
    std::vector<std::string> names;
    for (const GNode& n : pruned.nodes)
        names.push_back(n.name);
    gn::pruneFrames(pruned.frames, names);
    g.setValue({set.id(), attr(set, "graph"), -1}, pruned.toJson().dump());
}

void writeSettings(scene::Graph& g, Node& set, const SetSettings& settings)
{
    g.setValue({set.id(), attr(set, "settings"), -1}, settings.toJson().dump());
}

ChannelList extraChannels(const SetSettings& s)
{
    ChannelList out;
    for (const Channel& c : s.channels)
        if (std::none_of(standardChannels().begin(), standardChannels().end(), [&](const auto& sc) { return sc.first == c.name; }))
            out.emplace_back(c.name, c.type);
    return out;
}

std::vector<OutputRequest> channelRequests(const Graph& g, const SetSettings& s)
{
    std::vector<std::pair<std::string, PortType>> channels;
    for (const Channel& c : s.channels)
        channels.emplace_back(c.name, c.type);
    return materialRequests(g, channels);
}

std::pair<int, int> udimOrigin(int udim) { return {(udim - 1001) % 10, (udim - 1001) / 10}; }
} // namespace lila::tex
