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

uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

uint64_t mixText(uint64_t h, std::string_view s) { return mix(h, std::hash<std::string_view>{}(s)); }

/**
 * A fused pass's source with the slot numbers it names (gU[n], texReadBuf(n, .base = nu) replaced by their order of
 * first use, and those slots in that order: two plans that number their slots differently give equal text.
 */
std::pair<std::string, std::vector<int>> slotFree(const std::string& src)
{
    static constexpr std::string_view kMarks[] = {"gU[", "texReadBuf(", ".base = "};
    std::string out;
    out.reserve(src.size());
    std::vector<int> order;
    size_t i = 0;
    while (i < src.size())
    {
        size_t at = std::string::npos;
        std::string_view mark;
        for (std::string_view m : kMarks)
            if (size_t f = src.find(m, i); f < at)
            {
                at = f;
                mark = m;
            }
        if (at == std::string::npos)
        {
            out.append(src, i);
            break;
        }
        size_t d = at + mark.size();
        out.append(src, i, d - i);
        size_t e = d;
        while (e < src.size() && src[e] >= '0' && src[e] <= '9')
            ++e;
        if (e == d)
        {
            i = d;
            continue;
        }
        const int slot = std::stoi(src.substr(d, e - d));
        auto it = std::find(order.begin(), order.end(), slot);
        if (it == order.end())
            it = order.insert(order.end(), slot);
        out += "#" + std::to_string(it - order.begin());
        i = e;
    }
    return {std::move(out), std::move(order)};
}

/**
 * Per buffer, a hash of everything its texels depend on (the passes that make it, their values, the images and
 * externals they read), 0 when it can't be told. Equal keys across two plans mean equal texels, so a changed stack
 * keeps what it shares with the last one. externalKey gives an External buffer's key from its id.
 */
std::vector<uint64_t> contentKeys(const Prepared& p, const std::function<uint64_t(const BufferDesc&)>& externalKey)
{
    const Plan& plan = p.plan;
    std::vector<uint64_t> key(plan.buffers.size(), 0);
    std::map<int, int> bufferOfSlot;
    for (const BufferDesc& b : plan.buffers)
        bufferOfSlot[b.slot] = b.id;
    auto shape = [&](uint64_t h, const BufferDesc& b) {
        const size_t id = size_t(b.id);
        h = mix(h, uint64_t(p.sizes[id].first) << 32 | uint32_t(p.sizes[id].second));
        h = mix(h, p.half[id]);
        h = mix(h, uint64_t(b.type));
        return mixText(h, b.format);
    };
    auto value = [&](uint64_t h, int slot) {
        if (auto it = bufferOfSlot.find(slot); it != bufferOfSlot.end())
            return key[size_t(it->second)] ? mix(h, key[size_t(it->second)]) : 0;
        uint64_t bits[2] = {};
        if (size_t(slot) * 4 + 4 <= plan.uniforms.size())
            std::memcpy(bits, &plan.uniforms[size_t(slot) * 4], sizeof(bits));
        return mix(mix(h, bits[0]), bits[1]);
    };
    for (const BufferDesc& b : plan.buffers)
    {
        const size_t id = size_t(b.id);
        if (b.kind == BufferKind::Image)
            if (auto it = p.images.find(b.id); it != p.images.end() && it->second)
                key[id] = shape(mix(mixText(1, b.name), uint64_t(uintptr_t(it->second.get()))), b);
        if (b.kind == BufferKind::External)
            if (uint64_t k = externalKey(b))
                key[id] = shape(mix(mixText(2, b.name), k), b);
    }
    const uint64_t place =
        mix(mix(std::bit_cast<uint32_t>(p.inputs.uvOffset[0]), std::bit_cast<uint32_t>(p.inputs.uvOffset[1])), p.gpuForm ? 1 : 0);
    for (size_t i = 0; i < plan.passes.size(); ++i)
    {
        const Pass& pass = plan.passes[i];
        uint64_t h = mix(3, place);
        if (pass.kind == PassKind::Fused)
        {
            auto [text, slots] = slotFree(pass.source);
            h = mixText(h, text);
            for (int s : slots)
                if (h && !(h = value(h, s)))
                    break;
        }
        else
        {
            h = mixText(mix(h, i < p.routes.size() ? uint64_t(p.routes[i]) : 0), pass.nativeType);
            for (const auto& [id, b] : pass.inputBuffers)
                h = key[size_t(b)] ? mix(mixText(h, id), key[size_t(b)]) : 0;
        }
        for (int s : pass.slots)
            if (h)
                h = value(h, s);
        for (int r : pass.reads)
            h = h && key[size_t(r)] ? mix(h, key[size_t(r)]) : 0;
        if (h && pass.output >= 0)
            key[size_t(pass.output)] = shape(h, plan.buffers[size_t(pass.output)]);
    }
    for (size_t b = 0; b < key.size(); ++b)
        if (p.unused[b])
            key[b] = 0;
    return key;
}
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
    // A new structure (a layer added) still keeps the buffers the last plan computed that the new one makes the same way.
    std::vector<std::pair<int, int>> carried;
    std::vector<VkDeviceSize> carriedFrom;
    std::vector<uint64_t> carriedVersion;
    if (!keep && cur && arena.buffer && !arenaFresh && !trimmed && !packed && !cur->inputs.pages && !next->inputs.pages &&
        !next->inputs.compact && dirty.size() == cur->plan.passes.size() && load.size() == cur->plan.buffers.size())
    {
        std::vector<uint64_t> was = contentKeys(*cur, [&](const BufferDesc& b) -> uint64_t {
            const size_t id = size_t(b.id);
            return load[id] || loadTarget[id] ? 0 : loadedVersion[id];
        });
        for (size_t p = 0; p < cur->plan.passes.size(); ++p)
            if (dirty[p] && cur->plan.passes[p].output >= 0)
                was[size_t(cur->plan.passes[p].output)] = 0;
        for (const BufferDesc& b : cur->plan.buffers)
            if (b.kind == BufferKind::Image && load[size_t(b.id)])
                was[size_t(b.id)] = 0;
        std::unordered_map<uint64_t, int> byKey;
        for (size_t b = 0; b < was.size(); ++b)
            if (was[b])
                byKey.emplace(was[b], int(b));
        const std::vector<uint64_t> now = contentKeys(*next, [&](const BufferDesc& b) -> uint64_t {
            auto g = next->inputs.gpuExternals.find(b.name);
            return g != next->inputs.gpuExternals.end() ? versionOf(g->second) : 0;
        });
        for (size_t b = 0; b < now.size(); ++b)
            if (auto it = now[b] ? byKey.find(now[b]) : byKey.end(); it != byKey.end())
            {
                carried.emplace_back(it->second, int(b));
                carriedFrom.push_back(offset[size_t(it->second)]);
                carriedVersion.push_back(loadedVersion[size_t(it->second)]);
            }
    }
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
        regionQueue.clear();
        regionTargets.clear();
        uniformsDirty = true;
    }
    else
    {
        flushRegions();
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
    carry.clear();
    if (!packed && !carried.empty())
    {
        std::vector<int> producer(nBuffers, -1);
        for (size_t p = 0; p < nPasses; ++p)
            if (cur->plan.passes[p].output >= 0)
                producer[size_t(cur->plan.passes[p].output)] = int(p);
        for (size_t i = 0; i < carried.size(); ++i)
        {
            const size_t b = size_t(carried[i].second);
            carry.push_back({carriedFrom[i], offset[b], spans[b]});
            if (producer[b] >= 0)
                dirty[size_t(producer[b])] = 0;
            else
            {
                load[b] = 0;
                loadedVersion[b] = carriedVersion[i];
            }
        }
        std::scoped_lock lock(mutex);
        stats.passesDirty = size_t(std::count(dirty.begin(), dirty.end(), uint8_t(1)));
        stats.passesCarried = carried.size();
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
    {
        splitRegions();
        propagate({}, false);
    }
    maskPages();
    passIndex = 0;
    passRow = 0;
    passStarted = false;
    std::scoped_lock lock(mutex);
    stats.passesDirty = size_t(std::count(dirty.begin(), dirty.end(), uint8_t(1)));
}

namespace
{
/** Rectangles merged where they overlap, then the pair adding the least area while more than maxCount remain. */
std::vector<Rect> mergeRegions(std::vector<Rect> rects, size_t maxCount)
{
    auto area = [](const Rect& r) { return double(r.x1 - r.x0) * double(r.y1 - r.y0); };
    auto overlap = [](const Rect& a, const Rect& b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; };
    std::erase_if(rects, [](const Rect& r) { return r.empty(); });
    for (bool merged = true; merged;)
    {
        merged = false;
        for (size_t i = 0; i < rects.size() && !merged; ++i)
            for (size_t j = i + 1; j < rects.size() && !merged; ++j)
                if (overlap(rects[i], rects[j]))
                {
                    rects[i] = rects[i].unite(rects[j]);
                    rects.erase(rects.begin() + std::ptrdiff_t(j));
                    merged = true;
                }
    }
    while (rects.size() > maxCount)
    {
        double best = std::numeric_limits<double>::max();
        size_t bi = 0, bj = 1;
        for (size_t i = 0; i < rects.size(); ++i)
            for (size_t j = i + 1; j < rects.size(); ++j)
                if (const double waste = area(rects[i].unite(rects[j])) - area(rects[i]) - area(rects[j]); waste < best)
                {
                    best = waste;
                    bi = i;
                    bj = j;
                }
        rects[bi] = rects[bi].unite(rects[bj]);
        rects.erase(rects.begin() + std::ptrdiff_t(bj));
    }
    return rects;
}
} // namespace

void Evaluator::State::splitRegions()
{
    std::vector<Rect> parts = regionQueue;
    for (const BufferDesc& b : cur->plan.buffers)
    {
        const size_t id = size_t(b.id);
        if (b.kind != BufferKind::External || !load[id])
            continue;
        auto g = cur->inputs.gpuExternals.find(b.name);
        if (g == cur->inputs.gpuExternals.end() || !g->second)
            continue;
        std::optional<std::vector<std::array<int, 4>>> rects;
        if (loadTarget[id] != 0 && loadedImage.size() == loadedVersion.size() && loadedImage[id].first == g->second->key())
            rects = g->second->rectsSince(loadedImage[id].second);
        // A whole load, or changes not known one by one: one box over everything.
        if (!rects)
        {
            flushRegions();
            return;
        }
        for (const std::array<int, 4>& r : *rects)
            parts.push_back(Rect::texels(r, g->second->width(), g->second->height()));
        regionTargets[id] = loadTarget[id];
    }
    Rect box;
    for (const Rect& r : parts)
        box = box.unite(r);
    const std::vector<Rect> regions = mergeRegions(std::move(parts), 4);
    double split = 0.0;
    for (const Rect& r : regions)
        split += double(r.x1 - r.x0) * double(r.y1 - r.y0);
    // Each region costs a pass over the graph of its own: worth it only when it leaves much of the box out.
    if (regions.size() <= 1 || split > 0.6 * double(box.x1 - box.x0) * double(box.y1 - box.y0))
    {
        flushRegions();
        return;
    }
    for (const auto& [id, target] : regionTargets)
    {
        load[id] = 1;
        loadRect[id] = regions.front();
        loadTarget[id] = target;
    }
    regionQueue.assign(regions.begin() + 1, regions.end());
}

void Evaluator::State::nextRegion()
{
    const Rect r = regionQueue.front();
    regionQueue.erase(regionQueue.begin());
    for (const auto& [id, target] : regionTargets)
    {
        load[id] = 1;
        loadRect[id] = r;
        loadTarget[id] = target;
    }
    if (regionQueue.empty())
        regionTargets.clear();
    propagate({}, false);
    maskPages();
    passIndex = 0;
    passRow = 0;
    passStarted = false;
}

void Evaluator::State::flushRegions()
{
    if (regionQueue.empty())
    {
        regionTargets.clear();
        return;
    }
    Rect all;
    for (const Rect& r : regionQueue)
        all = all.unite(r);
    for (const auto& [id, target] : regionTargets)
    {
        loadRect[id] = load[id] ? loadRect[id].unite(all) : all;
        load[id] = 1;
        if (loadTarget[id] != 0)
            loadTarget[id] = target;
    }
    regionQueue.clear();
    regionTargets.clear();
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
            const auto near = pass.reach.find(b);
            if (pass.kind == PassKind::Fused && (near != pass.reach.end() || std::find(pass.local.begin(), pass.local.end(), b) != pass.local.end()))
            {
                // Bilinear reads reach a texel of the source either side, plus the output texel's own extent and any neighbour taps.
                const auto [bw, bh] = cur->sizes[size_t(b)];
                const float taps = near != pass.reach.end() ? float(near->second) : 0.0f;
                r = r.unite(src.grow((1.5f + taps) / float(bw) + 1.0f / float(ow), (1.5f + taps) / float(bh) + 1.0f / float(oh)));
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
        compute::Buffer old = arena;
        arena = compute::makeBuffer(vk, bytes, 0, compute::Host::None);
        reallocate = false;
        arenaFresh = carry.empty() || !old.buffer;
        if (!arenaFresh)
        {
            std::vector<VkBufferCopy> regions;
            for (const Carry& c : carry)
                regions.push_back({c.from * kTexel, c.to * kTexel, c.texels * kTexel});
            sub.run(vk, [&](VkCommandBuffer cmd) {
                vkCmdCopyBuffer(cmd, old.buffer, arena.buffer, uint32_t(regions.size()), regions.data());
                compute::memoryBarrier(cmd);
            });
        }
        compute::destroy(vk, old);
        carry.clear();
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
