#pragma once

#include "render/TexEval.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "render/Compute.h"
#include "render/Renderer.h"
#include "tex/Natives.h"
#include "tex/NodeDef.h"
#include "tex/PaintStrokes.h"
#include "tex/Prelude.h"
#include "tex/SlangRt.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <numeric>
#include <set>
#include <stdexcept>

namespace lila::render::texgpu
{
namespace fs = std::filesystem;
using tex::BufferDesc;
using tex::BufferKind;
using tex::Pass;
using tex::PassKind;
using tex::Plan;

constexpr VkDeviceSize kTexel = 16;
/** Texels around a page region that its region externals also hold, for bilinear reads at its edges. */
constexpr VkDeviceSize kPageMargin = 2;
/** Per slice: descriptor sets and pass headers. */
constexpr uint32_t kMaxDispatches = 512;
constexpr VkDeviceSize kHeaderStride = 256;
constexpr VkShaderStageFlags kCompute = VK_SHADER_STAGE_COMPUTE_BIT;

struct PassHeader
{
    float res[4];
    float uv[4];
    /** x: the output is half floats. */
    uint32_t info[4];
};

struct DecodePush
{
    VkDeviceAddress src;
    VkDeviceAddress dst;
    uint32_t width, height, format, srgb, groupsX, dstStride, srcWidth, srcHeight;
    float scale[2];
    float origin[2];
    uint32_t dstHalf, pad0;
};
static_assert(sizeof(DecodePush) == 72);

struct Kernel
{
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::vector<uint32_t> code;
    uint64_t hash = 0;
    uint32_t push = 0;
};

using Code = std::shared_ptr<const std::vector<uint32_t>>;

struct Shared
{
    std::mutex mutex;
    std::map<std::string, NativeGpuFn> natives;
    MeshMapSource meshMaps;
    std::map<int, std::function<void(const std::string&)>> listeners;
    int nextListener = 1;
    std::map<uint64_t, Code> spirv;
    size_t compiles = 0, hits = 0;
    std::vector<std::weak_ptr<Evaluator::State>> live;

    // Render thread.
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout passLayout = VK_NULL_HANDLE;
    std::map<std::string, Kernel> kernels;
};

Shared& shared();
VkPipeline kernel(VulkanContext& vk, const char* file, const char* entry, uint32_t push, VkPipelineLayout& layout);
void ensurePassLayout(VulkanContext& vk);
inline uint32_t formatCode(TextureFormat f) { return uint32_t(f); }
void dispatchLinear(VkCommandBuffer cmd, uint64_t items);
uint32_t groupsX(uint64_t items);
NativeGpuFn findNative(const std::string& type);

/**
 * How a native pass runs: a registered GPU function, the node's Slang kernels (tex/Natives.h), the built-in blur, or its
 * CPU function on read-back buffers (slow, for natives with no GPU form).
 */
enum class Route : uint8_t
{
    None,
    Registered,
    Kernels,
    Builtin,
    Cpu,
};

/** Image parameters by path, kept while the file is unchanged so value edits don't read them again. */
struct LoadedImage
{
    fs::file_time_type time;
    std::shared_ptr<const image::Image> image;
};

/** A region of a buffer or image as fractions of its size: x right, y down (rows), [x0, x1) x [y0, y1). */
struct Rect
{
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;

    static Rect full() { return {0.0f, 0.0f, 1.0f, 1.0f}; }
    static Rect texels(const std::array<int, 4>& r, int w, int h)
    {
        return {float(r[0]) / float(w), float(r[1]) / float(h), float(r[2]) / float(w), float(r[3]) / float(h)};
    }
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    Rect unite(const Rect& o) const
    {
        if (empty())
            return o;
        if (o.empty())
            return *this;
        return {std::min(x0, o.x0), std::min(y0, o.y0), std::max(x1, o.x1), std::max(y1, o.y1)};
    }
    Rect grow(float dx, float dy) const
    {
        if (empty())
            return *this;
        return {std::max(0.0f, x0 - dx), std::max(0.0f, y0 - dy), std::min(1.0f, x1 + dx), std::min(1.0f, y1 + dy)};
    }
    /** Texels [x0, x1) x [y0, y1) of a w x h grid covering it, the ends rounded out to multiples of align (and clamped). */
    std::array<int, 4> cover(int w, int h, int align = 1) const
    {
        auto lo = [&](float f, int n) { return std::clamp(int(std::floor(f * float(n))) / align * align, 0, n); };
        auto hi = [&](float f, int n) { return std::clamp((int(std::ceil(f * float(n))) + align - 1) / align * align, 0, n); };
        return {lo(x0, w), lo(y0, h), hi(x1, w), hi(y1, h)};
    }
};

/** What setPlan hands the render thread. */
struct Prepared
{
    Plan plan;
    Inputs inputs;
    std::vector<StoreTarget> stores;
    /** Per pass; null for natives. And the hash the code is cached by. */
    std::vector<Code> code;
    std::vector<uint64_t> codeHash;
    /** Per buffer: stored as half4 texels (8 bytes) instead of float4. */
    std::vector<uint8_t> half;
    /** Fused passes run as gpuSource rewrote them (else as the plan wrote them, every buffer float4). */
    bool gpuForm = false;
    /** Pages plans: per pass, run only per page; per buffer, an External that comes in per page. */
    std::vector<uint8_t> pagePass, regionExternal;
    /** Per pass: how a native runs, and for kernel natives their schedule and each step's SPIR-V and hash. */
    std::vector<Route> routes;
    std::vector<tex::NativeSchedule> schedules;
    std::vector<std::vector<std::pair<Code, uint64_t>>> stepCode;
    /** Texels the largest kernel native's scratch buffers take (at the end of the arena). */
    VkDeviceSize scratchTexels = 0;
    /** Image buffers by id. */
    std::map<int, std::shared_ptr<const image::Image>> images;
    /** Per buffer: nothing reads or writes it. */
    std::vector<uint8_t> unused;
    /** Every buffer's size, Image and External ones from their sources. */
    std::vector<std::pair<int, int>> sizes;
    /** Per store, the output buffer each channel reads (-1 for a constant). */
    std::vector<std::array<int, 4>> storeSources;
    size_t compiled = 0, cached = 0;
    std::vector<std::string> warnings;
};

struct Evaluator::State
{
    mutable std::mutex mutex;
    std::unique_ptr<Prepared> pending; // guarded
    /** setExternals asked: the current plan's GPU externals from the next step on. */
    std::unique_ptr<std::map<std::string, GpuImageRef>> pendingExternals; // guarded
    Stats stats;                       // guarded
    std::map<std::string, LoadedImage> imageCache; // guarded
    std::atomic<bool> done{true};
    /** refreshExternals asked: compare the GPU externals' versions on the next step. */
    std::atomic<bool> refresh{false};
    /** Texels a ms the loads run at, measured: what spreads a plan's loads over slices. Render thread. */
    double loadRate = 0.2e6;
    /** trim freed the buffers: whatever runs next runs in full. */
    bool trimmed = false;

    // Render thread.
    std::unique_ptr<Prepared> cur;
    compute::Buffer arena, uniforms, uniformStaging, headers, scratch, words, loadRaw;
    std::vector<VkDeviceSize> offset; // texels
    std::vector<float> uniformValues;
    bool uniformsDirty = true;
    std::vector<uint8_t> dirty;      // per pass
    std::vector<uint8_t> load;       // per buffer
    std::vector<uint8_t> storeDirty; // per store
    /** What of each dirty pass, pending load and dirty store has to be redone. */
    std::vector<Rect> passRect, loadRect, storeRect;
    /** What the stores wrote since takeStored last asked. */
    Rect stored;
    /** Per buffer with a pending partial load: the image version it brings the buffer up to (0: whatever is current). */
    std::vector<uint64_t> loadTarget;
    std::vector<uint64_t> loadedVersion; // per buffer: the GPU external's key and version last loaded
    std::vector<std::pair<uint64_t, uint64_t>> loadedImage; // per buffer: that image's key and version, for partial reloads
    /** The arena was made this step: nothing in it can be kept. */
    bool arenaFresh = true;
    /** Buffers share memory over their lifetimes (Inputs::compact): nothing outlives a run, so any change reruns it all. */
    bool packed = false;
    /** The most bytes the arena may take: one storage buffer binding, or Inputs::maxArenaBytes. */
    VkDeviceSize arenaLimit = 0;
    VkDeviceSize arenaTexels = 0;
    bool reallocate = true;
    VkDeviceSize scratchBase = 0;
    VkDeviceSize scratchCapacity = 0;
    /** Per-slice native frames (gU of kernel natives), bump allocated. */
    compute::Buffer frames;
    VkDeviceSize framesUsed = 0;
    /** Pages plans: where the page area starts in the arena and what one page takes (16-byte texels), and the places in it. */
    VkDeviceSize pageBase = 0, pageTexels = 0;
    std::map<int, VkDeviceSize> pageExternalAt, pageOutputAt;
    VkDescriptorPool pagePool = VK_NULL_HANDLE;
    compute::Buffer pageHeaders, pageUniforms;
    /** Texels this slice loaded, computed and stored, for Stats. */
    uint64_t sliceLoaded = 0, sliceComputed = 0, sliceStored = 0;
    /** The next pass is a CPU-route native, run between slices. */
    bool cpuNext = false;
    size_t passIndex = 0;
    /** The next row of the current pass, and the texels [passX0, passX1) x [.., passY1) it computes; set when it starts. */
    int passRow = 0;
    int passX0 = 0, passX1 = 0, passY1 = 0;
    bool passStarted = false, passBands = false;
    double rate = 4e6; // texel * cost units per ms
    VkDescriptorPool pool = VK_NULL_HANDLE;
    compute::Submitter sub;

    void release(VulkanContext& vk)
    {
        freeDevice(vk);
        // Whatever ran is gone with the arena: the next step runs everything again.
        if (cur)
        {
            dirtyAll();
            done = false;
        }
    }

    void freeDevice(VulkanContext& vk)
    {
        for (compute::Buffer* b : {&arena, &uniforms, &uniformStaging, &headers, &scratch, &words, &frames, &loadRaw})
            compute::destroy(vk, *b);
        if (pool)
            vkDestroyDescriptorPool(vk.device, pool, nullptr);
        pool = VK_NULL_HANDLE;
        if (pagePool)
            vkDestroyDescriptorPool(vk.device, pagePool, nullptr);
        pagePool = VK_NULL_HANDLE;
        compute::destroy(vk, pageHeaders);
        compute::destroy(vk, pageUniforms);
        sub.destroy(vk);
    }

    /** Marks every load, pass and store to be redone in full, from the first pass. */
    void dirtyAll()
    {
        std::fill(dirty.begin(), dirty.end(), uint8_t(1));
        std::fill(load.begin(), load.end(), uint8_t(1));
        std::fill(storeDirty.begin(), storeDirty.end(), uint8_t(1));
        std::fill(passRect.begin(), passRect.end(), Rect::full());
        std::fill(loadRect.begin(), loadRect.end(), Rect::full());
        std::fill(storeRect.begin(), storeRect.end(), Rect::full());
        std::fill(loadTarget.begin(), loadTarget.end(), uint64_t(0));
        uniformsDirty = true;
        passIndex = 0;
        passRow = 0;
        passStarted = false;
    }

    /** Pages plans: page passes never run in step, and externals that come in per page never load whole. */
    void maskPages()
    {
        if (!cur || !cur->inputs.pages)
            return;
        for (size_t p = 0; p < dirty.size(); ++p)
            if (cur->pagePass[p])
                dirty[p] = 0;
        for (size_t b = 0; b < load.size(); ++b)
            if (cur->regionExternal[b])
                load[b] = 0;
    }
    /** Bytes per texel of a buffer in the arena. */
    VkDeviceSize elemBytes(size_t b) const { return cur->half[b] ? 8 : kTexel; }
    /** Takes a pending plan and places its buffers, packing them when asked or when they'd pass maxBytes side by side. */
    void adopt(VkDeviceSize maxBytes);
    /** Loads the GPU externals whose version moved and dirties what reads them (render thread). */
    void markExternals();
    /**
     * Marks a GPU external for loading: just what changed since its last load when that is known, else all of it.
     * Returns false when it is up to date.
     */
    bool markExternal(size_t b, const GpuImageRef& img);
    /**
     * Dirties the passes reading pending loads, value slots in changed or (moved) the uv offset, and everything
     * downstream, each over the region its changed inputs reach (all of it unless it reads them at its own uv), then the
     * stores those passes feed. Unions with what was already dirty.
     */
    void propagate(const std::set<int>& changed, bool moved);
    /** Starts a pass: the rows and columns it computes, aligned for row bands. */
    void startPass(VulkanContext& vk, const Pass& p);
    void allocate(VulkanContext& vk);
    /**
     * Records the pending loads that fit the slice's budget (always at least one); false while some wait for the next
     * slice, when nothing that reads them may run yet.
     */
    bool recordLoads(VulkanContext& vk, VkCommandBuffer cmd, std::vector<compute::Buffer>& temps, double budgetMs);
    /** Records passes until the budget is used; returns the predicted ms. */
    double recordPasses(VulkanContext& vk, VkCommandBuffer cmd, double budgetMs, Stats& delta);
    /** Records a kernel native's steps from passRow on; false when the slice filled first. */
    bool recordKernelSteps(VulkanContext& vk, VkCommandBuffer cmd, const Pass& p, double budgetMs, double& used, uint32_t& dispatches, Stats& delta);
    /** Runs a CPU-route native: reads its inputs back, runs it, uploads its output. Returns the ms spent. */
    double runCpuNative(VulkanContext& vk);
    void recordStores(VulkanContext& vk, VkCommandBuffer cmd, std::vector<compute::Buffer>& temps, Stats& delta);
    bool anyWork() const;
};
} // namespace lila::render::texgpu
