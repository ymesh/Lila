#include "render/TexEval.h"
#include "render/TexEvalInternal.h"

namespace lila::render::texgpu
{
namespace
{
/**
 * Places buffers so that those never live at once share memory: each lives from the pass that writes it (loaded ones
 * from the start) to the last pass that reads it, outputs to the end. spans are in texels; returns the texels used.
 */
VkDeviceSize packBuffers(const Plan& plan, const std::vector<VkDeviceSize>& spans, std::vector<VkDeviceSize>& offset)
{
    LILA_ZONE();
    const size_t n = plan.buffers.size();
    std::vector<int> first(n, -1), last(n, -1);
    for (size_t p = 0; p < plan.passes.size(); ++p)
    {
        const Pass& pass = plan.passes[p];
        first[size_t(pass.output)] = int(p);
        last[size_t(pass.output)] = std::max(last[size_t(pass.output)], int(p));
        for (int r : pass.reads)
            last[size_t(r)] = std::max(last[size_t(r)], int(p));
        for (const auto& [id, r] : pass.inputBuffers)
            last[size_t(r)] = std::max(last[size_t(r)], int(p));
    }
    for (const BufferDesc& b : plan.buffers)
        if (b.kind == BufferKind::Output)
            last[size_t(b.id)] = std::numeric_limits<int>::max();
    for (const auto& [name, id] : plan.outputs)
        last[size_t(id)] = std::numeric_limits<int>::max();

    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return spans[a] > spans[b]; });
    std::vector<size_t> placed;
    std::vector<std::pair<VkDeviceSize, VkDeviceSize>> taken;
    VkDeviceSize total = 0;
    for (size_t b : order)
    {
        taken.clear();
        for (size_t o : placed)
            if (first[o] <= last[b] && first[b] <= last[o])
                taken.emplace_back(offset[o], offset[o] + spans[o]);
        std::sort(taken.begin(), taken.end());
        VkDeviceSize at = 0;
        for (const auto& [lo, hi] : taken)
        {
            if (lo >= at + spans[b])
                break;
            at = std::max(at, hi);
        }
        offset[b] = at;
        placed.push_back(b);
        total = std::max(total, at + spans[b]);
    }
    return total;
}

bool sameBits(const float* a, const float* b) { return std::memcmp(a, b, sizeof(float) * 4) == 0; }

bool sameImage(const image::Image& a, const image::Image& b) { return a.width == b.width && a.height == b.height && a.rgba == b.rgba; }

bool sameStores(const std::vector<StoreTarget>& a, const std::vector<StoreTarget>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].image != b[i].image)
            return false;
        for (int c = 0; c < 4; ++c)
        {
            const auto &x = a[i].channels[size_t(c)], &y = b[i].channels[size_t(c)];
            if (x.output != y.output || x.component != y.component || x.constant != y.constant)
                return false;
        }
    }
    return true;
}

uint64_t versionOf(const GpuImageRef& img) { return img ? img->key() * 1000003ull ^ img->version() : 0; }
} // namespace

void Evaluator::State::adopt(VkDeviceSize maxBytes)
{
    std::unique_ptr<Prepared> next;
    {
        std::scoped_lock lock(mutex);
        next = std::move(pending);
    }
    if (!next)
        return;
    const Plan& np = next->plan;
    const size_t nPasses = np.passes.size(), nBuffers = np.buffers.size();
    const bool keep = cur && arena.buffer && cur->plan.structureHash == np.structureHash && cur->plan.width == np.width &&
                      cur->plan.height == np.height && cur->sizes == next->sizes && cur->unused == next->unused && cur->half == next->half &&
                      cur->pagePass == next->pagePass && cur->regionExternal == next->regionExternal && cur->plan.passes.size() == nPasses &&
                      next->scratchTexels <= scratchCapacity;
    std::set<int> changed;
    bool moved = false, storesChanged = false;
    std::vector<std::pair<size_t, GpuImageRef>> gpuLoads;
    if (!keep)
    {
        reallocate = true;
        load.assign(nBuffers, 1);
        dirty.assign(nPasses, 1);
        storeDirty.assign(next->stores.size(), 1);
        loadRect.assign(nBuffers, Rect::full());
        passRect.assign(nPasses, Rect::full());
        storeRect.assign(next->stores.size(), Rect::full());
        loadTarget.assign(nBuffers, 0);
        loadedVersion.assign(nBuffers, 0);
        uniformsDirty = true;
    }
    else
    {
        // Value slots that changed; descriptor slots are the evaluator's own.
        std::set<int> descriptors;
        for (const BufferDesc& b : np.buffers)
            descriptors.insert(b.slot);
        for (int s = 0; s < np.uniformSlots; ++s)
            if (!descriptors.contains(s) && !sameBits(&np.uniforms[size_t(s) * 4], &cur->plan.uniforms[size_t(s) * 4]))
                changed.insert(s);
        const Inputs &ni = next->inputs, &ci = cur->inputs;
        moved = ni.uvOffset[0] != ci.uvOffset[0] || ni.uvOffset[1] != ci.uvOffset[1];
        for (const BufferDesc& b : np.buffers)
        {
            const size_t id = size_t(b.id);
            bool whole = false;
            if (b.kind == BufferKind::Image)
                whole = b.name != cur->plan.buffers[id].name || next->images[b.id] != cur->images[b.id];
            else if (b.kind == BufferKind::External)
            {
                auto g = ni.gpuExternals.find(b.name);
                auto c = ni.cpuExternals.find(b.name);
                auto oc = ci.cpuExternals.find(b.name);
                if (g != ni.gpuExternals.end() && g->second)
                    gpuLoads.emplace_back(id, g->second);
                else if (c != ni.cpuExternals.end())
                    whole = oc == ci.cpuExternals.end() || !sameImage(c->second, oc->second);
                else
                    whole = moved || loadedVersion[id] != 0 || oc != ci.cpuExternals.end();
            }
            if (whole)
            {
                load[id] = 1;
                loadRect[id] = Rect::full();
                loadTarget[id] = 0;
            }
        }
        if (!changed.empty())
            uniformsDirty = true;
        storesChanged = !sameStores(next->stores, cur->stores);
    }
    passIndex = 0;
    passRow = 0;
    passStarted = false;

    Stats fresh;
    fresh.compiled = next->compiled;
    fresh.cached = next->cached;
    fresh.warnings = next->warnings;
    cur = std::move(next);
    if (keep)
    {
        for (auto& [id, img] : gpuLoads)
            markExternal(id, img);
        if (storesChanged)
        {
            storeDirty.assign(cur->stores.size(), 1);
            storeRect.assign(cur->stores.size(), Rect::full());
        }
        propagate(changed, moved);
    }
    maskPages();
    fresh.passesDirty = size_t(std::count(dirty.begin(), dirty.end(), uint8_t(1)));
    {
        std::scoped_lock lock(mutex);
        stats = fresh;
    }

    // Place the buffers: each starts 256-byte aligned, so row bands of a pass bind at aligned offsets.
    offset.assign(nBuffers, 0);
    std::vector<VkDeviceSize> spans(nBuffers);
    VkDeviceSize total = 0;
    for (size_t b = 0; b < nBuffers; ++b)
    {
        offset[b] = total;
        const auto [w, h] = cur->sizes[b];
        const VkDeviceSize texels = VkDeviceSize(w) * VkDeviceSize(h);
        spans[b] = ((cur->half[b] ? (texels + 1) / 2 : texels) + 15) & ~VkDeviceSize(15);
        total += spans[b];
    }
    const bool wasPacked = packed;
    arenaLimit = cur->inputs.maxArenaBytes ? std::min<VkDeviceSize>(maxBytes, cur->inputs.maxArenaBytes) : maxBytes;
    packed = !cur->inputs.pages && (cur->inputs.compact || (total + cur->scratchTexels) * kTexel > arenaLimit);
    if (packed)
        total = packBuffers(cur->plan, spans, offset);
    if (keep && packed != wasPacked)
        reallocate = true;
    if (keep && (packed || wasPacked))
    {
        dirtyAll();
        std::scoped_lock lock(mutex);
        stats.passesDirty = nPasses;
    }
    // Pages plans: room for a batch of pages after the buffers, each page its region externals (with a margin for
    // bilinear reads) and its page passes' outputs.
    pageBase = total;
    pageTexels = 0;
    pageExternalAt.clear();
    pageOutputAt.clear();
    if (cur->inputs.pages)
    {
        const VkDeviceSize side = VkDeviceSize(std::max(cur->inputs.pageSize, 8));
        for (const BufferDesc& b : cur->plan.buffers)
            if (cur->regionExternal[size_t(b.id)])
            {
                pageExternalAt[b.id] = pageTexels;
                pageTexels += ((side + 2 * kPageMargin) * (side + 2 * kPageMargin) + 15) & ~VkDeviceSize(15);
            }
        for (size_t p = 0; p < nPasses; ++p)
            if (cur->pagePass[p])
            {
                pageOutputAt[cur->plan.passes[p].output] = pageTexels;
                pageTexels += (side * side + 15) & ~VkDeviceSize(15);
            }
        total += pageTexels * Evaluator::kPagesPerBatch;
    }
    if (!keep)
        scratchCapacity = cur->scratchTexels;
    scratchBase = total;
    arenaTexels = std::max<VkDeviceSize>(total + scratchCapacity, 16);
    {
        std::scoped_lock lock(mutex);
        stats.arenaBytes = arenaTexels * kTexel;
    }
    uniformValues = cur->plan.uniforms;
    if (uniformValues.empty())
        uniformValues.assign(4, 0.0f);
    for (const BufferDesc& b : cur->plan.buffers)
    {
        // The GPU form indexes the arena in 32-bit pairs.
        const size_t id = size_t(b.id);
        const uint32_t d[4] = {uint32_t(cur->gpuForm ? offset[id] * 2 : offset[id]), uint32_t(cur->sizes[id].first), uint32_t(cur->sizes[id].second),
                               cur->half[id] ? 1u : 0u};
        std::memcpy(&uniformValues[size_t(b.slot) * 4], d, sizeof(d));
    }
    if (!keep)
        uniformsDirty = true;
}

bool Evaluator::State::markExternal(size_t b, const GpuImageRef& img)
{
    if (versionOf(img) == loadedVersion[b])
        return false;
    Rect r = Rect::full();
    uint64_t target = 0;
    if (loadedImage.size() == loadedVersion.size() && loadedImage[b].first == img->key() && !arenaFresh)
    {
        const uint64_t now = img->version();
        if (std::optional<std::array<int, 4>> t = img->rectSince(loadedImage[b].second))
        {
            r = Rect::texels(*t, img->width(), img->height());
            target = now;
        }
    }
    // A whole load already pending covers this; otherwise r holds every change since the last load.
    if (load[b] && loadTarget[b] == 0)
        return true;
    load[b] = 1;
    loadRect[b] = target ? r : Rect::full();
    loadTarget[b] = target;
    return true;
}

void Evaluator::State::markExternals()
{
    bool any = false;
    for (const BufferDesc& b : cur->plan.buffers)
        if (b.kind == BufferKind::External)
            if (auto g = cur->inputs.gpuExternals.find(b.name); g != cur->inputs.gpuExternals.end() && g->second)
                any = markExternal(size_t(b.id), g->second) || any;
    if (!any)
        return;
    if (packed)
        dirtyAll();
    else
        propagate({}, false);
    maskPages();
    passIndex = 0;
    passRow = 0;
    passStarted = false;
    std::scoped_lock lock(mutex);
    stats.passesDirty = size_t(std::count(dirty.begin(), dirty.end(), uint8_t(1)));
}

void Evaluator::State::propagate(const std::set<int>& changed, bool moved)
{
    const Plan& plan = cur->plan;
    std::vector<int> writer(plan.buffers.size(), -1);
    for (size_t p = 0; p < plan.passes.size(); ++p)
        writer[size_t(plan.passes[p].output)] = int(p);
    for (size_t p = 0; p < plan.passes.size(); ++p)
    {
        const Pass& pass = plan.passes[p];
        const auto [ow, oh] = cur->sizes[size_t(pass.output)];
        bool whole = moved && pass.kind == PassKind::Fused;
        for (int s : pass.slots)
            whole = whole || changed.contains(s);
        bool any = whole;
        Rect r;
        for (int b : pass.reads)
        {
            Rect src;
            bool srcDirty = false;
            if (load[size_t(b)])
            {
                src = loadRect[size_t(b)];
                srcDirty = true;
            }
            if (const int w = writer[size_t(b)]; w >= 0 && dirty[size_t(w)])
            {
                src = srcDirty ? src.unite(passRect[size_t(w)]) : passRect[size_t(w)];
                srcDirty = true;
            }
            if (!srcDirty)
                continue;
            any = true;
            if (pass.kind == PassKind::Fused && std::find(pass.local.begin(), pass.local.end(), b) != pass.local.end())
            {
                // Bilinear reads reach a texel of the source either side, plus the output texel's own extent.
                const auto [bw, bh] = cur->sizes[size_t(b)];
                r = r.unite(src.grow(1.5f / float(bw) + 1.0f / float(ow), 1.5f / float(bh) + 1.0f / float(oh)));
            }
            else
                whole = true;
        }
        if (!any)
            continue;
        const Rect add = whole ? Rect::full() : r;
        passRect[p] = dirty[p] ? passRect[p].unite(add) : add;
        dirty[p] = 1;
    }
    for (size_t t = 0; t < cur->stores.size(); ++t)
    {
        const StoreTarget& target = cur->stores[t];
        Rect r;
        bool any = false;
        for (int b : cur->storeSources[t])
            if (b >= 0 && writer[size_t(b)] >= 0 && dirty[size_t(writer[size_t(b)])])
            {
                r = r.unite(passRect[size_t(writer[size_t(b)])]);
                any = true;
            }
        if (!any)
            continue;
        if (target.image)
            r = r.grow(1.0f / float(target.image->width()), 1.0f / float(target.image->height()));
        storeRect[t] = storeDirty[t] ? storeRect[t].unite(r) : r;
        storeDirty[t] = 1;
    }
}

bool Evaluator::State::anyWork() const
{
    auto any = [](const std::vector<uint8_t>& v) { return std::find(v.begin(), v.end(), uint8_t(1)) != v.end(); };
    return uniformsDirty || any(load) || any(dirty) || any(storeDirty);
}

void Evaluator::State::allocate(VulkanContext& vk)
{
    LILA_ZONE();
    ensurePassLayout(vk);
    sub.init(vk);
    if (!pool)
    {
        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxDispatches}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kMaxDispatches}};
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = kMaxDispatches;
        ci.poolSizeCount = 2;
        ci.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(vk.device, &ci, nullptr, &pool) != VK_SUCCESS)
            throw std::runtime_error("texture graph GPU: cannot create a descriptor pool");
    }
    if (!frames.buffer)
        frames = compute::makeBuffer(vk, VkDeviceSize(256) << 10, 0, compute::Host::Write);
    if (!headers.buffer)
        headers = compute::makeBuffer(vk, kHeaderStride * kMaxDispatches, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, compute::Host::Write);
    if (reallocate || !arena.buffer)
    {
        const VkDeviceSize bytes = arenaTexels * kTexel;
        if (bytes > arenaLimit)
            throw ArenaTooLarge("texture graph GPU: the graph's buffers need " + std::to_string(bytes >> 20) + " MB, more than one GPU buffer can hold (" +
                                std::to_string(arenaLimit >> 20) + " MB)");
        compute::destroy(vk, arena);
        arena = compute::makeBuffer(vk, bytes, 0, compute::Host::None);
        reallocate = false;
        arenaFresh = true;
    }
    const VkDeviceSize uniformBytes = uniformValues.size() * sizeof(float);
    if (uniforms.size < uniformBytes)
    {
        compute::destroy(vk, uniforms);
        compute::destroy(vk, uniformStaging);
        uniforms = compute::makeBuffer(vk, uniformBytes, 0, compute::Host::None);
        uniformStaging = compute::makeBuffer(vk, uniformBytes, 0, compute::Host::Write);
        uniformsDirty = true;
    }
    VkDeviceSize scratchBytes = 0, wordBytes = 0;
    for (const Pass& p : cur->plan.passes)
        if (p.kind == PassKind::Native)
        {
            const auto [w, h] = cur->sizes[size_t(p.output)];
            scratchBytes = std::max(scratchBytes, VkDeviceSize(w) * VkDeviceSize(h) * kTexel);
        }
    for (const StoreTarget& t : cur->stores)
        if (t.image)
            wordBytes = std::max(wordBytes, (VkDeviceSize(t.image->width()) * VkDeviceSize(t.image->height()) * VkDeviceSize(texelBytes(t.image->format())) + 3) &
                                                ~VkDeviceSize(3));
    if (scratch.size < scratchBytes)
    {
        compute::destroy(vk, scratch);
        scratch = compute::makeBuffer(vk, scratchBytes, 0, compute::Host::None);
    }
    if (words.size < wordBytes)
    {
        compute::destroy(vk, words);
        words = compute::makeBuffer(vk, wordBytes, 0, compute::Host::None);
    }
}
} // namespace lila::render::texgpu
