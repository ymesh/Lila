#include "render/GpuImage.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "render/Renderer.h"
#include "render/SharedMemory.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <stdexcept>

namespace lila::render
{
namespace
{
constexpr VkPipelineStageFlags2 kReadStages = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkAccessFlags2 kReadAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

struct Dead
{
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView storageView = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE; // a shared image's, instead of alloc
    void* handle = nullptr;
};

struct Registry
{
    std::mutex mutex;
    std::set<GpuImage*> live;
    std::vector<Dead> dead;
};

// Leaked on purpose: images may be dropped during static destruction.
Registry& registry()
{
    static Registry* r = new Registry;
    return *r;
}

void destroyDead(VulkanContext& vk, const Dead& d)
{
    if (d.storageView)
        vkDestroyImageView(vk.device, d.storageView, nullptr);
    if (d.view)
        vkDestroyImageView(vk.device, d.view, nullptr);
    if (d.memory)
    {
        vkDestroyImage(vk.device, d.image, nullptr);
        sharedMemory::release(vk, d.memory, d.handle);
    }
    else if (d.image)
        vmaDestroyImage(vk.allocator, d.image, d.alloc);
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, uint32_t baseMip, uint32_t mips, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mips, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

VkFormatFeatureFlags formatFeatures(VulkanContext& vk, VkFormat f)
{
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(vk.physical, f, &fp);
    return fp.optimalTilingFeatures;
}
} // namespace

VkFormat vkFormat(TextureFormat f, bool srgb)
{
    switch (f)
    {
    case TextureFormat::R8: return VK_FORMAT_R8_UNORM;
    case TextureFormat::RG8: return VK_FORMAT_R8G8_UNORM;
    case TextureFormat::RGBA8: return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::R16F: return VK_FORMAT_R16_SFLOAT;
    case TextureFormat::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case TextureFormat::R32F: return VK_FORMAT_R32_SFLOAT;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

VkComponentMapping sampleSwizzle(TextureFormat f)
{
    constexpr auto R = VK_COMPONENT_SWIZZLE_R, G = VK_COMPONENT_SWIZZLE_G, one = VK_COMPONENT_SWIZZLE_ONE, id = VK_COMPONENT_SWIZZLE_IDENTITY;
    switch (channelCount(f))
    {
    case 1: return {R, R, R, one};
    case 2: return {R, R, R, G};
    default: return {id, id, id, id};
    }
}

GpuImage::GpuImage(const GpuImageDesc& desc) : mDesc(desc), mKey(nextResourceKey()) {}

GpuImageRef GpuImage::create(const GpuImageDesc& desc)
{
    if (desc.width < 1 || desc.height < 1 || desc.width > 16384 || desc.height > 16384)
        throw std::invalid_argument("GpuImage: size out of range");
    GpuImageRef img(new GpuImage(desc));
    std::scoped_lock lock(registry().mutex);
    registry().live.insert(img.get());
    return img;
}

GpuImage::~GpuImage()
{
    Registry& r = registry();
    std::scoped_lock lock(r.mutex);
    r.live.erase(this);
    if (mImage)
        r.dead.push_back({mImage, mAlloc, mView, mStorageView, mSharedMemory, mShared ? mShared->handle : nullptr});
}

std::future<void> GpuImage::realize(std::vector<GpuImageRef> images)
{
    Renderer& r = Renderer::get();
    if (!r.running())
    {
        std::promise<void> p;
        p.set_exception(std::make_exception_ptr(std::runtime_error("GpuImage::realize: the renderer is not running")));
        return p.get_future();
    }
    return r.runTask([images = std::move(images)](VulkanContext& vk) {
        compute::Submitter sub;
        sub.init(vk);
        std::vector<compute::Buffer> staging;
        auto cleanup = [&] {
            for (compute::Buffer& b : staging)
                compute::destroy(vk, b);
            sub.destroy(vk);
        };
        try
        {
            sub.run(vk, [&](VkCommandBuffer cmd) {
                for (const GpuImageRef& img : images)
                    img->prepare(vk, cmd, staging);
            });
        }
        catch (...)
        {
            cleanup();
            throw;
        }
        cleanup();
        return true;
    });
}

std::optional<std::array<uint8_t, 16>> GpuImage::sharingDevice()
{
    Renderer& r = Renderer::get();
    const VulkanContext* vk = r.running() ? r.context() : nullptr;
    if (!vk || !vk->externalMemory)
        return std::nullopt;
    return vk->deviceUuid;
}

std::optional<SharedImageMemory> GpuImage::sharedMemory() const
{
    std::scoped_lock lock(mSharedMutex);
    return mShared;
}

void GpuImage::acquireExternal()
{
    mAcquire = true;
    changed(0, 0, mDesc.width, mDesc.height);
}

void GpuImage::update(int x, int y, int w, int h, std::span<const uint8_t> texels)
{
    update(x, y, w, h, std::vector<uint8_t>(texels.begin(), texels.end()));
}

void GpuImage::update(int x, int y, int w, int h, std::vector<uint8_t>&& texels)
{
    if (x < 0 || y < 0 || w < 1 || h < 1 || x + w > mDesc.width || y + h > mDesc.height)
        throw std::invalid_argument("GpuImage::update: region outside the image");
    if (texels.size() != size_t(w) * size_t(h) * size_t(texelBytes(mDesc.format)))
        throw std::invalid_argument("GpuImage::update: texels do not match the region");
    {
        std::scoped_lock lock(mMutex);
        // A region covering the whole image makes everything queued before it moot.
        if (x == 0 && y == 0 && w == mDesc.width && h == mDesc.height)
            mPending.clear();
        mPending.push_back({x, y, w, h, std::move(texels)});
    }
    changed(x, y, x + w, y + h);
}

void GpuImage::changed(int x0, int y0, int x1, int y1)
{
    std::scoped_lock lock(mChangeMutex);
    const uint64_t v = ++mVersion;
    mChanges.push_back({v, x0, y0, x1, y1});
    if (mChanges.size() > 256)
        mChanges.erase(mChanges.begin(), mChanges.begin() + 64);
}

void GpuImage::noteWriteRows(int first, int last) { noteWriteRect(0, first, mDesc.width, last); }

void GpuImage::noteWriteRect(int x0, int y0, int x1, int y1)
{
    if (x1 <= x0 || y1 <= y0)
        return;
    std::scoped_lock lock(mChangeMutex);
    if (mWrite[2] <= mWrite[0] || mWrite[3] <= mWrite[1])
    {
        mWrite[0] = x0;
        mWrite[1] = y0;
        mWrite[2] = x1;
        mWrite[3] = y1;
    }
    else
    {
        mWrite[0] = std::min(mWrite[0], x0);
        mWrite[1] = std::min(mWrite[1], y0);
        mWrite[2] = std::max(mWrite[2], x1);
        mWrite[3] = std::max(mWrite[3], y1);
    }
}

std::optional<std::array<int, 4>> GpuImage::rectSince(uint64_t version) const
{
    std::scoped_lock lock(mChangeMutex);
    if (version >= mVersion.load())
        return std::array<int, 4>{0, 0, 0, 0};
    std::array<int, 4> r{mDesc.width, mDesc.height, 0, 0};
    uint64_t expect = version + 1;
    for (const Change& c : mChanges)
    {
        if (c.version <= version)
            continue;
        if (c.version != expect || c.x1 <= c.x0 || c.y1 <= c.y0)
            return std::nullopt;
        ++expect;
        r = {std::min(r[0], c.x0), std::min(r[1], c.y0), std::max(r[2], c.x1), std::max(r[3], c.y1)};
    }
    if (expect != mVersion.load() + 1)
        return std::nullopt;
    r[0] = std::clamp(r[0], 0, mDesc.width);
    r[2] = std::clamp(r[2], r[0], mDesc.width);
    r[1] = std::clamp(r[1], 0, mDesc.height);
    r[3] = std::clamp(r[3], r[1], mDesc.height);
    return r;
}

std::optional<std::vector<std::array<int, 4>>> GpuImage::rectsSince(uint64_t version) const
{
    std::scoped_lock lock(mChangeMutex);
    std::vector<std::array<int, 4>> out;
    if (version >= mVersion.load())
        return out;
    uint64_t expect = version + 1;
    for (const Change& c : mChanges)
    {
        if (c.version <= version)
            continue;
        if (c.version != expect || c.x1 <= c.x0 || c.y1 <= c.y0)
            return std::nullopt;
        ++expect;
        const int x0 = std::clamp(c.x0, 0, mDesc.width), y0 = std::clamp(c.y0, 0, mDesc.height);
        out.push_back({x0, y0, std::clamp(c.x1, x0, mDesc.width), std::clamp(c.y1, y0, mDesc.height)});
    }
    if (expect != mVersion.load() + 1)
        return std::nullopt;
    return out;
}

void GpuImage::noteWriteRegions(std::vector<std::array<int, 4>> regions)
{
    std::scoped_lock lock(mChangeMutex);
    mWriteRegions.insert(mWriteRegions.end(), regions.begin(), regions.end());
}

std::optional<std::pair<int, int>> GpuImage::rowsSince(uint64_t version) const
{
    std::optional<std::array<int, 4>> r = rectSince(version);
    if (!r)
        return std::nullopt;
    if ((*r)[2] <= (*r)[0] || (*r)[3] <= (*r)[1])
        return std::pair<int, int>{0, 0};
    return std::pair<int, int>{(*r)[1], (*r)[3]};
}

void GpuImage::updateRgba(int x, int y, int w, int h, std::span<const float> rgba)
{
    std::vector<uint8_t> texels;
    encodeTexels(mDesc.format, mDesc.srgb, rgba, texels);
    update(x, y, w, h, std::move(texels));
}

TexturePtr GpuImage::asTexture()
{
    std::scoped_lock lock(mMutex);
    if (TexturePtr t = mTexture.lock())
        return t;
    auto t = std::make_shared<TextureImage>();
    t->width = mDesc.width;
    t->height = mDesc.height;
    t->format = mDesc.format;
    t->srgb = mDesc.srgb;
    t->gpu = shared_from_this();
    mTexture = t;
    return t;
}

uint32_t GpuImage::mipCount() const
{
    uint32_t mips = 1;
    while ((uint32_t(std::max(mDesc.width, mDesc.height)) >> mips) > 0)
        ++mips;
    return mips;
}

bool GpuImage::prepare(VulkanContext& vk, VkCommandBuffer cmd, std::vector<compute::Buffer>& staging)
{
    if (mFailed)
        return false;
    std::vector<Pending> pending;
    {
        std::scoped_lock lock(mMutex);
        pending.swap(mPending);
    }
    const bool fresh = !mImage;
    if (fresh)
    {
        LILA_ZONE();

        mFormat = render::vkFormat(mDesc.format, mDesc.srgb);
        const VkFormat storageFormat = render::vkFormat(mDesc.format, false);
        const VkFormatFeatureFlags features = formatFeatures(vk, mFormat);
        const bool storage = (formatFeatures(vk, storageFormat) & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
        constexpr VkFormatFeatureFlags blit =
            VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        mMips = mDesc.mips && !mDesc.shared && (features & blit) == blit ? mipCount() : 1;

        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        const bool split = storage && storageFormat != mFormat;
        // The sRGB format cannot be a storage image: compute writes through a unorm view of the same bytes.
        ici.flags = split ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT : 0;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = mFormat;
        ici.extent = {uint32_t(mDesc.width), uint32_t(mDesc.height), 1};
        ici.mipLevels = mMips;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
        if (mDesc.shared)
        {
            if (!createShared(vk, ici))
            {
                log::warn("render: cannot create the {}x{} shared GPU image {}", mDesc.width, mDesc.height, mDesc.name);
                mFailed = true;
                return false;
            }
        }
        else
        {
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            if (VkResult r = vmaCreateImage(vk.allocator, &ici, &aci, &mImage, &mAlloc, nullptr); r != VK_SUCCESS)
            {
                log::warn("render: cannot create the {}x{} GPU image {} ({})", mDesc.width, mDesc.height, mDesc.name, vkResultName(r));
                mImage = VK_NULL_HANDLE;
                mFailed = true;
                return false;
            }
        }
        VkImageViewUsageCreateInfo sampledUsage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
        sampledUsage.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.pNext = split ? &sampledUsage : nullptr;
        vci.image = mImage;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = mFormat;
        vci.components = sampleSwizzle(mDesc.format);
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mMips, 0, 1};
        vkCreateImageView(vk.device, &vci, nullptr, &mView);
        if (storage)
        {
            VkImageViewCreateInfo sci = vci;
            sci.pNext = nullptr;
            sci.format = storageFormat;
            sci.components = {};
            sci.subresourceRange.levelCount = 1;
            vkCreateImageView(vk.device, &sci, nullptr, &mStorageView);
        }

        imageBarrier(cmd, mImage, 0, mMips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkClearColorValue zero{};
        VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, mMips, 0, 1};
        vkCmdClearColorImage(cmd, mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &all);
        imageBarrier(cmd, mImage, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    if (!fresh && mAcquire.exchange(false))
    {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.dstStageMask = kReadStages;
        b.dstAccessMask = kReadAccess;
        b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        b.dstQueueFamilyIndex = vk.queueFamily;
        b.image = mImage;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
    }
    if (pending.empty())
    {
        if (fresh)
            recordMips(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        return true;
    }

    LILA_ZONE();
    // Buffer offsets of copies must be a multiple of the texel size and of 4.
    auto aligned = [](size_t n) { return (n + 15) & ~size_t(15); };
    size_t total = 0;
    for (const Pending& p : pending)
        total += aligned(p.texels.size());
    compute::Buffer buf = compute::makeBuffer(vk, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, compute::Host::Write);
    staging.push_back(buf);
    std::vector<VkBufferImageCopy> regions;
    size_t offset = 0;
    for (const Pending& p : pending)
    {
        std::memcpy(static_cast<uint8_t*>(buf.mapped) + offset, p.texels.data(), p.texels.size());
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {p.x, p.y, 0};
        region.imageExtent = {uint32_t(p.w), uint32_t(p.h), 1};
        regions.push_back(region);
        offset += aligned(p.texels.size());
    }
    vmaFlushAllocation(vk.allocator, buf.alloc, 0, VK_WHOLE_SIZE);
    if (!fresh)
        imageBarrier(cmd, mImage, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kReadStages, 0,
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(cmd, buf.buffer, mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()), regions.data());
    recordMips(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    return true;
}

void GpuImage::recordMips(VkCommandBuffer cmd, VkImageLayout from, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, const int* rect)
{
    constexpr auto transfer = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    if (mMips == 1)
    {
        imageBarrier(cmd, mImage, 0, 1, from, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, srcStage, srcAccess, kReadStages, kReadAccess);
        return;
    }
    // A region only once the mips hold the rest; a stroke's store then costs its own size, not the image's.
    const bool region = rect && mMipsBuilt && rect[2] > rect[0] && rect[3] > rect[1];
    imageBarrier(cmd, mImage, 0, 1, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcStage, srcAccess, transfer, VK_ACCESS_2_TRANSFER_READ_BIT);
    imageBarrier(cmd, mImage, 1, mMips - 1, region ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kReadStages, 0, transfer, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    int x0 = region ? rect[0] : 0, y0 = region ? rect[1] : 0, x1 = region ? rect[2] : mDesc.width, y1 = region ? rect[3] : mDesc.height;
    for (uint32_t mip = 1; mip < mMips; ++mip)
    {
        const int32_t sw = std::max(mDesc.width >> (mip - 1), 1), sh = std::max(mDesc.height >> (mip - 1), 1);
        const int32_t dw = std::max(mDesc.width >> mip, 1), dh = std::max(mDesc.height >> mip, 1);
        // The level's texels over the region, rounded out, and the texels of the level above that make them.
        x0 = std::clamp(x0 / 2, 0, dw - 1);
        y0 = std::clamp(y0 / 2, 0, dh - 1);
        x1 = std::clamp((x1 + 1) / 2, x0 + 1, dw);
        y1 = std::clamp((y1 + 1) / 2, y0 + 1, dh);
        VkImageBlit b{};
        b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 1};
        b.srcOffsets[0] = {std::min(x0 * 2, sw - 1), std::min(y0 * 2, sh - 1), 0};
        b.srcOffsets[1] = {std::min(x1 * 2, sw), std::min(y1 * 2, sh), 1};
        b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
        b.dstOffsets[0] = {x0, y0, 0};
        b.dstOffsets[1] = {x1, y1, 1};
        vkCmdBlitImage(cmd, mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
        imageBarrier(cmd, mImage, mip, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, transfer,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, transfer, VK_ACCESS_2_TRANSFER_READ_BIT);
    }
    imageBarrier(cmd, mImage, 0, mMips, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, transfer,
                 VK_ACCESS_2_TRANSFER_READ_BIT, kReadStages, kReadAccess);
    mMipsBuilt = true;
}

void GpuImage::beginWrite(VkCommandBuffer cmd)
{
    imageBarrier(cmd, mImage, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, kReadStages, 0,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

void GpuImage::recordClear(VkCommandBuffer cmd, const float (&rgba)[4])
{
    compute::memoryBarrier(cmd);
    VkClearColorValue c{};
    std::memcpy(c.float32, rgba, sizeof(c.float32));
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, mImage, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &range);
    compute::memoryBarrier(cmd);
}

void GpuImage::endWrite(VkCommandBuffer cmd)
{
    int r[4] = {0, 0, 0, 0};
    std::vector<std::array<int, 4>> regions;
    {
        std::scoped_lock lock(mChangeMutex);
        std::swap(r, mWrite);
        std::swap(regions, mWriteRegions);
    }
    recordMips(cmd, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, r);
    if (regions.empty() || r[2] <= r[0] || r[3] <= r[1])
        changed(r[0], r[1], r[2], r[3]);
    else
        for (const std::array<int, 4>& g : regions)
            changed(g[0], g[1], g[2], g[3]);
}

void GpuImage::recordRead(VkCommandBuffer cmd, const compute::Buffer& into)
{
    imageBarrier(cmd, mImage, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kReadStages, 0,
                 VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {uint32_t(mDesc.width), uint32_t(mDesc.height), 1};
    vkCmdCopyImageToBuffer(cmd, mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, into.buffer, 1, &region);
    imageBarrier(cmd, mImage, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, 0, kReadStages, kReadAccess);
}

void GpuImage::recordRead(VkCommandBuffer cmd, const compute::Buffer& into, int first, int last)
{
    first = std::clamp(first, 0, mDesc.height);
    last = std::clamp(last, first, mDesc.height);
    if (last <= first)
        return;
    imageBarrier(cmd, mImage, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kReadStages, 0,
                 VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.bufferOffset = VkDeviceSize(first) * VkDeviceSize(mDesc.width) * VkDeviceSize(texelBytes(mDesc.format));
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, first, 0};
    region.imageExtent = {uint32_t(mDesc.width), uint32_t(last - first), 1};
    vkCmdCopyImageToBuffer(cmd, mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, into.buffer, 1, &region);
    imageBarrier(cmd, mImage, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, 0, kReadStages, kReadAccess);
}

void GpuImage::recordReadRect(VkCommandBuffer cmd, const compute::Buffer& into, VkDeviceSize offset, int x0, int y0, int w, int h)
{
    if (w < 1 || h < 1)
        return;
    imageBarrier(cmd, mImage, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kReadStages, 0,
                 VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.bufferOffset = offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {x0, y0, 0};
    region.imageExtent = {uint32_t(w), uint32_t(h), 1};
    vkCmdCopyImageToBuffer(cmd, mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, into.buffer, 1, &region);
    imageBarrier(cmd, mImage, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, 0, kReadStages, kReadAccess);
}

bool GpuImage::createShared(VulkanContext& vk, VkImageCreateInfo ici)
{
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.handleTypes = sharedMemory::kHandleType;
    ici.pNext = &external;
    if (!vk.externalMemory || vkCreateImage(vk.device, &ici, nullptr, &mImage) != VK_SUCCESS)
    {
        mImage = VK_NULL_HANDLE;
        return false;
    }
    SharedImageMemory m{.format = ici.format, .flags = ici.flags, .usage = ici.usage, .width = mDesc.width, .height = mDesc.height};
    if (!sharedMemory::allocate(vk, mImage, mSharedMemory, m.handle, m.size))
    {
        vkDestroyImage(vk.device, mImage, nullptr);
        mImage = VK_NULL_HANDLE;
        return false;
    }
    std::scoped_lock lock(mSharedMutex);
    mShared = m;
    return true;
}

void GpuImage::destroyDevice(VulkanContext& vk)
{
    destroyDead(vk, {mImage, mAlloc, mView, mStorageView, mSharedMemory, mShared ? mShared->handle : nullptr});
    {
        std::scoped_lock lock(mSharedMutex);
        mShared.reset();
    }
    mSharedMemory = VK_NULL_HANDLE;
    mImage = VK_NULL_HANDLE;
    mAlloc = VK_NULL_HANDLE;
    mView = mStorageView = VK_NULL_HANDLE;
    mFailed = false;
    mMipsBuilt = false;
}

bool GpuImage::collectGarbage(VulkanContext& vk)
{
    std::vector<Dead> dead;
    {
        std::scoped_lock lock(registry().mutex);
        dead.swap(registry().dead);
    }
    if (dead.empty())
        return false;
    vkDeviceWaitIdle(vk.device);
    for (const Dead& d : dead)
        destroyDead(vk, d);
    return true;
}

void GpuImage::releaseAll(VulkanContext& vk)
{
    Registry& r = registry();
    std::scoped_lock lock(r.mutex);
    for (const Dead& d : r.dead)
        destroyDead(vk, d);
    r.dead.clear();
    // A live image comes back as zero the next time it is prepared, unless it is updated again.
    for (GpuImage* img : r.live)
        img->destroyDevice(vk);
}

std::vector<uint8_t> readGpuImage(const GpuImageRef& image)
{
    Renderer& r = Renderer::get();
    if (!r.running())
        throw std::runtime_error("readGpuImage: the renderer is not running");
    std::vector<uint8_t> out;
    r.runTask([&](VulkanContext& vk) {
         compute::Submitter sub;
         sub.init(vk);
         std::vector<compute::Buffer> staging;
         compute::Buffer read;
         try
         {
             const size_t bytes = size_t(image->width()) * size_t(image->height()) * size_t(texelBytes(image->format()));
             read = compute::makeBuffer(vk, bytes, 0, compute::Host::Read);
             sub.run(vk, [&](VkCommandBuffer cmd) {
                 if (image->prepare(vk, cmd, staging))
                     image->recordRead(cmd, read);
             });
             if (image->realized())
             {
                 vmaInvalidateAllocation(vk.allocator, read.alloc, 0, VK_WHOLE_SIZE);
                 const auto* p = static_cast<const uint8_t*>(read.mapped);
                 out.assign(p, p + bytes);
             }
         }
         catch (...)
         {
             for (compute::Buffer& b : staging)
                 compute::destroy(vk, b);
             compute::destroy(vk, read);
             sub.destroy(vk);
             throw;
         }
         for (compute::Buffer& b : staging)
             compute::destroy(vk, b);
         compute::destroy(vk, read);
         sub.destroy(vk);
         return true;
     }).get();
    return out;
}
} // namespace lila::render
