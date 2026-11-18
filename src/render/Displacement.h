#pragma once

#include "core/Export.h"
#include "render/RenderMesh.h"

#include <volk.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace lila::render
{
class VulkanContext;
class GpuImage;
}

/**
 * Displacement preview: a mesh subdivided on the CPU (tessellate) whose vertices the GPU moves along their smooth normals
 * by a height map every time the height changes (record), so painting height reshapes the surface live. Each material
 * section of a tessellated mesh owns a contiguous run of vertices (RenderMesh::displaceRanges), displaced by that
 * section's material height (Material::heightMap).
 */
namespace lila::render::displacement
{
/** The mesh with every triangle split into 4^level, its vertices grouped by section, with displacement directions. */
LILA_UI_API std::shared_ptr<RenderMesh> tessellate(const RenderMesh& mesh, int level);

/**
 * The subdivision level that brings the mesh's average edge near edgePixels device pixels in a view heightPx tall, at
 * most maxLevel and no more than triangleBudget triangles after subdividing.
 */
LILA_UI_API int levelFor(const RenderMesh& mesh, const glm::dmat4& world, const glm::dmat4& view, const glm::dmat4& proj, double heightPx,
                         double edgePixels, int maxLevel = 6, size_t triangleBudget = 4u << 20);

/** Texels of one height image, row 0 at the top, tightly packed in format; the first channel is the height. */
struct HeightImage
{
    int width = 0;
    int height = 0;
    TextureFormat format = TextureFormat::R32F;
    std::span<const uint8_t> texels;
};

/** The height at uv (V-up), bilinear and repeating, as the GPU kernel reads it. */
LILA_UI_API float sampleHeight(const HeightImage& image, glm::vec2 uv);

/** How one section displaces: offset = (height - mid) * depth along the vertex's direction. */
struct SectionParams
{
    float depth = 0.0f;
    float mid = 0.5f;
    bool uv1 = false;
};

/** The CPU reference: vertices (the tessellated mesh's, displaced in place) moved as record would move them. */
LILA_UI_API void displaceCpu(std::span<MeshVertex> vertices, std::span<const glm::vec4> dirs, uint32_t first, uint32_t count,
                             const HeightImage& image, const SectionParams& params);

/** Push constants of displace.slang. */
struct Push
{
    uint64_t vertices = 0; // MeshVertex, written
    uint64_t base = 0;     // MeshVertex, undisplaced
    uint64_t dirs = 0;     // vec4 per vertex
    uint64_t height = 0;   // texels, tightly packed
    uint32_t firstVertex = 0;
    uint32_t count = 0;
    uint32_t width = 0;
    uint32_t heightPx = 0;
    uint32_t format = 0; // TextureFormat
    uint32_t uv1 = 0;
    int32_t tileU = -1; // only vertices whose uv lies in this UDIM tile; -1 for every vertex
    int32_t tileV = -1;
    float depth = 0.0f;
    float mid = 0.5f;
    float pad[2]{};
};

/** displace.slang's pipeline. */
struct LILA_UI_API Kernel
{
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    /** False when the shader is missing or rejected. */
    bool init(VulkanContext& vk);
    void destroy(VulkanContext& vk);
    bool valid() const { return pipeline != VK_NULL_HANDLE; }
};

/**
 * Displaces a tessellated mesh's vertices on the GPU now, every section with the same images (one per UDIM tile {u, v},
 * or one plain image at {-1, -1}) and params, and returns them. Waits for the render thread; throws when it can't run.
 */
LILA_UI_API std::vector<MeshVertex> displaceNow(const RenderMesh& mesh, const std::vector<std::pair<std::shared_ptr<GpuImage>, glm::ivec2>>& images,
                                                const SectionParams& params);

/** Records one section's displacement (vertices already hold the base copy). */
LILA_UI_API void record(VkCommandBuffer cmd, const Kernel& kernel, const Push& push);
} // namespace lila::render::displacement
