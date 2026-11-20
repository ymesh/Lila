#include "ui/TexLive.h"

#include "core/Events.h"
#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "ops/Command.h"
#include "render/PaintGpu.h"
#include "render/Renderer.h"
#include "render/TexEval.h"
#include "render/VirtualTexture.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "tex/Layers.h"
#include "tex/Sequence.h"
#include "tex/TextureSet.h"
#include "ui/ViewportTool.h"

#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace lila::ui::texLive
{
namespace
{
using json = nlohmann::json;
using render::GpuImage;
using render::GpuImageRef;
using render::TextureFormat;
using render::texgpu::Evaluator;
using render::texgpu::StoreTarget;

struct TileState
{
    int udim = 1001;
    int width = 0;
    int height = 0;
    std::map<std::string, GpuImageRef> channels;
    GpuImageRef orm;
    /** Preview levels first, the tile's full size last. */
    std::vector<std::shared_ptr<Evaluator>> levels;
    std::vector<std::pair<int, int>> levelSizes;
    /** The full size has finished since the last job that ran the previews: its images are sharp and current. */
    std::shared_ptr<std::atomic<bool>> fullShown = std::make_shared<std::atomic<bool>>(false);
    /** Worker: the graph and settings the last job planned. */
    std::string plannedKey;
    /** Plans each level has been given, for texLiveStatus. */
    std::vector<std::atomic<uint64_t>> plans;
    /** Worker: the GPU externals each level's plan runs with, so an unchanged graph swaps them instead of replanning. */
    std::vector<std::map<std::string, GpuImageRef>> externals;
    /** External updates given without a new plan, per level, for texLiveStatus. */
    std::vector<std::atomic<uint64_t>> swaps;
};

/**
 * Whether a level can take new GPU externals in place: the same names, and each image (or its absence) giving the
 * buffer the size and format the plan was set with, so only the loads and their readers change.
 */
bool externalsSwap(const std::map<std::string, GpuImageRef>& was, const std::map<std::string, GpuImageRef>& now, int width, int height)
{
    // The buffer size setPlan gives an external: the level's, or the image's own when it is smaller.
    auto planned = [&](const GpuImageRef& img) {
        return !img || (img->width() >= width && img->height() >= height) ? std::pair{width, height} : std::pair{img->width(), img->height()};
    };
    for (const auto& [name, img] : now)
    {
        auto it = was.find(name);
        const GpuImageRef old = it == was.end() ? nullptr : it->second;
        if (planned(img) != planned(old))
            return false;
        // A missing external planned as a float uv fill; a present one keeps its format so its half or float buffer holds.
        if ((old && img && old->format() != img->format()) || (old && !img))
            return false;
    }
    for (const auto& [name, img] : was)
        if (img && !now.contains(name))
            return false;
    return true;
}

using Tiles = std::vector<std::shared_ptr<TileState>>;

class SetPages;

struct LiveSet
{
    scene::NodeId node = scene::kNoNode;
    std::string name;
    std::string material;
    /** What the images and evaluators were made for: tiles, sizes, channels, levels. */
    std::string configKey;
    tex::SetSettings settings;
    Tiles tiles;
    Maps maps;
    /** A level has written the current images; until then the material keeps its own maps rather than showing black. */
    bool ready = false;
    std::string error;
    /** Shown through a virtual texture instead of whole images: its pages and what makes them. */
    std::shared_ptr<render::vt::Texture> virtualTexture;
    std::shared_ptr<SetPages> pages;
    /** Displacement preview asked a virtual set for its height: tiles then holds a capped, height-only copy of it. */
    bool wantsHeight = false;
    /** The graph changes with time (an Image Sequence node, a keyed parameter): replanned on every time change. */
    bool animated = false;
};

struct Job
{
    scene::NodeId node = scene::kNoNode;
    tex::Graph graph;
    tex::SetSettings settings;
    Tiles tiles;
    std::shared_ptr<SetPages> pages;
};

/** An evaluator the render task steps, lower levels (previews) first. */
struct Entry
{
    int level = 0;
    uint64_t seq = 0;
    scene::NodeId node = scene::kNoNode;
    std::shared_ptr<Evaluator> ev;
    bool failed = false;
    /** A preview's tile at full size, null for the full size itself. */
    std::shared_ptr<Evaluator> full;
    /** The full size's TileState::fullShown, null for a preview. */
    std::shared_ptr<std::atomic<bool>> shown;
    std::chrono::steady_clock::time_point ran = std::chrono::steady_clock::now();
};

// Main thread.
std::map<scene::NodeId, std::shared_ptr<LiveSet>> gSets;
std::map<std::string, scene::NodeId> gIndex;
bool gIndexDirty = true;
bool gInstalled = false;
int gGraphListener = 0;
int gMeshListener = 0;
int gPaintListener = 0;
int gPrefListener = 0;
events::SubscriptionId gShutdownSub = 0;
events::SubscriptionId gTimeSub = 0;

// Worker.
std::mutex gJobMutex;
std::condition_variable gJobCv;
std::map<scene::NodeId, Job> gJobs; // newest per set
/** Graphs a set is about to have, compiled ahead when no real job waits; newest per set. */
std::map<scene::NodeId, Job> gWarm;
/** Warm jobs finished, per set, for status(). */
std::map<scene::NodeId, uint64_t> gWarmed;
bool gJobBusy = false;
bool gJobQuit = false;
std::thread gWorker;

/** At exit without shutdown(): let the worker go rather than join it from a static destructor. */
struct WorkerGuard
{
    ~WorkerGuard()
    {
        if (!gWorker.joinable())
            return;
        {
            std::scoped_lock lock(gJobMutex);
            gJobQuit = true;
        }
        gJobCv.notify_all();
        gWorker.detach();
    }
} gWorkerGuard;

/** When paint last landed on any set, steady clock ns: slices stay frame sized while a stroke goes on. */
std::atomic<int64_t> gPaintedAt{0};

/** A slice's GPU budget, ms: short while paint is landing, so the viewport keeps drawing between slices. */
double sliceBudgetMs()
{
    const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
    return now - gPaintedAt.load() < std::chrono::nanoseconds(std::chrono::milliseconds(500)).count() ? 4.0 : 40.0;
}

// Render task.
std::mutex gTaskMutex;
std::vector<Entry> gEntries;
bool gTaskQueued = false;
uint64_t gSeq = 0;

bool enabled() { return PrefStore::get().value<bool>("general", "/texture/liveViewport", true); }

std::vector<int> previewLevels()
{
    std::vector<int> out;
    json v = PrefStore::get().value("general", "/texture/previewLevels", json::array({256, 1024}));
    if (v.is_array())
        for (const json& x : v)
            if (x.is_number_integer() && x.get<int>() > 0)
                out.push_back(x.get<int>());
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::pair<int, int>> levelSizes(int w, int h, const std::vector<int>& previews)
{
    std::vector<std::pair<int, int>> out;
    for (int l : previews)
        if (l < std::max(w, h))
        {
            const double s = double(l) / double(std::max(w, h));
            out.emplace_back(std::max(1, int(std::lround(w * s))), std::max(1, int(std::lround(h * s))));
        }
    out.emplace_back(w, h);
    return out;
}

/** The longest side of a virtual set's height copy for displacement preview, per tile. */
int heightPreviewSize()
{
    return std::clamp(PrefStore::get().value<int>("general", "/texture/displacementPreviewSize", 4096), 256, 16384);
}

/** A tile's size scaled so its longest side is at most cap. */
std::pair<int, int> capped(std::pair<int, int> size, int cap)
{
    const double s = std::min(1.0, double(cap) / double(std::max(size.first, size.second)));
    return {std::max(1, int(std::lround(size.first * s))), std::max(1, int(std::lround(size.second * s)))};
}

bool colorData(const std::string& channel) { return channel == "baseColor" || channel == "emissive"; }

TextureFormat channelFormat(const tex::Channel& c)
{
    const bool gray = c.type == tex::PortType::Gray || c.type == tex::PortType::Float || c.type == tex::PortType::Sdf2d;
    if (gray)
        return c.bits >= 32 ? TextureFormat::R32F : c.bits >= 16 ? TextureFormat::R16F : TextureFormat::R8;
    return c.bits >= 16 ? TextureFormat::RGBA16F : TextureFormat::RGBA8;
}

std::string joinMaterials(const std::vector<std::string>& names)
{
    std::string out;
    for (const std::string& n : names)
        out += (out.empty() ? "" : ", ") + n;
    return out;
}

/** The set the viewport draws a material's occlusion, roughness and metallic from: one packed texture, so one set. */
std::string packedSetFor(const scene::Node& material)
{
    const std::map<std::string, std::string> picks = scene::materials::textureSetMaps(material);
    if (auto it = picks.find("orm"); it != picks.end())
        return it->second;
    std::vector<std::string> sets;
    for (const char* c : {"ambientOcclusion", "roughness", "metallic"})
        if (std::string set = scene::materials::textureSetFor(material, c); !set.empty() && std::ranges::find(sets, set) == sets.end())
            sets.push_back(set);
    if (sets.size() > 1)
    {
        static std::set<std::string> told;
        if (told.insert(material.name() + "|" + sets[0] + "|" + sets[1]).second)
            log::warn("{}: occlusion, roughness and metallic come from {} and {}; the viewport draws them from one texture set, so it "
                      "shows {}'s",
                      material.name(), sets[0], sets[1], sets[0]);
    }
    return sets.empty() ? std::string() : sets.front();
}

void requestRedraw()
{
    mainq::post([] { viewportTools::requestRedraw(); });
}

// ---------------------------------------------------------------- virtual textures

/**
 * Whether a set shows through a virtual texture: /texture/virtualTexturing "on", or "auto" (the default) for sets with
 * at least /texture/vtThresholdTexels texels over their tiles, or tiles too large for a whole image.
 */
bool wantsVirtual(const tex::SetSettings& s)
{
    if (!render::vt::available())
        return false;
    const std::string mode = PrefStore::get().value<std::string>("general", "/texture/virtualTexturing", std::string("auto"));
    if (mode == "off")
        return false;
    if (mode == "on")
        return true;
    double texels = 0.0;
    for (const tex::Tile& t : s.tiles)
    {
        const auto [w, h] = s.tileSize(t);
        if (w > 16384 || h > 16384)
            return true;
        texels += double(w) * double(h);
    }
    return texels >= PrefStore::get().value<double>("general", "/texture/vtThresholdTexels", 4.0 * 4096.0 * 4096.0);
}

/** A virtual texture's layers for a set's maps (base colour, normal, ORM, emissive, those it has) and how each is composed. */
struct VirtualLayers
{
    std::vector<render::vt::LayerFormat> formats;
    std::vector<render::texgpu::PageLayer> compose;
    int baseColor = -1, normal = -1, orm = -1, emissive = -1;
};

VirtualLayers virtualLayers(const tex::SetSettings& s)
{
    using Source = StoreTarget::Source;
    VirtualLayers out;
    auto has = [&](const char* name) { return s.channel(name) != nullptr; };
    auto add = [&](int& index, render::TextureFormat format, bool srgb, std::array<Source, 4> channels) {
        index = int(out.formats.size());
        out.formats.push_back({format, srgb && format == render::TextureFormat::RGBA8});
        render::texgpu::PageLayer l;
        l.channels = channels;
        l.format = format;
        l.srgb = srgb && format == render::TextureFormat::RGBA8;
        out.compose.push_back(l);
    };
    auto rgba = [&](const tex::Channel& c) { return c.bits >= 16 ? render::TextureFormat::RGBA16F : render::TextureFormat::RGBA8; };
    auto color = [&](const char* name, Source alpha) {
        return std::array<Source, 4>{Source{name, 0, 1.0f}, Source{name, 1, 1.0f}, Source{name, 2, 1.0f}, alpha};
    };
    if (const tex::Channel* c = s.channel("baseColor"))
        add(out.baseColor, rgba(*c), true, color("baseColor", has("opacity") ? Source{"opacity", 0, 1.0f} : Source{{}, 0, 1.0f}));
    if (const tex::Channel* c = s.channel("normal"))
        add(out.normal, rgba(*c), false, color("normal", c->type == tex::PortType::Color ? Source{"normal", 3, 1.0f} : Source{{}, 0, 1.0f}));
    if (has("ao") || has("roughness") || has("metallic"))
    {
        bool wide = false;
        for (const char* n : {"ao", "roughness", "metallic"})
            if (const tex::Channel* c = s.channel(n); c && c->bits > 8)
                wide = true;
        add(out.orm, wide ? render::TextureFormat::RGBA16F : render::TextureFormat::RGBA8, false,
            {has("ao") ? Source{"ao", 0, 1.0f} : Source{{}, 0, 1.0f}, has("roughness") ? Source{"roughness", 0, 0.5f} : Source{{}, 0, 0.5f},
             has("metallic") ? Source{"metallic", 0, 0.0f} : Source{{}, 0, 0.0f}, Source{{}, 0, 1.0f}});
    }
    if (const tex::Channel* c = s.channel("emissive"))
        add(out.emissive, rgba(*c), true, color("emissive", c->type == tex::PortType::Color ? Source{"emissive", 3, 1.0f} : Source{{}, 0, 1.0f}));
    return out;
}

/**
 * Makes a texture set's virtual texture pages: per UDIM tile a pages plan (render::texgpu::Inputs::pages), evaluated
 * by a small pool of evaluators that move between tiles, painted pixels gathered straight from paint pages and mesh
 * maps read from the baker's images, region by region.
 */
class SetPages final : public render::vt::Producer
{
public:
    explicit SetPages(scene::NodeId node) : mNode(node) {}

    struct TilePlan
    {
        tex::Plan plan;
        render::texgpu::Inputs inputs;
    };

    /** Worker: the plans to make pages with. True when they differ from the last ones (every page is then stale). */
    bool setPlans(std::map<int, TilePlan> plans, std::string bakeProject, std::vector<render::texgpu::PageLayer> layers)
    {
        std::string key = bakeProject;
        for (const auto& [udim, t] : plans)
        {
            key += "|" + std::to_string(udim) + ":" + std::to_string(t.plan.structureHash) + ":" + std::to_string(t.plan.width) + "x" +
                   std::to_string(t.plan.height) + ":" + t.plan.error;
            key.append(reinterpret_cast<const char*>(t.plan.uniforms.data()), t.plan.uniforms.size() * sizeof(float));
        }
        for (const auto& l : layers)
            for (const auto& c : l.channels)
                key += "|" + c.output + std::to_string(c.component) + std::to_string(c.constant);
        std::scoped_lock lock(mMutex);
        const bool changed = key != mKey;
        mKey = key;
        mPlans = std::move(plans);
        mBakeProject = std::move(bakeProject);
        mLayers = std::move(layers);
        ++mVersion;
        return changed;
    }

    bool prepare(render::VulkanContext& vk, std::span<const render::vt::Job> jobs, double budgetMs) override
    {
        std::vector<int> udims;
        for (const render::vt::Job& j : jobs)
            if (std::find(udims.begin(), udims.end(), j.udim) == udims.end() && udims.size() < kEvaluators)
                udims.push_back(j.udim);
        bool any = false;
        for (int udim : udims)
        {
            Slot* s = slotFor(udim);
            if (!s || s->failed)
                continue;
            if (!s->ev->pagesReady())
                s->ev->step(vk, std::min(budgetMs, sliceBudgetMs()) / double(udims.size()));
            any = any || s->ev->pagesReady();
        }
        return any;
    }

    std::vector<uint8_t> record(render::VulkanContext& vk, VkCommandBuffer cmd, std::span<const render::vt::Job> jobs,
                                std::vector<render::compute::Buffer>& temps) override
    {
        std::vector<uint8_t> made(jobs.size(), 0);
        std::vector<render::texgpu::PageLayer> layers;
        std::string bakeProject;
        {
            std::scoped_lock lock(mMutex);
            layers = mLayers;
            bakeProject = mBakeProject;
        }
        for (Slot& s : mSlots)
        {
            if (!s.ev || s.failed || !s.ev->pagesReady())
                continue;
            std::vector<size_t> picked;
            std::vector<render::texgpu::PageJob> pageJobs;
            for (size_t i = 0; i < jobs.size() && pageJobs.size() < render::texgpu::Evaluator::kPagesPerBatch; ++i)
            {
                const render::vt::Job& j = jobs[i];
                if (j.udim != s.udim)
                    continue;
                render::texgpu::PageJob pj;
                pj.x0 = j.x0;
                pj.y0 = j.y0;
                pj.width = pj.height = render::vt::kSlotSize;
                pj.levelWidth = j.levelWidth;
                pj.levelHeight = j.levelHeight;
                pj.layers = j.layers;
                pageJobs.push_back(std::move(pj));
                picked.push_back(i);
            }
            if (pageJobs.empty())
                continue;
            const int udim = s.udim;
            const scene::NodeId node = mNode;
            auto load = [&, udim, node](render::VulkanContext& v, VkCommandBuffer c, const std::string& name, int lw, int lh, const std::array<int, 4>& rect,
                                        VkDeviceAddress dst, uint32_t stride) {
                std::string buffer, channel;
                if (tex::paint::parseExternalName(name, &buffer, &channel))
                {
                    const render::paint::PageView view = render::paint::pageView(node, buffer, channel, udim);
                    if (!view.table)
                        return false;
                    render::paint::recordGather(v, c, view, lw, lh, rect, dst, stride);
                    return true;
                }
                GpuImageRef map = render::texgpu::meshMap(bakeProject, name, udim);
                return map && render::texgpu::recordImageRegion(v, c, map, lw, lh, rect, dst, stride, temps);
            };
            try
            {
                const size_t n = s.ev->recordPages(vk, cmd, pageJobs, layers, load, temps);
                for (size_t k = 0; k < n && k < picked.size(); ++k)
                    made[picked[k]] = 1;
                s.lastUse = ++mUses;
            }
            catch (const std::exception& e)
            {
                log::warn("texture set pages, tile {}: {}", udim, e.what());
                s.failed = true;
            }
        }
        return made;
    }

private:
    static constexpr size_t kEvaluators = 2;

    struct Slot
    {
        int udim = 0;
        uint64_t version = 0;
        std::shared_ptr<Evaluator> ev;
        uint64_t lastUse = 0;
        bool failed = false;
    };

    /** The evaluator for a tile, taking the least recently used one over (render thread). Null when the tile has no plan. */
    Slot* slotFor(int udim)
    {
        TilePlan plan;
        uint64_t version = 0;
        {
            std::scoped_lock lock(mMutex);
            auto it = mPlans.find(udim);
            if (it == mPlans.end())
                return nullptr;
            version = mVersion;
            plan = it->second;
        }
        Slot* slot = nullptr;
        for (Slot& s : mSlots)
            if (s.ev && s.udim == udim)
                slot = &s;
        if (!slot)
        {
            if (mSlots.size() < kEvaluators)
                slot = &mSlots.emplace_back();
            else
                slot = &*std::min_element(mSlots.begin(), mSlots.end(), [](const Slot& a, const Slot& b) { return a.lastUse < b.lastUse; });
            if (!slot->ev)
                slot->ev = std::make_shared<Evaluator>();
            slot->udim = udim;
            slot->version = 0;
        }
        if (slot->version != version)
        {
            slot->version = version;
            slot->failed = false;
            if (std::string err = slot->ev->setPlan(std::move(plan.plan), std::move(plan.inputs)); !err.empty())
            {
                log::warn("texture set pages, tile {}: {}", udim, err);
                slot->failed = true;
            }
        }
        slot->lastUse = ++mUses;
        return slot;
    }

    scene::NodeId mNode;
    std::mutex mMutex;
    std::map<int, TilePlan> mPlans; // guarded
    std::string mBakeProject;       // guarded
    std::vector<render::texgpu::PageLayer> mLayers; // guarded
    std::string mKey;               // guarded
    uint64_t mVersion = 0;          // guarded
    // Render thread.
    std::vector<Slot> mSlots;
    uint64_t mUses = 0;
};

/** Worker: a virtual set's plans, one per tile at the tile's size up to /texture/vtPlanMax (pages evaluate at any level). */
std::string runVirtualJob(const Job& job)
{
    LILA_ZONE();
    std::vector<tex::OutputRequest> requests = tex::channelRequests(job.graph, job.settings);
    if (requests.empty())
        return job.graph.firstOfType(tex::kMaterialOutputType) ? "the set has no channels" : "the graph has no Material Output";
    const int cap = std::max(64, PrefStore::get().value<int>("general", "/texture/vtPlanMax", 2048));
    std::map<int, SetPages::TilePlan> plans;
    for (const tex::Tile& t : job.settings.tiles)
    {
        const auto [w, h] = job.settings.tileSize(t);
        tex::PlanRequest req;
        const double s = std::min(1.0, double(cap) / double(std::max(w, h)));
        req.width = std::max(1, int(std::lround(w * s)));
        req.height = std::max(1, int(std::lround(h * s)));
        req.outputs = requests;
        SetPages::TilePlan tp;
        tp.plan = tex::buildPlan(job.graph, req);
        if (!tp.plan.error.empty())
            return tp.plan.error;
        const auto [u, v] = tex::udimOrigin(t.udim);
        tp.inputs.uvOffset[0] = float(u);
        tp.inputs.uvOffset[1] = float(v);
        tp.inputs.halfBuffers = true;
        tp.inputs.pages = true;
        tp.inputs.pageSize = render::vt::kSlotSize;
        // Externals read whole (warped or tiled) come in as images; the rest region by region per page.
        for (const std::string& name : render::texgpu::wholeExternals(tp.plan))
        {
            if (GpuImageRef paint = render::paint::externalImage(job.node, name, t.udim))
                tp.inputs.gpuExternals[name] = paint;
            else if (GpuImageRef map = render::texgpu::meshMap(job.settings.bakeProject, name, t.udim))
                tp.inputs.gpuExternals[name] = map;
        }
        plans[t.udim] = std::move(tp);
    }
    if (job.pages->setPlans(std::move(plans), job.settings.bakeProject, virtualLayers(job.settings).compose))
        mainq::post([node = job.node] {
            if (auto it = gSets.find(node); it != gSets.end() && it->second->virtualTexture)
                it->second->virtualTexture->invalidateAll();
        });
    return {};
}

// ---------------------------------------------------------------- render task

double keepSeconds() { return PrefStore::get().value<double>("general", "/texture/keepEvaluatorSeconds", 30.0); }

/**
 * Frees the device memory of idle evaluators: previews once their tile's full size is done, full sizes unused for
 * /texture/keepEvaluatorSeconds. A freed evaluator reruns in full on the next edit instead of only what changed.
 */
void trimIdle(render::VulkanContext& vk)
{
    LILA_ZONE();
    std::vector<Entry> list;
    {
        std::scoped_lock lock(gTaskMutex);
        list = gEntries;
    }
    const auto now = std::chrono::steady_clock::now();
    const std::chrono::duration<double> keep(std::max(keepSeconds() - 0.5, 0.0));
    for (const Entry& e : list)
        if (e.ev->idle() && (e.full ? e.full->idle() : now - e.ran >= keep))
            e.ev->trim(vk);
}

std::atomic<uint64_t> gTrimGeneration{0};

/** From any thread: trims once the full sizes have gone keepSeconds without running, unless a later call comes first. */
void scheduleTrim()
{
    const uint64_t generation = ++gTrimGeneration;
    mainq::post([generation] {
        QTimer::singleShot(int(keepSeconds() * 1000.0), [generation] {
            render::Renderer& r = render::Renderer::get();
            if (generation != gTrimGeneration || !gInstalled || !r.running())
                return;
            r.runTask([](render::VulkanContext& vk) {
                trimIdle(vk);
                return true;
            });
        });
    });
}

bool runSlice(render::VulkanContext& vk)
{
    std::vector<Entry> list;
    {
        std::scoped_lock lock(gTaskMutex);
        list = gEntries;
    }
    for (Entry& e : list)
    {
        if (e.failed || e.ev->idle())
            continue;
        {
            std::scoped_lock lock(gTaskMutex);
            for (Entry& g : gEntries)
                if (g.ev == e.ev)
                    g.ran = std::chrono::steady_clock::now();
        }
        try
        {
            const bool finished = e.ev->step(vk, sliceBudgetMs());
            // A preview writes the tile's full-size images too, upsampled: the full size writes its texels back over
            // them, even when it has nothing of its own to redo there.
            if (std::array<float, 4> uv; e.full && e.ev->takeStored(uv))
                e.full->storeAgain(uv);
            if (finished && e.shown)
                e.shown->store(true);
            // A finished level has written its images: show them.
            if (finished)
                mainq::post([node = e.node] {
                    if (auto it = gSets.find(node); it != gSets.end())
                        it->second->ready = true;
                    viewportTools::requestRedraw();
                });
        }
        catch (const std::exception& ex)
        {
            const std::string what = ex.what();
            {
                std::scoped_lock lock(gTaskMutex);
                for (Entry& g : gEntries)
                    if (g.ev == e.ev)
                        g.failed = true;
            }
            mainq::post([node = e.node, what] {
                if (auto it = gSets.find(node); it != gSets.end())
                {
                    it->second->error = what;
                    log::warn("texture set {}: {}", it->second->name, what);
                }
            });
        }
        return false;
    }
    {
        std::scoped_lock lock(gTaskMutex);
        for (const Entry& e : gEntries)
            if (!e.failed && !e.ev->idle())
                return false;
        gTaskQueued = false;
    }
    trimIdle(vk);
    scheduleTrim();
    return true;
}

void ensureTask()
{
    std::scoped_lock lock(gTaskMutex);
    if (gTaskQueued)
        return;
    render::Renderer& r = render::Renderer::get();
    if (!r.running())
        return;
    gTaskQueued = true;
    r.runTask(runSlice);
}

// ---------------------------------------------------------------- worker

std::vector<StoreTarget> storesFor(const TileState& t, const tex::SetSettings& s)
{
    std::vector<StoreTarget> out;
    auto has = [&](const char* name) { return s.channel(name) != nullptr; };
    for (const tex::Channel& c : s.channels)
    {
        auto it = t.channels.find(c.name);
        if (it == t.channels.end())
            continue;
        StoreTarget st;
        st.image = it->second;
        for (int k = 0; k < 4; ++k)
            st.channels[size_t(k)] = {c.name, k, 1.0f};
        if (c.name == "baseColor")
            st.channels[3] = has("opacity") ? StoreTarget::Source{"opacity", 0, 1.0f} : StoreTarget::Source{{}, 0, 1.0f};
        else if (c.type != tex::PortType::Color)
            st.channels[3] = {{}, 0, 1.0f};
        out.push_back(std::move(st));
    }
    if (t.orm)
    {
        StoreTarget st;
        st.image = t.orm;
        st.channels[0] = has("ao") ? StoreTarget::Source{"ao", 0, 1.0f} : StoreTarget::Source{{}, 0, 1.0f};
        st.channels[1] = has("roughness") ? StoreTarget::Source{"roughness", 0, 0.5f} : StoreTarget::Source{{}, 0, 0.5f};
        st.channels[2] = has("metallic") ? StoreTarget::Source{"metallic", 0, 0.0f} : StoreTarget::Source{{}, 0, 0.0f};
        st.channels[3] = {{}, 0, 1.0f};
        out.push_back(std::move(st));
    }
    return out;
}

/** Evaluates the whole-image tiles a job carries, for the channels settings lists. */
std::string runTiles(const Job& job, const tex::SetSettings& settings)
{
    std::vector<tex::OutputRequest> requests = tex::channelRequests(job.graph, settings);
    if (requests.empty())
        return job.graph.firstOfType(tex::kMaterialOutputType) ? "the set has no channels" : "the graph has no Material Output";
    const std::string key = job.graph.toJson().dump() + settings.toJson().dump();
    for (const auto& tile : job.tiles)
    {
        const std::vector<StoreTarget> stores = storesFor(*tile, settings);
        const auto [u, v] = tex::udimOrigin(tile->udim);
        // Previews write the shown images upsampled, so over sharp ones they flash blurry. When only the inputs moved (a
        // stroke committed or undone, maps rebaked) the full size alone redoes what changed over images already sharp.
        const bool fullOnly = key == tile->plannedKey && tile->fullShown->load();
        tile->plannedKey = key;
        for (size_t l = fullOnly ? tile->levels.size() - 1 : 0; l < tile->levels.size(); ++l)
        {
            tex::PlanRequest req;
            req.width = tile->levelSizes[l].first;
            req.height = tile->levelSizes[l].second;
            req.outputs = requests;
            tex::Plan plan = tex::buildPlan(job.graph, req);
            if (!plan.error.empty())
                return plan.error;
            render::texgpu::Inputs in;
            in.uvOffset[0] = float(u);
            in.uvOffset[1] = float(v);
            in.halfBuffers = true;
            for (const tex::BufferDesc& b : plan.buffers)
                if (b.kind == tex::BufferKind::External)
                {
                    if (GpuImageRef paint = render::paint::externalImage(job.node, b.name, tile->udim))
                        in.gpuExternals[b.name] = paint;
                    else if (GpuImageRef map = render::texgpu::meshMap(job.settings.bakeProject, b.name, tile->udim))
                        in.gpuExternals[b.name] = map;
                }
            if (tile->externals.size() < tile->levels.size())
                tile->externals.resize(tile->levels.size());
            // Same graph: a new plan would reload every image and recompute the whole stack in one slice (a stall on
            // a stroke's first press or its commit). Only what reads the changed externals runs.
            if (fullOnly && tile->plans[l] > 0 && externalsSwap(tile->externals[l], in.gpuExternals, req.width, req.height))
            {
                tile->externals[l] = in.gpuExternals;
                tile->levels[l]->setExternals(std::move(in.gpuExternals));
                ++tile->swaps[l];
                ensureTask();
                continue;
            }
            tile->externals[l] = in.gpuExternals;
            if (std::string err = tile->levels[l]->setPlan(std::move(plan), std::move(in), stores); !err.empty())
                return err;
            ++tile->plans[l];
            {
                std::scoped_lock lock(gTaskMutex);
                for (Entry& e : gEntries)
                    if (e.ev == tile->levels[l])
                        e.failed = false;
            }
            ensureTask();
        }
        // After the new plans, so the old plan's full size finishing late can't claim the new previews are done.
        if (!fullOnly)
            tile->fullShown->store(false);
    }
    return {};
}

std::string runJob(const Job& job)
{
    LILA_ZONE();
    if (!job.pages)
        return runTiles(job, job.settings);
    if (std::string err = runVirtualJob(job); !err.empty() || job.tiles.empty())
        return err;
    // A virtual set's tiles are only its height copy for displacement preview.
    tex::SetSettings height = job.settings;
    std::erase_if(height.channels, [](const tex::Channel& c) { return c.name != "height"; });
    return runTiles(job, height);
}

/**
 * Compiles the passes a graph will need at every level of every tile, into the shared shader and pipeline caches, so the
 * edit that makes it the set's graph plans without compiling.
 */
void runWarm(const Job& job)
{
    LILA_ZONE();
    if (job.pages)
        return;
    std::vector<tex::OutputRequest> requests = tex::channelRequests(job.graph, job.settings);
    if (requests.empty())
        return;
    for (const auto& tile : job.tiles)
        for (const auto& [w, h] : tile->levelSizes)
        {
            tex::PlanRequest req;
            req.width = w;
            req.height = h;
            req.outputs = requests;
            const tex::Plan plan = tex::buildPlan(job.graph, req);
            if (!plan.error.empty())
                return;
            render::texgpu::precompile(plan);
        }
}

void workerLoop()
{
    for (;;)
    {
        Job job;
        bool warm = false;
        {
            std::unique_lock lock(gJobMutex);
            gJobCv.wait(lock, [] { return gJobQuit || !gJobs.empty() || !gWarm.empty(); });
            if (gJobQuit)
                return;
            // Real edits first: a warm job only fills the time the worker would sit idle.
            warm = gJobs.empty();
            std::map<scene::NodeId, Job>& from = warm ? gWarm : gJobs;
            job = std::move(from.begin()->second);
            from.erase(from.begin());
            gJobBusy = !warm;
        }
        if (warm)
        {
            try
            {
                runWarm(job);
            }
            catch (const std::exception& e)
            {
                log::debug("texture set warm-up: {}", e.what());
            }
            std::scoped_lock lock(gJobMutex);
            ++gWarmed[job.node];
            continue;
        }
        std::string err;
        try
        {
            err = runJob(job);
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        mainq::post([node = job.node, err] {
            if (auto it = gSets.find(node); it != gSets.end())
            {
                if (!err.empty() && it->second->error != err)
                    log::warn("texture set {}: {}", it->second->name, err);
                it->second->error = err;
            }
        });
        std::scoped_lock lock(gJobMutex);
        gJobBusy = false;
    }
}

void enqueue(Job job)
{
    {
        std::scoped_lock lock(gJobMutex);
        if (!gWorker.joinable())
        {
            gJobQuit = false;
            gWorker = std::thread(workerLoop);
        }
        const scene::NodeId id = job.node;
        gJobs[id] = std::move(job);
    }
    gJobCv.notify_one();
}

// ---------------------------------------------------------------- main thread

void dropEntries(const LiveSet& s)
{
    std::scoped_lock lock(gTaskMutex);
    std::erase_if(gEntries, [&](const Entry& e) { return e.node == s.node; });
}

render::TexturePtr textureOf(const Tiles& tiles, const std::function<GpuImageRef(const TileState&)>& pick)
{
    if (tiles.empty() || !pick(*tiles.front()))
        return nullptr;
    if (tiles.size() == 1)
        return pick(*tiles.front())->asTexture();
    auto t = std::make_shared<render::TextureImage>();
    const GpuImageRef first = pick(*tiles.front());
    t->width = first->width();
    t->height = first->height();
    t->format = first->format();
    t->srgb = first->desc().srgb;
    for (const auto& tile : tiles)
        if (GpuImageRef img = pick(*tile))
            t->tiles.push_back({tile->udim, img->asTexture()});
    std::sort(t->tiles.begin(), t->tiles.end(), [](const render::UdimTile& a, const render::UdimTile& b) { return a.udim < b.udim; });
    return t;
}

std::string configKey(const tex::SetSettings& s, const std::vector<int>& previews, bool wantsHeight)
{
    const bool virt = wantsVirtual(s);
    json k = {{"tiles", json::array()},
              {"channels", json::array()},
              {"previews", previews},
              {"virtual", virt},
              {"heightCopy", virt && wantsHeight && s.channel("height") ? heightPreviewSize() : 0}};
    for (const tex::Tile& t : s.tiles)
        k["tiles"].push_back({t.udim, s.tileSize(t).first, s.tileSize(t).second});
    for (const tex::Channel& c : s.channels)
        k["channels"].push_back({c.name, int(c.type), c.bits});
    return k.dump();
}

/** A virtual set: one virtual texture over its tiles, no whole images or evaluators of its own. */
void rebuildVirtual(LiveSet& s)
{
    const VirtualLayers layers = virtualLayers(s.settings);
    s.maps = {};
    s.virtualTexture.reset();
    s.pages.reset();
    if (layers.formats.empty())
        return;
    std::vector<render::vt::TileDesc> tiles;
    for (const tex::Tile& t : s.settings.tiles)
    {
        const auto [w, h] = s.settings.tileSize(t);
        tiles.push_back({t.udim, w, h});
    }
    try
    {
        s.pages = std::make_shared<SetPages>(s.node);
        s.virtualTexture = render::vt::Texture::create(s.name, layers.formats, tiles, s.pages);
    }
    catch (const std::exception& e)
    {
        s.error = e.what();
        s.pages.reset();
        return;
    }
    auto map = [&](int layer) { return layer < 0 ? nullptr : s.virtualTexture->map(layer); };
    s.maps.baseColor = map(layers.baseColor);
    s.maps.normal = map(layers.normal);
    s.maps.orm = map(layers.orm);
    s.maps.emissive = map(layers.emissive);
    s.maps.occlusion = s.settings.channel("ao") != nullptr;
    s.maps.roughness = s.settings.channel("roughness") != nullptr;
    s.maps.metallic = s.settings.channel("metallic") != nullptr;
}

/**
 * Gives s a whole-image tile per UDIM holding channels (and the packed ORM when orm), each tile's longest side capped
 * at cap when it is above 0, with an evaluator per level queued for the render task. False, with s.error, when the GPU
 * can't hold the images.
 */
bool addTiles(LiveSet& s, const std::vector<tex::Channel>& channels, bool orm, bool ormFloat, int cap)
{
    const std::vector<int> previews = previewLevels();
    for (const tex::Tile& t : s.settings.tiles)
    {
        auto tile = std::make_shared<TileState>();
        tile->udim = t.udim;
        std::tie(tile->width, tile->height) = cap > 0 ? capped(s.settings.tileSize(t), cap) : s.settings.tileSize(t);
        const std::string tag = s.name + "." + std::to_string(t.udim) + ".";
        try
        {
            for (const tex::Channel& c : channels)
                tile->channels[c.name] = GpuImage::create({tile->width, tile->height, channelFormat(c), colorData(c.name), true, tag + c.name});
            if (orm)
                tile->orm = GpuImage::create({tile->width, tile->height, ormFloat ? TextureFormat::RGBA16F : TextureFormat::RGBA8, false, true, tag + "orm"});
        }
        catch (const std::exception& e)
        {
            s.error = e.what();
            dropEntries(s);
            s.tiles.clear();
            s.maps = {};
            return false;
        }
        tile->levelSizes = levelSizes(tile->width, tile->height, previews);
        tile->plans = std::vector<std::atomic<uint64_t>>(tile->levelSizes.size());
        tile->swaps = std::vector<std::atomic<uint64_t>>(tile->levelSizes.size());
        std::scoped_lock lock(gTaskMutex);
        for (size_t l = 0; l < tile->levelSizes.size(); ++l)
            tile->levels.push_back(std::make_shared<Evaluator>());
        for (size_t l = 0; l < tile->levels.size(); ++l)
        {
            const bool preview = l + 1 < tile->levels.size();
            gEntries.push_back({.level = int(l),
                                .seq = gSeq++,
                                .node = s.node,
                                .ev = tile->levels[l],
                                .full = preview ? tile->levels.back() : nullptr,
                                .shown = preview ? nullptr : tile->fullShown});
        }
        std::stable_sort(gEntries.begin(), gEntries.end(), [](const Entry& a, const Entry& b) { return a.level < b.level; });
        s.tiles.push_back(std::move(tile));
    }
    return true;
}

void rebuild(LiveSet& s)
{
    s.ready = false;
    dropEntries(s);
    s.tiles.clear();
    s.virtualTexture.reset();
    s.pages.reset();
    if (wantsVirtual(s.settings))
    {
        rebuildVirtual(s);
        if (const tex::Channel* height = s.settings.channel("height"); height && s.wantsHeight && s.virtualTexture)
            addTiles(s, {*height}, false, false, heightPreviewSize());
        return;
    }
    bool ormFloat = false;
    for (const char* n : {"ao", "roughness", "metallic"})
        if (const tex::Channel* c = s.settings.channel(n); c && c->bits > 8)
            ormFloat = true;
    const bool orm = s.settings.channel("ao") || s.settings.channel("roughness") || s.settings.channel("metallic");
    if (!addTiles(s, s.settings.channels, orm, ormFloat, 0))
        return;
    s.maps = {};
    auto channel = [&](const char* name) {
        return textureOf(s.tiles, [name](const TileState& t) {
            auto it = t.channels.find(name);
            return it == t.channels.end() ? nullptr : it->second;
        });
    };
    s.maps.baseColor = channel("baseColor");
    s.maps.normal = channel("normal");
    s.maps.emissive = channel("emissive");
    s.maps.orm = textureOf(s.tiles, [](const TileState& t) { return t.orm; });
    s.maps.occlusion = s.settings.channel("ao") != nullptr;
    s.maps.roughness = s.settings.channel("roughness") != nullptr;
    s.maps.metallic = s.settings.channel("metallic") != nullptr;
}

void replan(LiveSet& s)
{
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.node(s.node);
    if (!n)
        return;
    Job job;
    job.node = s.node;
    try
    {
        job.graph = tex::readGraph(*n);
        job.settings = tex::readSettings(*n);
        tex::followLiveFrame(job.graph, job.settings);
        s.animated = tex::sequence::animated(job.graph);
        if (s.animated)
            tex::sequence::followTime(job.graph, g, *n, g.time());
    }
    catch (const std::exception& e)
    {
        s.error = e.what();
        return;
    }
    s.name = n->name();
    s.material = joinMaterials(tex::materialsUsing(g, n->name()));
    s.settings = job.settings;
    const std::string key = configKey(job.settings, previewLevels(), s.wantsHeight);
    if (key != s.configKey)
    {
        s.configKey = key;
        rebuild(s);
    }
    job.tiles = s.tiles;
    job.pages = s.pages;
    if (s.pages)
        try
        {
            // Pages gather painted pixels from paint pages, which nothing else may have asked for yet.
            render::paint::sync(g, *n);
        }
        catch (const std::exception& e)
        {
            log::warn("texture set {}: paint: {}", s.name, e.what());
        }
    enqueue(std::move(job));
}

void ensureIndex()
{
    if (!gIndexDirty)
        return;
    gIndexDirty = false;
    gIndex.clear();
    scene::Graph& g = scene::Graph::current();
    for (scene::NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (n->type().name != tex::kTextureSetType)
            continue;
        gIndex.emplace(n->name(), id);
    }
}

/** A live set's maps by set name, starting its evaluation the first time; nullopt when there is no such set. */
std::optional<Maps> setMaps(const std::string& name)
{
    ensureIndex();
    auto it = gIndex.find(name);
    if (it == gIndex.end())
        return std::nullopt;
    std::shared_ptr<LiveSet>& live = gSets[it->second];
    if (!live)
    {
        live = std::make_shared<LiveSet>();
        live->node = it->second;
        replan(*live);
    }
    Maps maps = live->maps;
    maps.ready = live->ready || live->virtualTexture != nullptr;
    return maps;
}


void dropSet(scene::NodeId id)
{
    auto it = gSets.find(id);
    if (it == gSets.end())
        return;
    dropEntries(*it->second);
    {
        std::scoped_lock lock(gJobMutex);
        gJobs.erase(id);
    }
    gSets.erase(it);
}

void onChange(scene::Change c, scene::NodeId id)
{
    scene::Graph& g = scene::Graph::current();
    switch (c)
    {
    case scene::Change::Created:
    case scene::Change::Renamed:
        gIndexDirty = true;
        if (auto it = gSets.find(id); it != gSets.end())
            replan(*it->second);
        break;
    case scene::Change::Deleted:
        gIndexDirty = true;
        dropSet(id);
        break;
    case scene::Change::Cleared:
    case scene::Change::Reset:
    {
        gIndexDirty = true;
        std::vector<scene::NodeId> ids;
        for (auto& [k, v] : gSets)
            ids.push_back(k);
        for (scene::NodeId k : ids)
        {
            scene::Node* n = g.node(k);
            if (!n || n->type().name != tex::kTextureSetType)
                dropSet(k);
            else
                replan(*gSets[k]);
        }
        break;
    }
    case scene::Change::Value:
        if (scene::Node* n = g.node(id); n && n->type().name == tex::kTextureSetType)
        {
            gIndexDirty = true;
            if (auto it = gSets.find(id); it != gSets.end())
                replan(*it->second);
        }
        break;
    default: break;
    }
}
} // namespace

namespace
{
/**
 * A stroke in progress changed the set's paint images: rerun only the passes reading them, over the region the stroke
 * changed, at one level per tile (the tile's own size up to /texture/paintLivePixels texels, else its largest preview),
 * with no new plan. The stroke's commit replans every level. False when the set has no plan yet.
 */
bool refreshPaint(LiveSet& s)
{
    if (s.tiles.empty() || s.configKey.empty())
        return false;
    const double budget = PrefStore::get().value<double>("general", "/texture/paintLivePixels", 16384.0 * 16384.0);
    for (const auto& tile : s.tiles)
    {
        if (tile->levels.empty())
            return false;
        size_t pick = 0;
        for (size_t l = 0; l < tile->levels.size(); ++l)
            if (double(tile->levelSizes[l].first) * double(tile->levelSizes[l].second) <= budget)
                pick = l;
        tile->levels[pick]->refreshExternals();
    }
    ensureTask();
    return true;
}
} // namespace

std::optional<Maps> mapsFor(const std::string& material)
{
    if (!gInstalled || material.empty() || !enabled() || !render::Renderer::get().running())
        return std::nullopt;
    scene::Node* m = scene::materials::find(scene::Graph::current(), material);
    if (!m)
        return std::nullopt;
    std::map<std::string, std::optional<Maps>> sets;
    auto of = [&](const std::string& channel) -> const Maps* {
        const std::string set = scene::materials::textureSetFor(*m, channel);
        if (set.empty())
            return nullptr;
        auto [it, added] = sets.try_emplace(set);
        if (added)
            it->second = setMaps(set);
        return it->second ? &*it->second : nullptr;
    };
    Maps out;
    out.ready = true;
    bool any = false;
    auto take = [&](const Maps* from, render::TexturePtr Maps::* field) {
        if (!from || !(from->*field))
            return;
        out.*field = from->*field;
        out.ready = out.ready && from->ready;
        any = true;
    };
    take(of("baseColor"), &Maps::baseColor);
    take(of("normal"), &Maps::normal);
    take(of("emissive"), &Maps::emissive);
    // The viewport draws occlusion, roughness and metallic from one packed texture, so they come from one set.
    const std::string packedSet = packedSetFor(*m);
    if (!packedSet.empty())
    {
        auto [it, added] = sets.try_emplace(packedSet);
        if (added)
            it->second = setMaps(packedSet);
        if (const Maps* from = it->second ? &*it->second : nullptr; from && from->orm)
        {
            take(from, &Maps::orm);
            out.occlusion = from->occlusion && scene::materials::textureSetFor(*m, "ambientOcclusion") == packedSet;
            out.roughness = from->roughness && scene::materials::textureSetFor(*m, "roughness") == packedSet;
            out.metallic = from->metallic && scene::materials::textureSetFor(*m, "metallic") == packedSet;
            if (scene::materials::textureSetMaps(*m).contains("orm"))
            {
                out.occlusion = from->occlusion;
                out.roughness = from->roughness;
                out.metallic = from->metallic;
            }
        }
    }
    if (!any)
        return std::nullopt;
    return out;
}


render::GpuImageRef channelImage(const std::string& set, const std::string& channel, int udim)
{
    for (const auto& [id, s] : gSets)
        if (s->name == set)
        {
            if (s->virtualTexture && channel == "height" && !s->wantsHeight)
            {
                s->wantsHeight = true;
                replan(*s);
            }
            // A virtual set's height copy is black until its first level runs; that would sink the whole surface.
            if (s->virtualTexture && !s->ready)
                return nullptr;
            for (const auto& t : s->tiles)
                if (t->udim == udim)
                {
                    auto it = t->channels.find(channel);
                    return it == t->channels.end() ? nullptr : it->second;
                }
        }
    return nullptr;
}

std::optional<HeightCopy> heightCopy(const std::string& set)
{
    for (const auto& [id, s] : gSets)
        if (s->name == set && s->virtualTexture)
        {
            HeightCopy out;
            for (const tex::Tile& t : s->settings.tiles)
            {
                const auto [w, h] = s->settings.tileSize(t);
                out.full = std::max({out.full, w, h});
            }
            for (const auto& t : s->tiles)
                out.size = std::max({out.size, t->width, t->height});
            out.ready = s->ready && out.size > 0;
            return out;
        }
    return std::nullopt;
}

bool isLive(const std::string& set)
{
    for (const auto& [id, s] : gSets)
        if (s->name == set)
            return true;
    return false;
}

void install()
{
    if (gInstalled)
        return;
    gInstalled = true;
    if (!ops::find("texLiveStatus"))
        ops::registerCommand({
            .name = "texLiveStatus",
            .doc = "The viewport's live texture sets: per set its material, warm-ups compiled ahead, tiles and evaluation levels (size, idle, plans, passes, compiles, GPU "
                   "milliseconds) and the last error; idle is whether every set has finished its latest edit.",
            .undoable = false,
            .run = [](const Args&) -> nlohmann::json { return {{"idle", idle()}, {"sets", status()}}; },
        });
    gGraphListener = scene::Graph::current().addListener(onChange);
    gTimeSub = events::subscribe(events::Event::TimeChanged, [](const events::EventData&) {
        std::vector<std::shared_ptr<LiveSet>> sets;
        for (auto& [id, s] : gSets)
            if (s->animated)
                sets.push_back(s);
        for (auto& s : sets)
            replan(*s);
    });
    // A new height copy size rebuilds the copies made at the old one.
    gPrefListener = PrefStore::get().onChanged([](const std::string& file) {
        if (file != "general")
            return;
        mainq::post([] {
            for (auto& [id, s] : gSets)
                if (s->wantsHeight && configKey(s->settings, previewLevels(), true) != s->configKey)
                    replan(*s);
        });
    });
    gMeshListener = render::texgpu::addMeshMapListener([](const std::string& project) {
        mainq::post([project] {
            for (auto& [id, s] : gSets)
                if (s->settings.bakeProject == project)
                {
                    replan(*s);
                    if (s->virtualTexture)
                        s->virtualTexture->invalidateAll();
                }
        });
    });
    gPaintListener = render::paint::addChangeListener([](scene::NodeId set) {
        gPaintedAt.store(std::chrono::steady_clock::now().time_since_epoch().count());
        auto it = gSets.find(set);
        if (it == gSets.end())
            return;
        if (const auto& vt = it->second->virtualTexture)
        {
            // Pages over what the stroke changed are redone, and pinned while it lasts.
            const bool live = render::paint::anyLive(set);
            for (const auto& [udim, rect] : render::paint::takeChanges(set))
                vt->invalidate(udim, rect, live);
            if (!live)
                vt->unpin();
            return;
        }
        if (render::paint::anyLive(set) && refreshPaint(*it->second))
            return;
        replan(*it->second);
    });
    gShutdownSub = events::subscribe(events::Event::Shutdown, [](const events::EventData&) { shutdown(); });
    render::vt::setRedraw([] { viewportTools::requestRedraw(); });
}

void shutdown()
{
    if (!gInstalled)
        return;
    gInstalled = false;
    scene::Graph::current().removeListener(gGraphListener);
    render::texgpu::removeMeshMapListener(gMeshListener);
    render::paint::removeChangeListener(gPaintListener);
    PrefStore::get().removeOnChanged(gPrefListener);
    events::unsubscribe(gShutdownSub);
    events::unsubscribe(gTimeSub);
    {
        std::scoped_lock lock(gJobMutex);
        gJobQuit = true;
        gJobs.clear();
    }
    gJobCv.notify_all();
    if (gWorker.joinable())
        gWorker.join();
    {
        std::scoped_lock lock(gTaskMutex);
        gEntries.clear();
    }
    gSets.clear();
    gIndex.clear();
    gIndexDirty = true;
}

void warm(const std::string& set, const tex::Graph& graph)
{
    if (!gInstalled)
        return;
    for (const auto& [id, s] : gSets)
    {
        if (s->name != set || s->tiles.empty() || s->pages || s->virtualTexture)
            continue;
        Job job;
        job.node = id;
        job.graph = graph;
        job.settings = s->settings;
        job.tiles = s->tiles;
        {
            std::scoped_lock lock(gJobMutex);
            if (!gWorker.joinable())
            {
                gJobQuit = false;
                gWorker = std::thread(workerLoop);
            }
            gWarm[id] = std::move(job);
        }
        gJobCv.notify_one();
        return;
    }
}

bool idle()
{
    {
        std::scoped_lock lock(gJobMutex);
        if (gJobBusy || !gJobs.empty())
            return false;
    }
    std::scoped_lock lock(gTaskMutex);
    for (const Entry& e : gEntries)
        if (!e.failed && !e.ev->idle())
            return false;
    return true;
}

bool waitIdle(double timeoutMs)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double, std::milli>(timeoutMs);
    for (;;)
    {
        render::paint::settleLive();
        mainq::drain();
        if (idle())
        {
            mainq::drain();
            return true;
        }
        if (std::chrono::steady_clock::now() > end)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

json status()
{
    json out = json::array();
    std::vector<Entry> entries;
    {
        std::scoped_lock lock(gTaskMutex);
        entries = gEntries;
    }
    for (const auto& [id, s] : gSets)
    {
        json tiles = json::array();
        for (const auto& t : s->tiles)
        {
            json levels = json::array();
            for (size_t l = 0; l < t->levels.size(); ++l)
            {
                const render::texgpu::Stats st = t->levels[l]->stats();
                bool failed = false;
                for (const Entry& e : entries)
                    failed = failed || (e.ev == t->levels[l] && e.failed);
                levels.push_back({{"width", t->levelSizes[l].first},
                                  {"height", t->levelSizes[l].second},
                                  {"idle", t->levels[l]->idle()},
                                  {"plans", l < t->plans.size() ? t->plans[l].load() : 0},
                                  {"swaps", l < t->swaps.size() ? t->swaps[l].load() : 0},
                                  {"failed", failed},
                                  {"passesDirty", st.passesDirty},
                                  {"passesCarried", st.passesCarried},
                                  {"passesRun", st.passesRun},
                                  {"compiled", st.compiled},
                                  {"pipelinesCreated", st.pipelinesCreated},
                                  {"gpuMs", st.gpuMs},
                                  {"texelsLoaded", st.texelsLoaded},
                                  {"texelsComputed", st.texelsComputed},
                                  {"texelsStored", st.texelsStored}});
            }
            tiles.push_back({{"udim", t->udim}, {"levels", levels}});
        }
        json entry = {{"set", s->name}, {"material", s->material}, {"tiles", tiles}, {"error", s->error}};
        {
            std::scoped_lock lock(gJobMutex);
            auto w = gWarmed.find(id);
            entry["warmed"] = w == gWarmed.end() ? 0 : w->second;
        }
        if (s->virtualTexture)
        {
            const render::vt::TextureStats vs = s->virtualTexture->stats();
            entry["virtual"] = {{"resident", vs.resident}, {"queued", vs.queued}, {"stale", vs.stale}, {"pinned", vs.pinned},
                                {"produced", vs.produced}, {"requested", vs.requested}, {"bytes", vs.bytes}};
        }
        out.push_back(std::move(entry));
    }
    return out;
}
} // namespace lila::ui::texLive
