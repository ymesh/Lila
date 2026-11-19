#include "render/Compute.h"

#include "core/Paths.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace lila::render::compute
{
Buffer makeBuffer(VulkanContext& vk, VkDeviceSize size, VkBufferUsageFlags usage, Host host)
{
    Buffer b;
    b.size = std::max<VkDeviceSize>(size, 16);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = b.size;
    bci.usage = usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = host == Host::None ? VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE : VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    if (host == Host::Write)
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    else if (host == Host::Read)
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (VkResult r = vmaCreateBuffer(vk.allocator, &bci, &aci, &b.buffer, &b.alloc, &info); r != VK_SUCCESS)
        throw std::runtime_error(std::string("GPU: could not allocate ") + std::to_string(size >> 20) + " MB (" + vkResultName(r) + ")");
    b.mapped = info.pMappedData;
    VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, b.buffer};
    b.address = vkGetBufferDeviceAddress(vk.device, &ai);
    return b;
}

Buffer hostBuffer(VulkanContext& vk, const void* data, size_t size, VkBufferUsageFlags usage)
{
    Buffer b = makeBuffer(vk, size, usage, Host::Write);
    if (size)
        std::memcpy(b.mapped, data, size);
    vmaFlushAllocation(vk.allocator, b.alloc, 0, VK_WHOLE_SIZE);
    return b;
}

void destroy(VulkanContext& vk, Buffer& b)
{
    if (b.buffer)
        vmaDestroyBuffer(vk.allocator, b.buffer, b.alloc);
    b = {};
}

void invalidate(VulkanContext& vk, const Buffer& b)
{
    if (b.alloc)
        vmaInvalidateAllocation(vk.allocator, b.alloc, 0, VK_WHOLE_SIZE);
}

void copy(VkCommandBuffer cmd, const Buffer& from, const Buffer& to, VkDeviceSize size)
{
    if (!size)
        return;
    VkBufferCopy region{0, 0, size};
    vkCmdCopyBuffer(cmd, from.buffer, to.buffer, 1, &region);
}

void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = dstStage;
    mb.dstAccessMask = dstAccess;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
}

std::vector<uint32_t> loadSpirv(const char* name, std::string* err)
{
    const auto path = paths::executableDir() / "shaders" / name;
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
    {
        if (err)
            *err = "missing shader " + paths::toUtf8(path);
        return {};
    }
    const size_t size = size_t(in.tellg());
    std::vector<uint32_t> code(size / 4);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(code.data()), std::streamsize(size));
    return code;
}

uint64_t spirvHash(std::span<const uint32_t> code)
{
    uint64_t h = 1469598103934665603ull;
    for (uint32_t w : code)
    {
        h ^= w;
        h *= 1099511628211ull;
    }
    return h ^ code.size();
}

VkShaderModule shaderModule(VulkanContext& vk, std::span<const uint32_t> code)
{
    if (code.empty())
        return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size() * 4;
    ci.pCode = code.data();
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(vk.device, &ci, nullptr, &m) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return m;
}

VkPipeline computePipeline(VulkanContext& vk, VkPipelineLayout layout, VkShaderModule module, const char* entry)
{
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, entry};
    ci.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (VkResult r = vkCreateComputePipelines(vk.device, vk.pipelineCache, 1, &ci, nullptr, &p); r != VK_SUCCESS)
    {
        vk.pipelineFailed(entry, r);
        return VK_NULL_HANDLE;
    }
    return p;
}

VkPipelineLayout pipelineLayout(VulkanContext& vk, uint32_t pushBytes, VkShaderStageFlags stages, std::span<const VkDescriptorSetLayout> sets)
{
    VkPushConstantRange range{stages, 0, pushBytes};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.pushConstantRangeCount = pushBytes ? 1 : 0;
    lci.pPushConstantRanges = &range;
    lci.setLayoutCount = uint32_t(sets.size());
    lci.pSetLayouts = sets.data();
    VkPipelineLayout l = VK_NULL_HANDLE;
    vkCreatePipelineLayout(vk.device, &lci, nullptr, &l);
    return l;
}

void destroy(VulkanContext& vk, VkPipelineLayout layout)
{
    if (layout)
        vkDestroyPipelineLayout(vk.device, layout, nullptr);
}

VkPipeline PipelineCache::get(VulkanContext& vk, VkPipelineLayout layout, uint64_t layoutKey, std::span<const uint32_t> code, uint64_t hash,
                              const char* entry)
{
    auto key = std::make_tuple(hash, std::string(entry), layoutKey);
    VkShaderModule mod = VK_NULL_HANDLE;
    {
        std::scoped_lock lock(mMutex);
        if (auto it = mPipelines.find(key); it != mPipelines.end())
        {
            ++mHits;
            return it->second;
        }
        if (auto it = mModules.find(hash); it != mModules.end())
            mod = it->second;
    }
    ++mMisses;
    LILA_ZONE_N("compute pipeline create");
    if (!mod)
    {
        VkShaderModule made = shaderModule(vk, code);
        if (!made)
            throw std::runtime_error(std::string("GPU: bad shader module for ") + entry);
        std::scoped_lock lock(mMutex);
        auto [it, added] = mModules.try_emplace(hash, made);
        if (!added)
            vkDestroyShaderModule(vk.device, made, nullptr);
        mod = it->second;
    }
    VkPipeline p = computePipeline(vk, layout, mod, entry);
    if (!p)
        throw std::runtime_error(std::string("GPU: compute pipeline creation failed (") + entry + ")");
    std::scoped_lock lock(mMutex);
    // Another thread may have made the same pipeline meanwhile; keep the first.
    auto [it, added] = mPipelines.try_emplace(std::move(key), p);
    if (!added)
        vkDestroyPipeline(vk.device, p, nullptr);
    return it->second;
}

bool PipelineCache::contains(uint64_t layoutKey, uint64_t codeHash, const char* entry) const
{
    std::scoped_lock lock(mMutex);
    return mPipelines.contains(std::make_tuple(codeHash, std::string(entry), layoutKey));
}

void PipelineCache::clear(VulkanContext& vk)
{
    std::scoped_lock lock(mMutex);
    for (auto& [k, p] : mPipelines)
        vkDestroyPipeline(vk.device, p, nullptr);
    for (auto& [k, m] : mModules)
        vkDestroyShaderModule(vk.device, m, nullptr);
    mPipelines.clear();
    mModules.clear();
}

PipelineCache& sharedPipelines()
{
    static PipelineCache cache;
    return cache;
}

std::shared_mutex& offThreadDeviceUse()
{
    static std::shared_mutex m;
    return m;
}

uint64_t layoutKey(uint32_t pushBytes, VkShaderStageFlags stages, std::string_view setsTag)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i)
        {
            h ^= (v >> (i * 8)) & 0xffu;
            h *= 1099511628211ull;
        }
    };
    mix(pushBytes);
    mix(stages);
    for (char c : setsTag)
        mix(uint8_t(c));
    return h;
}

size_t sliceFit(size_t n, double perItemMs, double usedMs, double sliceMs, size_t minItems)
{
    if (n == 0 || perItemMs <= 0.0)
        return n;
    const double room = sliceMs - usedMs;
    const size_t fit = room > 0.0 ? size_t(room / perItemMs) : 0;
    if (fit >= n)
        return n;
    // A slice already holding work ends here; an empty one takes at least a few thousand items.
    if (usedMs > 0.0 && fit < std::min(n, minItems))
        return 0;
    return std::max(fit, std::min(n, minItems));
}

double adaptRate(double rate, double predictedMs, double measuredMs)
{
    const double f = std::clamp(predictedMs / std::max(measuredMs, 1e-6), 0.25, 4.0);
    return rate * (0.5 + 0.5 * f);
}

void Submitter::init(VulkanContext& vk)
{
    if (pool)
        return;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = vk.queueFamily;
    vkCreateCommandPool(vk.device, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(vk.device, &ai, &cmd);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(vk.device, &fci, nullptr, &fence);
    if (vk.properties.limits.timestampComputeAndGraphics)
    {
        VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = 2;
        vkCreateQueryPool(vk.device, &qci, nullptr, &queries);
    }
}

void Submitter::destroy(VulkanContext& vk)
{
    if (mInFlight)
        vkWaitForFences(vk.device, 1, &fence, VK_TRUE, 30'000'000'000ull);
    if (queries)
        vkDestroyQueryPool(vk.device, queries, nullptr);
    if (fence)
        vkDestroyFence(vk.device, fence, nullptr);
    if (pool)
        vkDestroyCommandPool(vk.device, pool, nullptr);
    *this = {};
}

void Submitter::begin(VulkanContext& vk)
{
    if (mInFlight)
        poll(vk, 30'000'000'000ull);
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    memoryBarrier(cmd);
    if (queries)
    {
        vkCmdResetQueryPool(cmd, queries, 0, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 0);
    }
}

double Submitter::submit(VulkanContext& vk, bool wait)
{
    if (queries)
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 1);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    mSubmitted = std::chrono::steady_clock::now();
    if (VkResult r = vkQueueSubmit(vk.queue, 1, &si, fence); r != VK_SUCCESS)
        throw std::runtime_error(std::string("GPU: submit failed (") + vkResultName(r) + ")");
    mInFlight = true;
    if (!wait)
        return 0.0;
    const VkResult r = vkWaitForFences(vk.device, 1, &fence, VK_TRUE, 30'000'000'000ull);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mSubmitted).count();
    finished(vk, r, ms);
    return ms;
}

bool Submitter::poll(VulkanContext& vk, uint64_t timeoutNs)
{
    if (!mInFlight)
        return true;
    const VkResult r = vkWaitForFences(vk.device, 1, &fence, VK_TRUE, timeoutNs);
    if (r == VK_TIMEOUT && timeoutNs < 30'000'000'000ull)
        return false;
    finished(vk, r, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mSubmitted).count());
    return true;
}

void Submitter::waitLaunched(VulkanContext& vk) const
{
    if (fence && mInFlight)
        vkWaitForFences(vk.device, 1, &fence, VK_TRUE, 30'000'000'000ull);
}

void Submitter::finished(VulkanContext& vk, VkResult waited, double wallMs)
{
    mInFlight = false;
    vkResetFences(vk.device, 1, &fence);
    if (waited != VK_SUCCESS)
        throw std::runtime_error(std::string("GPU: the GPU did not finish (") + vkResultName(waited) + ")");
    mGpuMs = wallMs;
    uint64_t stamps[2]{};
    if (queries && vkGetQueryPoolResults(vk.device, queries, 0, 2, sizeof(stamps), stamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
        stamps[1] >= stamps[0])
        mGpuMs = double(stamps[1] - stamps[0]) * double(vk.properties.limits.timestampPeriod) * 1e-6;
}
} // namespace lila::render::compute
