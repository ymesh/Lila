#pragma once

#include "core/Export.h"
#include "render/Compute.h"
#include "render/GpuImage.h"
#include "render/RenderMesh.h"
#include "render/TextureFormat.h"

#include <atomic>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

/**
 * Virtual texturing: textures far larger than the GPU holds (a 60-tile UDIM set at 8K), shown through a page table
 * with only the pages the viewport samples resident.
 *
 * A Texture is one texture set's maps over its UDIM tiles: several layers (base colour, normal, ORM, emissive) sharing
 * one page table, each page 128x128 texels at some mip level of a tile, plus a 4-texel border so bilinear filtering
 * never reads a neighbour's slot. Pages live in physical spaces, one per tuple of layer formats (R8, RGBA8, sRGB RGBA8,
 * RGBA16F): each layer an atlas of chunk images of 16x16 slots, a slot index valid in every layer of the space. Chunks
 * are added on demand while the space stays under the VRAM budget (memoryBudget's device-local heap budget, or the pref
 * /texture/vtBudgetMB), after which the least recently sampled pages are evicted. Each tile's coarsest page is always
 * resident, so a lookup always finds something.
 *
 * The mesh pass writes the page each sampled pixel wants (one pixel in sixteen, cycling) to a feedback buffer, a compute
 * pass dedupes it, and the next time the frame's fence is waited the requests reach the system. Missing pages are made
 * by the texture's Producer on the render thread (the texture graph evaluating just that region, painted pixels
 * gathered from paint pages) and copied into their slots; meanwhile the shader samples the nearest resident coarser mip,
 * and new pages fade in over a few frames. Painting invalidates the pages it touches, which are redone in place and
 * pinned while the stroke lasts.
 *
 * Main thread or any thread for the public calls; the render thread does the GPU work in frames (Renderer) and tasks.
 */
namespace lila::render::vt
{
inline constexpr int kPageSize = 128;
inline constexpr int kBorder = 4;
inline constexpr int kSlotSize = kPageSize + 2 * kBorder;
/** Slots per side of a chunk image. */
inline constexpr int kChunkSlots = 16;
/** Feedback is written for one pixel in kFeedbackCell x kFeedbackCell. */
inline constexpr int kFeedbackCell = 4;
/** Words at the start of a frame's texture table: feedback address (2), cells per row, jitter. */
inline constexpr uint32_t kTableHeaderWords = 4;
/** A texture table entry that heads a virtual texture map (mesh.slang). */
inline constexpr uint32_t kVtEntry = 0x40000000u;
/** With kVtEntry: the map repeats its one tile over every UV cell, as a plain texture does. */
inline constexpr uint32_t kVtRepeat = 0x20000000u;

struct LayerFormat
{
    TextureFormat format = TextureFormat::RGBA8;
    /** RGBA8 colour data sampled through an sRGB view. */
    bool srgb = false;
    bool operator==(const LayerFormat&) const = default;
};

struct TileDesc
{
    int udim = 1001;
    int width = 0;
    int height = 0;
};

/** A page: a tile (index into the texture's tiles), a mip level and the page's column and row at that level. */
struct PageId
{
    int tile = 0;
    int mip = 0;
    int x = 0;
    int y = 0;
    auto operator<=>(const PageId&) const = default;
};

/** One page for a producer to fill. */
struct Job
{
    PageId page;
    int udim = 1001;
    /** The level's size in texels. */
    int levelWidth = 0;
    int levelHeight = 0;
    /** The slot's first texel in level texels: the page's origin less the border, so it may be negative. */
    int x0 = 0;
    int y0 = 0;
    /** Per layer, where its kSlotSize x kSlotSize texels go, rows top first, tightly packed in the layer's format. */
    std::vector<VkDeviceAddress> layers;
    /** The buffer the layer addresses point into, for producers that copy texels in (offset = address - stagingAddress). */
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceAddress stagingAddress = 0;
};

class Producer
{
public:
    virtual ~Producer() = default;
    /**
     * Render thread, inside a streaming slice: records the jobs' texels (nothing else may be submitted while it records).
     * Returns per job whether it was recorded; the rest stay queued for a later slice. temps are freed once the slice's
     * commands have run.
     */
    virtual std::vector<uint8_t> record(VulkanContext& vk, VkCommandBuffer cmd, std::span<const Job> jobs, std::vector<compute::Buffer>& temps) = 0;
    /**
     * Render thread, before record: a chance to run what the jobs need (graph evaluations, which submit their own work)
     * within about budgetMs. Returns false when none of them can be made yet.
     */
    virtual bool prepare(VulkanContext& vk, std::span<const Job> jobs, double budgetMs)
    {
        (void)vk;
        (void)jobs;
        (void)budgetMs;
        return true;
    }
};

struct TextureStats
{
    size_t resident = 0;
    size_t pinned = 0;
    size_t stale = 0;
    size_t queued = 0;
    /** Pages made since the texture was created, and pages a feedback asked for (unique per frame, summed). */
    size_t produced = 0;
    size_t requested = 0;
    /** Pages the last feedback that reached this texture asked for: [tile, mip, x, y]. */
    std::vector<std::array<int, 4>> lastRequests;
    /** Bytes of physical memory the resident pages take, over all layers. */
    size_t bytes = 0;
};

class LILA_UI_API Texture : public std::enable_shared_from_this<Texture>
{
public:
    /** Throws std::invalid_argument for no layers, no tiles, a tile outside UDIM 1001-1100 or a size outside 1-65536. */
    static std::shared_ptr<Texture> create(std::string name, std::vector<LayerFormat> layers, std::vector<TileDesc> tiles,
                                           std::shared_ptr<Producer> producer);
    ~Texture();
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    const std::string& name() const { return mName; }
    const std::vector<LayerFormat>& layers() const { return mLayers; }
    const std::vector<TileDesc>& tiles() const { return mTiles; }
    /** Mip levels of a tile: down to the first that fits one page. */
    int mips(int tile) const;
    /** A tile's size at a level (at least 1). */
    std::pair<int, int> levelSize(int tile, int mip) const;
    /** Pages per row and column of a tile at a level. */
    std::pair<int, int> pages(int tile, int mip) const;
    /** The tile index of a UDIM, -1 for none. */
    int tileOf(int udim) const;

    /** A material map sampling one layer (a TextureImage whose vt is this). */
    TexturePtr map(int layer);

    /**
     * Pages overlapping texels [x0, x1) x [y0, y1) of a tile's level 0 (and the pages above them) need making again:
     * resident ones stay shown until their new texels land. pin keeps them resident until unpin.
     */
    void invalidate(int udim, const std::array<int, 4>& rect, bool pin = false);
    /** Every page is stale (the graph changed). */
    void invalidateAll();
    /** Bumped by every invalidate: what the pages show changed. */
    uint64_t contentVersion() const { return mContentVersion.load(); }
    void unpin();
    /** Samples outside UDIM 1001 wrap into it (a plain image), instead of reading as black. */
    void setRepeat(bool on) { mRepeat = on; }
    bool repeat() const { return mRepeat; }
    /** Asks for pages as a feedback would (tests, prefetching a view). */
    void request(std::span<const PageId> pages);

    TextureStats stats() const;
    /** Group id the feedback names it by. */
    uint32_t id() const { return mId; }

    struct Impl;
    Impl& impl() { return *m; }

private:
    Texture(std::string name, std::vector<LayerFormat> layers, std::vector<TileDesc> tiles, std::shared_ptr<Producer> producer);
    std::string mName;
    std::atomic<uint64_t> mContentVersion{0};
    std::vector<LayerFormat> mLayers;
    std::vector<TileDesc> mTiles;
    uint32_t mId = 0;
    bool mRepeat = false;
    std::unique_ptr<Impl> m;
};

using TexturePtrVt = std::shared_ptr<Texture>;

struct SystemStats
{
    size_t budgetBytes = 0;
    /** Physical memory allocated (chunks over all spaces and layers), and what resident pages use of it. */
    size_t allocatedBytes = 0;
    size_t residentBytes = 0;
    size_t residentPages = 0;
    size_t chunks = 0;
    size_t spaces = 0;
    size_t textures = 0;
    size_t produced = 0;
    size_t evicted = 0;
    size_t feedbackFrames = 0;
    /** Pages waiting to be made. */
    size_t queued = 0;
};

/** Whether the device can do virtual texturing (bindless, 64-bit integers in shaders, stores from fragment shaders). */
LILA_UI_API bool supported(const VulkanContext& vk);
/** Whether the running renderer can (false when it isn't running). */
LILA_UI_API bool available();

LILA_UI_API SystemStats stats();
LILA_UI_API nlohmann::json statsJson();
/** Bytes the physical spaces may use: /texture/vtBudgetMB when set (> 0), else a quarter of the device-local heap's budget. */
LILA_UI_API size_t budget();
/** Bytes allocated from device-local memory heaps, everything included (the allocator's statistics). */
LILA_UI_API size_t deviceBytesInUse();
/** Overrides the budget (tests); 0 goes back to the default. Spaces shrink by evicting on their next growth. */
LILA_UI_API void setBudget(size_t bytes);
/** Called when pages land or fade in, so viewports redraw. Main thread. */
LILA_UI_API void setRedraw(std::function<void()> fn);
/** Whether nothing is queued or fading. Any thread. */
LILA_UI_API bool idle();
/** Runs the main queue until idle or the timeout (tests). */
LILA_UI_API bool waitIdle(double timeoutMs = 30000.0);

// ---------------------------------------------------------------------------------------------------- renderer side

/** The texture a VT map samples, its layer, null for other textures. */
LILA_UI_API Texture* textureOf(const TextureImage& t);

/** What the renderer writes into its frame table for a VT map: see mesh.slang vtSample. Render thread. */
LILA_UI_API void tableEntry(const TextureImage& t, std::vector<uint32_t>& table, const std::function<uint32_t(const TexturePtr&)>& bindlessSlot);
/** The chunk images a VT map's layer samples, for the renderer to give bindless slots. Render thread. */
LILA_UI_API std::vector<TexturePtr> chunkTextures(const TextureImage& t);

/** Render thread, before a frame's rendering: lands page table changes and fades. Staging buffers go to staging. */
LILA_UI_API void recordFrame(VulkanContext& vk, VkCommandBuffer cmd, std::vector<compute::Buffer>& staging);

/** A frame's feedback: cells written by the mesh pass, deduped into a list the host reads after the fence. Render thread. */
class LILA_UI_API Feedback
{
public:
    /** Sizes the cells for a viewport; false when the device has no VT. */
    bool begin(VulkanContext& vk, VkCommandBuffer cmd, int width, int height, uint64_t frame);
    /** The table header words for the frame: address of the cells, cells per row, jitter. */
    std::array<uint32_t, kTableHeaderWords> header() const;
    /** After the mesh pass: dedupes the cells into the readback list. */
    void end(VulkanContext& vk, VkCommandBuffer cmd);
    /** After the frame's fence: hands the requests to the system. */
    void collect();
    void destroy(VulkanContext& vk);
    bool active() const { return mActive; }

private:
    compute::Buffer mCells, mTable, mList, mHost;
    int mCellsX = 0, mCellsY = 0;
    uint32_t mJitter = 0;
    bool mActive = false, mPending = false;
};

/** Render thread at stop: frees every device resource (textures re-make theirs when the renderer starts again). */
LILA_UI_API void releaseAll(VulkanContext& vk);
} // namespace lila::render::vt
