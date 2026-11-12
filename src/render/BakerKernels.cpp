#include "render/Baker.h"
#include "render/BakerInternal.h"

namespace lila::render::detail
{
namespace
{
/** How a resident texture finishes after its resolve. */
enum class Finish
{
    /** No finalize, or one that only writes meta (run on the CPU after the read back). */
    None,
    Renormalize,
    Height,
    Thickness,
    Curvature,
    /** The finalize runs on the CPU (normal to height's Poisson solve), and so does its dilation. */
    Cpu,
};

Finish finishOf(const std::string& id)
{
    if (id == "normal" || id == "objectNormal" || id == "bentNormal" || id == "roundedNormal")
        return Finish::Renormalize;
    if (id == "height")
        return Finish::Height;
    if (id == "thickness")
        return Finish::Thickness;
    if (id == "curvature" || id == "normalToCurvature" || id == "convexity" || id == "cavity" || id == "edgeWear")
        return Finish::Curvature;
    if (id == "normalToHeight")
        return Finish::Cpu;
    return Finish::None;
}

/** The curvature maps' source, as sourceOf in MapsSurface.cpp. */
bool curvatureFromMesh(const bake::MapSettings& s)
{
    const char* fallback = s.map == "edgeWear" || s.map == "normalToCurvature" ? "normals" : "mesh";
    if (s.map == "normalToCurvature")
        return false;
    const std::string v = s.options.is_object() ? s.options.value("source", std::string(fallback)) : std::string(fallback);
    return !(v == "normals" || v == "normal");
}

template <class T> T option(const bake::json& o, const char* key, T fallback)
{
    return o.is_object() ? o.value(key, fallback) : fallback;
}
} // namespace

const GpuResident::Kernel& GpuResident::kernel(size_t m, size_t g)
{
    const auto key = std::make_pair(m, g);
    auto it = mKernels.find(key);
    if (it == mKernels.end())
        it = mKernels.emplace(key, makeKernel(m, g)).first;
    return it->second;
}

GpuResident::Kernel GpuResident::makeKernel(size_t m, size_t g)
{
    LILA_ZONE();

    GpuBackend& b = mB;
    const bake::ResidentMap& rm = mMaps[m];
    const std::string& id = rm.def->id;
    const bake::json& o = rm.settings.options;
    const bake::GroupInputs& gi = mIn->groups[g];
    const float diag = glm::length(gi.highMax - gi.highMin);
    const float eps = std::max(1e-4f * diag, 1e-4f);
    Kernel k;
    GpuBakePush& p = k.push;
    p.flags = kFlagResident;
    p.highs = b.mHighInfo.address + b.mGroupBase[g] * sizeof(GpuHighInfo);
    p.highList = b.mIota.address;
    p.highCount = uint32_t(b.mGroupHighs[g]);
    p.lows = mLowTables[g].address;
    p.boundsMin = glm::vec4(mIn->highMin, 0.0f);
    p.boundsExt = glm::vec4(mIn->highMax - mIn->highMin, 0.0f);
    p.width = uint32_t(mOut.width);
    p.height = uint32_t(mOut.height);
    k.pipe = b.mShade;
    k.layout = b.mLayoutSw;
    k.rate = &b.mShadeRate;
    auto normalMaps = [&] {
        if (!option(o, "highNormalMap", false))
            return;
        if (const uint64_t t = normalMapTable(g))
        {
            p.flags |= kFlagNormalMaps;
            p.normalMaps = t;
        }
    };
    auto frame = [&] {
        p.flags |= (option(o, "greenUp", false) ? kFlagGreenUp : 0u) | (option(o, "flipX", false) ? kFlagFlipX : 0u);
        if (option(o, "space", std::string("tangent")) == "object")
            p.flags |= kFlagObjectSpace;
    };
    auto distance = [&](const char* key) {
        const double d = option(o, key, 0.0);
        return d > 0.0 ? float(d) : std::max(diag, 1e-3f);
    };
    const std::string groupScope = option(o, "scope", gi.settings.aoScope);
    if (id == "normal" || id == "objectNormal")
    {
        p.map = id == "normal" ? kMapNormal : kMapObjectNormal;
        if (id == "normal")
            frame();
        normalMaps();
    }
    else if (id == "position")
        p.map = kMapPosition;
    else if (id == "height")
        p.map = kMapHeight;
    else if (id == "alpha")
        p.map = kMapAlpha;
    else if (id == "uvIslands")
    {
        p.map = kMapIslands;
        p.tableBase = b.mIslands[g].address;
        p.table = b.mIslands[g].address + gi.lows.size() * sizeof(uint32_t);
    }
    else if (id == "id")
    {
        if (option(o, "source", std::string("material")) == "vertexColor")
        {
            p.map = kMapIdVertexColor;
            p.extra = colorTable(g, std::string(), true);
        }
        else
        {
            p.map = kMapId;
            const bake::MapContext ctx{rm.settings, *mIn, gi, g, nullptr, mOut.width, mOut.height, nullptr};
            p.table = b.idTable(*rm.def, ctx).address;
        }
    }
    else if (id == "curvature" || id == "normalToCurvature" || id == "convexity" || id == "cavity" || id == "edgeWear" || id == "normalToHeight")
    {
        if (id != "normalToHeight" && curvatureFromMesh(rm.settings))
        {
            p.map = kMapCurvatureMesh;
            p.extra = curvatureTable(g);
        }
        else
        {
            p.map = kMapSlopes;
            normalMaps();
        }
    }
    else if (id == "vertexColor" || (id == "baseColor" && option(o, "source", std::string("material")) == "vertexColor"))
    {
        p.map = kMapVertexColor;
        p.extra = colorTable(g, option(o, "set", std::string()), false);
    }
    else if (id == "baseColor" || id == "roughness" || id == "metallic" || id == "emissive" || id == "materialOpacity")
    {
        p.map = kMapMaterial;
        p.mode = id == "baseColor" ? 0u : id == "roughness" ? 1u : id == "metallic" ? 2u : id == "emissive" ? 3u : 4u;
        p.extra = materialTable(g, id);
    }
    else if (id == "wireframe" || id == "islandBorder")
    {
        p.map = kMapEdges;
        p.mode = id == "wireframe" ? 0u : 1u;
        p.params.x = 0.5f * std::max(0.0f, option(o, "width", id == "wireframe" ? 1.0f : 2.0f));
    }
    else if (id == "gradient")
    {
        p.map = kMapGradient;
        glm::vec3 d(0.0f, 1.0f, 0.0f);
        const bake::json dir = option(o, "direction", bake::json());
        if (dir.is_array() && dir.size() >= 3)
            d = glm::vec3(dir[0].get<float>(), dir[1].get<float>(), dir[2].get<float>());
        const float dl = glm::length(d);
        d = dl > 1e-20f ? d / dl : glm::vec3(0.0f, 1.0f, 0.0f);
        const bool all = option(o, "bounds", std::string("group")) == "all";
        const glm::vec3 lo = all ? mIn->highMin : gi.highMin, hi = all ? mIn->highMax : gi.highMax;
        float mn = 1e30f, mx = -1e30f;
        for (int c = 0; c < 8; ++c)
        {
            const glm::vec3 q((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z);
            mn = std::min(mn, glm::dot(q, d));
            mx = std::max(mx, glm::dot(q, d));
        }
        p.params = glm::vec4(d, 0.0f);
        p.params2 = glm::vec4(mn, mx - mn, 0.0f, 0.0f);
    }
    else if (id == "ao")
    {
        p.rays = uint32_t(std::clamp(option(o, "rays", 64), 1, 4096));
        p.boundsMin.w = distance("distance");
        p.boundsExt.w = eps;
        scope(k, g, groupScope, b.mAoSw, b.mAoHw);
        k.cost = double(p.rays);
    }
    else if (id == "thickness")
    {
        p.rays = uint32_t(std::clamp(option(o, "rays", 64), 1, 4096));
        const float half = glm::radians(std::clamp(option(o, "spread", 180.0f), 0.0f, 180.0f)) * 0.5f;
        p.params = glm::vec4(distance("distance"), eps, half, 0.0f);
        scope(k, g, groupScope, b.mThicknessSw, b.mThicknessHw);
        k.cost = double(p.rays) * 1.5;
    }
    else if (id == "bentNormal")
    {
        p.rays = uint32_t(std::clamp(option(o, "rays", 64), 1, 4096));
        p.params = glm::vec4(distance("distance"), eps, 0.0f, 0.0f);
        frame();
        scope(k, g, groupScope, b.mBentSw, b.mBentHw);
        k.cost = double(p.rays);
    }
    else if (id == "roundedNormal")
    {
        p.rays = uint32_t(std::clamp(option(o, "rays", 32), 1, 4096));
        const double r = option(o, "radius", 0.0);
        p.params.x = r > 0.0 ? float(r) : std::max(0.01f * diag, 1e-3f);
        frame();
        scope(k, g, option(o, "scope", std::string("group")), b.mRoundedSw, b.mRoundedHw);
        k.cost = double(p.rays) * 3.0;
    }
    else if (id == "error")
    {
        const double beyond = option(o, "beyond", 0.0);
        p.params.x = beyond > 0.0 ? float(beyond) : std::max(2.0f * diag, 1e-3f);
        scope(k, g, "group", b.mErrorSw, b.mErrorHw);
        k.cost = 4.0;
    }
    return k;
}

void GpuResident::reserveBand(size_t slots, size_t items, size_t blocks)
{
    if (slots > mSlotCap)
    {
        for (GpuBuffer* b : {&mClaim, &mSamples, &mHits, &mOutBuf, &mValid})
            destroy(mVk, *b);
        mClaim = makeBuffer(mVk, slots * sizeof(uint32_t), 0, Host::None);
        mSamples = makeBuffer(mVk, slots * sizeof(TexelSample), 0, Host::None);
        mHits = makeBuffer(mVk, slots * sizeof(HitRecord), 0, Host::None);
        mOutBuf = makeBuffer(mVk, slots * sizeof(glm::vec4), 0, Host::None);
        mValid = makeBuffer(mVk, slots * sizeof(uint32_t), 0, Host::None);
        mSlotCap = slots;
    }
    if (items > mItemCap || !mItems.buffer)
    {
        destroy(mVk, mItems);
        mItemCap = std::max<size_t>(items, 4096);
        mItems = makeBuffer(mVk, mItemCap * 4 * sizeof(uint32_t), 0, Host::Write);
    }
    if (blocks > mBlockCap || !mBlocks.buffer)
    {
        destroy(mVk, mBlocks);
        mBlockCap = std::max<size_t>(blocks, 4096);
        mBlocks = makeBuffer(mVk, mBlockCap * sizeof(uint32_t), 0, Host::Write);
    }
    if (!mStats.buffer)
    {
        mStats = makeBuffer(mVk, 32, 0, Host::None);
        mStatsRead = makeBuffer(mVk, 32, 0, Host::Read);
    }
}

bake::ResidentStats GpuResident::bakeTile(const bake::ResidentTile& tile)
{
    LILA_ZONE();

    GpuBackend& b = mB;
    const size_t g = tile.group;
    const bake::TexelRaster& raster = *tile.raster;
    const int W = raster.settings().width, H = raster.settings().height, rows = raster.settings().bandRows;
    const uint32_t grid = uint32_t(raster.gridSize()), S = grid * grid;
    const bake::LowInput& lowIn = mIn->groups[g].lows[tile.low];
    const auto setupStarted = std::chrono::steady_clock::now();
    low(g, tile.low);
    const uint64_t lowInfo = mLowTables[g].address + tile.low * sizeof(GpuLowInfo);

    // Per map: its kernel and where each low material slot's samples accumulate.
    struct Job
    {
        const Kernel* kernel;
        uint64_t targets;
        std::string label;
    };
    std::vector<Job> jobs;
    for (size_t m = 0; m < mMaps.size() && m < tile.targets.size(); ++m)
    {
        const std::vector<bake::TextureBuffer*>& t = tile.targets[m];
        if (std::ranges::none_of(t, [](const bake::TextureBuffer* x) { return x != nullptr; }))
            continue;
        uint32_t slots = uint32_t(t.size());
        for (uint32_t mat : lowIn.soup.materials)
            slots = std::max(slots, mat + 1);
        std::vector<GpuAccTarget> targets(slots);
        for (size_t mat = 0; mat < t.size(); ++mat)
            if (t[mat])
            {
                DevTex& d = texture(*t[mat]);
                const bake::Resolve r = t[mat]->def ? t[mat]->def->resolve : bake::Resolve::Average;
                targets[mat] = {d.value.address, d.weight.address, d.count.address, d.exact.address, d.votes.address,
                                r == bake::Resolve::Exact ? 1u : r == bake::Resolve::Coverage ? 2u : 0u, d.id};
            }
        jobs.push_back({&kernel(m, g), table(targets.data(), targets.size() * sizeof(GpuAccTarget)), "map:" + mMaps[m].def->id});
    }

    mSetupSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - setupStarted).count();
    const int udim = tile.udim;
    const int tileU = (udim - 1001) % 10, tileV = (udim - 1001) / 10;
    const bool cullBack = mIn->groups[g].settings.ignoreBackfaces;
    const bool traceable = b.mGroupHighs[g] > 0 && (!b.mHardware || b.mGroupTlas[g].as);
    bake::ResidentStats stats;
    std::vector<uint32_t> items, blocks, blockIndex;
    const int blockCols = (W + 7) / 8;
    // Texel tiles: which the mask lets through, and which had work.
    const int maskTile = std::max(8, tile.maskTile / 8 * 8);
    const int tileCols = (W + maskTile - 1) / maskTile, tileRows = (H + maskTile - 1) / maskTile;
    const std::vector<uint8_t>* mask = tile.mask && tile.mask->size() == size_t(tileCols) * size_t(tileRows) ? tile.mask : nullptr;
    std::vector<uint8_t> worked(size_t(tileCols) * size_t(tileRows), 0);
    for (size_t band = 0; band < raster.bandCount(); ++band)
    {
        const int y0 = int(band) * rows, y1 = std::min(H, y0 + rows);
        const std::vector<uint32_t>& tris = raster.bandTriangles(udim, band);
        if (tris.empty() || jobs.empty())
        {
            if (tile.onBand)
                tile.onBand(y1 - y0);
            continue;
        }
        // 8 x 8 texel blocks of each triangle's bounds in the band, as TexelRaster::band walks them. Only blocks some
        // triangle touches get slots, so a low covering little of the texture costs little.
        items.clear();
        blocks.clear();
        const int blockRow0 = y0 / 8;
        blockIndex.assign(size_t(blockCols) * size_t((y1 - 1) / 8 - blockRow0 + 1), UINT32_MAX);
        for (uint32_t t : tris)
        {
            const glm::vec2* uv = &lowIn.soup.uvs[size_t(t) * 3];
            glm::dvec2 q[3];
            for (int k = 0; k < 3; ++k)
                q[k] = {(double(uv[k].x) - tileU) * W, (1.0 - (double(uv[k].y) - tileV)) * H};
            const double area = (q[1].x - q[0].x) * (q[2].y - q[0].y) - (q[1].y - q[0].y) * (q[2].x - q[0].x);
            if (std::abs(area) < 1e-12)
                continue;
            const int xa = std::clamp(int(std::floor(std::min({q[0].x, q[1].x, q[2].x}))), 0, W);
            const int xb = std::clamp(int(std::ceil(std::max({q[0].x, q[1].x, q[2].x}))), 0, W);
            const int ya = std::clamp(int(std::floor(std::min({q[0].y, q[1].y, q[2].y}))), y0, y1);
            const int yb = std::clamp(int(std::ceil(std::max({q[0].y, q[1].y, q[2].y}))), y0, y1);
            if (xa >= xb || ya >= yb)
                continue;
            for (int by = ya / 8; by <= (yb - 1) / 8; ++by)
                for (int bx = xa / 8; bx <= (xb - 1) / 8; ++bx)
                {
                    const size_t mt = size_t(std::min(by * 8 / maskTile, tileRows - 1)) * size_t(tileCols) + size_t(bx * 8 / maskTile);
                    if (mask && !(*mask)[mt])
                        continue;
                    worked[mt] = 1;
                    uint32_t& block = blockIndex[size_t(by - blockRow0) * size_t(blockCols) + size_t(bx)];
                    if (block == UINT32_MAX)
                    {
                        block = uint32_t(blocks.size());
                        blocks.push_back(uint32_t(bx) | (uint32_t(by) << 16));
                    }
                    items.insert(items.end(), {t, uint32_t(bx), uint32_t(by), block});
                }
        }
        if (items.empty())
        {
            if (tile.onBand)
                tile.onBand(y1 - y0);
            continue;
        }
        const size_t texels = blocks.size() * 64;
        const size_t slots = texels * S;
        const size_t itemCount = items.size() / 4;
        reserveBand(slots, itemCount, blocks.size());
        std::memcpy(mItems.mapped, items.data(), items.size() * sizeof(uint32_t));
        vmaFlushAllocation(mVk.allocator, mItems.alloc, 0, VK_WHOLE_SIZE);
        std::memcpy(mBlocks.mapped, blocks.data(), blocks.size() * sizeof(uint32_t));
        vmaFlushAllocation(mVk.allocator, mBlocks.alloc, 0, VK_WHOLE_SIZE);

        GpuRasterPush rp;
        rp.low = lowInfo;
        rp.items = mItems.address;
        rp.claim = mClaim.address;
        rp.out = mSamples.address;
        rp.width = uint32_t(W);
        rp.height = uint32_t(H);
        rp.grid = grid;
        rp.y0 = uint32_t(y0);
        rp.rows = uint32_t(y1 - y0);
        rp.tileU = uint32_t(tileU);
        rp.tileV = uint32_t(tileV);
        rp.udim = uint32_t(udim);
        rp.lowIndex = uint32_t(tile.low);
        rp.explicitCage = lowIn.cagePositions.size() == lowIn.soup.positions.size() ? 1u : 0u;
        rp.frontal = tile.cage.frontal;
        rp.rear = tile.cage.rear;
        rp.offset = tile.cage.offset;
        rp.skew = tile.cage.skew;
        rp.blocks = mBlocks.address;

        std::vector<GpuStep> steps;
        steps.push_back({1, 0.0, nullptr, [&](VkCommandBuffer cmd, uint32_t, uint32_t) {
                             vkCmdFillBuffer(cmd, mClaim.buffer, 0, slots * sizeof(uint32_t), 0xffffffffu);
                             vkCmdFillBuffer(cmd, mStats.buffer, 0, 32, 0);
                         },
                         "raster"});
        steps.push_back({itemCount, double(S) * 64.0, &b.mRasterRate, [&](VkCommandBuffer cmd, uint32_t first, uint32_t n) {
                             vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.mClaim);
                             dispatchChunks(cmd, b.mLayoutRaster, rp, first, n, 1, [](GpuRasterPush& q, uint32_t f, uint32_t c) {
                                 q.first = f;
                                 q.count = f + c;
                             });
                         },
                         "raster"});
        steps.push_back({slots, 4.0, &b.mRasterRate, [&](VkCommandBuffer cmd, uint32_t first, uint32_t n) {
                             vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.mSamplesPipe);
                             dispatchChunks(cmd, b.mLayoutRaster, rp, first, n, 64, [](GpuRasterPush& q, uint32_t f, uint32_t c) {
                                 q.first = f;
                                 q.count = f + c;
                             });
                         },
                         "raster"});

        GpuBakePush pp;
        pp.samples = mSamples.address;
        pp.hits = mHits.address;
        pp.highs = b.mHighInfo.address + b.mGroupBase[g] * sizeof(GpuHighInfo);
        pp.highList = b.mIota.address;
        pp.highCount = traceable ? uint32_t(b.mGroupHighs[g]) : 0u;
        pp.flags = kFlagResident | (cullBack ? kFlagCullBack : 0u);
        pp.stats = mStats.address;
        b.useLinks(pp, g, false);
        const bool hw = b.mHardware && traceable;
        if (hw)
            pp.instanceBase = b.mGroupTlas[g].instanceBase.address;
        steps.push_back({slots, 1.0, &b.mProjectRate, [&](VkCommandBuffer cmd, uint32_t first, uint32_t n) {
                             const VkPipelineLayout layout = hw ? b.mLayoutHw : b.mLayoutSw;
                             vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hw ? b.mProjectHw : b.mProjectSw);
                             if (hw)
                                 vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &b.mGroupTlas[g].set, 0, nullptr);
                             dispatchChunks(cmd, layout, pp, first, n, 64, [](GpuBakePush& q, uint32_t f, uint32_t c) {
                                 q.first = f;
                                 q.count = c;
                             });
                         },
                         "trace"});

        GpuImagePush ap;
        ap.samples = mSamples.address;
        ap.out = mOutBuf.address;
        ap.valid = mValid.address;
        ap.low = lowInfo;
        ap.width = uint32_t(W);
        ap.height = uint32_t(H);
        ap.grid = grid;
        ap.y0 = uint32_t(y0);
        ap.rows = uint32_t(y1 - y0);
        ap.blocks = mBlocks.address;
        std::vector<GpuBakePush> kernelPush(jobs.size());
        std::vector<GpuImagePush> accPush(jobs.size(), ap);
        for (size_t j = 0; j < jobs.size(); ++j)
        {
            const Kernel& k = *jobs[j].kernel;
            GpuBakePush& kp = kernelPush[j];
            kp = k.push;
            kp.samples = mSamples.address;
            kp.hits = mHits.address;
            kp.outFloat = mOutBuf.address;
            kp.valid = mValid.address;
            accPush[j].targets = jobs[j].targets;
            steps.push_back({slots, k.cost, k.rate, [&, j](VkCommandBuffer cmd, uint32_t first, uint32_t n) {
                                 const Kernel& kk = *jobs[j].kernel;
                                 vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kk.pipe);
                                 if (kk.set)
                                     vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kk.layout, 0, 1, &kk.set, 0, nullptr);
                                 dispatchChunks(cmd, kk.layout, kernelPush[j], first, n, 64, [](GpuBakePush& q, uint32_t f, uint32_t c) {
                                     q.first = f;
                                     q.count = c;
                                 });
                             },
                             jobs[j].label});
            steps.push_back({texels, double(S), &b.mImageRate, [&, j](VkCommandBuffer cmd, uint32_t first, uint32_t n) {
                                 vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.mImagePipes.at("accumulateCS"));
                                 dispatchChunks(cmd, b.mLayoutImage, accPush[j], first, n, 64, [](GpuImagePush& q, uint32_t f, uint32_t c) {
                                     q.first = f;
                                     q.total = f + c;
                                 });
                             },
                             "accumulate"});
        }
        steps.push_back({1, 0.0, nullptr, [&](VkCommandBuffer cmd, uint32_t, uint32_t) { copy(cmd, mStats, mStatsRead, 32); }});
        b.runSteps(steps, tile.cancelled);
        vmaInvalidateAllocation(mVk.allocator, mStatsRead.alloc, 0, VK_WHOLE_SIZE);
        const uint32_t* counts = static_cast<const uint32_t*>(mStatsRead.mapped);
        stats.samples += counts[0];
        stats.hits += counts[1];
        stats.backfaces += counts[2];
        stats.reaimed += counts[3];
        stats.probed += counts[4];
        if (tile.onBand)
            tile.onBand(y1 - y0);
    }
    stats.tiles = uint64_t(std::ranges::count(worked, uint8_t(1)));
    return stats;
}

bool GpuResident::finish(bake::TextureBuffer& tex, int padding) { return finishImpl(tex, padding, nullptr); }

void GpuResident::finishInto(bake::TextureBuffer& tex, int padding, const GpuImageRef& image)
{
    if (!image || image->width() != tex.width || image->height() != tex.height)
        throw std::invalid_argument("GPU bake: the image does not match the texture");
    if (finishOf(tex.def->id) == Finish::Cpu)
        throw std::runtime_error("GPU bake: " + tex.def->id + " finalizes on the CPU and cannot finish into an image");
    finishImpl(tex, padding, &image);
}

bool GpuResident::finishImpl(bake::TextureBuffer& tex, int padding, const GpuImageRef* into)
{
    LILA_ZONE();

    GpuBackend& b = mB;
    DevTex& d = texture(tex);
    const size_t n = tex.texelCount();
    const bake::MapDef& def = *tex.def;
    const bake::MapSettings& ms = tex.settings;
    const Finish kind = finishOf(def.id);
    if (into && n > mWorkCap)
    {
        for (GpuBuffer* buf : {&mWorkValue, &mWorkCount, &mPack})
            destroy(mVk, *buf);
        mWorkValue = makeBuffer(mVk, n * sizeof(glm::vec4), 0, Host::None);
        mWorkCount = makeBuffer(mVk, n * sizeof(uint32_t), 0, Host::None);
        mPack = makeBuffer(mVk, n * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Host::None);
        mWorkCap = n;
    }
    if (n > mTexelCap)
    {
        for (GpuBuffer* buf : {&mSeeds[0], &mSeeds[1], &mScratch, &mReadback})
            destroy(mVk, *buf);
        mSeeds[0] = makeBuffer(mVk, n * 4, 0, Host::None);
        mSeeds[1] = makeBuffer(mVk, n * 4, 0, Host::None);
        mScratch = makeBuffer(mVk, n * 4, 0, Host::None);
        mReadback = makeBuffer(mVk, n * (sizeof(glm::vec4) + sizeof(uint32_t)), 0, Host::Read);
        mTexelCap = n;
    }
    if (!mReduce.buffer)
    {
        mReduce = makeBuffer(mVk, 256 * 4, 0, Host::None);
        mReduceRead = makeBuffer(mVk, 256 * 4, 0, Host::Read);
    }

    GpuImagePush base;
    // Into an image, the passes work on a copy so the accumulators stay for the next incremental update.
    base.value = into ? mWorkValue.address : d.value.address;
    base.weight = d.weight.address;
    base.count = into ? mWorkCount.address : d.count.address;
    base.exact = d.exact.address;
    base.votes = d.votes.address;
    base.scratch = mScratch.address;
    base.reduce = mReduce.address;
    base.width = uint32_t(tex.width);
    base.height = uint32_t(tex.height);
    base.grid = uint32_t(std::max(1, int(std::sqrt(double(std::max(1, tex.samplesPerTexel))) + 1e-6)));
    base.background = def.background;
    base.mode = def.resolve == bake::Resolve::Exact ? 1u : def.resolve == bake::Resolve::Coverage ? 2u : 0u;

    std::vector<GpuStep> steps;
    // A per-texel pass over the whole texture.
    auto pass = [&](const char* entry, GpuImagePush push) {
        const std::string e = entry;
        const char* label = e == "resolveCS" ? "resolve" : e.starts_with("jfa") || e == "dilateApplyCS" ? "dilate" : "finalize";
        steps.push_back({n, 1.0, &b.mImageRate, [&b, entry, push](VkCommandBuffer cmd, uint32_t first, uint32_t count) {
                             vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.mImagePipes.at(entry));
                             dispatchChunks(cmd, b.mLayoutImage, push, first, count, 64, [](GpuImagePush& q, uint32_t f, uint32_t c) {
                                 q.first = f;
                                 q.total = f + c;
                             });
                         },
                         label});
    };
    auto run = [&] {
        b.runSteps(steps);
        steps.clear();
    };
    auto clearReduce = [&] { steps.push_back({1, 0.0, nullptr, [this](VkCommandBuffer cmd, uint32_t, uint32_t) { vkCmdFillBuffer(cmd, mReduce.buffer, 0, VK_WHOLE_SIZE, 0); }}); };
    auto readReduce = [&] {
        steps.push_back({1, 0.0, nullptr, [this](VkCommandBuffer cmd, uint32_t, uint32_t) { copy(cmd, mReduce, mReduceRead, 256 * 4); }});
        run();
        vmaInvalidateAllocation(mVk.allocator, mReduceRead.alloc, 0, VK_WHOLE_SIZE);
        return static_cast<const uint32_t*>(mReduceRead.mapped);
    };
    // The largest |r| (mode 0) or r (mode 1) over covered texels; 0 when none.
    auto largest = [&](uint32_t mode) {
        clearReduce();
        GpuImagePush q = base;
        q.mode = mode;
        pass("maxCS", q);
        return std::bit_cast<float>(readReduce()[0]);
    };

    if (into)
        steps.push_back({1, 0.0, nullptr, [&](VkCommandBuffer cmd, uint32_t, uint32_t) {
                             copy(cmd, d.value, mWorkValue, n * sizeof(glm::vec4));
                             copy(cmd, d.count, mWorkCount, n * sizeof(uint32_t));
                         },
                         "resolve"});
    pass("resolveCS", base);
    bool deviceFinalized = kind != Finish::Cpu;
    switch (kind)
    {
    case Finish::None:
    case Finish::Cpu: break;
    case Finish::Renormalize: pass("renormalizeCS", base); break;
    case Finish::Height:
    {
        double range = option(ms.options, "range", 0.0);
        const bool automatic = !(range > 0.0);
        if (automatic)
        {
            range = largest(0);
            if (range <= 0.0)
                range = 1.0;
        }
        GpuImagePush q = base;
        q.params.x = float(range);
        pass("heightCS", q);
        tex.meta["heightRange"] = range;
        tex.meta["heightMin"] = -range;
        tex.meta["heightMax"] = range;
        tex.meta["heightAuto"] = automatic;
        tex.meta["decode"] = "height = (value - 0.5) * 2 * heightRange";
        break;
    }
    case Finish::Thickness:
    {
        double range = option(ms.options, "distance", 0.0);
        const bool automatic = !(range > 0.0);
        if (automatic)
        {
            range = largest(1);
            if (!(range > 0.0))
                range = 1.0;
        }
        const bool invert = option(ms.options, "invert", false);
        GpuImagePush q = base;
        q.params.x = float(range);
        q.mode = invert ? 1u : 0u;
        pass("thicknessApplyCS", q);
        tex.meta["thicknessRange"] = range;
        tex.meta["thicknessAuto"] = automatic;
        tex.meta["decode"] = invert ? "thickness = (1 - value) * thicknessRange (cm)" : "thickness = value * thicknessRange (cm)";
        break;
    }
    case Finish::Curvature:
    {
        const bool mesh = curvatureFromMesh(ms);
        if (mesh)
            pass("copyRedCS", base);
        else
        {
            GpuImagePush q = base;
            q.mode = uint32_t(std::max(1, option(ms.options, "radius", 1)));
            pass("slopeCurvatureCS", q);
        }
        double range = option(ms.options, "range", 0.0);
        const bool automatic = !(range > 0.0);
        if (automatic)
        {
            // percentileAbs(0.99) by radix select over the float bits of |h|, a byte per pass from the top.
            uint32_t prefix = 0;
            size_t k = 0, covered = 0;
            for (int p = 0; p < 4; ++p)
            {
                clearReduce();
                GpuImagePush q = base;
                q.shift = uint32_t(24 - 8 * p);
                q.prefix = prefix;
                steps.push_back({n, 1.0, &b.mImageRate, [&b, q](VkCommandBuffer cmd, uint32_t first, uint32_t count) {
                                     vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.mImagePipes.at("histogramCS"));
                                     dispatchChunks(cmd, b.mLayoutImage, q, first, count, 256 * 16, [](GpuImagePush& r, uint32_t f, uint32_t c) {
                                         r.first = f;
                                         r.total = f + c;
                                     });
                                 },
                                 "finalize"});
                const uint32_t* hist = readReduce();
                if (p == 0)
                {
                    for (int i = 0; i < 256; ++i)
                        covered += hist[i];
                    if (!covered)
                        break;
                    k = std::min(covered - 1, size_t(0.99f * float(covered - 1) + 0.5f));
                }
                size_t below = 0;
                int bin = 255;
                for (int i = 0; i < 256; ++i)
                {
                    if (k < below + hist[i])
                    {
                        bin = i;
                        break;
                    }
                    below += hist[i];
                }
                k -= below;
                prefix = (prefix << 8) | uint32_t(bin);
            }
            range = covered ? double(std::bit_cast<float>(prefix)) : 0.0;
            if (!(range > 0.0))
                range = 1.0;
        }
        const std::string id = def.id;
        GpuImagePush q = base;
        q.mode = id == "convexity" ? 1u : id == "cavity" ? 2u : id == "edgeWear" ? 3u : 0u;
        q.params = glm::vec4(float(range), option(ms.options, "threshold", 0.1f), option(ms.options, "softness", 0.4f),
                             std::clamp(option(ms.options, "noise", 0.0f), 0.0f, 1.0f));
        q.params2.x = std::max(1.0f, option(ms.options, "noiseScale", 8.0f));
        pass("curvatureCS", q);
        tex.meta["curvatureRange"] = range;
        tex.meta["curvatureAuto"] = automatic;
        tex.meta["source"] = mesh ? "mesh" : "normals";
        if (q.mode == 0)
            tex.meta["decode"] = "curvature = (value - 0.5) * 2 * curvatureRange (1/cm mean curvature, convex positive)";
        break;
    }
    }

    if (deviceFinalized && def.dilate && padding != 0)
    {
        // Steps k, k/2 .. 1 reach 2k - 1 texels; two more at 2 and 1 mend jump flood's rare misses.
        const int extent = std::max(tex.width, tex.height);
        const int reach = padding < 0 ? extent : std::min(padding, extent);
        std::vector<int> jumps;
        for (int k = int(std::bit_ceil(unsigned((reach + 1) / 2 + 1))); k >= 1; k /= 2)
            jumps.push_back(k);
        jumps.push_back(2);
        jumps.push_back(1);
        GpuImagePush q = base;
        q.seedsOut = mSeeds[0].address;
        pass("jfaInitCS", q);
        int cur = 0;
        for (int step : jumps)
        {
            q.seedsIn = mSeeds[cur].address;
            q.seedsOut = mSeeds[1 - cur].address;
            q.step = step;
            pass("jfaStepCS", q);
            cur = 1 - cur;
        }
        q.seedsIn = mSeeds[cur].address;
        q.padding = padding;
        pass("dilateApplyCS", q);
    }
    if (into)
    {
        writeImage(steps, base, **into);
        run();
        for (GpuBuffer& buf : mStaging)
            destroy(mVk, buf);
        mStaging.clear();
        return true;
    }
    steps.push_back({1, 0.0, nullptr, [&](VkCommandBuffer cmd, uint32_t, uint32_t) {
                         VkBufferCopy regions[2] = {{0, 0, n * sizeof(glm::vec4)}, {0, n * sizeof(glm::vec4), n * sizeof(uint32_t)}};
                         vkCmdCopyBuffer(cmd, d.value.buffer, mReadback.buffer, 1, &regions[0]);
                         vkCmdCopyBuffer(cmd, d.count.buffer, mReadback.buffer, 1, &regions[1]);
                     },
                     "readback"});
    run();
    vmaInvalidateAllocation(mVk.allocator, mReadback.alloc, 0, VK_WHOLE_SIZE);
    tex.value.resize(n);
    tex.covered.resize(n);
    const auto* values = static_cast<const glm::vec4*>(mReadback.mapped);
    const auto* covered = reinterpret_cast<const uint32_t*>(static_cast<const char*>(mReadback.mapped) + n * sizeof(glm::vec4));
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n, 1 << 16), [&](const tbb::blocked_range<size_t>& r) {
        std::memcpy(tex.value.data() + r.begin(), values + r.begin(), (r.end() - r.begin()) * sizeof(glm::vec4));
        for (size_t i = r.begin(); i < r.end(); ++i)
            tex.covered[i] = covered[i] ? 1 : 0;
    });
    mSpareTex.push_back(d);
    mTex.erase(&tex);
    if (kind == Finish::None && def.finalize)
        def.finalize(ms, *mIn, tex, tex.meta);
    return deviceFinalized;
}
} // namespace lila::render::detail
