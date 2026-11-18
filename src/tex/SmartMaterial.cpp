#include "tex/SmartMaterial.h"

#include "core/ExtensionContext.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "extensions/ExtensionManager.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace lila::tex
{
namespace fs = std::filesystem;

namespace
{
constexpr const char* kExt = ".lilasmart";

fs::path tagsFile()
{
    return PrefStore::get().userDir() / "smartmaterials" / "tags.json";
}

json readTagOverrides()
{
    std::ifstream in(tagsFile(), std::ios::binary);
    if (!in)
        return json::object();
    try
    {
        json j = json::parse(in);
        return j.is_object() ? j : json::object();
    }
    catch (const json::exception&)
    {
        return json::object();
    }
}

void applyTagOverride(SmartMaterial& m, const json& overrides)
{
    auto it = overrides.find(m.name);
    if (it == overrides.end() || !it->is_array())
        return;
    std::vector<std::string> tags;
    for (const json& s : *it)
        if (s.is_string())
            tags.push_back(s.get<std::string>());
    m.tags = normalizeTags(tags);
}

SmartMaterial readSmartFile(const fs::path& path, const std::string& tier)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::invalid_argument("can't read " + paths::toUtf8(path));
    std::stringstream ss;
    ss << in.rdbuf();
    json j;
    try
    {
        j = json::parse(ss.str());
    }
    catch (const json::exception& e)
    {
        throw std::invalid_argument(paths::toUtf8(path) + ": " + e.what());
    }
    SmartMaterial m = SmartMaterial::fromJson(j);
    m.file = path;
    m.tier = tier;
    if (m.name.empty())
        m.name = path.stem().string();
    applyTagOverride(m, readTagOverrides());
    return m;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

/** The folders in tier order with their tier names; extension packs sit between built-in and the user's. */
std::vector<std::pair<fs::path, const char*>> tieredDirs()
{
    std::vector<std::pair<fs::path, const char*>> out = {{paths::resourcesDir() / "smartmaterials", "builtin"}};
    std::vector<std::string> exts;
    try
    {
        exts = extensions::ExtensionManager::get().loaded();
    }
    catch (const std::exception&)
    {
    }
    for (const std::string& id : exts)
        if (ext::Context* ctx = extensions::contextOf(id))
            out.push_back({ctx->resources() / "smartmaterials", "extension"});
    out.push_back({PrefStore::get().userDir() / "smartmaterials", "user"});
    if (Workspace::get().isOpen())
        out.push_back({Workspace::get().stateDir(false) / "smartmaterials", "workspace"});
    return out;
}

void renumber(Layer& l, LayerStack& stack, std::map<std::string, std::string>& ids)
{
    std::string fresh = stack.newId('L');
    ids[l.id] = fresh;
    l.id = fresh;
    for (Mask& m : l.masks)
        m.id = stack.newId('M');
    for (Layer& c : l.children)
        renumber(c, stack, ids);
}

void visitLayers(Layer& l, const std::function<void(Layer&)>& fn)
{
    fn(l);
    for (Layer& c : l.children)
        visitLayers(c, fn);
}
} // namespace

json SmartMaterial::toJson() const
{
    json j = {{"format", "lilasmart"}, {"version", 1}, {"name", name}, {"kind", kind}};
    if (!description.empty())
        j["description"] = description;
    if (!tags.empty())
        j["tags"] = tags;
    if (kind == "mask")
        j["masks"] = masks;
    else
        j["layer"] = layer;
    return j;
}

SmartMaterial SmartMaterial::fromJson(const json& j)
{
    if (!j.is_object() || j.value("format", std::string()) != "lilasmart")
        throw std::invalid_argument("not a lilasmart file");
    SmartMaterial m;
    m.name = j.value("name", std::string());
    m.description = j.value("description", std::string());
    m.kind = j.value("kind", std::string("material"));
    if (auto t = j.find("tags"); t != j.end() && t->is_array())
    {
        for (const json& s : *t)
            if (s.is_string())
                m.tags.push_back(s.get<std::string>());
        m.tags = normalizeTags(m.tags);
    }
    if (m.kind == "material")
    {
        m.layer = j.value("layer", json());
        Layer::fromJson(m.layer);
    }
    else if (m.kind == "mask")
    {
        m.masks = j.value("masks", json::array());
        if (!m.masks.is_array())
            throw std::invalid_argument("masks must be an array");
        for (const json& e : m.masks)
            Mask::fromJson(e);
    }
    else
        throw std::invalid_argument("kind must be material or mask");
    return m;
}

std::vector<fs::path> smartMaterialDirs()
{
    std::vector<fs::path> out;
    for (const auto& [dir, tier] : tieredDirs())
        out.push_back(dir);
    return out;
}

/** A smart material file read before, kept while the file's time and size stay the same: panels list them on every refresh. */
SmartMaterial readSmartFileCached(const fs::path& f, const std::string& tier)
{
    struct Entry
    {
        fs::file_time_type time;
        uintmax_t size = 0;
        std::string tier;
        SmartMaterial material;
    };
    static std::mutex mutex;
    static std::map<fs::path, Entry> cache;
    std::error_code ec;
    const fs::file_time_type time = fs::last_write_time(f, ec);
    const uintmax_t size = ec ? 0 : fs::file_size(f, ec);
    if (!ec)
    {
        std::scoped_lock lock(mutex);
        if (auto it = cache.find(f); it != cache.end() && it->second.time == time && it->second.size == size && it->second.tier == tier)
            return it->second.material;
    }
    SmartMaterial m = readSmartFile(f, tier);
    if (!ec)
    {
        std::scoped_lock lock(mutex);
        cache[f] = {time, size, tier, m};
    }
    return m;
}

std::vector<SmartMaterial> listSmartMaterials()
{
    std::map<std::string, SmartMaterial> byName;
    const auto dirs = tieredDirs();
    for (size_t i = 0; i < dirs.size(); ++i)
    {
        std::error_code ec;
        if (!fs::is_directory(dirs[i].first, ec))
            continue;
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(dirs[i].first, ec))
            if (e.path().extension() == kExt)
                files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const fs::path& f : files)
        {
            try
            {
                SmartMaterial m = readSmartFileCached(f, dirs[i].second);
                byName[m.name] = std::move(m);
            }
            catch (const std::exception&)
            {
            }
        }
    }
    const json overrides = readTagOverrides();
    std::vector<SmartMaterial> out;
    for (auto& [k, m] : byName)
    {
        applyTagOverride(m, overrides);
        out.push_back(std::move(m));
    }
    return out;
}

SmartMaterial loadSmartMaterial(const std::string& nameOrPath)
{
    const fs::path p = paths::fromUtf8(nameOrPath);
    std::error_code ec;
    if (p.has_extension() && fs::is_regular_file(p, ec))
        return readSmartFile(p, "file");
    const auto dirs = tieredDirs();
    for (size_t i = dirs.size(); i-- > 0;)
    {
        fs::path f = dirs[i].first / (nameOrPath + kExt);
        if (fs::is_regular_file(f, ec))
            return readSmartFile(f, dirs[i].second);
    }
    const std::string want = lower(nameOrPath);
    for (SmartMaterial& m : listSmartMaterials())
        if (lower(m.name) == want || lower(m.file.stem().string()) == want)
            return m;
    throw std::invalid_argument("no smart material or mask named " + nameOrPath);
}

void saveSmartMaterial(const SmartMaterial& m, const fs::path& path)
{
    std::error_code ec;
    if (path.has_parent_path())
        fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out)
        throw std::runtime_error("can't write " + paths::toUtf8(path));
    out << m.toJson().dump(2) << "\n";
}

std::vector<std::string> normalizeTags(const std::vector<std::string>& tags)
{
    std::vector<std::string> out;
    for (std::string t : tags)
    {
        t = lower(t);
        const auto a = t.find_first_not_of(" \t");
        if (a == std::string::npos)
            continue;
        t = t.substr(a, t.find_last_not_of(" \t") - a + 1);
        out.push_back(t);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void setSmartMaterialTags(const std::string& name, const std::vector<std::string>& tags)
{
    const SmartMaterial current = loadSmartMaterial(name);
    std::vector<std::string> fileTags;
    {
        std::ifstream in(current.file, std::ios::binary);
        try
        {
            fileTags = SmartMaterial::fromJson(json::parse(in)).tags;
        }
        catch (const std::exception&)
        {
        }
    }
    json overrides = readTagOverrides();
    const std::vector<std::string> want = normalizeTags(tags);
    if (want == fileTags)
        overrides.erase(current.name);
    else
        overrides[current.name] = want;
    std::error_code ec;
    fs::create_directories(tagsFile().parent_path(), ec);
    std::ofstream out(tagsFile(), std::ios::binary);
    if (!out)
        throw std::runtime_error("can't write " + paths::toUtf8(tagsFile()));
    out << overrides.dump(2) << '\n';
}

Layer instantiateLayer(const json& layerJson, LayerStack& stack)
{
    Layer l = Layer::fromJson(layerJson);
    std::map<std::string, std::string> ids;
    renumber(l, stack, ids);

    const std::map<std::string, std::string> existing = stack.anchors();
    std::map<std::string, std::string> anchors;
    visitLayers(l, [&](Layer& x) {
        if (x.kind == LayerKind::Instance)
            if (auto it = ids.find(x.source); it != ids.end())
                x.source = it->second;
        if (x.anchor.empty() || !existing.contains(x.anchor))
            return;
        std::string name = x.anchor;
        for (int n = 2; existing.contains(name) || anchors.contains(name); ++n)
            name = x.anchor + std::to_string(n);
        anchors[x.anchor] = name;
        x.anchor = name;
    });
    if (!anchors.empty())
        visitLayers(l, [&](Layer& x) {
            for (Mask& m : x.masks)
            {
                if (auto it = anchors.find(m.anchor); m.kind == MaskKind::Anchor && it != anchors.end())
                    m.anchor = it->second;
                for (auto& [port, role] : m.bindings)
                {
                    if (!role.starts_with("anchor:"))
                        continue;
                    const std::string ref = role.substr(7);
                    const auto dot = ref.find('.');
                    if (auto it = anchors.find(ref.substr(0, dot)); it != anchors.end())
                        role = "anchor:" + it->second + (dot == std::string::npos ? std::string() : ref.substr(dot));
                }
            }
        });
    return l;
}

std::vector<Mask> instantiateMasks(const json& masks, LayerStack& stack)
{
    std::vector<Mask> out;
    for (const json& j : masks)
    {
        Mask m = Mask::fromJson(j);
        m.id = stack.newId('M');
        out.push_back(std::move(m));
    }
    return out;
}
} // namespace lila::tex
