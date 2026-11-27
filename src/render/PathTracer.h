#pragma once

#include "core/Export.h"
#include "render/Compute.h"
#include "render/FrameState.h"

#include <volk.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace lila::render
{
class VulkanContext;
struct EnvironmentData;

/** One acceleration structure instance as pathtrace.slang reads it. */
struct PtInstance
{
    uint64_t vertices = 0; // MeshVertex
    uint64_t indices = 0;
    glm::vec4 rows[3]{};   // world matrix rows, camera-relative
    uint32_t firstSection = 0;
    uint32_t sectionCount = 0;
    uint32_t tint[2]{}; // instance colour as four halves: linear rgb, amount (read under kInstanceColor)
};
static_assert(sizeof(PtInstance) == 80);

/** One material section: the mesh shader's inputs plus the extended channels. */
struct PtSection
{
    uint32_t firstTriangle = 0;
    uint32_t triangleCount = 0;
    uint32_t flags = 0;
    uint32_t sampling = 0;
    glm::vec4 base{0.5f, 0.5f, 0.5f, 0.55f}; // rgb, roughness
    glm::vec4 surface{0.0f, 0.5f, 1.0f, 0.3333f}; // metallic, specular, opacity, opacity clip
    glm::vec4 emissive{0.0f};                // rgb, w shading model
    glm::uvec4 maps{0xffffffffu};            // 16-bit table handles: base | normal << 16, ORM | emissive << 16, then extended maps
    glm::vec4 ext0{0.0f, 0.0f, 0.0f, 0.5f};  // sheen rgb, sheen roughness
    glm::vec4 ext1{0.0f, 0.05f, 0.0f, 0.0f}; // coat, coat roughness, anisotropy, anisotropy rotation (radians)
    glm::vec4 ext2{0.0f};                    // subsurface rgb, transmission
    glm::vec4 ext3{1.0f, 1.0f, 1.0f, 1.5f};  // specular colour times weight, ior
    glm::uvec4 extMaps0{0xffffffffu};        // 16-bit table handles of the ExtendedShading maps, two to a word
    glm::uvec4 extMaps1{0xffffffffu};
    float subsurfaceRadius = 1.0f;
    float pad2[3]{};
};
static_assert(sizeof(PtSection) == 192);

/** pathtrace.slang's parameters, read through a device address. */
struct PtParams
{
    glm::mat4 invViewProj{1.0f};
    glm::vec4 eye{0.0f};
    glm::vec4 camRight{0.0f};
    glm::vec4 camUp{0.0f};
    glm::vec4 camForward{0.0f};
    glm::vec4 env{1.0f, 0.0f, 1.0f, 1.0f};
    glm::vec4 envExtra{0.0f};
    glm::vec4 size{0.0f};
    glm::uvec4 counts{0u};
    glm::uvec4 counts2{0u};
    glm::uvec4 flags{0u};
    glm::vec4 misc{1.0f, 50.0f, 0.0f, 0.0f};
    uint64_t instances = 0, sections = 0, textureTable = 0, lights = 0, envTable = 0, pad = 0;
};

/**
 * The environment as a distribution to pick light directions from: res x res cells per cube face (Vulkan face order,
 * rows down), each weighted by its luminance times its solid angle. Returns the running sums (the last is 1) then each
 * cell's probability, 2 * 6 * res * res floats; empty for an environment without light.
 */
LILA_UI_API std::vector<float> environmentTable(const EnvironmentData& env, int& res);

/**
 * The path-traced viewport (FrameState::pathTrace): samples accumulate per viewport while nothing changes, and the
 * averaged, optionally denoised and tonemapped result is shown in place of the shaded meshes like an external
 * renderer's image (wires, gizmos and overlays draw over it, depth tested against its depth). Render thread only.
 */
class LILA_UI_API PathTracer
{
public:
    struct View;
    /** What one frame traces. */
    struct Inputs
    {
        VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
        std::span<const PtInstance> instances;
        std::span<const PtSection> sections;
        uint64_t textureTable = 0;
        uint64_t lights = 0; // GpuFrame then GpuLight entries
        uint32_t lightCount = 0;
        VkDescriptorSet bindlessSet = VK_NULL_HANDLE;
        VkImageView envView = VK_NULL_HANDLE;
        VkSampler envSampler = VK_NULL_HANDLE;
        VkExtent2D extent{};
        PtParams params; // camera, environment and display; counts and addresses are filled in
        FrameState::PathTrace settings;
        /** Everything else the image depends on (scene, materials, textures, lights); a change starts it over. */
        uint64_t signature = 0;
    };
    struct Result
    {
        TexturePtr color; // RGBA16F display-linear, straight alpha
        TexturePtr depth; // R32F cm along the view
        uint32_t samples = 0;
        bool converged = false;
    };

    PathTracer();
    ~PathTracer();
    /** False when the device can't trace rays or the shaders are missing. */
    bool init(VulkanContext& vk, VkDescriptorSetLayout bindlessLayout);
    bool ready() const;
    void destroy(VulkanContext& vk);
    /** After a new environment is uploaded, the device idle. */
    void setEnvironment(VulkanContext& vk, const EnvironmentData& env);

    static std::shared_ptr<View> makeView();
    /** The device idle. */
    void releaseView(VulkanContext& vk, View& view);

    /**
     * Adds this frame's samples to the view's sums (none once converged on an unchanged scene) and writes the shown
     * image. Staging buffers made on the way go to garbage, freed once cmd has finished. False when nothing could be traced.
     */
    bool record(VulkanContext& vk, VkCommandBuffer cmd, View& view, const Inputs& in, std::vector<compute::Buffer>& garbage, Result& out);

    /** After record: copies the averaged linear light (exposed, alpha coverage) for the host. */
    void recordReadback(VulkanContext& vk, VkCommandBuffer cmd, View& view);
    /** Once that work finished: RGBA floats, row 0 at the top. */
    std::vector<float> readback(VulkanContext& vk, View& view);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
} // namespace lila::render
