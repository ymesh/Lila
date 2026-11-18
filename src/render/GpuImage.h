#pragma once

#include "core/Export.h"
#include "render/Compute.h"
#include "render/RenderMesh.h"
#include "render/TextureFormat.h"

#include <array>
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lila::render
{
/** The Vulkan format a texture format uploads as: sRGB only for RGBA8 colour. */
LILA_UI_API VkFormat vkFormat(TextureFormat f, bool srgb);
/** How materials sample a format: one channel as gray (alpha 1), two as gray and alpha, RGBA as it is. */
LILA_UI_API VkComponentMapping sampleSwizzle(TextureFormat f);

struct GpuImageDesc
{
    int width = 1;
    int height = 1;
    TextureFormat format = TextureFormat::RGBA8;
    /** RGBA8 colour data, sampled through an sRGB view (compute still writes the raw unorm bytes). */
    bool srgb = false;
    /** A full mip chain, rebuilt after every update and write. */
    bool mips = false;
    /** Where it shows in logs and captures. */
    std::string name;
    /** In memory of its own that another Vulkan device in the process can import (sharedMemory); no mips. */
    bool shared = false;
};

/** A shared GpuImage's memory and the parameters an importing device creates its alias of the image with. */
struct SharedImageMemory
{
    /** An opaque Win32 handle the image owns and closes: import it while the image lives. */
    void* handle = nullptr;
    uint64_t size = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageCreateFlags flags = 0;
    VkImageUsageFlags usage = 0;
    int width = 0;
    int height = 0;
};

class GpuImage;
using GpuImageRef = std::shared_ptr<GpuImage>;

/**
 * A mutable image the renderer owns on the GPU: the CPU updates regions of it, GPU producers (the baker, texture graphs,
 * painting) write it from compute, and the viewport samples it as a material texture (wrap it with asTexture), all with
 * no CPU readback. It starts out zero.
 *
 * Threads: create, update and asTexture from any thread; the device image is made lazily on the render thread the first
 * time a frame or task prepares it. Updates queue under a mutex and land, in order, before the next frame that draws
 * the image or the next prepare in a task, whichever comes first. The render-thread calls (prepare, beginWrite,
 * endWrite, the handles) belong in Renderer::runTask tasks or the renderer's own frame. The device memory is freed on the
 * render thread once the last reference is gone and no frame can still read it.
 */
class LILA_UI_API GpuImage : public std::enable_shared_from_this<GpuImage>
{
public:
    /** Throws std::invalid_argument for a size below 1 or above 16384. */
    static GpuImageRef create(const GpuImageDesc& desc);
    /** Makes the images' device images now, on the render thread; the future throws when the renderer isn't running. Any thread. */
    static std::future<void> realize(std::vector<GpuImageRef> images);
    /** The deviceUUID of the GPU shared images live on; nullopt when the renderer isn't running or can't share memory. */
    static std::optional<std::array<uint8_t, 16>> sharingDevice();
    ~GpuImage();
    GpuImage(const GpuImage&) = delete;
    GpuImage& operator=(const GpuImage&) = delete;

    const GpuImageDesc& desc() const { return mDesc; }
    int width() const { return mDesc.width; }
    int height() const { return mDesc.height; }
    TextureFormat format() const { return mDesc.format; }
    /** Unique across the process, like TextureImage keys. */
    uint64_t key() const { return mKey; }
    /** Bumped by every update and endWrite: a redraw shows something new. */
    uint64_t version() const { return mVersion.load(); }
    /**
     * The rows [first, last) every change after `version` touched, so a reader can copy just those; {0, 0} when nothing
     * changed, nullopt when that isn't known (a change of unknown extent, or too long ago). Any thread.
     */
    std::optional<std::pair<int, int>> rowsSince(uint64_t version) const;
    /** The same as a rectangle {x0, y0, x1, y1} of texels, empty (x1 <= x0) when nothing changed. Any thread. */
    std::optional<std::array<int, 4>> rectSince(uint64_t version) const;
    /** Each change after `version` on its own, so a reader can redo separate regions without their bounding box. Any thread. */
    std::optional<std::vector<std::array<int, 4>>> rectsSince(uint64_t version) const;
    /**
     * Before endWrite: the write's separate regions, each counted as its own change (a stroke over distant uv islands).
     * They should lie inside what noteWriteRect covers.
     */
    void noteWriteRegions(std::vector<std::array<int, 4>> regions);
    /** Before endWrite: the write touches only rows [first, last) (unioned over calls). Without it a write counts as whole. */
    void noteWriteRows(int first, int last);
    /** Before endWrite: the write touches only texels [x0, x1) x [y0, y1) (unioned over calls). */
    void noteWriteRect(int x0, int y0, int x1, int y1);

    /**
     * Queues texels for the region (x, y, w, h), row 0 at the top, tightly packed in the image's format. Any thread.
     * Throws std::invalid_argument when the region is outside the image or texels is the wrong size.
     */
    void update(int x, int y, int w, int h, std::span<const uint8_t> texels);
    /** The same, taking the texels without a copy. */
    void update(int x, int y, int w, int h, std::vector<uint8_t>&& texels);
    /** update from RGBA floats (4 per texel), encoded with encodeTexels. */
    void updateRgba(int x, int y, int w, int h, std::span<const float> rgba);

    /** A material texture that samples this image (a TextureImage whose gpu is this). Any thread. */
    TexturePtr asTexture();

    /** A shared image's memory once a task or frame has prepared it; nullopt before, or for an image that isn't shared. Any thread. */
    std::optional<SharedImageMemory> sharedMemory() const;
    /**
     * Any thread: another device wrote this shared image, leaving it SHADER_READ_ONLY_OPTIMAL and released to
     * VK_QUEUE_FAMILY_EXTERNAL, and that work has finished. The next prepare acquires it, and the redraw shows it.
     */
    void acquireExternal();

    // ---------------------------------------------------------------- render thread

    /**
     * Makes the device image on first use and records the queued updates into cmd (and the mip chain after them),
     * leaving the image shader-readable. Staging buffers go to staging; free them once cmd has finished. False when the
     * image cannot be made (no device memory), which is logged once.
     */
    bool prepare(VulkanContext& vk, VkCommandBuffer cmd, std::vector<compute::Buffer>& staging);
    /** Whether prepare has made the device image. */
    bool realized() const { return mImage != VK_NULL_HANDLE; }
    /** The sampled view (sRGB for an sRGB image). Null before prepare. */
    VkImageView view() const { return mView; }
    VkImage image() const { return mImage; }
    VkFormat vkFormat() const { return mFormat; }
    /** The view compute writes through (unorm for an sRGB image); null when the format has no storage support. */
    VkImageView storageView() const { return mStorageView; }

    /**
     * After prepare: moves the image to GENERAL for compute writes through storageView (and reads of what they wrote).
     * Pair with endWrite, which makes the writes visible to sampling and rebuilds the mips.
     */
    void beginWrite(VkCommandBuffer cmd);
    void endWrite(VkCommandBuffer cmd);
    /** Between beginWrite and endWrite: fills mip 0 with one value (raw channel values, before any sRGB encoding). */
    void recordClear(VkCommandBuffer cmd, const float (&rgba)[4]);
    /** After prepare: copies mip 0 into a buffer for the host to read, leaving the image shader-readable. */
    void recordRead(VkCommandBuffer cmd, const compute::Buffer& into);
    /** The same for rows [first, last) only, at their place in the buffer (as a whole read lays them out). */
    void recordRead(VkCommandBuffer cmd, const compute::Buffer& into, int first, int last);
    /** Texels [x0, x0 + w) x [y0, y0 + h) of mip 0, tightly packed at offset in the buffer. */
    void recordReadRect(VkCommandBuffer cmd, const compute::Buffer& into, VkDeviceSize offset, int x0, int y0, int w, int h);

    /** Render thread, the device idle: frees the device images of dropped GpuImages. True when there were any. */
    static bool collectGarbage(VulkanContext& vk);
    /** Render thread at shutdown, the device idle: frees every device image, live or dropped. Live images realize again later. */
    static void releaseAll(VulkanContext& vk);

private:
    explicit GpuImage(const GpuImageDesc& desc);
    void destroyDevice(VulkanContext& vk);
    bool createShared(VulkanContext& vk, VkImageCreateInfo ici);
    /**
     * Rebuilds the mips below level 0 from it, all of them or (rect, texels of level 0, once they have been built whole)
     * only over that region, keeping the rest.
     */
    void recordMips(VkCommandBuffer cmd, VkImageLayout from, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                    const int* rect = nullptr);
    uint32_t mipCount() const;

    struct Pending
    {
        int x = 0, y = 0, w = 0, h = 0;
        std::vector<uint8_t> texels;
    };

    /** Counts a change of texels [x0, x1) x [y0, y1), or of everything when the rectangle is empty. */
    void changed(int x0, int y0, int x1, int y1);

    struct Change
    {
        uint64_t version = 0;
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };

    GpuImageDesc mDesc;
    uint64_t mKey;
    std::atomic<uint64_t> mVersion{1};
    mutable std::mutex mChangeMutex;
    std::vector<Change> mChanges; // guarded, the latest few
    int mWrite[4] = {0, 0, 0, 0}; // guarded: x0, y0, x1, y1
    std::vector<std::array<int, 4>> mWriteRegions; // guarded
    std::mutex mMutex;
    std::vector<Pending> mPending; // guarded by mMutex
    std::weak_ptr<const TextureImage> mTexture; // guarded by mMutex

    // Render thread.
    VkImage mImage = VK_NULL_HANDLE;
    VmaAllocation mAlloc = VK_NULL_HANDLE;
    VkDeviceMemory mSharedMemory = VK_NULL_HANDLE; // instead of mAlloc for a shared image
    std::atomic<bool> mAcquire{false};
    mutable std::mutex mSharedMutex;
    std::optional<SharedImageMemory> mShared; // guarded by mSharedMutex
    VkImageView mView = VK_NULL_HANDLE;
    VkImageView mStorageView = VK_NULL_HANDLE;
    VkFormat mFormat = VK_FORMAT_UNDEFINED;
    uint32_t mMips = 1;
    /** The mips below level 0 hold a downsample of it: a later write can redo just its region. Render thread. */
    bool mMipsBuilt = false;
    bool mFailed = false;
};

/**
 * Blocks until the renderer has copied the image's mip 0 back: texels in its format, row 0 at the top. For tests and
 * export, never from the render thread. Throws std::runtime_error when the renderer is not running.
 */
LILA_UI_API std::vector<uint8_t> readGpuImage(const GpuImageRef& image);
} // namespace lila::render
