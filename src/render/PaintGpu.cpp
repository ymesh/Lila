#include "render/PaintGpu.h"

#include "brush/Alphas.h"
#include "brush/Brush.h"
#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "ops/Undo.h"
#include "render/Compute.h"
#include "render/Renderer.h"
#include "scene/MeshNodes.h"
#include "scene/Node.h"
#include "sculpt/SculptSession.h"
#include "tex/Evaluate.h"
#include "tex/Library.h"
#include "tex/NodeDef.h"
#include "tex/PaintSymmetry.h"
#include "tex/Plan.h"
#include "tex/TextureSet.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

namespace lila::render::paint
{
namespace
{
using namespace tex::paint;
using scene::NodeId;

constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr VkDeviceSize kTexels = VkDeviceSize(kPageSize) * kPageSize;
/** A channel page: RGBA16F texels. */
constexpr VkDeviceSize kHalfPage = kTexels * 8;
/** A geometry page (GeoTexel) or a stroke's build-up page (float4). */
constexpr VkDeviceSize kWidePage = kTexels * 16;
constexpr uint32_t kMaxChannels = 24;
constexpr uint32_t kItemsPerDispatch = 4096;
constexpr uint32_t kDabsPerBatch = 512;
constexpr int kMaxDepthSize = 1024;
constexpr VkShaderStageFlags kCompute = VK_SHADER_STAGE_COMPUTE_BIT;

// ------------------------------------------------------------------------------------------- GPU-side structures

struct GpuTri
{
    glm::vec4 p[3];
    glm::vec4 n[3];
    glm::vec4 t[3];
};
static_assert(sizeof(GpuTri) == 144);

struct GpuDab
{
    glm::vec4 centerRadius;
    glm::vec4 normalAmount;
    glm::vec4 tangentRoundness;
    glm::vec4 direction;
};
static_assert(sizeof(GpuDab) == 64);

struct GpuItem
{
    VkDeviceAddress geo = 0;
    VkDeviceAddress acc = 0;
    uint32_t dabFirst = 0;
    uint32_t dabCount = 0;
    uint32_t page = 0;
    uint32_t tile = 0;
    VkDeviceAddress dst[kMaxChannels]{};
    VkDeviceAddress src[kMaxChannels]{};
};
static_assert(sizeof(GpuItem) == 32 + 16 * kMaxChannels);

/** A UDIM tile as the kernels see it: size and per channel a table of its pages before the stroke. */
struct GpuTile
{
    uint32_t width = 0, height = 0, pagesX = 0, pagesY = 0;
    VkDeviceAddress tables[kMaxChannels]{};
};
static_assert(sizeof(GpuTile) == 16 + 8 * kMaxChannels);

struct GpuParams
{
    glm::mat4 objectToClip{1.0f};
    glm::mat4 clipToObject{1.0f};
    glm::vec4 eye{0.0f};
    glm::vec4 viewDir{0.0f};
    glm::vec4 values[kMaxChannels]{};
    VkDeviceAddress items = 0, dabIndex = 0, dabs = 0, tris = 0, falloff = 0, alpha = 0, depth = 0, tiles = 0, triMask = 0, image = 0, stamp = 0;
    VkDeviceAddress valueImages[kMaxChannels]{};
    uint32_t depthW = 1, depthH = 1, alphaW = 0, alphaH = 0;
    uint32_t channels = 0, blend = 0, flags = 0, directionChannel = kNone;
    uint32_t maskChannel = kNone, itemCount = 0, mode = 0, sub = 0;
    uint32_t normalMask = 0, imageW = 0, imageH = 0, stampW = 0;
    uint32_t stampH = 0, valueW = 0, valueH = 0, colorChannel = kNone;
    float strength = 1.0f, tilt = 0.5f, depthBias = 0.0f, filterRadius = 4.0f;
    float amount = 1.0f, sourceU = 0.0f, sourceV = 0.0f, facingFade = 0.0f;
    float intensity = 1.0f, valueScale = 1.0f;
    /** The stroke's stencil: its picture's size, placement (view pixels), kStencil* flags and the view's size. */
    uint32_t stencilW = 0, stencilH = 0;
    float stencilX = 0.0f, stencilY = 0.0f, stencilScale = 1.0f, stencilAngle = 0.0f;
    uint32_t stencilFlags = 0, viewW = 1, viewH = 1, stencilColor = kNone;
    VkDeviceAddress stencil = 0;
    /** The view the stencil is pinned to (a projection's symmetry copies project through other views). */
    glm::mat4 stencilToClip{1.0f};
    /** Locked triangles (a bit each), and the limit mask's texels (RGBA, its alpha read) with its size. */
    VkDeviceAddress locked = 0, limit = 0;
    uint32_t limitW = 0, limitH = 0;
    /** A gradient fill: its type, space (0 uv, 1 screen), dither, ends, and per channel a ramp of kRamp rgba entries (value, amount). */
    uint32_t gradType = 0, gradSpace = 0, gradDither = 0, gradOn = 0;
    float gradA[2]{}, gradB[2]{};
    VkDeviceAddress ramp = 0;
    glm::mat4 gradToClip{1.0f};
    /**
     * Varied tiling of the source images (hex tiling): on, the channel whose value steers the blend (kNone: the cell
     * weights alone), how far each cell turns (1 is up to a half turn), the blend's contrast and a per-cell brightness spread.
     */
    uint32_t hexOn = 0, hexWeight = kNone;
    float hexRotate = 0.0f, hexContrast = 0.5f, hexJitter = 0.0f;
    uint32_t hexColor = kNone;
    /** Where the source images map: 0 the uvs (valueScale tiles a uv unit), 1 object space, triplanar, valueSize cm a tile. */
    uint32_t valueSpace = 0;
    float valueSize = 100.0f;
};
static_assert(offsetof(GpuParams, items) == 160 + 16 * kMaxChannels);
static_assert(offsetof(GpuParams, valueImages) == 248 + 16 * kMaxChannels);
static_assert(offsetof(GpuParams, depthW) == 248 + 24 * kMaxChannels);
static_assert(offsetof(GpuParams, strength) == 328 + 24 * kMaxChannels);
static_assert(offsetof(GpuParams, stencilW) == 368 + 24 * kMaxChannels);
static_assert(offsetof(GpuParams, stencil) == 408 + 24 * kMaxChannels);
static_assert(offsetof(GpuParams, locked) == 480 + 24 * kMaxChannels);
static_assert(offsetof(GpuParams, ramp) == 536 + 24 * kMaxChannels);
static_assert(offsetof(GpuParams, hexOn) == 608 + 24 * kMaxChannels);
static_assert(sizeof(GpuParams) == 640 + 24 * kMaxChannels);
constexpr int kRamp = 256;

struct DabPush
{
    VkDeviceAddress params;
    uint32_t firstItem;
    uint32_t pad;
};

struct DepthPush
{
    glm::mat4 objectToClip;
    VkDeviceAddress tris;
    VkDeviceAddress depth;
    uint32_t triCount, width, height, groupsX;
};
static_assert(sizeof(DepthPush) == 96);

constexpr uint32_t kFlagOcclusion = 1, kFlagDirection = 2, kFlagView = 4, kFlagValueImages = 8, kFlagFlow = 16;
constexpr uint32_t kStencilOn = 1, kStencilInvert = 2, kStencilTile = 4, kStencilColor = 8;
enum Mode : uint32_t
{
    kModePaint,
    kModeFilter,
    kModeFill,
    kModeProject,
    kModeNormal,
};

struct VisPush
{
    glm::mat4 objectToClip;
    VkDeviceAddress tris, depth, vis;
    uint32_t triCount, width, height, groupsX, pad0;
};

struct UvPush
{
    glm::mat4 objectToClip;
    VkDeviceAddress tris, vis, out;
    uint32_t width, height, groupsX, pad0;
};

struct GatherPush
{
    VkDeviceAddress table, dst;
    int32_t x0, y0;
    uint32_t width, height, tileW, tileH, levelW, levelH, pagesX, dstStride, groupsX, pad;
};
static_assert(sizeof(GatherPush) == 64);

/** paint_particles.slang ParticlePush. */
struct ParticlePush
{
    glm::vec4 down;
    VkDeviceAddress nodes, corners, tris, paintable, dabs, out, counts;
    uint32_t threads, perDab, firstIndex, seed, maxOut, triCount, groupsX, kind, emitter, pad;
    VkDeviceAddress spec;
};
static_assert(sizeof(ParticlePush) == 120);

/** paint_particles.slang Spec: the particle settings the walk reads. */
struct ParticleSpecGpu
{
    float length, size, spread, rate, gravity, friction, wander, pool, fade;
};

/** paint_particles.slang InDab and OutDab. */
struct ParticleInDab
{
    uint32_t triangle;
    float u, v, radius, opacity;
    uint32_t pad[3];
};
struct ParticleOutDab
{
    uint32_t triangle;
    float u, v, radius, opacity;
    uint32_t pad;
};

struct ViewPush
{
    VkDeviceAddress src, dst;
    glm::vec4 rect;
    uint32_t srcWidth, srcHeight, format, srgb, width, height, groupsX, display;
};

// ---------------------------------------------------------------------------------------------------- page pools

/** Equal-sized pages carved from chunk buffers, reused through a free list. Render thread. */
class Pool
{
public:
    Pool(VkDeviceSize pageBytes, uint32_t perChunk) : mPage(pageBytes), mPerChunk(perChunk) {}

    uint32_t alloc(VulkanContext& vk)
    {
        if (mFree.empty())
        {
            compute::Buffer b = compute::makeBuffer(vk, mPage * mPerChunk, 0, compute::Host::None);
            const uint32_t base = uint32_t(mChunks.size()) * mPerChunk;
            mChunks.push_back(b);
            for (uint32_t i = mPerChunk; i-- > 0;)
                mFree.push_back(base + i);
        }
        const uint32_t s = mFree.back();
        mFree.pop_back();
        ++mUsed;
        return s;
    }
    void release(uint32_t slot)
    {
        if (slot == kNone)
            return;
        mFree.push_back(slot);
        --mUsed;
    }
    VkBuffer buffer(uint32_t slot) const { return mChunks[slot / mPerChunk].buffer; }
    VkDeviceSize offset(uint32_t slot) const { return VkDeviceSize(slot % mPerChunk) * mPage; }
    VkDeviceAddress address(uint32_t slot) const { return mChunks[slot / mPerChunk].address + offset(slot); }
    VkDeviceSize pageBytes() const { return mPage; }
    size_t used() const { return mUsed; }
    void destroy(VulkanContext& vk)
    {
        for (compute::Buffer& b : mChunks)
            compute::destroy(vk, b);
        mChunks.clear();
        mFree.clear();
        mUsed = 0;
    }

private:
    VkDeviceSize mPage;
    uint32_t mPerChunk;
    std::vector<compute::Buffer> mChunks;
    std::vector<uint32_t> mFree;
    size_t mUsed = 0;
};

/** A channel tile's page table on the device for gathers, rebuilt whenever its pages change. Render thread. */
struct ViewEntry
{
    compute::Buffer table;
    int width = 0, height = 0;
};
std::map<std::tuple<NodeId, std::string, std::string, int>, ViewEntry> gViews; // set, buffer, channel, udim
/** Tile sizes of every buffer, for views of channels nothing painted yet. Render thread. */
std::map<std::tuple<NodeId, std::string, int>, std::pair<int, int>> gViewSizes;

struct Kernel
{
    std::vector<uint32_t> code;
    uint64_t hash = 0;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    uint32_t push = 0;
};

/** Mesh triangles on the device, for painting and depth. */
struct TriBuffer
{
    std::shared_ptr<const PaintMesh> mesh;
    compute::Buffer buffer;
};

/** A paint mesh's BVH on the device (geo::FlatBvh) and which triangles are paintable, for surface walks. */
struct BvhBuffer
{
    std::shared_ptr<const PaintMesh> mesh;
    compute::Buffer nodes, corners, paintable;
};

/** Render-thread state shared by every buffer. */
struct Gpu
{
    Pool half{kHalfPage, 32};
    Pool wide{kWidePage, 16};
    uint32_t zeroPage = kNone;
    compute::Submitter sub;
    /** Live stroke batches: launched by the render thread, waited on by the paint worker. */
    compute::Submitter batches;
    bool ready = false;
    Kernel dab, depth, vis, uvmap, view, gather, particles;
    std::map<const PaintMesh*, TriBuffer> tris;
    std::map<const PaintMesh*, BvhBuffer> bvhs;

    void init(VulkanContext& vk)
    {
        if (ready)
            return;
        sub.init(vk);
        batches.init(vk);
        loadKernel(vk, dab, "paint_dab.spv", sizeof(DabPush));
        loadKernel(vk, depth, "paint_depth.spv", sizeof(DepthPush));
        loadKernel(vk, vis, "paint_screen.spv", sizeof(VisPush));
        loadKernel(vk, uvmap, "paint_screen.spv", sizeof(UvPush));
        loadKernel(vk, view, "paint_screen.spv", sizeof(ViewPush));
        loadKernel(vk, gather, "paint_gather.spv", sizeof(GatherPush));
        loadKernel(vk, particles, "paint_particles.spv", sizeof(ParticlePush));
        zeroPage = half.alloc(vk);
        sub.run(vk, [&](VkCommandBuffer cmd) { vkCmdFillBuffer(cmd, half.buffer(zeroPage), half.offset(zeroPage), kHalfPage, 0); });
        ready = true;
    }

    static void loadKernel(VulkanContext& vk, Kernel& k, const char* file, uint32_t push)
    {
        if (k.layout)
            return;
        std::string err;
        k.code = compute::loadSpirv(file, &err);
        if (k.code.empty())
            throw std::runtime_error("texture painting: " + err);
        k.hash = compute::spirvHash(k.code);
        k.push = push;
        k.layout = compute::pipelineLayout(vk, push, kCompute);
    }

    VkPipeline pipeline(VulkanContext& vk, const Kernel& k, const char* entry)
    {
        return compute::sharedPipelines().get(vk, k.layout, compute::layoutKey(k.push, kCompute), k.code, k.hash, entry);
    }

    const TriBuffer& triangles(VulkanContext& vk, const std::shared_ptr<const PaintMesh>& mesh)
    {
        if (auto it = tris.find(mesh.get()); it != tris.end())
            return it->second;
        std::vector<GpuTri> t(std::max<size_t>(mesh->triangleCount(), 1));
        for (size_t i = 0; i < mesh->triangleCount(); ++i)
            for (size_t k = 0; k < 3; ++k)
            {
                // Corner uv rides in the w components.
                t[i].p[k] = glm::vec4(mesh->positions[i * 3 + k], mesh->uvs[i * 3 + k].x);
                t[i].n[k] = glm::vec4(mesh->normals[i * 3 + k], mesh->uvs[i * 3 + k].y);
                t[i].t[k] = mesh->tangents[i * 3 + k];
            }
        TriBuffer tb{mesh, compute::makeBuffer(vk, t.size() * sizeof(GpuTri), 0, compute::Host::None)};
        compute::Buffer staging = compute::hostBuffer(vk, t.data(), t.size() * sizeof(GpuTri));
        sub.run(vk, [&](VkCommandBuffer cmd) { compute::copy(cmd, staging, tb.buffer, t.size() * sizeof(GpuTri)); });
        compute::destroy(vk, staging);
        return tris.emplace(mesh.get(), std::move(tb)).first->second;
    }

    const BvhBuffer& bvh(VulkanContext& vk, const std::shared_ptr<const PaintMesh>& mesh)
    {
        if (auto it = bvhs.find(mesh.get()); it != bvhs.end())
            return it->second;
        const geo::FlatBvh flat = mesh->bvh->exportFlat();
        std::vector<uint32_t> paint(std::max<size_t>(mesh->triangleCount(), 1), 0);
        for (size_t i = 0; i < mesh->triangleCount(); ++i)
            paint[i] = mesh->paintable[i] ? 1u : 0u;
        auto upload = [&](const void* data, size_t bytes) {
            compute::Buffer b = compute::makeBuffer(vk, std::max<size_t>(bytes, 16), 0, compute::Host::None);
            if (bytes > 0)
            {
                compute::Buffer staging = compute::hostBuffer(vk, data, bytes);
                sub.run(vk, [&](VkCommandBuffer cmd) { compute::copy(cmd, staging, b, bytes); });
                compute::destroy(vk, staging);
            }
            return b;
        };
        BvhBuffer bb{mesh, upload(flat.nodes.data(), flat.nodes.size() * sizeof(geo::FlatBvh::Node)),
                     upload(flat.corners.data(), flat.corners.size() * sizeof(glm::vec4)), upload(paint.data(), paint.size() * 4)};
        return bvhs.emplace(mesh.get(), std::move(bb)).first->second;
    }

    void release(VulkanContext& vk)
    {
        if (!ready)
            return;
        for (auto& [k, t] : tris)
            compute::destroy(vk, t.buffer);
        tris.clear();
        for (auto& [k, b] : bvhs)
            for (compute::Buffer* x : {&b.nodes, &b.corners, &b.paintable})
                compute::destroy(vk, *x);
        bvhs.clear();
        for (auto& [k, v] : gViews)
            compute::destroy(vk, v.table);
        gViews.clear();
        gViewSizes.clear();
        half.destroy(vk);
        wide.destroy(vk);
        zeroPage = kNone;
        sub.destroy(vk);
        batches.destroy(vk);
        ready = false;
    }
};

// Leaked on purpose: undo steps holding pages may outlive static destruction order.
Gpu& gpu()
{
    static Gpu* g = new Gpu;
    return *g;
}

/** Bumped when the renderer stops: every page from before is gone. */
std::atomic<uint64_t> gEpoch{1};

void installStopHook()
{
    static std::once_flag once;
    std::call_once(once, [] {
        Renderer::get().addStopHook([](VulkanContext& vk) {
            gpu().release(vk);
            ++gEpoch;
        });
    });
}

void drain();

/**
 * Runs f on the render thread and waits for it, once queued live batches have painted. Main thread. f may free what
 * earlier GPU work (live batches, launched texture graph slices) used: the queue is idle when it starts.
 */
template <class F> void gpuRun(F&& f)
{
    drain();
    Renderer& r = Renderer::get();
    if (!r.running() && !r.start())
        throw std::runtime_error("texture painting needs a GPU, and the renderer could not start");
    installStopHook();
    r.runTask([&](VulkanContext& vk) {
         Gpu& g = gpu();
         g.init(vk);
         vkQueueWaitIdle(vk.queue);
         f(vk, g);
         return true;
     }).get();
}

// ---------------------------------------------------------------------------------------------------- buffers

struct Key
{
    NodeId set = scene::kNoNode;
    std::string buffer;
    auto operator<=>(const Key&) const = default;
};

using TileKey = std::pair<std::string, int>; // channel, udim

struct ChannelTile
{
    /** Page index -> half pool slot. */
    std::map<uint32_t, uint32_t> pages;
    /** Pages whose image texels are stale. */
    std::set<uint32_t> dirty;
};

struct Surface
{
    std::shared_ptr<const PaintMesh> mesh;
    std::unique_ptr<SurfaceAtlas> atlas;
    /** Geometry page -> wide pool slot (render thread). */
    std::map<uint32_t, uint32_t> geo;
};

struct CowPage
{
    uint32_t before = kNone;
    uint32_t after = kNone;
};

struct ValueImages;

/** A stroke being painted: its pages' originals and build-up, and what its dabs need on the GPU. */
struct Live
{
    StrokePtr header;
    std::shared_ptr<const PaintMesh> mesh;
    std::vector<std::string> channels;
    std::vector<glm::vec3> values;
    std::vector<float> falloff;
    std::shared_ptr<const brush::AlphaImage> alpha;
    float roundness = 1.0f;
    /** Narrows the tip per dab by its pen tilt. */
    brush::PressureCurve tiltRoundness;
    float strength = 1.0f;
    uint32_t mode = 0, sub = 0;
    /** fill: a bit per triangle. */
    std::vector<uint32_t> triMask;
    /** A bit per locked triangle, and the limit mask (RGBA floats); empty when the stroke has none. */
    std::vector<uint32_t> lockMask;
    std::vector<float> limit;
    int limitW = 0, limitH = 0;
    VkDeviceAddress lockAddr = 0, limitAddr = 0;
    /** Gradient fills: per channel kRamp rgba entries (value, amount). */
    std::vector<float> ramp;
    VkDeviceAddress rampAddr = 0;
    /** project: the picture (RGBA floats) and the NDC box (x0, y0, x1, y1) of its covered pixels. */
    std::vector<float> image;
    int imageW = 0, imageH = 0;
    glm::vec4 imageBox{0.0f};
    /** normal stamps: the normal map. */
    std::vector<float> stamp;
    int stampW = 0, stampH = 0;
    /** The stencil's picture (RGBA floats); empty when the stroke has none. */
    std::vector<float> stencil;
    int stencilW = 0, stencilH = 0;
    VkDeviceAddress stencilAddr = 0;
    /** Channel values from a texture graph, and its tiles per uv unit. */
    std::shared_ptr<const ValueImages> sourceImages;
    float valueScale = 1.0f;
    /** The source maps in object space (payload source space "object"), valueSize cm a tile; else on the uvs. */
    bool valueObject = false;
    float valueSize = 100.0f;
    /** The source's varied tiling (payload source "tiling"); null for a plain repeat. */
    json tiling;
    /** The above on the device, made on the first batch. */
    bool uploaded = false;
    std::vector<compute::Buffer> uploads;
    VkDeviceAddress falloffAddr = 0, alphaAddr = 0, triMaskAddr = 0, imageAddr = 0, stampAddr = 0;
    VkDeviceAddress valueAddr[kMaxChannels]{};
    /** Dabs given so far, and the last centre per symmetry copy (direction strokes). */
    size_t dabs = 0;
    std::map<uint32_t, glm::vec3> last;
    std::map<std::pair<TileKey, uint32_t>, CowPage> cow;
    /** Build-up page per (udim, page). */
    std::map<std::pair<int, uint32_t>, uint32_t> acc;
    compute::Buffer depth;
    int depthW = 0, depthH = 0;
    bool depthReady = false;
    /** The symmetry the stroke was painted with, and the views a projection projects through (one per copy). */
    PaintSymmetry sym;
    std::vector<StrokeView> views;
    size_t viewIndex = 0;
    /** Per triangle, 1 where the stroke view sees only its back (the kernel paints none of it); empty until a batch needs it. */
    std::vector<uint8_t> backFacing;

    const StrokeView* view() const
    {
        if (viewIndex < views.size())
            return &views[viewIndex];
        return header->view ? &*header->view : nullptr;
    }
};

struct Layer
{
    uint64_t generation = 0;
    uint64_t epoch = 0;
    /** Tile sizes by UDIM, as rasterised. */
    std::map<int, std::pair<int, int>> sizes;
    std::map<TileKey, ChannelTile> tiles;
    std::vector<uint64_t> applied;
    /** Each painted mesh's signature (PaintMesh::signature) when its strokes were last rasterised. */
    std::map<std::string, uint64_t> meshSignatures;
    std::shared_ptr<const Baseline> baseline;
    uint64_t clears = 0;
    bool synced = false;
    std::unique_ptr<Live> live;
    /** Texels {x0, y0, x1, y1} per UDIM tile whose pages changed since takeChanges last looked. */
    std::map<int, std::array<int, 4>> changed;
};

std::atomic<uint64_t> gGeneration{1};
std::map<Key, std::shared_ptr<Layer>> gLayers;
/** Surfaces by mesh, UDIM and size. */
std::map<std::tuple<const PaintMesh*, int, int, int>, std::shared_ptr<Surface>> gSurfaces;

struct MeshEntry
{
    scene::MeshPtr mesh;
    std::shared_ptr<const geo::MeshBvh> bvh;
    std::map<std::string, std::shared_ptr<const PaintMesh>> paint;
};
std::map<std::string, MeshEntry> gMeshes;

bool meshInUse(const PaintMesh* m)
{
    for (auto& [path, e] : gMeshes)
        for (auto& [mat, p] : e.paint)
            if (p.get() == m)
                return true;
    for (auto& [k, l] : gLayers)
        if (l->live && l->live->mesh.get() == m)
            return true;
    return false;
}

// Images the texture graph reads, shared with other threads.
struct ImageEntry
{
    GpuImageRef image;
    bool full = true;
};
std::mutex gImageMutex;
std::map<std::tuple<NodeId, std::string, std::string, int>, ImageEntry> gImages; // set, buffer, channel, udim
std::map<std::tuple<NodeId, std::string, int>, std::pair<int, int>> gImageSizes;  // set, buffer, udim

std::map<int, std::function<void(NodeId)>> gListeners;
int gNextListener = 1;
bool gCommandsRegistered = false;

void notify(NodeId set)
{
    const auto listeners = gListeners;
    for (const auto& [id, fn] : listeners)
        fn(set);
}

Layer& layerFor(const Key& k)
{
    drain();
    std::shared_ptr<Layer>& l = gLayers[k];
    if (!l)
        l = std::make_shared<Layer>();
    if (l->epoch != gEpoch.load())
    {
        // The renderer restarted: the pages are gone, so this is a fresh buffer.
        const uint64_t epoch = gEpoch.load();
        l = std::make_shared<Layer>();
        l->epoch = epoch;
        l->generation = gGeneration++;
    }
    return *l;
}

/** The layer without waiting for queued live batches: for what they leave alone (whether it exists, its live stroke). */
std::shared_ptr<Layer> peekLayer(const Key& k)
{
    auto it = gLayers.find(k);
    if (it == gLayers.end() || it->second->epoch != gEpoch.load())
        return nullptr;
    return it->second;
}

Layer* findLayer(const Key& k)
{
    drain();
    return peekLayer(k).get();
}

/** Guards gSurfaces: the paint worker finds surfaces while the main thread adds warmed ones. */
std::mutex gSurfaceMutex;

std::shared_ptr<Surface> surface(const std::shared_ptr<const PaintMesh>& mesh, int udim, int w, int h)
{
    std::scoped_lock lock(gSurfaceMutex);
    const auto key = std::make_tuple(mesh.get(), udim, w, h);
    if (auto it = gSurfaces.find(key); it != gSurfaces.end())
        return it->second;
    auto s = std::make_shared<Surface>();
    s->mesh = mesh;
    s->atlas = std::make_unique<SurfaceAtlas>(mesh, udim, w, h);
    gSurfaces[key] = s;
    return s;
}

bool meshInUse(const PaintMesh* m);

/** Frees the geometry pages and triangles of meshes nothing paints any more (render thread, the main thread waiting). */
void pruneSurfaces(VulkanContext& vk, Gpu& g)
{
    std::scoped_lock lock(gSurfaceMutex);
    for (auto it = gSurfaces.begin(); it != gSurfaces.end();)
        if (!meshInUse(it->second->mesh.get()))
        {
            for (auto& [p, slot] : it->second->geo)
                g.wide.release(slot);
            it = gSurfaces.erase(it);
        }
        else
            ++it;
    for (auto it = g.tris.begin(); it != g.tris.end();)
        if (!meshInUse(it->first))
        {
            compute::destroy(vk, it->second.buffer);
            it = g.tris.erase(it);
        }
        else
            ++it;
}

uint32_t channelSlot(Layer& l, const TileKey& k, uint32_t page)
{
    auto it = l.tiles.find(k);
    if (it == l.tiles.end())
        return kNone;
    auto p = it->second.pages.find(page);
    return p == it->second.pages.end() ? kNone : p->second;
}

/** Guards Layer::changed: live batches note changes on the render thread while the main thread takes them. */
std::mutex gChangedMutex;

/** Notes that texels [x0, x1) x [y0, y1) of a tile changed. */
void noteChanged(Layer& l, int udim, int x0, int y0, int x1, int y1)
{
    std::scoped_lock lock(gChangedMutex);
    auto [it, fresh] = l.changed.try_emplace(udim, std::array<int, 4>{x0, y0, x1, y1});
    if (!fresh)
        it->second = {std::min(it->second[0], x0), std::min(it->second[1], y0), std::max(it->second[2], x1), std::max(it->second[3], y1)};
}

void setSlot(Layer& l, const TileKey& k, uint32_t page, uint32_t slot)
{
    ChannelTile& t = l.tiles[k];
    if (slot == kNone)
        t.pages.erase(page);
    else
        t.pages[page] = slot;
    t.dirty.insert(page);
    if (auto size = l.sizes.find(k.second); size != l.sizes.end())
    {
        const int pagesX = (size->second.first + kPageSize - 1) / kPageSize;
        const int x0 = int(page % uint32_t(pagesX)) * kPageSize, y0 = int(page / uint32_t(pagesX)) * kPageSize;
        noteChanged(l, k.second, x0, y0, x0 + kPageSize, y0 + kPageSize);
    }
}

// ---------------------------------------------------------------------------------------------------- page views

void dropViews(VulkanContext& vk, NodeId set, const std::string& buffer)
{
    for (auto it = gViews.begin(); it != gViews.end();)
        if (std::get<0>(it->first) == set && std::get<1>(it->first) == buffer)
        {
            compute::destroy(vk, it->second.table);
            it = gViews.erase(it);
        }
        else
            ++it;
    std::erase_if(gViewSizes, [&](const auto& kv) { return std::get<0>(kv.first) == set && std::get<1>(kv.first) == buffer; });
}

/**
 * Rebuilds the views of a layer's tiles whose pages changed (render thread, inside a recording): the tables are written
 * by the commands, so GPU work launched before still reads the tables it was recorded with.
 */
void updateViews(VulkanContext& vk, VkCommandBuffer cmd, Gpu& g, const Key& key, Layer& l, std::vector<compute::Buffer>& temps)
{
    bool copied = false;
    for (auto& [udim, size] : l.sizes)
        gViewSizes[{key.set, key.buffer, udim}] = size;
    for (auto& [tk, tile] : l.tiles)
    {
        if (tile.dirty.empty())
            continue;
        auto size = l.sizes.find(tk.second);
        if (size == l.sizes.end())
            continue;
        const auto [w, h] = size->second;
        const size_t pagesX = size_t((w + kPageSize - 1) / kPageSize), pagesY = size_t((h + kPageSize - 1) / kPageSize);
        ViewEntry& v = gViews[{key.set, key.buffer, tk.first, tk.second}];
        const VkDeviceSize bytes = std::max<VkDeviceSize>(pagesX * pagesY * sizeof(VkDeviceAddress), 16);
        if (v.table.size != bytes)
        {
            // The queue is idle when a table changes size (a sync or a rasterise, through gpuRun).
            compute::destroy(vk, v.table);
            v.table = compute::makeBuffer(vk, bytes, 0, compute::Host::None);
        }
        v.width = w;
        v.height = h;
        std::vector<VkDeviceAddress> table(size_t(bytes / sizeof(VkDeviceAddress)), 0);
        for (auto& [page, slot] : tile.pages)
            if (page < pagesX * pagesY)
                table[page] = g.half.address(slot);
        temps.push_back(compute::hostBuffer(vk, table.data(), bytes));
        compute::copy(cmd, temps.back(), v.table, bytes);
        copied = true;
    }
    if (copied)
        compute::memoryBarrier(cmd);
}

/**
 * Pages (row-major ids on a grid pagesX wide) as a few texel rectangles: runs along each row, stacked when rows repeat
 * them, then the closest pairs merged while there are too many. A stroke over distant uv islands stays several small
 * regions instead of one box over the texture, which is what the texture set evaluates again.
 */
std::vector<std::array<int, 4>> pageRegions(std::span<const uint32_t> pages, int pagesX, int width, int height)
{
    constexpr size_t kMaxRegions = 12;
    std::vector<uint32_t> sorted(pages.begin(), pages.end());
    std::sort(sorted.begin(), sorted.end());
    // Runs of consecutive pages in a row: {x0, x1, y} in pages.
    std::vector<std::array<int, 3>> runs;
    for (uint32_t p : sorted)
    {
        const int x = int(p % uint32_t(pagesX)), y = int(p / uint32_t(pagesX));
        if (!runs.empty() && runs.back()[2] == y && runs.back()[1] == x)
            ++runs.back()[1];
        else
            runs.push_back({x, x + 1, y});
    }
    // Page rectangles {x0, y0, x1, y1}: a run extends the rectangle above it when it spans the same columns.
    std::vector<std::array<int, 4>> rects;
    for (const auto& [x0, x1, y] : runs)
    {
        auto above = std::find_if(rects.begin(), rects.end(), [&](const std::array<int, 4>& r) { return r[0] == x0 && r[2] == x1 && r[3] == y; });
        if (above != rects.end())
            (*above)[3] = y + 1;
        else
            rects.push_back({x0, y, x1, y + 1});
    }
    auto area = [](const std::array<int, 4>& r) { return int64_t(r[2] - r[0]) * int64_t(r[3] - r[1]); };
    auto unite = [](const std::array<int, 4>& a, const std::array<int, 4>& b) {
        return std::array<int, 4>{std::min(a[0], b[0]), std::min(a[1], b[1]), std::max(a[2], b[2]), std::max(a[3], b[3])};
    };
    // Many rectangles: first one per cell of an 8 x 8 grid, so the pairwise merging below stays cheap.
    if (rects.size() > 64)
    {
        const int pagesY = (height + kPageSize - 1) / kPageSize;
        std::map<int, std::array<int, 4>> cells;
        for (const std::array<int, 4>& r : rects)
        {
            const int cell = std::min(7, (r[0] + r[2]) * 4 / std::max(pagesX, 1)) + 8 * std::min(7, (r[1] + r[3]) * 4 / std::max(pagesY, 1));
            auto [it, fresh] = cells.try_emplace(cell, r);
            if (!fresh)
                it->second = unite(it->second, r);
        }
        rects.clear();
        for (const auto& [cell, r] : cells)
            rects.push_back(r);
    }
    // Merge the pair that adds the least area until few enough remain, and merge freely while it adds nothing.
    while (rects.size() > 1)
    {
        int64_t best = INT64_MAX;
        size_t bi = 0, bj = 0;
        for (size_t i = 0; i < rects.size(); ++i)
            for (size_t j = i + 1; j < rects.size(); ++j)
            {
                const int64_t waste = area(unite(rects[i], rects[j])) - area(rects[i]) - area(rects[j]);
                if (waste < best)
                {
                    best = waste;
                    bi = i;
                    bj = j;
                }
            }
        if (best > 0 && rects.size() <= kMaxRegions)
            break;
        rects[bi] = unite(rects[bi], rects[bj]);
        rects.erase(rects.begin() + std::ptrdiff_t(bj));
    }
    std::vector<std::array<int, 4>> out;
    for (const std::array<int, 4>& r : rects)
        out.push_back({r[0] * kPageSize, r[1] * kPageSize, std::min(r[2] * kPageSize, width), std::min(r[3] * kPageSize, height)});
    return out;
}

/** Copies stale pages into the images the graph reads (render thread, inside a recording). */
void recordResolve(VulkanContext& vk, VkCommandBuffer cmd, Gpu& g, const Key& key, Layer& l, std::vector<compute::Buffer>& temps)
{
    updateViews(vk, cmd, g, key, l, temps);
    for (auto& [tk, tile] : l.tiles)
    {
        GpuImageRef img;
        bool full = false;
        {
            std::scoped_lock lock(gImageMutex);
            auto it = gImages.find({key.set, key.buffer, tk.first, tk.second});
            if (it != gImages.end())
            {
                img = it->second.image;
                full = it->second.full;
                it->second.full = false;
            }
        }
        if (!img)
        {
            tile.dirty.clear();
            continue;
        }
        std::vector<uint32_t> pages;
        if (full)
            for (auto& [p, s] : tile.pages)
                pages.push_back(p);
        else
            pages.assign(tile.dirty.begin(), tile.dirty.end());
        tile.dirty.clear();
        if (pages.empty() && !full)
            continue;
        if (!img->prepare(vk, cmd, temps))
            continue;
        img->beginWrite(cmd);
        compute::memoryBarrier(cmd);
        const int pagesX = (img->width() + kPageSize - 1) / kPageSize;
        if (!full)
            img->noteWriteRegions(pageRegions(pages, pagesX, img->width(), img->height()));
        // One copy command per pool chunk: a copy per page costs several times the bytes it moves.
        std::map<VkBuffer, std::vector<VkBufferImageCopy>> regions;
        for (uint32_t p : pages)
        {
            const int x0 = int(p % uint32_t(pagesX)) * kPageSize, y0 = int(p / uint32_t(pagesX)) * kPageSize;
            if (x0 >= img->width() || y0 >= img->height())
                continue;
            auto s = tile.pages.find(p);
            const uint32_t slot = s == tile.pages.end() ? g.zeroPage : s->second;
            VkBufferImageCopy region{};
            region.bufferOffset = g.half.offset(slot);
            region.bufferRowLength = uint32_t(kPageSize);
            region.bufferImageHeight = uint32_t(kPageSize);
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageOffset = {x0, y0, 0};
            region.imageExtent = {uint32_t(std::min(kPageSize, img->width() - x0)), uint32_t(std::min(kPageSize, img->height() - y0)), 1};
            img->noteWriteRect(x0, y0, x0 + std::min(kPageSize, img->width() - x0), y0 + std::min(kPageSize, img->height() - y0));
            regions[g.half.buffer(slot)].push_back(region);
        }
        for (const auto& [buffer, list] : regions)
            vkCmdCopyBufferToImage(cmd, buffer, img->image(), VK_IMAGE_LAYOUT_GENERAL, uint32_t(list.size()), list.data());
        compute::memoryBarrier(cmd);
        img->endWrite(cmd);
    }
}

void resolve(VulkanContext& vk, Gpu& g, const Key& key, Layer& l)
{
    std::vector<compute::Buffer> temps;
    g.sub.run(vk, [&](VkCommandBuffer cmd) { recordResolve(vk, cmd, g, key, l, temps); });
    for (compute::Buffer& b : temps)
        compute::destroy(vk, b);
}

// ---------------------------------------------------------------------------------------------------- strokes

std::pair<int, int> tileSize(const tex::SetSettings& s, const tex::Tile& t, const Buffer* data)
{
    if (data && data->width > 0 && data->height > 0)
        return {data->width, data->height};
    return s.tileSize(t);
}

// ---------------------------------------------------------------------------------------------------- graph payloads

/** Channel values a texture graph gives over one tile of uv (the material brush), RGBA floats rows top first. */
struct ValueImages
{
    int width = 0, height = 0;
    std::map<std::string, std::vector<float>> channels;
};

std::mutex gGraphCacheMutex;
std::map<std::string, std::shared_ptr<const ValueImages>> gValueCache;
std::map<std::string, std::shared_ptr<const brush::AlphaImage>> gTipCache;

/** The graph a source names and what to request of it for the channels. Throws std::runtime_error. */
tex::Graph sourceGraph(const json& src, const std::vector<std::pair<std::string, tex::PortType>>& channels, std::vector<tex::OutputRequest>& requests)
{
    if (src.contains("library"))
    {
        const std::string id = src["library"].get<std::string>();
        std::optional<tex::library::Entry> e = tex::library::find(id);
        if (!e)
            throw std::runtime_error("no library material " + id);
        tex::Graph g = tex::library::graphOf(tex::library::document(*e));
        requests = tex::materialRequests(g, channels);
        return g;
    }
    if (src.contains("graph"))
    {
        tex::Graph g = tex::Graph::fromJson(src["graph"]);
        if (!g.firstOfType(tex::kMaterialOutputType))
            throw std::runtime_error("a brush source graph needs a Material Output");
        requests = tex::materialRequests(g, channels);
        return g;
    }
    if (src.contains("node"))
    {
        const std::string type = src["node"].get<std::string>();
        const tex::NodeDef* def = tex::findNode(type);
        if (!def || def->outputs.empty())
            throw std::runtime_error("no texture node type " + type + " with an output");
        tex::Graph g;
        g.name = "BrushSource";
        g.nodes.push_back({"Source", type, {0.0, 0.0}, src.value("values", json::object())});
        const tex::PortDecl& out = def->outputs.front();
        for (const auto& [name, t] : channels)
        {
            const bool material = out.type == tex::PortType::Material;
            requests.push_back({name, "Source", out.id, material ? t : tex::PortType::Color, material ? name : std::string()});
        }
        return g;
    }
    throw std::runtime_error("a brush source needs library, graph or node");
}

std::shared_ptr<const ValueImages> valueImages(const json& src, const std::vector<std::string>& written)
{
    std::vector<std::pair<std::string, tex::PortType>> channels;
    const bool node = src.contains("node");
    for (const std::string& c : written)
    {
        if (c == kMaskChannel)
            continue;
        // A material graph only has its Material Output's channels.
        auto it = std::find_if(tex::standardChannels().begin(), tex::standardChannels().end(), [&](const auto& s) { return s.first == c; });
        if (it != tex::standardChannels().end())
            channels.emplace_back(c, it->second);
        else if (node)
            channels.emplace_back(c, tex::PortType::Color);
    }
    const int res = std::clamp(src.value("resolution", 512), 16, 4096);
    std::vector<tex::OutputRequest> requests;
    std::optional<tex::Graph> g;
    if (!channels.empty())
        g = sourceGraph(src, channels, requests);
    // Keyed on the graph itself, so a scene set or library file edited since the last stroke evaluates again.
    std::string key = (g ? g->toJson().dump() : std::string()) + "|" + std::to_string(res);
    for (const auto& [c, t] : channels)
        key += "|" + c;
    {
        std::scoped_lock lock(gGraphCacheMutex);
        if (auto it = gValueCache.find(key); it != gValueCache.end())
            return it->second;
    }
    auto out = std::make_shared<ValueImages>();
    out->width = out->height = res;
    if (g)
    {
        tex::PlanRequest req;
        req.width = req.height = res;
        req.outputs = requests;
        const tex::Plan plan = tex::buildPlan(*g, req);
        if (!plan.error.empty())
            throw std::runtime_error("brush source: " + plan.error);
        tex::EvalResult r = tex::evaluate(plan, {}, "");
        for (auto& [name, img] : r.outputs)
            if (img.width == res && img.height == res)
                out->channels[name] = std::move(img.rgba);
    }
    std::scoped_lock lock(gGraphCacheMutex);
    // Every graph edit makes a new key; drop the old images rather than keep each version.
    if (gValueCache.size() >= 16)
        gValueCache.clear();
    gValueCache[key] = out;
    return out;
}

/** A tip from a texture graph: its first output's luminance times alpha. */
std::shared_ptr<const brush::AlphaImage> tipImage(const json& tip)
{
    const int res = std::clamp(tip.value("resolution", 256), 16, 2048);
    const std::string key = tip.dump() + "|" + std::to_string(res);
    {
        std::scoped_lock lock(gGraphCacheMutex);
        if (auto it = gTipCache.find(key); it != gTipCache.end())
            return it->second;
    }
    json src = tip;
    src["resolution"] = res;
    std::shared_ptr<const ValueImages> v = valueImages(src, {"baseColor"});
    auto it = v->channels.find("baseColor");
    if (it == v->channels.end())
        throw std::runtime_error("the brush tip graph has no colour output");
    auto a = std::make_shared<brush::AlphaImage>();
    a->width = a->height = res;
    a->values.resize(size_t(res) * size_t(res));
    for (size_t i = 0; i < a->values.size(); ++i)
    {
        const float* t = &it->second[i * 4];
        a->values[i] = std::clamp((t[0] * 0.2126f + t[1] * 0.7152f + t[2] * 0.0722f) * t[3], 0.0f, 1.0f);
    }
    std::scoped_lock lock(gGraphCacheMutex);
    gTipCache[key] = a;
    return a;
}

// ---------------------------------------------------------------------------------------------------- strokes

uint32_t modeOf(const Payload& p, uint32_t& sub)
{
    sub = 0;
    if (p.kind == "fill")
        return kModeFill;
    if (p.kind == "project")
        return kModeProject;
    if (p.kind == "normal")
    {
        sub = p.normalMode == "stamp" ? 1 : 0;
        return kModeNormal;
    }
    static const char* filters[] = {"smudge", "blur", "sharpen", "clone", "heal"};
    for (uint32_t i = 0; i < 5; ++i)
        if (p.kind == filters[i])
        {
            sub = i;
            return kModeFilter;
        }
    return kModePaint;
}

/**
 * A gradient's ramps: per stroke channel kRamp entries of (value, amount) between its stops, linear. A stop without the
 * channel takes the payload's value; amount defaults to 1.
 */
std::vector<float> gradientRamp(const Payload& p, const std::vector<std::string>& channels, const std::vector<glm::vec3>& values)
{
    struct Stop
    {
        float position;
        float amount;
        json channels;
    };
    std::vector<Stop> stops;
    for (const json& s : p.gradient.value("stops", json::array()))
        stops.push_back({std::clamp(s.value("position", 0.0f), 0.0f, 1.0f), std::clamp(s.value("amount", 1.0f), 0.0f, 1.0f), s.value("channels", json::object())});
    if (stops.empty())
        stops = {{0.0f, 1.0f, json::object()}, {1.0f, 0.0f, json::object()}};
    std::stable_sort(stops.begin(), stops.end(), [](const Stop& a, const Stop& b) { return a.position < b.position; });
    auto valueAt = [&](const Stop& s, size_t c) {
        const json v = s.channels.value(channels[c], json());
        if (v.is_array() && v.size() >= 3)
            return glm::vec3(v[0].get<float>(), v[1].get<float>(), v[2].get<float>());
        if (v.is_number())
            return glm::vec3(v.get<float>());
        return values[c];
    };
    std::vector<float> ramp(channels.size() * kRamp * 4);
    for (size_t c = 0; c < channels.size(); ++c)
        for (int i = 0; i < kRamp; ++i)
        {
            const float t = float(i) / float(kRamp - 1);
            size_t k = 0;
            while (k + 1 < stops.size() && stops[k + 1].position < t)
                ++k;
            const Stop& a = stops[k];
            const Stop& b = stops[std::min(k + 1, stops.size() - 1)];
            const float span = b.position - a.position;
            const float f = span > 1e-6f ? std::clamp((t - a.position) / span, 0.0f, 1.0f) : (t < a.position ? 0.0f : 1.0f);
            const glm::vec3 v = glm::mix(valueAt(a, c), valueAt(b, c), f);
            const float amount = glm::mix(a.amount, b.amount, f);
            float* e = &ramp[(c * kRamp + size_t(i)) * 4];
            e[0] = v.x, e[1] = v.y, e[2] = v.z, e[3] = amount;
        }
    return ramp;
}

/** What a stroke needs on the GPU besides its dabs, from its header. image is a project stroke's picture. */
std::unique_ptr<Live> makeLive(StrokePtr header, std::shared_ptr<const PaintMesh> mesh, std::shared_ptr<const PaintImage> image = nullptr,
                               std::shared_ptr<const PaintImage> stencil = nullptr, std::shared_ptr<const PaintImage> limit = nullptr)
{
    auto live = std::make_unique<Live>();
    live->header = header;
    live->mesh = std::move(mesh);
    const brush::BrushSettings b = brush::BrushSettings::fromJson(header->brush);
    live->roundness = float(std::clamp(b.roundness, 1e-3, 1.0));
    live->tiltRoundness = b.tiltRoundness;
    live->strength = float(std::clamp(b.strength, 0.0, 1.0));
    const brush::Falloff f = b.resolveFalloff();
    live->falloff.resize(257);
    for (int i = 0; i <= 256; ++i)
        live->falloff[size_t(i)] = float(f.evaluate(i / 256.0));
    const Payload& p = header->payload;
    if (!p.tip.is_null())
        live->alpha = tipImage(p.tip);
    else if (!b.alphaPath.empty())
    {
        std::string err;
        live->alpha = brush::AlphaImage::load(brush::alphas::resolve(b.alphaPath), &err);
        if (!live->alpha)
            log::warn("paint stroke: alpha {}: {}", b.alphaPath, err);
    }
    live->mode = modeOf(p, live->sub);
    for (const std::string& c : p.written())
    {
        if (live->channels.size() >= kMaxChannels)
        {
            log::warn("paint stroke: paints the first {} channels; {} and the rest are left out", kMaxChannels, c);
            break;
        }
        live->channels.push_back(c);
        auto it = p.channels.find(c);
        live->values.push_back(it != p.channels.end() ? it->second : glm::vec3(1.0f));
    }
    if (live->mode == kModeFill)
    {
        live->triMask.assign((live->mesh->triangleCount() + 31) / 32 + 1, 0u);
        for (uint32_t t : p.triangles)
            if (t < live->mesh->triangleCount())
                live->triMask[t >> 5] |= 1u << (t & 31u);
    }
    live->sym = PaintSymmetry::fromJson(header->symmetry);
    if (live->mode == kModeProject)
    {
        if (!image)
            throw std::runtime_error("the projection's image " + p.image + " is missing");
        if (!header->view)
            throw std::runtime_error("a projection needs the view it was painted in");
        // Copy k sees a texel at p where the stroke's view saw transform(k) * p.
        for (uint32_t k = 0; k < live->sym.count(); ++k)
        {
            const glm::dmat4 t = live->sym.transform(k), inv = glm::inverse(t);
            StrokeView v = *header->view;
            v.objectToClip = glm::mat4(glm::dmat4(header->view->objectToClip) * t);
            v.eye = glm::vec3(inv * glm::dvec4(glm::dvec3(header->view->eye), 1.0));
            v.direction = glm::vec3(glm::normalize(glm::dmat3(inv) * glm::dvec3(header->view->direction)));
            live->views.push_back(v);
        }
        live->image = image->rgba();
        live->imageW = image->width;
        live->imageH = image->height;
        glm::vec2 lo(1e30f), hi(-1e30f);
        for (int y = 0; y < image->height; ++y)
            for (int x = 0; x < image->width; ++x)
                if (live->image[(size_t(y) * size_t(image->width) + size_t(x)) * 4 + 3] > 0.0f)
                {
                    lo = glm::min(lo, glm::vec2(float(x), float(y)));
                    hi = glm::max(hi, glm::vec2(float(x + 1), float(y + 1)));
                }
        const glm::vec2 size(float(image->width), float(image->height));
        live->imageBox = lo.x <= hi.x ? glm::vec4(lo / size * 2.0f - 1.0f, hi / size * 2.0f - 1.0f) : glm::vec4(0.0f);
    }
    if (live->mode == kModeNormal && live->sub == 1)
    {
        std::string err;
        image::Image st = image::read(paths::fromUtf8(p.stampImage), &err);
        if (st.width <= 0)
            throw std::runtime_error("normal stamp " + p.stampImage + ": " + err);
        live->stamp = std::move(st.rgba);
        live->stampW = st.width;
        live->stampH = st.height;
    }
    if (!p.source.is_null() && (live->mode == kModePaint || live->mode == kModeFill))
    {
        live->sourceImages = valueImages(p.source, live->channels);
        live->valueScale = float(p.source.value("scale", 1.0));
        live->valueObject = p.source.value("space", std::string("uv")) == "object";
        live->valueSize = float(p.source.value("size", 100.0));
        live->tiling = p.source.value("tiling", json());
    }
    if (!p.gradient.is_null() && live->mode == kModeFill)
        live->ramp = gradientRamp(p, live->channels, live->values);
    if (!header->payload.locked.empty())
    {
        live->lockMask.assign((live->mesh->triangleCount() + 31) / 32 + 1, 0u);
        for (uint32_t t : header->payload.locked)
            if (t < live->mesh->triangleCount())
                live->lockMask[t >> 5] |= 1u << (t & 31u);
    }
    if (!header->payload.limit.empty())
    {
        if (!limit)
            log::warn("paint stroke {}: its limit mask {} is missing; painted without it", header->id, header->payload.limit);
        else
        {
            live->limit = limit->rgba();
            live->limitW = limit->width;
            live->limitH = limit->height;
        }
    }
    if (header->stencil)
    {
        if (!header->view)
            log::warn("paint stroke {}: its stencil needs the view it was painted in; painted without it", header->id);
        else if (!stencil)
            log::warn("paint stroke {}: stencil image {} is missing; painted without it", header->id, header->stencil->image);
        else
        {
            live->stencil = stencil->rgba();
            live->stencilW = stencil->width;
            live->stencilH = stencil->height;
        }
    }
    return live;
}

/** The pictures and tables a stroke reads, on the device once per stroke (render thread). */
void ensureUploads(VulkanContext& vk, Gpu& g, Live& live)
{
    if (live.uploaded)
        return;
    live.uploaded = true;
    auto upload = [&](const void* data, size_t bytes) {
        std::vector<uint8_t> padded(std::max<size_t>(bytes, 16), 0);
        if (bytes)
            std::memcpy(padded.data(), data, bytes);
        compute::Buffer dst = compute::makeBuffer(vk, padded.size(), 0, compute::Host::None);
        compute::Buffer staging = compute::hostBuffer(vk, padded.data(), padded.size());
        g.sub.run(vk, [&](VkCommandBuffer cmd) { compute::copy(cmd, staging, dst, bytes); });
        compute::destroy(vk, staging);
        live.uploads.push_back(dst);
        return dst.address;
    };
    live.falloffAddr = upload(live.falloff.data(), live.falloff.size() * 4);
    if (live.alpha && live.alpha->width > 0)
        live.alphaAddr = upload(live.alpha->values.data(), live.alpha->values.size() * 4);
    if (!live.triMask.empty())
        live.triMaskAddr = upload(live.triMask.data(), live.triMask.size() * 4);
    if (!live.image.empty())
        live.imageAddr = upload(live.image.data(), live.image.size() * 4);
    if (!live.stamp.empty())
        live.stampAddr = upload(live.stamp.data(), live.stamp.size() * 4);
    if (!live.stencil.empty())
        live.stencilAddr = upload(live.stencil.data(), live.stencil.size() * 4);
    if (!live.lockMask.empty())
        live.lockAddr = upload(live.lockMask.data(), live.lockMask.size() * 4);
    if (!live.limit.empty())
        live.limitAddr = upload(live.limit.data(), live.limit.size() * 4);
    if (!live.ramp.empty())
        live.rampAddr = upload(live.ramp.data(), live.ramp.size() * 4);
    if (live.sourceImages)
        for (size_t c = 0; c < live.channels.size(); ++c)
            if (auto it = live.sourceImages->channels.find(live.channels[c]); it != live.sourceImages->channels.end())
                live.valueAddr[c] = upload(it->second.data(), it->second.size() * 4);
}

void releaseLive(VulkanContext& vk, Live& live)
{
    compute::destroy(vk, live.depth);
    for (compute::Buffer& b : live.uploads)
        compute::destroy(vk, b);
    live.uploads.clear();
}

GpuDab gpuDab(Live& live, const Dab& d)
{
    const PaintMesh& m = *live.mesh;
    GpuDab g{};
    if (d.triangle >= m.triangleCount())
        return g;
    const glm::vec3 c = m.point(d), n = m.normal(d.triangle, d.u, d.v);
    g.centerRadius = glm::vec4(c, d.radius);
    g.normalAmount = glm::vec4(n, d.opacity);
    const bool round = live.roundness >= 1.0f && !live.tiltRoundness.enabled && !live.alpha && live.stamp.empty();
    if (!round)
    {
        const glm::vec3 t0 = tipFrame(n), b0 = glm::cross(n, t0);
        const float roundness =
            d.tilt > 0.0f ? std::clamp(live.roundness * float(live.tiltRoundness.map(1.0 - d.tilt)), 0.02f, 1.0f) : live.roundness;
        // A negative roundness tells the kernel the copy is mirrored: its tip, alpha and stamp turn the other way.
        g.tangentRoundness = glm::vec4(t0 * std::cos(d.angle) + b0 * std::sin(d.angle), live.sym.mirrored(d.copy) ? -roundness : roundness);
    }
    if (auto it = live.last.find(d.copy); it != live.last.end() && it->second != c)
        g.direction = glm::vec4(c - it->second, 1.0f);
    live.last[d.copy] = c;
    const Payload& p = live.header->payload;
    if (p.kind == "clone" || p.kind == "heal")
    {
        // Each copy clones from its own source.
        const glm::vec2 off = d.copy < p.sourceOffsets.size() ? p.sourceOffsets[d.copy] : p.sourceOffset;
        g.direction = glm::vec4(off, 0.0f, 2.0f);
    }
    return g;
}

/** The depth of the stroke's view, once per stroke (render thread). */
void ensureDepth(VulkanContext& vk, Gpu& g, Live& live)
{
    if (live.depthReady || !live.view() || !live.header->occlusion || live.mode == kModeFill)
        return;
    const StrokeView& v = *live.view();
    const double scale = std::min(1.0, double(kMaxDepthSize) / double(std::max(v.width, v.height)));
    live.depthW = std::max(1, int(v.width * scale));
    live.depthH = std::max(1, int(v.height * scale));
    live.depth = compute::makeBuffer(vk, VkDeviceSize(live.depthW) * live.depthH * 4, 0, compute::Host::None);
    const TriBuffer& tb = g.triangles(vk, live.mesh);
    DepthPush push{v.objectToClip, tb.buffer.address, live.depth.address, uint32_t(live.mesh->triangleCount()), uint32_t(live.depthW),
                   uint32_t(live.depthH), 0};
    const uint64_t groups = std::max<uint64_t>((push.triCount + 63) / 64, 1);
    push.groupsX = uint32_t(std::min<uint64_t>(groups, 65535));
    g.sub.run(vk, [&](VkCommandBuffer cmd) {
        vkCmdFillBuffer(cmd, live.depth.buffer, 0, VK_WHOLE_SIZE, 0);
        compute::memoryBarrier(cmd);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipeline(vk, g.depth, "paintDepthCS"));
        vkCmdPushConstants(cmd, g.depth.layout, kCompute, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, push.groupsX, uint32_t((groups + push.groupsX - 1) / push.groupsX), 1);
    });
    live.depthReady = true;
}

/** Work for one batch of dabs (or a whole fill or projection), prepared on the main thread. */
struct Batch
{
    std::vector<GpuDab> dabs;
    std::vector<uint32_t> index;
    struct Item
    {
        std::shared_ptr<Surface> surface;
        int udim = 1001;
        uint32_t page = 0;
        uint32_t first = 0, count = 0;
    };
    std::vector<Item> items;
    /** Geometry pages to upload: surface, page. */
    std::vector<std::pair<std::shared_ptr<Surface>, uint32_t>> geo;

    void add(const std::shared_ptr<Surface>& s, uint32_t page, std::set<std::pair<const Surface*, uint32_t>>& queued)
    {
        if (!s->geo.contains(page) && queued.insert({s.get(), page}).second)
        {
            s->atlas->page(page);
            geo.emplace_back(s, page);
        }
    }
};

/** Marks the triangles whose every corner normal faces away from the stroke's view: the kernel rejects all their texels. */
void cullBackFacing(Live& live)
{
    const StrokeView* view = live.view();
    if (!view || live.mode == kModeFill || !live.backFacing.empty())
        return;
    const PaintMesh& m = *live.mesh;
    const glm::vec3 toEye = -glm::normalize(view->direction);
    live.backFacing.assign(m.triangleCount(), 0);
    for (size_t t = 0; t < m.triangleCount(); ++t)
    {
        bool away = true;
        for (size_t k = 0; k < 3 && away; ++k)
        {
            const glm::vec3& n = m.normals[t * 3 + k];
            // A margin, so rounding in the kernel's interpolation can't turn a culled texel to face the view.
            away = glm::dot(n, toEye) < -1e-3f * glm::length(n);
        }
        live.backFacing[t] = away ? 1 : 0;
    }
}

Batch prepareBatch(Layer& l, Live& live, std::span<const Dab> dabs)
{
    LILA_ZONE();
    Batch b;
    cullBackFacing(live);
    std::vector<DabReach> reach;
    const float extent = live.alpha || !live.stamp.empty() ? 1.415f : 1.0f;
    for (const Dab& d : dabs)
    {
        b.dabs.push_back(gpuDab(live, d));
        reach.push_back({glm::vec3(b.dabs.back().centerRadius), d.triangle < live.mesh->triangleCount() ? d.radius * extent : 0.0f});
    }
    std::set<std::pair<const Surface*, uint32_t>> queued;
    for (const auto& [udim, size] : l.sizes)
    {
        std::shared_ptr<Surface> s = surface(live.mesh, udim, size.first, size.second);
        const std::map<uint32_t, std::vector<uint32_t>> reached = s->atlas->pagesFor(reach, 0, live.backFacing);
        // A brush entering unpainted parts reaches many new pages at once; rasterised one by one they stall the stroke.
        std::vector<uint32_t> pages;
        for (const auto& [page, list] : reached)
            pages.push_back(page);
        s->atlas->preparePages(pages);
        for (auto& [page, list] : reached)
        {
            b.add(s, page, queued);
            b.items.push_back({s, udim, page, uint32_t(b.index.size()), uint32_t(list.size())});
            b.index.insert(b.index.end(), list.begin(), list.end());
        }
    }
    return b;
}

bool wholeStroke(const Live& live) { return live.mode == kModeFill || live.mode == kModeProject; }

/** A fill's or projection's pages: every page holding a triangle it reaches. */
Batch prepareWhole(Layer& l, Live& live)
{
    LILA_ZONE();
    Batch b;
    const PaintMesh& m = *live.mesh;
    std::vector<uint8_t> reached(m.triangleCount(), 0);
    if (live.mode == kModeFill)
        for (size_t t = 0; t < m.triangleCount(); ++t)
            reached[t] = (live.triMask[t >> 5] >> (t & 31)) & 1u;
    else
    {
        const glm::mat4& toClip = live.view()->objectToClip;
        const glm::vec4 box = live.imageBox;
        for (size_t t = 0; t < m.triangleCount(); ++t)
        {
            if (!m.paintable[t])
                continue;
            glm::vec2 lo(1e30f), hi(-1e30f);
            bool front = false;
            for (int k = 0; k < 3; ++k)
            {
                const glm::vec4 c = toClip * glm::vec4(m.positions[t * 3 + size_t(k)], 1.0f);
                if (c.w <= 1e-6f)
                    continue;
                front = true;
                lo = glm::min(lo, glm::vec2(c) / c.w);
                hi = glm::max(hi, glm::vec2(c) / c.w);
            }
            reached[t] = front && hi.x >= box.x && lo.x <= box.z && hi.y >= box.y && lo.y <= box.w;
        }
    }
    std::set<std::pair<const Surface*, uint32_t>> queued;
    for (const auto& [udim, size] : l.sizes)
    {
        std::shared_ptr<Surface> s = surface(live.mesh, udim, size.first, size.second);
        const std::vector<uint32_t> pages = s->atlas->pagesWhere([&](uint32_t t) { return reached[t] != 0; });
        s->atlas->preparePages(pages);
        for (uint32_t page : pages)
        {
            b.add(s, page, queued);
            b.items.push_back({s, udim, page, 0, 0});
        }
    }
    return b;
}

/**
 * Paints a batch (render thread): copy on write, build-up pages, the dab kernel, then the images. With launched, the work
 * is launched on Gpu::batches and its temporaries go there, to be freed once it finishes; else it waits for the GPU.
 */
size_t runBatch(VulkanContext& vk, Gpu& g, const Key& key, Layer& l, Live& live, Batch& b, std::vector<compute::Buffer>* launched = nullptr)
{
    LILA_ZONE();
    if (!launched)
        pruneSurfaces(vk, g);
    ensureDepth(vk, g, live);
    ensureUploads(vk, g, live);
    std::vector<compute::Buffer> temps;
    size_t newPages = 0;
    const Stroke& h = *live.header;
    GpuParams params;
    if (const StrokeView* view = live.view(); view && live.mode != kModeFill)
    {
        params.objectToClip = view->objectToClip;
        params.clipToObject = glm::inverse(view->objectToClip);
        params.eye = glm::vec4(view->eye, 1.0f);
        params.viewDir = glm::vec4(glm::normalize(view->direction), 0.0f);
        params.flags |= kFlagView;
        if (live.depthReady)
        {
            params.flags |= kFlagOcclusion;
            params.depth = live.depth.address;
            params.depthW = uint32_t(live.depthW);
            params.depthH = uint32_t(live.depthH);
        }
    }
    const Payload& pay = h.payload;
    if (pay.kind == "direction")
    {
        params.flags |= kFlagDirection;
        if (pay.directionChannel != "normal")
            params.flags |= kFlagFlow;
    }
    params.mode = live.mode;
    params.sub = live.sub;
    params.channels = uint32_t(live.channels.size());
    params.blend = uint32_t(pay.blend);
    params.strength = live.strength;
    params.tilt = pay.tilt;
    params.filterRadius = pay.filterRadius;
    params.amount = pay.amount;
    params.sourceU = pay.sourceOffset.x;
    params.sourceV = pay.sourceOffset.y;
    params.facingFade = pay.facingFade;
    params.intensity = pay.intensity;
    params.valueScale = live.valueScale;
    params.valueSpace = live.valueObject ? 1u : 0u;
    params.valueSize = live.valueSize;
    for (size_t c = 0; c < live.channels.size(); ++c)
    {
        params.values[c] = glm::vec4(live.values[c], 1.0f);
        if (live.channels[c] == kMaskChannel)
            params.maskChannel = uint32_t(c);
        if ((pay.kind == "direction" || pay.kind == "normal") && live.channels[c] == pay.directionChannel)
            params.directionChannel = uint32_t(c);
        if (pay.kind == "project" && live.channels[c] == pay.colorChannel)
            params.colorChannel = uint32_t(c);
        if (live.channels[c] == "normal")
            params.normalMask |= 1u << c;
        if (live.valueAddr[c])
        {
            params.valueImages[c] = live.valueAddr[c];
            params.flags |= kFlagValueImages;
        }
    }
    if (live.sourceImages)
    {
        params.valueW = uint32_t(live.sourceImages->width);
        params.valueH = uint32_t(live.sourceImages->height);
        if (live.tiling.is_object())
        {
            params.hexOn = 1;
            params.hexRotate = std::clamp(live.tiling.value("rotate", 1.0f), 0.0f, 1.0f);
            params.hexContrast = std::clamp(live.tiling.value("contrast", 0.5f), 0.0f, 0.99f);
            params.hexJitter = std::clamp(live.tiling.value("jitter", 0.0f), 0.0f, 1.0f);
            // Height steers the blend where the source has one (the higher sample wins the seam), else base colour's brightness.
            for (const char* steer : {"height", "baseColor"})
                for (size_t c = 0; c < live.channels.size() && params.hexWeight == kNone; ++c)
                    if (live.channels[c] == steer && live.valueAddr[c])
                        params.hexWeight = uint32_t(c);
            for (size_t c = 0; c < live.channels.size(); ++c)
                if (live.channels[c] == "baseColor")
                    params.hexColor = uint32_t(c);
        }
    }
    if (live.rampAddr)
    {
        const json& gr = pay.gradient;
        const std::string type = gr.value("type", std::string("linear"));
        params.gradOn = 1;
        params.gradType = type == "radial" ? 1 : type == "reflected" ? 2 : type == "angle" ? 3 : 0;
        params.gradSpace = gr.value("space", std::string("uv")) == "screen" ? 1 : 0;
        params.gradDither = gr.value("dither", false) ? 1 : 0;
        const json from = gr.value("start", json::array({0.0, 0.0})), to = gr.value("end", json::array({1.0, 0.0}));
        params.gradA[0] = from[0].get<float>(), params.gradA[1] = from[1].get<float>();
        params.gradB[0] = to[0].get<float>(), params.gradB[1] = to[1].get<float>();
        params.ramp = live.rampAddr;
        if (h.view)
        {
            params.gradToClip = h.view->objectToClip;
            params.viewW = uint32_t(h.view->width);
            params.viewH = uint32_t(h.view->height);
        }
    }
    params.locked = live.lockAddr;
    params.limit = live.limitAddr;
    params.limitW = uint32_t(live.limitW);
    params.limitH = uint32_t(live.limitH);
    if (live.stencilAddr && h.stencil && h.view)
    {
        const StrokeStencil& st = *h.stencil;
        params.stencil = live.stencilAddr;
        params.stencilW = uint32_t(live.stencilW);
        params.stencilH = uint32_t(live.stencilH);
        params.stencilX = st.offset.x;
        params.stencilY = st.offset.y;
        params.stencilScale = st.scale;
        params.stencilAngle = st.rotation;
        params.stencilFlags = kStencilOn | (st.invert ? kStencilInvert : 0) | (st.tile ? kStencilTile : 0) | (st.mode == "color" ? kStencilColor : 0);
        params.viewW = uint32_t(h.view->width);
        params.viewH = uint32_t(h.view->height);
        params.stencilToClip = h.view->objectToClip;
        for (size_t c = 0; c < live.channels.size(); ++c)
            if (st.mode == "color" && live.channels[c] == st.colorChannel)
                params.stencilColor = uint32_t(c);
    }

    // Copy on write for every channel page the batch reaches, and a cleared build-up page per page.
    std::vector<GpuItem> items(b.items.size());
    std::vector<std::pair<uint32_t, uint32_t>> copies; // before, after
    std::vector<uint32_t> clearHalf, clearWide;
    std::vector<std::pair<std::vector<GeoTexel>*, uint32_t>> uploads;
    for (auto& [s, page] : b.geo)
    {
        const uint32_t slot = g.wide.alloc(vk);
        s->geo[page] = slot;
        uploads.emplace_back(const_cast<std::vector<GeoTexel>*>(&s->atlas->page(page)), slot);
    }
    std::map<int, uint32_t> tileIndex;
    for (const auto& [udim, size] : l.sizes)
        tileIndex.emplace(udim, uint32_t(tileIndex.size()));
    for (size_t i = 0; i < b.items.size(); ++i)
    {
        const Batch::Item& it = b.items[i];
        GpuItem& gi = items[i];
        gi.geo = g.wide.address(it.surface->geo.at(it.page));
        auto [acc, freshAcc] = live.acc.try_emplace({it.udim, it.page}, kNone);
        if (freshAcc)
        {
            acc->second = g.wide.alloc(vk);
            clearWide.push_back(acc->second);
        }
        gi.acc = g.wide.address(acc->second);
        gi.dabFirst = it.first;
        gi.dabCount = it.count;
        gi.page = it.page;
        gi.tile = tileIndex.count(it.udim) ? tileIndex[it.udim] : 0;
        for (size_t c = 0; c < live.channels.size(); ++c)
        {
            const TileKey tk{live.channels[c], it.udim};
            auto [cw, fresh] = live.cow.try_emplace({tk, it.page});
            if (fresh)
            {
                cw->second.before = channelSlot(l, tk, it.page);
                cw->second.after = g.half.alloc(vk);
                if (cw->second.before == kNone)
                {
                    clearHalf.push_back(cw->second.after);
                    ++newPages;
                }
                else
                    copies.emplace_back(cw->second.before, cw->second.after);
                setSlot(l, tk, it.page, cw->second.after);
            }
            l.tiles[tk].dirty.insert(it.page);
            gi.dst[c] = g.half.address(cw->second.after);
            gi.src[c] = cw->second.before == kNone ? 0 : g.half.address(cw->second.before);
        }
    }

    auto host = [&](const void* data, size_t bytes) {
        // Never read past a small (or empty) source to make up the minimum size.
        uint8_t small[16] = {};
        if (bytes && bytes < 16)
            std::memcpy(small, data, bytes);
        temps.push_back(bytes >= 16 ? compute::hostBuffer(vk, data, bytes) : compute::hostBuffer(vk, small, 16));
        return temps.back().address;
    };
    // Tiles, and for filters each channel's pages as they were before the stroke.
    std::vector<GpuTile> tiles;
    for (const auto& [udim, size] : l.sizes)
    {
        GpuTile t;
        t.width = uint32_t(size.first);
        t.height = uint32_t(size.second);
        t.pagesX = uint32_t((size.first + kPageSize - 1) / kPageSize);
        t.pagesY = uint32_t((size.second + kPageSize - 1) / kPageSize);
        if (live.mode == kModeFilter)
            for (size_t c = 0; c < live.channels.size(); ++c)
            {
                const TileKey tk{live.channels[c], udim};
                std::vector<VkDeviceAddress> table(size_t(t.pagesX) * t.pagesY, 0);
                if (auto ct = l.tiles.find(tk); ct != l.tiles.end())
                    for (auto& [page, slot] : ct->second.pages)
                        if (page < table.size())
                            table[page] = g.half.address(slot);
                for (auto& [k, cw] : live.cow)
                    if (k.first == tk && k.second < table.size())
                        table[k.second] = cw.before == kNone ? 0 : g.half.address(cw.before);
                t.tables[c] = host(table.data(), table.size() * sizeof(VkDeviceAddress));
            }
        tiles.push_back(t);
    }
    std::vector<compute::Buffer> geoStaging;
    for (auto& [texels, slot] : uploads)
        geoStaging.push_back(compute::hostBuffer(vk, texels->data(), texels->size() * sizeof(GeoTexel)));
    const TriBuffer& tb = g.triangles(vk, live.mesh);
    params.items = host(items.data(), items.size() * sizeof(GpuItem));
    params.dabIndex = host(b.index.data(), b.index.size() * 4);
    params.dabs = host(b.dabs.data(), b.dabs.size() * sizeof(GpuDab));
    params.tiles = host(tiles.data(), tiles.size() * sizeof(GpuTile));
    params.tris = tb.buffer.address;
    params.falloff = live.falloffAddr;
    if (live.alphaAddr)
    {
        params.alpha = live.alphaAddr;
        params.alphaW = uint32_t(live.alpha->width);
        params.alphaH = uint32_t(live.alpha->height);
    }
    params.triMask = live.triMaskAddr;
    params.image = live.imageAddr;
    params.imageW = uint32_t(live.imageW);
    params.imageH = uint32_t(live.imageH);
    params.stamp = live.stampAddr;
    params.stampW = uint32_t(live.stampW);
    params.stampH = uint32_t(live.stampH);
    params.itemCount = uint32_t(items.size());
    const VkDeviceAddress paramsAddr = host(&params, sizeof(params));

    auto record = [&](VkCommandBuffer cmd) {
        for (size_t i = 0; i < uploads.size(); ++i)
        {
            VkBufferCopy r{0, g.wide.offset(uploads[i].second), kWidePage};
            vkCmdCopyBuffer(cmd, geoStaging[i].buffer, g.wide.buffer(uploads[i].second), 1, &r);
        }
        for (uint32_t s : clearWide)
            vkCmdFillBuffer(cmd, g.wide.buffer(s), g.wide.offset(s), kWidePage, 0);
        for (uint32_t s : clearHalf)
            vkCmdFillBuffer(cmd, g.half.buffer(s), g.half.offset(s), kHalfPage, 0);
        for (auto [from, to] : copies)
        {
            VkBufferCopy r{g.half.offset(from), g.half.offset(to), kHalfPage};
            vkCmdCopyBuffer(cmd, g.half.buffer(from), g.half.buffer(to), 1, &r);
        }
        compute::memoryBarrier(cmd);
        if (!items.empty())
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipeline(vk, g.dab, "paintDabCS"));
            for (uint32_t first = 0; first < items.size(); first += kItemsPerDispatch)
            {
                DabPush push{paramsAddr, first, 0};
                vkCmdPushConstants(cmd, g.dab.layout, kCompute, 0, sizeof(push), &push);
                vkCmdDispatch(cmd, uint32_t(kTexels / 64), std::min<uint32_t>(kItemsPerDispatch, uint32_t(items.size()) - first), 1);
            }
            compute::memoryBarrier(cmd);
        }
        recordResolve(vk, cmd, g, key, l, temps);
    };
    if (launched)
    {
        g.batches.launch(vk, record);
        launched->insert(launched->end(), temps.begin(), temps.end());
        launched->insert(launched->end(), geoStaging.begin(), geoStaging.end());
        return newPages;
    }
    g.sub.run(vk, record);
    for (compute::Buffer& t : temps)
        compute::destroy(vk, t);
    for (compute::Buffer& t : geoStaging)
        compute::destroy(vk, t);
    return newPages;
}

/** Applies dabs to the live stroke in batches. Main thread. */
void paintDabs(const Key& key, Layer& l, std::span<const Dab> dabs)
{
    Live& live = *l.live;
    for (size_t i = 0; i < dabs.size(); i += kDabsPerBatch)
    {
        std::span<const Dab> part = dabs.subspan(i, std::min<size_t>(kDabsPerBatch, dabs.size() - i));
        Batch b = prepareBatch(l, live, part);
        gpuRun([&](VulkanContext& vk, Gpu& g) { runBatch(vk, g, key, l, live, b); });
        live.dabs += part.size();
    }
}

/** Paints a fill or a projection in one go. Main thread. */
void paintWhole(const Key& key, Layer& l)
{
    Live& live = *l.live;
    for (size_t v = 0; v < std::max<size_t>(live.views.size(), 1); ++v)
    {
        if (v > 0)
            gpuRun([&](VulkanContext& vk, Gpu&) {
                compute::destroy(vk, live.depth);
                live.depthReady = false;
            });
        live.viewIndex = v;
        Batch b = prepareWhole(l, live);
        gpuRun([&](VulkanContext& vk, Gpu& g) { runBatch(vk, g, key, l, live, b); });
    }
}

// ---------------------------------------------------------------------------------------------------- live batches

/**
 * Live strokes paint off the main thread. liveDabs queues dabs and returns; the paint worker takes everything a buffer
 * has queued, prepares one batch, has the render thread record and launch it, and waits for the GPU itself (the render
 * thread goes back to drawing). The main thread hears of the change afterwards. Anything else that reads or changes
 * paint state drains the queue first (findLayer, layerFor, gpuRun), which keeps batches, cancels and commits in order.
 */
struct LiveQueue
{
    struct Job
    {
        std::shared_ptr<Layer> layer;
        std::vector<Dab> dabs;
    };
    std::mutex mutex;
    std::condition_variable cv;
    std::map<Key, Job> queued;
    bool busy = false;
    bool quit = false;
    int draining = 0;
    /** When the next batch may launch: at most one a frame, so dabs arriving meanwhile join it. */
    std::chrono::steady_clock::time_point nextLaunch{};
    /** Sets whose paint changed that the main thread hasn't told its listeners about yet. */
    std::set<NodeId> changedSets;
    bool tellPosted = false;
    std::thread thread;
};

// Leaked on purpose, like gpu(): a joinable thread destroyed at exit would terminate.
LiveQueue& liveQueue()
{
    static LiveQueue* q = new LiveQueue;
    return *q;
}

thread_local bool tPaintWorker = false;
constexpr auto kBatchInterval = std::chrono::milliseconds(8);

/** Tells the change listeners about sets live batches changed. Main thread. */
void tellListeners()
{
    std::set<NodeId> sets;
    {
        LiveQueue& q = liveQueue();
        std::scoped_lock lock(q.mutex);
        sets.swap(q.changedSets);
        q.tellPosted = false;
    }
    for (NodeId set : sets)
        notify(set);
}

/** Paints a job's dabs (the paint worker). */
void paintQueued(const Key& key, LiveQueue::Job& job)
{
    LILA_ZONE();
    Layer& l = *job.layer;
    if (!l.live)
        return;
    Live& live = *l.live;
    Renderer& r = Renderer::get();
    const std::span<const Dab> dabs(job.dabs);
    for (size_t i = 0; i < dabs.size(); i += kDabsPerBatch)
    {
        std::span<const Dab> part = dabs.subspan(i, std::min<size_t>(kDabsPerBatch, dabs.size() - i));
        Batch b = prepareBatch(l, live, part);
        std::vector<compute::Buffer> temps;
        r.runTask([&](VulkanContext& vk) {
             Gpu& g = gpu();
             g.init(vk);
             runBatch(vk, g, key, l, live, b, &temps);
             return true;
         }).get();
        if (VulkanContext* vk = r.context())
        {
            gpu().batches.waitLaunched(*vk);
            for (compute::Buffer& t : temps)
                compute::destroy(*vk, t);
        }
        live.dabs += part.size();
    }
}

void workerLoop()
{
    LILA_THREAD_NAME("Paint");
    tPaintWorker = true;
    LiveQueue& q = liveQueue();
    std::unique_lock lock(q.mutex);
    for (;;)
    {
        q.cv.wait(lock, [&] { return q.quit || !q.queued.empty(); });
        if (q.quit)
            return;
        q.cv.wait_until(lock, q.nextLaunch, [&] { return q.quit || q.draining > 0; });
        if (q.quit)
            return;
        auto it = q.queued.begin();
        const Key key = it->first;
        LiveQueue::Job job = std::move(it->second);
        q.queued.erase(it);
        q.busy = true;
        q.nextLaunch = std::chrono::steady_clock::now() + kBatchInterval;
        lock.unlock();
        try
        {
            paintQueued(key, job);
        }
        catch (const std::exception& e)
        {
            log::warn("Texture paint: {}", e.what());
        }
        lock.lock();
        q.busy = false;
        q.changedSets.insert(key.set);
        if (!q.tellPosted)
        {
            q.tellPosted = true;
            mainq::post([] { tellListeners(); });
        }
        q.cv.notify_all();
    }
}

/** Waits until every queued live batch has painted. Anywhere but the paint worker. */
void drain()
{
    if (tPaintWorker)
        return;
    LiveQueue& q = liveQueue();
    std::unique_lock lock(q.mutex);
    if (q.queued.empty() && !q.busy)
        return;
    LILA_ZONE();
    ++q.draining;
    q.cv.notify_all();
    q.cv.wait(lock, [&] { return q.queued.empty() && !q.busy; });
    --q.draining;
}

void stopWorker()
{
    LiveQueue& q = liveQueue();
    drain();
    {
        std::scoped_lock lock(q.mutex);
        q.quit = true;
    }
    q.cv.notify_all();
    if (q.thread.joinable())
        q.thread.join();
    std::scoped_lock lock(q.mutex);
    q.quit = false;
}

/** Ends the live stroke: the build-up pages go back; the originals go to undo (returned) or back to the pool. */
std::vector<std::pair<std::pair<TileKey, uint32_t>, CowPage>> finishLive(Layer& l, bool keepOriginals)
{
    std::vector<std::pair<std::pair<TileKey, uint32_t>, CowPage>> out(l.live->cow.begin(), l.live->cow.end());
    std::unique_ptr<Live> live = std::move(l.live);
    gpuRun([&](VulkanContext& vk, Gpu& g) {
        for (auto& [k, slot] : live->acc)
            g.wide.release(slot);
        if (!keepOriginals)
            for (auto& [k, c] : out)
                g.half.release(c.before);
        releaseLive(vk, *live);
        pruneSurfaces(vk, g);
    });
    return out;
}

void cancel(const Key& key, Layer& l)
{
    if (!l.live)
        return;
    std::unique_ptr<Live> live = std::move(l.live);
    gpuRun([&](VulkanContext& vk, Gpu& g) {
        for (auto& [k, c] : live->cow)
        {
            setSlot(l, k.first, k.second, c.before);
            g.half.release(c.after);
        }
        for (auto& [k, slot] : live->acc)
            g.wide.release(slot);
        releaseLive(vk, *live);
        resolve(vk, g, key, l);
    });
}

std::shared_ptr<const PaintImage> imageOf(const scene::Node* set, const Stroke& s)
{
    if (s.payload.image.empty() || !set)
        return nullptr;
    PaintPtr data = readPaint(*set);
    auto it = data->images.find(s.payload.image);
    return it == data->images.end() ? nullptr : it->second;
}

/** An image a stroke refers to by key: the set's, else one the paint tool holds for a stroke not yet stored. */
std::shared_ptr<const PaintImage> keyedImage(const scene::Node* set, const std::string& key)
{
    if (key.empty())
        return nullptr;
    if (set)
    {
        PaintPtr data = readPaint(*set);
        if (auto it = data->images.find(key); it != data->images.end())
            return it->second;
    }
    return heldImage(key);
}

std::shared_ptr<const PaintImage> stencilOf(const scene::Node* set, const Stroke& s)
{
    return s.stencil ? keyedImage(set, s.stencil->image) : nullptr;
}

std::shared_ptr<const PaintImage> limitOf(const scene::Node* set, const Stroke& s) { return keyedImage(set, s.payload.limit); }

/**
 * The stroke on a mesh whose triangles or uvs changed since it was painted: each dab moved to the closest surface point to
 * its anchor; dabs with none within their radius drop out. A fill over the whole mesh fills the new one; any other fill
 * keeps only what its anchors say, which a fill has none of, so it drops out and is counted.
 */
StrokePtr retargetStroke(const StrokePtr& s, const PaintMesh& mesh, RetargetCount& n)
{
    auto out = std::make_shared<Stroke>(*s);
    out->dabs.clear();
    out->anchors.clear();
    for (size_t i = 0; i < s->dabs.size() && i < s->anchors.size(); ++i)
    {
        const Dab& d = s->dabs[i];
        geo::BvhClosest hit;
        if (!mesh.bvh->closestPoint(glm::dvec3(s->anchors[i]), hit, std::max(double(d.radius), 1e-6)) || hit.triangle >= mesh.triangleCount() ||
            !mesh.paintable[hit.triangle])
        {
            ++n.lost;
            continue;
        }
        Dab r = d;
        r.triangle = uint32_t(hit.triangle);
        r.u = float(hit.bary.y);
        r.v = float(hit.bary.z);
        out->dabs.push_back(r);
        out->anchors.push_back(s->anchors[i]);
        ++n.found;
    }
    if (s->payload.kind == "fill")
    {
        Payload p = s->payload;
        if (p.fillBy == "mesh")
            p.triangles = mesh.allTriangles();
        else
        {
            n.lost += p.triangles.size();
            p.triangles.clear();
        }
        out->payload = p;
    }
    if (!s->payload.locked.empty())
        out->payload.locked.clear();
    out->meshSignature = mesh.signature;
    return out;
}

/** Paints a whole stroke with no undo (rasterising from records). */
void replay(const Key& key, Layer& l, scene::Graph& g, const std::string& material, StrokePtr s)
{
    std::shared_ptr<const PaintMesh> mesh = paintMesh(g, s->mesh, material);
    if (!mesh)
    {
        log::warn("paint buffer {}: stroke {} paints {}, which isn't a mesh any more; skipped", key.buffer, s->id, s->mesh);
        return;
    }
    l.meshSignatures[s->mesh] = mesh->signature;
    if (s->meshSignature != 0 && s->meshSignature != mesh->signature)
    {
        RetargetCount n;
        s = retargetStroke(s, *mesh, n);
        if (n.lost > 0)
            log::warn("paint buffer {}: stroke {}: {} of {} dabs found no surface on the changed mesh and were left out", key.buffer, s->id, n.lost,
                      n.lost + n.found);
    }
    try
    {
        l.live = makeLive(s, mesh, imageOf(g.node(key.set), *s), stencilOf(g.node(key.set), *s), limitOf(g.node(key.set), *s));
    }
    catch (const std::exception& e)
    {
        log::warn("paint buffer {}: stroke {}: {}; skipped", key.buffer, s->id, e.what());
        return;
    }
    paintDabs(key, l, s->dabs);
    if (wholeStroke(*l.live))
        paintWhole(key, l);
    finishLive(l, false);
}

size_t pagesOf(const Layer& l)
{
    size_t n = 0;
    for (auto& [k, t] : l.tiles)
        n += t.pages.size();
    return n;
}

void freeAll(Layer& l)
{
    if (l.live)
        finishLive(l, false);
    if (l.epoch != gEpoch.load() || l.tiles.empty())
    {
        l.tiles.clear();
        return;
    }
    gpuRun([&](VulkanContext&, Gpu& g) {
        for (auto& [k, t] : l.tiles)
            for (auto& [p, s] : t.pages)
                g.half.release(s);
    });
    for (auto& [k, t] : l.tiles)
    {
        for (auto& [p, s] : t.pages)
            t.dirty.insert(p);
        t.pages.clear();
    }
}

// ---------------------------------------------------------------------------------------------------- baselines

/** Reads pages back: slot -> texels (render thread inside gpuRun). */
std::vector<std::vector<uint8_t>> readSlots(VulkanContext& vk, Gpu& g, const std::vector<uint32_t>& slots)
{
    std::vector<std::vector<uint8_t>> out(slots.size());
    if (slots.empty())
        return out;
    compute::Buffer host = compute::makeBuffer(vk, kHalfPage * slots.size(), 0, compute::Host::Read);
    g.sub.run(vk, [&](VkCommandBuffer cmd) {
        for (size_t i = 0; i < slots.size(); ++i)
        {
            VkBufferCopy r{g.half.offset(slots[i]), kHalfPage * i, kHalfPage};
            vkCmdCopyBuffer(cmd, g.half.buffer(slots[i]), host.buffer, 1, &r);
        }
    });
    vmaInvalidateAllocation(vk.allocator, host.alloc, 0, VK_WHOLE_SIZE);
    const auto* p = static_cast<const uint8_t*>(host.mapped);
    for (size_t i = 0; i < slots.size(); ++i)
        out[i].assign(p + kHalfPage * i, p + kHalfPage * (i + 1));
    compute::destroy(vk, host);
    return out;
}

/** Writes pages of texels into the layer (render thread inside gpuRun). */
void writePages(VulkanContext& vk, Gpu& g, Layer& l, const TileKey& tk, const std::map<uint32_t, std::vector<uint8_t>>& pages)
{
    if (pages.empty())
        return;
    std::vector<uint8_t> all;
    std::vector<std::pair<uint32_t, uint32_t>> slots;
    for (const auto& [p, texels] : pages)
    {
        const uint32_t s = g.half.alloc(vk);
        slots.emplace_back(p, s);
        all.insert(all.end(), texels.begin(), texels.end());
        setSlot(l, tk, p, s);
    }
    compute::Buffer staging = compute::hostBuffer(vk, all.data(), all.size());
    g.sub.run(vk, [&](VkCommandBuffer cmd) {
        for (size_t i = 0; i < slots.size(); ++i)
        {
            VkBufferCopy r{kHalfPage * i, g.half.offset(slots[i].second), kHalfPage};
            vkCmdCopyBuffer(cmd, staging.buffer, g.half.buffer(slots[i].second), 1, &r);
        }
    });
    compute::destroy(vk, staging);
}

/** A baseline tile's texels at another size, bilinear, back as pages (all-zero pages dropped). */
std::map<uint32_t, std::vector<uint8_t>> resample(const Baseline::Tile& t, const std::vector<uint8_t>& raw, int w, int h)
{
    const int sw = std::max(1, t.width), sh = std::max(1, t.height);
    const int spx = (sw + kPageSize - 1) / kPageSize;
    std::vector<float> src(size_t(sw) * sh * 4, 0.0f);
    for (size_t i = 0; i < t.pages.size(); ++i)
    {
        const uint32_t p = t.pages[i];
        const int x0 = int(p % uint32_t(spx)) * kPageSize, y0 = int(p / uint32_t(spx)) * kPageSize;
        const auto* texels = reinterpret_cast<const uint16_t*>(raw.data() + kHalfPage * i);
        for (int y = 0; y < kPageSize && y0 + y < sh; ++y)
            for (int x = 0; x < kPageSize && x0 + x < sw; ++x)
                for (int c = 0; c < 4; ++c)
                    src[(size_t(y0 + y) * sw + (x0 + x)) * 4 + c] = halfToFloat(texels[(size_t(y) * kPageSize + x) * 4 + c]);
    }
    auto at = [&](int x, int y, int c) { return src[(size_t(std::clamp(y, 0, sh - 1)) * sw + std::clamp(x, 0, sw - 1)) * 4 + c]; };
    const int pagesX = (w + kPageSize - 1) / kPageSize, pagesY = (h + kPageSize - 1) / kPageSize;
    std::map<uint32_t, std::vector<uint8_t>> out;
    for (int py = 0; py < pagesY; ++py)
        for (int px = 0; px < pagesX; ++px)
        {
            std::vector<uint8_t> page(kHalfPage, 0);
            auto* texels = reinterpret_cast<uint16_t*>(page.data());
            bool any = false;
            for (int y = 0; y < kPageSize && py * kPageSize + y < h; ++y)
                for (int x = 0; x < kPageSize && px * kPageSize + x < w; ++x)
                {
                    const double sx = (px * kPageSize + x + 0.5) * sw / double(w) - 0.5, sy = (py * kPageSize + y + 0.5) * sh / double(h) - 0.5;
                    const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                    const float fx = float(sx - ix), fy = float(sy - iy);
                    for (int c = 0; c < 4; ++c)
                    {
                        const float v = (at(ix, iy, c) * (1 - fx) + at(ix + 1, iy, c) * fx) * (1 - fy) + (at(ix, iy + 1, c) * (1 - fx) + at(ix + 1, iy + 1, c) * fx) * fy;
                        texels[(size_t(y) * kPageSize + x) * 4 + c] = floatToHalf(v);
                        any = any || v != 0.0f;
                    }
                }
            if (any)
                out[uint32_t(py * pagesX + px)] = std::move(page);
        }
    return out;
}

void loadBaseline(Layer& l, const Baseline& b)
{
    std::map<TileKey, std::map<uint32_t, std::vector<uint8_t>>> all;
    for (const Baseline::Tile& t : b.tiles)
    {
        auto size = l.sizes.find(t.udim);
        if (size == l.sizes.end())
            continue;
        const std::vector<uint8_t> raw = t.raw();
        auto& pages = all[{t.channel, t.udim}];
        if (t.width == size->second.first && t.height == size->second.second)
            for (size_t i = 0; i < t.pages.size(); ++i)
                pages[t.pages[i]].assign(raw.begin() + std::ptrdiff_t(kHalfPage * i), raw.begin() + std::ptrdiff_t(kHalfPage * (i + 1)));
        else
            pages = resample(t, raw, size->second.first, size->second.second);
    }
    gpuRun([&](VulkanContext& vk, Gpu& g) {
        for (auto& [tk, pages] : all)
            writePages(vk, g, l, tk, pages);
    });
}

// ---------------------------------------------------------------------------------------------------- undo

/** A stroke's or a clear's pages before and after; undo swaps them back into the buffer's tables. */
struct PageRecord
{
    Key key;
    uint64_t generation = 0;
    uint64_t epoch = 0;
    struct Entry
    {
        TileKey tile;
        uint32_t page = 0;
        uint32_t before = kNone, after = kNone;
    };
    std::vector<Entry> entries;
    std::vector<uint64_t> appliedBefore, appliedAfter;
    std::shared_ptr<const Baseline> baselineBefore, baselineAfter;
    uint64_t clearsBefore = 0, clearsAfter = 0;
    bool done = true;
};

class PageUndo final : public UndoAction
{
public:
    explicit PageUndo(std::shared_ptr<PageRecord> r) : m(std::move(r)) {}
    ~PageUndo() override
    {
        // The side not in the buffer's tables belongs to this step.
        if (m->epoch != gEpoch.load() || !Renderer::get().running())
            return;
        std::vector<uint32_t> slots;
        for (const PageRecord::Entry& e : m->entries)
            slots.push_back(m->done ? e.before : e.after);
        if (std::all_of(slots.begin(), slots.end(), [](uint32_t s) { return s == kNone; }))
            return;
        try
        {
            gpuRun([&](VulkanContext&, Gpu& g) {
                for (uint32_t s : slots)
                    g.half.release(s);
            });
        }
        catch (const std::exception&)
        {
        }
    }
    void undo() override { swap(false); }
    void redo() override { swap(true); }
    size_t bytes() const override { return 64 + m->entries.size() * (sizeof(PageRecord::Entry) + kHalfPage); }

private:
    void swap(bool forward)
    {
        if (m->done == forward)
            return;
        Layer* l = findLayer(m->key);
        if (!l || l->generation != m->generation)
            return;
        if (l->live)
            cancel(m->key, *l);
        m->done = forward;
        for (const PageRecord::Entry& e : m->entries)
            setSlot(*l, e.tile, e.page, forward ? e.after : e.before);
        l->applied = forward ? m->appliedAfter : m->appliedBefore;
        l->baseline = forward ? m->baselineAfter : m->baselineBefore;
        l->clears = forward ? m->clearsAfter : m->clearsBefore;
        const Key key = m->key;
        gpuRun([&](VulkanContext& vk, Gpu& g) { resolve(vk, g, key, *l); });
        notify(key.set);
    }

    std::shared_ptr<PageRecord> m;
};

// ---------------------------------------------------------------------------------------------------- sync

std::map<int, std::pair<int, int>> wantedSizes(const scene::Node& set, const Buffer* data)
{
    const tex::SetSettings s = tex::readSettings(set);
    std::map<int, std::pair<int, int>> out;
    for (const tex::Tile& t : s.tiles)
        out[t.udim] = tileSize(s, t, data);
    return out;
}

void publishSizes(const Key& key, const Layer& l)
{
    std::scoped_lock lock(gImageMutex);
    for (const auto& [udim, size] : l.sizes)
    {
        gImageSizes[{key.set, key.buffer, udim}] = size;
        for (auto& [k, e] : gImages)
            if (std::get<0>(k) == key.set && std::get<1>(k) == key.buffer && std::get<3>(k) == udim && e.image &&
                (e.image->width() != size.first || e.image->height() != size.second))
                e = {GpuImage::create({size.first, size.second, TextureFormat::RGBA16F, false, false, key.buffer + "." + std::get<2>(k)}), true};
    }
}

std::vector<uint64_t> strokeIds(const Buffer& b)
{
    std::vector<uint64_t> out;
    for (const StrokePtr& s : b.strokes)
        out.push_back(s->id);
    return out;
}

void doRasterize(scene::Graph& g, scene::Node& set, const std::string& id, const Buffer& data)
{
    LILA_ZONE();
    const Key key{set.id(), id};
    Layer& l = layerFor(key);
    freeAll(l);
    l.tiles.clear();
    l.generation = gGeneration++;
    l.sizes = wantedSizes(set, &data);
    publishSizes(key, l);
    l.applied.clear();
    l.baseline = data.baseline;
    l.clears = data.clears;
    if (data.baseline)
        loadBaseline(l, *data.baseline);
    const std::string material = setMaterial(set);
    for (const StrokePtr& s : data.strokes)
    {
        replay(key, l, g, material, s);
        l.applied.push_back(s->id);
    }
    l.synced = true;
    for (const auto& [udim, size] : l.sizes)
        noteChanged(l, udim, 0, 0, size.first, size.second);
    gpuRun([&](VulkanContext& vk, Gpu& gp) {
        // The images may still show pages the rebuild did not paint again: clear them, then write every page.
        for (auto& [tk, t] : l.tiles)
            for (auto& [p, s] : t.pages)
                t.dirty.insert(p);
        std::vector<compute::Buffer> temps;
        gp.sub.run(vk, [&](VkCommandBuffer cmd) {
            std::scoped_lock lock(gImageMutex);
            for (auto& [k, e] : gImages)
                if (std::get<0>(k) == key.set && std::get<1>(k) == key.buffer && e.image && e.image->prepare(vk, cmd, temps))
                {
                    e.image->beginWrite(cmd);
                    const float zero[4] = {0, 0, 0, 0};
                    e.image->recordClear(cmd, zero);
                    e.image->endWrite(cmd);
                }
        });
        for (compute::Buffer& b : temps)
            compute::destroy(vk, b);
        resolve(vk, gp, key, l);
        pruneSurfaces(vk, gp);
    });
    notify(key.set);
}

void syncOne(scene::Graph& g, scene::Node& set, const std::string& id, const Buffer& data)
{
    const Key key{set.id(), id};
    Layer& l = layerFor(key);
    const std::vector<uint64_t> ids = strokeIds(data);
    // A mesh whose triangles or uvs changed since its strokes were rasterised: rasterise again, retargeting them.
    std::map<std::string, uint64_t> signatures;
    {
        const std::string material = setMaterial(set);
        for (const StrokePtr& s : data.strokes)
            if (!signatures.contains(s->mesh))
            {
                std::shared_ptr<const PaintMesh> pm = paintMesh(g, s->mesh, material);
                signatures[s->mesh] = pm ? pm->signature : 0;
            }
    }
    bool meshesSame = true;
    for (const auto& [mesh, sig] : signatures)
        if (auto it = l.meshSignatures.find(mesh); it != l.meshSignatures.end() && it->second != sig)
            meshesSame = false;
    for (const auto& [mesh, sig] : signatures)
        l.meshSignatures[mesh] = sig;
    const bool same = l.synced && meshesSame && l.sizes == wantedSizes(set, &data) && l.baseline == data.baseline && l.clears == data.clears;
    if (same && l.applied == ids)
        return;
    if (l.live)
        cancel(key, l);
    if (same && l.applied.size() < ids.size() && std::equal(l.applied.begin(), l.applied.end(), ids.begin()))
    {
        const std::string material = setMaterial(set);
        for (size_t i = l.applied.size(); i < ids.size(); ++i)
        {
            replay(key, l, g, material, data.strokes[i]);
            l.applied.push_back(ids[i]);
        }
        gpuRun([&](VulkanContext& vk, Gpu& gp) { resolve(vk, gp, key, l); });
        notify(key.set);
        return;
    }
    doRasterize(g, set, id, data);
}

void dropLayer(const Key& key)
{
    drain();
    auto it = gLayers.find(key);
    if (it == gLayers.end())
        return;
    std::shared_ptr<Layer> l = it->second;
    gLayers.erase(it);
    if (Renderer::get().running())
        try
        {
            freeAll(*l);
            gpuRun([&](VulkanContext& vk, Gpu&) { dropViews(vk, key.set, key.buffer); });
        }
        catch (const std::exception& e)
        {
            log::warn("paint buffer {}: {}", key.buffer, e.what());
        }
    std::scoped_lock lock(gImageMutex);
    std::erase_if(gImages, [&](const auto& kv) { return std::get<0>(kv.first) == key.set && std::get<1>(kv.first) == key.buffer; });
    std::erase_if(gImageSizes, [&](const auto& kv) { return std::get<0>(kv.first) == key.set && std::get<1>(kv.first) == key.buffer; });
}

void dropSet(NodeId set)
{
    std::vector<Key> keys;
    for (auto& [k, l] : gLayers)
        if (k.set == set)
            keys.push_back(k);
    for (const Key& k : keys)
        dropLayer(k);
}

std::set<NodeId> gSyncQueued;

/** After the current command or undo step has finished: bring the set's pages in line with its data. */
void queueSync(NodeId set)
{
    if (!gSyncQueued.insert(set).second)
        return;
    mainq::post([set] {
        gSyncQueued.erase(set);
        scene::Graph& g = scene::Graph::current();
        scene::Node* n = g.node(set);
        if (!n || n->type().name != tex::kTextureSetType)
        {
            dropSet(set);
            return;
        }
        try
        {
            sync(g, *n);
        }
        catch (const std::exception& e)
        {
            log::warn("texture set {}: paint: {}", n->name(), e.what());
        }
    });
}

void onSceneChange(scene::Change c, NodeId id)
{
    switch (c)
    {
    case scene::Change::Value:
    case scene::Change::Created:
        if (scene::Node* n = scene::Graph::current().node(id); n && n->type().name == tex::kTextureSetType)
        {
            bool known = false;
            for (auto& [k, l] : gLayers)
                known = known || k.set == id;
            if (known || readPaint(*n)->buffers.size())
                queueSync(id);
        }
        break;
    case scene::Change::Deleted: dropSet(id); break;
    case scene::Change::Cleared:
    case scene::Change::Reset:
    {
        std::set<NodeId> sets;
        for (auto& [k, l] : gLayers)
            sets.insert(k.set);
        scene::Graph& g = scene::Graph::current();
        for (NodeId s : sets)
        {
            scene::Node* n = g.node(s);
            if (!n || n->type().name != tex::kTextureSetType)
                dropSet(s);
            else
                queueSync(s);
        }
        break;
    }
    default: break;
    }
}
} // namespace

StrokePtr retarget(const StrokePtr& s, const PaintMesh& mesh, RetargetCount& n) { return retargetStroke(s, mesh, n); }

// ---------------------------------------------------------------------------------------------------- public

std::string setMaterial(const scene::Node& set)
{
    std::string key;
    for (const std::string& m : tex::materialsUsing(scene::Graph::current(), set.name()))
        key += (key.empty() ? "" : std::string(1, tex::paint::kMaterialSeparator)) + m;
    return key;
}

bool usesSet(const std::vector<std::string>& materialNames, const scene::Node& set)
{
    const std::vector<std::string> users = tex::materialsUsing(scene::Graph::current(), set.name());
    return std::ranges::any_of(materialNames, [&](const std::string& m) { return std::ranges::find(users, m) != users.end(); });
}

scene::MeshPtr paintSurface(const scene::Node& meshNode)
{
    const std::shared_ptr<sculpt::Session> s = sculpt::active();
    if (!s || s->node != meshNode.id())
        return meshNode.as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    // One copy per sculpt shape, so the paint mesh and its bvh rebuild only after the sculpt moved.
    static std::weak_ptr<sculpt::Session> session;
    static uint64_t version = 0;
    static scene::MeshPtr copy;
    if (!copy || session.lock() != s || version != s->sculpt.version())
    {
        copy = std::make_shared<const geo::Mesh>(s->sculpt.mesh());
        session = s;
        version = s->sculpt.version();
    }
    return copy;
}

std::shared_ptr<const PaintMesh> paintMesh(scene::Graph& g, const std::string& meshPath, const std::string& material)
{
    g.evaluate();
    scene::Node* n = g.find(meshPath);
    if (!n || !n->type().isA("mesh"))
        return nullptr;
    scene::MeshPtr mesh = paintSurface(*n);
    if (!mesh || !mesh->numFaces())
        return nullptr;
    MeshEntry& e = gMeshes[meshPath];
    if (e.mesh != mesh)
    {
        e.mesh = mesh;
        e.bvh = std::make_shared<const geo::MeshBvh>(*mesh);
        e.paint.clear();
    }
    std::shared_ptr<const PaintMesh>& p = e.paint[material];
    if (!p)
        p = PaintMesh::build(*mesh, e.bvh, material);
    return p;
}

void prewarmTarget(scene::Graph& g, scene::Node& set, const std::string& meshPath)
{
    LILA_ZONE();
    g.evaluate();
    scene::Node* n = g.find(meshPath);
    if (!n || !n->type().isA("mesh"))
        return;
    scene::MeshPtr mesh = paintSurface(*n);
    if (!mesh || !mesh->numFaces())
        return;
    const std::string material = setMaterial(set);
    const std::map<int, std::pair<int, int>> sizes = wantedSizes(set, nullptr);
    std::shared_ptr<const geo::MeshBvh> bvh;
    std::map<std::string, std::shared_ptr<const PaintMesh>> have;
    if (auto it = gMeshes.find(meshPath); it != gMeshes.end() && it->second.mesh == mesh)
    {
        bvh = it->second.bvh;
        have = it->second.paint;
    }
    auto surfaced = [&](const std::shared_ptr<const PaintMesh>& p) {
        std::scoped_lock lock(gSurfaceMutex);
        for (const auto& [udim, size] : sizes)
            if (!gSurfaces.contains(std::make_tuple(p.get(), udim, size.first, size.second)))
                return false;
        return true;
    };
    if (bvh && have.contains("") && have.contains(material) && surfaced(have[material]))
        return;
    // One warm-up per mesh version at a time.
    static std::set<const void*> inFlight;
    if (!inFlight.insert(mesh.get()).second)
        return;
    std::thread([mesh, meshPath, material, sizes, bvh, have]() mutable {
        LILA_ZONE_N("paint target warm-up");
        struct Built
        {
            std::shared_ptr<const geo::MeshBvh> bvh;
            std::map<std::string, std::shared_ptr<const PaintMesh>> paint;
            std::vector<std::tuple<int, int, int, std::shared_ptr<Surface>>> surfaces;
        };
        auto out = std::make_shared<Built>();
        try
        {
            out->bvh = bvh ? bvh : std::make_shared<const geo::MeshBvh>(*mesh);
            for (const std::string& m : {std::string(), material})
                if (!out->paint.contains(m))
                    out->paint[m] = have.contains(m) ? have[m] : PaintMesh::build(*mesh, out->bvh, m);
            for (const auto& [udim, size] : sizes)
            {
                auto sf = std::make_shared<Surface>();
                sf->mesh = out->paint[material];
                sf->atlas = std::make_unique<SurfaceAtlas>(sf->mesh, udim, size.first, size.second);
                out->surfaces.emplace_back(udim, size.first, size.second, std::move(sf));
            }
        }
        catch (const std::exception& e)
        {
            log::debug("paint target warm-up: {}", e.what());
        }
        mainq::post([mesh, meshPath, material, out] {
            inFlight.erase(mesh.get());
            if (!out->bvh)
                return;
            MeshEntry& e = gMeshes[meshPath];
            // A newer mesh or a build the stroke already made wins over the warm-up.
            if (e.mesh && e.mesh != mesh)
                return;
            if (!e.mesh)
            {
                e.mesh = mesh;
                e.bvh = out->bvh;
            }
            for (auto& [m, p] : out->paint)
                if (!e.paint[m])
                    e.paint[m] = p;
            // Atlases only for the paint mesh the cache kept, which is the one strokes will use.
            std::scoped_lock lock(gSurfaceMutex);
            for (auto& [udim, w, h, sf] : out->surfaces)
                if (e.paint[material] == sf->mesh)
                    gSurfaces.try_emplace(std::make_tuple(sf->mesh.get(), udim, w, h), sf);
        });
    }).detach();
}

void sync(scene::Graph& g, scene::Node& set)
{
    LILA_ZONE();
    PaintPtr data = readPaint(set);
    std::vector<Key> stale;
    for (auto& [k, l] : gLayers)
        if (k.set == set.id() && !data->buffers.contains(k.buffer))
            stale.push_back(k);
    for (const Key& k : stale)
        dropLayer(k);
    for (const auto& [id, buf] : data->buffers)
        syncOne(g, set, id, buf);
}

void syncBuffer(scene::Graph& g, scene::Node& set, const std::string& buffer)
{
    PaintPtr data = readPaint(set);
    if (const Buffer* b = data->buffer(buffer))
        syncOne(g, set, buffer, *b);
    else
    {
        // A buffer the data doesn't have yet: empty, at the set's size.
        const Key key{set.id(), buffer};
        Layer& l = layerFor(key);
        // Pages of a live stroke are expected; anything else came from data this buffer no longer has.
        if (!l.synced || !l.applied.empty() || l.baseline || (!l.live && !l.tiles.empty() && pagesOf(l) > 0))
            doRasterize(g, set, buffer, Buffer{});
    }
}

void rasterize(scene::Graph& g, scene::Node& set, const std::string& buffer)
{
    PaintPtr data = readPaint(set);
    const Buffer* b = data->buffer(buffer);
    doRasterize(g, set, buffer, b ? *b : Buffer{});
}

void prewarm()
{
    Renderer& r = Renderer::get();
    if (!r.running())
        return;
    installStopHook();
    // Not waited on: the driver takes hundreds of milliseconds to build the dab pipeline, which the first stroke would stall on.
    (void)r.runTask([](VulkanContext& vk) {
        try
        {
            Gpu& g = gpu();
            g.init(vk);
            g.pipeline(vk, g.dab, "paintDabCS");
            g.pipeline(vk, g.depth, "paintDepthCS");
        }
        catch (const std::exception& e)
        {
            log::warn("texture painting: {}", e.what());
        }
        return true;
    });
}

void beginLive(scene::Graph& g, scene::Node& set, const std::string& buffer, StrokePtr header)
{
    syncBuffer(g, set, buffer);
    const Key key{set.id(), buffer};
    Layer& l = layerFor(key);
    if (l.live)
        cancel(key, l);
    std::shared_ptr<const PaintMesh> mesh = paintMesh(g, header->mesh, setMaterial(set));
    if (!mesh)
        throw std::runtime_error(header->mesh + " isn't a mesh with faces");
    std::shared_ptr<const PaintImage> stencil = stencilOf(&set, *header), limit = limitOf(&set, *header);
    l.live = makeLive(std::move(header), std::move(mesh), nullptr, std::move(stencil), std::move(limit));
}

void liveDabs(NodeId set, const std::string& buffer, std::span<const Dab> dabs)
{
    const Key key{set, buffer};
    std::shared_ptr<Layer> l = peekLayer(key);
    if (!l || !l->live || dabs.empty())
        return;
    installStopHook();
    LiveQueue& q = liveQueue();
    std::scoped_lock lock(q.mutex);
    if (!q.thread.joinable())
        q.thread = std::thread(workerLoop);
    LiveQueue::Job& job = q.queued[key];
    job.layer = std::move(l);
    job.dabs.insert(job.dabs.end(), dabs.begin(), dabs.end());
    q.cv.notify_all();
}

void settleLive()
{
    drain();
    tellListeners();
}

void cancelLive(NodeId set, const std::string& buffer)
{
    const Key key{set, buffer};
    if (Layer* l = findLayer(key); l && l->live)
    {
        cancel(key, *l);
        notify(set);
    }
}

bool liveActive(NodeId set, const std::string& buffer)
{
    std::shared_ptr<Layer> l = peekLayer({set, buffer});
    return l && l->live;
}

bool anyLive(NodeId set)
{
    for (auto& [k, l] : gLayers)
        if (k.set == set && l->epoch == gEpoch.load() && l->live)
            return true;
    return false;
}

ApplyResult applyStroke(scene::Graph& g, scene::Node& set, const std::string& buffer, const Stroke& stroke, std::shared_ptr<const PaintImage> image)
{
    LILA_ZONE();
    const Key key{set.id(), buffer};
    Layer& l = layerFor(key);
    ApplyResult r;
    const bool adopt = l.live && l.live->header->id == stroke.id && l.live->dabs == stroke.dabs.size();
    if (!adopt)
    {
        if (l.live)
            cancel(key, l);
        auto header = std::make_shared<const Stroke>(stroke);
        std::shared_ptr<const PaintMesh> mesh = paintMesh(g, stroke.mesh, setMaterial(set));
        if (!mesh)
            throw std::runtime_error(stroke.mesh + " isn't a mesh with faces");
        l.live = makeLive(header, std::move(mesh), image ? image : imageOf(&set, stroke), stencilOf(&set, stroke), limitOf(&set, stroke));
        paintDabs(key, l, stroke.dabs);
        if (wholeStroke(*l.live))
            paintWhole(key, l);
    }
    r.dabs = l.live->dabs;
    if (l.live->mesh)
        l.meshSignatures[stroke.mesh] = l.live->mesh->signature;
    auto record = std::make_shared<PageRecord>();
    record->key = key;
    record->generation = l.generation;
    record->epoch = l.epoch;
    record->appliedBefore = l.applied;
    record->baselineBefore = record->baselineAfter = l.baseline;
    record->clearsBefore = record->clearsAfter = l.clears;
    for (auto& [k, c] : finishLive(l, true))
        record->entries.push_back({k.first, k.second, c.before, c.after});
    r.pages = record->entries.size();
    l.applied.push_back(stroke.id);
    record->appliedAfter = l.applied;
    UndoStack::get().record(std::make_unique<PageUndo>(record));
    notify(set.id());
    return r;
}

size_t clearPages(NodeId set, const std::string& buffer)
{
    const Key key{set, buffer};
    Layer* l = findLayer(key);
    if (!l)
        return 0;
    if (l->live)
        cancel(key, *l);
    auto record = std::make_shared<PageRecord>();
    record->key = key;
    record->generation = l->generation;
    record->epoch = l->epoch;
    record->appliedBefore = l->applied;
    record->baselineBefore = l->baseline;
    record->clearsBefore = l->clears;
    for (auto& [tk, t] : l->tiles)
        for (auto& [p, s] : t.pages)
            record->entries.push_back({tk, p, s, kNone});
    for (const PageRecord::Entry& e : record->entries)
        setSlot(*l, e.tile, e.page, kNone);
    l->applied.clear();
    l->baseline = nullptr;
    l->clears += 1;
    record->clearsAfter = l->clears;
    gpuRun([&](VulkanContext& vk, Gpu& g) { resolve(vk, g, key, *l); });
    UndoStack::get().record(std::make_unique<PageUndo>(record));
    notify(set);
    return record->entries.size();
}

std::shared_ptr<const Baseline> capture(NodeId set, const std::string& buffer)
{
    Layer* l = findLayer({set, buffer});
    auto b = std::make_shared<Baseline>();
    if (!l)
        return b;
    for (auto& [tk, t] : l->tiles)
    {
        if (t.pages.empty())
            continue;
        Baseline::Tile bt;
        bt.channel = tk.first;
        bt.udim = tk.second;
        std::tie(bt.width, bt.height) = l->sizes.count(tk.second) ? l->sizes.at(tk.second) : std::pair<int, int>{0, 0};
        std::vector<uint32_t> slots;
        for (auto& [p, s] : t.pages)
        {
            bt.pages.push_back(p);
            slots.push_back(s);
        }
        std::vector<std::vector<uint8_t>> texels;
        gpuRun([&](VulkanContext& vk, Gpu& g) { texels = readSlots(vk, g, slots); });
        std::vector<uint8_t> raw;
        raw.reserve(kHalfPage * slots.size());
        for (const auto& tx : texels)
            raw.insert(raw.end(), tx.begin(), tx.end());
        bt.data = compressPages(raw);
        b->tiles.push_back(std::move(bt));
    }
    return b;
}

void adopt(NodeId set, const std::string& buffer, const Buffer& data)
{
    if (Layer* l = findLayer({set, buffer}))
    {
        l->applied = strokeIds(data);
        l->baseline = data.baseline;
        l->clears = data.clears;
    }
}

image::Image readChannel(NodeId set, const std::string& buffer, const std::string& channel, int udim)
{
    Layer* l = findLayer({set, buffer});
    if (!l || !l->sizes.count(udim))
        return {};
    const auto [w, h] = l->sizes.at(udim);
    image::Image img{w, h, std::vector<float>(size_t(w) * h * 4, 0.0f)};
    auto it = l->tiles.find({channel, udim});
    if (it == l->tiles.end())
        return img;
    std::vector<uint32_t> pages, slots;
    for (auto& [p, s] : it->second.pages)
    {
        pages.push_back(p);
        slots.push_back(s);
    }
    std::vector<std::vector<uint8_t>> texels;
    gpuRun([&](VulkanContext& vk, Gpu& g) { texels = readSlots(vk, g, slots); });
    const int pagesX = (w + kPageSize - 1) / kPageSize;
    for (size_t i = 0; i < pages.size(); ++i)
    {
        const int x0 = int(pages[i] % uint32_t(pagesX)) * kPageSize, y0 = int(pages[i] / uint32_t(pagesX)) * kPageSize;
        const auto* t = reinterpret_cast<const uint16_t*>(texels[i].data());
        for (int y = 0; y < kPageSize && y0 + y < h; ++y)
            for (int x = 0; x < kPageSize && x0 + x < w; ++x)
                for (int c = 0; c < 4; ++c)
                    img.rgba[(size_t(y0 + y) * w + (x0 + x)) * 4 + c] = halfToFloat(t[(size_t(y) * kPageSize + x) * 4 + c]);
    }
    return img;
}

std::map<std::string, std::vector<uint8_t>> pageBytes(NodeId set, const std::string& buffer)
{
    std::map<std::string, std::vector<uint8_t>> out;
    Layer* l = findLayer({set, buffer});
    if (!l)
        return out;
    std::vector<std::string> names;
    std::vector<uint32_t> slots;
    for (auto& [tk, t] : l->tiles)
        for (auto& [p, s] : t.pages)
        {
            names.push_back(tk.first + "/" + std::to_string(tk.second) + "/" + std::to_string(p));
            slots.push_back(s);
        }
    std::vector<std::vector<uint8_t>> texels;
    gpuRun([&](VulkanContext& vk, Gpu& g) { texels = readSlots(vk, g, slots); });
    for (size_t i = 0; i < names.size(); ++i)
        out[names[i]] = std::move(texels[i]);
    return out;
}

json bufferInfo(NodeId set, const std::string& buffer)
{
    Layer* l = findLayer({set, buffer});
    if (!l)
        return {{"pages", 0}, {"channels", json::object()}, {"strokesApplied", 0}, {"live", false}};
    json channels = json::object();
    size_t pages = 0;
    for (auto& [tk, t] : l->tiles)
    {
        if (t.pages.empty())
            continue;
        channels[tk.first][std::to_string(tk.second)] = t.pages.size();
        pages += t.pages.size();
    }
    json sizes = json::object();
    for (auto& [udim, s] : l->sizes)
        sizes[std::to_string(udim)] = {s.first, s.second};
    return {{"pages", pages}, {"channels", channels}, {"sizes", sizes}, {"strokesApplied", l->applied.size()}, {"live", bool(l->live)},
            {"bytes", pages * kHalfPage}};
}

size_t pagesInUse()
{
    size_t n = 0;
    for (auto& [k, l] : gLayers)
        if (l->epoch == gEpoch.load())
            for (auto& [tk, t] : l->tiles)
                n += t.pages.size();
    return n;
}

PageView pageView(NodeId set, const std::string& buffer, const std::string& channel, int udim)
{
    if (auto it = gViews.find({set, buffer, channel, udim}); it != gViews.end())
        return {it->second.table.address, it->second.width, it->second.height};
    if (auto it = gViewSizes.find({set, buffer, udim}); it != gViewSizes.end())
        return {0, it->second.first, it->second.second};
    return {};
}

void recordGather(VulkanContext& vk, VkCommandBuffer cmd, const PageView& view, int levelWidth, int levelHeight, const std::array<int, 4>& rect,
                  VkDeviceAddress dst, uint32_t dstStride)
{
    const int w = rect[2] - rect[0], h = rect[3] - rect[1];
    if (w <= 0 || h <= 0 || !view.table)
        return;
    Gpu& g = gpu();
    g.init(vk);
    const uint64_t items = uint64_t(w) * uint64_t(h);
    const uint64_t groups = std::max<uint64_t>((items + 63) / 64, 1);
    const uint32_t gx = uint32_t(std::min<uint64_t>(groups, 65535));
    GatherPush push{view.table,
                    dst,
                    rect[0],
                    rect[1],
                    uint32_t(w),
                    uint32_t(h),
                    uint32_t(view.width),
                    uint32_t(view.height),
                    uint32_t(std::max(levelWidth, 1)),
                    uint32_t(std::max(levelHeight, 1)),
                    uint32_t((view.width + kPageSize - 1) / kPageSize),
                    dstStride,
                    gx,
                    0};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipeline(vk, g.gather, "paintGatherCS"));
    vkCmdPushConstants(cmd, g.gather.layout, kCompute, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, gx, uint32_t((groups + gx - 1) / gx), 1);
}

std::map<int, std::array<int, 4>> takeChanges(NodeId set)
{
    std::map<int, std::array<int, 4>> out;
    std::scoped_lock lock(gChangedMutex);
    for (auto& [k, l] : gLayers)
    {
        if (k.set != set)
            continue;
        for (auto& [udim, r] : l->changed)
        {
            auto [it, fresh] = out.try_emplace(udim, r);
            if (!fresh)
                it->second = {std::min(it->second[0], r[0]), std::min(it->second[1], r[1]), std::max(it->second[2], r[2]), std::max(it->second[3], r[3])};
        }
        l->changed.clear();
    }
    return out;
}

GpuImageRef externalImage(NodeId set, const std::string& name, int udim)
{
    std::string buffer, channel;
    if (!parseExternalName(name, &buffer, &channel))
        return nullptr;
    std::scoped_lock lock(gImageMutex);
    auto& e = gImages[{set, buffer, channel, udim}];
    if (e.image)
        return e.image;
    auto size = gImageSizes.find({set, buffer, udim});
    if (size == gImageSizes.end())
    {
        // Not rasterised yet: a blank stand-in until the main thread has synced the set (which re-evaluates it).
        gImages.erase({set, buffer, channel, udim});
        mainq::post([set] {
            if (gCommandsRegistered)
                queueSync(set);
        });
        GpuImageRef standIn = GpuImage::create({1, 1, TextureFormat::RGBA16F, false, false, name});
        // A channel never painted covers nothing; a mask never painted masks nothing.
        if (channel == kMaskChannel)
        {
            const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            standIn->updateRgba(0, 0, 1, 1, white);
        }
        return standIn;
    }
    e = {GpuImage::create({size->second.first, size->second.second, TextureFormat::RGBA16F, false, false, name}), true};
    // The new image starts blank: fill it from the pages, then have the graph read it again.
    mainq::post([set, buffer] {
        Layer* l = findLayer({set, buffer});
        if (!l)
            return;
        try
        {
            gpuRun([&](VulkanContext& vk, Gpu& g) { resolve(vk, g, {set, buffer}, *l); });
        }
        catch (const std::exception& ex)
        {
            log::warn("paint buffer {}: {}", buffer, ex.what());
            return;
        }
        notify(set);
    });
    return e.image;
}

namespace
{
/** Dispatch shape for one thread per item over 64-wide groups laid out in 2D. */
std::pair<uint32_t, uint32_t> linearGroups(uint64_t items)
{
    const uint64_t groups = std::max<uint64_t>((items + 63) / 64, 1);
    const uint32_t gx = uint32_t(std::min<uint64_t>(groups, 65535));
    return {gx, uint32_t((groups + gx - 1) / gx)};
}
} // namespace

std::optional<std::vector<Dab>> withParticlesGpu(const std::shared_ptr<const PaintMesh>& mesh, std::span<const Dab> dabs, uint32_t firstIndex,
                                                 const tex::paint::ParticleSpec& spec)
{
    LILA_ZONE();
    Renderer& r = Renderer::get();
    if (!r.running() || !mesh || !mesh->bvh || mesh->bvh->empty())
        return std::nullopt;
    std::vector<Dab> out;
    if (dabs.empty())
        return out;
    const uint32_t perDab = uint32_t(std::max(spec.count, 0));
    if (perDab == 0)
        return std::vector<Dab>(dabs.begin(), dabs.end());
    const bool flow = spec.kind != "drips" && spec.kind != "rain" && spec.kind != "sparks";
    // A walk never takes more steps than its longest travel over its shortest step (2% of the brush radius).
    const uint32_t steps = std::min<uint32_t>(flow ? 1024u : 512u, uint32_t(std::ceil(double(spec.length) * (flow ? 50.0 : 75.0))) + 1u);
    const uint32_t maxOut = steps + 2u;
    const ParticleSpecGpu specGpu{spec.length, spec.size, spec.spread, spec.rate, spec.gravity, spec.friction, spec.wander, spec.pool, spec.fade};
    const uint32_t kind = spec.kind == "drips" ? 0u : spec.kind == "rain" ? 1u : spec.kind == "sparks" ? 2u : 3u;
    const uint32_t emitter = spec.emitter == "dab" ? 0u : spec.emitter == "ring" ? 2u : 1u;
    // Dabs per dispatch, so the output slots stay within about 64 MB.
    const size_t chunk = std::max<size_t>(1, (size_t(64) << 20) / (size_t(perDab) * maxOut * sizeof(ParticleOutDab)));
    bool ok = true;
    std::vector<ParticleOutDab> slots;
    std::vector<uint32_t> counts;
    for (size_t first = 0; first < dabs.size() && ok; first += chunk)
    {
        const size_t n = std::min(chunk, dabs.size() - first);
        std::vector<ParticleInDab> in(n);
        for (size_t i = 0; i < n; ++i)
        {
            const Dab& d = dabs[first + i];
            in[i] = {d.triangle, d.u, d.v, d.radius, d.opacity, {0, 0, 0}};
        }
        const uint32_t threads = uint32_t(n) * perDab;
        slots.assign(size_t(threads) * maxOut, {});
        counts.assign(threads, 0);
        gpuRun([&](VulkanContext& vk, Gpu& gp) {
            if (!vk.shaderInt64)
            {
                ok = false;
                return;
            }
            const TriBuffer& tb = gp.triangles(vk, mesh);
            const BvhBuffer& bb = gp.bvh(vk, mesh);
            compute::Buffer inBuf = compute::hostBuffer(vk, in.data(), in.size() * sizeof(ParticleInDab));
            compute::Buffer specBuf = compute::hostBuffer(vk, &specGpu, sizeof(specGpu));
            const VkDeviceSize outBytes = VkDeviceSize(slots.size()) * sizeof(ParticleOutDab), countBytes = VkDeviceSize(threads) * 4;
            compute::Buffer outBuf = compute::makeBuffer(vk, outBytes, 0, compute::Host::None);
            compute::Buffer countBuf = compute::makeBuffer(vk, countBytes, 0, compute::Host::None);
            compute::Buffer hostOut = compute::makeBuffer(vk, outBytes, 0, compute::Host::Read);
            compute::Buffer hostCount = compute::makeBuffer(vk, countBytes, 0, compute::Host::Read);
            const auto [gx, gy] = linearGroups(threads);
            ParticlePush push{glm::vec4(spec.down, 0.0f), bb.nodes.address, bb.corners.address, tb.buffer.address, bb.paintable.address,
                              inBuf.address, outBuf.address, countBuf.address, threads, perDab, firstIndex + uint32_t(first), spec.seed, maxOut,
                              uint32_t(mesh->triangleCount()), gx, kind, emitter, 0, specBuf.address};
            gp.sub.run(vk, [&](VkCommandBuffer cmd) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gp.pipeline(vk, gp.particles, "paintParticlesCS"));
                vkCmdPushConstants(cmd, gp.particles.layout, kCompute, 0, sizeof(push), &push);
                vkCmdDispatch(cmd, gx, gy, 1);
                compute::memoryBarrier(cmd);
                compute::copy(cmd, outBuf, hostOut, outBytes);
                compute::copy(cmd, countBuf, hostCount, countBytes);
            });
            vmaInvalidateAllocation(vk.allocator, hostOut.alloc, 0, VK_WHOLE_SIZE);
            vmaInvalidateAllocation(vk.allocator, hostCount.alloc, 0, VK_WHOLE_SIZE);
            std::memcpy(slots.data(), hostOut.mapped, size_t(outBytes));
            std::memcpy(counts.data(), hostCount.mapped, size_t(countBytes));
            for (compute::Buffer* b : {&inBuf, &specBuf, &outBuf, &countBuf, &hostOut, &hostCount})
                compute::destroy(vk, *b);
        });
        if (!ok)
            break;
        // The CPU reference's order: each brush dab, then its particles' dabs one particle after another.
        for (size_t i = 0; i < n; ++i)
        {
            const Dab& from = dabs[first + i];
            out.push_back(from);
            for (uint32_t pi = 0; pi < perDab; ++pi)
            {
                const size_t t = i * perDab + pi;
                for (uint32_t k = 0; k < std::min(counts[t], maxOut); ++k)
                {
                    const ParticleOutDab& s = slots[t * maxOut + k];
                    Dab d;
                    d.triangle = s.triangle;
                    d.u = s.u;
                    d.v = s.v;
                    d.radius = s.radius;
                    d.pressure = 1.0f;
                    d.opacity = s.opacity;
                    d.angle = 0.0f;
                    d.copy = from.copy;
                    out.push_back(d);
                }
            }
        }
    }
    if (!ok)
        return std::nullopt;
    return out;
}

std::vector<Dab> withParticles(const std::shared_ptr<const PaintMesh>& mesh, std::span<const Dab> dabs, uint32_t firstIndex,
                               const tex::paint::ParticleSpec& spec)
{
    try
    {
        if (std::optional<std::vector<Dab>> gpu = withParticlesGpu(mesh, dabs, firstIndex, spec))
            return std::move(*gpu);
    }
    catch (const std::exception& e)
    {
        log::warn("Particle brush: the GPU walk failed ({}); using the CPU", e.what());
    }
    static std::once_flag told;
    std::call_once(told, [] { log::info("Particle brush: walking particles on the CPU (no renderer running)"); });
    return tex::paint::withParticles(*mesh, dabs, firstIndex, spec);
}

image::Image screenUvs(scene::Graph& g, const std::string& meshPath, const StrokeView& view, int width, int height)
{
    LILA_ZONE();
    std::shared_ptr<const PaintMesh> mesh = paintMesh(g, meshPath, "");
    if (!mesh)
        throw std::runtime_error(meshPath + " isn't a mesh with faces");
    width = std::clamp(width, 1, 4096);
    height = std::clamp(height, 1, 4096);
    image::Image out{width, height, std::vector<float>(size_t(width) * size_t(height) * 4, 0.0f)};
    gpuRun([&](VulkanContext& vk, Gpu& gp) {
        const VkDeviceSize px = VkDeviceSize(width) * VkDeviceSize(height);
        compute::Buffer depth = compute::makeBuffer(vk, px * 4, 0, compute::Host::None);
        compute::Buffer vis = compute::makeBuffer(vk, px * 4, 0, compute::Host::None);
        compute::Buffer uv = compute::makeBuffer(vk, px * 16, 0, compute::Host::None);
        compute::Buffer host = compute::makeBuffer(vk, px * 16, 0, compute::Host::Read);
        const TriBuffer& tb = gp.triangles(vk, mesh);
        const uint32_t tris = uint32_t(mesh->triangleCount());
        const auto [tx, ty] = linearGroups(tris);
        const auto [px0, py0] = linearGroups(px);
        DepthPush dpush{view.objectToClip, tb.buffer.address, depth.address, tris, uint32_t(width), uint32_t(height), tx};
        VisPush vpush{view.objectToClip, tb.buffer.address, depth.address, vis.address, tris, uint32_t(width), uint32_t(height), tx, 0};
        UvPush upush{view.objectToClip, tb.buffer.address, vis.address, uv.address, uint32_t(width), uint32_t(height), px0, 0};
        gp.sub.run(vk, [&](VkCommandBuffer cmd) {
            vkCmdFillBuffer(cmd, depth.buffer, 0, VK_WHOLE_SIZE, 0);
            vkCmdFillBuffer(cmd, vis.buffer, 0, VK_WHOLE_SIZE, 0xFFFFFFFFu);
            compute::memoryBarrier(cmd);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gp.pipeline(vk, gp.depth, "paintDepthCS"));
            vkCmdPushConstants(cmd, gp.depth.layout, kCompute, 0, sizeof(dpush), &dpush);
            vkCmdDispatch(cmd, tx, ty, 1);
            compute::memoryBarrier(cmd);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gp.pipeline(vk, gp.vis, "paintVisCS"));
            vkCmdPushConstants(cmd, gp.vis.layout, kCompute, 0, sizeof(vpush), &vpush);
            vkCmdDispatch(cmd, tx, ty, 1);
            compute::memoryBarrier(cmd);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gp.pipeline(vk, gp.uvmap, "paintUvCS"));
            vkCmdPushConstants(cmd, gp.uvmap.layout, kCompute, 0, sizeof(upush), &upush);
            vkCmdDispatch(cmd, px0, py0, 1);
            compute::memoryBarrier(cmd);
            compute::copy(cmd, uv, host, px * 16);
        });
        vmaInvalidateAllocation(vk.allocator, host.alloc, 0, VK_WHOLE_SIZE);
        std::memcpy(out.rgba.data(), host.mapped, size_t(px) * 16);
        for (compute::Buffer* b : {&depth, &vis, &uv, &host})
            compute::destroy(vk, *b);
    });
    return out;
}

std::vector<uint8_t> viewImage(const GpuImageRef& img, const glm::vec4& uvRect, int width, int height, bool colorDisplay, bool keepAlpha)
{
    LILA_ZONE();
    width = std::clamp(width, 1, 8192);
    height = std::clamp(height, 1, 8192);
    std::vector<uint8_t> out(size_t(width) * size_t(height) * 4, 0);
    if (!img)
        return out;
    gpuRun([&](VulkanContext& vk, Gpu& gp) {
        std::vector<compute::Buffer> temps;
        const VkDeviceSize px = VkDeviceSize(width) * VkDeviceSize(height);
        const uint64_t texels = uint64_t(img->width()) * uint64_t(img->height());
        compute::Buffer raw = compute::makeBuffer(vk, (texels * uint64_t(texelBytes(img->format())) + 3) & ~uint64_t(3), 0, compute::Host::None);
        compute::Buffer dst = compute::makeBuffer(vk, px * 4, 0, compute::Host::None);
        compute::Buffer host = compute::makeBuffer(vk, px * 4, 0, compute::Host::Read);
        const auto [gx, gy] = linearGroups(px);
        ViewPush push{raw.address,
                      dst.address,
                      uvRect,
                      uint32_t(img->width()),
                      uint32_t(img->height()),
                      uint32_t(img->format()),
                      usesSrgb(img->format(), img->desc().srgb) ? 1u : 0u,
                      uint32_t(width),
                      uint32_t(height),
                      gx,
                      (colorDisplay ? 1u : 0u) | (keepAlpha ? 2u : 0u)};
        bool ok = false;
        gp.sub.run(vk, [&](VkCommandBuffer cmd) {
            ok = img->prepare(vk, cmd, temps) && img->realized();
            if (ok)
                img->recordRead(cmd, raw);
            else
                vkCmdFillBuffer(cmd, raw.buffer, 0, VK_WHOLE_SIZE, 0);
            compute::memoryBarrier(cmd);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gp.pipeline(vk, gp.view, "paintViewCS"));
            vkCmdPushConstants(cmd, gp.view.layout, kCompute, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, gx, gy, 1);
            compute::memoryBarrier(cmd);
            compute::copy(cmd, dst, host, px * 4);
        });
        vmaInvalidateAllocation(vk.allocator, host.alloc, 0, VK_WHOLE_SIZE);
        std::memcpy(out.data(), host.mapped, out.size());
        for (compute::Buffer* b : {&raw, &dst, &host})
            compute::destroy(vk, *b);
        for (compute::Buffer& b : temps)
            compute::destroy(vk, b);
    });
    return out;
}

GpuImageRef channelImage(NodeId set, const std::string& buffer, const std::string& channel, int udim)
{
    if (!peekLayer({set, buffer}))
        return nullptr;
    return externalImage(set, externalName(buffer, channel), udim);
}

int addChangeListener(std::function<void(NodeId)> fn)
{
    const int id = gNextListener++;
    gListeners[id] = std::move(fn);
    return id;
}

void removeChangeListener(int id) { gListeners.erase(id); }

void shutdown()
{
    stopWorker();
    std::vector<Key> keys;
    for (auto& [k, l] : gLayers)
        keys.push_back(k);
    for (const Key& k : keys)
        dropLayer(k);
    gMeshes.clear();
    if (Renderer::get().running() && !gSurfaces.empty())
        gpuRun([](VulkanContext& vk, Gpu& g) {
            for (auto& [k, s] : gSurfaces)
                for (auto& [p, slot] : s->geo)
                    g.wide.release(slot);
            gSurfaces.clear();
            for (auto& [k, t] : g.tris)
                compute::destroy(vk, t.buffer);
            g.tris.clear();
        });
    gSurfaces.clear();
}

void installSync()
{
    static int listener = 0;
    if (!listener)
        listener = scene::Graph::current().addListener(onSceneChange);
    gCommandsRegistered = true;
}
} // namespace lila::render::paint
