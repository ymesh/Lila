#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "render/VulkanContext.h"

#include "core/CrashHandler.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Profile.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>

namespace lila::render
{
namespace
{/** One file per device; the driver rejects data from another device or driver version itself. */
std::filesystem::path pipelineCachePath(const VkPhysicalDeviceProperties& p)
{
    return paths::userConfigDir() / "cache" / "vkpipelines" / std::format("{:04x}_{:04x}.bin", p.vendorID, p.deviceID);
}

/** Past this the file is dropped and rebuilt: old shader builds accumulate in it. */
constexpr size_t kMaxPipelineCacheBytes = size_t(256) << 20;

std::string vkVersion(uint32_t v)
{
    return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "." + std::to_string(VK_API_VERSION_PATCH(v));
}

/** The GPU section of crash reports. */
void recordDeviceForCrashes(VkPhysicalDevice physical, const VkPhysicalDeviceProperties& p)
{
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
    vkGetPhysicalDeviceProperties2(physical, &p2);
    char ids[64];
    std::snprintf(ids, sizeof(ids), "vendor 0x%04X device 0x%04X", p.vendorID, p.deviceID);
    crash::setContext("gpu", "name", p.deviceName);
    crash::setContext("gpu", "ids", ids);
    crash::setContext("gpu", "driver", std::string(driver.driverName) + " " + driver.driverInfo);
    crash::setContext("gpu", "driverVersion.raw", std::to_string(p.driverVersion));
    crash::setContext("gpu", "vulkan.api", vkVersion(p.apiVersion));
    crash::setContext("gpu", "vulkan.conformance", std::to_string(driver.conformanceVersion.major) + "." +
                                                      std::to_string(driver.conformanceVersion.minor) + "." +
                                                      std::to_string(driver.conformanceVersion.subminor));
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        log::error("vulkan: {}", data->pMessage);
    else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        log::warn("vulkan: {}", data->pMessage);
    return VK_FALSE;
}

bool hasDeviceExtension(VkPhysicalDevice physical, const char* name)
{
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, exts.data());
    for (auto& e : exts)
        if (std::strcmp(e.extensionName, name) == 0)
            return true;
    return false;
}

bool hasInstanceExtension(const char* name)
{
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data());
    for (auto& e : exts)
        if (std::strcmp(e.extensionName, name) == 0)
            return true;
    return false;
}

/** The first feature the renderer can't run without that the device lacks, or null when it has them all. */
const char* missingRequiredFeature(VkPhysicalDevice physical)
{
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    vkGetPhysicalDeviceFeatures2(physical, &f2);
    const std::pair<VkBool32, const char*> required[] = {
        {f13.dynamicRendering, "dynamicRendering"},
        {f13.synchronization2, "synchronization2"},
        {f13.shaderDemoteToHelperInvocation, "shaderDemoteToHelperInvocation"},
        {f12.timelineSemaphore, "timelineSemaphore"},
        {f12.bufferDeviceAddress, "bufferDeviceAddress"},
        {f11.shaderDrawParameters, "shaderDrawParameters"},
        {f2.features.fillModeNonSolid, "fillModeNonSolid"},
    };
    for (const auto& [have, name] : required)
        if (!have)
            return name;
    if (!hasDeviceExtension(physical, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        return VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    return nullptr;
}

bool hasLayer(const char* name)
{
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> layers(n);
    vkEnumerateInstanceLayerProperties(&n, layers.data());
    for (auto& l : layers)
        if (std::strcmp(l.layerName, name) == 0)
            return true;
    return false;
}
} // namespace

bool VulkanContext::init(bool validation)
{
    LILA_ZONE();

    if (volkInitialize() != VK_SUCCESS)
    {
        log::error("vulkan: no Vulkan loader found; update the GPU driver");
        return false;
    }

    std::vector<const char*> exts = {VK_KHR_SURFACE_EXTENSION_NAME};
#ifdef _WIN32
    exts.push_back("VK_KHR_win32_surface");
#else
    exts.push_back("VK_KHR_xcb_surface");
#endif
    std::vector<const char*> layers;
    validation = validation && hasLayer("VK_LAYER_KHRONOS_validation");
    if (validation)
        layers.push_back("VK_LAYER_KHRONOS_validation");
    debugLabels = validation || hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debugLabels)
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Lila";
    app.pEngineName = "Lila";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = uint32_t(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    if (VkResult r = vkCreateInstance(&ici, nullptr, &instance); r != VK_SUCCESS)
    {
        log::error("vulkan: vkCreateInstance failed ({})", vkResultName(r));
        return false;
    }
    volkLoadInstance(instance);

    if (validation)
    {
        VkDebugUtilsMessengerCreateInfoEXT dci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dci.pfnUserCallback = debugCallback;
        vkCreateDebugUtilsMessengerEXT(instance, &dci, nullptr, &mMessenger);
    }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    // Discrete GPU with Vulkan 1.3 first.
    int best = -1;
    for (VkPhysicalDevice d : devices)
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.apiVersion < VK_API_VERSION_1_3)
            continue;
        if (const char* missing = missingRequiredFeature(d))
        {
            log::warn("vulkan: skipping {}, which lacks {}", p.deviceName, missing);
            continue;
        }
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
        if (score > best)
        {
            best = score;
            physical = d;
            properties = p;
        }
    }
    if (!physical)
    {
        log::error("vulkan: no GPU with Vulkan 1.3 and the features Lila needs; update the GPU driver");
        return false;
    }
    deviceName = properties.deviceName;
    recordDeviceForCrashes(physical, properties);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, qprops.data());
    for (uint32_t i = 0; i < qn; ++i)
        if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            queueFamily = i;
            break;
        }

    VkSampleCountFlags counts = properties.limits.framebufferColorSampleCounts & properties.limits.framebufferDepthSampleCounts;
    msaaSupported = counts;
    msaa = (counts & VK_SAMPLE_COUNT_8_BIT) ? VK_SAMPLE_COUNT_8_BIT
           : (counts & VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT
                                              : VK_SAMPLE_COUNT_1_BIT;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = VK_TRUE; // Slang lowers discard to demote
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.pNext = &f13;
    f11.shaderDrawParameters = VK_TRUE; // SV_VertexID in fullscreen passes
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f11;
    f12.timelineSemaphore = VK_TRUE;
    f12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    f2.features.fillModeNonSolid = VK_TRUE;
    VkPhysicalDeviceFeatures supported;
    vkGetPhysicalDeviceFeatures(physical, &supported);
    wideLines = supported.wideLines == VK_TRUE;
    f2.features.wideLines = supported.wideLines;
    largePoints = supported.largePoints == VK_TRUE;
    f2.features.largePoints = supported.largePoints;
    anisotropy = supported.samplerAnisotropy == VK_TRUE;
    f2.features.samplerAnisotropy = supported.samplerAnisotropy;
    bcTextures = supported.textureCompressionBC == VK_TRUE;
    f2.features.textureCompressionBC = supported.textureCompressionBC;

    // Optional features the baker uses: ask what the device has, enable only that.
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR baryHave{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rqHave{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, &baryHave};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asHave{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rqHave};
    VkPhysicalDeviceVulkan12Features f12Have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &asHave};
    VkPhysicalDeviceFeatures2 f2Have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12Have};
    vkGetPhysicalDeviceFeatures2(physical, &f2Have);

    std::vector<const char*> devExts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    const bool hasRt = hasDeviceExtension(physical, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
                       hasDeviceExtension(physical, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                       hasDeviceExtension(physical, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) && asHave.accelerationStructure &&
                       rqHave.rayQuery && std::getenv("LILA_VK_NO_RT") == nullptr;
    VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rq};
    if (hasRt)
    {
        devExts.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        devExts.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        devExts.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        as.accelerationStructure = VK_TRUE;
        rq.rayQuery = VK_TRUE;
        f13.pNext = &as;
        rayQuery = true;
        VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &asProps};
        vkGetPhysicalDeviceProperties2(physical, &p2);
        asScratchAlignment = std::max(1u, asProps.minAccelerationStructureScratchOffsetAlignment);
    }
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR bary{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
    if (hasDeviceExtension(physical, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME) && baryHave.fragmentShaderBarycentric &&
        std::getenv("LILA_VK_NO_BARYCENTRIC") == nullptr)
    {
        devExts.push_back(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
        bary.fragmentShaderBarycentric = VK_TRUE;
        bary.pNext = f13.pNext;
        f13.pNext = &bary;
        fragmentBarycentric = true;
    }
    if (hasDeviceExtension(physical, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME))
    {
        devExts.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        memoryBudget = true;
    }
    // The profiler's GPU zones: CPU and GPU clocks read together, and query resets from the host.
    if (hasDeviceExtension(physical, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME))
        calibratedTimestamps = VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
    else if (hasDeviceExtension(physical, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME))
        calibratedTimestamps = VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
    if (calibratedTimestamps && f12Have.hostQueryReset)
    {
        devExts.push_back(calibratedTimestamps);
        f12.hostQueryReset = VK_TRUE;
    }
    else
        calibratedTimestamps = nullptr;
#ifdef _WIN32
    if (hasDeviceExtension(physical, "VK_KHR_external_memory_win32"))
    {
        devExts.push_back("VK_KHR_external_memory_win32");
        externalMemory = true;
    }
#endif
    {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
        vkGetPhysicalDeviceProperties2(physical, &p2);
        std::memcpy(deviceUuid.data(), id.deviceUUID, deviceUuid.size());
    }
    descriptorIndexing = f12Have.descriptorIndexing && f12Have.runtimeDescriptorArray && f12Have.descriptorBindingPartiallyBound &&
                         f12Have.shaderSampledImageArrayNonUniformIndexing && f12Have.descriptorBindingSampledImageUpdateAfterBind &&
                         f12Have.descriptorBindingVariableDescriptorCount && f12Have.descriptorBindingUpdateUnusedWhilePending &&
                         std::getenv("LILA_VK_NO_BINDLESS") == nullptr;
    if (descriptorIndexing)
    {
        f12.descriptorIndexing = VK_TRUE;
        f12.runtimeDescriptorArray = VK_TRUE;
        f12.descriptorBindingPartiallyBound = VK_TRUE;
        f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        f12.descriptorBindingVariableDescriptorCount = VK_TRUE;
        f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    }
    shaderFloat16 = f12Have.shaderFloat16 == VK_TRUE;
    f12.shaderFloat16 = f12Have.shaderFloat16;
    int64Atomics = f12Have.shaderBufferInt64Atomics == VK_TRUE;
    f12.shaderBufferInt64Atomics = f12Have.shaderBufferInt64Atomics;
    shaderInt64 = supported.shaderInt64 == VK_TRUE;
    f2.features.shaderInt64 = supported.shaderInt64;
    shaderFloat64 = supported.shaderFloat64 == VK_TRUE;
    f2.features.shaderFloat64 = supported.shaderFloat64;
    fragmentStores = supported.fragmentStoresAndAtomics == VK_TRUE;
    f2.features.fragmentStoresAndAtomics = supported.fragmentStoresAndAtomics;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(devExts.size());
    dci.ppEnabledExtensionNames = devExts.data();
    if (VkResult r = vkCreateDevice(physical, &dci, nullptr, &device); r != VK_SUCCESS)
    {
        log::error("vulkan: vkCreateDevice failed ({})", vkResultName(r));
        return false;
    }
    volkLoadDevice(device);
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    VmaVulkanFunctions fns{};
    fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci{};
    aci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT | (memoryBudget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0);
    aci.vulkanApiVersion = VK_API_VERSION_1_3;
    aci.physicalDevice = physical;
    aci.device = device;
    aci.instance = instance;
    aci.pVulkanFunctions = &fns;
    vmaCreateAllocator(&aci, &allocator);

    {
        std::vector<char> data;
        if (std::ifstream in(pipelineCachePath(properties), std::ios::binary); in)
            data.assign(std::istreambuf_iterator<char>(in), {});
        if (data.size() > kMaxPipelineCacheBytes)
            data.clear();
        VkPipelineCacheCreateInfo pc{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        pc.initialDataSize = data.size();
        pc.pInitialData = data.empty() ? nullptr : data.data();
        if (vkCreatePipelineCache(device, &pc, nullptr, &pipelineCache) != VK_SUCCESS)
        {
            pc.initialDataSize = 0;
            pc.pInitialData = nullptr;
            vkCreatePipelineCache(device, &pc, nullptr, &pipelineCache);
        }
    }

    log::info("vulkan: {} ({}x MSAA{}{})", deviceName, int(msaa), rayQuery ? ", ray query" : "", validation ? ", validation on" : "");
    return true;
}

void VulkanContext::savePipelineCache()
{
    LILA_ZONE();
    size_t size = 0;
    if (!pipelineCache || vkGetPipelineCacheData(device, pipelineCache, &size, nullptr) != VK_SUCCESS || size == 0)
        return;
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(device, pipelineCache, &size, data.data()) != VK_SUCCESS)
        return;
    const std::filesystem::path path = pipelineCachePath(properties);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    // Another Lila process may be writing the same file.
    std::filesystem::path tmp = path;
    tmp += std::format(".{}.tmp", reinterpret_cast<uintptr_t>(this));
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(data.data(), std::streamsize(size));
        if (!out)
            return;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec)
        std::filesystem::remove(tmp, ec);
}

void VulkanContext::shutdown()
{
    if (device)
        vkDeviceWaitIdle(device);
    if (pipelineCache)
    {
        savePipelineCache();
        vkDestroyPipelineCache(device, pipelineCache, nullptr);
        pipelineCache = VK_NULL_HANDLE;
    }
    if (allocator)
        vmaDestroyAllocator(allocator);
    if (device)
        vkDestroyDevice(device, nullptr);
    if (mMessenger)
        vkDestroyDebugUtilsMessengerEXT(instance, mMessenger, nullptr);
    if (instance)
        vkDestroyInstance(instance, nullptr);
    allocator = VK_NULL_HANDLE;
    device = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
}

const char* vkResultName(VkResult r)
{
    switch (r)
    {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    default: return "VkResult";
    }
}
} // namespace lila::render
