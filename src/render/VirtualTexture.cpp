#include "render/VirtualTexture.h"

#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "render/Renderer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace lila::render::vt
{
namespace
{
constexpr uint32_t kGridCells = 100;
constexpr uint32_t kGridWords = kGridCells * 4;
constexpr uint32_t kSlotsPerChunk = uint32_t(kChunkSlots * kChunkSlots);
constexpr uint32_t kResident = 0x80000000u;
constexpr uint32_t kNoSlot = 0xffffffffu;
constexpr uint32_t kHashSize = 1u << 15;
constexpr uint32_t kListCap = 1u << 15;
constexpr uint32_t kFadeStep = 64;
/** Pages one streaming slice makes at most. */
constexpr size_t kJobsPerSlice = 48;
/** Pages a texture keeps queued at most; older requests drop off (a later feedback asks again). */
constexpr size_t kMaxQueued = 8192;
/** Frames a sampled page is safe from eviction. */
constexpr uint64_t kKeepFrames = 2;
constexpr VkShaderStageFlags kCompute = VK_SHADER_STAGE_COMPUTE_BIT;

uint64_t keyOf(const PageId& p) { return uint64_t(p.tile) << 48 | uint64_t(p.mip) << 40 | uint64_t(uint32_t(p.y)) << 20 | uint64_t(uint32_t(p.x)); }

PageId pageOf(uint64_t k) { return {int(k >> 48), int((k >> 40) & 0xff), int(k & 0xfffff), int((k >> 20) & 0xfffff)}; }

size_t layerBytes(const LayerFormat& f) { return size_t(texelBytes(f.format)); }

struct Space;

struct DedupePush
{
    VkDeviceAddress cells;
    VkDeviceAddress table;
    VkDeviceAddress list;
    uint32_t count, groupsX, hashMask, cap;
};

struct Retired
{
    compute::Buffer buffer;
    uint64_t frame = 0;
};
} // namespace

struct Texture::Impl
{
    Texture* self = nullptr;
    std::shared_ptr<Producer> producer;
    Space* space = nullptr;
    struct TileInfo
    {
        int udim = 1001;
        int width = 0, height = 0, mips = 1;
        std::vector<uint32_t> levelBase;
    };
    std::vector<TileInfo> tiles;
    /** Grid cell (row * 10 + column) -> tile index. */
    std::array<int, kGridCells> cellTile;
    std::vector<uint32_t> shadow;
    std::set<uint32_t> dirty;
    bool allDirty = true;
    compute::Buffer table;
    std::unordered_map<uint64_t, uint32_t> resident;
    std::set<uint64_t> stale;
    std::set<uint64_t> pinned;
    std::set<uint64_t> permanent;
    std::set<uint64_t> fading;
    /** Missing pages and the feedback frame that last asked. */
    std::map<uint64_t, uint64_t> queue;
    std::vector<std::weak_ptr<const TextureImage>> maps;
    size_t produced = 0, requested = 0;
    std::vector<std::array<int, 4>> lastRequests;
    uint64_t lastRequestFrame = 0;

    uint32_t entryIndex(const PageId& p) const
    {
        const TileInfo& t = tiles[size_t(p.tile)];
        const auto [px, py] = self->pages(p.tile, p.mip);
        return t.levelBase[size_t(p.mip)] + uint32_t(p.y) * uint32_t(px) + uint32_t(p.x);
    }
    void setEntry(const PageId& p, uint32_t value)
    {
        const uint32_t i = entryIndex(p);
        if (shadow[i] != value)
        {
            shadow[i] = value;
            dirty.insert(i);
        }
    }
    bool valid(const PageId& p) const
    {
        if (p.tile < 0 || size_t(p.tile) >= tiles.size() || p.mip < 0 || p.mip >= tiles[size_t(p.tile)].mips)
            return false;
        const auto [px, py] = self->pages(p.tile, p.mip);
        return p.x >= 0 && p.y >= 0 && p.x < px && p.y < py;
    }
};

namespace
{
struct Space
{
    std::vector<LayerFormat> formats;
    size_t slotBytes = 0;
    struct Chunk
    {
        std::vector<GpuImageRef> layers;
    };
    std::vector<Chunk> chunks;
    std::vector<uint32_t> free;
    struct Slot
    {
        Texture::Impl* owner = nullptr;
        uint64_t page = 0;
        uint64_t lastUse = 0;
    };
    std::vector<Slot> slots;

    size_t chunkBytes() const { return slotBytes * kSlotsPerChunk; }
};

struct Global
{
    std::recursive_mutex mutex;
    std::vector<std::unique_ptr<Space>> spaces;
    std::map<uint32_t, Texture::Impl*> textures;
    uint32_t nextId = 1;
    size_t budgetOverride = 0;
    size_t allocated = 0;
    uint64_t frame = 1;
    size_t produced = 0, evicted = 0, feedbackFrames = 0;
    bool taskQueued = false;
    bool working = false;
    std::function<void()> redraw;
    std::vector<Retired> retired;
    uint64_t renderFrames = 0;
    bool hookInstalled = false;

    // Render thread.
    VkPipelineLayout dedupeLayout = VK_NULL_HANDLE;
    std::vector<uint32_t> dedupeCode;
    uint64_t dedupeHash = 0;
    compute::Submitter sub;
    bool subReady = false;
};

// Leaked on purpose: textures may be dropped during static destruction.
Global& g()
{
    static Global* s = new Global;
    return *s;
}

void requestRedraw()
{
    mainq::post([] {
        std::function<void()> fn;
        {
            std::scoped_lock lock(g().mutex);
            fn = g().redraw;
        }
        if (fn)
            fn();
    });
}

void installHook()
{
    Global& s = g();
    if (s.hookInstalled)
        return;
    s.hookInstalled = true;
    Renderer::get().addStopHook([](VulkanContext& vk) { releaseAll(vk); });
}

Space& spaceFor(const std::vector<LayerFormat>& formats)
{
    Global& s = g();
    for (auto& sp : s.spaces)
        if (sp->formats == formats)
            return *sp;
    auto sp = std::make_unique<Space>();
    sp->formats = formats;
    for (const LayerFormat& f : formats)
        sp->slotBytes += layerBytes(f) * size_t(kSlotSize) * size_t(kSlotSize);
    s.spaces.push_back(std::move(sp));
    return *s.spaces.back();
}

/** Frees a slot (its page is no longer resident). Lock held. */
void releaseSlot(Space& sp, uint32_t slot)
{
    if (slot == kNoSlot || slot >= sp.slots.size())
        return;
    sp.slots[slot] = {};
    sp.free.push_back(slot);
}

/** Drops a resident page: its entry, its slot. Lock held. */
void evictPage(Texture::Impl& t, uint64_t key)
{
    auto it = t.resident.find(key);
    if (it == t.resident.end())
        return;
    releaseSlot(*t.space, it->second);
    t.resident.erase(it);
    t.stale.erase(key);
    t.fading.erase(key);
    t.setEntry(pageOf(key), 0);
    ++g().evicted;
}

/** A free slot, a new chunk while the budget allows, else the least recently used evictable page's. Lock held. */
uint32_t allocSlot(Space& sp)
{
    Global& s = g();
    if (!sp.free.empty())
    {
        const uint32_t slot = sp.free.back();
        sp.free.pop_back();
        return slot;
    }
    if (s.allocated + sp.chunkBytes() <= budget() && sp.chunks.size() * kSlotsPerChunk < 65535)
    {
        Space::Chunk c;
        const int size = kSlotSize * kChunkSlots;
        for (size_t l = 0; l < sp.formats.size(); ++l)
        {
            GpuImageDesc d;
            d.width = d.height = size;
            d.format = sp.formats[l].format;
            d.srgb = sp.formats[l].srgb;
            d.name = "vt chunk " + std::to_string(sp.chunks.size()) + "." + std::to_string(l);
            c.layers.push_back(GpuImage::create(d));
        }
        const uint32_t base = uint32_t(sp.chunks.size()) * kSlotsPerChunk;
        sp.chunks.push_back(std::move(c));
        sp.slots.resize(sp.slots.size() + kSlotsPerChunk);
        for (uint32_t i = kSlotsPerChunk; i-- > 1;)
            sp.free.push_back(base + i);
        s.allocated += sp.chunkBytes();
        return base;
    }
    uint32_t best = kNoSlot;
    uint64_t bestUse = ~uint64_t(0);
    for (uint32_t i = 0; i < sp.slots.size(); ++i)
    {
        const Space::Slot& sl = sp.slots[i];
        if (!sl.owner || sl.lastUse + kKeepFrames >= s.frame || sl.lastUse >= bestUse)
            continue;
        if (sl.owner->pinned.contains(sl.page) || sl.owner->permanent.contains(sl.page))
            continue;
        best = i;
        bestUse = sl.lastUse;
    }
    if (best == kNoSlot)
        return kNoSlot;
    Space::Slot victim = sp.slots[best];
    evictPage(*victim.owner, victim.page);
    // evictPage put it on the free list.
    sp.free.pop_back();
    return best;
}

/** Marks a page and its resident ancestors used this frame, and queues the missing ones. Lock held. */
void want(Texture::Impl& t, PageId p, uint64_t frame)
{
    for (int l = p.mip; l < t.tiles[size_t(p.tile)].mips; ++l)
    {
        const PageId q{p.tile, l, p.x >> (l - p.mip), p.y >> (l - p.mip)};
        if (!t.valid(q))
            break;
        const uint64_t k = keyOf(q);
        if (auto it = t.resident.find(k); it != t.resident.end())
            t.space->slots[it->second].lastUse = frame;
        else
            t.queue[k] = frame;
    }
    if (t.queue.size() > kMaxQueued)
    {
        // Drop the oldest requests.
        std::vector<std::pair<uint64_t, uint64_t>> byAge;
        for (auto& [k, f] : t.queue)
            byAge.emplace_back(f, k);
        std::sort(byAge.begin(), byAge.end());
        for (size_t i = 0; i + kMaxQueued < byAge.size(); ++i)
            if (!t.permanent.contains(byAge[i].second))
                t.queue.erase(byAge[i].second);
    }
}

void ensureTask();

/** Requests from one frame's feedback: pairs of words (see mesh.slang vtFeedback). */
void processRequests(std::span<const uint32_t> words)
{
    Global& s = g();
    {
        std::scoped_lock lock(s.mutex);
        const uint64_t frame = ++s.frame;
        ++s.feedbackFrames;
        for (size_t i = 0; i + 1 < words.size(); i += 2)
        {
            const uint32_t a = words[i], b = words[i + 1];
            if ((a & 0x80000000u) == 0)
                continue;
            auto it = s.textures.find(a & 0xffffu);
            if (it == s.textures.end())
                continue;
            Texture::Impl& t = *it->second;
            const uint32_t cell = (a >> 16) & 0xffu;
            if (cell >= kGridCells || t.cellTile[cell] < 0)
                continue;
            const PageId p{t.cellTile[cell], int((a >> 24) & 0x7fu), int(b & 0xffffu), int(b >> 16)};
            if (!t.valid(p))
                continue;
            if (t.lastRequestFrame != frame)
            {
                t.lastRequestFrame = frame;
                t.lastRequests.clear();
            }
            t.lastRequests.push_back({p.tile, p.mip, p.x, p.y});
            ++t.requested;
            want(t, p, frame);
        }
    }
    ensureTask();
}

/** Pages to make next: stale pages in use first, then missing ones coarse to fine, newest requests first. Lock held. */
std::vector<uint64_t> pickJobs(Texture::Impl& t, size_t max)
{
    struct Cand
    {
        int priority;
        uint64_t key;
    };
    std::vector<Cand> c;
    for (uint64_t k : t.stale)
    {
        const auto it = t.resident.find(k);
        const uint64_t use = it == t.resident.end() ? 0 : t.space->slots[it->second].lastUse;
        c.push_back({t.permanent.contains(k) || t.pinned.contains(k) ? 0 : use + kKeepFrames >= g().frame ? 1 : 3, k});
    }
    for (auto& [k, f] : t.queue)
        c.push_back({t.permanent.contains(k) ? 0 : 2, k});
    std::stable_sort(c.begin(), c.end(), [&](const Cand& a, const Cand& b) {
        if (a.priority != b.priority)
            return a.priority < b.priority;
        const PageId pa = pageOf(a.key), pb = pageOf(b.key);
        if (pa.mip != pb.mip)
            return pa.mip > pb.mip;
        auto fa = t.queue.find(a.key), fb = t.queue.find(b.key);
        const uint64_t ua = fa == t.queue.end() ? 0 : fa->second, ub = fb == t.queue.end() ? 0 : fb->second;
        return ua > ub;
    });
    std::vector<uint64_t> out;
    for (const Cand& x : c)
    {
        if (out.size() >= max)
            break;
        out.push_back(x.key);
    }
    return out;
}

/** One streaming slice (render thread): makes up to kJobsPerSlice pages and copies them into their slots. */
bool streamSlice(VulkanContext& vk)
{
    LILA_ZONE();
    Global& s = g();
    if (!s.subReady)
    {
        s.sub.init(vk);
        s.subReady = true;
    }
    struct Work
    {
        Texture::Impl* t;
        std::shared_ptr<Producer> producer;
        std::shared_ptr<Texture> keep;
        std::vector<Job> jobs;
        std::vector<uint32_t> slots;
        std::vector<bool> redo;
    };
    std::vector<Work> work;
    size_t total = 0;
    {
        std::scoped_lock lock(s.mutex);
        s.working = true;
        for (auto& [id, t] : s.textures)
        {
            if (total >= kJobsPerSlice)
                break;
            if (t->queue.empty() && t->stale.empty())
                continue;
            std::shared_ptr<Texture> keep;
            try
            {
                keep = t->self->shared_from_this();
            }
            catch (const std::bad_weak_ptr&)
            {
                continue;
            }
            Work w{t, t->producer, keep, {}, {}, {}};
            for (uint64_t k : pickJobs(*t, kJobsPerSlice - total))
            {
                const PageId p = pageOf(k);
                Job j;
                j.page = p;
                j.udim = t->tiles[size_t(p.tile)].udim;
                std::tie(j.levelWidth, j.levelHeight) = t->self->levelSize(p.tile, p.mip);
                j.x0 = p.x * kPageSize - kBorder;
                j.y0 = p.y * kPageSize - kBorder;
                w.jobs.push_back(std::move(j));
            }
            total += w.jobs.size();
            if (!w.jobs.empty())
                work.push_back(std::move(w));
        }
    }
    if (work.empty())
    {
        std::scoped_lock lock(s.mutex);
        s.working = false;
        s.taskQueued = false;
        bool more = false;
        for (auto& [id, t] : s.textures)
            more = more || !t->queue.empty() || !t->stale.empty();
        return !more;
    }

    // Inputs first (graph evaluations the producers step): each submits its own work.
    bool preparing = false;
    for (Work& w : work)
        if (!w.producer || !w.producer->prepare(vk, w.jobs, 30.0))
        {
            preparing = preparing || w.producer != nullptr;
            w.jobs.clear();
        }
    std::erase_if(work, [](const Work& w) { return w.jobs.empty(); });

    // Slots: a stale page keeps its own, a missing one takes a free (or evicted) one.
    {
        std::scoped_lock lock(s.mutex);
        for (Work& w : work)
        {
            Texture::Impl& t = *w.t;
            std::vector<Job> kept;
            for (Job& j : w.jobs)
            {
                const uint64_t k = keyOf(j.page);
                uint32_t slot = kNoSlot;
                bool redo = false;
                if (auto it = t.resident.find(k); it != t.resident.end())
                {
                    slot = it->second;
                    redo = true;
                }
                else
                    slot = allocSlot(*t.space);
                if (slot == kNoSlot)
                    continue;
                if (!redo)
                    t.space->slots[slot] = {&t, k, s.frame};
                w.slots.push_back(slot);
                w.redo.push_back(redo);
                kept.push_back(std::move(j));
            }
            w.jobs = std::move(kept);
        }
    }
    std::erase_if(work, [](const Work& w) { return w.jobs.empty(); });
    if (work.empty())
    {
        // Nothing to make now: keep going while a producer is still preparing, else wait for the next request.
        std::scoped_lock lock(s.mutex);
        s.working = false;
        if (preparing)
            return false;
        s.taskQueued = false;
        return true;
    }

    // Staging for every job's layers, then the producers record into it and the slices copy it into the chunks.
    VkDeviceSize bytes = 0;
    for (Work& w : work)
        for (Job& j : w.jobs)
            for (const LayerFormat& f : w.t->space->formats)
            {
                (void)f;
                j.layers.push_back(bytes);
                bytes += (VkDeviceSize(layerBytes(f)) * kSlotSize * kSlotSize + 255) & ~VkDeviceSize(255);
            }
    std::vector<compute::Buffer> temps;
    compute::Buffer staging;
    std::vector<std::vector<uint8_t>> made(work.size());
    try
    {
        staging = compute::makeBuffer(vk, std::max<VkDeviceSize>(bytes, 256), 0, compute::Host::None);
        for (Work& w : work)
            for (Job& j : w.jobs)
            {
                for (VkDeviceAddress& a : j.layers)
                    a += staging.address;
                j.staging = staging.buffer;
                j.stagingAddress = staging.address;
            }
        s.sub.run(vk, [&](VkCommandBuffer cmd) {
            for (size_t i = 0; i < work.size(); ++i)
            {
                made[i] = work[i].producer->record(vk, cmd, work[i].jobs, temps);
                made[i].resize(work[i].jobs.size(), 0);
            }
            compute::memoryBarrier(cmd);
            // Chunk images written this slice, prepared once.
            std::set<GpuImage*> open;
            for (size_t i = 0; i < work.size(); ++i)
            {
                Space& sp = *work[i].t->space;
                for (size_t k = 0; k < work[i].jobs.size(); ++k)
                {
                    if (!made[i][k])
                        continue;
                    const Job& j = work[i].jobs[k];
                    const uint32_t slot = work[i].slots[k];
                    const Space::Chunk& c = sp.chunks[slot / kSlotsPerChunk];
                    const uint32_t within = slot % kSlotsPerChunk;
                    for (size_t l = 0; l < sp.formats.size(); ++l)
                    {
                        GpuImage& img = *c.layers[l];
                        if (open.insert(&img).second)
                        {
                            std::vector<compute::Buffer> st;
                            img.prepare(vk, cmd, st);
                            temps.insert(temps.end(), st.begin(), st.end());
                            img.beginWrite(cmd);
                            compute::memoryBarrier(cmd);
                        }
                        VkBufferImageCopy region{};
                        region.bufferOffset = j.layers[l] - staging.address;
                        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                        region.imageOffset = {int32_t(within % kChunkSlots) * kSlotSize, int32_t(within / kChunkSlots) * kSlotSize, 0};
                        region.imageExtent = {uint32_t(kSlotSize), uint32_t(kSlotSize), 1};
                        vkCmdCopyBufferToImage(cmd, staging.buffer, img.image(), VK_IMAGE_LAYOUT_GENERAL, 1, &region);
                    }
                }
            }
            compute::memoryBarrier(cmd);
            for (GpuImage* img : open)
                img->endWrite(cmd);
        });
    }
    catch (const std::exception& e)
    {
        log::warn("virtual texture: {}", e.what());
        for (auto& m : made)
            m.assign(m.size(), 0);
    }
    for (compute::Buffer& b : temps)
        compute::destroy(vk, b);
    compute::destroy(vk, staging);

    bool landed = false;
    {
        std::scoped_lock lock(s.mutex);
        for (size_t i = 0; i < work.size(); ++i)
        {
            Texture::Impl& t = *work[i].t;
            for (size_t k = 0; k < work[i].jobs.size(); ++k)
            {
                const uint64_t key = keyOf(work[i].jobs[k].page);
                const uint32_t slot = work[i].slots[k];
                if (k >= made[i].size() || !made[i][k])
                {
                    // Not made: a fresh slot goes back.
                    if (!work[i].redo[k])
                        releaseSlot(*t.space, slot);
                    continue;
                }
                landed = true;
                ++t.produced;
                ++s.produced;
                t.queue.erase(key);
                t.stale.erase(key);
                if (work[i].redo[k])
                    continue;
                t.resident[key] = slot;
                t.fading.insert(key);
                t.setEntry(work[i].jobs[k].page, kResident | slot);
            }
        }
        s.working = false;
    }
    if (landed)
        requestRedraw();
    std::scoped_lock lock(s.mutex);
    bool more = false;
    for (auto& [id, t] : s.textures)
        more = more || !t->queue.empty() || !t->stale.empty();
    if (more && (landed || preparing))
        return false;
    s.taskQueued = false;
    return true;
}

void ensureTask()
{
    Global& s = g();
    {
        std::scoped_lock lock(s.mutex);
        if (s.taskQueued)
            return;
        bool any = false;
        for (auto& [id, t] : s.textures)
            any = any || !t->queue.empty() || !t->stale.empty();
        if (!any)
            return;
        s.taskQueued = true;
    }
    Renderer& r = Renderer::get();
    if (!r.running())
    {
        std::scoped_lock lock(s.mutex);
        s.taskQueued = false;
        return;
    }
    installHook();
    r.runTask(streamSlice);
}
} // namespace

// ---------------------------------------------------------------------------------------------------- Texture

Texture::Texture(std::string name, std::vector<LayerFormat> layers, std::vector<TileDesc> tiles, std::shared_ptr<Producer> producer)
    : mName(std::move(name)), mLayers(std::move(layers)), mTiles(std::move(tiles)), m(std::make_unique<Impl>())
{
    m->self = this;
    m->producer = std::move(producer);
    m->cellTile.fill(-1);
    uint32_t base = kGridWords;
    for (size_t i = 0; i < mTiles.size(); ++i)
    {
        Impl::TileInfo t;
        t.udim = mTiles[i].udim;
        t.width = mTiles[i].width;
        t.height = mTiles[i].height;
        t.mips = 1;
        while (std::max(std::max(t.width >> (t.mips - 1), 1), std::max(t.height >> (t.mips - 1), 1)) > kPageSize)
            ++t.mips;
        m->tiles.push_back(t);
        m->cellTile[size_t(t.udim - 1001)] = int(i);
    }
    for (size_t i = 0; i < m->tiles.size(); ++i)
    {
        Impl::TileInfo& t = m->tiles[i];
        for (int l = 0; l < t.mips; ++l)
        {
            t.levelBase.push_back(base);
            const auto [px, py] = pages(int(i), l);
            base += uint32_t(px * py);
        }
    }
    m->shadow.assign(base, 0u);
    for (size_t i = 0; i < m->tiles.size(); ++i)
    {
        const Impl::TileInfo& t = m->tiles[i];
        uint32_t* cell = &m->shadow[size_t(t.udim - 1001) * 4];
        cell[0] = t.levelBase[0] + 1;
        cell[1] = uint32_t(t.width);
        cell[2] = uint32_t(t.height);
        cell[3] = uint32_t(t.mips);
    }
    m->maps.resize(mLayers.size());
}

std::shared_ptr<Texture> Texture::create(std::string name, std::vector<LayerFormat> layers, std::vector<TileDesc> tiles,
                                         std::shared_ptr<Producer> producer)
{
    if (layers.empty() || tiles.empty())
        throw std::invalid_argument("virtual texture: no layers or no tiles");
    std::set<int> seen;
    for (const TileDesc& t : tiles)
    {
        if (t.udim < 1001 || t.udim > 1100 || !seen.insert(t.udim).second)
            throw std::invalid_argument("virtual texture: tile " + std::to_string(t.udim) + " is outside 1001-1100 or repeated");
        if (t.width < 1 || t.height < 1 || t.width > 65536 || t.height > 65536)
            throw std::invalid_argument("virtual texture: tile " + std::to_string(t.udim) + " size out of range");
    }
    std::shared_ptr<Texture> tex(new Texture(std::move(name), std::move(layers), std::move(tiles), std::move(producer)));
    Global& s = g();
    {
        std::scoped_lock lock(s.mutex);
        tex->m->space = &spaceFor(tex->mLayers);
        while (s.textures.contains(s.nextId) || s.nextId == 0 || s.nextId > 0xffff)
            s.nextId = s.nextId >= 0xffff ? 1 : s.nextId + 1;
        tex->mId = s.nextId++;
        s.textures[tex->mId] = tex->m.get();
        // Each tile's coarsest page stays resident: the fallback every lookup ends at.
        for (size_t i = 0; i < tex->m->tiles.size(); ++i)
        {
            const uint64_t k = keyOf({int(i), tex->m->tiles[i].mips - 1, 0, 0});
            tex->m->permanent.insert(k);
            tex->m->queue[k] = s.frame;
        }
    }
    ensureTask();
    return tex;
}

Texture::~Texture()
{
    Global& s = g();
    std::scoped_lock lock(s.mutex);
    s.textures.erase(mId);
    for (auto& [k, slot] : m->resident)
        releaseSlot(*m->space, slot);
    m->resident.clear();
    if (m->table.buffer)
        s.retired.push_back({m->table, s.renderFrames});
    m->table = {};
}

int Texture::mips(int tile) const { return m->tiles[size_t(tile)].mips; }

std::pair<int, int> Texture::levelSize(int tile, int mip) const
{
    const Impl::TileInfo& t = m->tiles[size_t(tile)];
    return {std::max(t.width >> mip, 1), std::max(t.height >> mip, 1)};
}

std::pair<int, int> Texture::pages(int tile, int mip) const
{
    const auto [w, h] = levelSize(tile, mip);
    return {(w + kPageSize - 1) / kPageSize, (h + kPageSize - 1) / kPageSize};
}

int Texture::tileOf(int udim) const
{
    if (udim < 1001 || udim > 1100)
        return -1;
    return m->cellTile[size_t(udim - 1001)];
}

TexturePtr Texture::map(int layer)
{
    std::scoped_lock lock(g().mutex);
    if (layer < 0 || size_t(layer) >= mLayers.size())
        return nullptr;
    if (TexturePtr t = m->maps[size_t(layer)].lock())
        return t;
    auto t = std::make_shared<TextureImage>();
    const auto [w, h] = levelSize(0, 0);
    t->width = w;
    t->height = h;
    t->format = mLayers[size_t(layer)].format;
    t->srgb = mLayers[size_t(layer)].srgb;
    t->vt = shared_from_this();
    t->vtLayer = layer;
    m->maps[size_t(layer)] = t;
    return t;
}

void Texture::invalidate(int udim, const std::array<int, 4>& rect, bool pin)
{
    ++mContentVersion;
    const int tile = tileOf(udim);
    if (tile < 0 || rect[2] <= rect[0] || rect[3] <= rect[1])
        return;
    {
        std::scoped_lock lock(g().mutex);
        const uint64_t frame = g().frame;
        for (int l = 0; l < m->tiles[size_t(tile)].mips; ++l)
        {
            // The slot's border reaches kBorder texels of this level past the page.
            const int pad = kBorder + 1;
            const int x0 = std::max((rect[0] >> l) - pad, 0) / kPageSize, y0 = std::max((rect[1] >> l) - pad, 0) / kPageSize;
            const auto [px, py] = pages(tile, l);
            const int x1 = std::min(((rect[2] >> l) + pad) / kPageSize, px - 1), y1 = std::min(((rect[3] >> l) + pad) / kPageSize, py - 1);
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    const uint64_t k = keyOf({tile, l, x, y});
                    auto it = m->resident.find(k);
                    if (pin)
                        m->pinned.insert(k);
                    if (it == m->resident.end())
                        continue;
                    const bool recent = m->space->slots[it->second].lastUse + kKeepFrames >= frame;
                    if (pin || recent || m->permanent.contains(k) || m->pinned.contains(k))
                        m->stale.insert(k);
                    else
                        evictPage(*m, k);
                }
        }
    }
    ensureTask();
}

void Texture::invalidateAll()
{
    ++mContentVersion;
    {
        std::scoped_lock lock(g().mutex);
        const uint64_t frame = g().frame;
        std::vector<uint64_t> drop;
        for (auto& [k, slot] : m->resident)
        {
            if (m->space->slots[slot].lastUse + kKeepFrames >= frame || m->permanent.contains(k) || m->pinned.contains(k))
                m->stale.insert(k);
            else
                drop.push_back(k);
        }
        for (uint64_t k : drop)
            evictPage(*m, k);
    }
    ensureTask();
}

void Texture::unpin()
{
    std::scoped_lock lock(g().mutex);
    m->pinned.clear();
}

void Texture::request(std::span<const PageId> pages)
{
    {
        std::scoped_lock lock(g().mutex);
        for (const PageId& p : pages)
            if (m->valid(p))
                want(*m, p, g().frame);
    }
    ensureTask();
}

TextureStats Texture::stats() const
{
    std::scoped_lock lock(g().mutex);
    TextureStats st;
    st.resident = m->resident.size();
    st.pinned = m->pinned.size();
    st.stale = m->stale.size();
    st.queued = m->queue.size();
    st.produced = m->produced;
    st.requested = m->requested;
    st.lastRequests = m->lastRequests;
    st.bytes = m->resident.size() * m->space->slotBytes;
    return st;
}

// ---------------------------------------------------------------------------------------------------- system

bool supported(const VulkanContext& vk) { return vk.descriptorIndexing && vk.shaderInt64 && vk.fragmentStores; }

bool available()
{
    Renderer& r = Renderer::get();
    VulkanContext* vk = r.running() ? r.context() : nullptr;
    return vk && supported(*vk) && !compute::loadSpirv("vt.spv").empty();
}

size_t budget()
{
    Global& s = g();
    if (s.budgetOverride)
        return s.budgetOverride;
    const double mb = PrefStore::get().value<double>("general", "/texture/vtBudgetMB", 0.0);
    if (mb > 0.0)
        return size_t(mb * 1024.0 * 1024.0);
    size_t out = size_t(1) << 30;
    Renderer& r = Renderer::get();
    if (VulkanContext* vk = r.running() ? r.context() : nullptr; vk && vk->allocator)
    {
        const VkPhysicalDeviceMemoryProperties* props = nullptr;
        vmaGetMemoryProperties(vk->allocator, &props);
        std::vector<VmaBudget> budgets(props->memoryHeapCount);
        vmaGetHeapBudgets(vk->allocator, budgets.data());
        VkDeviceSize best = 0;
        for (uint32_t h = 0; h < props->memoryHeapCount; ++h)
            if (props->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                best = std::max(best, vk->memoryBudget ? budgets[h].budget : props->memoryHeaps[h].size);
        if (best)
            out = std::clamp<size_t>(size_t(best / 4), size_t(256) << 20, size_t(8) << 30);
    }
    return out;
}

size_t deviceBytesInUse()
{
    Renderer& r = Renderer::get();
    VulkanContext* vk = r.running() ? r.context() : nullptr;
    if (!vk || !vk->allocator)
        return 0;
    const VkPhysicalDeviceMemoryProperties* props = nullptr;
    vmaGetMemoryProperties(vk->allocator, &props);
    std::vector<VmaBudget> budgets(props->memoryHeapCount);
    vmaGetHeapBudgets(vk->allocator, budgets.data());
    size_t out = 0;
    for (uint32_t h = 0; h < props->memoryHeapCount; ++h)
        if (props->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            out += size_t(budgets[h].statistics.allocationBytes);
    return out;
}

void setBudget(size_t bytes)
{
    std::scoped_lock lock(g().mutex);
    g().budgetOverride = bytes;
}

void setRedraw(std::function<void()> fn)
{
    std::scoped_lock lock(g().mutex);
    g().redraw = std::move(fn);
}

bool idle()
{
    Global& s = g();
    std::scoped_lock lock(s.mutex);
    if (s.working || s.taskQueued)
        return false;
    for (auto& [id, t] : s.textures)
        if (!t->queue.empty() || !t->stale.empty())
            return false;
    return true;
}

bool waitIdle(double timeoutMs)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double, std::milli>(timeoutMs);
    for (;;)
    {
        mainq::drain();
        ensureTask();
        if (idle())
        {
            mainq::drain();
            return true;
        }
        if (std::chrono::steady_clock::now() > end)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

SystemStats stats()
{
    Global& s = g();
    std::scoped_lock lock(s.mutex);
    SystemStats st;
    st.budgetBytes = budget();
    st.allocatedBytes = s.allocated;
    st.spaces = s.spaces.size();
    for (auto& sp : s.spaces)
        st.chunks += sp->chunks.size();
    st.textures = s.textures.size();
    for (auto& [id, t] : s.textures)
    {
        st.residentPages += t->resident.size();
        st.residentBytes += t->resident.size() * t->space->slotBytes;
        st.queued += t->queue.size() + t->stale.size();
    }
    st.produced = s.produced;
    st.evicted = s.evicted;
    st.feedbackFrames = s.feedbackFrames;
    return st;
}

nlohmann::json statsJson()
{
    const SystemStats s = stats();
    nlohmann::json textures = nlohmann::json::array();
    {
        std::scoped_lock lock(g().mutex);
        for (auto& [id, t] : g().textures)
            textures.push_back({{"name", t->self->name()},
                                {"id", id},
                                {"tiles", t->tiles.size()},
                                {"resident", t->resident.size()},
                                {"queued", t->queue.size()},
                                {"stale", t->stale.size()},
                                {"pinned", t->pinned.size()},
                                {"produced", t->produced}});
    }
    return {{"budgetBytes", s.budgetBytes}, {"allocatedBytes", s.allocatedBytes}, {"residentBytes", s.residentBytes},
            {"residentPages", s.residentPages}, {"chunks", s.chunks},           {"spaces", s.spaces},
            {"produced", s.produced},       {"evicted", s.evicted},             {"feedbackFrames", s.feedbackFrames},
            {"queued", s.queued},           {"textures", textures}};
}

Texture* textureOf(const TextureImage& t) { return t.vt.get(); }

std::vector<TexturePtr> chunkTextures(const TextureImage& t)
{
    std::vector<TexturePtr> out;
    if (!t.vt)
        return out;
    std::scoped_lock lock(g().mutex);
    const Space& sp = *t.vt->impl().space;
    for (const Space::Chunk& c : sp.chunks)
        if (size_t(t.vtLayer) < c.layers.size())
            out.push_back(c.layers[size_t(t.vtLayer)]->asTexture());
    return out;
}

void tableEntry(const TextureImage& t, std::vector<uint32_t>& table, const std::function<uint32_t(const TexturePtr&)>& bindlessSlot)
{
    Texture::Impl& m = t.vt->impl();
    std::vector<TexturePtr> chunks = chunkTextures(t);
    const VkDeviceAddress address = m.table.address;
    table.push_back(kVtEntry | (t.vt->repeat() ? kVtRepeat : 0u) | uint32_t(chunks.size()));
    table.push_back(uint32_t(address & 0xffffffffu));
    table.push_back(uint32_t(address >> 32));
    table.push_back(t.vt->id());
    for (const TexturePtr& c : chunks)
        table.push_back(bindlessSlot(c));
}

void recordFrame(VulkanContext& vk, VkCommandBuffer cmd, std::vector<compute::Buffer>& staging)
{
    LILA_ZONE();
    Global& s = g();
    std::scoped_lock lock(s.mutex);
    ++s.renderFrames;
    for (auto it = s.retired.begin(); it != s.retired.end();)
        if (it->frame + 4 < s.renderFrames)
        {
            compute::destroy(vk, it->buffer);
            it = s.retired.erase(it);
        }
        else
            ++it;
    bool fading = false;
    bool any = false;
    for (auto& [id, t] : s.textures)
    {
        for (auto it = t->fading.begin(); it != t->fading.end();)
        {
            auto r = t->resident.find(*it);
            if (r == t->resident.end())
            {
                it = t->fading.erase(it);
                continue;
            }
            const uint32_t e = t->shadow[t->entryIndex(pageOf(*it))];
            const uint32_t fade = std::min<uint32_t>(((e >> 16) & 0xffu) + kFadeStep, 255u);
            t->setEntry(pageOf(*it), kResident | (fade << 16) | r->second);
            if (fade >= 255)
                it = t->fading.erase(it);
            else
            {
                fading = true;
                ++it;
            }
        }
        const VkDeviceSize bytes = t->shadow.size() * sizeof(uint32_t);
        if (!t->table.buffer)
        {
            t->table = compute::makeBuffer(vk, bytes, 0, compute::Host::None);
            t->allDirty = true;
        }
        if (t->allDirty)
        {
            compute::Buffer up = compute::hostBuffer(vk, t->shadow.data(), bytes);
            staging.push_back(up);
            compute::copy(cmd, up, t->table, bytes);
            t->allDirty = false;
            t->dirty.clear();
            any = true;
            continue;
        }
        // Runs of dirty words, each written inline.
        auto it = t->dirty.begin();
        while (it != t->dirty.end())
        {
            const uint32_t first = *it;
            uint32_t last = first;
            ++it;
            while (it != t->dirty.end() && *it == last + 1 && (last + 1 - first) < 16384)
            {
                last = *it;
                ++it;
            }
            vkCmdUpdateBuffer(cmd, t->table.buffer, VkDeviceSize(first) * 4, VkDeviceSize(last - first + 1) * 4, &t->shadow[first]);
            any = true;
        }
        t->dirty.clear();
    }
    if (any)
        compute::memoryBarrier(cmd);
    if (fading)
        requestRedraw();
}

// ---------------------------------------------------------------------------------------------------- feedback

bool Feedback::begin(VulkanContext& vk, VkCommandBuffer cmd, int width, int height, uint64_t frame)
{
    mActive = false;
    if (!supported(vk))
        return false;
    const int cx = std::max(1, (width + kFeedbackCell - 1) / kFeedbackCell), cy = std::max(1, (height + kFeedbackCell - 1) / kFeedbackCell);
    try
    {
        if (cx * cy > mCellsX * mCellsY || !mCells.buffer)
        {
            compute::destroy(vk, mCells);
            mCells = compute::makeBuffer(vk, VkDeviceSize(cx) * VkDeviceSize(cy) * 8, 0, compute::Host::None);
        }
        if (!mTable.buffer)
            mTable = compute::makeBuffer(vk, VkDeviceSize(kHashSize) * 4, 0, compute::Host::None);
        if (!mList.buffer)
        {
            mList = compute::makeBuffer(vk, 16 + VkDeviceSize(kListCap) * 8, 0, compute::Host::None);
            mHost = compute::makeBuffer(vk, 16 + VkDeviceSize(kListCap) * 8, 0, compute::Host::Read);
        }
    }
    catch (const std::exception& e)
    {
        log::warn("virtual texture feedback: {}", e.what());
        return false;
    }
    mCellsX = cx;
    mCellsY = cy;
    // A 4x4 ordered dither of which pixel of each cell writes, so sixteen frames cover them all.
    static constexpr uint32_t kOrder[16] = {0, 10, 2, 8, 5, 15, 7, 13, 1, 11, 3, 9, 4, 14, 6, 12};
    const uint32_t j = kOrder[frame % 16];
    mJitter = (j % 4) | ((j / 4) << 8);
    vkCmdFillBuffer(cmd, mCells.buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(cmd, mTable.buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(cmd, mList.buffer, 0, 16, 0);
    compute::memoryBarrier(cmd);
    mActive = true;
    return true;
}

std::array<uint32_t, kTableHeaderWords> Feedback::header() const
{
    if (!mActive)
        return {0, 0, 0, 0};
    return {uint32_t(mCells.address & 0xffffffffu), uint32_t(mCells.address >> 32), uint32_t(mCellsX), mJitter};
}

void Feedback::end(VulkanContext& vk, VkCommandBuffer cmd)
{
    if (!mActive)
        return;
    Global& s = g();
    if (!s.dedupeLayout)
    {
        std::string err;
        s.dedupeCode = compute::loadSpirv("vt.spv", &err);
        if (s.dedupeCode.empty())
        {
            log::warn("virtual texture: {}", err);
            mActive = false;
            return;
        }
        s.dedupeHash = compute::spirvHash(s.dedupeCode);
        s.dedupeLayout = compute::pipelineLayout(vk, sizeof(DedupePush), kCompute);
    }
    compute::memoryBarrier(cmd);
    const uint32_t count = uint32_t(mCellsX * mCellsY);
    const uint32_t groups = (count + 63) / 64;
    const uint32_t gx = std::min<uint32_t>(groups, 65535);
    DedupePush push{mCells.address, mTable.address, mList.address, count, gx, kHashSize - 1, kListCap};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      compute::sharedPipelines().get(vk, s.dedupeLayout, compute::layoutKey(sizeof(DedupePush), kCompute), s.dedupeCode, s.dedupeHash,
                                                     "vtDedupeCS"));
    vkCmdPushConstants(cmd, s.dedupeLayout, kCompute, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, gx, (groups + gx - 1) / gx, 1);
    compute::memoryBarrier(cmd);
    compute::copy(cmd, mList, mHost, mList.size);
    compute::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    mPending = true;
}

void Feedback::collect()
{
    if (!mPending || !mHost.buffer)
        return;
    mPending = false;
    Renderer& r = Renderer::get();
    VulkanContext* vk = r.context();
    if (!vk)
        return;
    vmaInvalidateAllocation(vk->allocator, mHost.alloc, 0, VK_WHOLE_SIZE);
    const auto* p = static_cast<const uint32_t*>(mHost.mapped);
    const uint32_t n = std::min(p[0], kListCap);
    processRequests(std::span<const uint32_t>(p + 4, size_t(n) * 2));
}

void Feedback::destroy(VulkanContext& vk)
{
    for (compute::Buffer* b : {&mCells, &mTable, &mList, &mHost})
        compute::destroy(vk, *b);
    mCellsX = mCellsY = 0;
    mActive = mPending = false;
}

void releaseAll(VulkanContext& vk)
{
    Global& s = g();
    std::scoped_lock lock(s.mutex);
    for (Retired& r : s.retired)
        compute::destroy(vk, r.buffer);
    s.retired.clear();
    for (auto& [id, t] : s.textures)
    {
        compute::destroy(vk, t->table);
        t->table = {};
        // The chunk images go with the device: every page has to be made again.
        for (auto& [k, slot] : t->resident)
            t->queue[k] = s.frame;
        t->resident.clear();
        t->stale.clear();
        t->fading.clear();
        std::fill(t->shadow.begin() + kGridWords, t->shadow.end(), 0u);
        t->allDirty = true;
    }
    for (auto& sp : s.spaces)
    {
        sp->chunks.clear();
        sp->slots.clear();
        sp->free.clear();
    }
    s.allocated = 0;
    if (s.dedupeLayout)
        compute::destroy(vk, s.dedupeLayout);
    s.dedupeLayout = VK_NULL_HANDLE;
    if (s.subReady)
        s.sub.destroy(vk);
    s.subReady = false;
    s.taskQueued = false;
    s.working = false;
}
} // namespace lila::render::vt
