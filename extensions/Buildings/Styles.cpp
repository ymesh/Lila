// The style library: shipped JSON styles, the user's and the workspace's, read into one table any thread can look up.

#include "BuildingsScene.h"

#include <lila/Log.h>
#include <lila/Prefs.h>
#include <lila/Workspace.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <mutex>

namespace fs = std::filesystem;

namespace buildings
{
namespace
{
std::mutex& libMutex()
{
    static std::mutex m;
    return m;
}
std::map<std::string, std::shared_ptr<const Style>>& lib()
{
    static std::map<std::string, std::shared_ptr<const Style>> l;
    return l;
}
/** Shipped id -> the user or workspace style that replaces it. */
std::map<std::string, std::shared_ptr<const Style>>& replacements()
{
    static std::map<std::string, std::shared_ptr<const Style>> r;
    return r;
}
fs::path& shippedPath()
{
    static fs::path p;
    return p;
}
fs::path& loadedWorkspaceDir()
{
    static fs::path p;
    return p;
}
std::atomic<uint64_t> gRevision{1};

/** Lowercase words of a camelCase, snake or spaced name. */
std::vector<std::string> nameWords(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalpha(c) || (std::isupper(c) && !cur.empty()))
        {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        }
        if (std::isalpha(c))
            cur += char(std::tolower(c));
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

void syncWorkspace()
{
    const fs::path ws = workspaceStyleDir();
    {
        std::lock_guard lock(libMutex());
        if (ws == loadedWorkspaceDir())
            return;
    }
    reloadStyles();
}

void readDir(const fs::path& dir, const std::string& source, std::map<std::string, std::shared_ptr<const Style>>& into)
{
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec))
        return;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".json")
            files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files)
    {
        try
        {
            std::ifstream in(f);
            const json j = json::parse(in);
            std::string err;
            Style s = Style::fromJson(j, &err);
            if (!err.empty())
            {
                lila::log::warn("Buildings: {}: {}", f.generic_string(), err);
                continue;
            }
            s.source = source;
            const std::string id = s.id;
            into[id] = std::make_shared<const Style>(std::move(s));
        }
        catch (const std::exception& e)
        {
            lila::log::warn("Buildings: style {} unreadable: {}", f.generic_string(), e.what());
        }
    }
}
} // namespace

fs::path userStyleDir() { return lila::PrefStore::get().userDir() / "building-styles"; }

fs::path workspaceStyleDir()
{
    const lila::Workspace& ws = lila::Workspace::get();
    return ws.isOpen() ? ws.root() / "BuildingStyles" : fs::path();
}

void loadStyles(const fs::path& shippedDir)
{
    shippedPath() = shippedDir;
    reloadStyles();
}

void reloadStyles()
{
    std::map<std::string, std::shared_ptr<const Style>> next;
    const fs::path ws = workspaceStyleDir();
    readDir(shippedPath(), "shipped", next);
    readDir(userStyleDir(), "user", next);
    readDir(ws, "workspace", next);
    {
        std::lock_guard lock(libMutex());
        // Workspace styles are read last, so their replacements win over the user's.
        std::map<std::string, std::shared_ptr<const Style>> repl;
        for (const char* src : {"user", "workspace"})
            for (auto& [id, s] : next)
                if (s->source == src)
                    for (const std::string& r : s->replaces)
                        repl[r] = s;
        lib() = std::move(next);
        replacements() = std::move(repl);
        loadedWorkspaceDir() = ws;
    }
    ++gRevision;
    clearGenerationCache();
}

std::shared_ptr<const Style> findStyle(const std::string& id)
{
    syncWorkspace();
    std::lock_guard lock(libMutex());
    if (auto r = replacements().find(id); r != replacements().end())
        return r->second;
    auto it = lib().find(id);
    return it == lib().end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<const Style>> allStyles()
{
    syncWorkspace();
    std::vector<std::shared_ptr<const Style>> out;
    {
        std::lock_guard lock(libMutex());
        for (auto& [id, s] : lib())
            out.push_back(s);
    }
    // Shipped ones in menu order first, then the rest by id.
    const std::vector<std::string>& order = shippedStyleIds();
    std::stable_sort(out.begin(), out.end(), [&](const auto& a, const auto& b) {
        auto ia = std::find(order.begin(), order.end(), a->id), ib = std::find(order.begin(), order.end(), b->id);
        if (ia != ib)
            return ia < ib;
        return a->id < b->id;
    });
    return out;
}

std::shared_ptr<const Style> nearestStyle(const std::string& id)
{
    const std::vector<std::string> words = nameWords(id);
    std::shared_ptr<const Style> best, residential;
    int bestScore = 0;
    for (const auto& s : allStyles())
    {
        if (s->source != "shipped")
            continue;
        if (s->id == "residential")
            residential = s;
        const std::vector<std::string> own = nameWords(s->id);
        std::vector<std::string> tagged;
        for (const std::string& t : s->tags)
            for (std::string& w : nameWords(t))
                tagged.push_back(std::move(w));
        int score = 0;
        for (const std::string& w : words)
        {
            if (std::find(own.begin(), own.end(), w) != own.end())
                score += 2;
            else if (std::find(tagged.begin(), tagged.end(), w) != tagged.end())
                score += 1;
        }
        if (score > bestScore)
        {
            bestScore = score;
            best = s;
        }
    }
    return best ? best : residential;
}

uint64_t stylesRevision() { return gRevision.load(); }

fs::path saveStyle(const Style& s, const std::string& scope)
{
    const fs::path dir = scope == "workspace" ? workspaceStyleDir() : userStyleDir();
    if (dir.empty())
        throw std::runtime_error("no workspace is open; save the style to the user folder");
    fs::create_directories(dir);
    std::string file;
    for (char c : s.id)
        file += std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ? c : '_';
    const fs::path f = dir / (file + ".json");
    std::ofstream out(f);
    out << s.toJson().dump(2);
    out.close();
    reloadStyles();
    return f;
}
} // namespace buildings
