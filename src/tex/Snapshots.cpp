#include "tex/Snapshots.h"

#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "tex/TextureSet.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>

namespace lila::tex::snapshots
{
namespace
{
struct Store
{
    std::mutex mutex;
    /** Pages by content hash; several pages can share a hash, told apart by their texels. */
    std::unordered_multimap<uint64_t, std::weak_ptr<const Page>> pages;
    std::map<std::string, std::vector<std::shared_ptr<const Snapshot>>> bySet;
    uint64_t nextId = 1;
};

Store& store()
{
    static Store s;
    return s;
}

uint64_t hashTexels(const std::vector<float>& v)
{
    uint64_t h = 1469598103934665603ull;
    const auto* b = reinterpret_cast<const uint8_t*>(v.data());
    for (size_t i = 0, n = v.size() * sizeof(float); i < n; ++i)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

/** The stored page with these texels, made when there is none. Under the store's mutex. */
std::shared_ptr<const Page> intern(Store& st, std::vector<float>&& rgba, size_t& added)
{
    const uint64_t h = hashTexels(rgba);
    auto [lo, hi] = st.pages.equal_range(h);
    for (auto it = lo; it != hi; ++it)
        if (std::shared_ptr<const Page> p = it->second.lock(); p && std::memcmp(p->rgba.data(), rgba.data(), rgba.size() * sizeof(float)) == 0)
            return p;
    auto page = std::make_shared<Page>();
    page->rgba = std::move(rgba);
    page->hash = h;
    st.pages.emplace(h, page);
    ++added;
    return page;
}

void sweep(Store& st)
{
    for (auto it = st.pages.begin(); it != st.pages.end();)
        it = it->second.expired() ? st.pages.erase(it) : std::next(it);
}

json describe(const Snapshot& s)
{
    json ch = json::array();
    for (const ChannelShot& c : s.channels)
        ch.push_back({{"channel", c.channel}, {"udim", c.udim}, {"width", c.width}, {"height", c.height}});
    return {{"set", s.set}, {"name", s.name}, {"channels", ch}, {"newPages", s.newPages}};
}
} // namespace

std::shared_ptr<const Snapshot> take(scene::Node& set, std::string name, int resolution, const std::string& backend)
{
    const SetSettings settings = readSettings(set);
    const Graph graph = readGraph(set);
    const std::vector<OutputRequest> requests = channelRequests(graph, settings);
    if (requests.empty())
        throw std::invalid_argument("the set has no channels to snapshot");
    auto snap = std::make_shared<Snapshot>();
    snap->set = set.name();
    for (const Tile& t : settings.tiles)
    {
        auto [tw, th] = settings.tileSize(t);
        int w = tw, h = th;
        if (resolution > 0)
        {
            const double scale = double(resolution) / double(std::max(tw, th));
            w = std::max(1, int(std::lround(tw * scale)));
            h = std::max(1, int(std::lround(th * scale)));
        }
        std::map<std::string, image::Image> images = evaluateOutputs(set, requests, t.udim, w, h, backend);
        Store& st = store();
        std::scoped_lock lock(st.mutex);
        for (auto& [channel, img] : images)
        {
            ChannelShot shot;
            shot.channel = channel;
            shot.udim = t.udim;
            shot.width = img.width;
            shot.height = img.height;
            const int cols = (img.width + kPage - 1) / kPage, rows = (img.height + kPage - 1) / kPage;
            for (int py = 0; py < rows; ++py)
                for (int px = 0; px < cols; ++px)
                {
                    std::vector<float> texels(size_t(kPage) * kPage * 4, 0.0f);
                    for (int y = 0; y < kPage && py * kPage + y < img.height; ++y)
                    {
                        const int x0 = px * kPage, n = std::min(kPage, img.width - x0);
                        std::memcpy(&texels[size_t(y) * kPage * 4], &img.rgba[(size_t(py * kPage + y) * img.width + x0) * 4], size_t(n) * 4 * sizeof(float));
                    }
                    shot.pages.push_back(intern(st, std::move(texels), snap->newPages));
                }
            snap->channels.push_back(std::move(shot));
        }
    }
    Store& st = store();
    std::scoped_lock lock(st.mutex);
    std::vector<std::shared_ptr<const Snapshot>>& shots = st.bySet[snap->set];
    if (name.empty())
        for (size_t n = shots.size() + 1;; ++n)
            if (std::ranges::none_of(shots, [&](const auto& s) { return s->name == "Snapshot " + std::to_string(n); }))
            {
                name = "Snapshot " + std::to_string(n);
                break;
            }
    snap->name = name;
    snap->id = st.nextId++;
    std::erase_if(shots, [&](const auto& s) { return s->name == snap->name; });
    shots.push_back(snap);
    sweep(st);
    return snap;
}

std::shared_ptr<const Snapshot> find(std::string_view set, std::string_view name)
{
    Store& st = store();
    std::scoped_lock lock(st.mutex);
    auto it = st.bySet.find(std::string(set));
    if (it == st.bySet.end())
        return nullptr;
    for (const auto& s : it->second)
        if (s->name == name)
            return s;
    return nullptr;
}

std::vector<std::shared_ptr<const Snapshot>> list(std::string_view set)
{
    Store& st = store();
    std::scoped_lock lock(st.mutex);
    auto it = st.bySet.find(std::string(set));
    return it == st.bySet.end() ? std::vector<std::shared_ptr<const Snapshot>>() : it->second;
}

bool remove(std::string_view set, std::string_view name)
{
    Store& st = store();
    std::scoped_lock lock(st.mutex);
    auto it = st.bySet.find(std::string(set));
    if (it == st.bySet.end())
        return false;
    const size_t n = std::erase_if(it->second, [&](const auto& s) { return s->name == name; });
    sweep(st);
    return n > 0;
}

void clear()
{
    Store& st = store();
    std::scoped_lock lock(st.mutex);
    st.bySet.clear();
    sweep(st);
}

image::Image image(const Snapshot& snap, std::string_view channel, int udim)
{
    image::Image out;
    for (const ChannelShot& c : snap.channels)
    {
        if (c.channel != channel || c.udim != udim)
            continue;
        out.width = c.width;
        out.height = c.height;
        out.rgba.assign(size_t(c.width) * c.height * 4, 0.0f);
        const int cols = (c.width + kPage - 1) / kPage;
        for (size_t i = 0; i < c.pages.size(); ++i)
        {
            const int px = int(i) % cols, py = int(i) / cols;
            for (int y = 0; y < kPage && py * kPage + y < c.height; ++y)
            {
                const int x0 = px * kPage, n = std::min(kPage, c.width - x0);
                std::memcpy(&out.rgba[(size_t(py * kPage + y) * c.width + x0) * 4], &c.pages[i]->rgba[size_t(y) * kPage * 4], size_t(n) * 4 * sizeof(float));
            }
        }
        break;
    }
    return out;
}

size_t storedPages()
{
    Store& st = store();
    std::scoped_lock lock(st.mutex);
    sweep(st);
    return st.pages.size();
}

size_t storedBytes() { return storedPages() * size_t(kPage) * kPage * 4 * sizeof(float); }

void registerCommands()
{
    using T = ArgType;
    auto setArg = [](const Args& a, const char* cmd) -> scene::Node& {
        if (a.positional.empty() || !a.positional[0].is_string())
            throw CommandError(std::string(cmd) + ": name a texture set");
        scene::Node* set = findSet(scene::Graph::current(), a.positional[0].get<std::string>());
        if (!set)
            throw CommandError(std::string(cmd) + ": no texture set " + a.positional[0].get<std::string>());
        return *set;
    };
    ops::registerCommand({
        .name = "texSnapshot",
        .doc = "Keep a texture set's evaluated channels as they are now, to compare later work against (texCompare). Unchanged regions "
               "share their pixels with earlier snapshots. Snapshots last the session. Returns {set, name, channels, newPages}.",
        .positionalDoc = "texture set",
        .flags = {{"name", "n", T::String, nullptr, "Its name; Snapshot N when left out. An existing snapshot of that name is replaced."},
                  presetFlag({"resolution", "r", T::Int, 1024, "Largest side of each tile's images; 0 keeps the tiles' own sizes.", {}, {}, 0.0, 16384.0},
                             kTextureSizePresets),
                  {"backend", "", T::String, nullptr, "gpu or cpu; the GPU when left out and the renderer can run.", {"gpu", "cpu"}}},
        .undoable = false,
        .run = [setArg](const Args& a) -> json {
            scene::Node& set = setArg(a, "texSnapshot");
            try
            {
                return describe(*take(set, a.get<std::string>("name"), a.get<int>("resolution", 1024), a.get<std::string>("backend")));
            }
            catch (const std::invalid_argument& e)
            {
                throw CommandError(std::string("texSnapshot: ") + e.what());
            }
        },
        .examples = {"texSnapshot(\"RockTex\", name=\"before rust\")"},
    });
    ops::registerCommand({
        .name = "texSnapshots",
        .doc = "A texture set's snapshots, oldest first, and the memory every snapshot holds. Returns {snapshots: [{set, name, channels, newPages}], pages, bytes}.",
        .positionalDoc = "texture set",
        .undoable = false,
        .run = [setArg](const Args& a) -> json {
            scene::Node& set = setArg(a, "texSnapshots");
            json out = json::array();
            for (const auto& s : list(set.name()))
                out.push_back(describe(*s));
            return {{"snapshots", out}, {"pages", storedPages()}, {"bytes", storedBytes()}};
        },
    });
    ops::registerCommand({
        .name = "texDeleteSnapshot",
        .doc = "Drop a texture set's snapshot and the pixels only it held. Returns whether there was one.",
        .positionalDoc = "texture set, name",
        .undoable = false,
        .run = [setArg](const Args& a) -> json {
            scene::Node& set = setArg(a, "texDeleteSnapshot");
            if (a.positional.size() < 2 || !a.positional[1].is_string())
                throw CommandError("texDeleteSnapshot: name the snapshot");
            return remove(set.name(), a.positional[1].get<std::string>());
        },
    });
}
} // namespace lila::tex::snapshots
