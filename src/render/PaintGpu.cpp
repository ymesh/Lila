#include "render/PaintGpu.h"

#include "brush/Alphas.h"
#include "brush/Brush.h"
#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "ops/Undo.h"
#include "render/Compute.h"
#include "render/Renderer.h"
#include "scene/MeshNodes.h"
#include "scene/Node.h"
#include "tex/Evaluate.h"
#include "tex/Library.h"
#include "tex/NodeDef.h"
#include "tex/PaintSymmetry.h"
#include "tex/Plan.h"
#include "tex/TextureSet.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

namespace lila::render::paint
{
namespace
{
using namespace tex::paint;
using scene::NodeId;

constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr VkDeviceSize kTexels = VkDeviceSize(kPageSize) * kPageSize;
/** A channel page: RGBA16F texels. */
constexpr VkDeviceSize kHalfPage = kTexels * 8;
/** A geometry page (GeoTexel) or a stroke's build-up page (float4). */
constexpr VkDeviceSize kWidePage = kTexels * 16;
constexpr uint32_t kMaxChannels = 8;
constexpr uint32_t kItemsPerDispatch = 4096;
constexpr uint32_t kDabsPerBatch = 512;
constexpr int kMaxDepthSize = 1024;
constexpr VkShaderStageFlags kCompute = VK_SHADER_STAGE_COMPUTE_BIT;

// ------------------------------------------------------------------------------------------- GPU-side structures

struct GpuTri
{
    glm::vec4 p[3];
    glm::vec4 n[3];
    glm::vec4 t[3];
};
static_assert(sizeof(GpuTri) == 144);

struct GpuDab
{
    glm::vec4 centerRadius;
    glm::vec4 normalAmount;
    glm::vec4 tangentRoundness;
    glm::vec4 direction;
};
static_assert(sizeof(GpuDab) == 64);

struct GpuItem
{
    VkDeviceAddress geo = 0;
    VkDeviceAddress acc = 0;
    uint32_t dabFirst = 0;
    uint32_t dabCount = 0;
    uint32_t page = 0;
    uint32_t tile = 0;
    VkDeviceAddress dst[kMaxChannels]{};
    VkDeviceAddress src[kMaxChannels]{};
};
static_assert(sizeof(GpuItem) == 160);

/** A UDIM tile as the kernels see it: size and per channel a table of its pages before the stroke. */
struct GpuTile
{
    uint32_t width = 0, height = 0, pagesX = 0, pagesY = 0;
    VkDeviceAddress tables[kMaxChannels]{};
};
static_assert(sizeof(GpuTile) == 80);

struct GpuParams
{
    glm::mat4 objectToClip{1.0f};
    glm::mat4 clipToObject{1.0f};
    glm::vec4 eye{0.0f};
    glm::vec4 viewDir{0.0f};
    glm::vec4 values[kMaxChannels]{};
    VkDeviceAddress items = 0, dabIndex = 0, dabs = 0, tris = 0, falloff = 0, alpha = 0, depth = 0, tiles = 0, triMask = 0, image = 0, stamp = 0;
    VkDeviceAddress valueImages[kMaxChannels]{};
    uint32_t depthW = 1, depthH = 1, alphaW = 0, alphaH = 0;
    uint32_t channels = 0, blend = 0, flags = 0, directionChannel = kNone;
    uint32_t maskChannel = kNone, itemCount = 0, mode = 0, sub = 0;
    uint32_t normalMask = 0, imageW = 0, imageH = 0, stampW = 0;
    uint32_t stampH = 0, valueW = 0, valueH = 0, colorChannel = kNone;
    float strength = 1.0f, tilt = 0.5f, depthBias = 0.0f, filterRadius = 4.0f;
    float amount = 1.0f, sourceU = 0.0f, sourceV = 0.0f, facingFade = 0.0f;
    float intensity = 1.0f, valueScale = 1.0f;
    uint32_t pad[4]{};
};
static_assert(offsetof(GpuParams, items) == 288);
static_assert(offsetof(GpuParams, valueImages) == 376);
static_assert(offsetof(GpuParams, depthW) == 440);
static_assert(offsetof(GpuParams, strength) == 520);
static_assert(sizeof(GpuParams) == 576);

struct DabPush
{
    VkDeviceAddress params;
    uint32_t firstItem;
    uint32_t pad;
};

struct DepthPush
{
    glm::mat4 objectToClip;
    VkDeviceAddress tris;
    VkDeviceAddress depth;
    uint32_t triCount, width, height, groupsX;
};
static_assert(sizeof(DepthPush) == 96);

constexpr uint32_t kFlagOcclusion = 1, kFlagDirection = 2, kFlagView = 4, kFlagValueImages = 8, kFlagFlow = 16;
enum Mode : uint32_t
{
    kModePaint,
    kModeFilter,
    kModeFill,
    kModeProject,
    kModeNormal,
};

struct VisPush
{
    glm::mat4 objectToClip;
    VkDeviceAddress tris, depth, vis;
    uint32_t triCount, width, height, groupsX, pad0;
};

struct UvPush
{
    glm::mat4 objectToClip;
    VkDeviceAddress tris, vis, out;
    uint32_t width, height, groupsX, pad0;
};

struct GatherPush
{
    VkDeviceAddress table, dst;
    int32_t x0, y0;
    uint32_t width, height, tileW, tileH, levelW, levelH, pagesX, dstStride, groupsX, pad;
};
static_assert(sizeof(GatherPush) == 64);

struct ViewPush
{
    VkDeviceAddress src, dst;
    glm::vec4 rect;
    uint32_t srcWidth, srcHeight, format, srgb, width, height, groupsX, display;
};

// ---------------------------------------------------------------------------------------------------- page pools

/** Equal-sized pages carved from chunk buffers, reused through a free list. Render thread. */
class Pool
{
public:
    Pool(VkDeviceSize pageBytes, uint32_t perChunk) : mPage(pageBytes), mPerChunk(perChunk) {}

    uint32_t alloc(VulkanContext& vk)
    {
        if (mFree.empty())
        {
            compute::Buffer b = compute::makeBuffer(vk, mPage * mPerChunk, 0, compute::Host::None);
            const uint32_t base = uint32_t(mChunks.size()) * mPerChunk;
            mChunks.push_back(b);
            for (uint32_t i = mPerChunk; i-- > 0;)
                mFree.push_back(base + i);
        }
        const uint32_t s = mFree.back();
        mFree.pop_back();
        ++mUsed;
        return s;
    }
    void release(uint32_t slot)
    {
        if (slot == kNone)
            return;
        mFree.push_back(slot);
        --mUsed;
    }
    VkBuffer buffer(uint32_t slot) const { return mChunks[slot / mPerChunk].buffer; }
    VkDeviceSize offset(uint32_t slot) const { return VkDeviceSize(slot % mPerChunk) * mPage; }
    VkDeviceAddress address(uint32_t slot) const { return mChunks[slot / mPerChunk].address + offset(slot); }
    VkDeviceSize pageBytes() const { return mPage; }
    size_t used() const { return mUsed; }
    void destroy(VulkanContext& vk)
    {
        for (compute::Buffer& b : mChunks)
            compute::destroy(vk, b);
        mChunks.clear();
        mFree.clear();
        mUsed = 0;
    }

private:
    VkDeviceSize mPage;
    uint32_t mPerChunk;
    std::vector<compute::Buffer> mChunks;
    std::vector<uint32_t> mFree;
    size_t mUsed = 0;
};

/** A channel tile's page table on the device for gathers, rebuilt whenever its pages change. Render thread. */
struct ViewEntry
{
    compute::Buffer table;
    int width = 0, height = 0;
};
std::map<std::tuple<NodeId, std::string, std::string, int>, ViewEntry> gViews; // set, buffer, channel, udim
/** Tile sizes of every buffer, for views of channels nothing painted yet. Render thread. */
std::map<std::tuple<NodeId, std::string, int>, std::pair<int, int>> gViewSizes;

struct Kernel
{
    std::vector<uint32_t> code;
    uint64_t hash = 0;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    uint32_t push = 0;
};

/** Mesh triangles on the device, for painting and depth. */
struct TriBuffer
{
    std::shared_ptr<const PaintMesh> mesh;
    compute::Buffer buffer;
};

/** Render-thread state shared by every buffer. */
struct Gpu
{
    Pool half{kHalfPage, 32};
    Pool wide{kWidePage, 16};
    uint32_t zeroPage = kNone;
    compute::Submitter sub;
    bool ready = false;
    Kernel dab, depth, vis, uvmap, view, gather;
    std::map<const PaintMesh*, TriBuffer> tris;

    void init(VulkanContext& vk)
    {
        if (ready)
            return;
        sub.init(vk);
        loadKernel(vk, dab, "paint_dab.spv", sizeof(DabPush));
        loadKernel(vk, depth, "paint_depth.spv", sizeof(DepthPush));
        loadKernel(vk, vis, "paint_screen.spv", sizeof(VisPush));
        loadKernel(vk, uvmap, "paint_screen.spv", sizeof(UvPush));
        loadKernel(vk, view, "paint_screen.spv", sizeof(ViewPush));
        loadKernel(vk, gather, "paint_gather.spv", sizeof(GatherPush));
        zeroPage = half.alloc(vk);
        sub.run(vk, [&](VkCommandBuffer cmd) { vkCmdFillBuffer(cmd, half.buffer(zeroPage), half.offset(zeroPage), kHalfPage, 0); });
        ready = true;
    }

    static void loadKernel(VulkanContext& vk, Kernel& k, const char* file, uint32_t push)
    {
        if (k.layout)
            return;
        std::string err;
        k.code = compute::loadSpirv(file, &err);
        if (k.code.empty())
            throw std::runtime_error("texture painting: " + err);
        k.hash = compute::spirvHash(k.code);
        k.push = push;
        k.layout = compute::pipelineLayout(vk, push, kCompute);
    }

    VkPipeline pipeline(VulkanContext& vk, const Kernel& k, const char* entry)
    {
        return compute::sharedPipelines().get(vk, k.layout, compute::layoutKey(k.push, kCompute), k.code, k.hash, entry);
    }

    const TriBuffer& triangles(VulkanContext& vk, const std::shared_ptr<const PaintMesh>& mesh)
    {
        if (auto it = tris.find(mesh.get()); it != tris.end())
            return it->second;
        std::vector<GpuTri> t(std::max<size_t>(mesh->triangleCount(), 1));
        for (size_t i = 0; i < mesh->triangleCount(); ++i)
            for (size_t k = 0; k < 3; ++k)
            {
                // Corner uv rides in the w components.
                t[i].p[k] = glm::vec4(mesh->positions[i * 3 + k], mesh->uvs[i * 3 + k].x);
                t[i].n[k] = glm::vec4(mesh->normals[i * 3 + k], mesh->uvs[i * 3 + k].y);
                t[i].t[k] = mesh->tangents[i * 3 + k];
            }
        TriBuffer tb{mesh, compute::makeBuffer(vk, t.size() * sizeof(GpuTri), 0, compute::Host::None)};
        compute::Buffer staging = compute::hostBuffer(vk, t.data(), t.size() * sizeof(GpuTri));
        sub.run(vk, [&](VkCommandBuffer cmd) { compute::copy(cmd, staging, tb.buffer, t.size() * sizeof(GpuTri)); });
        compute::destroy(vk, staging);
        return tris.emplace(mesh.get(), std::move(tb)).first->second;
    }

    void release(VulkanContext& vk)
    {
        if (!ready)
            return;
        for (auto& [k, t] : tris)
            compute::destroy(vk, t.buffer);
        tris.clear();
        for (auto& [k, v] : gViews)
            compute::destroy(vk, v.table);
        gViews.clear();
        gViewSizes.clear();
        half.destroy(vk);
        wide.destroy(vk);
        zeroPage = kNone;
        sub.destroy(vk);
        ready = false;
    }
};

// Leaked on purpose: undo steps holding pages may outlive static destruction order.
Gpu& gpu()
{
    static Gpu* g = new Gpu;
    return *g;
}

/** Bumped when the renderer stops: every page from before is gone. */
std::atomic<uint64_t> gEpoch{1};

void installStopHook()
{
    static std::once_flag once;
    std::call_once(once, [] {
        Renderer::get().addStopHook([](VulkanContext& vk) {
            gpu().release(vk);
            ++gEpoch;
        });
    });
}

/** Runs f on the render thread and waits for it. Main thread. */
template <class F> void gpuRun(F&& f)
{
    Renderer& r = Renderer::get();
    if (!r.running() && !r.start())
        throw std::runtime_error("texture painting needs a GPU, and the renderer could not start");
    installStopHook();
    r.runTask([&](VulkanContext& vk) {
         Gpu& g = gpu();
         g.init(vk);
         f(vk, g);
         return true;
     }).get();
}

// ---------------------------------------------------------------------------------------------------- buffers

struct Key
{
    NodeId set = scene::kNoNode;
    std::string buffer;
    auto operator<=>(const Key&) const = default;
};

using TileKey = std::pair<std::string, int>; // channel, udim

struct ChannelTile
{
    /** Page index -> half pool slot. */
    std::map<uint32_t, uint32_t> pages;
    /** Pages whose image texels are stale. */
    std::set<uint32_t> dirty;
};

struct Surface
{
    std::shared_ptr<const PaintMesh> mesh;
    std::unique_ptr<SurfaceAtlas> atlas;
    /** Geometry page -> wide pool slot (render thread). */
    std::map<uint32_t, uint32_t> geo;
};

struct CowPage
{
    uint32_t before = kNone;
    uint32_t after = kNone;
};

struct ValueImages;

/** A stroke being painted: its pages' originals and build-up, and what its dabs need on the GPU. */
struct Live
{
    StrokePtr header;
    std::shared_ptr<const PaintMesh> mesh;
    std::vector<std::string> channels;
    std::vector<glm::vec3> values;
    std::vector<float> falloff;
    std::shared_ptr<const brush::AlphaImage> alpha;
    float roundness = 1.0f;
    float strength = 1.0f;
    uint32_t mode = 0, sub = 0;
    /** fill: a bit per triangle. */
    std::vector<uint32_t> triMask;
    /** project: the picture (RGBA floats) and the NDC box (x0, y0, x1, y1) of its covered pixels. */
    std::vector<float> image;
    int imageW = 0, imageH = 0;
    glm::vec4 imageBox{0.0f};
    /** normal stamps: the normal map. */
    std::vector<float> stamp;
    int stampW = 0, stampH = 0;
    /** Channel values from a texture graph, and its tiles per uv unit. */
    std::shared_ptr<const ValueImages> sourceImages;
    float valueScale = 1.0f;
    /** The above on the device, made on the first batch. */
    bool uploaded = false;
    std::vector<compute::Buffer> uploads;
    VkDeviceAddress falloffAddr = 0, alphaAddr = 0, triMaskAddr = 0, imageAddr = 0, stampAddr = 0;
    VkDeviceAddress valueAddr[kMaxChannels]{};
    /** Dabs given so far, and the last centre per symmetry copy (direction strokes). */
    size_t dabs = 0;
    std::map<uint32_t, glm::vec3> last;
    std::map<std::pair<TileKey, uint32_t>, CowPage> cow;
    /** Build-up page per (udim, page). */
    std::map<std::pair<int, uint32_t>, uint32_t> acc;
    compute::Buffer depth;
    int depthW = 0, depthH = 0;
    bool depthReady = false;
    /** The symmetry the stroke was painted with, and the views a projection projects through (one per copy). */
    PaintSymmetry sym;
    std::vector<StrokeView> views;
    size_t viewIndex = 0;

    const StrokeView* view() const
    {
        if (viewIndex < views.size())
            return &views[viewIndex];
        return header->view ? &*header->view : nullptr;
    }
};

struct Layer
{
    uint64_t generation = 0;
    uint64_t epoch = 0;
    /** Tile sizes by UDIM, as rasterised. */
    std::map<int, std::pair<int, int>> sizes;
    std::map<TileKey, ChannelTile> tiles;
    std::vector<uint64_t> applied;
    std::shared_ptr<const Baseline> baseline;
    uint64_t clears = 0;
    bool synced = false;
    std::unique_ptr<Live> live;
    /** Texels {x0, y0, x1, y1} per UDIM tile whose pages changed since takeChanges last looked. */
    std::map<int, std::array<int, 4>> changed;
};

std::atomic<uint64_t> gGeneration{1};
std::map<Key, std::shared_ptr<Layer>> gLayers;
/** Surfaces by mesh, UDIM and size. */
std::map<std::tuple<const PaintMesh*, int, int, int>, std::shared_ptr<Surface>> gSurfaces;

struct MeshEntry
{
    scene::MeshPtr mesh;
    std::shared_ptr<const geo::MeshBvh> bvh;
    std::map<std::string, std::shared_ptr<const PaintMesh>> paint;
};
std::map<std::string, MeshEntry> gMeshes;

bool meshInUse(const PaintMesh* m)
{
    for (auto& [path, e] : gMeshes)
        for (auto& [mat, p] : e.paint)
            if (p.get() == m)
                return true;
    for (auto& [k, l] : gLayers)
        if (l->live && l->live->mesh.get() == m)
            return true;
    return false;
}

// Images the texture graph reads, shared with other threads.
struct ImageEntry
{
    GpuImageRef image;
    bool full = true;
};
std::mutex gImageMutex;
std::map<std::tuple<NodeId, std::string, std::string, int>, ImageEntry> gImages; // set, buffer, channel, udim
std::map<std::tuple<NodeId, std::string, int>, std::pair<int, int>> gImageSizes;  // set, buffer, udim

std::map<int, std::function<void(NodeId)>> gListeners;
int gNextListener = 1;
bool gCommandsRegistered = false;

void notify(NodeId set)
{
    const auto listeners = gListeners;
    for (const auto& [id, fn] : listeners)
        fn(set);
}

Layer& layerFor(const Key& k)
{
    std::shared_ptr<Layer>& l = gLayers[k];
    if (!l)
        l = std::make_shared<Layer>();
    if (l->epoch != gEpoch.load())
    {
        // The renderer restarted: the pages are gone, so this is a fresh buffer.
        const uint64_t epoch = gEpoch.load();
        l = std::make_shared<Layer>();
        l->epoch = epoch;
        l->generation = gGeneration++;
    }
    return *l;
}

Layer* findLayer(const Key& k)
{
    auto it = gLayers.find(k);
    if (it == gLayers.end() || it->second->epoch != gEpoch.load())
        return nullptr;
    return it->second.get();
}

std::shared_ptr<Surface> surface(const std::shared_ptr<const PaintMesh>& mesh, int udim, int w, int h)
{
    const auto key = std::make_tuple(mesh.get(), udim, w, h);
    if (auto it = gSurfaces.find(key); it != gSurfaces.end())
        return it->second;
    auto s = std::make_shared<Surface>();
    s->mesh = mesh;
    s->atlas = std::make_unique<SurfaceAtlas>(mesh, udim, w, h);
    gSurfaces[key] = s;
    return s;
}

bool meshInUse(const PaintMesh* m);

/** Frees the geometry pages and triangles of meshes nothing paints any more (render thread, the main thread waiting). */
void pruneSurfaces(VulkanContext& vk, Gpu& g)
{
    for (auto it = gSurfaces.begin(); it != gSurfaces.end();)
        if (!meshInUse(it->second->mesh.get()))
        {
            for (auto& [p, slot] : it->second->geo)
                g.wide.release(slot);
            it = gSurfaces.erase(it);
        }
        else
            ++it;
    for (auto it = g.tris.begin(); it != g.tris.end();)
        if (!meshInUse(it->first))
        {
            compute::destroy(vk, it->second.buffer);
            it = g.tris.erase(it);
        }
        else
            ++it;
}

uint32_t channelSlot(Layer& l, const TileKey& k, uint32_t page)
{
    auto it = l.tiles.find(k);
    if (it == l.tiles.end())
        return kNone;
    auto p = it->second.pages.find(page);
    return p == it->second.pages.end() ? kNone : p->second;
}

/** Notes that texels [x0, x1) x [y0, y1) of a tile changed. */
void noteChanged(Layer& l, int udim, int x0, int y0, int x1, int y1)
{
    auto [it, fresh] = l.changed.try_emplace(udim, std::array<int, 4>{x0, y0, x1, y1});
    if (!fresh)
        it->second = {std::min(it->second[0], x0), std::min(it->second[1], y0), std::max(it->second[2], x1), std::max(it->second[3], y1)};
}

void setSlot(Layer& l, const TileKey& k, uint32_t page, uint32_t slot)
{
    ChannelTile& t = l.tiles[k];
    if (slot == kNone)
        t.pages.erase(page);
    else
        t.pages[page] = slot;
    t.dirty.insert(page);
    if (auto size = l.sizes.find(k.second); size != l.sizes.end())
    {
        const int pagesX = (size->second.first + kPageSize - 1) / kPageSize;
        const int x0 = int(page % uint32_t(pagesX)) * kPageSize, y0 = int(page / uint32_t(pagesX)) * kPageSize;
        noteChanged(l, k.second, x0, y0, x0 + kPageSize, y0 + kPageSize);
    }
}

// ---------------------------------------------------------------------------------------------------- page views

void dropViews(VulkanContext& vk, NodeId set, const std::string& buffer)
{
    for (auto it = gViews.begin(); it != gViews.end();)
        if (std::get<0>(it->first) == set && std::get<1>(it->first) == buffer)
        {
            compute::destroy(vk, it->second.table);
            it = gViews.erase(it);
        }
        else
            ++it;
    std::erase_if(gViewSizes, [&](const auto& kv) { return std::get<0>(kv.first) == set && std::get<1>(kv.first) == buffer; });
}

/** Rebuilds the views of a layer's tiles whose pages changed (render thread, the main thread waiting). */
void updateViews(VulkanContext& vk, Gpu& g, const Key& key, Layer& l)
{
    for (auto& [udim, size] : l.sizes)
        gViewSizes[{key.set, key.buffer, udim}] = size;
    for (auto& [tk, tile] : l.tiles)
    {
        if (tile.dirty.empty())
            continue;
        auto size = l.sizes.find(tk.second);
        if (size == l.sizes.end())
            continue;
        const auto [w, h] = size->second;
        const size_t pagesX = size_t((w + kPageSize - 1) / kPageSize), pagesY = size_t((h + kPageSize - 1) / kPageSize);
        ViewEntry& v = gViews[{key.set, key.buffer, tk.first, tk.second}];
        const VkDeviceSize bytes = std::max<VkDeviceSize>(pagesX * pagesY * sizeof(VkDeviceAddress), 16);
        if (v.table.size != bytes)
        {
            compute::destroy(vk, v.table);
            v.table = compute::makeBuffer(vk, bytes, 0, compute::Host::Write);
        }
        v.width = w;
        v.height = h;
        auto* table = static_cast<VkDeviceAddress*>(v.table.mapped);
        std::fill(table, table + pagesX * pagesY, VkDeviceAddress(0));
        for (auto& [page, slot] : tile.pages)
            if (page < pagesX * pagesY)
                table[page] = g.half.address(slot);
        vmaFlushAllocation(vk.allocator, v.table.alloc, 0, VK_WHOLE_SIZE);
    }
}

/** Copies stale pages into the images the graph reads (render thread, inside a recording). */
void recordResolve(VulkanContext& vk, VkCommandBuffer cmd, Gpu& g, const Key& key, Layer& l, std::vector<compute::Buffer>& temps)
{
    updateViews(vk, g, key, l);
    for (auto& [tk, tile] : l.tiles)
    {
        GpuImageRef img;
        bool full = false;
        {
            std::scoped_lock lock(gImageMutex);
            auto it = gImages.find({key.set, key.buffer, tk.first, tk.second});
            if (it != gImages.end())
            {
                img = it->second.image;
                full = it->second.full;
                it->second.full = false;
            }
        }
        if (!img)
        {
            tile.dirty.clear();
            continue;
        }
        std::vector<uint32_t> pages;
        if (full)
            for (auto& [p, s] : tile.pages)
                pages.push_back(p);
        else
            pages.assign(tile.dirty.begin(), tile.dirty.end());
        tile.dirty.clear();
        if (pages.empty() && !full)
            continue;
        if (!img->prepare(vk, cmd, temps))
            continue;
        img->beginWrite(cmd);
        compute::memoryBarrier(cmd);
        const int pagesX = (img->width() + kPageSize - 1) / kPageSize;
        for (uint32_t p : pages)
        {
            const int x0 = int(p % uint32_t(pagesX)) * kPageSize, y0 = int(p / uint32_t(pagesX)) * kPageSize;
            if (x0 >= img->width() || y0 >= img->height())
                continue;
            auto s = tile.pages.find(p);
            const uint32_t slot = s == tile.pages.end() ? g.zeroPage : s->second;
            VkBufferImageCopy region{};
            region.bufferOffset = g.half.offset(slot);
            region.bufferRowLength = uint32_t(kPageSize);
            region.bufferImageHeight = uint32_t(kPageSize);
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageOffset = {x0, y0, 0};
            region.imageExtent = {uint32_t(std::min(kPageSize, img->width() - x0)), uint32_t(std::min(kPageSize, img->height() - y0)), 1};
            img->noteWriteRect(x0, y0, x0 + std::min(kPageSize, img->width() - x0), y0 + std::min(kPageSize, img->height() - y0));
            vkCmdCopyBufferToImage(cmd, g.half.buffer(slot), img->image(), VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        }
        compute::memoryBarrier(cmd);
        img->endWrite(cmd);
    }
}

void resolve(VulkanContext& vk, Gpu& g, const Key& key, Layer& l)
{
    std::vector<compute::Buffer> temps;
    g.sub.run(vk, [&](VkCommandBuffer cmd) { recordResolve(vk, cmd, g, key, l, temps); });
    for (compute::Buffer& b : temps)
        compute::destroy(vk, b);
}

// ---------------------------------------------------------------------------------------------------- strokes

std::pair<int, int> tileSize(const tex::SetSettings& s, const tex::Tile& t, const Buffer* data)
{
    if (data && data->width > 0 && data->height > 0)
        return {data->width, data->height};
    return s.tileSize(t);
}

// ---------------------------------------------------------------------------------------------------- graph payloads

/** Channel values a texture graph gives over one tile of uv (the material brush), RGBA floats rows top first. */
struct ValueImages
{
    int width = 0, height = 0;
    std::map<std::string, std::vector<float>> channels;
};

std::mutex gGraphCacheMutex;
std::map<std::string, std::shared_ptr<const ValueImages>> gValueCache;
std::map<std::string, std::shared_ptr<const brush::AlphaImage>> gTipCache;

/** The graph a source names and what to request of it for the channels. Throws std::runtime_error. */
tex::Graph sourceGraph(const json& src, const std::vector<std::pair<std::string, tex::PortType>>& channels, std::vector<tex::OutputRequest>& requests)
{
    if (src.contains("library"))
    {
        const std::string id = src["library"].get<std::string>();
        std::optional<tex::library::Entry> e = tex::library::find(id);
        if (!e)
            throw std::runtime_error("no library material " + id);
        tex::Graph g = tex::library::graphOf(tex::library::document(*e));
        requests = tex::materialRequests(g, channels);
        return g;
    }
    if (src.contains("graph"))
    {
        tex::Graph g = tex::Graph::fromJson(src["graph"]);
        if (!g.firstOfType(tex::kMaterialOutputType))
            throw std::runtime_error("a brush source graph needs a Material Output");
        requests = tex::materialRequests(g, channels);
        return g;
    }
    if (src.contains("node"))
    {
        const std::string type = src["node"].get<std::string>();
        const tex::NodeDef* def = tex::findNode(type);
        if (!def || def->outputs.empty())
            throw std::runtime_error("no texture node type " + type + " with an output");
        tex::Graph g;
        g.name = "BrushSource";
        g.nodes.push_back({"Source", type, {0.0, 0.0}, src.value("values", json::object())});
        const tex::PortDecl& out = def->outputs.front();
        for (const auto& [name, t] : channels)
        {
            const bool material = out.type == tex::PortType::Material;
            requests.push_back({name, "Source", out.id, material ? t : tex::PortType::Color, material ? name : std::string()});
        }
        return g;
    }
    throw std::runtime_error("a brush source needs library, graph or node");
}

std::shared_ptr<const ValueImages> valueImages(const json& src, const std::vector<std::string>& written)
{
    std::vector<std::pair<std::string, tex::PortType>> channels;
    const bool node = src.contains("node");
    for (const std::string& c : written)
    {
        if (c == kMaskChannel)
            continue;
        // A material graph only has its Material Output's channels.
        auto it = std::find_if(tex::standardChannels().begin(), tex::standardChannels().end(), [&](const auto& s) { return s.first == c; });
        if (it != tex::standardChannels().end())
            channels.emplace_back(c, it->second);
        else if (node)
            channels.emplace_back(c, tex::PortType::Color);
    }
    const int res = std::clamp(src.value("resolution", 512), 16, 4096);
    std::string key = src.dump() + "|" + std::to_string(res);
    for (const auto& [c, t] : channels)
        key += "|" + c;
    {
        std::scoped_lock lock(gGraphCacheMutex);
        if (auto it = gValueCache.find(key); it != gValueCache.end())
            return it->second;
    }
    auto out = std::make_shared<ValueImages>();
    out->width = out->height = res;
    if (!channels.empty())
    {
        std::vector<tex::OutputRequest> requests;
        const tex::Graph g = sourceGraph(src, channels, requests);
        tex::PlanRequest req;
        req.width = req.height = res;
        req.outputs = requests;
        const tex::Plan plan = tex::buildPlan(g, req);
        if (!plan.error.empty())
            throw std::runtime_error("brush source: " + plan.error);
        tex::EvalResult r = tex::evaluate(plan, {}, "");
        for (auto& [name, img] : r.outputs)
            if (img.width == res && img.height == res)
                out->channels[name] = std::move(img.rgba);
    }
    std::scoped_lock lock(gGraphCacheMutex);
    gValueCache[key] = out;
    return out;
}

/** A tip from a texture graph: its first output's luminance times alpha. */
std::shared_ptr<const brush::AlphaImage> tipImage(const json& tip)
{
    const int res = std::clamp(tip.value("resolution", 256), 16, 2048);
    const std::string key = tip.dump() + "|" + std::to_string(res);
    {
        std::scoped_lock lock(gGraphCacheMutex);
        if (auto it = gTipCache.find(key); it != gTipCache.end())
            return it->second;
    }
    json src = tip;
    src["resolution"] = res;
    std::shared_ptr<const ValueImages> v = valueImages(src, {"baseColor"});
    auto it = v->channels.find("baseColor");
    if (it == v->channels.end())
        throw std::runtime_error("the brush tip graph has no colour output");
    auto a = std::make_shared<brush::AlphaImage>();
    a->width = a->height = res;
    a->values.resize(size_t(res) * size_t(res));
    for (size_t i = 0; i < a->values.size(); ++i)
    {
        const float* t = &it->second[i * 4];
        a->values[i] = std::clamp((t[0] * 0.2126f + t[1] * 0.7152f + t[2] * 0.0722f) * t[3], 0.0f, 1.0f);
    }
    std::scoped_lock lock(gGraphCacheMutex);
    gTipCache[key] = a;
    return a;
}

// ---------------------------------------------------------------------------------------------------- strokes

uint32_t modeOf(const Payload& p, uint32_t& sub)
{
    sub = 0;
    if (p.kind == "fill")
        return kModeFill;
    if (p.kind == "project")
        return kModeProject;
    if (p.kind == "normal")
    {
        sub = p.normalMode == "stamp" ? 1 : 0;
        return kModeNormal;
    }
    static const char* filters[] = {"smudge", "blur", "sharpen", "clone", "heal"};
    for (uint32_t i = 0; i < 5; ++i)
        if (p.kind == filters[i])
        {
            sub = i;
            return kModeFilter;
        }
    return kModePaint;
}

/** What a stroke needs on the GPU besides its dabs, from its header. image is a project stroke's picture. */
std::unique_ptr<Live> makeLive(StrokePtr header, std::shared_ptr<const PaintMesh> mesh, std::shared_ptr<const PaintImage> image = nullptr)
{
    auto live = std::make_unique<Live>();
    live->header = header;
    live->mesh = std::move(mesh);
    const brush::BrushSettings b = brush::BrushSettings::fromJson(header->brush);
    live->roundness = float(std::clamp(b.roundness, 1e-3, 1.0));
    live->strength = float(std::clamp(b.strength, 0.0, 1.0));
    const brush::Falloff f = b.resolveFalloff();
    live->falloff.resize(257);
    for (int i = 0; i <= 256; ++i)
        live->falloff[size_t(i)] = float(f.evaluate(i / 256.0));
    const Payload& p = header->payload;
    if (!p.tip.is_null())
        live->alpha = tipImage(p.tip);
    else if (!b.alphaPath.empty())
    {
        std::string err;
        live->alpha = brush::AlphaImage::load(brush::alphas::resolve(b.alphaPath), &err);
        if (!live->alpha)
            log::warn("paint stroke: alpha {}: {}", b.alphaPath, err);
    }
    live->mode = modeOf(p, live->sub);
    for (const std::string& c : p.written())
    {
        if (live->channels.size() >= kMaxChannels)
            break;
        live->channels.push_back(c);
        auto it = p.channels.find(c);
        live->values.push_back(it != p.channels.end() ? it->second : glm::vec3(1.0f));
    }
    if (live->mode == kModeFill)
    {
        live->triMask.assign((live->mesh->triangleCount() + 31) / 32 + 1, 0u);
        for (uint32_t t : p.triangles)
            if (t < live->mesh->triangleCount())
                live->triMask[t >> 5] |= 1u << (t & 31u);
    }
    live->sym = PaintSymmetry::fromJson(header->symmetry);
    if (live->mode == kModeProject)
    {
        if (!image)
            throw std::runtime_error("the projection's image " + p.image + " is missing");
        if (!header->view)
            throw std::runtime_error("a projection needs the view it was painted in");
        // Copy k sees a texel at p where the stroke's view saw transform(k) * p.
        for (uint32_t k = 0; k < live->sym.count(); ++k)
        {
            const glm::dmat4 t = live->sym.transform(k), inv = glm::inverse(t);
            StrokeView v = *header->view;
            v.objectToClip = glm::mat4(glm::dmat4(header->view->objectToClip) * t);
            v.eye = glm::vec3(inv * glm::dvec4(glm::dvec3(header->view->eye), 1.0));
            v.direction = glm::vec3(glm::normalize(glm::dmat3(inv) * glm::dvec3(header->view->direction)));
            live->views.push_back(v);
        }
        live->image = image->rgba();
        live->imageW = image->width;
        live->imageH = image->height;
        glm::vec2 lo(1e30f), hi(-1e30f);
        for (int y = 0; y < image->height; ++y)
            for (int x = 0; x < image->width; ++x)
                if (live->image[(size_t(y) * size_t(image->width) + size_t(x)) * 4 + 3] > 0.0f)
                {
                    lo = glm::min(lo, glm::vec2(float(x), float(y)));
                    hi = glm::max(hi, glm::vec2(float(x + 1), float(y + 1)));
                }
        const glm::vec2 size(float(image->width), float(image->height));
        live->imageBox = lo.x <= hi.x ? glm::vec4(lo / size * 2.0f - 1.0f, hi / size * 2.0f - 1.0f) : glm::vec4(0.0f);
    }
    if (live->mode == kModeNormal && live->sub == 1)
    {
        std::string err;
        image::Image st = image::read(paths::fromUtf8(p.stampImage), &err);
        if (st.width <= 0)
            throw std::runtime_error("normal stamp " + p.stampImage + ": " + err);
        live->stamp = std::move(st.rgba);
        live->stampW = st.width;
        live->stampH = st.height;
    }
    if (!p.source.is_null() && (live->mode == kModePaint || live->mode == kModeFill))
    {
        live->sourceImages = valueImages(p.source, live->channels);
        live->valueScale = float(p.source.value("scale", 1.0));
    }
    return live;
}

/** The pictures and tables a stroke reads, on the device once per stroke (render thread). */
void ensureUploads(VulkanContext& vk, Gpu& g, Live& live)
{
    if (live.uploaded)
        return;
    live.uploaded = true;
    auto upload = [&](const void* data, size_t bytes) {
        std::vector<uint8_t> padded(std::max<size_t>(bytes, 16), 0);
        if (bytes)
            std::memcpy(padded.data(), data, bytes);
        compute::Buffer dst = compute::makeBuffer(vk, padded.size(), 0, compute::Host::None);
        compute::Buffer staging = compute::hostBuffer(vk, padded.data(), padded.size());
        g.sub.run(vk, [&](VkCommandBuffer cmd) { compute::copy(cmd, staging, dst, bytes); });
        compute::destroy(vk, staging);
        live.uploads.push_back(dst);
        return dst.address;
    };
    live.falloffAddr = upload(live.falloff.data(), live.falloff.size() * 4);
    if (live.alpha && live.alpha->width > 0)
        live.alphaAddr = upload(live.alpha->values.data(), live.alpha->values.size() * 4);
    if (!live.triMask.empty())
        live.triMaskAddr = upload(live.triMask.data(), live.triMask.size() * 4);
    if (!live.image.empty())
        live.imageAddr = upload(live.image.data(), live.image.size() * 4);
    if (!live.stamp.empty())
        live.stampAddr = upload(live.stamp.data(), live.stamp.size() * 4);
    if (live.sourceImages)
        for (size_t c = 0; c < live.channels.size(); ++c)
            if (auto it = live.sourceImages->channels.find(live.channels[c]); it != live.sourceImages->channels.end())
                live.valueAddr[c] = upload(it->second.data(), it->second.size() * 4);
}

void releaseLive(VulkanContext& vk, Live& live)
{
    compute::destroy(vk, live.depth);
    for (compute::Buffer& b : live.uploads)
        compute::destroy(vk, b);
    live.uploads.clear();
}

GpuDab gpuDab(Live& live, const Dab& d)
{
    const PaintMesh& m = *live.mesh;
    GpuDab g{};
    if (d.triangle >= m.triangleCount())
        return g;
    const glm::vec3 c = m.point(d), n = m.normal(d.triangle, d.u, d.v);
    g.centerRadius = glm::vec4(c, d.radius);
    g.normalAmount = glm::vec4(n, d.opacity);
    const bool round = live.roundness >= 1.0f && !live.alpha && live.stamp.empty();
    if (!round)
    {
        const glm::vec3 t0 = tipFrame(n), b0 = glm::cross(n, t0);
        // A negative roundness tells the kernel the copy is mirrored: its tip, alpha and stamp turn the other way.
        g.tangentRoundness = glm::vec4(t0 * std::cos(d.angle) + b0 * std::sin(d.angle), live.sym.mirrored(d.copy) ? -live.roundness : live.roundness);
    }
    if (auto it = live.last.find(d.copy); it != live.last.end() && it->second != c)
        g.direction = glm::vec4(c - it->second, 1.0f);
    live.last[d.copy] = c;
    const Payload& p = live.header->payload;
    if (p.kind == "clone" || p.kind == "heal")
    {
        // Each copy clones from its own source.
        const glm::vec2 off = d.copy < p.sourceOffsets.size() ? p.sourceOffsets[d.copy] : p.sourceOffset;
        g.direction = glm::vec4(off, 0.0f, 2.0f);
    }
    return g;
}

/** The depth of the stroke's view, once per stroke (render thread). */
void ensureDepth(VulkanContext& vk, Gpu& g, Live& live)
{
    if (live.depthReady || !live.view() || !live.header->occlusion || live.mode == kModeFill)
        return;
    const StrokeView& v = *live.view();
    const double scale = std::min(1.0, double(kMaxDepthSize) / double(std::max(v.width, v.height)));
    live.depthW = std::max(1, int(v.width * scale));
    live.depthH = std::max(1, int(v.height * scale));
    live.depth = compute::makeBuffer(vk, VkDeviceSize(live.depthW) * live.depthH * 4, 0, compute::Host::None);
    const TriBuffer& tb = g.triangles(vk, live.mesh);
    DepthPush push{v.objectToClip, tb.buffer.address, live.depth.address, uint32_t(live.mesh->triangleCount()), uint32_t(live.depthW),
                   uint32_t(live.depthH), 0};
    const uint64_t groups = std::max<uint64_t>((push.triCount + 63) / 64, 1);
    push.groupsX = uint32_t(std::min<uint64_t>(groups, 65535));
    g.sub.run(vk, [&](VkCommandBuffer cmd) {
        vkCmdFillBuffer(cmd, live.depth.buffer, 0, VK_WHOLE_SIZE, 0);
        compute::memoryBarrier(cmd);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipeline(vk, g.depth, "paintDepthCS"));
        vkCmdPushConstants(cmd, g.depth.layout, kCompute, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, push.groupsX, uint32_t((groups + push.groupsX - 1) / push.groupsX), 1);
    });
    live.depthReady = true;
}

/** Work for one batch of dabs (or a whole fill or projection), prepared on the main thread. */
struct Batch
{
    std::vector<GpuDab> dabs;
    std::vector<uint32_t> index;
    struct Item
    {
        std::shared_ptr<Surface> surface;
        int udim = 1001;
        uint32_t page = 0;
        uint32_t first = 0, count = 0;
    };
    std::vector<Item> items;
    /** Geometry pages to upload: surface, page. */
    std::vector<std::pair<std::shared_ptr<Surface>, uint32_t>> geo;

    void add(const std::shared_ptr<Surface>& s, uint32_t page, std::set<std::pair<const Surface*, uint32_t>>& queued)
    {
        if (!s->geo.contains(page) && queued.insert({s.get(), page}).second)
        {
            s->atlas->page(page);
            geo.emplace_back(s, page);
        }
    }
};

Batch prepareBatch(Layer& l, Live& live, std::span<const Dab> dabs)
{
    LILA_ZONE();
    Batch b;
    std::vector<DabReach> reach;
    const float extent = live.alpha || !live.stamp.empty() ? 1.415f : 1.0f;
    for (const Dab& d : dabs)
    {
        b.dabs.push_back(gpuDab(live, d));
        reach.push_back({glm::vec3(b.dabs.back().centerRadius), d.triangle < live.mesh->triangleCount() ? d.radius * extent : 0.0f});
    }
    std::set<std::pair<const Surface*, uint32_t>> queued;
    for (const auto& [udim, size] : l.sizes)
    {
        std::shared_ptr<Surface> s = surface(live.mesh, udim, size.first, size.second);
        for (auto& [page, list] : s->atlas->pagesFor(reach))
        {
            b.add(s, page, queued);
            b.items.push_back({s, udim, page, uint32_t(b.index.size()), uint32_t(list.size())});
            b.index.insert(b.index.end(), list.begin(), list.end());
        }
    }
    return b;
}

bool wholeStroke(const Live& live) { return live.mode == kModeFill || live.mode == kModeProject; }

/** A fill's or projection's pages: every page holding a triangle it reaches. */
Batch prepareWhole(Layer& l, Live& live)
{
    LILA_ZONE();
    Batch b;
    const PaintMesh& m = *live.mesh;
    std::vector<uint8_t> reached(m.triangleCount(), 0);
    if (live.mode == kModeFill)
        for (size_t t = 0; t < m.triangleCount(); ++t)
            reached[t] = (live.triMask[t >> 5] >> (t & 31)) & 1u;
    else
    {
        const glm::mat4& toClip = live.view()->objectToClip;
        const glm::vec4 box = live.imageBox;
        for (size_t t = 0; t < m.triangleCount(); ++t)
        {
            if (!m.paintable[t])
                continue;
            glm::vec2 lo(1e30f), hi(-1e30f);
            bool front = false;
            for (int k = 0; k < 3; ++k)
            {
                const glm::vec4 c = toClip * glm::vec4(m.positions[t * 3 + size_t(k)], 1.0f);
                if (c.w <= 1e-6f)
                    continue;
                front = true;
                lo = glm::min(lo, glm::vec2(c) / c.w);
                hi = glm::max(hi, glm::vec2(c) / c.w);
            }
            reached[t] = front && hi.x >= box.x && lo.x <= box.z && hi.y >= box.y && lo.y <= box.w;
        }
    }
    std::set<std::pair<const Surface*, uint32_t>> queued;
    for (const auto& [udim, size] : l.sizes)
    {
        std::shared_ptr<Surface> s = surface(live.mesh, udim, size.first, size.second);
        for (uint32_t page : s->atlas->pagesWhere([&](uint32_t t) { return reached[t] != 0; }))
        {
            b.add(s, page, queued);
            b.items.push_back({s, udim, page, 0, 0});
        }
    }
    return b;
}

/** Paints a batch (render thread): copy on write, build-up pages, the dab kernel, then the images. */
size_t runBatch(VulkanContext& vk, Gpu& g, const Key& key, Layer& l, Live& live, Batch& b)
{
    LILA_ZONE();
    pruneSurfaces(vk, g);
    ensureDepth(vk, g, live);
    ensureUploads(vk, g, live);
    std::vector<compute::Buffer> temps;
    size_t newPages = 0;
    const Stroke& h = *live.header;
    GpuParams params;
    if (const StrokeView* view = live.view(); view && live.mode != kModeFill)
    {
        params.objectToClip = view->objectToClip;
        params.clipToObject = glm::inverse(view->objectToClip);
        params.eye = glm::vec4(view->eye, 1.0f);
        params.viewDir = glm::vec4(glm::normalize(view->direction), 0.0f);
        params.flags |= kFlagView;
        if (live.depthReady)
        {
            params.flags |= kFlagOcclusion;
            params.depth = live.depth.address;
            params.depthW = uint32_t(live.depthW);
            params.depthH = uint32_t(live.depthH);
        }
    }
    const Payload& pay = h.payload;
    if (pay.kind == "direction")
    {
        params.flags |= kFlagDirection;
        if (pay.directionChannel != "normal")
            params.flags |= kFlagFlow;
    }
    params.mode = live.mode;
    params.sub = live.sub;
    params.channels = uint32_t(live.channels.size());
    params.blend = uint32_t(pay.blend);
    params.strength = live.strength;
    params.tilt = pay.tilt;
    params.filterRadius = pay.filterRadius;
    params.amount = pay.amount;
    params.sourceU = pay.sourceOffset.x;
    params.sourceV = pay.sourceOffset.y;
    params.facingFade = pay.facingFade;
    params.intensity = pay.intensity;
    params.valueScale = live.valueScale;
    for (size_t c = 0; c < live.channels.size(); ++c)
    {
        params.values[c] = glm::vec4(live.values[c], 1.0f);
        if (live.channels[c] == kMaskChannel)
            params.maskChannel = uint32_t(c);
        if ((pay.kind == "direction" || pay.kind == "normal") && live.channels[c] == pay.directionChannel)
            params.directionChannel = uint32_t(c);
        if (pay.kind == "project" && live.channels[c] == pay.colorChannel)
            params.colorChannel = uint32_t(c);
        if (live.channels[c] == "normal")
            params.normalMask |= 1u << c;
        if (live.valueAddr[c])
        {
            params.valueImages[c] = live.valueAddr[c];
            params.flags |= kFlagValueImages;
        }
    }
    if (live.sourceImages)
    {
        params.valueW = uint32_t(live.sourceImages->width);
        params.valueH = uint32_t(live.sourceImages->height);
    }

    // Copy on write for every channel page the batch reaches, and a cleared build-up page per page.
    std::vector<GpuItem> items(b.items.size());
    std::vector<std::pair<uint32_t, uint32_t>> copies; // before, after
    std::vector<uint32_t> clearHalf, clearWide;
    std::vector<std::pair<std::vector<GeoTexel>*, uint32_t>> uploads;
    for (auto& [s, page] : b.geo)
    {
        const uint32_t slot = g.wide.alloc(vk);
        s->geo[page] = slot;
        uploads.emplace_back(const_cast<std::vector<GeoTexel>*>(&s->atlas->page(page)), slot);
    }
    std::map<int, uint32_t> tileIndex;
    for (const auto& [udim, size] : l.sizes)
        tileIndex.emplace(udim, uint32_t(tileIndex.size()));
    for (size_t i = 0; i < b.items.size(); ++i)
    {
        const Batch::Item& it = b.items[i];
        GpuItem& gi = items[i];
        gi.geo = g.wide.address(it.surface->geo.at(it.page));
        auto [acc, freshAcc] = live.acc.try_emplace({it.udim, it.page}, kNone);
        if (freshAcc)
        {
            acc->second = g.wide.alloc(vk);
            clearWide.push_back(acc->second);
        }
        gi.acc = g.wide.address(acc->second);
        gi.dabFirst = it.first;
        gi.dabCount = it.count;
        gi.page = it.page;
        gi.tile = tileIndex.count(it.udim) ? tileIndex[it.udim] : 0;
        for (size_t c = 0; c < live.channels.size(); ++c)
        {
            const TileKey tk{live.channels[c], it.udim};
            auto [cw, fresh] = live.cow.try_emplace({tk, it.page});
            if (fresh)
            {
                cw->second.before = channelSlot(l, tk, it.page);
                cw->second.after = g.half.alloc(vk);
                if (cw->second.before == kNone)
                {
                    clearHalf.push_back(cw->second.after);
                    ++newPages;
                }
                else
                    copies.emplace_back(cw->second.before, cw->second.after);
                setSlot(l, tk, it.page, cw->second.after);
            }
            l.tiles[tk].dirty.insert(it.page);
            gi.dst[c] = g.half.address(cw->second.after);
            gi.src[c] = cw->second.before == kNone ? 0 : g.half.address(cw->second.before);
        }
    }

    auto host = [&](const void* data, size_t bytes) {
        // Never read past a small (or empty) source to make up the minimum size.
        uint8_t small[16] = {};
        if (bytes && bytes < 16)
            std::memcpy(small, data, bytes);
        temps.push_back(bytes >= 16 ? compute::hostBuffer(vk, data, bytes) : compute::hostBuffer(vk, small, 16));
        return temps.back().address;
    };
    // Tiles, and for filters each channel's pages as they were before the stroke.
    std::vector<GpuTile> tiles;
    for (const auto& [udim, size] : l.sizes)
    {
        GpuTile t;
        t.width = uint32_t(size.first);
        t.height = uint32_t(size.second);
        t.pagesX = uint32_t((size.first + kPageSize - 1) / kPageSize);
        t.pagesY = uint32_t((size.second + kPageSize - 1) / kPageSize);
        if (live.mode == kModeFilter)
            for (size_t c = 0; c < live.channels.size(); ++c)
            {
                const TileKey tk{live.channels[c], udim};
                std::vector<VkDeviceAddress> table(size_t(t.pagesX) * t.pagesY, 0);
                if (auto ct = l.tiles.find(tk); ct != l.tiles.end())
                    for (auto& [page, slot] : ct->second.pages)
                        if (page < table.size())
                            table[page] = g.half.address(slot);
                for (auto& [k, cw] : live.cow)
                    if (k.first == tk && k.second < table.size())
                        table[k.second] = cw.before == kNone ? 0 : g.half.address(cw.before);
                t.tables[c] = host(table.data(), table.size() * sizeof(VkDeviceAddress));
            }
        tiles.push_back(t);
    }
    std::vector<compute::Buffer> geoStaging;
    for (auto& [texels, slot] : uploads)
        geoStaging.push_back(compute::hostBuffer(vk, texels->data(), texels->size() * sizeof(GeoTexel)));
    const TriBuffer& tb = g.triangles(vk, live.mesh);
    params.items = host(items.data(), items.size() * sizeof(GpuItem));
    params.dabIndex = host(b.index.data(), b.index.size() * 4);
    params.dabs = host(b.dabs.data(), b.dabs.size() * sizeof(GpuDab));
    params.tiles = host(tiles.data(), tiles.size() * sizeof(GpuTile));
    params.tris = tb.buffer.address;
    params.falloff = live.falloffAddr;
    if (live.alphaAddr)
    {
        params.alpha = live.alphaAddr;
        params.alphaW = uint32_t(live.alpha->width);
        params.alphaH = uint32_t(live.alpha->height);
    }
    params.triMask = live.triMaskAddr;
    params.image = live.imageAddr;
    params.imageW = uint32_t(live.imageW);
    params.imageH = uint32_t(live.imageH);
    params.stamp = live.stampAddr;
    params.stampW = uint32_t(live.stampW);
    params.stampH = uint32_t(live.stampH);
    params.itemCount = uint32_t(items.size());
    const VkDeviceAddress paramsAddr = host(&params, sizeof(params));

    g.sub.run(vk, [&](VkCommandBuffer cmd) {
        for (size_t i = 0; i < uploads.size(); ++i)
        {
            VkBufferCopy r{0, g.wide.offset(uploads[i].second), kWidePage};
            vkCmdCopyBuffer(cmd, geoStaging[i].buffer, g.wide.buffer(uploads[i].second), 1, &r);
        }
        for (uint32_t s : clearWide)
            vkCmdFillBuffer(cmd, g.wide.buffer(s), g.wide.offset(s), kWidePage, 0);
        for (uint32_t s : clearHalf)
            vkCmdFillBuffer(cmd, g.half.buffer(s), g.half.offset(s), kHalfPage, 0);
        for (auto [from, to] : copies)
        {
            VkBufferCopy r{g.half.offset(from), g.half.offset(to), kHalfPage};
            vkCmdCopyBuffer(cmd, g.half.buffer(from), g.half.buffer(to), 1, &r);
        }
        compute::memoryBarrier(cmd);
        if (!items.empty())
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipeline(vk, g.dab, "paintDabCS"));
            for (uint32_t first = 0; first < items.size(); first += kItemsPerDispatch)
            {
                DabPush push{paramsAddr, first, 0};
                vkCmdPushConstants(cmd, g.dab.layout, kCompute, 0, sizeof(push), &push);
                vkCmdDispatch(cmd, uint32_t(kTexels / 64), std::min<uint32_t>(kItemsPerDispatch, uint32_t(items.size()) - first), 1);
            }
            compute::memoryBarrier(cmd);
        }
        recordResolve(vk, cmd, g, key, l, temps);
    });
    for (compute::Buffer& t : temps)
        compute::destroy(vk, t);
    for (compute::Buffer& t : geoStaging)
        compute::destroy(vk, t);
    return newPages;
}

/** Applies dabs to the live stroke in batches. Main thread. */
void paintDabs(const Key& key, Layer& l, std::span<const Dab> dabs)
{
    Live& live = *l.live;
    for (size_t i = 0; i < dabs.size(); i += kDabsPerBatch)
    {
        std::span<const Dab> part = dabs.subspan(i, std::min<size_t>(kDabsPerBatch, dabs.size() - i));
        Batch b = prepareBatch(l, live, part);
        gpuRun([&](VulkanContext& vk, Gpu& g) { runBatch(vk, g, key, l, live, b); });
        live.dabs += part.size();
    }
}

/** Paints a fill or a projection in one go. Main thread. */
void paintWhole(const Key& key, Layer& l)
{
    Live& live = *l.live;
    for (size_t v = 0; v < std::max<size_t>(live.views.size(), 1); ++v)
    {
        if (v > 0)
            gpuRun([&](VulkanContext& vk, Gpu&) {
                compute::destroy(vk, live.depth);
                live.depthReady = false;
            });
        live.viewIndex = v;
        Batch b = prepareWhole(l, live);
        gpuRun([&](VulkanContext& vk, Gpu& g) { runBatch(vk, g, key, l, live, b); });
    }
}

/** Ends the live stroke: the build-up pages go back; the originals go to undo (returned) or back to the pool. */
std::vector<std::pair<std::pair<TileKey, uint32_t>, CowPage>> finishLive(Layer& l, bool keepOriginals)
{
    std::vector<std::pair<std::pair<TileKey, uint32_t>, CowPage>> out(l.live->cow.begin(), l.live->cow.end());
    std::unique_ptr<Live> live = std::move(l.live);
    gpuRun([&](VulkanContext& vk, Gpu& g) {
        for (auto& [k, slot] : live->acc)
            g.wide.release(slot);
        if (!keepOriginals)
            for (auto& [k, c] : out)
                g.half.release(c.before);
        releaseLive(vk, *live);
    });
    return out;
}

void cancel(const Key& key, Layer& l)
{
    if (!l.live)
        return;
    std::unique_ptr<Live> live = std::move(l.live);
    gpuRun([&](VulkanContext& vk, Gpu& g) {
        for (auto& [k, c] : live->cow)
        {
            setSlot(l, k.first, k.second, c.before);
            g.half.release(c.after);
        }
        for (auto& [k, slot] : live->acc)
            g.wide.release(slot);
        releaseLive(vk, *live);
        resolve(vk, g, key, l);
    });
}

std::shared_ptr<const PaintImage> imageOf(const scene::Node* set, const Stroke& s)
{
    if (s.payload.image.empty() || !set)
        return nullptr;
    PaintPtr data = readPaint(*set);
    auto it = data->images.find(s.payload.image);
    return it == data->images.end() ? nullptr : it->second;
}

/** Paints a whole stroke with no undo (rasterising from records). */
void replay(const Key& key, Layer& l, scene::Graph& g, const std::string& material, const StrokePtr& s)
{
    std::shared_ptr<const PaintMesh> mesh = paintMesh(g, s->mesh, material);
    if (!mesh)
    {
        log::warn("paint buffer {}: stroke {} paints {}, which isn't a mesh any more; skipped", key.buffer, s->id, s->mesh);
        return;
    }
    try
    {
        l.live = makeLive(s, mesh, imageOf(g.node(key.set), *s));
    }
    catch (const std::exception& e)
    {
        log::warn("paint buffer {}: stroke {}: {}; skipped", key.buffer, s->id, e.what());
        return;
    }
    paintDabs(key, l, s->dabs);
    if (wholeStroke(*l.live))
        paintWhole(key, l);
    finishLive(l, false);
}

size_t pagesOf(const Layer& l)
{
    size_t n = 0;
    for (auto& [k, t] : l.tiles)
        n += t.pages.size();
    return n;
}

void freeAll(Layer& l)
{
    if (l.live)
        finishLive(l, false);
    if (l.epoch != gEpoch.load() || l.tiles.empty())
    {
        l.tiles.clear();
        return;
    }
    gpuRun([&](VulkanContext&, Gpu& g) {
        for (auto& [k, t] : l.tiles)
            for (auto& [p, s] : t.pages)
                g.half.release(s);
    });
    for (auto& [k, t] : l.tiles)
    {
        for (auto& [p, s] : t.pages)
            t.dirty.insert(p);
        t.pages.clear();
    }
}

// ---------------------------------------------------------------------------------------------------- baselines

/** Reads pages back: slot -> texels (render thread inside gpuRun). */
std::vector<std::vector<uint8_t>> readSlots(VulkanContext& vk, Gpu& g, const std::vector<uint32_t>& slots)
{
    std::vector<std::vector<uint8_t>> out(slots.size());
    if (slots.empty())
        return out;
    compute::Buffer host = compute::makeBuffer(vk, kHalfPage * slots.size(), 0, compute::Host::Read);
    g.sub.run(vk, [&](VkCommandBuffer cmd) {
        for (size_t i = 0; i < slots.size(); ++i)
        {
            VkBufferCopy r{g.half.offset(slots[i]), kHalfPage * i, kHalfPage};
            vkCmdCopyBuffer(cmd, g.half.buffer(slots[i]), host.buffer, 1, &r);
        }
    });
    vmaInvalidateAllocation(vk.allocator, host.alloc, 0, VK_WHOLE_SIZE);
    const auto* p = static_cast<const uint8_t*>(host.mapped);
    for (size_t i = 0; i < slots.size(); ++i)
        out[i].assign(p + kHalfPage * i, p + kHalfPage * (i + 1));
    compute::destroy(vk, host);
    return out;
}

/** Writes pages of texels into the layer (render thread inside gpuRun). */
void writePages(VulkanContext& vk, Gpu& g, Layer& l, const TileKey& tk, const std::map<uint32_t, std::vector<uint8_t>>& pages)
{
    if (pages.empty())
        return;
    std::vector<uint8_t> all;
    std::vector<std::pair<uint32_t, uint32_t>> slots;
    for (const auto& [p, texels] : pages)
    {
        const uint32_t s = g.half.alloc(vk);
        slots.emplace_back(p, s);
        all.insert(all.end(), texels.begin(), texels.end());
        setSlot(l, tk, p, s);
    }
    compute::Buffer staging = compute::hostBuffer(vk, all.data(), all.size());
    g.sub.run(vk, [&](VkCommandBuffer cmd) {
        for (size_t i = 0; i < slots.size(); ++i)
        {
            VkBufferCopy r{kHalfPage * i, g.half.offset(slots[i].second), kHalfPage};
            vkCmdCopyBuffer(cmd, staging.buffer, g.half.buffer(slots[i].second), 1, &r);
        }
    });
    compute::destroy(vk, staging);
}

/** A baseline tile's texels at another size, bilinear, back as pages (all-zero pages dropped). */
std::map<uint32_t, std::vector<uint8_t>> resample(const Baseline::Tile& t, const std::vector<uint8_t>& raw, int w, int h)
{
    const int sw = std::max(1, t.width), sh = std::max(1, t.height);
    const int spx = (sw + kPageSize - 1) / kPageSize;
    std::vector<float> src(size_t(sw) * sh * 4, 0.0f);
    for (size_t i = 0; i < t.pages.size(); ++i)
    {
        const uint32_t p = t.pages[i];
        const int x0 = int(p % uint32_t(spx)) * kPageSize, y0 = int(p / uint32_t(spx)) * kPageSize;
        const auto* texels = reinterpret_cast<const uint16_t*>(raw.data() + kHalfPage * i);
        for (int y = 0; y < kPageSize && y0 + y < sh; ++y)
            for (int x = 0; x < kPageSize && x0 + x < sw; ++x)
                for (int c = 0; c < 4; ++c)
                    src[(size_t(y0 + y) * sw + (x0 + x)) * 4 + c] = halfToFloat(texels[(size_t(y) * kPageSize + x) * 4 + c]);
    }
    auto at = [&](int x, int y, int c) { return src[(size_t(std::clamp(y, 0, sh - 1)) * sw + std::clamp(x, 0, sw - 1)) * 4 + c]; };
    const int pagesX = (w + kPageSize - 1) / kPageSize, pagesY = (h + kPageSize - 1) / kPageSize;
    std::map<uint32_t, std::vector<uint8_t>> out;
    for (int py = 0; py < pagesY; ++py)
        for (int px = 0; px < pagesX; ++px)
        {
            std::vector<uint8_t> page(kHalfPage, 0);
            auto* texels = reinterpret_cast<uint16_t*>(page.data());
            bool any = false;
            for (int y = 0; y < kPageSize && py * kPageSize + y < h; ++y)
                for (int x = 0; x < kPageSize && px * kPageSize + x < w; ++x)
                {
                    const double sx = (px * kPageSize + x + 0.5) * sw / double(w) - 0.5, sy = (py * kPageSize + y + 0.5) * sh / double(h) - 0.5;
                    const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                    const float fx = float(sx - ix), fy = float(sy - iy);
                    for (int c = 0; c < 4; ++c)
                    {
                        const float v = (at(ix, iy, c) * (1 - fx) + at(ix + 1, iy, c) * fx) * (1 - fy) + (at(ix, iy + 1, c) * (1 - fx) + at(ix + 1, iy + 1, c) * fx) * fy;
                        texels[(size_t(y) * kPageSize + x) * 4 + c] = floatToHalf(v);
                        any = any || v != 0.0f;
                    }
                }
            if (any)
                out[uint32_t(py * pagesX + px)] = std::move(page);
        }
    return out;
}

void loadBaseline(Layer& l, const Baseline& b)
{
    std::map<TileKey, std::map<uint32_t, std::vector<uint8_t>>> all;
    for (const Baseline::Tile& t : b.tiles)
    {
        auto size = l.sizes.find(t.udim);
        if (size == l.sizes.end())
            continue;
        const std::vector<uint8_t> raw = t.raw();
        auto& pages = all[{t.channel, t.udim}];
        if (t.width == size->second.first && t.height == size->second.second)
            for (size_t i = 0; i < t.pages.size(); ++i)
                pages[t.pages[i]].assign(raw.begin() + std::ptrdiff_t(kHalfPage * i), raw.begin() + std::ptrdiff_t(kHalfPage * (i + 1)));
        else
            pages = resample(t, raw, size->second.first, size->second.second);
    }
    gpuRun([&](VulkanContext& vk, Gpu& g) {
        for (auto& [tk, pages] : all)
            writePages(vk, g, l, tk, pages);
    });
}

// ---------------------------------------------------------------------------------------------------- undo

/** A stroke's or a clear's pages before and after; undo swaps them back into the buffer's tables. */
struct PageRecord
{
    Key key;
    uint64_t generation = 0;
    uint64_t epoch = 0;
    struct Entry
    {
        TileKey tile;
        uint32_t page = 0;
        uint32_t before = kNone, after = kNone;
    };
    std::vector<Entry> entries;
    std::vector<uint64_t> appliedBefore, appliedAfter;
    std::shared_ptr<const Baseline> baselineBefore, baselineAfter;
    uint64_t clearsBefore = 0, clearsAfter = 0;
    bool done = true;
};

class PageUndo final : public UndoAction
{
public:
    explicit PageUndo(std::shared_ptr<PageRecord> r) : m(std::move(r)) {}
    ~PageUndo() override
    {
        // The side not in the buffer's tables belongs to this step.
        if (m->epoch != gEpoch.load() || !Renderer::get().running())
            return;
        std::vector<uint32_t> slots;
        for (const PageRecord::Entry& e : m->entries)
            slots.push_back(m->done ? e.before : e.after);
        if (std::all_of(slots.begin(), slots.end(), [](uint32_t s) { return s == kNone; }))
            return;
        try
        {
            gpuRun([&](VulkanContext&, Gpu& g) {
                for (uint32_t s : slots)
                    g.half.release(s);
            });
        }
        catch (const std::exception&)
        {
        }
    }
    void undo() override { swap(false); }
    void redo() override { swap(true); }
    size_t bytes() const override { return 64 + m->entries.size() * (sizeof(PageRecord::Entry) + kHalfPage); }

private:
    void swap(bool forward)
    {
        if (m->done == forward)
            return;
        Layer* l = findLayer(m->key);
        if (!l || l->generation != m->generation)
            return;
        if (l->live)
            cancel(m->key, *l);
        m->done = forward;
        for (const PageRecord::Entry& e : m->entries)
            setSlot(*l, e.tile, e.page, forward ? e.after : e.before);
        l->applied = forward ? m->appliedAfter : m->appliedBefore;
        l->baseline = forward ? m->baselineAfter : m->baselineBefore;
        l->clears = forward ? m->clearsAfter : m->clearsBefore;
        const Key key = m->key;
        gpuRun([&](VulkanContext& vk, Gpu& g) { resolve(vk, g, key, *l); });
        notify(key.set);
    }

    std::shared_ptr<PageRecord> m;
};

// ---------------------------------------------------------------------------------------------------- sync

std::map<int, std::pair<int, int>> wantedSizes(const scene::Node& set, const Buffer* data)
{
    const tex::SetSettings s = tex::readSettings(set);
    std::map<int, std::pair<int, int>> out;
    for (const tex::Tile& t : s.tiles)
        out[t.udim] = tileSize(s, t, data);
    return out;
}

void publishSizes(const Key& key, const Layer& l)
{
    std::scoped_lock lock(gImageMutex);
    for (const auto& [udim, size] : l.sizes)
    {
        gImageSizes[{key.set, key.buffer, udim}] = size;
        for (auto& [k, e] : gImages)
            if (std::get<0>(k) == key.set && std::get<1>(k) == key.buffer && std::get<3>(k) == udim && e.image &&
                (e.image->width() != size.first || e.image->height() != size.second))
                e = {GpuImage::create({size.first, size.second, TextureFormat::RGBA16F, false, false, key.buffer + "." + std::get<2>(k)}), true};
    }
}

std::vector<uint64_t> strokeIds(const Buffer& b)
{
    std::vector<uint64_t> out;
    for (const StrokePtr& s : b.strokes)
        out.push_back(s->id);
    return out;
}

void doRasterize(scene::Graph& g, scene::Node& set, const std::string& id, const Buffer& data)
{
    LILA_ZONE();
    const Key key{set.id(), id};
    Layer& l = layerFor(key);
    freeAll(l);
    l.tiles.clear();
    l.generation = gGeneration++;
    l.sizes = wantedSizes(set, &data);
    publishSizes(key, l);
    l.applied.clear();
    l.baseline = data.baseline;
    l.clears = data.clears;
    if (data.baseline)
        loadBaseline(l, *data.baseline);
    const std::string material = setMaterial(set);
    for (const StrokePtr& s : data.strokes)
    {
        replay(key, l, g, material, s);
        l.applied.push_back(s->id);
    }
    l.synced = true;
    for (const auto& [udim, size] : l.sizes)
        noteChanged(l, udim, 0, 0, size.first, size.second);
    gpuRun([&](VulkanContext& vk, Gpu& gp) {
        // The images may still show pages the rebuild did not paint again: clear them, then write every page.
        for (auto& [tk, t] : l.tiles)
            for (auto& [p, s] : t.pages)
                t.dirty.insert(p);
        std::vector<compute::Buffer> temps;
        gp.sub.run(vk, [&](VkCommandBuffer cmd) {
            std::scoped_lock lock(gImageMutex);
            for (auto& [k, e] : gImages)
                if (std::get<0>(k) == key.set && std::get<1>(k) == key.buffer && e.image && e.image->prepare(vk, cmd, temps))
                {
                    e.image->beginWrite(cmd);
                    const float zero[4] = {0, 0, 0, 0};
                    e.image->recordClear(cmd, zero);
                    e.image->endWrite(cmd);
                }
        });
        for (compute::Buffer& b : temps)
            compute::destroy(vk, b);
        resolve(vk, gp, key, l);
        pruneSurfaces(vk, gp);
    });
    notify(key.set);
}

void syncOne(scene::Graph& g, scene::Node& set, const std::string& id, const Buffer& data)
{
    const Key key{set.id(), id};
    Layer& l = layerFor(key);
    const std::vector<uint64_t> ids = strokeIds(data);
    const bool same = l.synced && l.sizes == wantedSizes(set, &data) && l.baseline == data.baseline && l.clears == data.clears;
    if (same && l.applied == ids)
        return;
    if (l.live)
        cancel(key, l);
    if (same && l.applied.size() < ids.size() && std::equal(l.applied.begin(), l.applied.end(), ids.begin()))
    {
        const std::string material = setMaterial(set);
        for (size_t i = l.applied.size(); i < ids.size(); ++i)
        {
            replay(key, l, g, material, data.strokes[i]);
            l.applied.push_back(ids[i]);
        }
        gpuRun([&](VulkanContext& vk, Gpu& gp) { resolve(vk, gp, key, l); });
        notify(key.set);
        return;
    }
    doRasterize(g, set, id, data);
}

void dropLayer(const Key& key)
{
    auto it = gLayers.find(key);
    if (it == gLayers.end())
        return;
    std::shared_ptr<Layer> l = it->second;
    gLayers.erase(it);
    if (Renderer::get().running())
        try
        {
            freeAll(*l);
            gpuRun([&](VulkanContext& vk, Gpu&) { dropViews(vk, key.set, key.buffer); });
        }
        catch (const std::exception& e)
        {
            log::warn("paint buffer {}: {}", key.buffer, e.what());
        }
    std::scoped_lock lock(gImageMutex);
    std::erase_if(gImages, [&](const auto& kv) { return std::get<0>(kv.first) == key.set && std::get<1>(kv.first) == key.buffer; });
    std::erase_if(gImageSizes, [&](const auto& kv) { return std::get<0>(kv.first) == key.set && std::get<1>(kv.first) == key.buffer; });
}

void dropSet(NodeId set)
{
    std::vector<Key> keys;
    for (auto& [k, l] : gLayers)
        if (k.set == set)
            keys.push_back(k);
    for (const Key& k : keys)
        dropLayer(k);
}

std::set<NodeId> gSyncQueued;

/** After the current command or undo step has finished: bring the set's pages in line with its data. */
void queueSync(NodeId set)
{
    if (!gSyncQueued.insert(set).second)
        return;
    mainq::post([set] {
        gSyncQueued.erase(set);
        scene::Graph& g = scene::Graph::current();
        scene::Node* n = g.node(set);
        if (!n || n->type().name != tex::kTextureSetType)
        {
            dropSet(set);
            return;
        }
        try
        {
            sync(g, *n);
        }
        catch (const std::exception& e)
        {
            log::warn("texture set {}: paint: {}", n->name(), e.what());
        }
    });
}

void onSceneChange(scene::Change c, NodeId id)
{
    switch (c)
    {
    case scene::Change::Value:
    case scene::Change::Created:
        if (scene::Node* n = scene::Graph::current().node(id); n && n->type().name == tex::kTextureSetType)
        {
            bool known = false;
            for (auto& [k, l] : gLayers)
                known = known || k.set == id;
            if (known || readPaint(*n)->buffers.size())
                queueSync(id);
        }
        break;
    case scene::Change::Deleted: dropSet(id); break;
    case scene::Change::Cleared:
    case scene::Change::Reset:
    {
        std::set<NodeId> sets;
        for (auto& [k, l] : gLayers)
            sets.insert(k.set);
        scene::Graph& g = scene::Graph::current();
        for (NodeId s : sets)
        {
            scene::Node* n = g.node(s);
            if (!n || n->type().name != tex::kTextureSetType)
                dropSet(s);
            else
                queueSync(s);
        }
        break;
    }
    default: break;
    }
}
} // namespace

// ---------------------------------------------------------------------------------------------------- public

std::string setMaterial(const scene::Node& set)
{
    const tex::SetSettings s = tex::readSettings(set);
    return s.material.empty() ? set.name() : s.material;
}

std::shared_ptr<const PaintMesh> paintMesh(scene::Graph& g, const std::string& meshPath, const std::string& material)
{
    g.evaluate();
    scene::Node* n = g.find(meshPath);
    if (!n || !n->type().isA("mesh"))
        return nullptr;
    scene::MeshPtr mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    if (!mesh || !mesh->numFaces())
        return nullptr;
    MeshEntry& e = gMeshes[meshPath];
    if (e.mesh != mesh)
    {
        e.mesh = mesh;
        e.bvh = std::make_shared<const geo::MeshBvh>(*mesh);
        e.paint.clear();
    }
    std::shared_ptr<const PaintMesh>& p = e.paint[material];
    if (!p)
        p = PaintMesh::build(*mesh, e.bvh, material);
    return p;
}

void prewarmTarget(scene::Graph& g, scene::Node& set, const std::string& meshPath)
{
    LILA_ZONE();
    g.evaluate();
    scene::Node* n = g.find(meshPath);
    if (!n || !n->type().isA("mesh"))
        return;
    scene::MeshPtr mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    if (!mesh || !mesh->numFaces())
        return;
    const std::string material = setMaterial(set);
    const std::map<int, std::pair<int, int>> sizes = wantedSizes(set, nullptr);
    std::shared_ptr<const geo::MeshBvh> bvh;
    std::map<std::string, std::shared_ptr<const PaintMesh>> have;
    if (auto it = gMeshes.find(meshPath); it != gMeshes.end() && it->second.mesh == mesh)
    {
        bvh = it->second.bvh;
        have = it->second.paint;
    }
    auto surfaced = [&](const std::shared_ptr<const PaintMesh>& p) {
        for (const auto& [udim, size] : sizes)
            if (!gSurfaces.contains(std::make_tuple(p.get(), udim, size.first, size.second)))
                return false;
        return true;
    };
    if (bvh && have.contains("") && have.contains(material) && surfaced(have[material]))
        return;
    // One warm-up per mesh version at a time.
    static std::set<const void*> inFlight;
    if (!inFlight.insert(mesh.get()).second)
        return;
    std::thread([mesh, meshPath, material, sizes, bvh, have]() mutable {
        LILA_ZONE_N("paint target warm-up");
        struct Built
        {
            std::shared_ptr<const geo::MeshBvh> bvh;
            std::map<std::string, std::shared_ptr<const PaintMesh>> paint;
            std::vector<std::tuple<int, int, int, std::shared_ptr<Surface>>> surfaces;
        };
        auto out = std::make_shared<Built>();
        try
        {
            out->bvh = bvh ? bvh : std::make_shared<const geo::MeshBvh>(*mesh);
            for (const std::string& m : {std::string(), material})
                if (!out->paint.contains(m))
                    out->paint[m] = have.contains(m) ? have[m] : PaintMesh::build(*mesh, out->bvh, m);
            for (const auto& [udim, size] : sizes)
            {
                auto sf = std::make_shared<Surface>();
                sf->mesh = out->paint[material];
                sf->atlas = std::make_unique<SurfaceAtlas>(sf->mesh, udim, size.first, size.second);
                out->surfaces.emplace_back(udim, size.first, size.second, std::move(sf));
            }
        }
        catch (const std::exception& e)
        {
            log::debug("paint target warm-up: {}", e.what());
        }
        mainq::post([mesh, meshPath, material, out] {
            inFlight.erase(mesh.get());
            if (!out->bvh)
                return;
            MeshEntry& e = gMeshes[meshPath];
            // A newer mesh or a build the stroke already made wins over the warm-up.
            if (e.mesh && e.mesh != mesh)
                return;
            if (!e.mesh)
            {
                e.mesh = mesh;
                e.bvh = out->bvh;
            }
            for (auto& [m, p] : out->paint)
                if (!e.paint[m])
                    e.paint[m] = p;
            // Atlases only for the paint mesh the cache kept, which is the one strokes will use.
            for (auto& [udim, w, h, sf] : out->surfaces)
                if (e.paint[material] == sf->mesh)
                    gSurfaces.try_emplace(std::make_tuple(sf->mesh.get(), udim, w, h), sf);
        });
    }).detach();
}

void sync(scene::Graph& g, scene::Node& set)
{
    LILA_ZONE();
    PaintPtr data = readPaint(set);
    std::vector<Key> stale;
    for (auto& [k, l] : gLayers)
        if (k.set == set.id() && !data->buffers.contains(k.buffer))
            stale.push_back(k);
    for (const Key& k : stale)
        dropLayer(k);
    for (const auto& [id, buf] : data->buffers)
        syncOne(g, set, id, buf);
}

void syncBuffer(scene::Graph& g, scene::Node& set, const std::string& buffer)
{
    PaintPtr data = readPaint(set);
    if (const Buffer* b = data->buffer(buffer))
        syncOne(g, set, buffer, *b);
    else
    {
        // A buffer the data doesn't have yet: empty, at the set's size.
        const Key key{set.id(), buffer};
        Layer& l = layerFor(key);
        // Pages of a live stroke are expected; anything else came from data this buffer no longer has.
        if (!l.synced || !l.applied.empty() || l.baseline || (!l.live && !l.tiles.empty() && pagesOf(l) > 0))
            doRasterize(g, set, buffer, Buffer{});
    }
}

void rasterize(scene::Graph& g, scene::Node& set, const std::string& buffer)
{
    PaintPtr data = readPaint(set);
    const Buffer* b = data->buffer(buffer);
    doRasterize(g, set, buffer, b ? *b : Buffer{});
}

void prewarm()
{
    Renderer& r = Renderer::get();
    if (!r.running())
        return;
    installStopHook();
    // Not waited on: the driver takes hundreds of milliseconds to build the dab pipeline, which the first stroke would stall on.
    (void)r.runTask([](VulkanContext& vk) {
        try
        {
            Gpu& g = gpu();
            g.init(vk);
            g.pipeline(vk, g.dab, "paintDabCS");
            g.pipeline(vk, g.depth, "paintDepthCS");
        }
        catch (const std::exception& e)
        {
            log::warn("texture painting: {}", e.what());
        }
        return true;
    });
}

void beginLive(scene::Graph& g, scene::Node& set, const std::string& buffer, StrokePtr header)
{
    syncBuffer(g, set, buffer);
    const Key key{set.id(), buffer};
    Layer& l = layerFor(key);
    if (l.live)
        cancel(key, l);
    std::shared_ptr<const PaintMesh> mesh = paintMesh(g, header->mesh, setMaterial(set));
    if (!mesh)
        throw std::runtime_error(header->mesh + " isn't a mesh with faces");
    l.live = makeLive(std::move(header), std::move(mesh));
}

void liveDabs(NodeId set, const std::string& buffer, std::span<const Dab> dabs)
{
    const Key key{set, buffer};
    Layer* l = findLayer(key);
    if (!l || !l->live || dabs.empty())
        return;
    paintDabs(key, *l, dabs);
    notify(set);
}

void cancelLive(NodeId set, const std::string& buffer)
{
    const Key key{set, buffer};
    if (Layer* l = findLayer(key); l && l->live)
    {
        cancel(key, *l);
        notify(set);
    }
}

bool liveActive(NodeId set, const std::string& buffer)
{
    Layer* l = findLayer({set, buffer});
    return l && l->live;
}

bool anyLive(NodeId set)
{
    for (auto& [k, l] : gLayers)
        if (k.set == set && l->epoch == gEpoch.load() && l->live)
            return true;
    return false;
}

ApplyResult applyStroke(scene::Graph& g, scene::Node& set, const std::string& buffer, const Stroke& stroke, std::shared_ptr<const PaintImage> image)
{
    LILA_ZONE();
    const Key key{set.id(), buffer};
    Layer& l = layerFor(key);
    ApplyResult r;
    const bool adopt = l.live && l.live->header->id == stroke.id && l.live->dabs == stroke.dabs.size();
    if (!adopt)
    {
        if (l.live)
            cancel(key, l);
        auto header = std::make_shared<const Stroke>(stroke);
        std::shared_ptr<const PaintMesh> mesh = paintMesh(g, stroke.mesh, setMaterial(set));
        if (!mesh)
            throw std::runtime_error(stroke.mesh + " isn't a mesh with faces");
        l.live = makeLive(header, std::move(mesh), image ? image : imageOf(&set, stroke));
        paintDabs(key, l, stroke.dabs);
        if (wholeStroke(*l.live))
            paintWhole(key, l);
    }
    r.dabs = l.live->dabs;
    auto record = std::make_shared<PageRecord>();
    record->key = key;
    record->generation = l.generation;
    record->epoch = l.epoch;
    record->appliedBefore = l.applied;
    record->baselineBefore = record->baselineAfter = l.baseline;
    record->clearsBefore = record->clearsAfter = l.clears;
    for (auto& [k, c] : finishLive(l, true))
        record->entries.push_back({k.first, k.second, c.before, c.after});
    r.pages = record->entries.size();
    l.applied.push_back(stroke.id);
    record->appliedAfter = l.applied;
    UndoStack::get().record(std::make_unique<PageUndo>(record));
    notify(set.id());
    return r;
}

size_t clearPages(NodeId set, const std::string& buffer)
{
    const Key key{set, buffer};
    Layer* l = findLayer(key);
    if (!l)
        return 0;
    if (l->live)
        cancel(key, *l);
    auto record = std::make_shared<PageRecord>();
    record->key = key;
    record->generation = l->generation;
    record->epoch = l->epoch;
    record->appliedBefore = l->applied;
    record->baselineBefore = l->baseline;
    record->clearsBefore = l->clears;
    for (auto& [tk, t] : l->tiles)
        for (auto& [p, s] : t.pages)
            record->entries.push_back({tk, p, s, kNone});
    for (const PageRecord::Entry& e : record->entries)
        setSlot(*l, e.tile, e.page, kNone);
    l->applied.clear();
    l->baseline = nullptr;
    l->clears += 1;
    record->clearsAfter = l->clears;
    gpuRun([&](VulkanContext& vk, Gpu& g) { resolve(vk, g, key, *l); });
    UndoStack::get().record(std::make_unique<PageUndo>(record));
    notify(set);
    return record->entries.size();
}

std::shared_ptr<const Baseline> capture(NodeId set, const std::string& buffer)
{
    Layer* l = findLayer({set, buffer});
    auto b = std::make_shared<Baseline>();
    if (!l)
        return b;
    for (auto& [tk, t] : l->tiles)
    {
        if (t.pages.empty())
            continue;
        Baseline::Tile bt;
        bt.channel = tk.first;
        bt.udim = tk.second;
        std::tie(bt.width, bt.height) = l->sizes.count(tk.second) ? l->sizes.at(tk.second) : std::pair<int, int>{0, 0};
        std::vector<uint32_t> slots;
        for (auto& [p, s] : t.pages)
        {
            bt.pages.push_back(p);
            slots.push_back(s);
        }
        std::vector<std::vector<uint8_t>> texels;
        gpuRun([&](VulkanContext& vk, Gpu& g) { texels = readSlots(vk, g, slots); });
        std::vector<uint8_t> raw;
        raw.reserve(kHalfPage * slots.size());
        for (const auto& tx : texels)
            raw.insert(raw.end(), tx.begin(), tx.end());
        bt.data = compressPages(raw);
        b->tiles.push_back(std::move(bt));
    }
    return b;
}

void adopt(NodeId set, const std::string& buffer, const Buffer& data)
{
    if (Layer* l = findLayer({set, buffer}))
    {
        l->applied = strokeIds(data);
        l->baseline = data.baseline;
        l->clears = data.clears;
    }
}

image::Image readChannel(NodeId set, const std::string& buffer, const std::string& channel, int udim)
{
    Layer* l = findLayer({set, buffer});
    if (!l || !l->sizes.count(udim))
        return {};
    const auto [w, h] = l->sizes.at(udim);
    image::Image img{w, h, std::vector<float>(size_t(w) * h * 4, 0.0f)};
    auto it = l->tiles.find({channel, udim});
    if (it == l->tiles.end())
        return img;
    std::vector<uint32_t> pages, slots;
    for (auto& [p, s] : it->second.pages)
    {
        pages.push_back(p);
        slots.push_back(s);
    }
    std::vector<std::vector<uint8_t>> texels;
    gpuRun([&](VulkanContext& vk, Gpu& g) { texels = readSlots(vk, g, slots); });
    const int pagesX = (w + kPageSize - 1) / kPageSize;
    for (size_t i = 0; i < pages.size(); ++i)
    {
        const int x0 = int(pages[i] % uint32_t(pagesX)) * kPageSize, y0 = int(pages[i] / uint32_t(pagesX)) * kPageSize;
        const auto* t = reinterpret_cast<const uint16_t*>(texels[i].data());
        for (int y = 0; y < kPageSize && y0 + y < h; ++y)
            for (int x = 0; x < kPageSize && x0 + x < w; ++x)
                for (int c = 0; c < 4; ++c)
                    img.rgba[(size_t(y0 + y) * w + (x0 + x)) * 4 + c] = halfToFloat(t[(size_t(y) * kPageSize + x) * 4 + c]);
    }
    return img;
}

std::map<std::string, std::vector<uint8_t>> pageBytes(NodeId set, const std::string& buffer)
{
    std::map<std::string, std::vector<uint8_t>> out;
    Layer* l = findLayer({set, buffer});
    if (!l)
        return out;
    std::vector<std::string> names;
    std::vector<uint32_t> slots;
    for (auto& [tk, t] : l->tiles)
        for (auto& [p, s] : t.pages)
        {
            names.push_back(tk.first + "/" + std::to_string(tk.second) + "/" + std::to_string(p));
            slots.push_back(s);
        }
    std::vector<std::vector<uint8_t>> texels;
    gpuRun([&](VulkanContext& vk, Gpu& g) { texels = readSlots(vk, g, slots); });
    for (size_t i = 0; i < names.size(); ++i)
        out[names[i]] = std::move(texels[i]);
    return out;
}

json bufferInfo(NodeId set, const std::string& buffer)
{
    Layer* l = findLayer({set, buffer});
    if (!l)
        return {{"pages", 0}, {"channels", json::object()}, {"strokesApplied", 0}, {"live", false}};
    json channels = json::object();
    size_t pages = 0;
    for (auto& [tk, t] : l->tiles)
    {
        if (t.pages.empty())
            continue;
        channels[tk.first][std::to_string(tk.second)] = t.pages.size();
        pages += t.pages.size();
    }
    json sizes = json::object();
    for (auto& [udim, s] : l->sizes)
        sizes[std::to_string(udim)] = {s.first, s.second};
    return {{"pages", pages}, {"channels", channels}, {"sizes", sizes}, {"strokesApplied", l->applied.size()}, {"live", bool(l->live)},
            {"bytes", pages * kHalfPage}};
}

size_t pagesInUse()
{
    size_t n = 0;
    for (auto& [k, l] : gLayers)
        if (l->epoch == gEpoch.load())
            for (auto& [tk, t] : l->tiles)
                n += t.pages.size();
    return n;
}

PageView pageView(NodeId set, const std::string& buffer, const std::string& channel, int udim)
{
    if (auto it = gViews.find({set, buffer, channel, udim}); it != gViews.end())
        return {it->second.table.address, it->second.width, it->second.height};
    if (auto it = gViewSizes.find({set, buffer, udim}); it != gViewSizes.end())
        return {0, it->second.first, it->second.second};
    return {};
}

void recordGather(VulkanContext& vk, VkCommandBuffer cmd, const PageView& view, int levelWidth, int levelHeight, const std::array<int, 4>& rect,
                  VkDeviceAddress dst, uint32_t dstStride)
{
    const int w = rect[2] - rect[0], h = rect[3] - rect[1];
    if (w <= 0 || h <= 0 || !view.table)
        return;
    Gpu& g = gpu();
    g.init(vk);
    const uint64_t items = uint64_t(w) * uint64_t(h);
    const uint64_t groups = std::max<uint64_t>((items + 63) / 64, 1);
    const uint32_t gx = uint32_t(std::min<uint64_t>(groups, 65535));
    GatherPush push{view.table,
                    dst,
                    rect[0],
                    rect[1],
                    uint32_t(w),
                    uint32_t(h),
                    uint32_t(view.width),
                    uint32_t(view.height),
                    uint32_t(std::max(levelWidth, 1)),
                    uint32_t(std::max(levelHeight, 1)),
                    uint32_t((view.width + kPageSize - 1) / kPageSize),
                    dstStride,
                    gx,
                    0};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipeline(vk, g.gather, "paintGatherCS"));
    vkCmdPushConstants(cmd, g.gather.layout, kCompute, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, gx, uint32_t((groups + gx - 1) / gx), 1);
}

std::map<int, std::array<int, 4>> takeChanges(NodeId set)
{
    std::map<int, std::array<int, 4>> out;
    for (auto& [k, l] : gLayers)
    {
        if (k.set != set)
            continue;
        for (auto& [udim, r] : l->changed)
        {
            auto [it, fresh] = out.try_emplace(udim, r);
            if (!fresh)
                it->second = {std::min(it->second[0], r[0]), std::min(it->second[1], r[1]), std::max(it->second[2], r[2]), std::max(it->second[3], r[3])};
        }
        l->changed.clear();
    }
    return out;
}

GpuImageRef externalImage(NodeId set, const std::string& name, int udim)
{
    std::string buffer, channel;
    if (!parseExternalName(name, &buffer, &channel))
        return nullptr;
    std::scoped_lock lock(gImageMutex);
    auto& e = gImages[{set, buffer, channel, udim}];
    if (e.image)
        return e.image;
    auto size = gImageSizes.find({set, buffer, udim});
    if (size == gImageSizes.end())
    {
        // Not rasterised yet: a blank stand-in until the main thread has synced the set (which re-evaluates it).
        gImages.erase({set, buffer, channel, udim});
        mainq::post([set] {
            if (gCommandsRegistered)
                queueSync(set);
        });
        GpuImageRef standIn = GpuImage::create({1, 1, TextureFormat::RGBA16F, false, false, name});
        // A channel never painted covers nothing; a mask never painted masks nothing.
        if (channel == kMaskChannel)
        {
            const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            standIn->updateRgba(0, 0, 1, 1, white);
        }
        return standIn;
    }
    e = {GpuImage::create({size->second.first, size->second.second, TextureFormat::RGBA16F, false, false, name}), true};
    // The new image starts blank: fill it from the pages, then have the graph read it again.
    mainq::post([set, buffer] {
        Layer* l = findLayer({set, buffer});
        if (!l)
            return;
        try
        {
            gpuRun([&](VulkanContext& vk, Gpu& g) { resolve(vk, g, {set, buffer}, *l); });
        }
        catch (const std::exception& ex)
        {
            log::warn("paint buffer {}: {}", buffer, ex.what());
            return;
        }
        notify(set);
    });
    return e.image;
}

namespace
{
/** Dispatch shape for one thread per item over 64-wide groups laid out in 2D. */
std::pair<uint32_t, uint32_t> linearGroups(uint64_t items)
{
    const uint64_t groups = std::max<uint64_t>((items + 63) / 64, 1);
    const uint32_t gx = uint32_t(std::min<uint64_t>(groups, 65535));
    return {gx, uint32_t((groups + gx - 1) / gx)};
}
} // namespace

image::Image screenUvs(scene::Graph& g, const std::string& meshPath, const StrokeView& view, int width, int height)
{
    LILA_ZONE();
    std::shared_ptr<const PaintMesh> mesh = paintMesh(g, meshPath, "");
    if (!mesh)
        throw std::runtime_error(meshPath + " isn't a mesh with faces");
    width = std::clamp(width, 1, 4096);
    height = std::clamp(height, 1, 4096);
    image::Image out{width, height, std::vector<float>(size_t(width) * size_t(height) * 4, 0.0f)};
    gpuRun([&](VulkanContext& vk, Gpu& gp) {
        const VkDeviceSize px = VkDeviceSize(width) * VkDeviceSize(height);
        compute::Buffer depth = compute::makeBuffer(vk, px * 4, 0, compute::Host::None);
        compute::Buffer vis = compute::makeBuffer(vk, px * 4, 0, compute::Host::None);
        compute::Buffer uv = compute::makeBuffer(vk, px * 16, 0, compute::Host::None);
        compute::Buffer host = compute::makeBuffer(vk, px * 16, 0, compute::Host::Read);
        const TriBuffer& tb = gp.triangles(vk, mesh);
        const uint32_t tris = uint32_t(mesh->triangleCount());
        const auto [tx, ty] = linearGroups(tris);
        const auto [px0, py0] = linearGroups(px);
        DepthPush dpush{view.objectToClip, tb.buffer.address, depth.address, tris, uint32_t(width), uint32_t(height), tx};
        VisPush vpush{view.objectToClip, tb.buffer.address, depth.address, vis.address, tris, uint32_t(width), uint32_t(height), tx, 0};
        UvPush upush{view.objectToClip, tb.buffer.address, vis.address, uv.address, uint32_t(width), uint32_t(height), px0, 0};
        gp.sub.run(vk, [&](VkCommandBuffer cmd) {
            vkCmdFillBuffer(cmd, depth.buffer, 0, VK_WHOLE_SIZE, 0);
            vkCmdFillBuffer(cmd, vis.buffer, 0, VK_WHOLE_SIZE, 0xFFFFFFFFu);
            compute::memoryBarrier(cmd);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gp.pipeline(vk, gp.depth, "paintDepthCS"));
            vkCmdPushConstants(cmd, gp.depth.layout, kCompute, 0, sizeof(dpush), &dpush);
            vkCmdDispatch(cmd, tx, ty, 1);
            compute::memoryBarrier(cmd);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gp.pipeline(vk, gp.vis, "paintVisCS"));
            vkCmdPushConstants(cmd, gp.vis.layout, kCompute, 0, sizeof(vpush), &vpush);
            vkCmdDispatch(cmd, tx, ty, 1);
            compute::memoryBarrier(cmd);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gp.pipeline(vk, gp.uvmap, "paintUvCS"));
            vkCmdPushConstants(cmd, gp.uvmap.layout, kCompute, 0, sizeof(upush), &upush);
            vkCmdDispatch(cmd, px0, py0, 1);
            compute::memoryBarrier(cmd);
            compute::copy(cmd, uv, host, px * 16);
        });
        vmaInvalidateAllocation(vk.allocator, host.alloc, 0, VK_WHOLE_SIZE);
        std::memcpy(out.rgba.data(), host.mapped, size_t(px) * 16);
        for (compute::Buffer* b : {&depth, &vis, &uv, &host})
            compute::destroy(vk, *b);
    });
    return out;
}

std::vector<uint8_t> viewImage(const GpuImageRef& img, const glm::vec4& uvRect, int width, int height, bool colorDisplay)
{
    LILA_ZONE();
    width = std::clamp(width, 1, 8192);
    height = std::clamp(height, 1, 8192);
    std::vector<uint8_t> out(size_t(width) * size_t(height) * 4, 0);
    if (!img)
        return out;
    gpuRun([&](VulkanContext& vk, Gpu& gp) {
        std::vector<compute::Buffer> temps;
        const VkDeviceSize px = VkDeviceSize(width) * VkDeviceSize(height);
        const uint64_t texels = uint64_t(img->width()) * uint64_t(img->height());
        compute::Buffer raw = compute::makeBuffer(vk, (texels * uint64_t(texelBytes(img->format())) + 3) & ~uint64_t(3), 0, compute::Host::None);
        compute::Buffer dst = compute::makeBuffer(vk, px * 4, 0, compute::Host::None);
        compute::Buffer host = compute::makeBuffer(vk, px * 4, 0, compute::Host::Read);
        const auto [gx, gy] = linearGroups(px);
        ViewPush push{raw.address,
                      dst.address,
                      uvRect,
                      uint32_t(img->width()),
                      uint32_t(img->height()),
                      uint32_t(img->format()),
                      usesSrgb(img->format(), img->desc().srgb) ? 1u : 0u,
                      uint32_t(width),
                      uint32_t(height),
                      gx,
                      colorDisplay ? 1u : 0u};
        bool ok = false;
        gp.sub.run(vk, [&](VkCommandBuffer cmd) {
            ok = img->prepare(vk, cmd, temps) && img->realized();
            if (ok)
                img->recordRead(cmd, raw);
            else
                vkCmdFillBuffer(cmd, raw.buffer, 0, VK_WHOLE_SIZE, 0);
            compute::memoryBarrier(cmd);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gp.pipeline(vk, gp.view, "paintViewCS"));
            vkCmdPushConstants(cmd, gp.view.layout, kCompute, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, gx, gy, 1);
            compute::memoryBarrier(cmd);
            compute::copy(cmd, dst, host, px * 4);
        });
        vmaInvalidateAllocation(vk.allocator, host.alloc, 0, VK_WHOLE_SIZE);
        std::memcpy(out.data(), host.mapped, out.size());
        for (compute::Buffer* b : {&raw, &dst, &host})
            compute::destroy(vk, *b);
        for (compute::Buffer& b : temps)
            compute::destroy(vk, b);
    });
    return out;
}

GpuImageRef channelImage(NodeId set, const std::string& buffer, const std::string& channel, int udim)
{
    if (!findLayer({set, buffer}))
        return nullptr;
    return externalImage(set, externalName(buffer, channel), udim);
}

int addChangeListener(std::function<void(NodeId)> fn)
{
    const int id = gNextListener++;
    gListeners[id] = std::move(fn);
    return id;
}

void removeChangeListener(int id) { gListeners.erase(id); }

void shutdown()
{
    std::vector<Key> keys;
    for (auto& [k, l] : gLayers)
        keys.push_back(k);
    for (const Key& k : keys)
        dropLayer(k);
    gMeshes.clear();
    if (Renderer::get().running() && !gSurfaces.empty())
        gpuRun([](VulkanContext& vk, Gpu& g) {
            for (auto& [k, s] : gSurfaces)
                for (auto& [p, slot] : s->geo)
                    g.wide.release(slot);
            gSurfaces.clear();
            for (auto& [k, t] : g.tris)
                compute::destroy(vk, t.buffer);
            g.tris.clear();
        });
    gSurfaces.clear();
}

void installSync()
{
    static int listener = 0;
    if (!listener)
        listener = scene::Graph::current().addListener(onSceneChange);
    gCommandsRegistered = true;
}
} // namespace lila::render::paint
