#include "render/HeightfieldRenderer.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "render/CameraRelative.h"
#include "render/VulkanContext.h"

#include <glm/gtc/matrix_access.hpp>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace lila::render
{
namespace
{
/** A node of the selection spans two patches a side at its level. */
constexpr int kHfNodeSamples = 2 * kHeightfieldPatch;
constexpr uint32_t kHfNoMap = 0xffffu;

struct HfGpuInstance
{
    glm::mat4 world{1.0f};
    glm::vec4 color{0.0f};
};

/** Matches HfLayer in heightfield.slang. */
struct HfGpuLayer
{
    glm::vec4 color{0.0f};
    glm::vec4 tiling{0.0f};
    uint64_t weights = 0;
    uint32_t baseMap = kHfNoMap;
    uint32_t normalMap = kHfNoMap;
    uint32_t pad[4]{};
};
static_assert(sizeof(HfGpuLayer) == 64, "matches HfLayer in heightfield.slang");

enum : uint32_t
{
    kHfFlagWire = 1,
    kHfFlagArrows = 2,
};

/** Matches HfParams in heightfield.slang. */
struct HfGpuParams
{
    HfGpuInstance instance;
    glm::vec4 grid{0.0f};
    glm::vec4 origin{0.0f};
    glm::vec4 eye{0.0f};
    glm::vec4 extra{0.0f};
    glm::uvec4 info{0u};
    glm::vec4 overlay{0.0f};
    glm::vec4 contour{0.0f};
    glm::vec4 flow{0.0f};
    glm::vec4 lods[kHeightfieldMaxLods]{};
    uint64_t heights = 0;
    uint64_t patches = 0;
    uint64_t overlayMask = 0;
    uint64_t flowMask = 0;
    HfGpuLayer layers[kHeightfieldMaxLayers];
};
static_assert(sizeof(HfGpuParams) % 16 == 0 && offsetof(HfGpuParams, layers) == 624, "matches HfParams in heightfield.slang");
static_assert(sizeof(HeightfieldPatch) == 16, "matches HfPatch in heightfield.slang");

bool validHeightfieldDraw(const HeightfieldDraw& d)
{
    return d.heights && d.nx >= 2 && d.nz >= 2 && d.heights->size() >= size_t(d.nx) * size_t(d.nz) && d.cell > 0.0f;
}

/** The wire a draw shows over (or instead of) its surface, as DrawBatches::wireColor picks it for meshes. */
glm::vec4 heightfieldWireColor(const FrameState& s, const HeightfieldDraw& d)
{
    return (d.flags & DrawSelected)        ? ((d.flags & DrawSelectedOther) ? s.wireSelectedOther : s.wireSelected)
           : (d.flags & DrawPreselect)     ? s.wirePreselect
           : (d.flags & DrawUnderSelected) ? s.wireUnderSelected
           : d.wireColor.a > 0.0f          ? d.wireColor
                                           : s.wireDefault;
}

bool heightfieldShowsWire(const HeightfieldDraw& d)
{
    return (d.flags & (DrawWire | DrawSelected | DrawUnderSelected | DrawPreselect)) != 0;
}

compute::Buffer uploadHeightfieldGrid(VulkanContext& vk, VkCommandBuffer cmd, std::vector<compute::Buffer>& staging, const void* data, size_t bytes)
{
    compute::Buffer dst = compute::makeBuffer(vk, bytes, 0, compute::Host::None);
    compute::Buffer from;
    try
    {
        from = compute::hostBuffer(vk, data, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    }
    catch (...)
    {
        compute::destroy(vk, dst);
        throw;
    }
    compute::copy(cmd, from, dst, bytes);
    staging.push_back(from);
    return dst;
}

/** Values as bytes, four to a word: clamped to 0..1, or stretched from their own range when stretch is set. */
std::vector<uint32_t> packHeightfieldBytes(const std::vector<float>& v, size_t count, bool stretch)
{
    float lo = 0.0f, hi = 1.0f;
    if (stretch)
    {
        using R = std::pair<float, float>;
        const R r = tbb::parallel_reduce(
            tbb::blocked_range<size_t>(0, count), R{FLT_MAX, -FLT_MAX},
            [&](const tbb::blocked_range<size_t>& b, R acc) {
                for (size_t k = b.begin(); k < b.end(); ++k)
                    if (std::isfinite(v[k]))
                        acc = {std::min(acc.first, v[k]), std::max(acc.second, v[k])};
                return acc;
            },
            [](R a, R b) { return R{std::min(a.first, b.first), std::max(a.second, b.second)}; });
        lo = r.first;
        hi = r.second > r.first ? r.second : r.first + 1.0f;
    }
    const float scale = 255.0f / (hi - lo);
    std::vector<uint32_t> words((count + 3) / 4, 0u);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, words.size()), [&](const tbb::blocked_range<size_t>& b) {
        for (size_t w = b.begin(); w < b.end(); ++w)
        {
            uint32_t packed = 0;
            for (size_t k = 0; k < 4 && w * 4 + k < count; ++k)
            {
                const float x = v[w * 4 + k];
                const float q = std::isfinite(x) ? std::clamp((x - lo) * scale, 0.0f, 255.0f) : 0.0f;
                packed |= uint32_t(std::lround(q)) << (k * 8);
            }
            words[w] = packed;
        }
    });
    return words;
}

/** Squared distance from p to the box. */
double heightfieldBoxDistance2(const glm::dvec3& lo, const glm::dvec3& hi, const glm::dvec3& p)
{
    const glm::dvec3 d = glm::max(glm::max(lo - p, p - hi), glm::dvec3(0.0));
    return glm::dot(d, d);
}

struct HfSelector
{
    const HeightfieldDraw& d;
    const HeightPyramid& py;
    glm::dvec3 eye;
    glm::dvec4 planes[5];
    double ranges[kHeightfieldMaxLods];
    size_t maxPatches;
    std::vector<HeightfieldPatch>& out;
    bool full = false;

    void box(int lod, int bx, int bz, glm::dvec3& lo, glm::dvec3& hi) const
    {
        const int size = kHfNodeSamples << lod;
        const int x0 = bx * size, z0 = bz * size;
        const int x1 = std::min(x0 + size, d.nx - 1), z1 = std::min(z0 + size, d.nz - 1);
        const glm::vec2 mm = py.ranges[lod][size_t(bz) * size_t(py.dims[lod].x) + size_t(bx)];
        lo = glm::dvec3(d.origin.x + double(x0) * d.cell, mm.x, d.origin.y + double(z0) * d.cell);
        hi = glm::dvec3(d.origin.x + double(x1) * d.cell, mm.y, d.origin.y + double(z1) * d.cell);
    }

    bool visible(const glm::dvec3& lo, const glm::dvec3& hi) const
    {
        for (const glm::dvec4& p : planes)
        {
            const glm::dvec3 far(p.x >= 0.0 ? hi.x : lo.x, p.y >= 0.0 ? hi.y : lo.y, p.z >= 0.0 ? hi.z : lo.z);
            if (glm::dot(glm::dvec3(p), far) + p.w < 0.0)
                return false;
        }
        return true;
    }

    bool inRange(const glm::dvec3& lo, const glm::dvec3& hi, int lod) const
    {
        const double r = ranges[lod];
        return r >= 0.0 && (r >= DBL_MAX * 0.5 || heightfieldBoxDistance2(lo, hi, eye) <= r * r);
    }

    /** One patch at lod from sample (x, z), unless it starts past the grid. */
    void patch(int lod, int x, int z)
    {
        if (x >= d.nx - 1 || z >= d.nz - 1)
            return;
        if (out.size() >= maxPatches)
        {
            full = true;
            return;
        }
        out.push_back({glm::vec2(float(x), float(z)), float(1 << lod), uint32_t(lod)});
    }

    /** Draws the node's area at lod, all four patches of it or the quarter a child spans. */
    void whole(int lod, int bx, int bz)
    {
        const int size = kHfNodeSamples << lod, half = kHeightfieldPatch << lod;
        for (int q = 0; q < 4; ++q)
            patch(lod, bx * size + (q & 1) * half, bz * size + (q >> 1) * half);
    }

    /** False when the node lies beyond its level's range, for its parent to draw the area instead. */
    bool select(int lod, int bx, int bz)
    {
        glm::dvec3 lo, hi;
        box(lod, bx, bz, lo, hi);
        if (!inRange(lo, hi, lod))
            return false;
        if (!visible(lo, hi))
            return true;
        if (lod == 0 || !inRange(lo, hi, lod - 1))
        {
            whole(lod, bx, bz);
            return true;
        }
        const glm::ivec2 below = py.dims[lod - 1];
        for (int c = 0; c < 4; ++c)
        {
            const int cx = bx * 2 + (c & 1), cz = bz * 2 + (c >> 1);
            if (cx >= below.x || cz >= below.y)
                continue;
            if (select(lod - 1, cx, cz))
                continue;
            glm::dvec3 clo, chi;
            box(lod - 1, cx, cz, clo, chi);
            if (visible(clo, chi))
                patch(lod, cx * (kHfNodeSamples << (lod - 1)), cz * (kHfNodeSamples << (lod - 1)));
        }
        return true;
    }
};
} // namespace

/** The lowest and highest finite sample of level 0's block (bx, bz), which spans its edges' samples. */
static glm::vec2 heightBlockRange(const std::vector<float>& h, int nx, int nz, int bx, int bz)
{
    const int z0 = bz * kHfNodeSamples, z1 = std::min(z0 + kHfNodeSamples, nz - 1);
    const int x0 = bx * kHfNodeSamples, x1 = std::min(x0 + kHfNodeSamples, nx - 1);
    float lo = FLT_MAX, hi = -FLT_MAX;
    for (int z = z0; z <= z1; ++z)
    {
        const float* row = h.data() + size_t(z) * size_t(nx);
        for (int x = x0; x <= x1; ++x)
            if (std::isfinite(row[x]))
            {
                lo = std::min(lo, row[x]);
                hi = std::max(hi, row[x]);
            }
    }
    return lo <= hi ? glm::vec2(lo, hi) : glm::vec2(0.0f);
}

/** Every level above the first, rebuilt from it. */
static void buildUpperLevels(HeightPyramid& p)
{
    p.dims.resize(1);
    p.ranges.resize(1);
    glm::ivec2 dim = p.dims[0];
    while ((dim.x > 1 || dim.y > 1) && p.levels() < kHeightfieldMaxLods)
    {
        const glm::ivec2 up((dim.x + 1) / 2, (dim.y + 1) / 2);
        const std::vector<glm::vec2>& below = p.ranges.back();
        std::vector<glm::vec2> next(size_t(up.x) * size_t(up.y), glm::vec2(FLT_MAX, -FLT_MAX));
        for (int z = 0; z < dim.y; ++z)
            for (int x = 0; x < dim.x; ++x)
            {
                glm::vec2& n = next[size_t(z / 2) * size_t(up.x) + size_t(x / 2)];
                const glm::vec2 b = below[size_t(z) * size_t(dim.x) + size_t(x)];
                n = glm::vec2(std::min(n.x, b.x), std::max(n.y, b.y));
            }
        dim = up;
        p.dims.push_back(dim);
        p.ranges.push_back(std::move(next));
    }
}

HeightPyramid buildHeightPyramid(const std::vector<float>& h, int nx, int nz)
{
    LILA_ZONE();
    HeightPyramid p;
    const glm::ivec2 dim((nx - 2) / kHfNodeSamples + 1, (nz - 2) / kHfNodeSamples + 1);
    std::vector<glm::vec2> level(size_t(dim.x) * size_t(dim.y));
    tbb::parallel_for(0, dim.y, [&](int bz) {
        for (int bx = 0; bx < dim.x; ++bx)
            level[size_t(bz) * size_t(dim.x) + size_t(bx)] = heightBlockRange(h, nx, nz, bx, bz);
    });
    p.dims.push_back(dim);
    p.ranges.push_back(std::move(level));
    buildUpperLevels(p);
    return p;
}

void updateHeightPyramid(HeightPyramid& p, const std::vector<float>& h, int nx, int nz, const std::vector<HeightfieldRect>& rects)
{
    if (p.levels() == 0 || rects.empty())
        return;
    LILA_ZONE();
    const glm::ivec2 dim = p.dims[0];
    for (const HeightfieldRect& r : rects)
    {
        // A block spans the sample on its far edge too, so a change at a block's first sample reaches the block before.
        const int bx0 = std::max((r.x0 - 1) / kHfNodeSamples, 0), bx1 = std::min((r.x1 - 1) / kHfNodeSamples, dim.x - 1);
        const int bz0 = std::max((r.z0 - 1) / kHfNodeSamples, 0), bz1 = std::min((r.z1 - 1) / kHfNodeSamples, dim.y - 1);
        tbb::parallel_for(bz0, bz1 + 1, [&](int bz) {
            for (int bx = bx0; bx <= bx1; ++bx)
                p.ranges[0][size_t(bz) * size_t(dim.x) + size_t(bx)] = heightBlockRange(h, nx, nz, bx, bz);
        });
    }
    buildUpperLevels(p);
}

std::vector<HeightfieldRect> heightfieldDirtyRects(const float* a, const float* b, int nx, int nz)
{
    LILA_ZONE();
    const int tilesX = (nx + kHeightfieldTile - 1) / kHeightfieldTile, tilesZ = (nz + kHeightfieldTile - 1) / kHeightfieldTile;
    std::vector<uint8_t> dirty(size_t(tilesX) * size_t(tilesZ), 0);
    tbb::parallel_for(0, tilesZ, [&](int tz) {
        const int z0 = tz * kHeightfieldTile, z1 = std::min(z0 + kHeightfieldTile, nz);
        for (int tx = 0; tx < tilesX; ++tx)
        {
            const int x0 = tx * kHeightfieldTile, x1 = std::min(x0 + kHeightfieldTile, nx);
            for (int z = z0; z < z1; ++z)
            {
                const size_t at = size_t(z) * size_t(nx) + size_t(x0);
                // Bitwise, so a NaN that stays a NaN is no change.
                if (std::memcmp(a + at, b + at, size_t(x1 - x0) * sizeof(float)) != 0)
                {
                    dirty[size_t(tz) * size_t(tilesX) + size_t(tx)] = 1;
                    break;
                }
            }
        }
    });
    std::vector<HeightfieldRect> out;
    std::vector<size_t> open, next; // rects reaching the previous row of tiles, then this one
    for (int tz = 0; tz < tilesZ; ++tz)
    {
        next.clear();
        const int z0 = tz * kHeightfieldTile, z1 = std::min(z0 + kHeightfieldTile, nz);
        for (int tx = 0; tx < tilesX;)
        {
            if (!dirty[size_t(tz) * size_t(tilesX) + size_t(tx)])
            {
                ++tx;
                continue;
            }
            const int start = tx;
            while (tx < tilesX && dirty[size_t(tz) * size_t(tilesX) + size_t(tx)])
                ++tx;
            const int x0 = start * kHeightfieldTile, x1 = std::min(tx * kHeightfieldTile, nx);
            auto same = std::ranges::find_if(open, [&](size_t i) { return out[i].x0 == x0 && out[i].x1 == x1; });
            if (same != open.end())
            {
                out[*same].z1 = z1;
                next.push_back(*same);
            }
            else
            {
                next.push_back(out.size());
                out.push_back({x0, z0, x1, z1});
            }
        }
        std::swap(open, next);
    }
    return out;
}

std::vector<HeightfieldCopy> heightfieldRectCopies(const std::vector<HeightfieldRect>& rects, int nx)
{
    std::vector<HeightfieldCopy> out;
    size_t packed = 0;
    for (const HeightfieldRect& r : rects)
        for (int z = r.z0; z < r.z1; ++z)
        {
            const size_t dst = size_t(z) * size_t(nx) + size_t(r.x0), count = size_t(r.x1 - r.x0);
            if (!out.empty() && out.back().src + out.back().count == packed && out.back().dst + out.back().count == dst)
                out.back().count += count;
            else
                out.push_back({packed, dst, count});
            packed += count;
        }
    return out;
}

HeightfieldSelection selectHeightfieldPatches(const HeightfieldDraw& d, const HeightPyramid& py, const HeightfieldView& view, double pixelsPerQuad,
                                              size_t maxPatches)
{
    LILA_ZONE();
    HeightfieldSelection sel;
    const int levels = py.levels();
    sel.levels = levels;
    if (levels == 0)
        return sel;
    glm::dmat4 world(d.world);
    world[3] -= glm::dvec4(view.origin * world[3].w, 0.0);
    const glm::dvec3 eye = glm::dvec3(glm::inverse(world) * glm::dvec4(view.eye, 1.0));
    // Local lengths: the patches' screen size doesn't depend on a uniform placement scale.
    const double scale = std::max(1e-9, std::sqrt(glm::length(glm::dvec3(world[0])) * glm::length(glm::dvec3(world[2]))));
    const double cell = d.cell;

    HfSelector s{d, py, eye, {}, {}, maxPatches, sel.patches};
    const glm::dmat4 m = view.cull.value_or(view.viewProj) * world;
    const glm::dvec4 r0 = glm::row(m, 0), r1 = glm::row(m, 1), r2 = glm::row(m, 2), r3 = glm::row(m, 3);
    const glm::dvec4 planes[5] = {r3 + r0, r3 - r0, r3 + r1, r3 - r1, r3 - r2};
    for (int k = 0; k < 5; ++k)
    {
        const double len = std::max(glm::length(glm::dvec3(planes[k])), 1e-30);
        // A couple of cells of slack: jitter and the morph move the surface a little within a node.
        s.planes[k] = planes[k] / len + glm::dvec4(0.0, 0.0, 0.0, 2.0 * cell);
    }

    if (view.ortho)
    {
        const double pixelLocal = view.pixelWorld / scale;
        const int flat = std::clamp(int(std::floor(std::log2(std::max(1.0, pixelsPerQuad * pixelLocal / cell)))), 0, levels - 1);
        for (int k = 0; k < levels; ++k)
        {
            s.ranges[k] = k < flat ? -1.0 : DBL_MAX;
            sel.morph[k] = glm::vec2(FLT_MAX, 0.0f);
        }
    }
    else
    {
        double prev = 0.0;
        for (int k = 0; k < levels; ++k)
        {
            if (k == levels - 1)
            {
                s.ranges[k] = DBL_MAX;
                sel.morph[k] = glm::vec2(FLT_MAX, 0.0f);
                break;
            }
            // Level k's quads, cell << k across, stay under pixelsPerQuad pixels out to here; a node never borders one
            // more than a level away, as each range spans well over a node.
            const double nodeDiagonal = std::sqrt(2.0) * double(kHfNodeSamples << k) * cell;
            const double r = std::max({cell * double(2 << k) * view.focalPx / pixelsPerQuad, 2.0 * nodeDiagonal, prev * 2.0});
            s.ranges[k] = r;
            const double start = prev + (r - prev) * 0.7;
            sel.morph[k] = glm::vec2(float(start), float(1.0 / std::max(r - start, 1e-6)));
            prev = r;
        }
    }

    const int top = levels - 1;
    for (int bz = 0; bz < py.dims[top].y; ++bz)
        for (int bx = 0; bx < py.dims[top].x; ++bx)
            s.select(top, bx, bz);
    if (s.full)
        log::warn("render: a heightfield reached {} patches; the rest of it is not drawn this frame", maxPatches);
    return sel;
}

HeightfieldRenderer::Grid* HeightfieldRenderer::heights(VulkanContext& vk, VkCommandBuffer cmd, FrameData& f, const HeightfieldDraw& d, uint64_t frame)
{
    const void* key = d.heights.get();
    auto it = mHeights.find(key);
    if (it != mHeights.end())
    {
        it->second.lastUsed = frame;
        return &it->second;
    }
    // New heights for a node whose last ones are still on the GPU at the same size: an edit, so only what changed goes up.
    // Not when a draw this frame still reads the last ones.
    if (d.node)
        if (auto last = mNodeHeights.find(d.node); last != mNodeHeights.end())
            if (auto old = mHeights.find(last->second);
                old != mHeights.end() && old->second.nx == d.nx && old->second.nz == d.nz && old->second.lastUsed != frame)
            {
                patchHeights(vk, cmd, f, old->second, d);
                auto moved = mHeights.extract(old);
                moved.key() = key;
                Grid& g = mHeights.insert(std::move(moved)).position->second;
                g.keep = d.heights;
                g.lastUsed = frame;
                last->second = key;
                return &g;
            }
    LILA_ZONE_N("Upload heightfield heights");
    Grid g;
    g.pyramid = buildHeightPyramid(*d.heights, d.nx, d.nz);
    g.buffer = uploadHeightfieldGrid(vk, cmd, f.staging, d.heights->data(), size_t(d.nx) * size_t(d.nz) * sizeof(float));
    g.keep = d.heights;
    g.lastUsed = frame;
    g.nx = d.nx;
    g.nz = d.nz;
    if (d.node)
        mNodeHeights[d.node] = key;
    return &mHeights.emplace(key, std::move(g)).first->second;
}

void HeightfieldRenderer::patchHeights(VulkanContext& vk, VkCommandBuffer cmd, FrameData& f, Grid& g, const HeightfieldDraw& d)
{
    LILA_ZONE_N("Patch heightfield heights");
    const std::vector<float>& now = *d.heights;
    const std::vector<HeightfieldRect> rects = heightfieldDirtyRects(g.keep->data(), now.data(), d.nx, d.nz);
    if (rects.empty())
        return;
    const std::vector<HeightfieldCopy> copies = heightfieldRectCopies(rects, d.nx);
    const size_t floats = copies.back().src + copies.back().count;
    compute::Buffer from = compute::makeBuffer(vk, floats * sizeof(float), 0, compute::Host::Write);
    f.staging.push_back(from);
    std::vector<VkBufferCopy> regions;
    regions.reserve(copies.size());
    auto* out = static_cast<float*>(from.mapped);
    for (const HeightfieldCopy& c : copies)
    {
        std::memcpy(out + c.src, now.data() + c.dst, c.count * sizeof(float));
        regions.push_back({c.src * sizeof(float), c.dst * sizeof(float), c.count * sizeof(float)});
    }
    vmaFlushAllocation(vk.allocator, from.alloc, 0, VK_WHOLE_SIZE);
    // Earlier frames' draws may still read the grid: they finish before the copy overwrites it.
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    mb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
    vkCmdCopyBuffer(cmd, from.buffer, g.buffer.buffer, uint32_t(regions.size()), regions.data());
    updateHeightPyramid(g.pyramid, now, d.nx, d.nz, rects);
}

VkDeviceAddress HeightfieldRenderer::mask(VulkanContext& vk, VkCommandBuffer cmd, FrameData& f, const GridValues& values, size_t count, bool stretch,
                                          uint64_t frame)
{
    if (!values || values->size() < count)
        return 0;
    const auto key = std::make_pair(static_cast<const void*>(values.get()), stretch);
    auto it = mMasks.find(key);
    if (it != mMasks.end())
    {
        it->second.lastUsed = frame;
        return it->second.buffer.address;
    }
    LILA_ZONE_N("Upload heightfield mask");
    const std::vector<uint32_t> words = packHeightfieldBytes(*values, count, stretch);
    Grid g;
    g.buffer = uploadHeightfieldGrid(vk, cmd, f.staging, words.data(), words.size() * sizeof(uint32_t));
    g.keep = values;
    g.lastUsed = frame;
    return mMasks.emplace(key, std::move(g)).first->second.buffer.address;
}

void HeightfieldRenderer::prepare(VulkanContext& vk, VkCommandBuffer cmd, const void* frameKey, const FrameState& s, uint32_t heightPx, uint64_t frame,
                                  const MapHandle& handle, const std::vector<glm::mat4>& shadowViews)
{
    mPrepared.clear();
    mShadows.assign(shadowViews.size(), {});
    mPatches = 0;
    if (s.heightfields.empty())
        return;
    LILA_ZONE();
    FrameData& f = mFrames[frameKey];
    f.lastUsed = frame;
    // This frame's last submission has finished, so its staging copies are done.
    for (compute::Buffer& b : f.staging)
        compute::destroy(vk, b);
    f.staging.clear();

    HeightfieldView view;
    view.viewProj = s.proj * s.view;
    view.eye = s.eye;
    view.origin = s.renderOrigin;
    view.ortho = s.proj[3][3] == 1.0;
    const double py = std::abs(s.proj[1][1]);
    view.focalPx = py * double(std::max(heightPx, 1u)) * 0.5;
    view.pixelWorld = 2.0 / std::max(py * double(std::max(heightPx, 1u)), 1e-12);

    std::vector<HfGpuParams> params;
    std::vector<HeightfieldPatch> patches;
    std::vector<size_t> firstPatch;
    for (const HeightfieldDraw& d : s.heightfields)
    {
        if (!validHeightfieldDraw(d) || !(d.flags & (DrawShaded | DrawWire | DrawSelected | DrawUnderSelected | DrawPreselect)))
            continue;
        const size_t count = size_t(d.nx) * size_t(d.nz);
        HfGpuParams p{};
        HeightfieldSelection sel;
        std::vector<HeightfieldSelection> shadowSel(d.flags & DrawShaded ? shadowViews.size() : 0);
        try
        {
            const Grid* g = heights(vk, cmd, f, d, frame);
            p.heights = g->buffer.address;
            const double quadPixels = std::clamp(double(d.quadPixels), 1.0, 64.0);
            sel = selectHeightfieldPatches(d, g->pyramid, view, quadPixels);
            for (size_t v = 0; v < shadowSel.size(); ++v)
            {
                HeightfieldView shadowView = view;
                shadowView.cull = glm::dmat4(shadowViews[v]);
                shadowSel[v] = selectHeightfieldPatches(d, g->pyramid, shadowView, quadPixels);
            }
            if (sel.patches.empty() && std::ranges::all_of(shadowSel, [](const HeightfieldSelection& h) { return h.patches.empty(); }))
                continue;
            p.overlayMask = d.overlayOpacity > 0.0f ? mask(vk, cmd, f, d.overlay, count, true, frame) : 0;
            p.flowMask = d.flowArrows ? mask(vk, cmd, f, d.flow, count, false, frame) : 0;
            uint32_t layers = 0;
            bool skippedMaps = false;
            for (const HeightfieldLayer& l : d.layers)
            {
                if (layers == uint32_t(kHeightfieldMaxLayers))
                    break;
                HfGpuLayer& gl = p.layers[layers];
                gl.weights = mask(vk, cmd, f, l.weights, count, false, frame);
                if (!gl.weights)
                    continue;
                gl.color = glm::vec4(l.color, l.roughness);
                gl.tiling = glm::vec4(1.0f / std::max(l.tiling, 1e-3f), 0.0f, 0.0f, 0.0f);
                if (bindless)
                {
                    gl.baseMap = l.baseColorMap ? handle(l.baseColorMap, false) : kHfNoMap;
                    gl.normalMap = l.normalMap ? handle(l.normalMap, true) : kHfNoMap;
                }
                else
                    skippedMaps = skippedMaps || l.baseColorMap || l.normalMap;
                ++layers;
            }
            if (skippedMaps && !mWarnedMaps)
            {
                log::warn("render: this GPU has no bindless textures, so terrain paint layers draw their colours without their maps");
                mWarnedMaps = true;
            }
            p.info.y = layers;
        }
        catch (const std::exception& e)
        {
            log::error("render: heightfield {}x{} not drawn: {}", d.nx, d.nz, e.what());
            continue;
        }
        p.instance.world = relativeTo(d.world, s.renderOrigin);
        p.instance.color = heightfieldShowsWire(d) ? heightfieldWireColor(s, d) : glm::vec4(0.0f);
        p.grid = glm::vec4(float(d.nx), float(d.nz), d.cell, std::max(d.wireWidth, 1.0f));
        p.origin = glm::vec4(d.origin, 1.0f / float(d.nx - 1), 1.0f / float(d.nz - 1));
        p.eye = glm::vec4(glm::vec3(glm::inverse(d.world) * glm::dvec4(s.eye + s.renderOrigin, 1.0)), 0.0f);
        p.info.x = (heightfieldShowsWire(d) ? kHfFlagWire : 0u) | (d.flowArrows ? kHfFlagArrows : 0u);
        p.info.z = uint32_t(std::max(d.overlayRamp, 0));
        p.info.w = uint32_t(std::max(d.contourMajor, 1));
        p.overlay = glm::vec4(p.overlayMask ? std::clamp(d.overlayOpacity, 0.0f, 1.0f) : 0.0f, 0.0f, 0.0f, 0.0f);
        p.contour = glm::vec4(std::max(d.contourInterval, 0.0f), 0.85f, 0.0f, 0.0f);
        p.flow = glm::vec4(std::max(d.arrowSpacing, 8.0f), p.flowMask ? 1.0f : 0.0f, 0.9f, 0.0f);
        for (int k = 0; k < sel.levels && k < kHeightfieldMaxLods; ++k)
            p.lods[k] = glm::vec4(sel.morph[k], 0.0f, 0.0f);
        // A copy of the parameters per view that sees patches of it: only the patches differ.
        auto add = [&](const HeightfieldSelection& from, std::vector<Prepared>& into) {
            into.push_back({&d, 0, uint32_t(from.patches.size()), params.size()});
            if (from.patches.empty())
                return;
            firstPatch.push_back(patches.size());
            patches.insert(patches.end(), from.patches.begin(), from.patches.end());
            params.push_back(p);
        };
        add(sel, mPrepared);
        for (size_t v = 0; v < mShadows.size(); ++v)
            add(v < shadowSel.size() ? shadowSel[v] : HeightfieldSelection{}, mShadows[v]);
    }
    if (!f.staging.empty())
        compute::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    if (params.empty())
        return;
    if (!mIndices.buffer)
    {
        constexpr uint32_t row = kHeightfieldPatch + 1;
        std::vector<uint32_t> indices;
        indices.reserve(size_t(kHeightfieldPatch) * kHeightfieldPatch * 6);
        for (uint32_t z = 0; z < uint32_t(kHeightfieldPatch); ++z)
            for (uint32_t x = 0; x < uint32_t(kHeightfieldPatch); ++x)
            {
                const uint32_t a = z * row + x, b = a + row, c = b + 1, d = a + 1;
                indices.insert(indices.end(), {a, b, c, a, c, d});
            }
        try
        {
            mIndices = compute::hostBuffer(vk, indices.data(), indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        }
        catch (const std::exception& e)
        {
            log::error("render: heightfields not drawn: {}", e.what());
            mPrepared.clear();
            mShadows.assign(mShadows.size(), {});
            return;
        }
    }

    const size_t paramBytes = params.size() * sizeof(HfGpuParams);
    const size_t bytes = paramBytes + patches.size() * sizeof(HeightfieldPatch);
    try
    {
        if (f.host.size < bytes)
        {
            compute::destroy(vk, f.host);
            f.host = compute::makeBuffer(vk, std::max<size_t>(bytes * 3 / 2, 64 * 1024), 0, compute::Host::Write);
        }
    }
    catch (const std::exception& e)
    {
        log::error("render: heightfields not drawn: {}", e.what());
        mPrepared.clear();
        mShadows.assign(mShadows.size(), {});
        return;
    }
    for (size_t i = 0; i < params.size(); ++i)
        params[i].patches = f.host.address + paramBytes + firstPatch[i] * sizeof(HeightfieldPatch);
    auto place = [&](std::vector<Prepared>& list) {
        for (Prepared& r : list)
            r.params = r.patches ? f.host.address + r.slot * sizeof(HfGpuParams) : 0;
    };
    place(mPrepared);
    for (std::vector<Prepared>& list : mShadows)
        place(list);
    auto* out = static_cast<uint8_t*>(f.host.mapped);
    std::memcpy(out, params.data(), paramBytes);
    std::memcpy(out + paramBytes, patches.data(), patches.size() * sizeof(HeightfieldPatch));
    vmaFlushAllocation(vk.allocator, f.host.alloc, 0, bytes);
    for (const Prepared& r : mPrepared)
        mPatches += r.patches;
    LILA_PLOT("Heightfield patches", int64_t(mPatches));
}

void HeightfieldRenderer::record(VkCommandBuffer cmd, VkPipelineLayout layout, size_t i, MeshPush p, const glm::mat4& viewProj, int shadowView) const
{
    const Prepared& r = shadowView < 0 ? mPrepared[i] : mShadows[size_t(shadowView)][i];
    if (r.patches == 0)
        return;
    p.viewProj = viewProj;
    p.instances = r.params;
    p.firstInstance = 0;
    p.edgeMask = r.params;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
    vkCmdBindIndexBuffer(cmd, mIndices.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, uint32_t(kHeightfieldPatch * kHeightfieldPatch * 6), r.patches, 0, 0, 0);
}

glm::vec2 HeightfieldRenderer::heightRange(const HeightfieldDraw& d) const
{
    if (!validHeightfieldDraw(d))
        return glm::vec2(0.0f);
    if (auto it = mHeights.find(d.heights.get()); it != mHeights.end() && it->second.pyramid.levels() > 0)
        return it->second.pyramid.ranges.back().front();
    const std::vector<float>& h = *d.heights;
    using R = std::pair<float, float>;
    const R r = tbb::parallel_reduce(
        tbb::blocked_range<size_t>(0, size_t(d.nx) * size_t(d.nz)), R{FLT_MAX, -FLT_MAX},
        [&](const tbb::blocked_range<size_t>& b, R acc) {
            for (size_t k = b.begin(); k < b.end(); ++k)
                if (std::isfinite(h[k]))
                    acc = {std::min(acc.first, h[k]), std::max(acc.second, h[k])};
            return acc;
        },
        [](R a, R b) { return R{std::min(a.first, b.first), std::max(a.second, b.second)}; });
    return r.first <= r.second ? glm::vec2(r.first, r.second) : glm::vec2(0.0f);
}

void HeightfieldRenderer::evict(VulkanContext& vk, uint64_t frame)
{
    auto stale = [&](const Grid& g) { return g.lastUsed + 240 < frame || (g.keep.use_count() == 1 && g.lastUsed != frame); };
    bool any = false;
    for (const auto& [key, f] : mFrames)
        any = any || f.lastUsed + 240 < frame;
    for (const auto& [key, g] : mHeights)
        any = any || stale(g);
    for (const auto& [key, g] : mMasks)
        any = any || stale(g);
    if (!any)
        return;
    LILA_ZONE();
    vkDeviceWaitIdle(vk.device);
    std::erase_if(mHeights, [&](auto& e) {
        if (!stale(e.second))
            return false;
        compute::destroy(vk, e.second.buffer);
        return true;
    });
    std::erase_if(mNodeHeights, [&](const auto& e) { return !mHeights.contains(e.second); });
    std::erase_if(mMasks, [&](auto& e) {
        if (!stale(e.second))
            return false;
        compute::destroy(vk, e.second.buffer);
        return true;
    });
    // Frames that stopped drawing heightfields (a closed view, a finished capture) keep nothing.
    std::erase_if(mFrames, [&](auto& e) {
        if (e.second.lastUsed + 240 >= frame)
            return false;
        compute::destroy(vk, e.second.host);
        for (compute::Buffer& b : e.second.staging)
            compute::destroy(vk, b);
        return true;
    });
}

void HeightfieldRenderer::destroy(VulkanContext& vk)
{
    compute::destroy(vk, mIndices);
    for (auto& [key, g] : mHeights)
        compute::destroy(vk, g.buffer);
    for (auto& [key, g] : mMasks)
        compute::destroy(vk, g.buffer);
    for (auto& [key, f] : mFrames)
    {
        compute::destroy(vk, f.host);
        for (compute::Buffer& b : f.staging)
            compute::destroy(vk, b);
    }
    mHeights.clear();
    mNodeHeights.clear();
    mMasks.clear();
    mFrames.clear();
    mPrepared.clear();
    mShadows.clear();
}
} // namespace lila::render
