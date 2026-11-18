#include "tex/MaterialFromMaps.h"

#include "core/Paths.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "tex/Layers.h"
#include "tex/Library.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <regex>
#include <set>

namespace lila::tex::maps
{
namespace
{
namespace fs = std::filesystem;
using json = nlohmann::json;

/** Lower-case tokens of a file stem, split at separators and at lower-to-upper case changes (BaseColor: base, color). */
std::vector<std::string> tokens(const std::string& stem)
{
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty())
            out.push_back(cur);
        cur.clear();
    };
    for (size_t i = 0; i < stem.size(); ++i)
    {
        const char c = stem[i];
        if (c == '_' || c == '-' || c == '.' || c == ' ')
        {
            flush();
            continue;
        }
        if (std::isupper(static_cast<unsigned char>(c)) && i > 0 && std::islower(static_cast<unsigned char>(stem[i - 1])))
            flush();
        cur += char(std::tolower(static_cast<unsigned char>(c)));
    }
    flush();
    return out;
}

struct Token
{
    const char* token;
    const char* channel;
};

/** Single tokens; two-token names (base color, ambient occlusion) are joined before lookup. */
constexpr Token kTokens[] = {
    {"basecolor", "baseColor"}, {"albedo", "baseColor"},  {"diffuse", "baseColor"},  {"diff", "baseColor"},     {"color", "baseColor"},
    {"colour", "baseColor"},    {"col", "baseColor"},     {"alb", "baseColor"},      {"roughness", "roughness"}, {"rough", "roughness"},
    {"rgh", "roughness"},       {"glossiness", "gloss"},  {"gloss", "gloss"},        {"metallic", "metallic"},   {"metalness", "metallic"},
    {"metal", "metallic"},      {"met", "metallic"},      {"normal", "normal"},      {"nrm", "normal"},          {"nor", "normal"},
    {"norm", "normal"},         {"nml", "normal"},        {"height", "height"},      {"disp", "height"},         {"displacement", "height"},
    {"bump", "height"},         {"ao", "ao"},             {"ambientocclusion", "ao"}, {"occlusion", "ao"},       {"occ", "ao"},
    {"emissive", "emissive"},   {"emission", "emissive"}, {"emit", "emissive"},      {"glow", "emissive"},       {"opacity", "opacity"},
    {"alpha", "opacity"},       {"transparency", "opacity"},
};

/** Packed maps by their letters: r, g, b in order. */
const std::map<std::string, std::vector<std::string>>& packedTokens()
{
    static const std::map<std::string, std::vector<std::string>> k = {
        {"orm", {"ao", "roughness", "metallic"}},  {"arm", {"ao", "roughness", "metallic"}}, {"rma", {"roughness", "metallic", "ao"}},
        {"mra", {"metallic", "roughness", "ao"}},  {"rmao", {"roughness", "metallic", "ao"}}, {"occlusionroughnessmetallic", {"ao", "roughness", "metallic"}},
    };
    return k;
}

bool imageFile(const fs::path& p)
{
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".tif" || ext == ".tiff" || ext == ".exr" || ext == ".psd" ||
           ext == ".bmp" || ext == ".hdr";
}

bool floatImageFile(const fs::path& p)
{
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
    return ext == ".exr" || ext == ".hdr";
}
} // namespace

const char* imageColorSpace(const fs::path& file, const std::string& channel)
{
    const bool colour = channel == "baseColor" || channel == "emissive";
    return colour && !floatImageFile(file) ? "srgb" : "linear";
}

namespace
{

/** The index of the role token in the stem's tokens and how many it spans; -1 when none. */
std::optional<std::pair<MapRole, std::pair<size_t, size_t>>> findRole(const std::vector<std::string>& t)
{
    // From the end: names put the channel last (Rock_Normal_DX) more often than first.
    for (size_t k = t.size(); k-- > 0;)
    {
        for (size_t span : {size_t(3), size_t(2), size_t(1)})
        {
            if (k + 1 < span)
                continue;
            std::string joined;
            for (size_t i = k + 1 - span; i <= k; ++i)
                joined += t[i];
            if (auto it = packedTokens().find(joined); it != packedTokens().end())
                return std::pair{MapRole{"packed", it->second}, std::pair{k + 1 - span, span}};
            for (const Token& tok : kTokens)
                if (joined == tok.token)
                {
                    MapRole r;
                    r.channel = tok.channel;
                    if (r.channel == "gloss")
                    {
                        r.channel = "roughness";
                        r.gloss = true;
                    }
                    return std::pair{r, std::pair{k + 1 - span, span}};
                }
        }
    }
    return std::nullopt;
}
} // namespace

std::optional<MapRole> classify(const std::string& fileName)
{
    const std::vector<std::string> t = tokens(fs::path(paths::fromUtf8(fileName)).stem().string());
    auto found = findRole(t);
    if (!found)
        return std::nullopt;
    MapRole r = found->first;
    if (r.channel == "normal")
        for (const std::string& s : t)
            if (s == "dx" || s == "directx" || s == "d3d")
                r.directX = true;
    return r;
}

double sizeFromName(const std::string& fileName)
{
    static const std::regex kSize(R"((\d+(?:\.\d+)?)(?:x\d+(?:\.\d+)?)?(cm|m)(?![a-z]))", std::regex::icase);
    const std::string stem = fs::path(paths::fromUtf8(fileName)).stem().string();
    std::smatch m;
    if (!std::regex_search(stem, m, kSize))
        return 0.0;
    const double v = std::stod(m[1].str());
    std::string unit = m[2].str();
    std::transform(unit.begin(), unit.end(), unit.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
    return unit == "m" ? v * 100.0 : v;
}

std::vector<MapMaterial> group(const std::vector<fs::path>& files)
{
    std::map<std::pair<std::string, std::string>, MapMaterial> byKey;
    std::vector<std::string> unknown;
    for (const fs::path& f : files)
    {
        if (!imageFile(f))
            continue;
        const std::vector<std::string> t = tokens(f.stem().string());
        auto found = findRole(t);
        if (!found)
        {
            unknown.push_back(paths::toUtf8(f.filename()));
            continue;
        }
        // The material's name: the stem without its channel token and the tokens that only say how it's stored.
        static const std::set<std::string> kNoise = {"dx", "directx", "d3d", "gl", "opengl", "ogl", "1k", "2k", "4k", "8k", "16k"};
        std::string name;
        for (size_t i = 0; i < t.size(); ++i)
            if ((i < found->second.first || i >= found->second.first + found->second.second) && !kNoise.contains(t[i]) && sizeFromName(t[i]) == 0.0)
                name += (name.empty() ? "" : "_") + t[i];
        if (name.empty())
            name = paths::toUtf8(f.parent_path().filename());
        MapMaterial& m = byKey[{paths::toUtf8(f.parent_path()), name}];
        m.name = name;
        m.folder = f.parent_path();
        std::optional<MapRole> role = classify(paths::toUtf8(f.filename()));
        m.maps.push_back({f, *role});
        if (const double cm = sizeFromName(paths::toUtf8(f.filename())); cm > 0.0)
            m.scaleCm = cm;
    }
    std::vector<MapMaterial> out;
    for (auto& [k, m] : byKey)
    {
        std::sort(m.maps.begin(), m.maps.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::set<std::string> seen;
        for (const auto& [f, r] : m.maps)
            for (const std::string& c : r.channel == "packed" ? r.packed : std::vector<std::string>{r.channel})
                if (!c.empty() && !seen.insert(c).second)
                    m.warnings.push_back(m.name + ": more than one map for " + c + "; the last one wins");
        out.push_back(std::move(m));
    }
    if (!unknown.empty() && !out.empty())
        for (const std::string& u : unknown)
            out.front().warnings.push_back(u + ": no channel in its name, left out");
    return out;
}

Graph buildGraph(const MapMaterial& m)
{
    Graph g = Graph::makeMaterial(m.name);
    double y = -300.0;
    int k = 0;
    auto add = [&](const std::string& type, json values) -> std::string {
        const std::string name = type + std::to_string(++k);
        g.nodes.push_back({name, type, {-500.0, y}, std::move(values)});
        return name;
    };
    auto link = [&](const std::string& from, const std::string& socket, const std::string& to, const std::string& input) {
        std::erase_if(g.links, [&](const GLink& l) { return l.toNode == to && l.toSocket == input; });
        g.links.push_back({from, socket, to, input});
    };
    for (const auto& [file, role] : m.maps)
    {
        const std::string img = add("image", {{"file", paths::toUtf8(file)}, {"colorSpace", imageColorSpace(file, role.channel)}});
        if (role.channel == "packed")
        {
            static const char* kComponents[] = {"r", "g", "b"};
            for (size_t i = 0; i < role.packed.size() && i < 3; ++i)
            {
                const std::string ex = add("extractChannel", {{"channel", kComponents[i]}});
                link(img, "out", ex, "input");
                link(ex, "out", "Output", role.packed[i]);
            }
        }
        else if (role.channel == "normal" && role.directX)
        {
            const std::string inv = add("normalInvert", {{"invertX", false}, {"invertY", true}});
            link(img, "out", inv, "input");
            link(inv, "out", "Output", "normal");
        }
        else if (role.gloss)
        {
            const std::string ex = add("extractChannel", {{"channel", "r"}});
            const std::string one = add("math", {{"operation", "oneMinus"}});
            link(img, "out", ex, "input");
            link(ex, "out", one, "a");
            link(one, "out", "Output", "roughness");
        }
        else
            link(img, "out", "Output", role.channel);
        y += 160.0;
    }
    return g;
}

SetSettings settingsFor(const MapMaterial& m)
{
    SetSettings s = SetSettings::makeDefault();
    for (const auto& [file, role] : m.maps)
        if (role.channel == "opacity" && !s.channel("opacity"))
            s.channels.push_back({"opacity", PortType::Gray, 8});
    return s;
}

namespace
{
json fromMaps(const Args& a)
{
    std::vector<fs::path> files;
    for (const json& f : a.positional)
        if (f.is_string())
            files.push_back(paths::fromUtf8(f.get<std::string>()));
    if (a.has("folder"))
    {
        const fs::path dir = paths::fromUtf8(a.get<std::string>("folder"));
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
            throw CommandError("texMaterialFromMaps: " + paths::toUtf8(dir) + " isn't a folder");
        if (a.get<bool>("recursive", true))
        {
            for (const auto& e : fs::recursive_directory_iterator(dir, ec))
                if (e.is_regular_file())
                    files.push_back(e.path());
        }
        else
            for (const auto& e : fs::directory_iterator(dir, ec))
                if (e.is_regular_file())
                    files.push_back(e.path());
    }
    if (files.empty())
        throw CommandError("texMaterialFromMaps: give image files or folder=");
    const std::vector<MapMaterial> mats = group(files);
    if (mats.empty())
        throw CommandError("texMaterialFromMaps: none of the files has a channel in its name (basecolor, rough, nrm, ...)");
    json out = json::array();
    const std::string tier = a.get<std::string>("tier", "user");
    const std::string set = a.get<std::string>("set", "");
    for (const MapMaterial& m : mats)
    {
        const Graph g = buildGraph(m);
        const SetSettings s = settingsFor(m);
        json entry = {{"name", m.name}, {"maps", json::array()}, {"warnings", m.warnings}};
        for (const auto& [f, r] : m.maps)
        {
            json mj = {{"file", paths::toUtf8(f)}, {"channel", r.channel}};
            if (!r.packed.empty())
                mj["packed"] = r.packed;
            if (r.directX)
                mj["directX"] = true;
            if (r.gloss)
                mj["gloss"] = true;
            entry["maps"].push_back(mj);
        }
        if (!set.empty())
        {
            // Straight onto a set's stack, as a material layer.
            json base;
            Layer l = graphAsLayer(g, s, &base);
            json r = ops::execute("texAddLayer", {{"args", {set, "procedural"}}, {"name", m.name}, {"graph", l.graph}}, CallSource::Script);
            entry["layer"] = r.value("id", std::string());
        }
        else
        {
            library::Meta meta;
            meta.name = m.name;
            meta.category = a.get<std::string>("category", "Imported");
            if (m.scaleCm > 0.0)
                meta.scaleCm = m.scaleCm;
            const fs::path dir = a.has("path") ? paths::fromUtf8(a.get<std::string>("path")) : library::tierDir(tier);
            if (dir.empty())
                throw CommandError("texMaterialFromMaps: no " + tier + " material folder (open a workspace for tier='workspace')");
            const fs::path file = dir / paths::fromUtf8((a.has("path") ? std::string() : meta.category + "/") + m.name + ".lilamat");
            std::error_code ec;
            fs::create_directories(file.parent_path(), ec);
            const json doc = {{"format", "lilamat"}, {"version", 1}, {"name", m.name}, {"meta", meta.toJson()}, {"graph", g.toJson()}, {"settings", s.toJson()}};
            std::ofstream fo(file, std::ios::binary);
            if (!fo)
                throw CommandError("texMaterialFromMaps: can't write " + paths::toUtf8(file));
            fo << doc.dump(2) << "\n";
            entry["file"] = paths::toUtf8(file);
            entry["scaleCm"] = meta.scaleCm;
        }
        out.push_back(entry);
    }
    if (set.empty())
        library::reload();
    return out;
}
} // namespace

void registerCommands()
{
    ops::registerCommand({
        .name = "texMaterialFromMaps",
        .doc = "Make materials from texture maps: each file's channel comes from its name (BaseColor, albedo, diff, rough, gloss, metal, "
               "nrm or normal with dx or gl, height or disp, ao, emissive, opacity, and packed ORM, ARM, RMA, MRA), files differing only "
               "by that token make one material, and a size in the name (200cm, 2m) sets its scale. Packed maps are split, DirectX "
               "normals flipped, gloss inverted. Writes a .lilamat per material to the library (tier, category, or path), or with set= "
               "adds each as a material layer. Returns one entry per material with its maps and warnings.",
        .positionalDoc = "[file...]",
        .flags = {{"folder", "", ArgType::String, nullptr, "A folder of maps (searched recursively unless recursive=False)."},
                  {"recursive", "", ArgType::Bool, true, "Search the folder's subfolders too."},
                  {"tier", "", ArgType::String, "user", "user or workspace.", {"user", "workspace"}},
                  {"category", "", ArgType::String, "Imported", "Library category."},
                  {"path", "", ArgType::String, nullptr, "Write the .lilamat files to this folder instead."},
                  {"set", "", ArgType::String, nullptr, "Add them to this texture set as layers instead."}},
        .run = fromMaps,
    });
}
} // namespace lila::tex::maps
