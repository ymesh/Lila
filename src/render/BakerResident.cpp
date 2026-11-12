#include "render/Baker.h"
#include "render/BakerInternal.h"

namespace lila::render::detail
{
uint64_t GpuResident::table(const void* data, size_t bytes)
{
    mOwned.push_back(hostBuffer(mVk, data, bytes));
    return mOwned.back().address;
}

void GpuResident::initLows()
{
    mLows.clear();
    mLows.resize(mIn->groups.size());
    mLowTables.resize(mIn->groups.size());
    for (size_t g = 0; g < mIn->groups.size(); ++g)
    {
        mLows[g].resize(mIn->groups[g].lows.size());
        std::vector<GpuLowInfo> zero(std::max<size_t>(1, mIn->groups[g].lows.size()));
        mLowTables[g] = hostBuffer(mVk, zero.data(), zero.size() * sizeof(GpuLowInfo));
    }
}

void GpuResident::rebind(const bake::BakeInputs& inputs)
{
    LILA_ZONE();

    mB.runTask([this](VulkanContext&) {
        release(true);
        return true;
    });
    mKernels.clear();
    mTextures.clear();
    mNormalMapTables.clear();
    mCurvatureTables.clear();
    mMaterialTables.clear();
    mColorTables.clear();
    mIn = &inputs;
    initLows();
}

void GpuResident::clearTiles(const bake::TextureBuffer& tex, std::span<const uint32_t> tiles, int tile)
{
    LILA_ZONE();

    DevTex& d = texture(tex);
    if (tiles.empty())
    {
        mB.runTask([&](VulkanContext& vk) {
            mB.initDevice(vk);
            mB.mSub.run(vk, [&](VkCommandBuffer cmd) {
                for (GpuBuffer* buf : {&d.value, &d.weight, &d.count, &d.exact, &d.votes})
                    if (buf->buffer)
                        vkCmdFillBuffer(cmd, buf->buffer, 0, VK_WHOLE_SIZE, 0);
            });
            return true;
        });
        return;
    }
    if (tiles.size() > mTileListCap)
    {
        destroy(mVk, mTileList);
        mTileListCap = std::max<size_t>(tiles.size(), 1024);
        mTileList = makeBuffer(mVk, mTileListCap * sizeof(uint32_t), 0, Host::Write);
    }
    std::memcpy(mTileList.mapped, tiles.data(), tiles.size_bytes());
    vmaFlushAllocation(mVk.allocator, mTileList.alloc, 0, VK_WHOLE_SIZE);
    GpuImagePush q;
    q.value = d.value.address;
    q.weight = d.weight.address;
    q.count = d.count.address;
    q.exact = d.exact.address;
    q.votes = d.votes.address;
    q.seedsIn = mTileList.address;
    q.width = uint32_t(tex.width);
    q.height = uint32_t(tex.height);
    q.step = tile;
    q.shift = uint32_t((tex.width + tile - 1) / tile);
    q.mode = tex.def && tex.def->resolve == bake::Resolve::Exact ? 1u : 0u;
    GpuBackend& b = mB;
    std::vector<GpuStep> steps;
    steps.push_back({tiles.size() * size_t(tile) * size_t(tile), 1.0, &b.mImageRate, [&b, q](VkCommandBuffer cmd, uint32_t first, uint32_t count) {
                         vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.mImagePipes.at("clearTilesCS"));
                         dispatchChunks(cmd, b.mLayoutImage, q, first, count, 64, [](GpuImagePush& r, uint32_t f, uint32_t c) {
                             r.first = f;
                             r.total = f + c;
                         });
                     },
                     "clear"});
    b.runSteps(steps);
}

void GpuResident::drop(const bake::TextureBuffer& tex)
{
    auto it = mTex.find(&tex);
    if (it == mTex.end())
        return;
    mB.runTask([&](VulkanContext&) {
        for (GpuBuffer* buf : {&it->second.value, &it->second.weight, &it->second.count, &it->second.exact, &it->second.votes})
            destroy(mVk, *buf);
        return true;
    });
    mTex.erase(it);
}

void GpuResident::writeImage(std::vector<GpuStep>& steps, const GpuImagePush& base, GpuImage& image)
{
    GpuBackend& b = mB;
    const size_t n = size_t(base.width) * size_t(base.height);
    GpuImagePush q = base;
    q.seedsOut = mPack.address;
    size_t items = n;
    switch (image.format())
    {
    case TextureFormat::RGBA16F: q.mode = 0; break;
    case TextureFormat::R16F:
        q.mode = 1;
        items = (n + 1) / 2;
        break;
    case TextureFormat::RGBA8: q.mode = 2; break;
    default: throw std::invalid_argument("GPU bake: images take RGBA16F, R16F or RGBA8");
    }
    steps.push_back({items, 1.0, &b.mImageRate, [&b, q](VkCommandBuffer cmd, uint32_t first, uint32_t count) {
                         vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.mImagePipes.at("packTexelsCS"));
                         dispatchChunks(cmd, b.mLayoutImage, q, first, count, 64, [](GpuImagePush& r, uint32_t f, uint32_t c) {
                             r.first = f;
                             r.total = f + c;
                         });
                     },
                     "write"});
    steps.push_back({1, 0.0, nullptr, [this, &image](VkCommandBuffer cmd, uint32_t, uint32_t) {
                         if (!image.prepare(mVk, cmd, mStaging))
                             throw std::runtime_error("GPU bake: cannot make the image " + image.desc().name);
                         image.beginWrite(cmd);
                         memoryBarrier(cmd);
                         VkBufferImageCopy region{};
                         region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                         region.imageExtent = {uint32_t(image.width()), uint32_t(image.height()), 1};
                         vkCmdCopyBufferToImage(cmd, mPack.buffer, image.image(), VK_IMAGE_LAYOUT_GENERAL, 1, &region);
                         memoryBarrier(cmd);
                         image.endWrite(cmd);
                     },
                     "write"});
}

void GpuResident::release(bool keepTextures)
{
    for (auto& per : mLows)
        for (auto& l : per)
            if (l)
                for (GpuBuffer* b : {&l->positions, &l->normals, &l->tangents, &l->uvs, &l->cageNormals, &l->cageScale, &l->cageSkew, &l->cagePositions,
                                     &l->materials, &l->edges})
                    destroy(mVk, *b);
    mLows.clear();
    for (GpuBuffer& b : mLowTables)
        destroy(mVk, b);
    mLowTables.clear();
    for (GpuBuffer& b : mOwned)
        destroy(mVk, b);
    mOwned.clear();
    if (keepTextures)
        return;
    for (auto& [k, t] : mTex)
        for (GpuBuffer* b : {&t.value, &t.weight, &t.count, &t.exact, &t.votes})
            destroy(mVk, *b);
    mTex.clear();
    for (DevTex& t : mSpareTex)
        for (GpuBuffer* b : {&t.value, &t.weight, &t.count, &t.exact, &t.votes})
            destroy(mVk, *b);
    mSpareTex.clear();
    for (GpuBuffer* b : {&mClaim, &mSamples, &mHits, &mOutBuf, &mValid, &mItems, &mBlocks, &mStats, &mStatsRead, &mSeeds[0], &mSeeds[1], &mScratch, &mReduce,
                         &mReduceRead, &mReadback, &mWorkValue, &mWorkCount, &mPack, &mTileList})
        destroy(mVk, *b);
    for (GpuBuffer& b : mStaging)
        destroy(mVk, b);
    mStaging.clear();
}

GpuResident::DevLow& GpuResident::low(size_t g, size_t l)
{
    std::unique_ptr<DevLow>& slot = mLows[g][l];
    if (slot)
        return *slot;
    LILA_ZONE();

    const bake::LowInput& in = mIn->groups[g].lows[l];
    const bake::MeshSoup& s = in.soup;
    auto d = std::make_unique<DevLow>();
    ResidentUploads up{mB};
    up.add(d->positions, s.positions.data(), s.positions.size() * sizeof(glm::vec3));
    up.add(d->normals, s.normals.data(), s.normals.size() * sizeof(glm::vec3));
    up.add(d->tangents, in.tangents.data(), in.tangents.size() * sizeof(glm::vec4));
    up.add(d->uvs, s.uvs.data(), s.uvs.size() * sizeof(glm::vec2));
    up.add(d->cageNormals, in.cageNormals.data(), in.cageNormals.size() * sizeof(glm::vec3));
    up.add(d->cageScale, in.cageScale.data(), in.cageScale.size() * sizeof(float));
    const bool skew = in.cageSkew.size() == s.positions.size();
    if (skew)
        up.add(d->cageSkew, in.cageSkew.data(), in.cageSkew.size() * sizeof(float));
    const bool cage = in.cagePositions.size() == s.positions.size();
    if (cage)
        up.add(d->cagePositions, in.cagePositions.data(), in.cagePositions.size() * sizeof(glm::vec3));
    up.add(d->materials, s.materials.data(), s.materials.size() * sizeof(uint32_t));
    std::vector<uint32_t> edges;
    if (mNeedEdges)
    {
        const std::vector<uint8_t> wire = bake::lowEdgeFlags(in, false), border = bake::lowEdgeFlags(in, true);
        edges.resize(s.triangleCount());
        for (size_t t = 0; t < edges.size(); ++t)
            edges[t] = uint32_t(wire[t]) | (uint32_t(border[t]) << 3);
        up.add(d->edges, edges.data(), edges.size() * sizeof(uint32_t));
    }
    up.flush();
    GpuLowInfo info;
    info.positions = d->positions.address;
    info.normals = d->normals.address;
    info.tangents = d->tangents.address;
    info.uvs = d->uvs.address;
    info.cageNormals = d->cageNormals.address;
    info.cageScale = d->cageScale.address;
    info.cageSkew = d->cageSkew.address;
    info.cagePositions = d->cagePositions.address;
    info.materials = d->materials.address;
    info.edges = d->edges.address;
    info.triangles = s.triangleCount();
    info.hasSkew = skew ? 1u : 0u;
    std::memcpy(static_cast<GpuLowInfo*>(mLowTables[g].mapped) + l, &info, sizeof info);
    vmaFlushAllocation(mVk.allocator, mLowTables[g].alloc, 0, VK_WHOLE_SIZE);
    slot = std::move(d);
    return *slot;
}

GpuResident::DevTex& GpuResident::texture(const bake::TextureBuffer& tex)
{
    auto it = mTex.find(&tex);
    if (it != mTex.end())
        return it->second;
    LILA_ZONE();

    const size_t n = tex.texelCount();
    const bool exact = tex.def && tex.def->resolve == bake::Resolve::Exact;
    DevTex d;
    auto spare = std::ranges::find_if(mSpareTex, [&](const DevTex& t) { return t.value.size == std::max<VkDeviceSize>(n * sizeof(glm::vec4), 16) && (t.exact.buffer != VK_NULL_HANDLE) == exact; });
    if (spare != mSpareTex.end())
    {
        d = *spare;
        mSpareTex.erase(spare);
    }
    else
    {
        if (!mSpareTex.empty())
        {
            for (GpuBuffer* b : {&mSpareTex.back().value, &mSpareTex.back().weight, &mSpareTex.back().count, &mSpareTex.back().exact, &mSpareTex.back().votes})
                destroy(mVk, *b);
            mSpareTex.pop_back();
        }
        d.value = makeBuffer(mVk, n * sizeof(glm::vec4), 0, Host::None);
        d.weight = makeBuffer(mVk, n * sizeof(float), 0, Host::None);
        d.count = makeBuffer(mVk, n * sizeof(uint32_t), 0, Host::None);
        if (exact)
        {
            d.exact = makeBuffer(mVk, n * sizeof(uint32_t), 0, Host::None);
            d.votes = makeBuffer(mVk, n * sizeof(uint32_t), 0, Host::None);
        }
    }
    d.id = mNextTexId++;
    mB.runTask([&](VulkanContext& vk) {
        mB.initDevice(vk);
        mB.mSub.run(vk, [&](VkCommandBuffer cmd) {
            for (GpuBuffer* b : {&d.value, &d.weight, &d.count, &d.exact, &d.votes})
                if (b->buffer)
                    vkCmdFillBuffer(cmd, b->buffer, 0, VK_WHOLE_SIZE, 0);
        });
        return true;
    });
    return mTex.emplace(&tex, std::move(d)).first->second;
}

GpuTextureRef GpuResident::loadTexture(const std::string& path)
{
    if (path.empty())
        return {};
    auto it = mTextures.find(path);
    if (it != mTextures.end())
        return it->second;
    GpuTextureRef ref;
    int w = 0, h = 0;
    std::vector<float> rgba;
    std::string err;
    if (bake::loadBakeTexture(path, w, h, rgba, err))
    {
        ResidentUploads up{mB};
        mOwned.emplace_back();
        up.add(mOwned.back(), rgba.data(), size_t(w) * size_t(h) * 4 * sizeof(float));
        up.flush();
        ref.texels = mOwned.back().address;
        ref.width = uint32_t(w);
        ref.height = uint32_t(h);
    }
    else
        log::warn("bake: can't read texture {}{}", path, err.empty() ? std::string() : ": " + err);
    mTextures.emplace(path, ref);
    return ref;
}

void GpuResident::needHighSurfaces(size_t g, ResidentUploads& up)
{
    GpuBackend& b = mB;
    bool changed = false;
    for (size_t h = 0; h < b.mGroupHighs[g]; ++h)
    {
        const size_t k = b.mGroupBase[g] + h;
        GpuHigh& gh = *b.mHighs[k];
        const bake::MeshSoup& soup = mIn->groups[g].highs[h].soup;
        if (!gh.positions.buffer && !soup.positions.empty())
        {
            up.add(gh.positions, soup.positions.data(), soup.positions.size() * sizeof(glm::vec3));
            b.mHighInfoHost[k].positions = gh.positions.address;
            changed = true;
        }
        if (!gh.uvs.buffer && !soup.uvs.empty())
        {
            up.add(gh.uvs, soup.uvs.data(), soup.uvs.size() * sizeof(glm::vec2));
            b.mHighInfoHost[k].uvs = gh.uvs.address;
            changed = true;
        }
    }
    if (!changed)
        return;
    up.flush();
    std::memcpy(b.mHighInfo.mapped, b.mHighInfoHost.data(), b.mHighInfoHost.size() * sizeof(GpuHighInfo));
    vmaFlushAllocation(mVk.allocator, b.mHighInfo.alloc, 0, VK_WHOLE_SIZE);
}

uint64_t GpuResident::normalMapTable(size_t g)
{
    if (auto it = mNormalMapTables.find(g); it != mNormalMapTables.end())
        return it->second;
    const bake::GroupInputs& gi = mIn->groups[g];
    std::vector<GpuTextureRef> refs;
    bool any = false;
    for (size_t h = 0; h < gi.highs.size(); ++h)
    {
        const bake::MeshSoup& soup = gi.highs[h].soup;
        const GpuHigh& gh = *mB.mHighs[mB.mGroupBase[g] + h];
        const uint32_t materialBase = mB.mMaterialBase[mB.mGroupBase[g] + h];
        refs.resize(std::max(refs.size(), size_t(materialBase) + gh.slotTriangle.size() + 1));
        // A self high's material normal map is usually this bake's last output (see bake::prepareHighNormalMaps).
        if (soup.uvs.empty() || (h < gi.highSelfLow.size() && gi.highSelfLow[h] >= 0))
            continue;
        for (size_t m = 0; m < soup.materialNames.size() && m < gh.slotTriangle.size(); ++m)
        {
            auto mat = mIn->materials.find(soup.materialNames[m]);
            if (mat == mIn->materials.end() || mat->second.normalMap.empty())
                continue;
            GpuTextureRef r = loadTexture(mat->second.normalMap);
            r.flags = mat->second.normalOpenGL ? 1u : 0u;
            refs[materialBase + m] = r;
            any = any || r.width;
        }
    }
    uint64_t address = 0;
    if (any)
    {
        ResidentUploads up{mB};
        needHighSurfaces(g, up);
        address = table(refs.data(), refs.size() * sizeof(GpuTextureRef));
    }
    mNormalMapTables[g] = address;
    return address;
}

uint64_t GpuResident::materialTable(size_t g, const std::string& map)
{
    const auto key = std::make_pair(g, map);
    if (auto it = mMaterialTables.find(key); it != mMaterialTables.end())
        return it->second;
    const bake::GroupInputs& gi = mIn->groups[g];
    static const bake::MaterialInput kNeutral;
    std::vector<GpuMaterialSlot> slots;
    bool textured = false;
    for (size_t h = 0; h < gi.highs.size(); ++h)
    {
        const bake::MeshSoup& soup = gi.highs[h].soup;
        const GpuHigh& gh = *mB.mHighs[mB.mGroupBase[g] + h];
        const uint32_t materialBase = mB.mMaterialBase[mB.mGroupBase[g] + h];
        const size_t count = gh.slotTriangle.size();
        slots.resize(std::max(slots.size(), size_t(materialBase) + count + 1));
        for (size_t m = 0; m <= count; ++m)
        {
            GpuMaterialSlot& slot = slots[materialBase + m];
            const bool named = m < soup.materialNames.size() && m < count;
            auto it = named ? mIn->materials.find(soup.materialNames[m]) : mIn->materials.end();
            const bake::MaterialInput& mat = it != mIn->materials.end() ? it->second : kNeutral;
            slot.baseColor = glm::vec4(mat.baseColor, 1.0f);
            slot.emissive = glm::vec4(mat.emissive, float(mat.blendMode));
            slot.scalars = glm::vec4(mat.roughness, mat.metallic, mat.opacity, mat.emissiveIntensity);
            if (!named || soup.uvs.empty())
                continue;
            const bake::TransferSource ts = bake::transferSource(mat, map);
            GpuTextureRef ref = loadTexture(ts.texture);
            slot.channel = uint32_t(ts.channel);
            slot.srgb = ts.srgb ? 1u : 0u;
            if (!ref.width && !ts.fallback.empty())
            {
                ref = loadTexture(ts.fallback);
                slot.channel = uint32_t(ts.fallbackChannel);
                slot.srgb = ts.fallbackSrgb ? 1u : 0u;
            }
            slot.texture = ref;
            textured = textured || ref.width;
        }
    }
    if (textured)
    {
        ResidentUploads up{mB};
        needHighSurfaces(g, up);
    }
    const uint64_t address = table(slots.data(), std::max<size_t>(1, slots.size()) * sizeof(GpuMaterialSlot));
    mMaterialTables[key] = address;
    return address;
}

uint64_t GpuResident::curvatureTable(size_t g)
{
    if (auto it = mCurvatureTables.find(g); it != mCurvatureTables.end())
        return it->second;
    GpuBackend& b = mB;
    const bake::GroupInputs& gi = mIn->groups[g];
    std::vector<GpuCornerTable> entries(std::max<size_t>(1, gi.highs.size()));
    std::vector<std::shared_ptr<const std::vector<float>>> corner(gi.highs.size());
    tbb::parallel_for(size_t(0), gi.highs.size(), [&](size_t h) { corner[h] = bake::highCurvature(gi.highs[h].soup); });
    ResidentUploads up{mB};
    for (size_t h = 0; h < gi.highs.size(); ++h)
    {
        GpuHigh& gh = *b.mHighs[b.mGroupBase[g] + h];
        if (!gh.curvature.buffer && corner[h] && !corner[h]->empty())
            up.add(gh.curvature, corner[h]->data(), corner[h]->size() * sizeof(float));
    }
    up.flush();
    for (size_t h = 0; h < gi.highs.size(); ++h)
    {
        const GpuHigh& gh = *b.mHighs[b.mGroupBase[g] + h];
        entries[h] = {gh.curvature.address, gh.curvature.buffer ? 1u : 0u, 0u};
    }
    const uint64_t address = table(entries.data(), entries.size() * sizeof(GpuCornerTable));
    mCurvatureTables[g] = address;
    return address;
}

uint64_t GpuResident::colorTable(size_t g, const std::string& set, bool first)
{
    const auto key = std::make_pair(g, (first ? std::string("\x01") : std::string("\x02")) + set);
    if (auto it = mColorTables.find(key); it != mColorTables.end())
        return it->second;
    GpuBackend& b = mB;
    const bake::GroupInputs& gi = mIn->groups[g];
    std::vector<GpuCornerTable> entries(std::max<size_t>(1, gi.highs.size()));
    ResidentUploads up{mB};
    std::vector<std::string> names(gi.highs.size());
    for (size_t h = 0; h < gi.highs.size(); ++h)
    {
        const bake::MeshSoup& soup = gi.highs[h].soup;
        // colorSet in MapsMaterial.cpp: the named set (empty: the first); the ID map's vertex colours read the first.
        const std::vector<glm::vec4>* c = nullptr;
        if (first)
            c = soup.colors.empty() ? nullptr : &soup.colors;
        else
            for (size_t k = 0; k < soup.colorSetNames.size() && !c; ++k)
                if (set.empty() || soup.colorSetNames[k] == set)
                {
                    const std::vector<glm::vec4>& v = k ? soup.moreColors[k - 1] : soup.colors;
                    c = v.empty() ? nullptr : &v;
                    if (!c)
                        break;
                }
        if (!c)
            continue;
        names[h] = first ? std::string() : set;
        GpuHigh& gh = *b.mHighs[b.mGroupBase[g] + h];
        GpuBuffer& buf = gh.colors[(first ? std::string("\x01") : std::string("\x02")) + set];
        if (!buf.buffer)
            up.add(buf, c->data(), c->size() * sizeof(glm::vec4));
    }
    up.flush();
    for (size_t h = 0; h < gi.highs.size(); ++h)
    {
        GpuHigh& gh = *b.mHighs[b.mGroupBase[g] + h];
        auto it = gh.colors.find((first ? std::string("\x01") : std::string("\x02")) + set);
        if (it != gh.colors.end() && it->second.buffer)
            entries[h] = {it->second.address, 1u, 0u};
    }
    const uint64_t address = table(entries.data(), entries.size() * sizeof(GpuCornerTable));
    mColorTables[key] = address;
    return address;
}

void GpuResident::scope(Kernel& k, size_t g, const std::string& name, VkPipeline sw, VkPipeline hw)
{
    GpuBackend& b = mB;
    const bake::OccluderScope s = bake::occluderScope(name);
    k.pipe = b.mHardware ? hw : sw;
    k.layout = b.mHardware ? b.mLayoutHw : b.mLayoutSw;
    k.rate = &b.mRayRate;
    if (s == bake::OccluderScope::Self)
        k.push.flags |= kFlagScopeSelf;
    if (s == bake::OccluderScope::All)
    {
        k.push.highs = b.mHighInfo.address;
        k.push.highCount = uint32_t(b.mHighs.size());
    }
    const bool global = s == bake::OccluderScope::All || (b.mHardware && !b.mGroupTlas[g].as);
    b.useLinks(k.push, g, global);
    if (!b.mHardware)
        return;
    if (global)
    {
        b.runTask([&](VulkanContext& vk) {
            b.initDevice(vk);
            b.allTlas(vk);
            return true;
        });
        k.set = b.mAll.set;
        k.push.instanceBase = b.mAll.instanceBase.address;
        return;
    }
    k.set = b.mGroupTlas[g].set;
    k.push.instanceBase = b.mGroupTlas[g].instanceBase.address;
}

std::unique_ptr<bake::ResidentBake> GpuBackend::beginResident(const bake::BakeInputs& inputs, const std::vector<bake::ResidentMap>& maps,
                                                              const bake::OutputSettings& output)
{
    if (mFailed || mInputs != &inputs || maps.empty())
        return nullptr;
    try
    {
        runTask([this](VulkanContext& vk) {
            initDevice(vk);
            return true;
        });
    }
    catch (const std::exception& e)
    {
        fail(e.what());
        return nullptr;
    }
    {
        std::scoped_lock lock(mGpuTimeMutex);
        mGpuTimes.clear();
    }
    return std::make_unique<GpuResident>(*this, inputs, maps, output);
}
} // namespace lila::render::detail
