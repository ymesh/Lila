#include "render/PathTracer.h"

#include "core/Log.h"
#include "render/Environment.h"
#include "render/GpuImage.h"
#include "render/VulkanContext.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace lila::render
{
namespace
{
constexpr uint32_t kRing = 3; // more than the frames in flight, so a slot's buffers and set are free again when it comes round
constexpr VkFormat kSumFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

struct StorageImage
{
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

void tracerBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

uint64_t fnv(uint64_t h, const void* data, size_t bytes)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
} // namespace

std::vector<float> environmentTable(const EnvironmentData& env, int& res)
{
    res = 0;
    if (env.size <= 0 || env.mips.empty())
        return {};
    size_t mip = 0;
    while (mip + 1 < env.mips.size() && (env.size >> mip) > 32)
        ++mip;
    res = std::max(env.size >> mip, 1);
    const size_t perFace = size_t(res) * size_t(res);
    const std::vector<uint16_t>& texels = env.mips[mip];
    if (texels.size() < perFace * 6 * 4)
    {
        res = 0;
        return {};
    }
    const size_t cells = perFace * 6;
    std::vector<float> out(cells * 2);
    double total = 0.0;
    std::vector<double> weight(cells);
    for (size_t c = 0; c < cells; ++c)
    {
        const size_t within = c % perFace;
        const float s = (float(within % size_t(res)) + 0.5f) / float(res) * 2.0f - 1.0f;
        const float t = (float(within / size_t(res)) + 0.5f) / float(res) * 2.0f - 1.0f;
        const float j = 1.0f + s * s + t * t;
        const float solidAngle = 1.0f / (j * std::sqrt(j));
        const float r = glm::unpackHalf1x16(texels[c * 4]), g = glm::unpackHalf1x16(texels[c * 4 + 1]), b = glm::unpackHalf1x16(texels[c * 4 + 2]);
        const float luma = std::max(0.2126f * r + 0.7152f * g + 0.0722f * b, 0.0f);
        // A floor keeps every direction reachable, so the estimate stays unbiased where the table says dark.
        weight[c] = double(luma + 1e-4f) * double(solidAngle);
        total += weight[c];
    }
    if (!(total > 0.0))
    {
        res = 0;
        return {};
    }
    double run = 0.0;
    for (size_t c = 0; c < cells; ++c)
    {
        run += weight[c] / total;
        out[c] = float(run);
        out[cells + c] = float(weight[c] / total);
    }
    out[cells - 1] = 1.0f;
    return out;
}

struct PathTracer::View
{
    VkExtent2D extent{};
    StorageImage sum, albedo, normal;
    GpuImageRef color, depth;
    TexturePtr colorTexture, depthTexture;
    std::array<compute::Buffer, kRing> params, instances, sections;
    std::array<VkDescriptorSet, kRing> sets{};
    uint32_t ring = 0;
    uint32_t samples = 0;
    uint64_t key = 0;
    bool fresh = true; // the sum images haven't left UNDEFINED yet
    float exposure = 1.0f;
    compute::Buffer readback;
};

struct PathTracer::Impl
{
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline trace = VK_NULL_HANDLE, resolve = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    compute::Buffer envTable;
    uint32_t envCells = 0, envRes = 0;
    uint64_t envGeneration = 0;
    bool tried = false;

    void destroyImage(VulkanContext& vk, StorageImage& i)
    {
        if (i.view)
            vkDestroyImageView(vk.device, i.view, nullptr);
        if (i.image)
            vmaDestroyImage(vk.allocator, i.image, i.alloc);
        i = {};
    }

    StorageImage makeImage(VulkanContext& vk, VkExtent2D extent)
    {
        StorageImage out;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = kSumFormat;
        ici.extent = {extent.width, extent.height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateImage(vk.allocator, &ici, &aci, &out.image, &out.alloc, nullptr) != VK_SUCCESS)
            return {};
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = out.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = kSumFormat;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(vk.device, &vci, nullptr, &out.view);
        return out;
    }

    void releaseImages(VulkanContext& vk, View& v)
    {
        destroyImage(vk, v.sum);
        destroyImage(vk, v.albedo);
        destroyImage(vk, v.normal);
        v.color.reset();
        v.depth.reset();
        v.colorTexture.reset();
        v.depthTexture.reset();
        compute::destroy(vk, v.readback);
        v.extent = {};
    }
};

PathTracer::PathTracer() : m(std::make_unique<Impl>()) {}
PathTracer::~PathTracer() = default;

bool PathTracer::ready() const { return m->trace && m->resolve; }

bool PathTracer::init(VulkanContext& vk, VkDescriptorSetLayout bindlessLayout)
{
    if (ready())
        return true;
    if (m->tried || !vk.rayQuery || !bindlessLayout)
        return false;
    m->tried = true;
    const std::vector<uint32_t> code = compute::loadSpirv("pathtrace.spv");
    if (code.empty())
    {
        log::warn("render: pathtrace.spv is missing; the path-traced view is off");
        return false;
    }
    VkDescriptorSetLayoutBinding b[7]{};
    for (uint32_t i = 0; i < 7; ++i)
    {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                              : i == 1 ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
                                       : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 7;
    lci.pBindings = b;
    vkCreateDescriptorSetLayout(vk.device, &lci, nullptr, &m->setLayout);
    const VkDescriptorSetLayout sets[2] = {m->setLayout, bindlessLayout};
    m->layout = compute::pipelineLayout(vk, sizeof(uint64_t), VK_SHADER_STAGE_COMPUTE_BIT, sets);
    VkShaderModule module = compute::shaderModule(vk, code);
    if (module && m->layout)
    {
        m->trace = compute::computePipeline(vk, m->layout, module, "traceCS");
        m->resolve = compute::computePipeline(vk, m->layout, module, "resolveCS");
    }
    if (module)
        vkDestroyShaderModule(vk.device, module, nullptr);
    const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 * kRing},
                                           {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 64 * kRing},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5 * 64 * kRing}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pci.maxSets = 64 * kRing;
    pci.poolSizeCount = 3;
    pci.pPoolSizes = sizes;
    vkCreateDescriptorPool(vk.device, &pci, nullptr, &m->pool);
    if (!ready() || !m->pool)
    {
        log::warn("render: the path tracer's pipelines were rejected; the path-traced view is off");
        destroy(vk);
        m->tried = true;
        return false;
    }
    return true;
}

void PathTracer::destroy(VulkanContext& vk)
{
    for (VkPipeline* p : {&m->trace, &m->resolve})
        if (*p)
        {
            vkDestroyPipeline(vk.device, *p, nullptr);
            *p = VK_NULL_HANDLE;
        }
    if (m->layout)
        compute::destroy(vk, m->layout);
    m->layout = VK_NULL_HANDLE;
    if (m->setLayout)
        vkDestroyDescriptorSetLayout(vk.device, m->setLayout, nullptr);
    m->setLayout = VK_NULL_HANDLE;
    if (m->pool)
        vkDestroyDescriptorPool(vk.device, m->pool, nullptr);
    m->pool = VK_NULL_HANDLE;
    compute::destroy(vk, m->envTable);
    m->envCells = m->envRes = 0;
    m->tried = false;
}

void PathTracer::setEnvironment(VulkanContext& vk, const EnvironmentData& env)
{
    compute::destroy(vk, m->envTable);
    m->envCells = m->envRes = 0;
    ++m->envGeneration;
    int res = 0;
    const std::vector<float> table = environmentTable(env, res);
    if (table.empty())
        return;
    try
    {
        m->envTable = compute::hostBuffer(vk, table.data(), table.size() * sizeof(float));
        m->envRes = uint32_t(res);
        m->envCells = uint32_t(table.size() / 2);
    }
    catch (const std::exception& e)
    {
        log::error("render: {}", e.what());
    }
}

std::shared_ptr<PathTracer::View> PathTracer::makeView() { return std::make_shared<View>(); }

void PathTracer::releaseView(VulkanContext& vk, View& v)
{
    m->releaseImages(vk, v);
    for (uint32_t i = 0; i < kRing; ++i)
    {
        compute::destroy(vk, v.params[i]);
        compute::destroy(vk, v.instances[i]);
        compute::destroy(vk, v.sections[i]);
        if (v.sets[i] && m->pool)
            vkFreeDescriptorSets(vk.device, m->pool, 1, &v.sets[i]);
        v.sets[i] = VK_NULL_HANDLE;
    }
}

bool PathTracer::record(VulkanContext& vk, VkCommandBuffer cmd, View& v, const Inputs& in, std::vector<compute::Buffer>& garbage, Result& out)
{
    if (!ready() || !in.tlas || !in.envView || in.extent.width == 0 || in.extent.height == 0)
        return false;
    // A new size makes new targets; their first use starts the sums over.
    if (v.extent.width != in.extent.width || v.extent.height != in.extent.height || !v.sum.image)
    {
        vkDeviceWaitIdle(vk.device);
        m->releaseImages(vk, v);
        v.sum = m->makeImage(vk, in.extent);
        v.albedo = m->makeImage(vk, in.extent);
        v.normal = m->makeImage(vk, in.extent);
        GpuImageDesc cd;
        cd.width = int(in.extent.width);
        cd.height = int(in.extent.height);
        cd.format = TextureFormat::RGBA16F;
        cd.name = "path traced view";
        v.color = GpuImage::create(cd);
        GpuImageDesc dd = cd;
        dd.format = TextureFormat::R32F;
        dd.name = "path traced depth";
        v.depth = GpuImage::create(dd);
        v.colorTexture = v.color->asTexture();
        v.depthTexture = v.depth->asTexture();
        v.extent = in.extent;
        v.fresh = true;
        v.samples = 0;
        if (!v.sum.image || !v.albedo.image || !v.normal.image)
        {
            m->releaseImages(vk, v);
            return false;
        }
    }
    std::vector<compute::Buffer> staging;
    const bool prepared = v.color->prepare(vk, cmd, staging) && v.depth->prepare(vk, cmd, staging);
    garbage.insert(garbage.end(), staging.begin(), staging.end());
    if (!prepared || !v.color->storageView() || !v.depth->storageView())
        return false;

    // Everything the picture depends on besides the sums themselves.
    uint64_t key = 1469598103934665603ull;
    key = fnv(key, &in.params.invViewProj, sizeof(glm::mat4));
    key = fnv(key, &in.params.eye, sizeof(glm::vec4) * 7);
    key = fnv(key, &in.extent, sizeof(in.extent));
    const FrameState::PathTrace& st = in.settings;
    const int traceSettings[5] = {st.bounces, st.denoise ? 1 : 0, int(st.seed), int(st.clamp * 1000.0f), 0};
    key = fnv(key, traceSettings, sizeof(traceSettings));
    key = fnv(key, &in.signature, sizeof(in.signature));
    key = fnv(key, &m->envGeneration, sizeof(m->envGeneration));
    if (key != v.key)
    {
        v.key = key;
        v.samples = 0;
    }
    const uint32_t target = uint32_t(std::max(st.maxSamples, 1));
    out.color = v.colorTexture;
    out.depth = v.depthTexture;
    if (v.samples >= target && !v.fresh)
    {
        out.samples = v.samples;
        out.converged = true;
        return true;
    }

    const uint32_t slot = v.ring = (v.ring + 1) % kRing;
    auto ensure = [&](compute::Buffer& b, size_t bytes) {
        bytes = std::max<size_t>(bytes, 16);
        if (b.size >= bytes)
            return;
        compute::destroy(vk, b);
        b = compute::makeBuffer(vk, bytes * 2, 0, compute::Host::Write);
    };
    try
    {
        ensure(v.params[slot], sizeof(PtParams));
        ensure(v.instances[slot], in.instances.size_bytes());
        ensure(v.sections[slot], in.sections.size_bytes());
    }
    catch (const std::exception& e)
    {
        log::error("render: {}", e.what());
        return false;
    }
    if (!in.instances.empty())
        std::memcpy(v.instances[slot].mapped, in.instances.data(), in.instances.size_bytes());
    if (!in.sections.empty())
        std::memcpy(v.sections[slot].mapped, in.sections.data(), in.sections.size_bytes());
    vmaFlushAllocation(vk.allocator, v.instances[slot].alloc, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(vk.allocator, v.sections[slot].alloc, 0, VK_WHOLE_SIZE);

    PtParams p = in.params;
    const uint32_t add = std::min(uint32_t(std::clamp(st.samplesPerFrame, 1, 64)), target - std::min(v.samples, target));
    p.size = glm::vec4(float(in.extent.width), float(in.extent.height), 1.0f / float(in.extent.width), 1.0f / float(in.extent.height));
    p.counts = glm::uvec4(v.samples, add, uint32_t(std::clamp(st.bounces, 1, 32)), st.seed);
    p.counts2 = glm::uvec4(in.lightCount, 0u, m->envTable.buffer ? m->envCells : 0u, m->envRes);
    p.flags.x = st.denoise ? 1u : 0u;
    p.flags.w = target;
    p.misc.y = std::max(st.clamp, 0.01f);
    p.instances = v.instances[slot].address;
    p.sections = v.sections[slot].address;
    p.textureTable = in.textureTable;
    p.lights = in.lights;
    p.envTable = m->envTable.address;
    std::memcpy(v.params[slot].mapped, &p, sizeof(p));
    vmaFlushAllocation(vk.allocator, v.params[slot].alloc, 0, VK_WHOLE_SIZE);
    v.exposure = p.camForward.w;

    if (!v.sets[slot])
    {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = m->pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &m->setLayout;
        if (vkAllocateDescriptorSets(vk.device, &ai, &v.sets[slot]) != VK_SUCCESS)
        {
            log::warn("render: out of path tracer descriptor sets");
            return false;
        }
    }
    VkDescriptorImageInfo env{in.envSampler, in.envView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo images[5] = {{VK_NULL_HANDLE, v.sum.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, v.albedo.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, v.normal.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, v.color->storageView(), VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, v.depth->storageView(), VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSetAccelerationStructureKHR as{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    as.accelerationStructureCount = 1;
    as.pAccelerationStructures = &in.tlas;
    VkWriteDescriptorSet w[7]{};
    for (uint32_t i = 0; i < 7; ++i)
    {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = v.sets[slot];
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
    }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &env;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    w[1].pNext = &as;
    for (uint32_t i = 2; i < 7; ++i)
    {
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[i].pImageInfo = &images[i - 2];
    }
    vkUpdateDescriptorSets(vk.device, 7, w, 0, nullptr);

    constexpr auto computeStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    constexpr auto rw = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    for (StorageImage* i : {&v.sum, &v.albedo, &v.normal})
        tracerBarrier(cmd, i->image, v.fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                     computeStage | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, rw | VK_ACCESS_2_TRANSFER_READ_BIT, computeStage, rw);
    v.fresh = false;
    v.color->beginWrite(cmd);
    v.depth->beginWrite(cmd);
    const VkDescriptorSet sets[2] = {v.sets[slot], in.bindlessSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m->layout, 0, 2, sets, 0, nullptr);
    const uint64_t address = v.params[slot].address;
    vkCmdPushConstants(cmd, m->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(address), &address);
    const uint32_t gx = (in.extent.width + 7) / 8, gy = (in.extent.height + 7) / 8;
    if (add > 0)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m->trace);
        vkCmdDispatch(cmd, gx, gy, 1);
        for (StorageImage* i : {&v.sum, &v.albedo, &v.normal})
            tracerBarrier(cmd, i->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, computeStage, rw, computeStage, rw);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m->resolve);
    vkCmdDispatch(cmd, gx, gy, 1);
    v.color->endWrite(cmd);
    v.depth->endWrite(cmd);
    v.samples += add;
    out.samples = v.samples;
    out.converged = v.samples >= target;
    return true;
}

void PathTracer::recordReadback(VulkanContext& vk, VkCommandBuffer cmd, View& v)
{
    if (!v.sum.image)
        return;
    const VkDeviceSize texels = VkDeviceSize(v.extent.width) * v.extent.height;
    try
    {
        if (v.readback.size < texels * 32)
        {
            compute::destroy(vk, v.readback);
            v.readback = compute::makeBuffer(vk, texels * 32, 0, compute::Host::Read);
        }
    }
    catch (const std::exception& e)
    {
        log::error("render: {}", e.what());
        return;
    }
    constexpr auto computeStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    constexpr auto rw = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    int k = 0;
    for (StorageImage* i : {&v.sum, &v.albedo})
    {
        tracerBarrier(cmd, i->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, computeStage, rw, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.bufferOffset = VkDeviceSize(k++) * texels * 16;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {v.extent.width, v.extent.height, 1};
        vkCmdCopyImageToBuffer(cmd, i->image, VK_IMAGE_LAYOUT_GENERAL, v.readback.buffer, 1, &region);
    }
}

std::vector<float> PathTracer::readback(VulkanContext& vk, View& v)
{
    std::vector<float> out;
    if (!v.readback.mapped)
        return out;
    compute::invalidate(vk, v.readback);
    const size_t texels = size_t(v.extent.width) * v.extent.height;
    const auto* sum = static_cast<const float*>(v.readback.mapped);
    const float* cover = sum + texels * 4;
    out.resize(texels * 4);
    for (size_t i = 0; i < texels; ++i)
    {
        const float n = std::max(sum[i * 4 + 3], 1.0f);
        const float covered = cover[i * 4 + 3];
        for (int c = 0; c < 3; ++c)
            out[i * 4 + c] = covered > 0.0f ? sum[i * 4 + c] / covered * v.exposure : 0.0f;
        out[i * 4 + 3] = covered / n;
    }
    return out;
}
} // namespace lila::render
