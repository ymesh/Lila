#include "bake/BakeTypes.h"

#include <stdexcept>

namespace lila::bake
{
namespace
{
template <class T> void read(const json& j, const char* key, T& out)
{
    auto it = j.find(key);
    if (it == j.end() || it->is_null())
        return;
    try
    {
        out = it->get<T>();
    }
    catch (const json::exception&)
    {
        throw std::invalid_argument(std::string(key) + " has the wrong type");
    }
}

std::vector<std::string> strings(const json& j, const char* key)
{
    std::vector<std::string> out;
    auto it = j.find(key);
    if (it == j.end() || !it->is_array())
        return out;
    for (const json& v : *it)
        if (v.is_string())
            out.push_back(v.get<std::string>());
    return out;
}
} // namespace

json OutputSettings::toJson() const
{
    json j = {{"folder", folder}, {"name", name},     {"format", format},               {"bits", bits},
              {"width", width},   {"height", height}, {"supersampling", supersampling}, {"padding", padding},
              {"dither", dither}, {"compression", compression}, {"compressionLevel", compressionLevel}, {"udimNaming", udimNaming},
              {"sidecar", sidecar}};
    if (!materials.empty())
    {
        j["materials"] = json::object();
        for (const auto& [material, v] : materials)
            j["materials"][material] = v;
    }
    return j;
}

OutputSettings OutputSettings::forMaterial(std::string_view material) const
{
    OutputSettings o = *this;
    o.materials.clear();
    const auto it = materials.find(material);
    if (it == materials.end())
        return o;
    json j = o.toJson();
    for (const char* k : kMaterialOutputKeys)
        if (it->second.contains(k))
            j[k] = it->second[k];
    return fromJson(j);
}

OutputSettings OutputSettings::fromJson(const json& j)
{
    OutputSettings o;
    if (!j.is_object())
        return o;
    read(j, "folder", o.folder);
    read(j, "name", o.name);
    read(j, "format", o.format);
    read(j, "bits", o.bits);
    if (j.contains("resolution"))
    {
        int r = 0;
        read(j, "resolution", r);
        o.width = o.height = r;
    }
    read(j, "width", o.width);
    read(j, "height", o.height);
    read(j, "supersampling", o.supersampling);
    read(j, "padding", o.padding);
    read(j, "dither", o.dither);
    read(j, "compression", o.compression);
    read(j, "compressionLevel", o.compressionLevel);
    read(j, "udimNaming", o.udimNaming);
    read(j, "sidecar", o.sidecar);
    if (j.contains("materials") && j["materials"].is_object())
        for (const auto& [name, v] : j["materials"].items())
        {
            if (!v.is_object())
                throw std::invalid_argument("materials." + name + " must be an object");
            json kept = json::object();
            for (const char* k : kMaterialOutputKeys)
                if (v.contains(k))
                    kept[k] = v[k];
            if (!kept.empty())
                o.materials[name] = kept;
        }
    if (o.format == "tiff")
        o.format = "tif";
    if (o.format != "png" && o.format != "exr" && o.format != "tga" && o.format != "tif" && o.format != "psd")
        throw std::invalid_argument("format must be png, exr, tga, tif or psd");
    if (o.compression != "deflate" && o.compression != "none")
        throw std::invalid_argument("compression must be deflate or none");
    if (o.compressionLevel < 1 || o.compressionLevel > 9)
        throw std::invalid_argument("compressionLevel must be 1 to 9");
    if (o.udimNaming != "1001" && o.udimNaming != "uv" && o.udimNaming != "uv0")
        throw std::invalid_argument("udimNaming must be 1001, uv or uv0");
    if (o.bits != 8 && o.bits != 16 && o.bits != 32)
        throw std::invalid_argument("bits must be 8, 16 or 32");
    if (o.width < 1 || o.height < 1 || o.width > 16384 || o.height > 16384)
        throw std::invalid_argument("resolution must be 1 to 16384");
    if (o.supersampling != 1 && o.supersampling != 4 && o.supersampling != 16 && o.supersampling != 64)
        throw std::invalid_argument("supersampling must be 1, 4, 16 or 64");
    if (o.padding < -1)
        throw std::invalid_argument("padding must be -1 (fill) or more");
    for (const auto& [name, v] : o.materials)
        try
        {
            o.forMaterial(name);
        }
        catch (const std::invalid_argument& e)
        {
            throw std::invalid_argument("material " + name + ": " + e.what());
        }
    return o;
}

json MapSettings::toJson() const
{
    json j = {{"map", map}, {"enabled", enabled}};
    if (!suffix.empty())
        j["suffix"] = suffix;
    if (bits)
        j["bits"] = bits;
    if (!format.empty())
        j["format"] = format;
    if (!options.empty())
        j["options"] = options;
    return j;
}

MapSettings MapSettings::fromJson(const json& j)
{
    MapSettings m;
    if (j.is_string())
    {
        m.map = j.get<std::string>();
        return m;
    }
    read(j, "map", m.map);
    read(j, "enabled", m.enabled);
    read(j, "suffix", m.suffix);
    read(j, "bits", m.bits);
    read(j, "format", m.format);
    if (j.contains("options") && j["options"].is_object())
        m.options = j["options"];
    if (m.map.empty())
        throw std::invalid_argument("a map entry needs a map id");
    return m;
}

json HighEntry::toJson() const
{
    json j = json::object();
    if (self)
        j["self"] = true;
    else if (!file.empty())
    {
        j["file"] = file;
        if (!object.empty())
            j["object"] = object;
        if (frame != 0.0)
            j["frame"] = frame;
    }
    else
        j["node"] = node;
    if (!modifiers.empty())
        j["modifiers"] = modifiers;
    return j;
}

HighEntry HighEntry::fromJson(const json& j)
{
    HighEntry h;
    if (j.is_string())
    {
        h.node = j.get<std::string>();
        return h;
    }
    read(j, "node", h.node);
    read(j, "self", h.self);
    read(j, "file", h.file);
    read(j, "object", h.object);
    read(j, "frame", h.frame);
    if (j.contains("modifiers") && j["modifiers"].is_array())
        h.modifiers = j["modifiers"];
    return h;
}

json Unlink::toJson() const
{
    return {{"lows", lows}, {"meshes", meshes}, {"materials", materials}, {"occlusion", occlusion}, {"projection", projection}};
}

Unlink Unlink::fromJson(const json& j)
{
    Unlink u;
    if (!j.is_object())
        throw std::invalid_argument("an unlink is an object: {lows, meshes, materials, occlusion, projection}");
    u.lows = strings(j, "lows");
    u.meshes = strings(j, "meshes");
    u.materials = strings(j, "materials");
    read(j, "occlusion", u.occlusion);
    read(j, "projection", u.projection);
    return u;
}

json GroupSettings::toJson() const
{
    json hs = json::array();
    for (const HighEntry& h : highs)
        hs.push_back(h.toJson());
    json j = {{"version", kSettingsVersion}, {"lows", lows},
              {"highs", hs},                 {"cage", cage},
              {"frontal", frontal},          {"rear", rear},
              {"cageOffset", cageOffset},    {"ignoreBackfaces", ignoreBackfaces},
              {"aoScope", aoScope},          {"skew", skew}};
    if (!unlinks.empty())
    {
        j["unlinks"] = json::array();
        for (const Unlink& u : unlinks)
            j["unlinks"].push_back(u.toJson());
    }
    return j;
}

GroupSettings GroupSettings::fromJson(const json& j)
{
    GroupSettings g;
    if (!j.is_object())
        return g;
    g.lows = strings(j, "lows");
    if (j.contains("highs") && j["highs"].is_array())
        for (const json& h : j["highs"])
            g.highs.push_back(HighEntry::fromJson(h));
    read(j, "cage", g.cage);
    read(j, "frontal", g.frontal);
    read(j, "rear", g.rear);
    read(j, "cageOffset", g.cageOffset);
    read(j, "ignoreBackfaces", g.ignoreBackfaces);
    read(j, "aoScope", g.aoScope);
    read(j, "skew", g.skew);
    if (j.contains("unlinks") && j["unlinks"].is_array())
        for (const json& u : j["unlinks"])
            g.unlinks.push_back(Unlink::fromJson(u));
    if (g.aoScope != "self" && g.aoScope != "group" && g.aoScope != "all")
        throw std::invalid_argument("aoScope must be self, group or all");
    if (g.frontal < 0.0 || g.rear < 0.0 || g.cageOffset < 0.0)
        throw std::invalid_argument("distances and cage offset can't be negative");
    if (g.skew < 0.0 || g.skew > 1.0)
        throw std::invalid_argument("skew must be 0 to 1");
    return g;
}

json ProjectSettings::toJson() const
{
    json ms = json::array();
    for (const MapSettings& m : maps)
        ms.push_back(m.toJson());
    json j = {{"version", kSettingsVersion}, {"output", output.toJson()},   {"maps", ms},
              {"target", target},            {"rebakeOnSave", rebakeOnSave}, {"autoAssign", autoAssign}};
    if (!groupDefaults.empty())
        j["groupDefaults"] = groupDefaults;
    return j;
}

ProjectSettings ProjectSettings::fromJson(const json& j)
{
    ProjectSettings p;
    if (!j.is_object())
        return defaults();
    if (j.contains("output"))
        p.output = OutputSettings::fromJson(j["output"]);
    if (j.contains("maps") && j["maps"].is_array())
        for (const json& m : j["maps"])
            p.maps.push_back(MapSettings::fromJson(m));
    else
        p.maps = defaults().maps;
    read(j, "target", p.target);
    read(j, "rebakeOnSave", p.rebakeOnSave);
    read(j, "autoAssign", p.autoAssign);
    if (j.contains("groupDefaults") && j["groupDefaults"].is_object())
        p.groupDefaults = j["groupDefaults"];
    return p;
}

ProjectSettings ProjectSettings::defaults()
{
    ProjectSettings p;
    for (const char* m : {"normal", "objectNormal", "height", "position", "curvature", "cavity", "thickness", "ao"})
        p.maps.push_back(MapSettings{.map = m});
    return p;
}
} // namespace lila::bake
