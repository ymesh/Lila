#include "render/MeshMaps.h"

#include "bake/AutoDistance.h"
#include "bake/Backend.h"
#include "bake/BakeCache.h"
#include "bake/BakeInputs.h"
#include "bake/BakeNodes.h"
#include "bake/CageEstimate.h"
#include "bake/Maps.h"
#include "bake/TexelRaster.h"
#include "core/Events.h"
#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "ops/Command.h"
#include "render/Baker.h"
#include "render/LiveBake.h"
#include "render/TexEval.h"
#include "render/Renderer.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "tex/Generators.h"

#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>

namespace lila::render
{
namespace
{
using Clock = std::chrono::steady_clock;
using scene::NodeId;

/** Texel tile edge: the unit an edit dirties and an incremental rebake re-traces. */
constexpr int kTile = 32;

int pref(const char* key, int fallback) { return PrefStore::get().value<int>("bake", key, fallback); }

/** Maps whose data is one channel (gray), stored R16F. */
bool singleChannel(std::string_view id)
{
    static const std::set<std::string, std::less<>> k = {"height",    "ao",        "curvature", "normalToCurvature", "convexity", "cavity",
                                                         "edgeWear",  "thickness", "alpha",     "roughness",         "metallic",  "materialOpacity"};
    return k.contains(id);
}

TextureFormat formatFor(const bake::MapDef& def)
{
    if (!def.linear || def.resolve == bake::Resolve::Exact)
        return TextureFormat::RGBA8;
    return singleChannel(def.id) ? TextureFormat::R16F : TextureFormat::RGBA16F;
}

/** Maps whose finalize normalises by a range measured over the whole texture, so any edit may change every texel. */
bool globalFinalize(const bake::MapSettings& s)
{
    const std::string& id = s.map;
    const auto zero = [&](const char* key) { return !(s.options.is_object() && s.options.value(key, 0.0) > 0.0); };
    if (id == "height")
        return zero("range");
    if (id == "thickness")
        return zero("distance");
    if (id == "curvature" || id == "normalToCurvature" || id == "convexity" || id == "cavity" || id == "edgeWear")
        return zero("range");
    return false;
}

bool sameBakeMaterial(const bake::MaterialInput& a, const bake::MaterialInput& b)
{
    return std::tie(a.found, a.baseColor, a.metallic, a.roughness, a.emissive, a.emissiveIntensity, a.opacity, a.blendMode, a.normalOpenGL,
                    a.packedLayout, a.baseColorMap, a.metallicMap, a.roughnessMap, a.normalMap, a.ormMap, a.emissiveMap, a.occlusionMap) ==
           std::tie(b.found, b.baseColor, b.metallic, b.roughness, b.emissive, b.emissiveIntensity, b.opacity, b.blendMode, b.normalOpenGL,
                    b.packedLayout, b.baseColorMap, b.metallicMap, b.roughnessMap, b.normalMap, b.ormMap, b.emissiveMap, b.occlusionMap);
}

bool sameMaterials(const bake::BakeInputs& a, const bake::BakeInputs& b)
{
    if (a.materials.size() != b.materials.size())
        return false;
    for (auto i = a.materials.begin(), j = b.materials.begin(); i != a.materials.end(); ++i, ++j)
        if (i->first != j->first || !sameBakeMaterial(i->second, j->second))
            return false;
    return true;
}

/** Same triangle count and the same per-corner arrays present, so lows compare triangle by triangle. */
bool sameShape(const bake::LowInput& a, const bake::LowInput& b)
{
    return a.soup.triangleCount() == b.soup.triangleCount() && a.soup.positions.size() == b.soup.positions.size() &&
           a.soup.normals.size() == b.soup.normals.size() && a.soup.uvs.size() == b.soup.uvs.size() && a.soup.colors.size() == b.soup.colors.size() &&
           a.soup.materialNames == b.soup.materialNames && a.tangents.size() == b.tangents.size() && a.cageNormals.size() == b.cageNormals.size() &&
           a.cageScale.size() == b.cageScale.size() && a.cageSkew.size() == b.cageSkew.size() && a.cagePositions.size() == b.cagePositions.size() &&
           a.islands.size() == b.islands.size() && a.faces.size() == b.faces.size();
}

template <class T> bool cornersDiffer(const std::vector<T>& a, const std::vector<T>& b, size_t t)
{
    return a.size() >= t * 3 + 3 && std::memcmp(&a[t * 3], &b[t * 3], 3 * sizeof(T)) != 0;
}

template <class T> bool valueDiffers(const std::vector<T>& a, const std::vector<T>& b, size_t t) { return t < a.size() && a[t] != b[t]; }

bool triangleChanged(const bake::LowInput& a, const bake::LowInput& b, size_t t)
{
    return cornersDiffer(a.soup.positions, b.soup.positions, t) || cornersDiffer(a.soup.normals, b.soup.normals, t) ||
           cornersDiffer(a.soup.uvs, b.soup.uvs, t) || cornersDiffer(a.soup.colors, b.soup.colors, t) || cornersDiffer(a.tangents, b.tangents, t) ||
           cornersDiffer(a.cageNormals, b.cageNormals, t) || cornersDiffer(a.cageScale, b.cageScale, t) || cornersDiffer(a.cageSkew, b.cageSkew, t) ||
           cornersDiffer(a.cagePositions, b.cagePositions, t) || valueDiffers(a.soup.materials, b.soup.materials, t) ||
           valueDiffers(a.islands, b.islands, t) || valueDiffers(a.faces, b.faces, t);
}

/** Flags the texel tiles under triangle t's uvs (one texel of margin for jittered samples) in its tile's mask. */
void markTriangle(const bake::LowInput& low, const bake::TexelRaster& raster, uint32_t t, int W, int H, std::map<int, std::vector<uint8_t>>& masks)
{
    const int udim = raster.triangleUdims()[t];
    if (udim == 0 || low.soup.uvs.size() < size_t(t) * 3 + 3)
        return;
    const int tileU = (udim - 1001) % 10, tileV = (udim - 1001) / 10;
    const int cols = (W + kTile - 1) / kTile, rows = (H + kTile - 1) / kTile;
    double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30;
    for (int k = 0; k < 3; ++k)
    {
        const glm::vec2 uv = low.soup.uvs[size_t(t) * 3 + size_t(k)];
        const double x = (double(uv.x) - tileU) * W, y = (1.0 - (double(uv.y) - tileV)) * H;
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
    }
    const int tx0 = std::clamp(int(std::floor(x0 - 1.0)) / kTile, 0, cols - 1), tx1 = std::clamp(int(std::floor(x1 + 1.0)) / kTile, 0, cols - 1);
    const int ty0 = std::clamp(int(std::floor(y0 - 1.0)) / kTile, 0, rows - 1), ty1 = std::clamp(int(std::floor(y1 + 1.0)) / kTile, 0, rows - 1);
    std::vector<uint8_t>& m = masks[udim];
    m.resize(size_t(cols) * size_t(rows), 0);
    for (int ty = ty0; ty <= ty1; ++ty)
        for (int tx = tx0; tx <= tx1; ++tx)
            m[size_t(ty) * size_t(cols) + size_t(tx)] = 1;
}

struct LiveTex
{
    bake::TextureBuffer buf;
    GpuImageRef image;
    uint64_t version = 0;
    std::string group;
    std::string low;
    bool planned = false;
};

struct LowJob
{
    size_t group = 0;
    size_t low = 0;
    /** Group name and the low's soup name: what matches a low across snapshots. */
    std::string key;
    std::unique_ptr<bake::TexelRaster> raster;
};

using TexKey = std::tuple<std::string, std::string, int>;

struct Session
{
    NodeId project = scene::kNoNode;
    std::string projectName;
    std::string set;
    LiveMapsOptions options;
    std::vector<bake::MapSettings> requested;
    bake::OutputSettings output;
    int debounceMs = 150;
    double sliceMs = 8.0;

    // Engine mutex.
    bool dirty = true;
    Clock::time_point dirtySince{};
    bool snapshotRequested = false;
    std::unique_ptr<bake::BakeInputs> pending;
    bool busy = false;
    std::map<std::string, std::vector<float>> offsets;
    /** Nodes a value change on dirties the set: the project, groups, meshes and everything upstream of them. */
    std::set<NodeId> watched;
    json last;
    std::string error;
    uint64_t updates = 0;
    uint64_t fullBakes = 0;
    std::vector<std::string> mapIds;
    std::vector<std::string> skipped;

    // Whoever holds runMutex. Declared so the device bake goes before its backend, and the inputs after both.
    std::mutex runMutex;
    std::unique_ptr<bake::BakeInputs> inputs;
    std::vector<std::vector<uint64_t>> highHashes;
    std::unique_ptr<bake::Backend> backend;
    std::vector<bake::ResidentMap> maps;
    std::unique_ptr<DeviceBake> dev;
    std::vector<LowJob> jobs;
    std::map<TexKey, std::unique_ptr<LiveTex>> textures;
};

using SessionKey = std::pair<NodeId, std::string>;

struct Engine
{
    std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    bool async = false;
    bool stop = false;
    std::map<SessionKey, std::shared_ptr<Session>> sessions;
    int graphListener = 0;
    bool hooked = false;

    std::mutex pubMutex;
    std::map<SessionKey, std::vector<MeshMapInfo>> published;

    std::mutex listenerMutex;
    std::map<int, std::function<void(const MeshMapChange&)>> listeners;
    int nextListener = 1;
};

/** Never destroyed: a set's device memory must not outlive the renderer at exit, and shutdownLiveMaps frees it in order. */
Engine& engine()
{
    static Engine* e = new Engine;
    return *e;
}

std::shared_ptr<Session> findSession(NodeId project, std::string_view set)
{
    Engine& e = engine();
    std::scoped_lock lock(e.mutex);
    auto it = e.sessions.find({project, std::string(set)});
    return it == e.sessions.end() ? nullptr : it->second;
}

/** The maps a set bakes: options.mapSettings, else options.maps from the project's list, else its enabled maps. */
std::vector<bake::MapSettings> requestedMaps(const bake::ProjectSettings& ps, const LiveMapsOptions& o)
{
    if (!o.mapSettings.empty())
        return o.mapSettings;
    std::vector<bake::MapSettings> out;
    if (!o.maps.empty())
    {
        for (const std::string& id : o.maps)
        {
            auto it = std::ranges::find_if(ps.maps, [&](const bake::MapSettings& m) { return m.map == id; });
            bake::MapSettings m;
            if (it != ps.maps.end())
                m = *it;
            else
                m.map = id;
            out.push_back(m);
        }
        return out;
    }
    for (const bake::MapSettings& m : ps.maps)
        if (m.enabled)
            out.push_back(m);
    return out;
}

/** The scene as the set bakes it, with a stroke's working cage offsets. Main thread. */
std::unique_ptr<bake::BakeInputs> snapshot(scene::Graph& g, NodeId project, const std::vector<NodeId>& groups,
                                           const std::map<std::string, std::vector<float>>& offsets)
{
    LILA_ZONE();

    scene::Node* p = g.node(project);
    if (!p || !bake::isProject(*p))
        throw std::runtime_error("the bake project is gone");
    g.evaluate();
    auto in = std::make_unique<bake::BakeInputs>(bake::buildInputs(g, project, groups, true));
    for (const auto& [path, values] : offsets)
    {
        scene::Node* n = nullptr;
        try
        {
            n = g.find(path);
        }
        catch (const std::exception&)
        {
        }
        if (!n || !n->type().isA("mesh") || values.empty())
            continue;
        for (bake::GroupInputs& gi : in->groups)
        {
            const auto it = std::ranges::find(gi.settings.lows, n->path());
            if (it == gi.settings.lows.end())
                continue;
            const size_t li = size_t(it - gi.settings.lows.begin());
            scene::MeshPtr m = scene::modifiers::exportMesh(g, n->id());
            if (li >= gi.lows.size() || !m || m->pointCapacity() != values.size())
                continue;
            const glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            const std::vector<geo::Index> cp = bake::soupCornerPoints(*m, world);
            if (cp.size() == gi.lows[li].cageScale.size())
                for (size_t c = 0; c < cp.size(); ++c)
                    gi.lows[li].cageScale[c] = values[cp[c]];
        }
    }
    return in;
}

/** The project, its groups (or those given), their lows, highs and cages, and every node upstream of them. Main thread. */
std::set<NodeId> watchedNodes(scene::Graph& g, NodeId project, const std::vector<NodeId>& only)
{
    std::vector<NodeId> stack{project};
    for (NodeId gid : only.empty() ? bake::groupsOf(g, project) : only)
    {
        scene::Node* grp = g.node(gid);
        if (!grp)
            continue;
        stack.push_back(gid);
        const bake::GroupSettings gs = bake::groupSettings(*grp);
        std::vector<std::string> paths = gs.lows;
        paths.push_back(gs.cage);
        for (const bake::HighEntry& h : gs.highs)
            paths.push_back(h.node);
        for (const std::string& path : paths)
        {
            if (path.empty())
                continue;
            try
            {
                stack.push_back(g.find(path)->id());
            }
            catch (const std::exception&)
            {
            }
        }
    }
    std::map<NodeId, std::vector<NodeId>> sources;
    for (const auto& [dst, src] : g.connections())
        sources[dst.node].push_back(src.node);
    std::set<NodeId> seen;
    while (!stack.empty())
    {
        const NodeId id = stack.back();
        stack.pop_back();
        if (id == scene::kNoNode || !seen.insert(id).second)
            continue;
        if (auto it = sources.find(id); it != sources.end())
            stack.insert(stack.end(), it->second.begin(), it->second.end());
        stack.push_back(g.parent(id));
    }
    return seen;
}

/** Plans the maps on the first update, when the backend can say which it keeps on the device. */
void planMaps(Session& s)
{
    bake::registerBuiltinMaps();
    std::vector<std::string> ids, skipped;
    for (const bake::MapSettings& m : s.requested)
    {
        const bake::MapDef* def = bake::findMap(m.map);
        if (!def)
            skipped.push_back(m.map + ": unknown map");
        else if (!def->pack.is_null())
            skipped.push_back(m.map + ": a channel pack (enable its channels instead)");
        else if (m.map == "normalToHeight")
            skipped.push_back(m.map + ": finalizes on the CPU");
        else if (std::ranges::find(ids, m.map) != ids.end())
            skipped.push_back(m.map + ": listed twice");
        else if (!s.backend->residentSupports(*def, m))
            skipped.push_back(m.map + ": not baked on the GPU");
        else
        {
            s.maps.push_back({def, m});
            ids.push_back(m.map);
        }
    }
    std::scoped_lock lock(engine().mutex);
    s.mapIds = ids;
    s.skipped = skipped;
}

/** The texel tiles of a mask as rectangles, merged along rows and grown by padding. */
std::vector<glm::ivec4> regionsOf(const std::vector<uint8_t>& mask, int W, int H, int padding)
{
    std::vector<glm::ivec4> out;
    const int cols = (W + kTile - 1) / kTile, rows = (H + kTile - 1) / kTile;
    for (int ty = 0; ty < rows; ++ty)
        for (int tx = 0; tx < cols;)
        {
            if (!mask[size_t(ty) * size_t(cols) + size_t(tx)])
            {
                ++tx;
                continue;
            }
            int end = tx;
            while (end < cols && mask[size_t(ty) * size_t(cols) + size_t(end)])
                ++end;
            const int pad = std::max(0, padding);
            const int x0 = std::max(0, tx * kTile - pad), y0 = std::max(0, ty * kTile - pad);
            const int x1 = std::min(W, end * kTile + pad), y1 = std::min(H, (ty + 1) * kTile + pad);
            out.push_back({x0, y0, x1 - x0, y1 - y0});
            tx = end;
        }
    return out;
}

/**
 * Rebakes a set against a new snapshot: what changed per low triangle dirties texel tiles, which are cleared and
 * re-traced, then every texture with a dirty tile finishes into its image. Throws on GPU failure.
 */
json runUpdate(Session& s, std::unique_ptr<bake::BakeInputs> next, std::vector<MeshMapChange>& changes)
{
    LILA_ZONE();

    const auto started = Clock::now();
    bake::BakeInputs& in = *next;
    const bake::BakeInputs* old = s.inputs.get();

    std::vector<std::vector<uint64_t>> hashes(in.groups.size());
    for (size_t g = 0; g < in.groups.size(); ++g)
    {
        hashes[g].resize(in.groups[g].highs.size());
        tbb::parallel_for(size_t(0), hashes[g].size(), [&](size_t h) { hashes[g][h] = bake::soupHash(in.groups[g].highs[h].soup); });
    }
    bool full = !s.dev || !old || old->groups.size() != in.groups.size() || !sameMaterials(*old, in) || old->highMin != in.highMin ||
                old->highMax != in.highMax;
    std::vector<uint8_t> groupFull(in.groups.size(), full ? 1 : 0);
    for (size_t g = 0; g < in.groups.size() && !full; ++g)
    {
        const bake::GroupInputs& a = old->groups[g];
        const bake::GroupInputs& b = in.groups[g];
        if (!(a.name == b.name && a.settings.toJson() == b.settings.toJson() && s.highHashes[g] == hashes[g] && a.lows.size() == b.lows.size()))
            groupFull[g] = 1;
    }

    if (!s.backend)
        s.backend = createGpuBakeBackend(GpuTrace::Auto);
    s.backend->prepare(in);
    // Distances measured once stay while the group and its highs do, so a low edit never moves every ray.
    if (std::ranges::find(groupFull, uint8_t(1)) != groupFull.end())
        bake::resolveAutoDistances(in, *s.backend);
    for (size_t g = 0; g < in.groups.size(); ++g)
        if (!groupFull[g])
        {
            in.groups[g].frontal = old->groups[g].frontal;
            in.groups[g].rear = old->groups[g].rear;
            in.groups[g].measured = old->groups[g].measured;
        }

    if (!s.dev)
    {
        planMaps(s);
        if (s.maps.empty())
            throw std::runtime_error("none of the maps bakes on the GPU");
        s.dev = beginDeviceBake(*s.backend, in, s.maps, s.output, s.sliceMs);
        if (!s.dev)
            throw std::runtime_error("live mesh maps need a GPU (the GPU bake backend failed)");
    }
    else
        s.dev->rebind(in);

    const int W = s.output.width, H = s.output.height;
    const int cols = (W + kTile - 1) / kTile, rows = (H + kTile - 1) / kTile;
    const size_t tileCount = size_t(cols) * size_t(rows);
    bake::RasterSettings rs;
    rs.width = W;
    rs.height = H;
    rs.supersampling = s.output.supersampling;
    rs.bandRows = std::max(1, s.dev->bandRows(W, s.output.supersampling));

    std::map<std::string, const LowJob*> oldJobs;
    for (const LowJob& j : s.jobs)
        oldJobs[j.key] = &j;
    std::vector<LowJob> jobs;
    for (size_t g = 0; g < in.groups.size(); ++g)
        for (size_t l = 0; l < in.groups[g].lows.size(); ++l)
            if (!in.groups[g].lows[l].soup.uvs.empty())
                jobs.push_back({g, l, in.groups[g].name + "\n" + in.groups[g].lows[l].soup.name,
                                std::make_unique<bake::TexelRaster>(in.groups[g].lows[l], rs)});

    for (auto& [k, t] : s.textures)
        t->planned = false;
    json stats = {{"full", full}, {"tilesDirty", 0}, {"tilesTraced", 0}, {"images", 0}, {"samples", 0}, {"hits", 0}};
    uint64_t tilesDirty = 0, tilesTraced = 0, samples = 0, hits = 0;
    std::vector<LiveTex*> finished;
    std::map<LiveTex*, std::vector<uint8_t>> partial;
    for (const LowJob& j : jobs)
    {
        const bake::GroupInputs& gi = in.groups[j.group];
        const bake::LowInput& low = gi.lows[j.low];
        auto oj = oldJobs.find(j.key);
        const bake::LowInput* before = oj != oldJobs.end() ? &old->groups[oj->second->group].lows[oj->second->low] : nullptr;
        const bool lowFull = groupFull[j.group] || !before || !sameShape(*before, low);
        std::map<int, std::vector<uint8_t>> masks;
        if (!lowFull)
            for (uint32_t t = 0; t < low.soup.triangleCount(); ++t)
                if (triangleChanged(*before, low, t))
                {
                    markTriangle(*before, *oj->second->raster, t, W, H, masks);
                    markTriangle(low, *j.raster, t, W, H, masks);
                }

        uint32_t slots = uint32_t(std::max<size_t>(1, low.soup.materialNames.size()));
        for (uint32_t m : low.soup.materials)
            slots = std::max(slots, m + 1);
        for (int udim : j.raster->udims())
        {
            std::vector<LiveTex*> texs;
            bool fresh = false;
            for (const bake::ResidentMap& rm : s.maps)
            {
                std::unique_ptr<LiveTex>& t = s.textures[{j.key, rm.def->id, udim}];
                if (!t)
                {
                    t = std::make_unique<LiveTex>();
                    t->buf.def = rm.def;
                    t->buf.settings = rm.settings;
                    t->buf.width = W;
                    t->buf.height = H;
                    t->buf.udim = udim;
                    t->buf.samplesPerTexel = j.raster->samplesPerTexel();
                    t->buf.groups.insert(gi.name);
                    t->buf.meshes.insert(low.soup.name);
                    t->group = gi.name;
                    t->low = low.soup.name;
                    const TextureFormat f = formatFor(*rm.def);
                    t->image = GpuImage::create({W, H, f, f == TextureFormat::RGBA8 && !rm.def->linear, true,
                                                 s.projectName + " " + low.soup.name + " " + rm.def->id + " " + std::to_string(udim)});
                    fresh = true;
                }
                t->planned = true;
                texs.push_back(t.get());
            }
            const bool whole = lowFull || fresh;
            const std::vector<uint8_t>* mask = nullptr;
            std::vector<uint32_t> list;
            if (!whole)
            {
                auto it = masks.find(udim);
                if (it == masks.end())
                    continue;
                for (size_t i = 0; i < it->second.size(); ++i)
                    if (it->second[i])
                        list.push_back(uint32_t(i));
                if (list.empty())
                    continue;
                mask = &it->second;
            }
            tilesDirty += whole ? tileCount : list.size();
            // A texture belongs to one low and tile, so it is cleared and re-traced once per update.
            for (LiveTex* t : texs)
            {
                s.dev->clearTiles(t->buf, list, kTile);
                finished.push_back(t);
                if (!whole)
                    partial[t] = *mask;
            }
            bake::ResidentTile tile;
            tile.group = j.group;
            tile.low = j.low;
            tile.udim = udim;
            tile.raster = j.raster.get();
            tile.cage = {gi.frontal, gi.rear, float(gi.settings.cageOffset), float(gi.settings.skew)};
            tile.targets.resize(s.maps.size());
            for (size_t m = 0; m < s.maps.size(); ++m)
                tile.targets[m].assign(slots, &texs[m]->buf);
            tile.mask = mask;
            tile.maskTile = kTile;
            const bake::ResidentStats r = s.dev->bakeTile(tile);
            tilesTraced += r.tiles;
            samples += r.samples;
            hits += r.hits;
        }
    }

    // Tiles a low no longer covers (or a low that went away): their images go too.
    for (auto it = s.textures.begin(); it != s.textures.end();)
    {
        if (it->second->planned)
        {
            ++it;
            continue;
        }
        s.dev->drop(it->second->buf);
        it = s.textures.erase(it);
    }

    const int padding = s.output.padding;
    for (LiveTex* t : finished)
    {
        s.dev->finishInto(t->buf, padding, t->image);
        ++t->version;
        MeshMapChange c;
        c.project = s.project;
        c.projectName = s.projectName;
        c.set = s.set;
        c.low = t->low;
        c.map = t->buf.def->id;
        c.udim = t->buf.udim;
        c.version = t->version;
        if (auto p = partial.find(t); p != partial.end() && !globalFinalize(t->buf.settings))
            c.regions = regionsOf(p->second, W, H, padding < 0 ? std::max(W, H) : padding);
        changes.push_back(std::move(c));
    }

    s.jobs = std::move(jobs);
    s.inputs = std::move(next);
    s.highHashes = std::move(hashes);
    stats["tilesDirty"] = tilesDirty;
    stats["tilesTraced"] = tilesTraced;
    stats["tileSize"] = kTile;
    stats["images"] = finished.size();
    stats["samples"] = samples;
    stats["hits"] = hits;
    json groups = json::array();
    for (const bake::GroupInputs& gi : s.inputs->groups)
        groups.push_back({{"group", gi.name}, {"frontal", gi.frontal}, {"rear", gi.rear}, {"measured", gi.measured}});
    stats["groups"] = groups;
    stats["seconds"] = std::chrono::duration<double>(Clock::now() - started).count();
    return stats;
}

/** What lookups see of a set: its images in job, map and tile order. */
std::vector<MeshMapInfo> publishedOf(const Session& s)
{
    std::vector<MeshMapInfo> out;
    for (const LowJob& j : s.jobs)
        for (int udim : j.raster->udims())
            for (const bake::ResidentMap& rm : s.maps)
            {
                auto it = s.textures.find({j.key, rm.def->id, udim});
                if (it == s.textures.end() || it->second->version == 0)
                    continue;
                const LiveTex& t = *it->second;
                MeshMapInfo info;
                info.image = t.image;
                info.version = t.version;
                info.project = s.project;
                info.projectName = s.projectName;
                info.set = s.set;
                info.group = t.group;
                info.low = t.low;
                info.map = rm.def->id;
                info.udim = udim;
                info.width = t.buf.width;
                info.height = t.buf.height;
                info.colorSpace = rm.def->linear ? "linear" : "srgb";
                info.meta = t.buf.meta;
                out.push_back(std::move(info));
            }
    return out;
}

void notify(const std::vector<MeshMapChange>& changes)
{
    Engine& e = engine();
    std::vector<std::function<void(const MeshMapChange&)>> fns;
    {
        std::scoped_lock lock(e.listenerMutex);
        for (const auto& [id, fn] : e.listeners)
            fns.push_back(fn);
    }
    std::set<std::string> projects;
    for (const MeshMapChange& c : changes)
    {
        projects.insert(c.projectName);
        for (const auto& fn : fns)
            fn(c);
    }
    for (const std::string& p : projects)
        texgpu::meshMapsChanged(p);
}

/** A baked map file uploaded for texture graphs, kept while the file is unchanged. */
struct BakedFile
{
    std::filesystem::path file;
    std::filesystem::file_time_type time;
    GpuImageRef image;
};

struct BakedFiles
{
    std::mutex mutex;
    std::map<std::tuple<std::string, std::string, int>, BakedFile> files;
    std::set<std::tuple<std::string, std::string, int>> pending;
    /** Looked for and not found: asked again only after bakedMapsChanged. */
    std::set<std::tuple<std::string, std::string, int>> misses;
};

BakedFiles& bakedFiles()
{
    static BakedFiles b;
    return b;
}

/** The cached upload of a project's baked file, or null when there is none or the file changed since. */
GpuImageRef cachedBakedFile(const std::string& project, const std::string& map, int udim)
{
    BakedFiles& b = bakedFiles();
    std::scoped_lock lock(b.mutex);
    auto it = b.files.find({project, map, udim});
    if (it == b.files.end())
        return nullptr;
    std::error_code ec;
    if (std::filesystem::last_write_time(it->second.file, ec) != it->second.time || ec)
    {
        b.files.erase(it);
        return nullptr;
    }
    return it->second.image;
}

/** Reads a project's baked map file into an upload, off the main thread; null when it can't be read. */
GpuImageRef readBakedFile(const std::filesystem::path& file, const std::string& map)
{
    LILA_ZONE();
    image::Image img = image::read(file);
    if (img.width <= 0)
        return nullptr;
    // Float files hold data (trim indices, rectangles), the rest are 8 bit.
    const bool exact = file.extension() == ".exr";
    GpuImageRef gpu = GpuImage::create({img.width, img.height, exact ? TextureFormat::RGBA16F : TextureFormat::RGBA8, false, false, "bake." + map});
    gpu->updateRgba(0, 0, img.width, img.height, img.rgba);
    return gpu;
}

/** Main thread: whether a group-less project has a baked file for the map, which then loads in the background. */
bool loadBakedFile(scene::Graph& g, const std::string& project, const std::string& map, int udim)
{
    const std::filesystem::path file = tex::bakedMapFile(g, project, map, udim);
    std::error_code ec;
    const std::filesystem::file_time_type time = file.empty() ? std::filesystem::file_time_type() : std::filesystem::last_write_time(file, ec);
    if (file.empty() || ec)
        return false;
    static tbb::task_arena arena;
    arena.enqueue([file, time, project, map, udim] {
        GpuImageRef gpu = readBakedFile(file, map);
        mainq::post([file, time, project, map, udim, gpu = std::move(gpu)]() mutable {
            const bool loaded = gpu != nullptr;
            {
                BakedFiles& b = bakedFiles();
                std::scoped_lock lock(b.mutex);
                b.pending.erase({project, map, udim});
                if (loaded)
                    b.files[{project, map, udim}] = {file, time, std::move(gpu)};
                else
                    b.misses.insert({project, map, udim});
            }
            if (loaded)
                texgpu::meshMapsChanged(project);
        });
    });
    return true;
}

/**
 * Texture graphs asking for a project's maps: a project without a live set gets one at preview resolution (in the app),
 * and a project with no groups to bake live (a trim sheet's) serves its last bake's files.
 */
GpuImageRef meshMapSource(const std::string& projectName, const std::string& external, int udim)
{
    const std::string map = tex::bakeMapName(external);
    if (auto m = findMeshMap(projectName, map, udim))
        return m->image;
    if (GpuImageRef baked = cachedBakedFile(projectName, map, udim))
        return baked;
    Engine& e = engine();
    if (!e.async)
        return nullptr;
    {
        BakedFiles& b = bakedFiles();
        std::scoped_lock lock(b.mutex);
        if (!b.misses.contains({projectName, map, udim}) && b.pending.insert({projectName, map, udim}).second)
            mainq::post([projectName, map, udim] {
                scene::Graph& g = scene::Graph::current();
                bool loading = false;
                for (NodeId id : bake::projects(g))
                    if (!loading && g.node(id)->name() == projectName && bake::groupsOf(g, id).empty())
                        loading = loadBakedFile(g, projectName, map, udim);
                if (!loading)
                {
                    BakedFiles& files = bakedFiles();
                    std::scoped_lock done(files.mutex);
                    files.pending.erase({projectName, map, udim});
                    files.misses.insert({projectName, map, udim});
                }
            });
    }
    static std::mutex triedMutex;
    static std::set<std::string> tried;
    {
        std::scoped_lock lock(triedMutex);
        if (!tried.insert(projectName).second)
            return nullptr;
    }
    mainq::post([projectName] {
        scene::Graph& g = scene::Graph::current();
        for (NodeId id : bake::projects(g))
            if (g.node(id)->name() == projectName && !liveMapsEnabled(id) && !bake::groupsOf(g, id).empty())
            {
                try
                {
                    enableLiveMaps(g, id);
                }
                catch (const std::exception& ex)
                {
                    log::warn("Live mesh maps of {}: {}", projectName, ex.what());
                }
            }
    });
    return nullptr;
}

/** Runs an update and publishes it; listeners hear it on the main thread (now, when this is it). Never throws. */
json updateSession(const std::shared_ptr<Session>& s, std::unique_ptr<bake::BakeInputs> inputs)
{
    std::vector<MeshMapChange> changes;
    json stats;
    std::string error;
    {
        std::scoped_lock run(s->runMutex);
        try
        {
            stats = runUpdate(*s, std::move(inputs), changes);
        }
        catch (const std::exception& ex)
        {
            error = ex.what();
            // The device state may be half updated: the next update starts over.
            s->dev.reset();
            s->inputs.reset();
            s->jobs.clear();
            s->maps.clear();
        }
        std::vector<MeshMapInfo> pub = publishedOf(*s);
        Engine& e = engine();
        std::scoped_lock lock(e.mutex, e.pubMutex);
        // A set disabled or replaced while it baked publishes nothing.
        if (auto it = e.sessions.find({s->project, s->set}); it != e.sessions.end() && it->second == s)
            e.published[{s->project, s->set}] = std::move(pub);
    }
    {
        Engine& e = engine();
        std::scoped_lock lock(e.mutex);
        s->error = error;
        if (error.empty())
        {
            ++s->updates;
            if (stats.value("full", false))
                ++s->fullBakes;
            s->last = stats;
        }
    }
    if (!error.empty())
        log::warn("Live mesh maps of {}: {}", s->projectName, error);
    if (!changes.empty())
    {
        if (mainq::isMainThread())
            notify(changes);
        else
            mainq::post([changes] { notify(changes); });
    }
    return error.empty() ? stats : json{{"error", error}};
}

void requestSnapshot(SessionKey key);

void workerLoop()
{
    Engine& e = engine();
    std::unique_lock lock(e.mutex);
    while (!e.stop)
    {
        const auto now = Clock::now();
        std::shared_ptr<Session> run;
        auto wake = Clock::time_point::max();
        for (auto& [key, s] : e.sessions)
        {
            if (s->pending && !s->busy)
            {
                run = s;
                break;
            }
            if (s->dirty && !s->snapshotRequested && !s->busy)
            {
                const auto due = s->dirtySince + std::chrono::milliseconds(s->debounceMs);
                if (due <= now)
                {
                    s->snapshotRequested = true;
                    mainq::post([k = key] { requestSnapshot(k); });
                }
                else
                    wake = std::min(wake, due);
            }
        }
        if (run)
        {
            std::unique_ptr<bake::BakeInputs> inputs = std::move(run->pending);
            run->busy = true;
            lock.unlock();
            updateSession(run, std::move(inputs));
            lock.lock();
            run->busy = false;
            continue;
        }
        if (wake == Clock::time_point::max())
            e.cv.wait(lock);
        else
            e.cv.wait_until(lock, wake);
    }
}

/** Main thread: snapshots a dirty set for the worker. */
void requestSnapshot(SessionKey key)
{
    Engine& e = engine();
    std::shared_ptr<Session> s;
    std::vector<NodeId> groups;
    std::map<std::string, std::vector<float>> offsets;
    {
        std::scoped_lock lock(e.mutex);
        auto it = e.sessions.find(key);
        if (it == e.sessions.end())
            return;
        s = it->second;
        s->snapshotRequested = false;
        s->dirty = false;
        groups = s->options.groups;
        offsets = s->offsets;
    }
    std::unique_ptr<bake::BakeInputs> inputs;
    std::set<NodeId> watched;
    try
    {
        inputs = snapshot(scene::Graph::current(), s->project, groups, offsets);
        watched = watchedNodes(scene::Graph::current(), s->project, groups);
    }
    catch (const std::exception& ex)
    {
        std::scoped_lock lock(e.mutex);
        s->error = ex.what();
        return;
    }
    {
        std::scoped_lock lock(e.mutex);
        s->pending = std::move(inputs);
        s->watched = std::move(watched);
    }
    e.cv.notify_all();
}

void markDirty(Session& s)
{
    if (!s.dirty)
    {
        s.dirty = true;
        // Edits keep coming while a stroke drags: rebakes follow at most every debounce, not only once it stops.
        s.dirtySince = Clock::now();
    }
}

void startWorker()
{
    Engine& e = engine();
    if (!e.async || e.worker.joinable())
        return;
    e.stop = false;
    e.worker = std::thread(workerLoop);
}

/** Drops sets whose project is gone, unpublishing their images. Main thread. */
void dropMissing(scene::Graph& g)
{
    Engine& e = engine();
    std::vector<std::shared_ptr<Session>> dropped;
    {
        std::scoped_lock lock(e.mutex);
        for (auto it = e.sessions.begin(); it != e.sessions.end();)
        {
            scene::Node* n = g.node(it->first.first);
            if (n && bake::isProject(*n))
            {
                ++it;
                continue;
            }
            dropped.push_back(it->second);
            {
                std::scoped_lock pub(e.pubMutex);
                e.published.erase(it->first);
            }
            it = e.sessions.erase(it);
        }
    }
    // Freed here, or by the worker when it still bakes one.
    dropped.clear();
}

void listen(scene::Graph& g)
{
    Engine& e = engine();
    if (!e.hooked)
    {
        e.hooked = true;
        events::subscribe(events::Event::Shutdown, [](const events::EventData&) { shutdownLiveMaps(); });
    }
    if (e.graphListener)
        return;
    e.graphListener = g.addListener([](scene::Change c, NodeId id) {
        if (c == scene::Change::Selection)
            return;
        Engine& e = engine();
        if (c == scene::Change::Deleted || c == scene::Change::Cleared || c == scene::Change::Reset)
            dropMissing(scene::Graph::current());
        {
            std::scoped_lock lock(e.mutex);
            // Value edits elsewhere (cameras, unrelated meshes) leave sets alone; structure changes may rewire anything.
            for (auto& [key, s] : e.sessions)
                if (c != scene::Change::Value || s->watched.empty() || s->watched.contains(id))
                    markDirty(*s);
        }
        e.cv.notify_all();
    });
}

std::optional<MeshMapInfo> findIn(const std::vector<MeshMapInfo>& list, std::string_view map, int udim, std::string_view low)
{
    for (const MeshMapInfo& m : list)
    {
        if (m.map != map || m.udim != udim)
            continue;
        if (low.empty() || m.low == low || (m.low.size() > low.size() && m.low.ends_with(low) && m.low[m.low.size() - low.size() - 1] == '|'))
            return m;
    }
    return std::nullopt;
}
} // namespace

json MeshMapInfo::toJson() const
{
    return {{"project", projectName},
            {"set", set},
            {"group", group},
            {"low", low},
            {"map", map},
            {"udim", udim},
            {"width", width},
            {"height", height},
            {"format", image ? formatName(image->format()) : ""},
            {"colorSpace", colorSpace},
            {"version", version},
            {"meta", meta}};
}

std::optional<MeshMapInfo> findMeshMap(NodeId project, std::string_view map, int udim, std::string_view low, std::string_view set)
{
    Engine& e = engine();
    std::scoped_lock lock(e.pubMutex);
    auto it = e.published.find({project, std::string(set)});
    return it == e.published.end() ? std::nullopt : findIn(it->second, map, udim, low);
}

std::optional<MeshMapInfo> findMeshMap(std::string_view projectName, std::string_view map, int udim, std::string_view low, std::string_view set)
{
    Engine& e = engine();
    std::scoped_lock lock(e.pubMutex);
    for (const auto& [key, list] : e.published)
        if (key.second == set && !list.empty() && list.front().projectName == projectName)
            return findIn(list, map, udim, low);
    return std::nullopt;
}

uint64_t meshMapVersion(NodeId project, std::string_view map, int udim, std::string_view low, std::string_view set)
{
    const auto m = findMeshMap(project, map, udim, low, set);
    return m ? m->version : 0;
}

std::vector<MeshMapInfo> meshMapSet(NodeId project, std::string_view set)
{
    Engine& e = engine();
    std::scoped_lock lock(e.pubMutex);
    auto it = e.published.find({project, std::string(set)});
    return it == e.published.end() ? std::vector<MeshMapInfo>{} : it->second;
}

int addMeshMapSetListener(std::function<void(const MeshMapChange&)> fn)
{
    Engine& e = engine();
    std::scoped_lock lock(e.listenerMutex);
    const int id = e.nextListener++;
    e.listeners[id] = std::move(fn);
    return id;
}

void removeMeshMapSetListener(int id)
{
    Engine& e = engine();
    std::scoped_lock lock(e.listenerMutex);
    e.listeners.erase(id);
}

void enableLiveMaps(scene::Graph& g, NodeId project, const LiveMapsOptions& options, std::string_view set)
{
    LILA_ZONE();

    scene::Node* p = g.node(project);
    if (!p || !bake::isProject(*p))
        throw std::runtime_error("enableLiveMaps: not a bake project");
    const bake::ProjectSettings ps = bake::projectSettings(*p);
    auto s = std::make_shared<Session>();
    s->project = project;
    s->projectName = p->name();
    s->set = std::string(set);
    s->options = options;
    s->requested = requestedMaps(ps, options);
    if (s->requested.empty())
        throw std::runtime_error("enableLiveMaps: " + p->name() + " has no maps to bake");
    const int res = std::clamp(options.resolution > 0 ? options.resolution : pref("/liveResolution", 1024), 16, 8192);
    s->output = ps.output;
    s->output.width = s->output.height = res;
    s->output.supersampling = options.supersampling > 0 ? options.supersampling : pref("/liveSupersampling", 1);
    s->output.padding = options.padding >= 0 ? options.padding
                        : ps.output.padding < 0 ? -1
                                                : std::max(2, int(std::lround(double(ps.output.padding) * res / std::max(1, ps.output.width))));
    s->debounceMs = std::max(0, options.debounceMs >= 0 ? options.debounceMs : pref("/liveDebounceMs", 150));
    s->sliceMs = double(std::max(2, pref("/liveSliceMs", 8)));

    Engine& e = engine();
    listen(g);
    std::shared_ptr<Session> replaced;
    {
        std::scoped_lock lock(e.mutex);
        auto& slot = e.sessions[{project, s->set}];
        if (slot)
            s->offsets = slot->offsets;
        replaced = std::exchange(slot, s);
        s->dirty = true;
        s->dirtySince = Clock::now() - std::chrono::milliseconds(s->debounceMs);
    }
    replaced.reset();
    if (e.async)
    {
        startWorker();
        e.cv.notify_all();
        return;
    }
    const json r = updateLiveMaps(g, project, set);
    if (r.contains("error"))
        throw std::runtime_error("enableLiveMaps: " + r["error"].get<std::string>());
}

void disableLiveMaps(NodeId project, std::string_view set)
{
    Engine& e = engine();
    std::shared_ptr<Session> s;
    {
        std::scoped_lock lock(e.mutex);
        auto it = e.sessions.find({project, std::string(set)});
        if (it == e.sessions.end())
            return;
        s = it->second;
        e.sessions.erase(it);
        std::scoped_lock pub(e.pubMutex);
        e.published.erase({project, std::string(set)});
    }
    s.reset();
}

bool liveMapsEnabled(NodeId project, std::string_view set) { return findSession(project, set) != nullptr; }

json liveMapsStatus(NodeId project, std::string_view set)
{
    std::shared_ptr<Session> s = findSession(project, set);
    if (!s)
        return {{"enabled", false}, {"set", set}};
    Engine& e = engine();
    std::scoped_lock lock(e.mutex);
    json j = {{"enabled", true},
              {"set", s->set},
              {"project", s->projectName},
              {"maps", s->mapIds},
              {"skipped", s->skipped},
              {"resolution", s->output.width},
              {"supersampling", s->output.supersampling},
              {"padding", s->output.padding},
              {"tileSize", kTile},
              {"debounceMs", s->debounceMs},
              {"updates", s->updates},
              {"fullBakes", s->fullBakes},
              {"busy", s->busy || s->pending != nullptr || s->snapshotRequested},
              {"dirty", s->dirty},
              {"async", e.async},
              {"last", s->last.is_null() ? json::object() : s->last}};
    if (!s->error.empty())
        j["error"] = s->error;
    j["images"] = meshMapSet(project, set).size();
    return j;
}

void setLiveCageOffsets(NodeId project, std::string_view set, const std::string& low, std::vector<float> values)
{
    Engine& e = engine();
    {
        std::scoped_lock lock(e.mutex);
        auto it = e.sessions.find({project, std::string(set)});
        if (it == e.sessions.end())
            return;
        if (values.empty())
            it->second->offsets.erase(low);
        else
            it->second->offsets[low] = std::move(values);
        markDirty(*it->second);
    }
    e.cv.notify_all();
}

json updateLiveMaps(scene::Graph& g, NodeId project, std::string_view set)
{
    LILA_ZONE();

    std::shared_ptr<Session> s = findSession(project, set);
    if (!s)
        throw std::runtime_error("updateLiveMaps: no live set for that project");
    Engine& e = engine();
    std::vector<NodeId> groups;
    std::map<std::string, std::vector<float>> offsets;
    {
        std::scoped_lock lock(e.mutex);
        s->dirty = false;
        groups = s->options.groups;
        offsets = s->offsets;
    }
    std::unique_ptr<bake::BakeInputs> inputs;
    std::set<NodeId> watched;
    try
    {
        inputs = snapshot(g, project, groups, offsets);
        watched = watchedNodes(g, project, groups);
    }
    catch (const std::exception& ex)
    {
        std::scoped_lock lock(e.mutex);
        s->error = ex.what();
        return {{"error", s->error}};
    }
    {
        std::scoped_lock lock(e.mutex);
        s->watched = std::move(watched);
    }
    return updateSession(s, std::move(inputs));
}

void setLiveMapsAsync(bool on)
{
    Engine& e = engine();
    e.async = on;
    if (on)
    {
        bool any = false;
        {
            std::scoped_lock lock(e.mutex);
            any = !e.sessions.empty();
        }
        if (any)
            startWorker();
    }
}

bool liveMapsIdle()
{
    Engine& e = engine();
    std::scoped_lock lock(e.mutex);
    return std::ranges::none_of(e.sessions, [&](const auto& kv) {
        const Session& s = *kv.second;
        return (e.async && s.dirty) || s.busy || s.pending || s.snapshotRequested;
    });
}

void shutdownLiveMaps()
{
    Engine& e = engine();
    std::map<SessionKey, std::shared_ptr<Session>> sessions;
    {
        std::scoped_lock lock(e.mutex);
        e.stop = true;
        sessions.swap(e.sessions);
        std::scoped_lock pub(e.pubMutex);
        e.published.clear();
    }
    e.cv.notify_all();
    if (e.worker.joinable())
        e.worker.join();
    sessions.clear();
    if (e.graphListener)
    {
        scene::Graph::current().removeListener(e.graphListener);
        e.graphListener = 0;
    }
    e.stop = false;
}

/** A live map read back for CPU evaluation, with the file's values (sRGB maps stay encoded) and single channels in rgb. */
static image::Image readLiveMap(const std::string& projectName, const std::string& map, int udim)
{
    auto m = findMeshMap(projectName, map, udim);
    if (!m || !m->image || !Renderer::get().running())
        return {};
    std::vector<uint8_t> bytes;
    try
    {
        bytes = readGpuImage(m->image);
    }
    catch (const std::exception& e)
    {
        log::warn("Reading live mesh map {} of {}: {}", map, projectName, e.what());
        return {};
    }
    if (bytes.empty())
        return {};
    image::Image img{m->image->width(), m->image->height(), {}};
    decodeTexels(m->image->format(), false, bytes, img.rgba);
    if (channelCount(m->image->format()) == 1)
        for (size_t i = 0; i < img.rgba.size(); i += 4)
            img.rgba[i + 1] = img.rgba[i + 2] = img.rgba[i];
    return img;
}

void registerLiveMapCommands()
{
    texgpu::setMeshMapSource(meshMapSource);
    tex::setLiveMeshMapReader(readLiveMap);
    tex::setBakedMapsListener([](const std::string& project) {
        {
            BakedFiles& b = bakedFiles();
            std::scoped_lock lock(b.mutex);
            std::erase_if(b.files, [&](const auto& f) { return std::get<0>(f.first) == project; });
            std::erase_if(b.misses, [&](const auto& k) { return std::get<0>(k) == project; });
        }
        texgpu::meshMapsChanged(project);
    });
    using T = ArgType;
    static const FlagDef kProject{"project", "pj", T::String, nullptr, "The bake project; defaults to the scene's only one."};
    static const FlagDef kSet{"set", "st", T::String, "", "The set: empty for the project's own, cagePreview for the cage view's."};
    auto projectOf = [](const Args& a, const char* cmd) -> scene::Node& {
        scene::Graph& g = scene::Graph::current();
        if (a.has("project"))
        {
            scene::Node* n = nullptr;
            try
            {
                n = g.find(a.get<std::string>("project"));
            }
            catch (const std::exception&)
            {
            }
            if (!n || !bake::isProject(*n))
                throw CommandError(std::string(cmd) + ": " + a.get<std::string>("project") + " is not a bake project");
            return *n;
        }
        const std::vector<NodeId> ps = bake::projects(g);
        if (ps.size() != 1)
            throw CommandError(std::string(cmd) + (ps.empty() ? ": the scene has no bake project" : ": several bake projects; name one with project="));
        return *g.node(ps.front());
    };

    ops::registerCommand({
        .name = "bakeLiveMaps",
        .doc = "Live mesh maps: keeps a bake project's maps on the GPU at a preview resolution, for texture graphs and the viewport, and "
               "rebakes only the texel tiles an edit to a low or cage touches. enable=True starts (or reconfigures) it, disable=True "
               "drops it, update=True rebakes what changed now and waits; with none of them, returns the status.",
        .flags = {kProject,
                  {"enable", "e", T::Bool, false, "Start, or restart with these options."},
                  {"disable", "d", T::Bool, false, "Stop and drop the images."},
                  {"update", "u", T::Bool, false, "Snapshot now and rebake what changed, waiting for it; returns the update's stats."},
                  {"maps", "m", T::List, nullptr, "Map ids from the project's list; default every enabled map the GPU bakes."},
                  {"groups", "g", T::List, nullptr, "Bake groups to include; default all of the project's."},
                  {"resolution", "r", T::Int, 0, "Texels per side; 0 uses the bake/liveResolution pref (1024).", {}, {}, 0.0, 8192.0},
                  {"supersampling", "ss", T::Int, 0, "Samples per texel (1, 4, 16, 64); 0 uses the bake/liveSupersampling pref (1).", {}, {}, 0.0, 64.0},
                  {"padding", "pd", T::Int, -1, "Dilation in texels; -1 scales the project's padding to the resolution.", {}, {}, -1.0},
                  {"debounce", "db", T::Int, -1, "Milliseconds of quiet after an edit before rebaking; -1 uses the bake/liveDebounceMs pref.", {}, {}, -1.0},
                  kSet},
        .undoable = false,
        .run = [projectOf](const Args& a) -> json {
            scene::Graph& g = scene::Graph::current();
            scene::Node& p = projectOf(a, "bakeLiveMaps");
            const std::string set = a.get<std::string>("set");
            if (a.get<bool>("disable"))
            {
                disableLiveMaps(p.id(), set);
                return liveMapsStatus(p.id(), set);
            }
            if (a.get<bool>("enable"))
            {
                LiveMapsOptions o;
                if (a.has("maps"))
                    for (const json& m : a.flags["maps"])
                        o.maps.push_back(m.get<std::string>());
                if (a.has("groups"))
                    for (const json& gname : a.flags["groups"])
                    {
                        scene::Node* n = nullptr;
                        try
                        {
                            n = g.find(gname.get<std::string>());
                        }
                        catch (const std::exception&)
                        {
                        }
                        if (!n || !bake::isGroup(*n))
                            throw CommandError("bakeLiveMaps: " + gname.get<std::string>() + " is not a bake group");
                        o.groups.push_back(n->id());
                    }
                o.resolution = a.get<int>("resolution");
                o.supersampling = a.get<int>("supersampling");
                o.padding = a.get<int>("padding", -1);
                o.debounceMs = a.get<int>("debounce", -1);
                try
                {
                    enableLiveMaps(g, p.id(), o, set);
                }
                catch (const std::exception& ex)
                {
                    throw CommandError(ex.what());
                }
            }
            if (a.get<bool>("update"))
            {
                if (!liveMapsEnabled(p.id(), set))
                    throw CommandError("bakeLiveMaps: " + p.name() + " has no live maps (enable=True)");
                json r = updateLiveMaps(g, p.id(), set);
                json status = liveMapsStatus(p.id(), set);
                status["update"] = r;
                return status;
            }
            return liveMapsStatus(p.id(), set);
        },
    });

    ops::registerCommand({
        .name = "bakeMapInfo",
        .doc = "The live mesh map images of a bake project (bakeLiveMaps): per low, map and UDIM tile its version (bumped on every rebake), "
               "resolution, format, colour space and decode data. map=, udim= and low= narrow it down.",
        .flags = {kProject,
                  {"map", "m", T::String, nullptr, "Only this map id."},
                  {"udim", "ud", T::Int, nullptr, "Only this UDIM tile.", {}, {}, 1001.0},
                  nodeFlag({"low", "lo", T::String, nullptr, "Only this low (node path)."}, "mesh"),
                  kSet},
        .undoable = false,
        .run = [projectOf](const Args& a) -> json {
            scene::Node& p = projectOf(a, "bakeMapInfo");
            const std::string set = a.get<std::string>("set");
            json images = json::array();
            for (const MeshMapInfo& m : meshMapSet(p.id(), set))
            {
                if (a.has("map") && m.map != a.get<std::string>("map"))
                    continue;
                if (a.has("udim") && m.udim != a.get<int>("udim"))
                    continue;
                if (a.has("low") && m.low != a.get<std::string>("low") && !m.low.ends_with("|" + a.get<std::string>("low")))
                    continue;
                images.push_back(m.toJson());
            }
            json j = liveMapsStatus(p.id(), set);
            j["images"] = images;
            return j;
        },
    });
}
} // namespace lila::render
