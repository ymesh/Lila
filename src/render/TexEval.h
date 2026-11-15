#pragma once

#include "core/Export.h"
#include "core/ImageIO.h"
#include "render/GpuImage.h"
#include "tex/Evaluate.h"
#include "tex/Plan.h"

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

/**
 * GPU evaluation of texture graph plans (tex/Plan.h has the pass contract): every buffer lives in one float4 arena on
 * the device, fused passes are the plan's Slang compiled to SPIR-V (pipelines shared by Pass::hash), native nodes run
 * through implementations registered here, and outputs land in GpuImageRefs a material samples directly, with no CPU
 * readback. A value edit uploads uniforms only and reruns just the passes downstream of what changed.
 */
namespace lila::render::texgpu
{
/** A buffer a native pass reads or writes: float4 texels at a device address, row 0 at the top. */
struct NativeBuffer
{
    VkDeviceAddress address = 0;
    int width = 0;
    int height = 0;
};

struct NativeCall
{
    const tex::Plan& plan;
    const tex::Pass& pass;
    /** The plan's uniforms with the buffer descriptors filled in. */
    std::span<const float> uniforms;
    std::map<std::string, NativeBuffer> inputs;
    NativeBuffer output;
    /** Device scratch holding at least one float4 per output texel, for multi-pass natives. */
    VkDeviceAddress scratch = 0;

    /** Component c of a parameter's first slot (0 when it has none). */
    float param(const std::string& id, int c = 0) const
    {
        auto it = pass.paramSlots.find(id);
        return it == pass.paramSlots.end() ? 0.0f : uniforms[size_t(it->second) * 4 + size_t(c)];
    }
};

/**
 * Records a native node's work into cmd (render thread). Each dispatch must stay well under the driver's timeout; the
 * evaluator puts a full memory barrier before and after the call, so a multi-pass native only orders its own steps.
 */
using NativeGpuFn = std::function<void(VulkanContext& vk, VkCommandBuffer cmd, const NativeCall& call)>;

/**
 * The GPU side of a native node type (the CPU side is tex::registerNativeCpu). Without one, a native runs its Slang kernels
 * (tex/Natives.h) when it has them, else the built-in blur, else its CPU function on buffers read back between slices.
 */
LILA_UI_API void registerNativeGpu(const std::string& type, NativeGpuFn fn);
LILA_UI_API bool hasNativeGpu(const std::string& type);

/**
 * Mesh maps for External buffers by bake project, map name (position, normal, curvature...) and UDIM tile: the baker's
 * GPU-resident maps install this. Null from the source (or no source) leaves the map to the uv fallback.
 */
using MeshMapSource = std::function<GpuImageRef(const std::string& bakeProject, const std::string& map, int udim)>;
LILA_UI_API void setMeshMapSource(MeshMapSource fn);
LILA_UI_API GpuImageRef meshMap(const std::string& bakeProject, const std::string& map, int udim);
/** Call when a project's mesh maps were rebaked: live texture sets reading them re-evaluate. Any thread. */
LILA_UI_API void meshMapsChanged(const std::string& bakeProject);
/** Hears meshMapsChanged (texture set views). Returns an id for removeMeshMapListener. */
LILA_UI_API int addMeshMapListener(std::function<void(const std::string& bakeProject)> fn);
LILA_UI_API void removeMeshMapListener(int id);

struct Inputs
{
    /** External buffers by map name, read from the GPU (their current contents each time the plan is set). */
    std::map<std::string, GpuImageRef> gpuExternals;
    /** Or given as CPU texels. Externals in neither read as (u, v, 0, 1) at the plan's size. */
    std::map<std::string, image::Image> cpuExternals;
    /** Relative image parameter paths resolve against this (as tex::EvalOptions::imageRoot). */
    std::filesystem::path imageRoot;
    /** The texture's place in uv: a UDIM tile's origin. */
    float uvOffset[2] = {0.0f, 0.0f};
    /**
     * Keep buffers that only fused passes touch as half floats (8 bytes a texel instead of 16), except those holding
     * coordinates or 32-bit maps. For display, where the memory matters more than the last bits; exports leave it off.
     */
    bool halfBuffers = false;
    /**
     * Let buffers share memory once no later pass reads them, so the arena holds only what is live at once. Every later
     * setPlan then reruns the whole plan, so it suits evaluations that run once (evaluate sets it). Plans whose buffers
     * don't fit one GPU buffer side by side are packed like this anyway. Not for pages plans.
     */
    bool compact = false;
    /** The most bytes the buffers may take, when less than one GPU buffer holds (0: what it holds). */
    uint64_t maxArenaBytes = 0;
    /**
     * Evaluate for virtual texture pages (Evaluator::recordPages): the fused passes that make outputs nothing else reads
     * run only per page, over each page's region, and the External buffers only they read at their own uv come in per
     * page through a RegionLoader instead of whole. The rest (intermediate buffers, images, other externals) still runs
     * at the plan's size by step. No stores.
     */
    bool pages = false;
    /** Pages: the largest page region side (with its border); the arena keeps room for a batch of them. */
    int pageSize = 136;
};
/**
 * A viewport image written from plan outputs whenever they change: each image channel takes one component of an
 * output (resampled bilinearly to the image's size, so a preview plan fills a full-size image) or a constant.
 */
struct StoreTarget
{
    struct Source
    {
        /** The plan output name; empty for the constant. */
        std::string output;
        int component = 0;
        float constant = 0.0f;
    };
    GpuImageRef image;
    std::array<Source, 4> channels;
};

/** One virtual texture page for Evaluator::recordPages. */
struct PageJob
{
    /** The region: its first texel and size in texels of a level levelWidth x levelHeight of the tile (it may reach past the edges). */
    int x0 = 0, y0 = 0, width = 0, height = 0;
    int levelWidth = 0, levelHeight = 0;
    /** Per layer, where its texels go: width x height, rows top first, tightly packed in the layer's format. */
    std::vector<VkDeviceAddress> layers;
};

/** A page layer composed from plan outputs, as a StoreTarget composes an image. */
struct PageLayer
{
    std::array<StoreTarget::Source, 4> channels;
    TextureFormat format = TextureFormat::RGBA8;
    bool srgb = false;
};

/**
 * Brings texels rect {x0, y0, x1, y1} (of a tile at levelWidth x levelHeight, clamped at its edges) of an External buffer
 * into the arena for a page: float4 texels, dstStride per row. Render thread, recording into cmd. False when it has
 * nothing under that name (the region then reads as zero).
 */
using RegionLoader = std::function<bool(VulkanContext& vk, VkCommandBuffer cmd, const std::string& name, int levelWidth, int levelHeight,
                                        const std::array<int, 4>& rect, VkDeviceAddress dst, uint32_t dstStride)>;

struct Stats
{
    /** Passes run to completion since the plan was set. */
    size_t passesRun = 0;
    /** Passes the last setPlan found dirty. */
    size_t passesDirty = 0;
    size_t dispatches = 0;
    size_t slices = 0;
    /** Images written. */
    size_t stores = 0;
    /** Fused passes compiled to SPIR-V, and those the cache already had, in setPlan. */
    size_t compiled = 0;
    size_t cached = 0;
    /** Pipelines created on the device (not found in the shared cache). */
    size_t pipelinesCreated = 0;
    double gpuMs = 0.0;
    /** Texels brought in from external images, computed by passes and written to store images since the plan was set. */
    uint64_t texelsLoaded = 0, texelsComputed = 0, texelsStored = 0;
    /** Bytes the plan's buffers take on the GPU (less than their sum when they share memory, Inputs::compact). */
    uint64_t arenaBytes = 0;
    /** Image parameters that could not be read (they evaluate as black). */
    std::vector<std::string> warnings;
};

/**
 * Evaluates one plan at a time on the GPU, keeping its buffers between plans: setting a plan with the same structure
 * and size only uploads what changed and marks the passes reading it (and everything downstream) dirty. Work runs in
 * slices from Renderer::runTask, each recording what fits the budget (fused passes split into row bands) and waiting
 * for it, so a large evaluation never holds a frame up for long.
 *
 * setPlan from any thread but the render thread; step on the render thread only. Device memory goes back once the
 * evaluator is gone (or the renderer stops).
 */
/** Evaluator::step's error when the plan's buffers need more than one GPU buffer holds even packed (Inputs::compact). */
struct ArenaTooLarge : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

class LILA_UI_API Evaluator
{
public:
    Evaluator();
    ~Evaluator();
    Evaluator(const Evaluator&) = delete;
    Evaluator& operator=(const Evaluator&) = delete;

    /**
     * Compiles the plan's new fused passes (blocking) and loads its image parameters, then hands it to the next step,
     * superseding a plan still in progress (its unfinished passes stay dirty). Returns an empty string, or why the
     * plan can't run (it then keeps the previous one).
     */
    std::string setPlan(tex::Plan plan, Inputs inputs = {}, std::vector<StoreTarget> stores = {});

    /**
     * Has the next step reload the GPU externals whose version changed since they were loaded, and rerun only what reads
     * them: a paint stroke's update with no new plan. Any thread but the render thread.
     */
    void refreshExternals();

    /**
     * Gives the current plan new GPU externals (images that appeared, like a paint layer's first stroke, or were
     * replaced) without a new plan: the next step loads the ones that differ and reruns only what reads them. The
     * caller keeps names, sizes and formats as the plan was set with; a later setPlan drops it. Any thread but the
     * render thread.
     */
    void setExternals(std::map<std::string, GpuImageRef> externals);

    /**
     * Render thread: the uv region {x0, y0, x1, y1} (fractions, rows down) the stores wrote since the last call, false
     * when none.
     */
    bool takeStored(std::array<float, 4>& uv);
    /**
     * Render thread: has the next step write the store images over uv again from what the plan computed, for images
     * another evaluator (a preview) wrote over. Reruns in full when trimmed.
     */
    void storeAgain(const std::array<float, 4>& uv);

    /** Render thread: one slice of about budgetMs. True when nothing is left to run. Throws std::runtime_error on GPU failure. */
    bool step(VulkanContext& vk, double budgetMs = 40.0);
    /** Whether every pass of the latest plan has run and its images are written. */
    bool idle() const;
    /**
     * Render thread: frees an idle evaluator's device memory, keeping the plan and the images it wrote. The next plan or
     * refresh then runs in full instead of only what changed. False when busy, a pages plan, or already freed.
     */
    bool trim(VulkanContext& vk);
    Stats stats() const;

    /** Blocks until the renderer copies the outputs back: RGBA floats by output name, rows top first. Not the render thread. */
    std::map<std::string, image::Image> readOutputs();

    /** Render thread, a pages plan (Inputs::pages): whether everything pages read has run (step until it has). */
    bool pagesReady() const;
    /** Pages recordPages takes at most per call. */
    static constexpr size_t kPagesPerBatch = 16;
    /**
     * Render thread, a pages plan: records the page passes over each job's region and composes the layers into the
     * jobs' destinations, into cmd (which the caller submits; no step may run in between). Returns how many leading
     * jobs it recorded (at most kPagesPerBatch); 0 when the plan isn't ready or isn't a pages plan. Throws
     * std::runtime_error when a layer reads an output the plan doesn't have.
     */
    size_t recordPages(VulkanContext& vk, VkCommandBuffer cmd, std::span<const PageJob> jobs, std::span<const PageLayer> layers,
                       const RegionLoader& load, std::vector<compute::Buffer>& temps);

    struct State;

private:
    std::shared_ptr<State> m;
};

/**
 * The External buffers a pages plan (Inputs::pages) needs whole, by name: those read by anything but a page pass at its
 * own uv. The rest come in per page through the RegionLoader.
 */
LILA_UI_API std::set<std::string> wholeExternals(const tex::Plan& plan);

/**
 * Render thread: texels rect {x0, y0, x1, y1} of an image taken as a tile at levelWidth x levelHeight (four samples per
 * texel when that is smaller, clamped at the edges) as float4 texels at dst, dstStride per row. Staging goes to temps.
 * False when the image can't be made on the device.
 */
LILA_UI_API bool recordImageRegion(VulkanContext& vk, VkCommandBuffer cmd, const GpuImageRef& image, int levelWidth, int levelHeight,
                                   const std::array<int, 4>& rect, VkDeviceAddress dst, uint32_t dstStride, std::vector<compute::Buffer>& temps);

/**
 * evaluateCpu's twin on the GPU: runs the plan to completion and reads the outputs back. Starts the renderer when needed.
 * A plan whose buffers don't fit one GPU buffer runs for a share of its outputs at a time. maxArenaBytes caps that
 * buffer below the device's limit (0: no cap). Throws.
 */
LILA_UI_API tex::EvalResult evaluate(const tex::Plan& plan, const tex::EvalOptions& options = {}, uint64_t maxArenaBytes = 0);

/** Fused passes compiled to SPIR-V and cache hits, process-wide. */
LILA_UI_API std::pair<size_t, size_t> spirvCacheStats();

/**
 * Compiles a plan's passes and makes their pipelines into the shared caches, without evaluating: an Evaluator given an
 * equal plan later finds them ready. Blocking, any thread but the render thread. Empty, or why the plan can't run.
 */
LILA_UI_API std::string precompile(const tex::Plan& plan);

/** Makes this the texture graph GPU backend (tex::setGpuEvaluator): texEvaluate and exports use it. Starts the renderer on demand. */
LILA_UI_API void registerTexGpu();
} // namespace lila::render::texgpu
