// HgiVulkan includes vulkan.h itself; without prototypes here, every call goes through the table loaded below.
#define VK_NO_PROTOTYPES

#include "UsdHydraInterop.h"

#include <pxr/imaging/hgiVulkan/conversions.h>
#include <pxr/imaging/hgiVulkan/device.h>
#include <pxr/imaging/hgiVulkan/commandQueue.h>
#include <pxr/imaging/hgiVulkan/hgi.h>
#include <pxr/imaging/hgiVulkan/instance.h>
#include <pxr/imaging/hgiVulkan/texture.h>

#include <chrono>
#include <cstring>
#include <vector>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

PXR_NAMESPACE_USING_DIRECTIVE

namespace lila::usd::hydra
{
namespace
{
#define LILA_INTEROP_INSTANCE_FNS(X)                                                                                                       \
    X(vkGetDeviceProcAddr)                                                                                                                 \
    X(vkGetPhysicalDeviceProperties2)                                                                                                      \
    X(vkGetPhysicalDeviceMemoryProperties)                                                                                                 \
    X(vkGetPhysicalDeviceFormatProperties)                                                                                                 \
    X(vkEnumerateDeviceExtensionProperties)

#define LILA_INTEROP_DEVICE_FNS(X)                                                                                                         \
    X(vkCreateImage)                                                                                                                       \
    X(vkDestroyImage)                                                                                                                      \
    X(vkGetImageMemoryRequirements)                                                                                                        \
    X(vkBindImageMemory)                                                                                                                   \
    X(vkCreateBuffer)                                                                                                                      \
    X(vkDestroyBuffer)                                                                                                                     \
    X(vkGetBufferMemoryRequirements)                                                                                                       \
    X(vkBindBufferMemory)                                                                                                                  \
    X(vkAllocateMemory)                                                                                                                    \
    X(vkFreeMemory)                                                                                                                        \
    X(vkCreateCommandPool)                                                                                                                 \
    X(vkDestroyCommandPool)                                                                                                                \
    X(vkAllocateCommandBuffers)                                                                                                            \
    X(vkResetCommandBuffer)                                                                                                                \
    X(vkBeginCommandBuffer)                                                                                                                \
    X(vkEndCommandBuffer)                                                                                                                  \
    X(vkCmdPipelineBarrier)                                                                                                                \
    X(vkCmdCopyImage)                                                                                                                      \
    X(vkCmdBlitImage)                                                                                                                      \
    X(vkCmdCopyImageToBuffer)                                                                                                              \
    X(vkCmdCopyBufferToImage)                                                                                                              \
    X(vkCreateFence)                                                                                                                       \
    X(vkDestroyFence)                                                                                                                      \
    X(vkWaitForFences)                                                                                                                     \
    X(vkResetFences)                                                                                                                       \
    X(vkQueueSubmit)

#if defined(_WIN32)
constexpr VkExternalMemoryHandleTypeFlagBits kHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
constexpr VkExternalMemoryHandleTypeFlagBits kHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

/** vkGetInstanceProcAddr from the Vulkan loader already in the process (Lila's renderer loaded it); null when none is. */
PFN_vkGetInstanceProcAddr loaderProc()
{
#if defined(_WIN32)
    const HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
    return loader ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr")) : nullptr;
#else
    void* loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD);
    return loader ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr")) : nullptr;
#endif
}

VkImageAspectFlags aspectOf(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

VkFormat vkFormatOf(const HgiTexture* t)
{
    const HgiTextureDesc& d = t->GetDescriptor();
    return HgiVulkanConversions::GetFormat(d.format, (d.usage & HgiTextureUsageBitsDepthTarget) != 0);
}
} // namespace

struct HgiInterop::Impl
{
#define LILA_INTEROP_DECLARE(name) PFN_##name name = nullptr;
    LILA_INTEROP_INSTANCE_FNS(LILA_INTEROP_DECLARE)
    LILA_INTEROP_DEVICE_FNS(LILA_INTEROP_DECLARE)
#undef LILA_INTEROP_DECLARE

    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    HgiVulkanCommandQueue* hgiQueue = nullptr;

    struct Target
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        int width = 0;
        int height = 0;
    };
    struct Slot
    {
        Target color, depth;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        /** Depth goes through a buffer: Vulkan copies no depth image straight into a colour one. */
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    };
    std::vector<Slot> slots;

    uint32_t memoryType(uint32_t bits) const
    {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(physical, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                return i;
        return UINT32_MAX;
    }

    void destroy(Target& t)
    {
        if (t.image)
            vkDestroyImage(device, t.image, nullptr);
        if (t.memory)
            vkFreeMemory(device, t.memory, nullptr);
        t = {};
    }

    void destroy(Slot& s)
    {
        if (s.fence)
        {
            vkWaitForFences(device, 1, &s.fence, VK_TRUE, UINT64_MAX);
            vkDestroyFence(device, s.fence, nullptr);
        }
        destroy(s.color);
        destroy(s.depth);
        if (s.staging)
            vkDestroyBuffer(device, s.staging, nullptr);
        if (s.stagingMemory)
            vkFreeMemory(device, s.stagingMemory, nullptr);
        // The pool frees the command buffer.
        s = {};
    }

    bool import(const SharedTarget& t, Target& out)
    {
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = kHandleType;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external};
        ici.flags = t.flags;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VkFormat(t.format);
        ici.extent = {uint32_t(t.width), uint32_t(t.height), 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = t.usage;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!t.handle || vkCreateImage(device, &ici, nullptr, &out.image) != VK_SUCCESS)
            return false;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, out.image, &req);
        const uint32_t type = memoryType(req.memoryTypeBits);
        if (type == UINT32_MAX || req.size > t.size)
        {
            destroy(out);
            return false;
        }
#if defined(_WIN32)
        VkImportMemoryWin32HandleInfoKHR imported{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
        imported.handleType = kHandleType;
        imported.handle = HANDLE(t.handle);
#else
        // Unreached: create() declines outside Windows until Lila's renderer exports fds (render/SharedMemory.cpp).
        VkImportMemoryFdInfoKHR imported{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
        imported.handleType = kHandleType;
        imported.fd = -1;
#endif
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imported};
        dedicated.image = out.image;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated};
        ai.allocationSize = t.size;
        ai.memoryTypeIndex = type;
        if (vkAllocateMemory(device, &ai, nullptr, &out.memory) != VK_SUCCESS || vkBindImageMemory(device, out.image, out.memory, 0) != VK_SUCCESS)
        {
            destroy(out);
            return false;
        }
        out.width = t.width;
        out.height = t.height;
        return true;
    }

    bool makeStaging(Slot& s, VkDeviceSize bytes)
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(device, &bci, nullptr, &s.staging) != VK_SUCCESS)
            return false;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, s.staging, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memoryType(req.memoryTypeBits);
        return ai.memoryTypeIndex != UINT32_MAX && vkAllocateMemory(device, &ai, nullptr, &s.stagingMemory) == VK_SUCCESS &&
               vkBindBufferMemory(device, s.staging, s.stagingMemory, 0) == VK_SUCCESS;
    }

    void barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags srcStage,
                 VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess, uint32_t srcFamily = VK_QUEUE_FAMILY_IGNORED,
                 uint32_t dstFamily = VK_QUEUE_FAMILY_IGNORED)
    {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = srcFamily;
        b.dstQueueFamilyIndex = dstFamily;
        b.image = image;
        b.subresourceRange = {aspect, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    /** Leaves a target as Lila samples it and hands it to Lila's device, whose acquire matches these layouts. */
    void release(VkCommandBuffer cmd, VkImage image)
    {
        barrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT);
        barrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, family,
                VK_QUEUE_FAMILY_EXTERNAL);
    }
};

HgiInterop::HgiInterop() : m(std::make_unique<Impl>()) {}

HgiInterop::~HgiInterop()
{
    if (!m->device)
        return;
    clearSlots();
    if (m->pool)
        m->vkDestroyCommandPool(m->device, m->pool, nullptr);
}

std::unique_ptr<HgiInterop> HgiInterop::create(Hgi* hgi, const std::array<uint8_t, 16>& deviceUuid, std::string& why)
{
    auto* vk = dynamic_cast<HgiVulkan*>(hgi);
    if (!vk || !vk->GetPrimaryDevice() || !vk->GetVulkanInstance())
    {
        why = "Hydra's GPU driver is not Vulkan";
        return nullptr;
    }
#if !defined(_WIN32)
    // Lila's renderer exports no memory outside Windows yet (render/SharedMemory.cpp).
    why = "GPU sharing with Hydra is Windows-only for now";
    return nullptr;
#endif
    const auto getInstanceProc = loaderProc();
    if (!getInstanceProc)
    {
        why = "the Vulkan loader is not loaded";
        return nullptr;
    }
    std::unique_ptr<HgiInterop> out(new HgiInterop());
    Impl& m = *out->m;
    HgiVulkanDevice* dev = vk->GetPrimaryDevice();
    const VkInstance instance = vk->GetVulkanInstance()->GetVulkanInstance();
#define LILA_INTEROP_LOAD_INSTANCE(name) m.name = reinterpret_cast<PFN_##name>(getInstanceProc(instance, #name));
    LILA_INTEROP_INSTANCE_FNS(LILA_INTEROP_LOAD_INSTANCE)
#undef LILA_INTEROP_LOAD_INSTANCE
    m.physical = dev->GetVulkanPhysicalDevice();
    m.device = dev->GetVulkanDevice();
    m.family = dev->GetGfxQueueFamilyIndex();
    m.hgiQueue = dev->GetCommandQueue();
    m.queue = m.hgiQueue->GetVulkanGraphicsQueue();
    bool loaded = m.vkGetDeviceProcAddr != nullptr;
#define LILA_INTEROP_LOAD_DEVICE(name)                                                                                                     \
    if (loaded)                                                                                                                            \
        loaded = (m.name = reinterpret_cast<PFN_##name>(m.vkGetDeviceProcAddr(m.device, #name))) != nullptr;
    LILA_INTEROP_DEVICE_FNS(LILA_INTEROP_LOAD_DEVICE)
#undef LILA_INTEROP_LOAD_DEVICE
    if (!loaded || !m.vkGetPhysicalDeviceProperties2 || !m.vkEnumerateDeviceExtensionProperties)
    {
        m.device = VK_NULL_HANDLE;
        why = "Vulkan functions are missing";
        return nullptr;
    }

    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    m.vkGetPhysicalDeviceProperties2(m.physical, &p2);
    if (std::memcmp(id.deviceUUID, deviceUuid.data(), deviceUuid.size()) != 0)
    {
        m.device = VK_NULL_HANDLE;
        why = std::string("Hydra runs on another GPU (") + p2.properties.deviceName + ")";
        return nullptr;
    }
    // HgiVulkan turns external memory on whenever the device has these, so having them means the device enabled them.
    uint32_t count = 0;
    m.vkEnumerateDeviceExtensionProperties(m.physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    m.vkEnumerateDeviceExtensionProperties(m.physical, nullptr, &count, exts.data());
    int found = 0;
    for (const VkExtensionProperties& e : exts)
        for (const char* want : {VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
#if defined(_WIN32)
                                 VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME})
#else
                                 VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME})
#endif
            found += std::strcmp(e.extensionName, want) == 0;
    if (found < 4)
    {
        m.device = VK_NULL_HANDLE;
        why = "Hydra's Vulkan device can't import memory";
        return nullptr;
    }
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = m.family;
    if (m.vkCreateCommandPool(m.device, &pci, nullptr, &m.pool) != VK_SUCCESS)
    {
        m.device = VK_NULL_HANDLE;
        why = "no command pool on Hydra's device";
        return nullptr;
    }
    return out;
}

bool HgiInterop::setSlot(size_t n, const SharedTarget& color, const SharedTarget& depth)
{
    if (m->slots.size() <= n)
        m->slots.resize(n + 1);
    Impl::Slot& s = m->slots[n];
    m->destroy(s);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = m->pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    const bool ok = m->import(color, s.color) && m->import(depth, s.depth) &&
                    m->makeStaging(s, VkDeviceSize(depth.width) * VkDeviceSize(depth.height) * 4) &&
                    m->vkAllocateCommandBuffers(m->device, &cai, &s.cmd) == VK_SUCCESS && m->vkCreateFence(m->device, &fci, nullptr, &s.fence) == VK_SUCCESS;
    if (!ok)
        m->destroy(s);
    return ok;
}

void HgiInterop::clearSlots()
{
    for (Impl::Slot& s : m->slots)
        m->destroy(s);
    m->slots.clear();
}

bool HgiInterop::copyable(HgiTexture* texture, bool color) const
{
    auto* t = dynamic_cast<HgiVulkanTexture*>(texture);
    if (!t || t->GetDescriptor().sampleCount != HgiSampleCount1 || t->GetImageLayout() == VK_IMAGE_LAYOUT_UNDEFINED)
        return false;
    const VkFormat f = vkFormatOf(t);
    if (!color)
        return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_R32_SFLOAT;
    if (f == VK_FORMAT_R16G16B16A16_SFLOAT)
        return true;
    VkFormatProperties fp;
    m->vkGetPhysicalDeviceFormatProperties(m->physical, f, &fp);
    return aspectOf(f) == VK_IMAGE_ASPECT_COLOR_BIT && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT);
}

bool HgiInterop::copy(size_t n, HgiTexture* color, HgiTexture* depth)
{
    if (n >= m->slots.size() || !m->slots[n].cmd || !copyable(color, true) || (depth && !copyable(depth, false)))
        return false;
    Impl::Slot& s = m->slots[n];
    auto* c = static_cast<HgiVulkanTexture*>(color);
    const HgiTextureDesc& cd = c->GetDescriptor();
    if (cd.dimensions[0] != s.color.width || cd.dimensions[1] != s.color.height)
        return false;
    auto* d = static_cast<HgiVulkanTexture*>(depth);
    if (d && (d->GetDescriptor().dimensions[0] != s.depth.width || d->GetDescriptor().dimensions[1] != s.depth.height))
        return false;

    VkDevice dev = m->device;
    m->vkWaitForFences(dev, 1, &s.fence, VK_TRUE, UINT64_MAX);
    m->vkResetFences(dev, 1, &s.fence);
    m->vkResetCommandBuffer(s.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    m->vkBeginCommandBuffer(s.cmd, &bi);
    VkCommandBuffer cmd = s.cmd;
    constexpr auto all = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    constexpr auto transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    constexpr auto anyAccess = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

    const VkFormat cf = vkFormatOf(c);
    const VkImageLayout cl = c->GetImageLayout();
    m->barrier(cmd, c->GetImage(), VK_IMAGE_ASPECT_COLOR_BIT, cl, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, all, VK_ACCESS_MEMORY_WRITE_BIT, transfer,
               VK_ACCESS_TRANSFER_READ_BIT);
    // What the target held is discarded, so it needs no ownership back from Lila's device.
    m->barrier(cmd, s.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, transfer, VK_ACCESS_TRANSFER_WRITE_BIT);
    if (cf == VK_FORMAT_R16G16B16A16_SFLOAT)
    {
        VkImageCopy region{};
        region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {uint32_t(s.color.width), uint32_t(s.color.height), 1};
        m->vkCmdCopyImage(cmd, c->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    else
    {
        VkImageBlit region{};
        region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffsets[1] = region.dstOffsets[1] = {s.color.width, s.color.height, 1};
        m->vkCmdBlitImage(cmd, c->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                          VK_FILTER_NEAREST);
    }
    m->barrier(cmd, c->GetImage(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, cl, transfer, 0, all, anyAccess);
    m->release(cmd, s.color.image);

    if (d)
    {
        const VkFormat df = vkFormatOf(d);
        const VkImageAspectFlags aspect = aspectOf(df);
        const VkImageLayout dl = d->GetImageLayout();
        m->barrier(cmd, d->GetImage(), aspect, dl, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, all, VK_ACCESS_MEMORY_WRITE_BIT, transfer,
                   VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VkImageAspectFlags(aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT), 0, 0, 1};
        region.imageExtent = {uint32_t(s.depth.width), uint32_t(s.depth.height), 1};
        m->vkCmdCopyImageToBuffer(cmd, d->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.staging, 1, &region);
        m->barrier(cmd, d->GetImage(), aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dl, transfer, 0, all, anyAccess);

        VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer = s.staging;
        bb.size = VK_WHOLE_SIZE;
        m->vkCmdPipelineBarrier(cmd, transfer, transfer, 0, 0, nullptr, 1, &bb, 0, nullptr);
        m->barrier(cmd, s.depth.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, transfer, VK_ACCESS_TRANSFER_WRITE_BIT);
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        m->vkCmdCopyBufferToImage(cmd, s.staging, s.depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        m->release(cmd, s.depth.image);
    }
    m->vkEndCommandBuffer(cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    // HgiVulkan holds submitted work back until something waits; the copy has to follow the frame it copies. It submits
    // from this thread too, which keeps the queue externally synchronized.
    m->hgiQueue->Flush(HgiSubmitWaitTypeNoWait);
    return m->vkQueueSubmit(m->queue, 1, &si, s.fence) == VK_SUCCESS;
}

void HgiInterop::wait(size_t n)
{
    if (n < m->slots.size() && m->slots[n].fence)
        m->vkWaitForFences(m->device, 1, &m->slots[n].fence, VK_TRUE, UINT64_MAX);
}
struct HgiDeviceWatch::Impl
{
    PFN_vkCreateFence vkCreateFence = nullptr;
    PFN_vkDestroyFence vkDestroyFence = nullptr;
    PFN_vkGetFenceStatus vkGetFenceStatus = nullptr;
    PFN_vkResetFences vkResetFences = nullptr;
    PFN_vkWaitForFences vkWaitForFences = nullptr;
    PFN_vkQueueSubmit vkQueueSubmit = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;
    bool lost = false;
    std::chrono::steady_clock::time_point submitted;
};

HgiDeviceWatch::HgiDeviceWatch() : m(std::make_unique<Impl>()) {}

HgiDeviceWatch::~HgiDeviceWatch()
{
    if (!m->fence)
        return;
    // A lost device reports its fences signalled, so this never waits long on one.
    if (m->pending)
        m->vkWaitForFences(m->device, 1, &m->fence, VK_TRUE, 2'000'000'000);
    m->vkDestroyFence(m->device, m->fence, nullptr);
}

std::unique_ptr<HgiDeviceWatch> HgiDeviceWatch::create(Hgi* hgi)
{
    auto* vk = dynamic_cast<HgiVulkan*>(hgi);
    const auto getInstanceProc = loaderProc();
    if (!vk || !vk->GetPrimaryDevice() || !vk->GetVulkanInstance() || !getInstanceProc)
        return nullptr;
    std::unique_ptr<HgiDeviceWatch> out(new HgiDeviceWatch());
    Impl& m = *out->m;
    HgiVulkanDevice* dev = vk->GetPrimaryDevice();
    const auto getDeviceProc =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(getInstanceProc(vk->GetVulkanInstance()->GetVulkanInstance(), "vkGetDeviceProcAddr"));
    if (!getDeviceProc)
        return nullptr;
    m.device = dev->GetVulkanDevice();
    m.queue = dev->GetCommandQueue()->GetVulkanGraphicsQueue();
#define LILA_WATCH_LOAD(name) m.name = reinterpret_cast<PFN_##name>(getDeviceProc(m.device, #name));
    LILA_WATCH_LOAD(vkCreateFence)
    LILA_WATCH_LOAD(vkDestroyFence)
    LILA_WATCH_LOAD(vkGetFenceStatus)
    LILA_WATCH_LOAD(vkResetFences)
    LILA_WATCH_LOAD(vkWaitForFences)
    LILA_WATCH_LOAD(vkQueueSubmit)
#undef LILA_WATCH_LOAD
    if (!m.vkCreateFence || !m.vkDestroyFence || !m.vkGetFenceStatus || !m.vkResetFences || !m.vkWaitForFences || !m.vkQueueSubmit)
        return nullptr;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (m.vkCreateFence(m.device, &fci, nullptr, &m.fence) != VK_SUCCESS)
        return nullptr;
    return out;
}

bool HgiDeviceWatch::lost()
{
    constexpr auto kHung = std::chrono::seconds(10);
    if (m->lost)
        return true;
    if (m->pending)
    {
        const VkResult r = m->vkGetFenceStatus(m->device, m->fence);
        if (r == VK_NOT_READY)
        {
            m->lost = std::chrono::steady_clock::now() - m->submitted > kHung;
            return m->lost;
        }
        if (r != VK_SUCCESS)
            return m->lost = true;
        m->vkResetFences(m->device, 1, &m->fence);
        m->pending = false;
    }
    // Empty, so it signals once everything submitted before it has finished.
    if (const VkResult r = m->vkQueueSubmit(m->queue, 0, nullptr, m->fence); r != VK_SUCCESS)
        return m->lost = r == VK_ERROR_DEVICE_LOST;
    m->pending = true;
    m->submitted = std::chrono::steady_clock::now();
    return false;
}

} // namespace lila::usd::hydra
