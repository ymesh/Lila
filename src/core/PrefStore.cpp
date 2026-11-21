#include "core/PrefStore.h"

#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/Paths.h"
#include "core/Profile.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <unordered_map>
#include <mutex>
#include <sstream>
#include <thread>

namespace lila
{
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace
{
constexpr auto kWriteDelay = std::chrono::milliseconds(200);
constexpr auto kPollInterval = std::chrono::milliseconds(250);
// Studio folders are usually on a network share; checking them every tick would hammer it.
constexpr auto kStudioPollInterval = std::chrono::seconds(5);
constexpr const char* kLockedKey = "$locked";

enum LayerIndex
{
    Defaults,
    User,
    Workspace,
    LayerCount
};

struct Doc
{
    json data = json::object();
    bool loaded = false;
    bool dirty = false;
    Clock::time_point dirtyAt;
    fs::file_time_type mtime{};
    std::string onDisk; // exact text last read or written, to ignore our own writes
};

struct Layer
{
    fs::path dir;
    std::map<std::string, Doc, std::less<>> docs;
    /** Studio layers: the files the folder held at the last poll, to notice files added or removed. */
    std::vector<std::string> files;
    bool scanned = false;
};

/** Every .json under dir, as pref file names ("shelves/Rigging"), sorted. */
std::vector<std::string> listPrefFiles(const fs::path& dir)
{
    std::vector<std::string> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (!it->is_regular_file(ec) || it->path().extension() != ".json")
            continue;
        fs::path rel = fs::relative(it->path(), dir, ec);
        rel.replace_extension();
        out.push_back(rel.generic_string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<std::string> readText(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeAtomic(const fs::path& path, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << text;
        if (!out.flush())
            return false;
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

json::json_pointer pointer(std::string_view key)
{
    if (key.empty() || key == "/")
        return json::json_pointer();
    return json::json_pointer(key.front() == '/' ? std::string(key) : "/" + std::string(key));
}

/** True when `locked` is `key` or one of its parents ("/fileDialog" locks "/fileDialog/delete"). */
bool lockCovers(const std::string& locked, const std::string& key)
{
    const std::string l = locked.empty() || locked.front() == '/' ? locked : "/" + locked;
    return key == l || l == "/" || l.empty() || (key.starts_with(l) && key.size() > l.size() && key[l.size()] == '/');
}

/** LILA_STUDIO_PREFS (';' separated on Windows, ':' elsewhere), else the "prefs" list of studio.json beside the exe. */
std::vector<fs::path> configuredStudioDirs()
{
    std::vector<fs::path> dirs;
    if (const char* env = std::getenv("LILA_STUDIO_PREFS"); env && *env)
    {
#ifdef _WIN32
        constexpr char sep = ';';
#else
        constexpr char sep = ':';
#endif
        std::string_view rest = env;
        while (!rest.empty())
        {
            const size_t at = rest.find(sep);
            if (std::string_view part = rest.substr(0, at); !part.empty())
                dirs.push_back(paths::fromUtf8(part));
            rest = at == std::string_view::npos ? std::string_view() : rest.substr(at + 1);
        }
        return dirs;
    }
    const fs::path config = paths::executableDir() / "studio.json";
    if (auto text = readText(config))
    {
        json j = json::parse(*text, nullptr, false, true);
        if (j.is_object() && j.contains("prefs") && j["prefs"].is_array())
            for (const json& d : j["prefs"])
                if (d.is_string())
                {
                    fs::path dir = paths::fromUtf8(d.get<std::string>());
                    dirs.push_back(dir.is_absolute() ? dir : paths::executableDir() / dir);
                }
    }
    return dirs;
}
} // namespace

struct PrefStore::Impl
{
    mutable std::mutex mutex;
    std::condition_variable cv;
    mutable std::array<Layer, LayerCount> layers;
    /** Read-only studio folders, most specific first. They sit between the defaults and the user's prefs. */
    mutable std::vector<Layer> studio;
    /** Last good copy of each studio file, so a share that is offline still applies its values and locks. */
    fs::path studioCache;
    Clock::time_point studioPolled;
    std::map<int, ChangeFn> listeners;
    /** value() results by file and key, dropped whenever any document, layer or default changes. */
    mutable std::unordered_map<std::string, json> memo;
    /** setDefaults, per file, oldest first. */
    std::map<std::string, std::vector<std::pair<std::string, json>>, std::less<>> contributed;
    int nextListener = 1;
    std::jthread io;
    bool flushRequested = false;
    std::condition_variable flushed;

    fs::path pathFor(const Layer& layer, std::string_view file) const
    {
        return layer.dir / (std::string(file) + ".json");
    }

    Doc& doc(int layerIndex, std::string_view file) const
    {
        Layer& layer = layers[layerIndex];
        auto it = layer.docs.find(file);
        if (it == layer.docs.end())
            it = layer.docs.emplace(std::string(file), Doc{}).first;
        Doc& d = it->second;
        if (!d.loaded)
            load(layer, file, d);
        return d;
    }

    Doc& studioDoc(size_t i, std::string_view file) const
    {
        Layer& layer = studio[i];
        auto it = layer.docs.find(file);
        if (it == layer.docs.end())
            it = layer.docs.emplace(std::string(file), Doc{}).first;
        Doc& d = it->second;
        if (!d.loaded)
            loadStudio(layer, file, d);
        return d;
    }

    fs::path studioCachePath(const Layer& layer, std::string_view file) const
    {
        if (studioCache.empty())
            return {};
        const size_t h = std::hash<std::string>()(paths::toUtf8(layer.dir));
        return studioCache / std::to_string(h) / (std::string(file) + ".json");
    }

    /** Reads a studio file and refreshes its cached copy; when the folder can't be reached, uses that copy. */
    void loadStudio(const Layer& layer, std::string_view file, Doc& d) const
    {
        d.loaded = true;
        const fs::path path = pathFor(layer, file);
        const fs::path cache = studioCachePath(layer, file);
        std::error_code ec;
        if (!fs::is_directory(layer.dir, ec))
        {
            if (auto text = cache.empty() ? std::nullopt : readText(cache))
                parseInto(cache, *text, d);
            return;
        }
        d.mtime = fs::last_write_time(path, ec);
        auto text = readText(path);
        if (!text)
        {
            if (!cache.empty())
                fs::remove(cache, ec);
            return;
        }
        d.onDisk = *text;
        if (parseInto(path, *text, d) && !cache.empty() && readText(cache) != text)
            writeAtomic(cache, *text);
    }

    bool isLocked(std::string_view file, const std::string& key) const
    {
        for (size_t i = 0; i < studio.size(); ++i)
        {
            const Doc& d = studioDoc(i, file);
            auto it = d.data.find(kLockedKey);
            if (it == d.data.end() || !it->is_array())
                continue;
            for (const json& l : *it)
                if (l.is_string() && lockCovers(l.get<std::string>(), key))
                    return true;
        }
        return false;
    }

    void pollStudio(std::vector<std::string>& changed);

    void load(const Layer& layer, std::string_view file, Doc& d) const
    {
        d.loaded = true;
        if (layer.dir.empty())
            return;
        fs::path path = pathFor(layer, file);
        std::error_code ec;
        d.mtime = fs::last_write_time(path, ec);
        if (auto text = readText(path))
        {
            d.onDisk = *text;
            parseInto(path, *text, d);
        }
    }

    static bool parseInto(const fs::path& path, const std::string& text, Doc& d)
    {
        json parsed = json::parse(text, nullptr, false, true);
        if (parsed.is_discarded() || !parsed.is_object())
        {
            log::warn("prefs: {} is not a valid JSON object, keeping previous values", path.string());
            return false;
        }
        d.data = std::move(parsed);
        return true;
    }

    void notify(const std::string& file)
    {
        mainq::post("prefs change listeners", [this, file] {
            std::map<int, ChangeFn> copy;
            {
                std::scoped_lock lock(mutex);
                copy = listeners;
            }
            for (auto& [id, fn] : copy)
                fn(file);
        });
    }

    void ioLoop(std::stop_token stop)
    {
        LILA_THREAD_NAME("Prefs IO");
        std::unique_lock lock(mutex);
        while (!stop.stop_requested())
        {
            cv.wait_for(lock, kPollInterval);
            writeDirty(lock, flushRequested);
            pollExternal(lock);
            if (flushRequested)
            {
                flushRequested = false;
                flushed.notify_all();
            }
        }
        writeDirty(lock, true);
    }

    void writeDirty(std::unique_lock<std::mutex>& lock, bool force)
    {
        LILA_ZONE();

        struct Pending
        {
            fs::path path;
            std::string text;
            Doc* doc;
        };
        std::vector<Pending> pending;
        auto now = Clock::now();
        for (int li = User; li < LayerCount; ++li)
        {
            Layer& layer = layers[li];
            if (layer.dir.empty())
                continue;
            for (auto& [file, d] : layer.docs)
            {
                if (!d.dirty || (!force && now - d.dirtyAt < kWriteDelay))
                    continue;
                d.dirty = false;
                std::string text = d.data.dump(2) + "\n";
                d.onDisk = text;
                pending.push_back({pathFor(layer, file), std::move(text), &d});
            }
        }
        if (pending.empty())
            return;

        lock.unlock();
        for (Pending& p : pending)
            if (!writeAtomic(p.path, p.text))
                log::error("prefs: failed to write {}", p.path.string());
        lock.lock();

        for (Pending& p : pending)
        {
            std::error_code ec;
            p.doc->mtime = fs::last_write_time(p.path, ec);
        }
    }

    void pollExternal(std::unique_lock<std::mutex>& lock)
    {
        LILA_ZONE();

        std::vector<std::string> changed;
        if (Clock::now() - studioPolled >= kStudioPollInterval)
        {
            studioPolled = Clock::now();
            pollStudio(changed);
        }
        for (int li = User; li < LayerCount; ++li)
        {
            Layer& layer = layers[li];
            if (layer.dir.empty())
                continue;
            for (auto& [file, d] : layer.docs)
            {
                if (d.dirty)
                    continue;
                fs::path path = pathFor(layer, file);
                std::error_code ec;
                auto mtime = fs::last_write_time(path, ec);
                if (ec || mtime == d.mtime)
                    continue;
                d.mtime = mtime;
                auto text = readText(path);
                if (!text || *text == d.onDisk)
                    continue;
                d.onDisk = *text;
                if (parseInto(path, *text, d))
                    changed.push_back(file);
            }
        }

        if (!changed.empty())
            memo.clear();
        lock.unlock();
        std::sort(changed.begin(), changed.end());
        changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
        for (const std::string& file : changed)
        {
            log::info("prefs: reloaded {}", file);
            notify(file);
        }
        lock.lock();
    }
};

void PrefStore::Impl::pollStudio(std::vector<std::string>& changed)
{
    for (Layer& layer : studio)
    {
        std::error_code ec;
        if (!fs::is_directory(layer.dir, ec))
            continue; // offline: keep what was loaded
        std::vector<std::string> files = listPrefFiles(layer.dir);
        if (layer.scanned)
            std::set_symmetric_difference(layer.files.begin(), layer.files.end(), files.begin(), files.end(),
                                          std::back_inserter(changed));
        layer.files = std::move(files);
        layer.scanned = true;
        for (auto& [file, d] : layer.docs)
        {
            const fs::path path = pathFor(layer, file);
            auto mtime = fs::last_write_time(path, ec);
            if (ec)
                mtime = {};
            if (mtime == d.mtime)
                continue;
            d.mtime = mtime;
            auto text = readText(path);
            if (text.value_or("") == d.onDisk)
                continue;
            d.onDisk = text.value_or("");
            if (!text)
                d.data = json::object();
            else if (!parseInto(path, *text, d))
                continue;
            if (const fs::path cache = studioCachePath(layer, file); !cache.empty())
            {
                if (text)
                    writeAtomic(cache, *text);
                else
                    fs::remove(cache, ec);
            }
            changed.push_back(file);
        }
    }
}

PrefStore::PrefStore() : m(std::make_unique<Impl>())
{
    m->layers[Defaults].dir = paths::resourcesDir() / "defaults";
    m->layers[User].dir = paths::userConfigDir() / "prefs";
    m->studioCache = paths::userConfigDir() / "studioCache";
    for (const fs::path& dir : configuredStudioDirs())
        m->studio.push_back(Layer{dir, {}});
    for (const Layer& l : m->studio)
        log::info("prefs: studio folder {}", paths::toUtf8(l.dir));
}

PrefStore::~PrefStore()
{
    stop();
}

PrefStore& PrefStore::get()
{
    static PrefStore store;
    return store;
}

void PrefStore::start()
{
    if (m->io.joinable())
        return;
    m->io = std::jthread([this](std::stop_token st) { m->ioLoop(st); });
}

void PrefStore::stop()
{
    if (!m->io.joinable())
        return;
    m->io.request_stop();
    m->cv.notify_all();
    m->io.join();
}

void PrefStore::setWorkspaceDir(const fs::path& lilaDir)
{
    {
        std::unique_lock lock(m->mutex);
        m->writeDirty(lock, true);
        m->layers[Workspace] = Layer{lilaDir, {}};
        m->memo.clear();
    }
}

json PrefStore::value(std::string_view file, std::string_view key, const json& fallback) const
{
    std::scoped_lock lock(m->mutex);
    std::string memoKey;
    memoKey.reserve(file.size() + key.size() + 1);
    memoKey.append(file).push_back('|');
    memoKey.append(key);
    if (auto it = m->memo.find(memoKey); it != m->memo.end())
        return it->second.is_discarded() ? fallback : it->second;
    json found = valueUncached(file, key);
    auto& slot = m->memo[std::move(memoKey)];
    slot = std::move(found);
    return slot.is_discarded() ? fallback : slot;
}

json PrefStore::valueUncached(std::string_view file, std::string_view key) const
{
    auto ptr = pointer(key);
    const bool locked = m->isLocked(file, ptr.to_string());
    for (int li = LayerCount - 1; li >= 0; --li)
    {
        if (li == Defaults)
        {
            for (size_t i = 0; i < m->studio.size(); ++i)
                if (const Doc& d = m->studioDoc(i, file); d.data.contains(ptr))
                    return d.data[ptr];
            if (auto c = m->contributed.find(file); c != m->contributed.end())
                for (auto it = c->second.rbegin(); it != c->second.rend(); ++it)
                    if (it->second.contains(ptr))
                        return it->second[ptr];
        }
        if (m->layers[li].dir.empty() || (locked && li != Defaults))
            continue;
        const Doc& d = m->doc(li, file);
        if (d.data.contains(ptr))
            return d.data[ptr];
    }
    return json(json::value_t::discarded);
}

bool PrefStore::isLocked(std::string_view file, std::string_view key) const
{
    std::scoped_lock lock(m->mutex);
    return m->isLocked(file, pointer(key).to_string());
}

void PrefStore::setStudioDirs(std::vector<fs::path> dirs)
{
    std::vector<std::string> files;
    {
        std::scoped_lock lock(m->mutex);
        for (const Layer& l : m->studio)
            for (auto& [file, d] : l.docs)
                files.push_back(file);
        m->studio.clear();
        for (fs::path& dir : dirs)
            m->studio.push_back(Layer{std::move(dir), {}});
        m->memo.clear();
    }
    for (const std::string& f : files)
        m->notify(f);
}

std::vector<std::string> PrefStore::studioFiles(std::string_view prefix) const
{
    std::scoped_lock lock(m->mutex);
    std::vector<std::string> out;
    for (const Layer& l : m->studio)
    {
        std::error_code ec;
        fs::path root = fs::is_directory(l.dir, ec) ? l.dir : m->studioCachePath(l, "x").parent_path();
        if (root.empty() || !fs::is_directory(root, ec))
            continue;
        for (std::string& name : listPrefFiles(root))
            if (name.starts_with(prefix))
                out.push_back(std::move(name));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<fs::path> PrefStore::studioDirs() const
{
    std::scoped_lock lock(m->mutex);
    std::vector<fs::path> out;
    for (const Layer& l : m->studio)
        out.push_back(l.dir);
    return out;
}

json PrefStore::merged(std::string_view file) const
{
    std::scoped_lock lock(m->mutex);
    json out = json::object();
    json under; // defaults, contributions and studio: what locked keys resolve to
    json locked = json::array();
    for (int li = 0; li < LayerCount; ++li)
    {
        if (!m->layers[li].dir.empty())
            out.merge_patch(m->doc(li, file).data);
        if (li != Defaults)
            continue;
        if (auto c = m->contributed.find(file); c != m->contributed.end())
            for (auto& [layer, data] : c->second)
                out.merge_patch(data);
        for (size_t i = m->studio.size(); i-- > 0;)
        {
            json data = m->studioDoc(i, file).data;
            if (auto it = data.find(kLockedKey); it != data.end())
            {
                if (it->is_array())
                    locked.insert(locked.end(), it->begin(), it->end());
                data.erase(it);
            }
            out.merge_patch(data);
        }
        under = out;
    }
    for (const json& l : locked)
    {
        if (!l.is_string())
            continue;
        const auto ptr = pointer(l.get<std::string>());
        if (ptr == json::json_pointer())
            out = under;
        else if (under.contains(ptr))
            out[ptr] = under[ptr];
        else if (out.contains(ptr))
            out[ptr.parent_pointer()].erase(ptr.back());
    }
    return out;
}

void PrefStore::setDefaults(std::string_view file, std::string layer, json data)
{
    {
        std::scoped_lock lock(m->mutex);
        auto& list = m->contributed[std::string(file)];
        auto it = std::find_if(list.begin(), list.end(), [&](auto& e) { return e.first == layer; });
        if (it != list.end())
            it->second = std::move(data);
        else
            list.emplace_back(std::move(layer), std::move(data));
        m->memo.clear();
    }
    m->notify(std::string(file));
}

bool PrefStore::removeDefaults(std::string_view file, std::string_view layer)
{
    {
        std::scoped_lock lock(m->mutex);
        auto c = m->contributed.find(file);
        if (c == m->contributed.end())
            return false;
        size_t n = std::erase_if(c->second, [&](auto& e) { return e.first == layer; });
        if (c->second.empty())
            m->contributed.erase(c);
        if (!n)
            return false;
        m->memo.clear();
    }
    m->notify(std::string(file));
    return true;
}

size_t PrefStore::removeDefaultsWithPrefix(std::string_view prefix)
{
    std::vector<std::string> files;
    size_t count = 0;
    {
        std::scoped_lock lock(m->mutex);
        for (auto c = m->contributed.begin(); c != m->contributed.end();)
        {
            size_t n = std::erase_if(c->second, [&](auto& e) { return e.first.starts_with(prefix); });
            if (n)
                files.push_back(c->first);
            count += n;
            c = c->second.empty() ? m->contributed.erase(c) : std::next(c);
        }
        if (count)
            m->memo.clear();
    }
    for (const std::string& f : files)
        m->notify(f);
    return count;
}

std::vector<std::string> PrefStore::contributedFiles(std::string_view prefix) const
{
    std::scoped_lock lock(m->mutex);
    std::vector<std::string> out;
    for (auto& [file, list] : m->contributed)
        if (file.starts_with(prefix))
            out.push_back(file);
    return out;
}

bool PrefStore::hasContributed(std::string_view file) const
{
    std::scoped_lock lock(m->mutex);
    return m->contributed.find(file) != m->contributed.end();
}

void PrefStore::set(std::string_view file, std::string_view key, json value, PrefScope scope)
{
    {
        std::scoped_lock lock(m->mutex);
        auto ptr = pointer(key);
        if (m->isLocked(file, ptr.to_string()))
            return;
        int li = scope == PrefScope::Workspace && !m->layers[Workspace].dir.empty() ? Workspace : User;
        Doc& d = m->doc(li, file);
        if (d.data.contains(ptr) && d.data[ptr] == value)
            return;
        d.data[ptr] = std::move(value);
        m->memo.clear();
        d.dirty = true;
        d.dirtyAt = Clock::now();
    }
    m->cv.notify_all();
    m->notify(std::string(file));
}

void PrefStore::remove(std::string_view file, std::string_view key, PrefScope scope)
{
    {
        std::scoped_lock lock(m->mutex);
        int li = scope == PrefScope::Workspace && !m->layers[Workspace].dir.empty() ? Workspace : User;
        Doc& d = m->doc(li, file);
        auto ptr = pointer(key);
        if (!d.data.contains(ptr))
            return;
        d.data[ptr.parent_pointer()].erase(ptr.back());
        m->memo.clear();
        d.dirty = true;
        d.dirtyAt = Clock::now();
    }
    m->cv.notify_all();
    m->notify(std::string(file));
}

json PrefStore::userDoc(std::string_view file) const
{
    std::scoped_lock lock(m->mutex);
    return m->doc(User, file).data;
}

void PrefStore::setUserDoc(std::string_view file, json data)
{
    {
        std::scoped_lock lock(m->mutex);
        Doc& d = m->doc(User, file);
        if (d.data == data)
            return;
        d.data = data.is_object() ? std::move(data) : json::object();
        m->memo.clear();
        d.dirty = true;
        d.dirtyAt = Clock::now();
    }
    m->cv.notify_all();
    m->notify(std::string(file));
}

void PrefStore::flush()
{
    std::unique_lock lock(m->mutex);
    if (!m->io.joinable())
    {
        m->writeDirty(lock, true);
        return;
    }
    m->flushRequested = true;
    m->cv.notify_all();
    m->flushed.wait(lock, [this] { return !m->flushRequested; });
}

int PrefStore::onChanged(ChangeFn fn)
{
    std::scoped_lock lock(m->mutex);
    m->listeners.emplace(m->nextListener, std::move(fn));
    return m->nextListener++;
}

void PrefStore::removeOnChanged(int id)
{
    std::scoped_lock lock(m->mutex);
    m->listeners.erase(id);
}

fs::path PrefStore::userDir() const
{
    return m->layers[User].dir;
}
} // namespace lila
