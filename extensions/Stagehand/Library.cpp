// The prop library's folders, scan and search.

#include "Library.h"

#include <lila/Log.h>
#include <lila/Prefs.h>
#include <lila/Profile.h>
#include <lila/Workspace.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <tuple>

namespace fs = std::filesystem;

namespace stagehand::library
{
namespace
{
constexpr const char* kModelExtensions[] = {".glb", ".gltf", ".fbx", ".obj", ".lila", ".lilaa"};

struct Scan
{
    bool done = false;
    std::vector<Asset> assets;
    std::vector<std::string> errors;
} gScan;

std::string lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> words(const std::string& text)
{
    std::vector<std::string> out;
    std::string w;
    for (char c : text)
    {
        if (std::isalnum(static_cast<unsigned char>(c)))
            w += char(std::tolower(static_cast<unsigned char>(c)));
        else if (!w.empty())
        {
            out.push_back(w);
            w.clear();
        }
    }
    if (!w.empty())
        out.push_back(w);
    return out;
}

/** Splits a path list on ';' (and ':' outside Windows drive letters). */
std::vector<std::string> splitPaths(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        const bool driveColon = c == ':' && cur.size() == 1 && std::isalpha(static_cast<unsigned char>(cur[0]));
        if (c == ';' || (c == ':' && !driveColon))
        {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        }
        else
            cur += c;
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

/** The model file an asset folder names, or the first model file in it. */
fs::path modelIn(const fs::path& dir, const json& meta)
{
    if (meta.contains("model") && meta["model"].is_string())
    {
        const fs::path p = dir / utf8Path(meta["model"].get<std::string>());
        if (fs::is_regular_file(p))
            return p;
    }
    for (const char* ext : kModelExtensions)
        for (const fs::directory_entry& e : fs::directory_iterator(dir))
            if (e.is_regular_file() && lower(e.path().extension().string()) == ext)
                return e.path();
    return {};
}

void scanRoot(const Root& r, std::map<std::string, Asset>& byId, std::vector<std::string>& order, std::vector<std::string>& errors)
{
    std::error_code ec;
    for (fs::recursive_directory_iterator it(r.dir, ec), end; it != end; it.increment(ec))
    {
        if (ec)
            break;
        if (!it->is_regular_file() || it->path().filename() != "asset.json")
            continue;
        const fs::path dir = it->path().parent_path();
        try
        {
            std::ifstream f(it->path());
            const json meta = json::parse(f);
            Asset a;
            const fs::path rel = fs::relative(dir, r.dir);
            a.id = rel.generic_string();
            a.collection = rel.has_parent_path() ? rel.parent_path().generic_string() : std::string();
            a.name = meta.value("name", dir.filename().string());
            a.tier = r.tier;
            a.dir = dir;
            a.model = modelIn(dir, meta);
            if (meta.contains("assembly") && meta["assembly"].is_object())
                a.assembly = meta["assembly"];
            if (a.model.empty() && a.assembly.is_null())
                throw std::runtime_error("no model file");
            if (fs::exists(dir / "thumb.png"))
                a.thumbnail = dir / "thumb.png";
            if (meta.contains("tags") && meta["tags"].is_array())
                for (const json& t : meta["tags"])
                    if (t.is_string())
                        a.tags.push_back(t.get<std::string>());
            a.description = meta.value("description", std::string());
            if (meta.contains("size") && meta["size"].is_array() && meta["size"].size() == 3)
                a.size = {meta["size"][0].get<double>(), meta["size"][1].get<double>(), meta["size"][2].get<double>()};
            if (meta.contains("physics") && meta["physics"].is_object())
                a.physics = meta["physics"];
            a.unreal = meta.value("unreal", std::string());
            if (meta.contains("snapPoints") && meta["snapPoints"].is_array())
                a.snapPoints = meta["snapPoints"];
            if (byId.count(a.id))
                continue;
            order.push_back(a.id);
            byId.emplace(a.id, std::move(a));
        }
        catch (const std::exception& e)
        {
            errors.push_back(it->path().generic_string() + ": " + e.what());
        }
    }
}

void ensureScanned()
{
    if (gScan.done)
        return;
    LILA_ZONE();

    gScan = {};
    if (starterEnabled())
    {
        try
        {
            buildStarter(false);
        }
        catch (const std::exception& e)
        {
            gScan.errors.push_back(starterDir().generic_string() + ": " + e.what());
        }
    }
    std::map<std::string, Asset> byId;
    std::vector<std::string> order;
    for (const Root& r : roots())
        scanRoot(r, byId, order, gScan.errors);
    for (const std::string& id : order)
        gScan.assets.push_back(byId.at(id));
    std::sort(gScan.assets.begin(), gScan.assets.end(), [](const Asset& a, const Asset& b) {
        return std::tie(a.collection, a.name, a.id) < std::tie(b.collection, b.name, b.id);
    });
    for (const std::string& e : gScan.errors)
        lila::log::write(lila::LogLevel::Warning, "Library: " + e);
    gScan.done = true;
}

/** Within one insertion, deletion or substitution. */
bool oneEditApart(const std::string& a, const std::string& b)
{
    if (a.size() < 4 || b.size() < 4)
        return false;
    const size_t la = a.size(), lb = b.size();
    if (la > lb + 1 || lb > la + 1)
        return false;
    size_t i = 0, j = 0;
    bool edited = false;
    while (i < la && j < lb)
    {
        if (a[i] == b[j])
        {
            ++i, ++j;
            continue;
        }
        if (edited)
            return false;
        edited = true;
        if (la > lb)
            ++i;
        else if (lb > la)
            ++j;
        else
            ++i, ++j;
    }
    return true;
}
} // namespace

json Asset::toJson() const
{
    return {{"id", id},
            {"name", name},
            {"collection", collection},
            {"tier", tier},
            {"dir", dir.generic_string()},
            {"model", model.generic_string()},
            {"thumbnail", thumbnail.generic_string()},
            {"tags", tags},
            {"description", description},
            {"size", json::array({size.x, size.y, size.z})},
            {"physics", physics},
            {"unreal", unreal},
            {"snapPoints", snapPoints},
            {"assembly", assembly}};
}

namespace
{
std::vector<Root>& extensionRoots()
{
    static std::vector<Root> list;
    return list;
}
} // namespace

void addExtensionRoot(const std::string& tier, const fs::path& dir)
{
    for (const Root& r : extensionRoots())
        if (r.dir == dir)
            return;
    extensionRoots().push_back({tier, dir});
    reload();
}

void removeExtensionRoot(const fs::path& dir)
{
    std::erase_if(extensionRoots(), [&](const Root& r) { return r.dir == dir; });
    reload();
}

std::vector<Root> roots(bool existingOnly)
{
    std::vector<Root> out;
    auto add = [&](const std::string& tier, const fs::path& dir) {
        std::error_code ec;
        if (dir.empty() || (existingOnly && !fs::is_directory(dir, ec)))
            return;
        for (const Root& r : out)
            if (fs::equivalent(r.dir, dir, ec))
                return;
        out.push_back({tier, dir});
    };
    add("workspace", tierDir("workspace"));
    const json paths = lila::PrefStore::get().value("stagehand", "/library/paths", json::array());
    if (paths.is_array())
        for (const json& p : paths)
            if (p.is_string())
                add("studio", utf8Path(p.get<std::string>()));
    if (const char* env = std::getenv("LILA_LIBRARY_PATH"))
        for (const std::string& p : splitPaths(env))
            add("studio", utf8Path(p));
    add("user", tierDir("user"));
    if (starterEnabled())
        add("starter", starterDir());
    for (const Root& r : extensionRoots())
        add(r.tier, r.dir);
    return out;
}

fs::path tierDir(const std::string& tier)
{
    if (tier == "workspace")
        return lila::Workspace::get().isOpen() ? lila::Workspace::get().root() / "Library" : fs::path();
    if (tier == "user")
        return lila::PrefStore::get().userDir() / "library";
    if (tier == "starter")
        return starterDir();
    if (tier.empty() || tier == "studio")
        return {};
    return utf8Path(tier);
}

fs::path utf8Path(std::string_view utf8)
{
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

const std::vector<Asset>& all()
{
    ensureScanned();
    return gScan.assets;
}

std::optional<Asset> find(const std::string& id)
{
    for (const Asset& a : all())
        if (a.id == id)
            return a;
    // A bare name finds the asset when only one has it.
    std::optional<Asset> only;
    for (const Asset& a : all())
        if (lower(a.name) == lower(id) || lower(fs::path(a.id).filename().string()) == lower(id))
        {
            if (only)
                return std::nullopt;
            only = a;
        }
    return only;
}

void reload() { gScan.done = false; }

const std::vector<std::string>& errors()
{
    ensureScanned();
    return gScan.errors;
}

double wordScore(const std::string& word, const std::string& field)
{
    double best = 0.0;
    for (const std::string& w : words(field))
    {
        if (w == word)
            return 1.0;
        if (w.rfind(word, 0) == 0)
            best = std::max(best, 0.8);
        else if (w.find(word) != std::string::npos)
            best = std::max(best, 0.5);
        else if (oneEditApart(w, word))
            best = std::max(best, 0.4);
    }
    return best;
}

std::vector<Asset> search(const Query& q)
{
    LILA_ZONE();

    const std::vector<std::string> want = words(q.text);
    std::vector<std::pair<double, const Asset*>> scored;
    for (const Asset& a : all())
    {
        if (!q.tier.empty() && a.tier != q.tier)
            continue;
        if (!q.collection.empty() && a.collection != q.collection && a.collection.rfind(q.collection + "/", 0) != 0)
            continue;
        bool tagged = true;
        for (const std::string& t : q.tags)
            tagged = tagged && std::any_of(a.tags.begin(), a.tags.end(), [&](const std::string& x) { return lower(x) == lower(t); });
        if (!tagged)
            continue;
        const double side = std::max({a.size.x, a.size.y, a.size.z});
        if ((q.maxSize > 0.0 && side > q.maxSize) || (q.minSize > 0.0 && side < q.minSize))
            continue;
        double score = 0.0;
        bool matched = true;
        for (const std::string& w : want)
        {
            // The name counts most, then tags, then the rest.
            std::string tagText;
            for (const std::string& t : a.tags)
                tagText += t + " ";
            const double s = std::max({wordScore(w, a.name) * 1.0, wordScore(w, tagText) * 0.8, wordScore(w, a.collection) * 0.6,
                                       wordScore(w, a.description) * 0.4, wordScore(w, a.id) * 0.6});
            if (s <= 0.0)
            {
                matched = false;
                break;
            }
            score += s;
        }
        if (matched)
            scored.emplace_back(score, &a);
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    std::vector<Asset> out;
    for (const auto& [s, a] : scored)
    {
        out.push_back(*a);
        if (q.limit && out.size() >= q.limit)
            break;
    }
    return out;
}

std::vector<std::string> collections()
{
    std::set<std::string> s;
    for (const Asset& a : all())
        if (!a.collection.empty())
            s.insert(a.collection);
    return {s.begin(), s.end()};
}

std::vector<std::string> tags()
{
    std::set<std::string> s;
    for (const Asset& a : all())
        for (const std::string& t : a.tags)
            s.insert(lower(t));
    return {s.begin(), s.end()};
}
} // namespace stagehand::library
