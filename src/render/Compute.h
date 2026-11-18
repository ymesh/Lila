#pragma once

#include "core/Export.h"
#include "core/Profile.h"
#include "render/VulkanContext.h"

#include <chrono>
#include <cstdint>
#include <atomic>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

/**
 * Building blocks for GPU work outside the viewport (the baker, texture graphs, painting): buffers reached through
 * device addresses, shader modules and compute pipelines shared by SPIR-V hash, and slicing work into render-thread
 * tasks that each stay near a time budget. Everything that records or creates runs on the render thread (inside
 * Renderer::runTask) unless it says otherwise.
 */
namespace lila::render::compute
{
struct Buffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceAddress address = 0;
    VkDeviceSize size = 0;
};

enum class Host
{
    None,
    Write,
    Read,
};

/**
 * A storage buffer with a device address that can also be copied to and from, device local (Host::None) or mapped for
 * the host to write or read. Any thread (VMA is thread safe). Throws std::runtime_error when it cannot be allocated.
 */
LILA_UI_API Buffer makeBuffer(VulkanContext& vk, VkDeviceSize size, VkBufferUsageFlags usage, Host host);
/** A host-visible buffer filled from data, for the GPU to read directly (small tables) or to copy from (staging). */
LILA_UI_API Buffer hostBuffer(VulkanContext& vk, const void* data, size_t size, VkBufferUsageFlags usage = 0);
LILA_UI_API void destroy(VulkanContext& vk, Buffer& b);
LILA_UI_API void copy(VkCommandBuffer cmd, const Buffer& from, const Buffer& to, VkDeviceSize size);
/** Makes what the GPU wrote to a Host::Read buffer visible to the host, after its work finished. */
LILA_UI_API void invalidate(VulkanContext& vk, const Buffer& b);
/** Makes every earlier write visible to what follows in dstStage. */
LILA_UI_API void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 dstStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               VkAccessFlags2 dstAccess = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);

/** A compiled shader next to the executable (shaders/<name>); empty with *err set when it is missing. Any thread. */
LILA_UI_API std::vector<uint32_t> loadSpirv(const char* name, std::string* err = nullptr);
/** FNV-1a over the words: equal code, equal hash, on every run. */
LILA_UI_API uint64_t spirvHash(std::span<const uint32_t> code);
/** Null when the driver rejects it. */
LILA_UI_API VkShaderModule shaderModule(VulkanContext& vk, std::span<const uint32_t> code);
/** Null when the driver rejects it. */
LILA_UI_API VkPipeline computePipeline(VulkanContext& vk, VkPipelineLayout layout, VkShaderModule module, const char* entry);
/** One push constant range of pushBytes for stages, and the set layouts in order. */
LILA_UI_API VkPipelineLayout pipelineLayout(VulkanContext& vk, uint32_t pushBytes, VkShaderStageFlags stages,
                                            std::span<const VkDescriptorSetLayout> sets = {});
LILA_UI_API void destroy(VulkanContext& vk, VkPipelineLayout layout);

/**
 * Compute pipelines kept by SPIR-V hash, entry point and a layout key, with their shader modules, so a second user of
 * the same kernel (the next bake, another graph node) creates nothing. Callers give equal layout keys only to layouts
 * that are compatible (same push constant ranges, identically defined set layouts): a cached pipeline is bound with
 * whichever of them the caller has, and the layout it was made with may already be gone. Any thread: the driver's
 * compile runs outside the lock, so a worker can create pipelines ahead of the render thread without stalling it. Off the
 * render thread, hold offThreadDeviceUse shared while calling.
 */
class LILA_UI_API PipelineCache
{
public:
    /** Throws std::runtime_error when the module or pipeline cannot be created. */
    VkPipeline get(VulkanContext& vk, VkPipelineLayout layout, uint64_t layoutKey, std::span<const uint32_t> code, const char* entry)
    {
        return get(vk, layout, layoutKey, code, spirvHash(code), entry);
    }
    /** With the code's spirvHash computed once by the caller. */
    VkPipeline get(VulkanContext& vk, VkPipelineLayout layout, uint64_t layoutKey, std::span<const uint32_t> code, uint64_t codeHash,
                   const char* entry);
    /** Destroys everything; pipelines handed out before are invalid afterwards. */
    void clear(VulkanContext& vk);
    /** Whether get would create nothing. */
    bool contains(uint64_t layoutKey, uint64_t codeHash, const char* entry) const;
    size_t hits() const { return mHits; }
    size_t misses() const { return mMisses; }
    size_t size() const
    {
        std::scoped_lock lock(mMutex);
        return mPipelines.size();
    }

private:
    mutable std::mutex mMutex;
    std::map<uint64_t, VkShaderModule> mModules;
    std::map<std::tuple<uint64_t, std::string, uint64_t>, VkPipeline> mPipelines;
    std::atomic<size_t> mHits = 0, mMisses = 0;
};

/** The process-wide cache, cleared when the renderer stops. */
LILA_UI_API PipelineCache& sharedPipelines();

/**
 * Held shared by threads that create device objects off the render thread (after checking Renderer::context is not
 * null); Renderer::stop holds it exclusively, so the device outlives every such call.
 */
LILA_UI_API std::shared_mutex& offThreadDeviceUse();

/** A layout key from what makes layouts compatible: the push constant size and stages, and a tag naming the set layouts. */
LILA_UI_API uint64_t layoutKey(uint32_t pushBytes, VkShaderStageFlags stages, std::string_view setsTag = {});

/**
 * How many of n remaining items fit in a slice: perItemMs each, usedMs of sliceMs already taken. 0 means the slice is
 * full and the items wait for the next one; an empty slice always takes at least min(n, minItems).
 */
LILA_UI_API size_t sliceFit(size_t n, double perItemMs, double usedMs, double sliceMs, size_t minItems = 4096);
/** A throughput estimate (items per ms) moved toward what a slice measured: predicted ms at that rate, measured ms. */
LILA_UI_API double adaptRate(double rate, double predictedMs, double measuredMs);

/** Dispatches items [first, first + count) of a kernel in chunks under the workgroup limit; set fills a chunk's push. */
template <class Push, class Set>
void dispatchChunks(VkCommandBuffer cmd, VkPipelineLayout layout, Push push, uint32_t first, uint32_t count, uint32_t perGroup, Set&& set)
{
    const uint32_t maxItems = 65535u * perGroup;
    for (uint32_t f = first; f < first + count; f += maxItems)
    {
        const uint32_t n = std::min(maxItems, first + count - f);
        set(push, f, n);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &push);
        vkCmdDispatch(cmd, (n + perGroup - 1) / perGroup, 1, 1);
    }
}

/**
 * Records commands and submits them: run waits for the GPU, launch returns at once and poll finishes it later. A run or
 * launch waits for an unfinished launch first. Render thread only, except waitLaunched.
 */
struct LILA_UI_API Submitter
{
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    /** Two timestamps around each submission; null where the queue has none. */
    VkQueryPool queries = VK_NULL_HANDLE;

    void init(VulkanContext& vk);
    void destroy(VulkanContext& vk);

    /** Returns the wall time from submit to fence, ms. Throws std::runtime_error when the GPU fails or times out. */
    template <class F> double run(VulkanContext& vk, F&& record)
    {
        LILA_ZONE();

        begin(vk);
        record(cmd);
        return submit(vk, true);
    }
    /** Records and submits without waiting for the GPU. */
    template <class F> void launch(VulkanContext& vk, F&& record)
    {
        LILA_ZONE();

        begin(vk);
        record(cmd);
        submit(vk, false);
    }
    /** Whether the last launch finished (true when nothing is in flight), waiting up to timeoutNs. Throws when the GPU failed. */
    bool poll(VulkanContext& vk, uint64_t timeoutNs = 0);
    bool busy() const { return mInFlight; }
    /** Waits for the launch in flight without finishing it (poll still does that); from another thread, only while nothing launches. */
    void waitLaunched(VulkanContext& vk) const;
    /** GPU time of the last finished submission, ms: its timestamps, else the wall time to its fence. */
    double gpuMs() const { return mGpuMs; }

private:
    /** Waits out a launch in flight, resets and begins cmd, with a barrier so earlier slices' writes are visible to this one. */
    void begin(VulkanContext& vk);
    /** Ends with a barrier for host reads and submits; with wait, waits for the GPU and returns the wall time, ms. */
    double submit(VulkanContext& vk, bool wait);
    /** After the fence: resets it and reads the timestamps. */
    void finished(VulkanContext& vk, VkResult waited, double wallMs);

    bool mInFlight = false;
    double mGpuMs = 0.0;
    std::chrono::steady_clock::time_point mSubmitted{};
};
} // namespace lila::render::compute
