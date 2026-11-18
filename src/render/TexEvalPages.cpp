#include "render/TexEval.h"
#include "render/TexEvalInternal.h"

namespace lila::render::texgpu
{
namespace
{
// Vectors first: in push constants they align to 16 bytes.
struct ComposePush
{
    uint32_t srcSize[4];
    uint32_t srcMode[4];
    float constants[4];
    VkDeviceAddress dst;
    VkDeviceAddress src[4];
    uint32_t width, height, format, srgb, groupsX, words;
    float x0, y0, levelW, levelH;
};
static_assert(sizeof(ComposePush) == 128);
} // namespace

bool Evaluator::pagesReady() const
{
    const State& st = *m;
    std::scoped_lock lock(st.mutex);
    return st.cur && st.cur->inputs.pages && !st.pending && st.arena.buffer && !st.anyWork() && !st.arenaFresh && !st.sub.busy();
}

size_t Evaluator::recordPages(VulkanContext& vk, VkCommandBuffer cmd, std::span<const PageJob> jobs, std::span<const PageLayer> layers,
                              const RegionLoader& load, std::vector<compute::Buffer>& temps)
{
    LILA_ZONE();
    State& st = *m;
    if (jobs.empty() || !pagesReady())
        return 0;
    const Prepared& c = *st.cur;
    const Plan& plan = c.plan;
    const size_t n = std::min(jobs.size(), kPagesPerBatch);
    const int side = std::max(c.inputs.pageSize, 8);
    for (size_t j = 0; j < n; ++j)
        if (jobs[j].width > side || jobs[j].height > side || jobs[j].width < 1 || jobs[j].height < 1 || jobs[j].layers.size() != layers.size())
            throw std::runtime_error("texture graph pages: a page doesn't fit the plan's page size or layers");

    // Every layer channel's source: a page pass's output (per page) or a buffer the base passes made (sampled whole).
    struct Source
    {
        int buffer = -1;
        bool page = false;
    };
    std::vector<std::array<Source, 4>> sources(layers.size());
    for (size_t l = 0; l < layers.size(); ++l)
        for (size_t ch = 0; ch < 4; ++ch)
        {
            const std::string& name = layers[l].channels[ch].output;
            if (name.empty())
                continue;
            auto it = plan.outputs.find(name);
            if (it == plan.outputs.end())
                throw std::runtime_error("the plan has no output " + name);
            sources[l][ch] = {it->second, st.pageOutputAt.contains(it->second)};
        }

    ensurePassLayout(vk);
    if (!st.pagePool)
    {
        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxDispatches}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kMaxDispatches}};
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = kMaxDispatches;
        ci.poolSizeCount = 2;
        ci.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(vk.device, &ci, nullptr, &st.pagePool) != VK_SUCCESS)
            throw std::runtime_error("texture graph GPU: cannot create a descriptor pool");
    }
    vkResetDescriptorPool(vk.device, st.pagePool, 0);
    if (!st.pageHeaders.buffer)
        st.pageHeaders = compute::makeBuffer(vk, kHeaderStride * kMaxDispatches, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, compute::Host::Write);

    // Per page uniforms: the plan's, with the region externals' descriptors pointing at the page's regions, each
    // placed by a slot appended after the plan's.
    std::vector<int> regions;
    for (const BufferDesc& b : plan.buffers)
        if (c.regionExternal[size_t(b.id)])
            regions.push_back(b.id);
    const VkDeviceSize align = std::max<VkDeviceSize>(vk.properties.limits.minStorageBufferOffsetAlignment, 16);
    const VkDeviceSize uniformBytes = (st.uniformValues.size() + regions.size() * 4) * sizeof(float);
    const VkDeviceSize uniformStride = (uniformBytes + align - 1) / align * align;
    if (st.pageUniforms.size < uniformStride * n)
    {
        compute::destroy(vk, st.pageUniforms);
        st.pageUniforms = compute::makeBuffer(vk, uniformStride * kPagesPerBatch, 0, compute::Host::Write);
    }
    const int slots = int(st.uniformValues.size() / 4);
    for (size_t j = 0; j < n; ++j)
    {
        const PageJob& job = jobs[j];
        std::vector<float> u = st.uniformValues;
        u.resize(u.size() + regions.size() * 4, 0.0f);
        const VkDeviceSize base = st.pageBase + VkDeviceSize(j) * st.pageTexels;
        for (size_t k = 0; k < regions.size(); ++k)
        {
            const BufferDesc& b = plan.buffers[size_t(regions[k])];
            const uint32_t rw = uint32_t(job.width + 2 * int(kPageMargin)), rh = uint32_t(job.height + 2 * int(kPageMargin));
            const uint32_t d[4] = {uint32_t((base + st.pageExternalAt.at(b.id)) * 2), rw, rh, 2u | (uint32_t(slots + int(k)) << 8)};
            std::memcpy(&u[size_t(b.slot) * 4], d, sizeof(d));
            // The region's first texel in the tile's uv space as texReadBuf sees it (UDIM offset included).
            float* t = &u[size_t(slots + int(k)) * 4];
            t[0] = float(double(c.inputs.uvOffset[0]) * job.levelWidth + job.x0 - int(kPageMargin));
            t[1] = float(-double(c.inputs.uvOffset[1]) * job.levelHeight + job.y0 - int(kPageMargin));
            t[2] = float(job.levelWidth);
            t[3] = float(job.levelHeight);
        }
        std::memcpy(static_cast<uint8_t*>(st.pageUniforms.mapped) + uniformStride * j, u.data(), u.size() * sizeof(float));
    }
    vmaFlushAllocation(vk.allocator, st.pageUniforms.alloc, 0, VK_WHOLE_SIZE);

    // Regions of the externals, then the page passes, then the layers.
    for (size_t j = 0; j < n; ++j)
    {
        const PageJob& job = jobs[j];
        const VkDeviceSize base = st.pageBase + VkDeviceSize(j) * st.pageTexels;
        for (int b : regions)
        {
            const int margin = int(kPageMargin);
            const std::array<int, 4> rect{job.x0 - margin, job.y0 - margin, job.x0 + job.width + margin, job.y0 + job.height + margin};
            const VkDeviceSize at = (base + st.pageExternalAt.at(b)) * kTexel;
            const uint32_t stride = uint32_t(job.width + 2 * margin);
            if (!load || !load(vk, cmd, plan.buffers[size_t(b)].name, job.levelWidth, job.levelHeight, rect, st.arena.address + at, stride))
                vkCmdFillBuffer(cmd, st.arena.buffer, at, VkDeviceSize(stride) * VkDeviceSize(job.height + 2 * margin) * kTexel, 0);
            st.sliceLoaded += uint64_t(stride) * uint64_t(job.height + 2 * margin);
        }
    }
    compute::memoryBarrier(cmd);
    const uint64_t passKey = compute::layoutKey(0, kCompute, "texgraph");
    uint32_t dispatches = 0;
    for (size_t j = 0; j < n; ++j)
    {
        const PageJob& job = jobs[j];
        const VkDeviceSize base = st.pageBase + VkDeviceSize(j) * st.pageTexels;
        for (size_t p = 0; p < plan.passes.size(); ++p)
        {
            if (!c.pagePass[p])
                continue;
            if (dispatches >= kMaxDispatches)
                throw std::runtime_error("texture graph pages: too many passes in one batch");
            const Pass& pass = plan.passes[p];
            VkPipeline pipe = compute::sharedPipelines().get(vk, shared().passLayout, passKey, *c.code[p], c.codeHash[p], pass.entry.c_str());
            const double lw = double(job.levelWidth), lh = double(job.levelHeight);
            PassHeader header{{float(job.width), float(job.height), 1.0f / float(job.width), 1.0f / float(job.height)},
                              {float(double(c.inputs.uvOffset[0]) + double(job.x0) / lw),
                               float(double(c.inputs.uvOffset[1]) + 1.0 - double(job.y0 + job.height) / lh), float(double(job.width) / lw),
                               float(double(job.height) / lh)},
                              {0u, 0u, 0u, 0u}};
            std::memcpy(static_cast<uint8_t*>(st.pageHeaders.mapped) + dispatches * kHeaderStride, &header, sizeof(header));
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = st.pagePool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &shared().setLayout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(vk.device, &ai, &set) != VK_SUCCESS)
                throw std::runtime_error("texture graph GPU: out of descriptor sets");
            const VkDescriptorBufferInfo infos[4] = {
                {st.pageHeaders.buffer, dispatches * kHeaderStride, sizeof(PassHeader)},
                {st.pageUniforms.buffer, uniformStride * j, uniformBytes},
                {st.arena.buffer, 0, VK_WHOLE_SIZE},
                {st.arena.buffer, (base + st.pageOutputAt.at(pass.output)) * kTexel, VkDeviceSize(job.width) * VkDeviceSize(job.height) * kTexel}};
            VkWriteDescriptorSet writes[4]{};
            for (uint32_t i = 0; i < 4; ++i)
            {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = set;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[i].pBufferInfo = &infos[i];
            }
            vkUpdateDescriptorSets(vk.device, 4, writes, 0, nullptr);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shared().passLayout, 0, 1, &set, 0, nullptr);
            vkCmdDispatch(cmd, uint32_t((job.width + 7) / 8), uint32_t((job.height + 7) / 8), 1);
            ++dispatches;
            st.sliceComputed += uint64_t(job.width) * uint64_t(job.height);
        }
    }
    if (dispatches)
        vmaFlushAllocation(vk.allocator, st.pageHeaders.alloc, 0, VK_WHOLE_SIZE);
    compute::memoryBarrier(cmd);

    VkPipelineLayout layout;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel(vk, "tex_page.spv", "texComposeCS", sizeof(ComposePush), layout));
    for (size_t j = 0; j < n; ++j)
    {
        const PageJob& job = jobs[j];
        const VkDeviceSize base = st.pageBase + VkDeviceSize(j) * st.pageTexels;
        for (size_t l = 0; l < layers.size(); ++l)
        {
            ComposePush push{};
            push.dst = job.layers[l];
            for (size_t ch = 0; ch < 4; ++ch)
            {
                const Source& src = sources[l][ch];
                push.constants[ch] = layers[l].channels[ch].constant;
                if (src.buffer < 0)
                    continue;
                const uint32_t comp = uint32_t(std::clamp(layers[l].channels[ch].component, 0, 3));
                if (src.page)
                {
                    push.src[ch] = st.arena.address + (base + st.pageOutputAt.at(src.buffer)) * kTexel;
                    push.srcSize[ch] = uint32_t(job.width) | (uint32_t(job.height) << 16);
                    push.srcMode[ch] = comp;
                }
                else
                {
                    const auto [bw, bh] = c.sizes[size_t(src.buffer)];
                    push.src[ch] = st.arena.address + st.offset[size_t(src.buffer)] * kTexel;
                    push.srcSize[ch] = uint32_t(bw) | (uint32_t(bh) << 16);
                    push.srcMode[ch] = comp | 0x200u | (c.half[size_t(src.buffer)] ? 0x100u : 0u);
                }
            }
            const uint64_t texels = uint64_t(job.width) * uint64_t(job.height);
            const uint64_t words = (texels * uint64_t(texelBytes(layers[l].format)) + 3) / 4;
            push.width = uint32_t(job.width);
            push.height = uint32_t(job.height);
            push.format = formatCode(layers[l].format);
            push.srgb = usesSrgb(layers[l].format, layers[l].srgb) ? 1u : 0u;
            push.groupsX = groupsX(words);
            push.words = uint32_t(words);
            push.x0 = float(job.x0);
            push.y0 = float(job.y0);
            push.levelW = float(job.levelWidth);
            push.levelH = float(job.levelHeight);
            vkCmdPushConstants(cmd, layout, kCompute, 0, sizeof(push), &push);
            dispatchLinear(cmd, words);
            st.sliceStored += texels;
        }
    }
    compute::memoryBarrier(cmd);
    {
        std::scoped_lock lock(st.mutex);
        st.stats.texelsLoaded += st.sliceLoaded;
        st.stats.texelsComputed += st.sliceComputed;
        st.stats.texelsStored += st.sliceStored;
    }
    st.sliceLoaded = st.sliceComputed = st.sliceStored = 0;
    (void)temps;
    return n;
}
} // namespace lila::render::texgpu
