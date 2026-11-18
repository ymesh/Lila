#include "render/Displacement.h"

#include "core/Log.h"
#include "render/Compute.h"
#include "render/GpuImage.h"
#include "render/Renderer.h"
#include "render/VulkanContext.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace lila::render::displacement
{
namespace
{
MeshVertex midpoint(const MeshVertex& a, const MeshVertex& b)
{
    MeshVertex m;
    m.pos = 0.5f * (a.pos + b.pos);
    const glm::vec3 n = a.normal + b.normal;
    m.normal = glm::dot(n, n) > 1e-20f ? glm::normalize(n) : a.normal;
    m.uv = 0.5f * (a.uv + b.uv);
    m.uv1 = 0.5f * (a.uv1 + b.uv1);
    const glm::vec3 t = glm::vec3(a.tangent) + glm::vec3(b.tangent);
    m.tangent = glm::vec4(glm::dot(t, t) > 1e-20f ? glm::normalize(t) : glm::vec3(a.tangent), a.tangent.w);
    m.color = HalfColor(0.5f * (glm::vec4(a.color) + glm::vec4(b.color)));
    return m;
}

struct PosKey
{
    float x, y, z;
    bool operator==(const PosKey&) const = default;
};

struct PosHash
{
    size_t operator()(const PosKey& k) const
    {
        uint32_t b[3];
        std::memcpy(b, &k, sizeof(b));
        return (size_t(b[0]) * 73856093u) ^ (size_t(b[1]) * 19349663u) ^ (size_t(b[2]) * 83492791u);
    }
};

float texel(const HeightImage& im, int x, int y)
{
    x = ((x % im.width) + im.width) % im.width;
    y = ((y % im.height) + im.height) % im.height;
    const size_t i = size_t(y) * size_t(im.width) + size_t(x);
    const uint8_t* p = im.texels.data() + i * size_t(texelBytes(im.format));
    switch (im.format)
    {
    case TextureFormat::R8:
    case TextureFormat::RG8:
    case TextureFormat::RGBA8: return float(p[0]) / 255.0f;
    case TextureFormat::R16F:
    case TextureFormat::RGBA16F:
    {
        uint16_t h;
        std::memcpy(&h, p, 2);
        return glm::unpackHalf1x16(h);
    }
    case TextureFormat::R32F:
    {
        float f;
        std::memcpy(&f, p, 4);
        return f;
    }
    }
    return 0.5f;
}
} // namespace

std::shared_ptr<RenderMesh> tessellate(const RenderMesh& mesh, int level)
{
    level = std::clamp(level, 0, 8);
    const RenderMesh& src = mesh.shareVertices ? *mesh.shareVertices : mesh;
    const std::vector<MeshVertex> dynamicCopy = [&] {
        if (!src.dynamic)
            return std::vector<MeshVertex>();
        std::scoped_lock lock(src.dynamic->mutex);
        return src.dynamic->vertices;
    }();
    std::span<const MeshVertex> verts = src.dynamic ? std::span<const MeshVertex>(dynamicCopy)
                                                    : std::span<const MeshVertex>(src.vertices.data(), src.vertices.size());

    auto out = std::make_shared<RenderMesh>();
    std::vector<MeshSection> sections = mesh.sections;
    if (sections.empty())
        sections.push_back({0, 0, uint32_t(mesh.indices.size())});
    std::vector<MeshVertex> vertices;
    for (const MeshSection& sec : sections)
    {
        const uint32_t first = uint32_t(vertices.size());
        std::unordered_map<uint32_t, uint32_t> local;
        std::vector<uint32_t> tris;
        const uint32_t end = std::min<uint32_t>(sec.firstIndex + sec.indexCount, uint32_t(mesh.indices.size()));
        for (uint32_t i = sec.firstIndex; i + 2 < end; i += 3)
            for (int k = 0; k < 3; ++k)
            {
                const uint32_t g = mesh.indices[i + k];
                auto [it, added] = local.try_emplace(g, uint32_t(vertices.size()));
                if (added)
                    vertices.push_back(g < verts.size() ? verts[g] : MeshVertex{});
                tris.push_back(it->second);
            }
        for (int l = 0; l < level; ++l)
        {
            std::unordered_map<uint64_t, uint32_t> mids;
            auto mid = [&](uint32_t a, uint32_t b) {
                const uint64_t key = a < b ? (uint64_t(a) << 32 | b) : (uint64_t(b) << 32 | a);
                auto [it, added] = mids.try_emplace(key, uint32_t(vertices.size()));
                if (added)
                    vertices.push_back(midpoint(vertices[a], vertices[b]));
                return it->second;
            };
            std::vector<uint32_t> next;
            next.reserve(tris.size() * 4);
            for (size_t t = 0; t + 2 < tris.size(); t += 3)
            {
                const uint32_t a = tris[t], b = tris[t + 1], c = tris[t + 2];
                const uint32_t ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
                next.insert(next.end(), {a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca});
            }
            tris = std::move(next);
        }
        MeshSection s = sec;
        s.firstIndex = uint32_t(out->indices.size());
        s.indexCount = uint32_t(tris.size());
        out->indices.insert(out->indices.end(), tris.begin(), tris.end());
        if (!mesh.sections.empty())
            out->sections.push_back(s);
        out->displaceRanges.push_back({first, uint32_t(vertices.size()) - first});
    }

    // Directions are the area-weighted face normals summed over every vertex at the same point, so hard edges and seams
    // move together instead of tearing open.
    std::unordered_map<PosKey, glm::vec3, PosHash> sums;
    for (size_t t = 0; t + 2 < out->indices.size(); t += 3)
    {
        const MeshVertex &a = vertices[out->indices[t]], &b = vertices[out->indices[t + 1]], &c = vertices[out->indices[t + 2]];
        const glm::vec3 n = glm::cross(b.pos - a.pos, c.pos - a.pos);
        for (const MeshVertex* v : {&a, &b, &c})
            sums[PosKey{v->pos.x, v->pos.y, v->pos.z}] += n;
    }
    out->displaceDirs.resize(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i)
    {
        const MeshVertex& v = vertices[i];
        auto it = sums.find(PosKey{v.pos.x, v.pos.y, v.pos.z});
        glm::vec3 d = it != sums.end() ? it->second : v.normal;
        if (glm::dot(d, d) < 1e-20f)
            d = v.normal;
        if (glm::dot(d, d) < 1e-20f)
            d = glm::vec3(0.0f, 1.0f, 0.0f);
        out->displaceDirs[i] = glm::vec4(glm::normalize(d), 0.0f);
    }
    out->vertices.assign(vertices.begin(), vertices.end());
    out->edgePoints = mesh.edgePoints;
    out->edgeIndices = mesh.edgeIndices;
    out->boundsMin = mesh.boundsMin;
    out->boundsMax = mesh.boundsMax;
    return out;
}

int levelFor(const RenderMesh& mesh, const glm::dmat4& world, const glm::dmat4& view, const glm::dmat4& proj, double heightPx, double edgePixels,
             int maxLevel, size_t triangleBudget)
{
    const size_t triangles = mesh.indices.size() / 3;
    if (triangles == 0 || heightPx <= 0.0)
        return 0;
    const glm::dvec3 lo(mesh.boundsMin), hi(mesh.boundsMax);
    const glm::dvec3 centre = glm::dvec3(world * glm::dvec4(0.5 * (lo + hi), 1.0));
    const double scale = std::max({glm::length(glm::dvec3(world[0])), glm::length(glm::dvec3(world[1])), glm::length(glm::dvec3(world[2]))});
    const double radius = 0.5 * glm::length(hi - lo) * scale;
    const glm::dvec3 eyeSpace = glm::dvec3(view * glm::dvec4(centre, 1.0));
    double diameterPx;
    if (proj[3][3] == 1.0)
        diameterPx = 2.0 * radius * proj[1][1] * 0.5 * heightPx;
    else
    {
        const double dist = std::max(-eyeSpace.z - radius, radius * 0.25 + 1e-3);
        diameterPx = 2.0 * radius / dist * std::abs(proj[1][1]) * 0.5 * heightPx;
    }
    // A closed surface shows about half its triangles across its projected disc.
    const double edgePx = diameterPx / std::sqrt(std::max(double(triangles) * 0.5, 1.0)) * 1.4;
    int level = 0;
    double e = edgePx;
    while (level < maxLevel && e > edgePixels && triangles * (size_t(1) << (2 * (level + 1))) <= triangleBudget)
    {
        e *= 0.5;
        ++level;
    }
    return level;
}

float sampleHeight(const HeightImage& im, glm::vec2 uv)
{
    if (im.width < 1 || im.height < 1)
        return 0.5f;
    const float fu = uv.x - std::floor(uv.x), fv = uv.y - std::floor(uv.y);
    const float x = fu * float(im.width) - 0.5f, y = (1.0f - fv) * float(im.height) - 0.5f;
    const float x0 = std::floor(x), y0 = std::floor(y);
    const float tx = x - x0, ty = y - y0;
    const int ix = int(x0), iy = int(y0);
    const float a = texel(im, ix, iy), b = texel(im, ix + 1, iy), c = texel(im, ix, iy + 1), d = texel(im, ix + 1, iy + 1);
    return (a * (1.0f - tx) + b * tx) * (1.0f - ty) + (c * (1.0f - tx) + d * tx) * ty;
}

void displaceCpu(std::span<MeshVertex> vertices, std::span<const glm::vec4> dirs, uint32_t first, uint32_t count, const HeightImage& image,
                 const SectionParams& params)
{
    const uint32_t end = std::min<uint32_t>(first + count, uint32_t(std::min(vertices.size(), dirs.size())));
    for (uint32_t i = first; i < end; ++i)
    {
        MeshVertex& v = vertices[i];
        const float h = sampleHeight(image, params.uv1 ? v.uv1 : v.uv);
        v.pos += glm::vec3(dirs[i]) * ((h - params.mid) * params.depth);
    }
}

bool Kernel::init(VulkanContext& vk)
{
    if (pipeline)
        return true;
    const std::vector<uint32_t> code = compute::loadSpirv("displace.spv");
    if (code.empty())
        return false;
    VkShaderModule module = compute::shaderModule(vk, code);
    if (!module)
        return false;
    layout = compute::pipelineLayout(vk, sizeof(Push), VK_SHADER_STAGE_COMPUTE_BIT);
    pipeline = layout ? compute::computePipeline(vk, layout, module, "displaceCS") : VK_NULL_HANDLE;
    vkDestroyShaderModule(vk.device, module, nullptr);
    if (!pipeline)
    {
        log::warn("render: the displacement kernel was rejected; displacement preview is off");
        destroy(vk);
        return false;
    }
    return true;
}

void Kernel::destroy(VulkanContext& vk)
{
    if (pipeline)
        vkDestroyPipeline(vk.device, pipeline, nullptr);
    if (layout)
        compute::destroy(vk, layout);
    pipeline = VK_NULL_HANDLE;
    layout = VK_NULL_HANDLE;
}

void record(VkCommandBuffer cmd, const Kernel& kernel, const Push& push)
{
    if (!kernel.valid() || push.count == 0)
        return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel.pipeline);
    vkCmdPushConstants(cmd, kernel.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &push);
    vkCmdDispatch(cmd, (push.count + 63) / 64, 1, 1);
}
std::vector<MeshVertex> displaceNow(const RenderMesh& mesh, const std::vector<std::pair<std::shared_ptr<GpuImage>, glm::ivec2>>& images, const SectionParams& params)
{
    std::vector<MeshVertex> out(mesh.vertices.size());
    Renderer::get()
        .runTask([&](VulkanContext& vk) {
            Kernel kernel;
            if (!kernel.init(vk))
                throw std::runtime_error("no displacement kernel");
            const size_t bytes = mesh.vertices.size() * sizeof(MeshVertex);
            compute::Buffer base = compute::hostBuffer(vk, mesh.vertices.data(), bytes);
            compute::Buffer dirs = compute::hostBuffer(vk, mesh.displaceDirs.data(), mesh.displaceDirs.size() * sizeof(glm::vec4));
            compute::Buffer verts = compute::makeBuffer(vk, bytes, 0, compute::Host::Read);
            std::vector<compute::Buffer> texels;
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pci.queueFamilyIndex = vk.queueFamily;
            VkCommandPool pool;
            vkCreateCommandPool(vk.device, &pci, nullptr, &pool);
            ai.commandPool = pool;
            ai.commandBufferCount = 1;
            VkCommandBuffer cmd;
            vkAllocateCommandBuffers(vk.device, &ai, &cmd);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            vkBeginCommandBuffer(cmd, &bi);
            std::vector<compute::Buffer> staging;
            for (const auto& [img, tile] : images)
            {
                if (!img->prepare(vk, cmd, staging))
                    throw std::runtime_error("height image not made");
                texels.push_back(compute::makeBuffer(vk, size_t(img->width()) * img->height() * texelBytes(img->format()), 0, compute::Host::None));
                img->recordRead(cmd, texels.back());
            }
            compute::copy(cmd, base, verts, bytes);
            compute::memoryBarrier(cmd);
            for (size_t si = 0; si < mesh.displaceRanges.size(); ++si)
                for (size_t k = 0; k < images.size(); ++k)
                {
                    Push p;
                    p.vertices = verts.address;
                    p.base = base.address;
                    p.dirs = dirs.address;
                    p.height = texels[k].address;
                    p.firstVertex = mesh.displaceRanges[si].x;
                    p.count = mesh.displaceRanges[si].y;
                    p.width = uint32_t(images[k].first->width());
                    p.heightPx = uint32_t(images[k].first->height());
                    p.format = uint32_t(images[k].first->format());
                    p.uv1 = params.uv1 ? 1u : 0u;
                    p.tileU = images[k].second.x;
                    p.tileV = images[k].second.y;
                    p.depth = params.depth;
                    p.mid = params.mid;
                    record(cmd, kernel, p);
                }
            compute::memoryBarrier(cmd);
            vkEndCommandBuffer(cmd);
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            vkQueueSubmit(vk.queue, 1, &si, VK_NULL_HANDLE);
            vkQueueWaitIdle(vk.queue);
            compute::invalidate(vk, verts);
            std::memcpy(out.data(), verts.mapped, bytes);
            for (compute::Buffer& b : staging)
                compute::destroy(vk, b);
            for (compute::Buffer& b : texels)
                compute::destroy(vk, b);
            compute::destroy(vk, base);
            compute::destroy(vk, dirs);
            compute::destroy(vk, verts);
            vkDestroyCommandPool(vk.device, pool, nullptr);
            kernel.destroy(vk);
            return true;
        })
        .get();
    return out;
}

} // namespace lila::render::displacement
