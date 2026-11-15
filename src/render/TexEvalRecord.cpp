#include "render/TexEval.h"
#include "render/TexEvalInternal.h"

namespace lila::render::texgpu
{
namespace
{
struct StorePush
{
    VkDeviceAddress arena;
    VkDeviceAddress dst;
    uint32_t src[4][4];
    float constants[4];
    uint32_t width, height, format, srgb, words, groupsX;
    /** The rectangle written: x | y << 16 and width | height << 16. */
    uint32_t origin, extent;
};
static_assert(sizeof(StorePush) == 128);

struct UvPush
{
    VkDeviceAddress dst;
    uint32_t width, height;
    float u0, v0;
    uint32_t half, pad;
};

struct BlurPush
{
    VkDeviceAddress src;
    VkDeviceAddress dst;
    uint32_t srcWidth, srcHeight, width, height;
    float sigma;
    int32_t radius;
    uint32_t vertical, pad0;
};

/** Separable Gaussian, as the CPU's blurCpu: radius in uv (fraction of the width), wrapping. */
void blurGpu(VulkanContext& vk, VkCommandBuffer cmd, const NativeCall& c)
{
    auto it = c.inputs.find("input");
    if (it == c.inputs.end())
        return;
    const NativeBuffer& in = it->second;
    const int w = c.output.width, h = c.output.height;
    const float sigma = std::max(1e-4f, c.param("radius") * float(w) / 3.0f);
    const int r = std::min(std::max(w, h), int(std::ceil(sigma * 3.0f)));
    VkPipelineLayout layout;
    VkPipeline p = kernel(vk, "tex_blur.spv", "texBlurCS", sizeof(BlurPush), layout);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    BlurPush push{in.address, c.scratch, uint32_t(in.width), uint32_t(in.height), uint32_t(w), uint32_t(h), sigma, r, 0, 0};
    vkCmdPushConstants(cmd, layout, kCompute, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, uint32_t((w + 7) / 8), uint32_t((h + 7) / 8), 1);
    compute::memoryBarrier(cmd);
    push = {c.scratch, c.output.address, uint32_t(w), uint32_t(h), uint32_t(w), uint32_t(h), sigma, r, 1, 0};
    vkCmdPushConstants(cmd, layout, kCompute, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, uint32_t((w + 7) / 8), uint32_t((h + 7) / 8), 1);
}
} // namespace

bool Evaluator::State::recordLoads(VulkanContext& vk, VkCommandBuffer cmd, std::vector<compute::Buffer>& temps, double budgetMs)
{
    LILA_ZONE();
    bool any = false;
    // A plan's first slice loading every 4K external at once holds the render thread for hundreds of ms, so loads
    // spread over slices like passes do, at the rate they were measured to run.
    const double maxTexels = std::max(1.0, budgetMs) * loadRate;
    double sliceTexels = 0.0;
    bool deferred = false;
    auto upload = [&](const image::Image& img, size_t b) {
        std::vector<uint16_t> halves;
        if (cur->half[b])
        {
            halves.resize(img.rgba.size());
            for (size_t i = 0; i < halves.size(); ++i)
                halves[i] = tex::paint::floatToHalf(img.rgba[i]);
        }
        const size_t bytes = cur->half[b] ? halves.size() * sizeof(uint16_t) : img.rgba.size() * sizeof(float);
        compute::Buffer staging = compute::hostBuffer(vk, cur->half[b] ? static_cast<const void*>(halves.data()) : img.rgba.data(), bytes);
        temps.push_back(staging);
        VkBufferCopy region{0, offset[b] * kTexel, bytes};
        vkCmdCopyBuffer(cmd, staging.buffer, arena.buffer, 1, &region);
    };
    if (loadedImage.size() != loadedVersion.size())
        loadedImage.assign(loadedVersion.size(), {0, 0});
    auto uvFill = [&](size_t b) {
        VkPipelineLayout layout;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel(vk, "tex_uvfill.spv", "texUvFillCS", sizeof(UvPush), layout));
        const auto [w, h] = cur->sizes[b];
        UvPush push{arena.address + offset[b] * kTexel, uint32_t(w), uint32_t(h), cur->inputs.uvOffset[0], cur->inputs.uvOffset[1], cur->half[b], 0};
        vkCmdPushConstants(cmd, layout, kCompute, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, uint32_t((w + 7) / 8), uint32_t((h + 7) / 8), 1);
        loadedVersion[b] = 0;
        loadedImage[b] = {0, 0};
    };

    /** A GPU external's rectangle to bring in: dst in buffer texels, src in image texels, at in the raw buffer. */
    struct GpuLoad
    {
        size_t buffer = 0;
        GpuImageRef image;
        std::array<int, 4> dst{}, src{};
        VkDeviceSize at = 0;
    };
    std::vector<GpuLoad> gpuLoads;
    VkDeviceSize rawBytes = 0;
    for (const BufferDesc& d : cur->plan.buffers)
    {
        const size_t b = size_t(d.id);
        if (!load[b])
            continue;
        if (!cur->unused[b])
        {
            const auto [lw, lh] = cur->sizes[b];
            const Rect r = loadTarget[b] != 0 ? loadRect[b] : Rect::full();
            const std::array<int, 4> c = r.cover(lw, lh);
            const double texels = double(std::max(0, c[2] - c[0])) * double(std::max(0, c[3] - c[1]));
            if (sliceTexels > 0.0 && sliceTexels + texels > maxTexels)
            {
                deferred = true;
                continue;
            }
            sliceTexels += texels;
        }
        load[b] = 0;
        if (cur->unused[b])
            continue;
        any = true;
        if (d.kind == BufferKind::Image)
        {
            upload(*cur->images.at(d.id), b);
            continue;
        }
        if (d.kind != BufferKind::External)
            continue;
        const Inputs& in = cur->inputs;
        if (auto g = in.gpuExternals.find(d.name); g != in.gpuExternals.end() && g->second)
        {
            const GpuImageRef& img = g->second;
            // Before prepare: an update queued after it must not count as loaded.
            const uint64_t now = img->version();
            std::vector<compute::Buffer> staging;
            const bool ok = img->prepare(vk, cmd, staging);
            temps.insert(temps.end(), staging.begin(), staging.end());
            if (!ok || !img->realized())
            {
                uvFill(b);
                continue;
            }
            const auto [bw, bh] = cur->sizes[b];
            const bool partial = !arenaFresh && loadedImage[b].first == img->key() && loadTarget[b] != 0;
            const uint64_t target = partial ? loadTarget[b] : now;
            loadedImage[b] = {img->key(), target};
            loadedVersion[b] = img->key() * 1000003ull ^ target;
            GpuLoad l;
            l.buffer = b;
            l.image = img;
            l.dst = (partial ? loadRect[b] : Rect::full()).cover(bw, bh);
            if (l.dst[2] <= l.dst[0] || l.dst[3] <= l.dst[1])
                continue;
            l.src = Rect::texels(l.dst, bw, bh).cover(img->width(), img->height());
            l.at = rawBytes;
            const VkDeviceSize bytes = VkDeviceSize(l.src[2] - l.src[0]) * VkDeviceSize(l.src[3] - l.src[1]) * VkDeviceSize(texelBytes(img->format()));
            rawBytes += (bytes + 255) & ~VkDeviceSize(255);
            gpuLoads.push_back(std::move(l));
        }
        else if (auto c = in.cpuExternals.find(d.name); c != in.cpuExternals.end() && c->second.width > 0)
        {
            upload(c->second, b);
            loadedVersion[b] = 0;
            loadedImage[b] = {0, 0};
        }
        else
            uvFill(b);
    }
    if (!gpuLoads.empty())
    {
        // Earlier slices have finished (each waits), so the previous raw buffer is free to go.
        if (loadRaw.size < rawBytes)
        {
            compute::destroy(vk, loadRaw);
            loadRaw = compute::makeBuffer(vk, std::max<VkDeviceSize>(rawBytes, VkDeviceSize(4) << 20), 0, compute::Host::None);
        }
        for (const GpuLoad& l : gpuLoads)
            l.image->recordReadRect(cmd, loadRaw, l.at, l.src[0], l.src[1], l.src[2] - l.src[0], l.src[3] - l.src[1]);
        compute::memoryBarrier(cmd);
        VkPipelineLayout layout;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel(vk, "tex_decode.spv", "texDecodeCS", sizeof(DecodePush), layout));
        for (const GpuLoad& l : gpuLoads)
        {
            const auto [bw, bh] = cur->sizes[l.buffer];
            const int w = l.dst[2] - l.dst[0], h = l.dst[3] - l.dst[1];
            const float sx = float(l.image->width()) / float(bw), sy = float(l.image->height()) / float(bh);
            const uint64_t texels = uint64_t(w) * uint64_t(h);
            DecodePush push{loadRaw.address + l.at,
                            arena.address + offset[l.buffer] * kTexel +
                                (VkDeviceSize(l.dst[1]) * VkDeviceSize(bw) + VkDeviceSize(l.dst[0])) * elemBytes(l.buffer),
                            uint32_t(w),
                            uint32_t(h),
                            formatCode(l.image->format()),
                            usesSrgb(l.image->format(), l.image->desc().srgb) ? 1u : 0u,
                            groupsX(texels),
                            uint32_t(bw),
                            uint32_t(l.src[2] - l.src[0]),
                            uint32_t(l.src[3] - l.src[1]),
                            {sx, sy},
                            {float(l.dst[0]) * sx - float(l.src[0]), float(l.dst[1]) * sy - float(l.src[1])},
                            cur->half[l.buffer],
                            0};
            vkCmdPushConstants(cmd, layout, kCompute, 0, sizeof(push), &push);
            dispatchLinear(cmd, texels);
            sliceLoaded += texels;
        }
    }
    if (any)
        compute::memoryBarrier(cmd);
    return !deferred;
}

void Evaluator::State::startPass(VulkanContext& vk, const Pass& p)
{
    passStarted = true;
    const size_t out = size_t(p.output);
    const auto [w, h] = cur->sizes[out];
    passX0 = 0;
    passX1 = w;
    passRow = 0;
    passY1 = h;
    passBands = false;
    const VkDeviceSize align = std::max<VkDeviceSize>(vk.properties.limits.minStorageBufferOffsetAlignment, 1);
    const VkDeviceSize rowBytes = VkDeviceSize(w) * elemBytes(out);
    // A band binds gOut at its first texel: rows of 8 keep that aligned, and columns of 8 too when the width allows.
    passBands = h % 8 == 0 && (offset[out] * kTexel) % align == 0 && (rowBytes * 8) % align == 0;
    if (!passBands)
        return;
    const std::array<int, 4> c = passRect[passIndex].cover(w, h, 8);
    passRow = c[1];
    passY1 = c[3];
    if (w % 8 == 0 && (8 * elemBytes(out)) % align == 0)
    {
        passX0 = c[0];
        passX1 = c[2];
    }
}

double Evaluator::State::recordPasses(VulkanContext& vk, VkCommandBuffer cmd, double budgetMs, Stats& delta)
{
    LILA_ZONE();
    double used = 0.0;
    uint32_t dispatches = 0;
    const VkDeviceSize uniformBytes = uniformValues.size() * sizeof(float);
    const uint64_t passKey = compute::layoutKey(0, kCompute, "texgraph");
    const auto& passes = cur->plan.passes;
    auto finishPass = [&] {
        dirty[passIndex] = 0;
        ++delta.passesRun;
        ++passIndex;
        passRow = 0;
        passStarted = false;
        compute::memoryBarrier(cmd);
    };
    while (passIndex < passes.size())
    {
        if (!dirty[passIndex])
        {
            ++passIndex;
            passRow = 0;
            passStarted = false;
            continue;
        }
        const Pass& p = passes[passIndex];
        const size_t out = size_t(p.output);
        const auto [w, h] = cur->sizes[out];
        const double unit = std::max(p.cost, 1.0);
        if (p.kind == PassKind::Native)
        {
            const double rowMs = double(w) * unit / rate;
            const Route route = cur->routes[passIndex];
            if (route == Route::Cpu)
            {
                cpuNext = true;
                break;
            }
            if (route == Route::Kernels)
            {
                if (!recordKernelSteps(vk, cmd, p, budgetMs, used, dispatches, delta))
                    break;
                sliceComputed += uint64_t(w) * uint64_t(h);
                finishPass();
                continue;
            }
            const double ms = rowMs * double(h);
            if (used > 0.0 && used + ms > budgetMs)
                break;
            NativeGpuFn fn = route == Route::Registered ? findNative(p.nativeType) : NativeGpuFn(blurGpu);
            if (!fn)
                throw std::runtime_error("no GPU implementation of native node " + p.nativeType);
            NativeCall call{cur->plan, p, std::span<const float>(uniformValues), {}, {arena.address + offset[out] * kTexel, w, h}, scratch.address};
            for (const auto& [id, b] : p.inputBuffers)
                call.inputs[id] = {arena.address + offset[size_t(b)] * kTexel, cur->sizes[size_t(b)].first, cur->sizes[size_t(b)].second};
            compute::memoryBarrier(cmd);
            fn(vk, cmd, call);
            used += ms;
            ++delta.dispatches;
            sliceComputed += uint64_t(w) * uint64_t(h);
            finishPass();
            continue;
        }
        if (dispatches >= kMaxDispatches)
            break;
        if (!passStarted)
            startPass(vk, p);
        const int cols = passX1 - passX0;
        if (cols <= 0 || passRow >= passY1)
        {
            finishPass();
            continue;
        }
        const double rowMs = double(cols) * unit / rate;
        int rows = passY1 - passRow;
        if (passBands)
        {
            const size_t groups = compute::sliceFit(size_t(rows / 8), rowMs * 8.0, used, budgetMs, 1);
            if (groups == 0)
                break;
            rows = int(groups) * 8;
        }
        else if (used > 0.0 && used + rowMs * double(rows) > budgetMs)
            break;

        const size_t misses = compute::sharedPipelines().misses();
        VkPipeline pipe = compute::sharedPipelines().get(vk, shared().passLayout, passKey, *cur->code[passIndex], p.hash, p.entry.c_str());
        delta.pipelinesCreated += compute::sharedPipelines().misses() - misses;

        // A band covers rows [passRow, passRow + rows) of columns [passX0, passX1): gOut starts at its first texel and
        // uv shifts by as much.
        PassHeader header{{float(w), float(h), 1.0f / float(w), 1.0f / float(h)},
                          {float(double(cur->inputs.uvOffset[0]) + double(passX0) / double(w)),
                           float(double(cur->inputs.uvOffset[1]) - double(passRow) / double(h)), 1.0f, 1.0f},
                          {cur->half[out] ? 1u : 0u, 0u, 0u, 0u}};
        std::memcpy(static_cast<uint8_t*>(headers.mapped) + dispatches * kHeaderStride, &header, sizeof(header));

        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &shared().setLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(vk.device, &ai, &set) != VK_SUCCESS)
            throw std::runtime_error("texture graph GPU: out of descriptor sets");
        const VkDescriptorBufferInfo infos[4] = {
            {headers.buffer, dispatches * kHeaderStride, sizeof(PassHeader)},
            {uniforms.buffer, 0, uniformBytes},
            {arena.buffer, 0, VK_WHOLE_SIZE},
            {arena.buffer, offset[out] * kTexel + (VkDeviceSize(passRow) * VkDeviceSize(w) + VkDeviceSize(passX0)) * elemBytes(out),
             (VkDeviceSize(rows - 1) * VkDeviceSize(w) + VkDeviceSize(cols)) * elemBytes(out)}};
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
        vkCmdDispatch(cmd, uint32_t((cols + 7) / 8), uint32_t((rows + 7) / 8), 1);
        ++dispatches;
        ++delta.dispatches;
        used += rowMs * double(rows);
        sliceComputed += uint64_t(cols) * uint64_t(rows);
        passRow += rows;
        if (passRow >= passY1)
            finishPass();
    }
    if (dispatches)
        vmaFlushAllocation(vk.allocator, headers.alloc, 0, VK_WHOLE_SIZE);
    return used;
}

bool Evaluator::State::recordKernelSteps(VulkanContext& vk, VkCommandBuffer cmd, const Pass& p, double budgetMs, double& used, uint32_t& dispatches,
                                         Stats& delta)
{
    const tex::NativeSchedule& sched = cur->schedules[passIndex];
    const tex::NodeDef* def = tex::findNode(p.nativeType);
    if (!def)
        throw std::runtime_error("no node type " + p.nativeType);
    std::vector<std::array<uint32_t, 3>> descs;
    for (const tex::PortDecl& in : def->inputs)
    {
        auto it = p.inputBuffers.find(in.id);
        if (it == p.inputBuffers.end())
            descs.push_back({uint32_t(scratchBase), 1u, 1u});
        else
            descs.push_back({uint32_t(offset[size_t(it->second)]), uint32_t(cur->sizes[size_t(it->second)].first),
                             uint32_t(cur->sizes[size_t(it->second)].second)});
    }
    const size_t out = size_t(p.output);
    descs.push_back({uint32_t(offset[out]), uint32_t(cur->sizes[out].first), uint32_t(cur->sizes[out].second)});
    VkDeviceSize at = scratchBase;
    for (const auto& sc : sched.scratch)
    {
        descs.push_back({uint32_t(at), uint32_t(sc[0]), uint32_t(sc[1])});
        at += (VkDeviceSize(sc[0]) * VkDeviceSize(sc[1]) + 15) & ~VkDeviceSize(15);
    }
    const std::vector<float> frame = tex::nativeFrame(*def, p, uniformValues, descs);
    const VkDeviceSize align = std::max<VkDeviceSize>(vk.properties.limits.minStorageBufferOffsetAlignment, 16);
    const VkDeviceSize frameAt = (framesUsed + align - 1) / align * align, frameBytes = frame.size() * sizeof(float);
    if (frameAt + frameBytes > frames.size)
    {
        if (framesUsed == 0)
            throw std::runtime_error("texture graph GPU: the frame of native node " + p.nativeType + " is too large");
        return false;
    }
    std::memcpy(static_cast<uint8_t*>(frames.mapped) + frameAt, frame.data(), frameBytes);
    vmaFlushAllocation(vk.allocator, frames.alloc, frameAt, frameBytes);
    framesUsed = frameAt + frameBytes;
    const uint64_t passKey = compute::layoutKey(0, kCompute, "texgraph");
    for (; passRow < int(sched.steps.size()); ++passRow)
    {
        const tex::NativeStep& st = sched.steps[size_t(passRow)];
        const double ms = double(st.threadsX) * double(st.threadsY) / rate;
        if ((used > 0.0 && used + ms > budgetMs) || dispatches >= kMaxDispatches)
            return false;
        const auto& [code, hash] = cur->stepCode[passIndex][size_t(passRow)];
        const size_t misses = compute::sharedPipelines().misses();
        VkPipeline pipe = compute::sharedPipelines().get(vk, shared().passLayout, passKey, *code, hash, "texMain");
        delta.pipelinesCreated += compute::sharedPipelines().misses() - misses;
        const auto& od = descs.at(size_t(st.output));
        PassHeader header{{float(od[1]), float(od[2]), 1.0f / float(od[1]), 1.0f / float(od[2])},
                          {st.constants[0], st.constants[1], st.constants[2], st.constants[3]},
                          {0u, 0u, 0u, 0u}};
        std::memcpy(static_cast<uint8_t*>(headers.mapped) + dispatches * kHeaderStride, &header, sizeof(header));
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &shared().setLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(vk.device, &ai, &set) != VK_SUCCESS)
            throw std::runtime_error("texture graph GPU: out of descriptor sets");
        const VkDescriptorBufferInfo infos[4] = {{headers.buffer, dispatches * kHeaderStride, sizeof(PassHeader)},
                                                 {frames.buffer, frameAt, frameBytes},
                                                 {arena.buffer, 0, VK_WHOLE_SIZE},
                                                 {arena.buffer, VkDeviceSize(od[0]) * kTexel, VkDeviceSize(od[1]) * VkDeviceSize(od[2]) * kTexel}};
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
        vkCmdDispatch(cmd, uint32_t((st.threadsX + 7) / 8), uint32_t((st.threadsY + 7) / 8), 1);
        compute::memoryBarrier(cmd);
        used += ms;
        ++dispatches;
        ++delta.dispatches;
    }
    return true;
}

double Evaluator::State::runCpuNative(VulkanContext& vk)
{
    LILA_ZONE();
    const Pass& p = cur->plan.passes[passIndex];
    tex::NativeCpuFn fn = tex::nativeCpu(p.nativeType);
    if (!fn)
        throw std::runtime_error("no implementation of native node " + p.nativeType);
    const size_t out = size_t(p.output);
    const auto [w, h] = cur->sizes[out];
    VkDeviceSize total = 0;
    std::vector<VkDeviceSize> places;
    for (const auto& [id, b] : p.inputBuffers)
    {
        places.push_back(total);
        total += VkDeviceSize(cur->sizes[size_t(b)].first) * VkDeviceSize(cur->sizes[size_t(b)].second) * kTexel;
    }
    compute::Buffer host = compute::makeBuffer(vk, std::max<VkDeviceSize>(total, 16), 0, compute::Host::Read);
    compute::Buffer staging;
    double ms = 0.0;
    try
    {
        ms += sub.run(vk, [&](VkCommandBuffer cmd) {
            size_t k = 0;
            for (const auto& [id, b] : p.inputBuffers)
            {
                VkBufferCopy region{offset[size_t(b)] * kTexel, places[k++],
                                    VkDeviceSize(cur->sizes[size_t(b)].first) * VkDeviceSize(cur->sizes[size_t(b)].second) * kTexel};
                vkCmdCopyBuffer(cmd, arena.buffer, host.buffer, 1, &region);
            }
        });
        vmaInvalidateAllocation(vk.allocator, host.alloc, 0, VK_WHOLE_SIZE);
        std::vector<float> result(size_t(w) * size_t(h) * 4, 0.0f);
        tex::NativeCall call{cur->plan, p, std::span<const float>(uniformValues), {}, {result.data(), w, h}};
        size_t k = 0;
        for (const auto& [id, b] : p.inputBuffers)
        {
            float* data = reinterpret_cast<float*>(static_cast<uint8_t*>(host.mapped) + places[k++]);
            call.inputs[id] = {data, cur->sizes[size_t(b)].first, cur->sizes[size_t(b)].second};
        }
        const auto t0 = std::chrono::steady_clock::now();
        fn(call);
        ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        staging = compute::hostBuffer(vk, result.data(), result.size() * sizeof(float));
        ms += sub.run(vk, [&](VkCommandBuffer cmd) {
            VkBufferCopy region{0, offset[out] * kTexel, result.size() * sizeof(float)};
            vkCmdCopyBuffer(cmd, staging.buffer, arena.buffer, 1, &region);
        });
    }
    catch (...)
    {
        compute::destroy(vk, host);
        compute::destroy(vk, staging);
        throw;
    }
    compute::destroy(vk, host);
    compute::destroy(vk, staging);
    dirty[passIndex] = 0;
    ++passIndex;
    passRow = 0;
    cpuNext = false;
    return ms;
}

void Evaluator::State::recordStores(VulkanContext& vk, VkCommandBuffer cmd, std::vector<compute::Buffer>& temps, Stats& delta)
{
    LILA_ZONE();
    for (size_t t = 0; t < cur->stores.size(); ++t)
    {
        if (!storeDirty[t])
            continue;
        storeDirty[t] = 0;
        const StoreTarget& target = cur->stores[t];
        const GpuImageRef& img = target.image;
        if (!img)
            continue;
        std::vector<compute::Buffer> staging;
        const bool ok = img->prepare(vk, cmd, staging);
        temps.insert(temps.end(), staging.begin(), staging.end());
        if (!ok)
            continue;
        const std::array<int, 4> r = storeRect[t].cover(img->width(), img->height());
        const int rw = r[2] - r[0], rh = r[3] - r[1];
        if (rw <= 0 || rh <= 0)
            continue;
        stored = stored.unite(Rect::texels(r, img->width(), img->height()));
        const uint64_t texels = uint64_t(rw) * uint64_t(rh);
        const uint64_t nWords = (texels * uint64_t(texelBytes(img->format())) + 3) / 4;
        StorePush push{};
        push.arena = arena.address;
        push.dst = words.address;
        for (size_t c = 0; c < 4; ++c)
        {
            const int b = cur->storeSources[t][c];
            if (b < 0)
            {
                push.src[c][0] = 0;
                push.src[c][1] = push.src[c][2] = 1;
                push.src[c][3] = 4;
                push.constants[c] = target.channels[c].constant;
                continue;
            }
            push.src[c][0] = uint32_t(offset[size_t(b)] * 2);
            push.src[c][1] = uint32_t(cur->sizes[size_t(b)].first);
            push.src[c][2] = uint32_t(cur->sizes[size_t(b)].second);
            push.src[c][3] = uint32_t(std::clamp(target.channels[c].component, 0, 3)) | (cur->half[size_t(b)] ? 0x100u : 0u);
        }
        push.width = uint32_t(img->width());
        push.height = uint32_t(img->height());
        push.format = formatCode(img->format());
        push.srgb = usesSrgb(img->format(), img->desc().srgb) ? 1u : 0u;
        push.words = uint32_t(nWords);
        push.groupsX = groupsX(nWords);
        push.origin = uint32_t(r[0]) | (uint32_t(r[1]) << 16);
        push.extent = uint32_t(rw) | (uint32_t(rh) << 16);
        VkPipelineLayout layout;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel(vk, "tex_store.spv", "texStoreCS", sizeof(StorePush), layout));
        vkCmdPushConstants(cmd, layout, kCompute, 0, sizeof(push), &push);
        dispatchLinear(cmd, nWords);
        compute::memoryBarrier(cmd);
        img->beginWrite(cmd);
        compute::memoryBarrier(cmd);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {r[0], r[1], 0};
        region.imageExtent = {uint32_t(rw), uint32_t(rh), 1};
        vkCmdCopyBufferToImage(cmd, words.buffer, img->image(), VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        compute::memoryBarrier(cmd);
        img->noteWriteRect(r[0], r[1], r[2], r[3]);
        img->endWrite(cmd);
        sliceStored += texels;
        ++delta.stores;
    }
}
} // namespace lila::render::texgpu
