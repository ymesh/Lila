// Hydra viewport renderers: each viewport session owns a render index for one render delegate, fed by Lila's scene
// index, its materials and the live stages (UsdHydraScene.h), drawn through HdxTaskController. Storm runs on Vulkan
// through its own device, headless; AOVs that are HgiVulkan textures on Lila's GPU are copied into images Lila's device
// shares (UsdHydraInterop.h), others are mapped and handed over as they are. Either way a worker thread finishes the
// frame, and Lila's composite flips, unpremultiplies, exposes and unprojects it.

#include "UsdBackend.h"
#include "UsdCommon.h"
#include "UsdHydraInterop.h"
#include "UsdHydraScene.h"
#include "UsdStage.h"

#include <lila/GpuImage.h>
#include <lila/Log.h>
#include <lila/Prefs.h>
#include <lila/Profile.h>
#include <lila/Tasks.h>
#include <lila/Transform.h>
#include <lila/ViewportRenderers.h>

#include <pxr/base/gf/half.h>
#include <pxr/base/gf/rect2i.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/glf/simpleLightingContext.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/driver.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/pluginRenderDelegateUniqueHandle.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/rendererCreateArgsSchema.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginHandle.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdx/colorCorrectionTask.h>
#include <pxr/imaging/hdx/renderSetupTask.h>
#include <pxr/imaging/hdx/selectionTracker.h>
#include <pxr/imaging/hdx/taskController.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/imaging/hgi/hgi.h>
#include <pxr/imaging/hgi/texture.h>
#include <pxr/imaging/hgi/tokens.h>
#include <pxr/usdImaging/usdImaging/sceneIndex.h>

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <format>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <thread>

PXR_NAMESPACE_USING_DIRECTIVE

using namespace lila;

namespace
{
namespace vr = ui::viewportRenderers;
using namespace usd::hydra;
using scene::Graph;
using scene::NodeId;

/** The Hgi new sessions get. Never freed, so Storm's shared resources outlive any one viewport and exit ordering. */
std::shared_ptr<Hgi>& currentHgi()
{
    static auto* hgi = new std::shared_ptr<Hgi>();
    return *hgi;
}

/**
 * One Vulkan Hgi for every session. Main thread. A lost one is replaced through forgetHgi, and goes once the last
 * session drawing with it closes.
 */
std::shared_ptr<Hgi> sharedHgi(std::string& error)
{
    static std::string failure;
    static bool tried = false;
    std::shared_ptr<Hgi>& hgi = currentHgi();
    if (!hgi && !tried)
    {
        HgiUniquePtr h = Hgi::CreateNamedHgi(HgiTokens->Vulkan);
        if (!h)
            failure = "HgiVulkan is not available";
        else if (!h->IsBackendSupported())
            failure = "no Vulkan device Hydra can use";
        else
            hgi = std::shared_ptr<Hgi>(h.release());
        tried = !hgi;
    }
    if (!hgi)
        error = failure;
    return hgi;
}

/** New sessions get a new Hgi from now on, when lost is still the current one. */
void forgetHgi(const Hgi* lost)
{
    if (currentHgi().get() == lost)
        currentHgi().reset();
}

/**
 * Lila's projection as Hydra wants it: OpenGL clip space (y up, depth -1 to 1, not reversed) with a finite far plane.
 * The x and y rows are Lila's, so the framing matches the viewport exactly.
 */
glm::dmat4 glProjection(const vr::Request& r)
{
    const glm::dmat4& p = r.view.proj;
    const double n = r.view.ortho ? r.nearClip : std::max(r.nearClip, 1e-4);
    const double f = r.farClip > n ? r.farClip : std::max(std::abs(n) * 1e7, 1e6);
    glm::dmat4 g(0.0);
    for (int c = 0; c < 4; ++c)
    {
        g[c][0] = p[c][0];
        g[c][1] = -p[c][1];
    }
    if (r.view.ortho)
    {
        g[2][2] = -2.0 / (f - n);
        g[3][2] = -(f + n) / (f - n);
        g[3][3] = 1.0;
    }
    else
    {
        g[2][2] = -(f + n) / (f - n);
        g[3][2] = -2.0 * f * n / (f - n);
        g[2][3] = -1.0;
    }
    return g;
}

/** Calls fn with a reader of format's components (pixel, index), chosen once so pixel loops don't switch per component. */
template <typename Fn>
void withReader(HdFormat format, Fn&& fn)
{
    switch (HdGetComponentFormat(format))
    {
    case HdFormatUNorm8: fn([](const uint8_t* p, int i) { return p[i] * (1.0f / 255.0f); }); break;
    case HdFormatSNorm8: fn([](const uint8_t* p, int i) { return std::max(-1.0f, int8_t(p[i]) / 127.0f); }); break;
    case HdFormatFloat16: fn([](const uint8_t* p, int i) { return float(reinterpret_cast<const GfHalf*>(p)[i]); }); break;
    case HdFormatFloat32: fn([](const uint8_t* p, int i) { return reinterpret_cast<const float*>(p)[i]; }); break;
    case HdFormatInt32: fn([](const uint8_t* p, int i) { return float(reinterpret_cast<const int32_t*>(p)[i]); }); break;
    default: break;
    }
}

/** Runs fn(y) for each of h rows across Lila's thread pool. */
template <typename Fn>
void forRows(int h, Fn&& fn)
{
    tbb::parallel_for(tbb::blocked_range<int>(0, h, 16), [&](const tbb::blocked_range<int>& rows) {
        for (int y = rows.begin(); y < rows.end(); ++y)
            fn(y);
    });
}

/** Runs one job at a time on a thread of its own, so a session's frame finishes while the main thread moves on. */
class Worker
{
public:
    Worker() : mThread([this] { loop(); }) {}
    ~Worker()
    {
        {
            std::scoped_lock lock(mMutex);
            mStop = true;
        }
        mChanged.notify_all();
        mThread.join();
    }
    /** Waits for the job before, then starts this one. */
    void run(std::function<void()> job)
    {
        std::unique_lock lock(mMutex);
        mChanged.wait(lock, [this] { return !mJob; });
        mJob = std::move(job);
        mChanged.notify_all();
    }
    void wait()
    {
        std::unique_lock lock(mMutex);
        mChanged.wait(lock, [this] { return !mJob; });
    }

private:
    void loop()
    {
        LILA_THREAD_NAME("Hydra frames");
        std::unique_lock lock(mMutex);
        while (true)
        {
            mChanged.wait(lock, [this] { return mStop || mJob; });
            if (!mJob)
                return;
            std::function<void()> job = mJob;
            lock.unlock();
            try
            {
                job();
            }
            catch (const std::exception& e)
            {
                log::error("Hydra viewport: {}", e.what());
            }
            lock.lock();
            mJob = nullptr;
            mChanged.notify_all();
        }
    }

    std::mutex mMutex;
    std::condition_variable mChanged;
    std::function<void()> mJob;
    bool mStop = false;
    std::thread mThread;
};

/** A render setting's value as Lila's settings carry it: bool, number or string; null for any other type. */
nlohmann::json toJson(const VtValue& v)
{
    if (v.IsHolding<bool>())
        return v.UncheckedGet<bool>();
    if (v.IsHolding<int>())
        return v.UncheckedGet<int>();
    if (v.IsHolding<unsigned int>())
        return v.UncheckedGet<unsigned int>();
    if (v.IsHolding<int64_t>())
        return v.UncheckedGet<int64_t>();
    if (v.IsHolding<float>())
        return v.UncheckedGet<float>();
    if (v.IsHolding<double>())
        return v.UncheckedGet<double>();
    if (v.IsHolding<std::string>())
        return v.UncheckedGet<std::string>();
    if (v.IsHolding<TfToken>())
        return v.UncheckedGet<TfToken>().GetString();
    return nullptr;
}

/** A value for a render setting, as the type of like (its current value) when there is one; empty when it doesn't convert. */
VtValue toVt(const nlohmann::json& j, const VtValue& like = VtValue())
{
    if (!like.IsEmpty())
    {
        if (like.IsHolding<bool>() && j.is_boolean())
            return VtValue(j.get<bool>());
        if (j.is_number())
        {
            if (like.IsHolding<int>())
                return VtValue(j.get<int>());
            if (like.IsHolding<unsigned int>())
                return VtValue(j.get<unsigned int>());
            if (like.IsHolding<int64_t>())
                return VtValue(j.get<int64_t>());
            if (like.IsHolding<float>())
                return VtValue(j.get<float>());
            if (like.IsHolding<double>())
                return VtValue(j.get<double>());
        }
        if (j.is_string() && like.IsHolding<TfToken>())
            return VtValue(TfToken(j.get<std::string>()));
        if (j.is_string() && like.IsHolding<std::string>())
            return VtValue(j.get<std::string>());
        return VtValue();
    }
    if (j.is_boolean())
        return VtValue(j.get<bool>());
    if (j.is_number_integer())
        return VtValue(j.get<int>());
    if (j.is_number())
        return VtValue(j.get<double>());
    if (j.is_string())
        return VtValue(j.get<std::string>());
    return VtValue();
}

struct StageChain
{
    UsdStageRefPtr stage;
    UsdImagingSceneIndexRefPtr usd;
    TfRefPtr<StageOverrides> overrides;
    TfRefPtr<StagePlacement> world;
};

class HydraSession final : public vr::Session
{
public:
    HydraSession(const std::string& plugin, int panel, const nlohmann::json& saved, std::string& error)
    {
        HdRendererPluginRegistry& registry = HdRendererPluginRegistry::GetInstance();
        HdRendererPluginHandle handle = registry.GetOrCreateRendererPlugin(TfToken(plugin));
        if (!handle)
        {
            error = "no render delegate " + plugin + " on the plugin path";
            return;
        }
        std::string hgiError;
        mHgiOwner = sharedHgi(hgiError);
        Hgi* hgi = mHgiOwner.get();
        HdContainerDataSourceHandle drivers;
        if (hgi)
        {
            const TfToken name = HgiTokens->renderDriver;
            const HdDataSourceBaseHandle value = HdRetainedTypedSampledDataSource<Hgi*>::New(hgi);
            drivers = HdRetainedContainerDataSource::New(1, &name, &value);
        }
        const HdRendererCreateArgsSchema args(
            HdRendererCreateArgsSchema::Builder().SetGpuEnabled(HdRetainedTypedSampledDataSource<bool>::New(true)).SetDrivers(drivers).Build());
        mStorm = plugin == "HdStormRendererPlugin";
        std::string why;
        if (mStorm && !hgi)
        {
            error = hgiError;
            return;
        }
        // Storm's own check tries the platform's default Hgi (OpenGL on Windows), which needs a window; it runs on ours.
        if (!mStorm && !handle->IsSupported(args, &why))
        {
            error = why.empty() ? "it can't run here" : why;
            return;
        }
        // Lila's scene units and up axis, for delegates placing it in their own world, with the settings saved for it.
        HdRenderSettingsMap initial;
        initial[TfToken("lila:metersPerUnit")] = VtValue(0.01);
        initial[TfToken("lila:upAxis")] = VtValue(std::string(PrefStore::get().value<std::string>("general", "/scene/upAxis", "y") == "y" ? "Y" : "Z"));
        for (auto& [key, value] : saved.items())
            if (VtValue v = toVt(value); !v.IsEmpty())
                initial[TfToken(key)] = v;
        mDelegate = handle->CreateDelegate(initial);
        if (!mDelegate)
        {
            error = "it failed to start";
            return;
        }
        HdDriverVector driverList;
        if (hgi)
        {
            mDriver.name = HgiTokens->renderDriver;
            mDriver.driver = VtValue(hgi);
            driverList.push_back(&mDriver);
        }
        mIndex.reset(HdRenderIndex::New(mDelegate.Get(), driverList, std::format("lilaViewport{}", panel), "Lila"));
        if (!mIndex)
        {
            error = "its render index failed to start";
            return;
        }
        mTasks = std::make_unique<HdxTaskController>(mIndex.get(), SdfPath(std::format("/LilaTasks/Panel{}", panel)), true);

        TfTokenVector aovs = {HdAovTokens->color};
        if (mDelegate->GetDefaultAovDescriptor(HdAovTokens->depth).format != HdFormatInvalid)
            aovs.push_back(HdAovTokens->depth);
        mTasks->SetRenderOutputs(aovs);
        HdAovDescriptor color = mTasks->GetRenderOutputSettings(HdAovTokens->color);
        color.clearValue = VtValue(GfVec4f(0.0f));
        mTasks->SetRenderOutputSettings(HdAovTokens->color, color);
        mTasks->SetEnableSelection(false);
        mTasks->SetEnablePresentation(false);
        mTasks->SetRenderTags({HdRenderTagTokens->geometry, HdRenderTagTokens->render});
        mTasks->SetCollection(HdRprimCollection(HdTokens->geometry, HdReprSelector(HdReprTokens->refined)));
        HdxColorCorrectionTaskParams cc;
        cc.colorCorrectionMode = HdxColorCorrectionTokens->disabled;
        mTasks->SetColorCorrectionParams(cc);
        HdxRenderTaskParams params;
        params.enableLighting = true;
        params.enableSceneLights = true;
        mTasks->SetRenderParams(params);
        mEngine.SetTaskContextData(HdxTokens->selectionState, VtValue(std::make_shared<HdxSelectionTracker>()));

        // A light at the eye for scenes that have none, as Lila's default lighting and usdview's camera light do.
        GlfSimpleLight eye;
        eye.SetIsCameraSpaceLight(true);
        eye.SetPosition(GfVec4f(0.0f, 0.0f, 0.0f, 1.0f));
        eye.SetDiffuse(GfVec4f(1.0f));
        eye.SetSpecular(GfVec4f(0.2f));
        eye.SetAmbient(GfVec4f(0.0f));
        GlfSimpleMaterial material;
        material.SetAmbient(GfVec4f(0.2f, 0.2f, 0.2f, 1.0f));
        material.SetSpecular(GfVec4f(0.1f, 0.1f, 0.1f, 1.0f));
        material.SetShininess(32.0);
        for (bool withEye : {true, false})
        {
            GlfSimpleLightingContextRefPtr ctx = GlfSimpleLightingContext::New();
            ctx->SetLights(withEye ? GlfSimpleLightVector{eye} : GlfSimpleLightVector{});
            ctx->SetMaterial(material);
            ctx->SetSceneAmbient(GfVec4f(0.01f, 0.01f, 0.01f, 1.0f));
            ctx->SetUseLighting(true);
            (withEye ? mEyeLight : mSceneLights) = ctx;
        }

        mLila = LilaSceneIndex::New();
        mIndex->InsertSceneIndex(mLila, SdfPath::AbsoluteRootPath(), false);
        mMaterialIndex = UsdImagingSceneIndex::New(HdRetainedContainerDataSource::New(), [](const HdSceneIndexBaseRefPtr& in) { return in; });
        mMaterialIndex->SetStage(mMaterials.stage());
        mIndex->InsertSceneIndex(mMaterialIndex, materialsRoot());
        mHgi = hgi;
        mPanel = panel;
        mWatch = HgiDeviceWatch::create(hgi);
        if (const auto device = hgi ? render::GpuImage::sharingDevice() : std::nullopt)
        {
            std::string interopError;
            mInterop = HgiInterop::create(hgi, *device, interopError);
            if (!mInterop)
                log::info("Hydra viewport: frames go through the CPU: {}", interopError);
        }
        mOk = true;
    }

    ~HydraSession() override
    {
        *mAlive = false;
        finishLast();
        mInterop.reset();
        if (mIndex)
        {
            for (auto& [node, c] : mStages)
                mIndex->RemoveSceneIndex(c.world);
            mIndex->RemoveSceneIndex(mMaterialIndex);
            mIndex->RemoveSceneIndex(mLila);
        }
        mTasks.reset();
        mIndex.reset();
    }

    bool ok() const { return mOk; }

    void render(const vr::Request& r) override
    {
        LILA_ZONE();
        if (!r.graph || r.width <= 0 || r.height <= 0)
            return;
        finishLast();
        mLast = r;
        mRequested = std::chrono::steady_clock::now();
        // A new origin moves every prim, stages included: the full sync does that.
        if (sceneCurrent(r) && hydraOrigin(r.view.eye) == mSyncedOrigin)
        {
            mLila->syncView(viewContext(r));
            // Stage edits made outside Lila's graph still arrive.
            mMaterialIndex->ApplyPendingUpdates();
            for (auto& [node, c] : mStages)
                c.usd->ApplyPendingUpdates();
        }
        else
            sync(r);
        draw();
    }

    void sceneChanged(scene::Change c, NodeId id) override { mMaterials.changed(Graph::current(), c, id); }

    std::vector<vr::Setting> settings() const override
    {
        std::vector<vr::Setting> out;
        for (const HdRenderSettingDescriptor& d : mDelegate->GetRenderSettingDescriptors())
        {
            VtValue v = mDelegate->GetRenderSetting(d.key);
            nlohmann::json j = toJson(v.IsEmpty() ? d.defaultValue : v);
            if (!j.is_null())
                out.push_back({d.key.GetString(), d.name, std::move(j)});
        }
        return out;
    }

    nlohmann::json stats() const override
    {
        nlohmann::json delegate = nlohmann::json::object();
        for (const auto& [key, value] : mDelegate->GetRenderStats())
            if (nlohmann::json j = toJson(value); !j.is_null())
                delegate[key] = std::move(j);
        return {{"frames", mFrames}, {"executeMs", mExecuteMs}, {"converged", mTasks->IsConverged()}, {"delegate", std::move(delegate)}};
    }

    bool setSetting(const std::string& key, const nlohmann::json& value) override
    {
        finishLast();
        const TfToken k(key);
        VtValue current = mDelegate->GetRenderSetting(k);
        if (current.IsEmpty())
            for (const HdRenderSettingDescriptor& d : mDelegate->GetRenderSettingDescriptors())
                if (d.key == k)
                    current = d.defaultValue;
        VtValue v = toVt(value, current);
        if (v.IsEmpty())
            return false;
        mDelegate->SetRenderSetting(k, v);
        return true;
    }

    /** For tests: what the last sync changed among Lila's prims. */
    LilaSceneIndex::Counts lastSync() const { return mLila->lastSync(); }

private:
    /** Whether the scene indices already hold what r shows, so only the camera or size changed. */
    bool sceneCurrent(const vr::Request& r) const
    {
        const scene::isolation::Set* iso = r.view.isolation;
        return mSynced && r.sceneRevision == mSyncedRevision && r.time == mSyncedTime && iso == mSyncedIsolation &&
               (!iso || iso->version == mSyncedIsolationVersion);
    }

    /** What Lila's prims need from r's view; it refers to r, so it lives no longer. */
    static ViewContext viewContext(const vr::Request& r)
    {
        ViewContext view;
        view.isolation = r.view.isolation;
        // Request::view measures logical pixels, the frame device ones.
        const double device = r.view.width > 0.0 ? r.width / r.view.width : 1.0;
        view.unitsPerPixel = [&r, device](const glm::dvec3& at) { return r.view.unitsPerPixel(at) / device; };
        view.curvePixels = float(2.0 * device);
        view.curveColor = ui::themeLinear("curve", view.curveColor);
        view.origin = hydraOrigin(r.view.eye);
        return view;
    }

    void sync(const vr::Request& r)
    {
        LILA_ZONE();
        Graph& g = *r.graph;
        g.evaluate();
        const ViewContext view = viewContext(r);
        mLila->sync(g, view, [this](const std::string& slot) { return mMaterials.bind(slot); });
        mMaterials.sync(g, mLila->usedSlots());
        mMaterialIndex->ApplyPendingUpdates();
        syncStages(g, view, r.time);
        mHasLights = sceneHasLights();
        mSynced = true;
        mSyncedRevision = r.sceneRevision;
        mSyncedOrigin = view.origin;
        mSyncedTime = r.time;
        mSyncedIsolation = r.view.isolation;
        mSyncedIsolationVersion = r.view.isolation ? r.view.isolation->version : 0;
    }

    void syncStages(Graph& g, const ViewContext& view, double time)
    {
        std::set<NodeId> live;
        for (const usd::stage::HydraView& v : usd::stage::hydraViews(g))
        {
            if (!scene::effectivelyVisible(g, v.node))
                continue;
            live.insert(v.node);
            StageChain& c = mStages[v.node];
            if (!c.usd || c.stage != v.stage)
            {
                if (c.world)
                    mIndex->RemoveSceneIndex(c.world);
                c.stage = v.stage;
                TfRefPtr<StageOverrides> overrides;
                c.usd = UsdImagingSceneIndex::New(HdRetainedContainerDataSource::New(), [&overrides](const HdSceneIndexBaseRefPtr& in) {
                    overrides = StageOverrides::New(in);
                    return HdSceneIndexBaseRefPtr(overrides);
                });
                c.overrides = overrides;
                c.usd->SetStage(v.stage);
                c.world = StagePlacement::New(c.usd);
                mIndex->InsertSceneIndex(c.world, stageRoot(v.node));
            }
            c.usd->SetTime(UsdTimeCode(time * v.timeCodesPerSecond));
            c.usd->ApplyPendingUpdates();

            const glm::dmat4 fromWorld = glm::inverse(v.toWorld);
            std::map<SdfPath, StagePlacement::Placed> placed;
            std::set<SdfPath> blank;
            for (const usd::stage::HydraView::Link& l : v.links)
                if (const scene::Node* n = g.node(l.node))
                {
                    placed[l.prim] = {.matrix = fromWorld * n->as<glm::dmat4>(scene::Transform::kWorldMatrix) * (l.turned ? v.turnedToLila : v.toLila),
                                      .visible = scene::effectivelyVisible(g, l.node) && (!view.isolation || view.isolation->shows(l.node))};
                    if (drawnByLila(*n))
                        blank.insert(l.prim);
                }
            if (c.overrides)
                c.overrides->update(std::move(blank), std::set<SdfPath>(v.realized.begin(), v.realized.end()));
            c.world->update(glm::translate(glm::dmat4(1.0), -view.origin) * v.toWorld, std::move(placed));
        }
        for (auto it = mStages.begin(); it != mStages.end();)
        {
            if (live.count(it->first))
            {
                ++it;
                continue;
            }
            mIndex->RemoveSceneIndex(it->second.world);
            it = mStages.erase(it);
        }
    }

    bool sceneHasLights() const
    {
        for (const TfToken& type : HdLightPrimTypeTokens())
            if (type != HdPrimTypeTokens->simpleLight && mIndex->IsSprimTypeSupported(type))
                for (const SdfPath& p : mIndex->GetSprimSubtree(type, SdfPath::AbsoluteRootPath()))
                    if (!p.HasPrefix(SdfPath("/LilaTasks")))
                        return true;
        return false;
    }

    void draw()
    {
        LILA_ZONE();
        finishLast();
        const vr::Request& r = mLast;
        const glm::dmat4 proj = glProjection(r);
        mTasks->SetRenderBufferSize(GfVec2i(r.width, r.height));
        mTasks->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), r.width, r.height)));
        mTasks->SetFreeCameraMatrices(usd::toGf(r.view.view * glm::translate(glm::dmat4(1.0), mSyncedOrigin)), usd::toGf(proj));
        mTasks->SetLightingState(mHasLights ? mSceneLights : mEyeLight);
        HdTaskSharedPtrVector tasks = mTasks->GetRenderingTasks();
        // One frame for HgiVulkan, so it cleans up once rather than after every submit.
        if (mHgi)
            mHgi->StartFrame();
        {
            LILA_ZONE_N("Hydra Execute");
            const auto start = std::chrono::steady_clock::now();
            mEngine.Execute(mIndex.get(), &tasks);
            mExecuteMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            ++mFrames;
        }
        if (r.deliver)
            deliver(r, proj);
        if (mHgi)
            mHgi->EndFrame();
        if (mWatch && mWatch->lost())
        {
            deviceLost();
            return;
        }
        if (!mTasks->IsConverged())
            refineLater(r.serial);
    }

    /**
     * Hydra's GPU device is gone, so this session only repeats its last frame. Opens the panel's renderer again on a new
     * Hgi; after a few losses it falls back to Lila's own, since something in the scene keeps faulting the GPU.
     */
    void deviceLost()
    {
        if (mLostReported)
            return;
        mLostReported = true;
        static int losses = 0;
        const bool retry = ++losses <= 3;
        const std::string note = retry ? "Hydra's GPU device was lost (a GPU fault in the render delegate); restarting the Hydra viewport"
                                       : "Hydra's GPU device keeps getting lost; the viewport is back on Lila's renderer";
        log::error("{}", note);
        LILA_MESSAGE(note.c_str(), note.size());
        forgetHgi(mHgi);
        mainq::post([panel = mPanel, retry] {
            const std::string id = vr::selected(panel);
            if (id == vr::kLila)
                return;
            vr::select(panel, vr::kLila);
            if (retry)
                vr::select(panel, id);
        });
    }

    /** Progressive delegates: draw again a display refresh later, until they converge or a newer request comes. */
    void refineLater(uint64_t serial)
    {
        std::weak_ptr<bool> alive = mAlive;
        const auto wait = std::chrono::duration<double>(vr::refreshInterval(mPanel));
        std::thread([alive, serial, wait, this] {
            std::this_thread::sleep_for(wait);
            mainq::post([alive, serial, this] {
                if (const auto a = alive.lock(); a && *a && mLast.serial == serial)
                {
                    mRequested = std::chrono::steady_clock::now();
                    draw();
                }
            });
        }).detach();
    }

    /** Waits for the frame on its way to the viewport, then unmaps the buffers it was read from: Hydra may write them again. */
    void finishLast()
    {
        LILA_ZONE_N("Hydra wait for last frame");
        mWorker.wait();
        for (HdRenderBuffer* b : mMapped)
            b->Unmap();
        mMapped.clear();
    }

    void deliver(const vr::Request& r, const glm::dmat4& proj)
    {
        LILA_ZONE();
        HdRenderBuffer* color = mTasks->GetRenderOutput(HdAovTokens->color);
        if (!color)
            return;
        color->Resolve();
        HdRenderBuffer* depth = mTasks->GetRenderOutput(HdAovTokens->depth);
        if (depth)
            depth->Resolve();
        const bool gpu = deliverOnGpu(r, proj, color, depth);
        if (!gpu)
            deliverFromCpu(r, proj, color, depth);
        LILA_PLOT("Hydra frames on the GPU", int64_t(gpu));
    }

    static HgiTexture* hgiTexture(const HdRenderBuffer* b)
    {
        const VtValue v = b ? b->GetResource(false) : VtValue();
        return v.IsHolding<HgiTextureHandle>() ? v.UncheckedGet<HgiTextureHandle>().Get() : nullptr;
    }

    /** Delegates drawing with HgiVulkan on Lila's GPU: their AOVs are copied into images Lila's device shares, never leaving the GPU. */
    bool deliverOnGpu(const vr::Request& r, const glm::dmat4& proj, HdRenderBuffer* color, HdRenderBuffer* depth)
    {
        HgiTexture* c = mInterop ? hgiTexture(color) : nullptr;
        if (!c || !mInterop->copyable(c, true))
            return false;
        HgiTexture* d = hgiTexture(depth);
        if (d && (!mInterop->copyable(d, false) || d->GetDescriptor().dimensions != c->GetDescriptor().dimensions))
            d = nullptr;
        const GfVec3i size = c->GetDescriptor().dimensions;
        if (!targetsReady(size[0], size[1]))
            return false;
        const size_t slot = mNextTarget;
        if (!mInterop->copy(slot, c, d))
            return false;
        mNextTarget = (mNextTarget + 1) % mTargets.size();

        const Targets t = mTargets[slot];
        vr::Frame f;
        f.serial = r.serial;
        f.width = size[0];
        f.height = size[1];
        f.image = t.color;
        if (d)
        {
            f.depthImage = t.depth;
            f.glDepthProj = proj;
        }
        f.bottomUp = true;
        f.premultiplied = true;
        f.exposure = r.exposure;
        mWorker.run([interop = mInterop.get(), slot, t, f = std::move(f), deliver = r.deliver, requested = mRequested]() mutable {
            {
                LILA_ZONE_N("Hydra wait for GPU copy");
                interop->wait(slot);
            }
            t.color->acquireExternal();
            if (f.depthImage)
                t.depth->acquireExternal();
            deliver(std::move(f));
            plotLatency(requested);
        });
        return true;
    }

    /**
     * The shared colour and depth images for a w x h frame, made on Lila's device and imported into Hydra's. A ring of
     * them, so a copy never lands in an image a frame Lila is still drawing samples. False while they are on their way.
     */
    bool targetsReady(int w, int h)
    {
        constexpr size_t kTargets = 4;
        if (w != mTargetWidth || h != mTargetHeight)
        {
            mInterop->clearSlots();
            mTargets.clear();
            mImported = false;
            mNextTarget = 0;
            mTargetWidth = w;
            mTargetHeight = h;
            std::vector<render::GpuImageRef> all;
            for (size_t i = 0; i < kTargets; ++i)
            {
                Targets t;
                t.color = render::GpuImage::create(
                    {.width = w, .height = h, .format = render::TextureFormat::RGBA16F, .name = "Hydra colour", .shared = true});
                t.depth = render::GpuImage::create({.width = w, .height = h, .format = render::TextureFormat::R32F, .name = "Hydra depth", .shared = true});
                all.push_back(t.color);
                all.push_back(t.depth);
                mTargets.push_back(std::move(t));
            }
            mRealizing = render::GpuImage::realize(std::move(all));
        }
        if (mImported)
            return true;
        if (!mRealizing.valid() || mRealizing.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return false;
        std::string why;
        try
        {
            mRealizing.get();
        }
        catch (const std::exception& e)
        {
            why = e.what();
        }
        for (size_t i = 0; why.empty() && i < mTargets.size(); ++i)
        {
            const auto c = mTargets[i].color->sharedMemory(), d = mTargets[i].depth->sharedMemory();
            if (!c || !d)
                why = "Lila's device could not share the images";
            else if (!mInterop->setSlot(i, sharedTarget(*c), sharedTarget(*d)))
                why = "Hydra's device could not import them";
        }
        if (!why.empty())
        {
            log::warn("Hydra viewport: frames go through the CPU: {}", why);
            const std::string note = "Hydra frames go through the CPU: " + why;
            LILA_MESSAGE(note.c_str(), note.size());
            mInterop.reset();
            mTargets.clear();
            return false;
        }
        mImported = true;
        return true;
    }

    /** For traces: from the request (or refinement) to its frame handed to the viewport. */
    static void plotLatency(std::chrono::steady_clock::time_point requested)
    {
        [[maybe_unused]] const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - requested).count();
        LILA_PLOT("Hydra request to frame (ms)", ms);
    }

    static SharedTarget sharedTarget(const render::SharedImageMemory& m)
    {
        return {.handle = m.handle,
                .size = m.size,
                .format = uint32_t(m.format),
                .flags = uint32_t(m.flags),
                .usage = uint32_t(m.usage),
                .width = m.width,
                .height = m.height};
    }

    /**
     * Any other delegate: its buffers are mapped here and copied as they are on the worker, flipping, unpremultiplying,
     * exposing and unprojecting left to Lila's composite. They stay mapped until finishLast.
     */
    void deliverFromCpu(const vr::Request& r, const glm::dmat4& proj, HdRenderBuffer* color, HdRenderBuffer* depth)
    {
        const int w = int(color->GetWidth()), h = int(color->GetHeight());
        const HdFormat cf = color->GetFormat();
        LILA_ZONE_N("Hydra map");
        const auto* c = static_cast<const uint8_t*>(color->Map());
        mMapped.push_back(color);
        if (!c || w <= 0 || h <= 0)
            return;
        const uint8_t* d = nullptr;
        HdFormat df = HdFormatInvalid;
        if (depth && int(depth->GetWidth()) == w && int(depth->GetHeight()) == h)
        {
            d = static_cast<const uint8_t*>(depth->Map());
            mMapped.push_back(depth);
            df = depth->GetFormat();
        }
        mWorker.run([=, serial = r.serial, exposure = r.exposure, deliver = r.deliver, requested = mRequested] {
            vr::Frame f = cpuFrame(c, cf, d, df, w, h);
            f.serial = serial;
            f.bottomUp = true;
            f.premultiplied = true;
            f.exposure = exposure;
            if (!f.depth.empty())
                f.glDepthProj = proj;
            deliver(std::move(f));
            plotLatency(requested);
        });
    }

    /** A frame of a delegate's pixels in the nearest format Lila takes: RGBA8 and RGBA16F colour as they are, others as half floats. */
    static vr::Frame cpuFrame(const uint8_t* c, HdFormat cf, const uint8_t* d, HdFormat df, int w, int h)
    {
        LILA_ZONE();
        vr::Frame f;
        f.width = w;
        f.height = h;
        const size_t pixels = size_t(w) * size_t(h);
        if (cf == HdFormatUNorm8Vec4 || cf == HdFormatFloat16Vec4)
        {
            f.texelFormat = cf == HdFormatUNorm8Vec4 ? render::TextureFormat::RGBA8 : render::TextureFormat::RGBA16F;
            f.texels.assign(c, c + pixels * HdDataSizeOfFormat(cf));
        }
        else
        {
            f.texelFormat = render::TextureFormat::RGBA16F;
            f.texels.resize(pixels * 8);
            const size_t stride = HdDataSizeOfFormat(cf);
            const int components = int(HdGetComponentCount(cf));
            auto* out = reinterpret_cast<GfHalf*>(f.texels.data());
            withReader(cf, [&](auto read) {
                forRows(h, [&](int y) {
                    const uint8_t* p = c + size_t(y) * w * stride;
                    GfHalf* o = out + size_t(y) * w * 4;
                    for (int x = 0; x < w; ++x, p += stride, o += 4)
                    {
                        for (int k = 0; k < 3; ++k)
                            o[k] = GfHalf(read(p, std::min(k, components - 1)));
                        o[3] = GfHalf(components >= 4 ? read(p, 3) : 1.0f);
                    }
                });
            });
        }
        if (d)
        {
            f.depth.resize(pixels);
            if (df == HdFormatFloat32)
                std::memcpy(f.depth.data(), d, pixels * sizeof(float));
            else
            {
                const size_t stride = HdDataSizeOfFormat(df);
                withReader(df, [&](auto read) {
                    forRows(h, [&](int y) {
                        const uint8_t* p = d + size_t(y) * w * stride;
                        float* o = &f.depth[size_t(y) * w];
                        for (int x = 0; x < w; ++x, p += stride)
                            o[x] = read(p, 0);
                    });
                });
            }
        }
        return f;
    }

    struct Targets
    {
        render::GpuImageRef color, depth;
    };

    bool mOk = false;
    bool mStorm = false;
    int mPanel = 0;
    bool mLostReported = false;
    /** Declared before everything drawing with it, so it outlives them. */
    std::shared_ptr<Hgi> mHgiOwner;
    Hgi* mHgi = nullptr;
    std::unique_ptr<HgiDeviceWatch> mWatch;
    HdPluginRenderDelegateUniqueHandle mDelegate;
    HdDriver mDriver;
    std::unique_ptr<HdRenderIndex> mIndex;
    std::unique_ptr<HdxTaskController> mTasks;
    HdEngine mEngine;
    TfRefPtr<LilaSceneIndex> mLila;
    MaterialLibrary mMaterials;
    UsdImagingSceneIndexRefPtr mMaterialIndex;
    std::map<NodeId, StageChain> mStages;
    GlfSimpleLightingContextRefPtr mEyeLight, mSceneLights;
    std::shared_ptr<bool> mAlive = std::make_shared<bool>(true);
    vr::Request mLast;
    std::chrono::steady_clock::time_point mRequested;
    uint64_t mFrames = 0;
    double mExecuteMs = 0.0;

    bool mSynced = false;
    uint64_t mSyncedRevision = 0;
    glm::dvec3 mSyncedOrigin{0.0};
    double mSyncedTime = 0.0;
    const scene::isolation::Set* mSyncedIsolation = nullptr;
    uint64_t mSyncedIsolationVersion = 0;
    bool mHasLights = false;

    std::unique_ptr<HgiInterop> mInterop;
    std::vector<Targets> mTargets;
    std::future<void> mRealizing;
    bool mImported = false;
    size_t mNextTarget = 0;
    int mTargetWidth = 0, mTargetHeight = 0;
    std::vector<HdRenderBuffer*> mMapped;
    Worker mWorker;
};
} // namespace

USD_BACKEND_EXPORT void* lilaHydraOpen(const char* plugin, int panel, const char* settings, std::string* error)
{
    try
    {
        nlohmann::json saved = nlohmann::json::parse(settings ? settings : "{}", nullptr, false);
        auto s = std::make_unique<HydraSession>(plugin, panel, saved.is_object() ? saved : nlohmann::json::object(), *error);
        if (!s->ok())
            return nullptr;
        return static_cast<vr::Session*>(s.release());
    }
    catch (const std::exception& e)
    {
        *error = e.what();
        return nullptr;
    }
}
