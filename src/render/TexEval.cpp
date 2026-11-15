#include "render/TexEval.h"
#include "render/TexEvalInternal.h"

#include "core/Progress.h"

#include <optional>
#include <tbb/parallel_for.h>

namespace lila::render::texgpu
{
namespace
{
Route nativeRoute(const std::string& type, bool kernels = true)
{
    if (findNative(type))
        return Route::Registered;
    if (kernels && tex::hasNativeKernels(type) && tex::findNode(type))
        return Route::Kernels;
    if (type == "blur")
        return Route::Builtin;
    if (tex::nativeCpu(type))
        return Route::Cpu;
    return Route::None;
}

/**
 * Rewrites every OpSRem as a - (a / b) * b. Some drivers (NVIDIA 5080, seen in tests) evaluate OpSRem with a negative
 * dividend as an unsigned remainder, which breaks the wrapped cell lookups of every cellular node at the tile's edges.
 */
void rewriteSignedRemainder(std::vector<uint32_t>& code)
{
    constexpr uint32_t kSRem = 138, kSDiv = 135, kIMul = 132, kISub = 130;
    if (code.size() < 5)
        return;
    std::vector<uint32_t> out(code.begin(), code.begin() + 5);
    out.reserve(code.size() + 64);
    uint32_t bound = code[3];
    for (size_t i = 5; i < code.size();)
    {
        const uint32_t op = code[i] & 0xffffu, n = std::max<uint32_t>(code[i] >> 16, 1);
        if (op == kSRem && n == 5 && i + 5 <= code.size())
        {
            const uint32_t type = code[i + 1], result = code[i + 2], a = code[i + 3], b = code[i + 4];
            const uint32_t q = bound++, m = bound++;
            out.insert(out.end(), {(5u << 16) | kSDiv, type, q, a, b, (5u << 16) | kIMul, type, m, q, b, (5u << 16) | kISub, type, result, a, m});
        }
        else
            out.insert(out.end(), code.begin() + std::ptrdiff_t(i), code.begin() + std::ptrdiff_t(std::min(i + n, code.size())));
        i += n;
    }
    out[3] = bound;
    code = std::move(out);
}

/**
 * A fused pass as the GPU runs it: the arena and the output as 32-bit pairs, so each buffer can hold float4 texels (two
 * pairs) or half4 texels (one, descriptor flag bit 0), and the output written through texOut, which packs halves when
 * the header says so. Empty when the source isn't in the shape Plan.cpp emits; the pass then runs as written.
 */
std::string gpuSource(const std::string& source)
{
    static const std::string prelude = tex::kPrelude;
    static const std::string gpuPrelude = [] {
        std::string p = tex::kPrelude;
        auto swap = [&p](const std::string& from, const std::string& to) {
            const size_t at = p.find(from);
            if (at == std::string::npos)
                return false;
            p.replace(at, from.size(), to);
            return true;
        };
        bool ok = swap("    float4 uv;\n};", "    float4 uv;\n    uint4 info;\n};");
        ok = ok && swap("StructuredBuffer<float4> gArena;\nRWStructuredBuffer<float4> gOut;", R"(StructuredBuffer<uint2> gArena;
RWStructuredBuffer<uint2> gOut;

float4 texWords(uint2 a, uint2 b) { return float4(asfloat(a.x), asfloat(a.y), asfloat(b.x), asfloat(b.y)); }
float4 texHalves(uint2 q) { return float4(f16tof32(q.x), f16tof32(q.x >> 16), f16tof32(q.y), f16tof32(q.y >> 16)); }
void texOut(uint i, float4 v)
{
    if (gPass.info.x != 0)
        gOut[i] = uint2(f32tof16(v.x) | (f32tof16(v.y) << 16), f32tof16(v.z) | (f32tof16(v.w) << 16));
    else
    {
        gOut[2 * i] = asuint(v.xy);
        gOut[2 * i + 1] = asuint(v.zw);
    }
})");
        ok = ok && swap("float2 texel() { return gPass.res.zw; }\nfloat2 texResolution() { return gPass.res.xy; }",
                        "float2 texel() { return gPass.res.zw * gPass.uv.zw; }\nfloat2 texResolution() { return gPass.res.xy / gPass.uv.zw; }");
        // Buffer reads: half texels (descriptor flag 1), and regions (flag 2) holding part of a tile for a page, placed by
        // the gU slot in the flags' upper bits (its first texel x, y and the level's width, height), clamped at their edges.
        const size_t from = p.find("float4 texFetchBuf(uint slot, int2 p)");
        const std::string tail = "    return lerp(lerp(a, b, t.x), lerp(c, e, t.x), t.y);\n}\n";
        const size_t to = from == std::string::npos ? std::string::npos : p.find(tail, from);
        if (!ok || to == std::string::npos)
            return std::string();
        p.replace(from, to + tail.size() - from, R"(float4 texFetchAt(float4 d, uint i)
{
    if ((asuint(d.w) & 1u) != 0)
        return texHalves(gArena[asuint(d.x) + i]);
    return texWords(gArena[asuint(d.x) + 2 * i], gArena[asuint(d.x) + 2 * i + 1]);
}

float4 texFetchBuf(uint slot, int2 p)
{
    float4 d = gU[slot];
    int w = int(asuint(d.y));
    int h = int(asuint(d.z));
    uint flags = asuint(d.w);
    int x, y;
    if ((flags & 2u) != 0)
    {
        float4 t = gU[flags >> 8];
        x = clamp(p.x - int(t.x), 0, w - 1);
        y = clamp(p.y - int(t.y), 0, h - 1);
    }
    else
    {
        x = ((p.x % w) + w) % w;
        y = ((p.y % h) + h) % h;
    }
    return texFetchAt(d, uint(y * w + x));
}

float4 texReadBuf(uint slot, float2 uv)
{
    float4 d = gU[slot];
    float2 size = float2(float(asuint(d.y)), float(asuint(d.z)));
    uint flags = asuint(d.w);
    if ((flags & 2u) != 0)
        size = gU[flags >> 8].zw;
    float2 p = float2(uv.x, 1.0 - uv.y) * size - 0.5;
    float2 f = floor(p);
    float2 t = p - f;
    int2 i = int2(f);
    float4 a = texFetchBuf(slot, i);
    float4 b = texFetchBuf(slot, i + int2(1, 0));
    float4 c = texFetchBuf(slot, i + int2(0, 1));
    float4 e = texFetchBuf(slot, i + int2(1, 1));
    return lerp(lerp(a, b, t.x), lerp(c, e, t.x), t.y);
}
)");
        return p;
    }();
    if (gpuPrelude.empty())
        return {};
    const size_t at = source.find(prelude);
    const std::string store = "    gOut[id.y * w + id.x] = ";
    const size_t out = source.find(store);
    if (at == std::string::npos || out == std::string::npos)
        return {};
    const size_t end = source.find(";\n", out);
    if (end == std::string::npos)
        return {};
    std::string s = source;
    s.replace(end, 1, ");");
    s.replace(out, store.size(), "    texOut(id.y * w + id.x, ");
    s.replace(at, prelude.size(), gpuPrelude);
    return s;
}

/** SPIR-V for Slang source, compiled once per hash for the life of the process. */
Code spirvFor(const std::string& source, uint64_t hash, bool& compiled, std::string& err)
{
    Shared& s = shared();
    {
        std::scoped_lock lock(s.mutex);
        if (auto it = s.spirv.find(hash); it != s.spirv.end())
        {
            ++s.hits;
            compiled = false;
            return it->second;
        }
    }
    std::vector<uint32_t> code = tex::slangrt::spirv(source, &err);
    if (code.empty())
        return nullptr;
    rewriteSignedRemainder(code);
    auto ptr = std::make_shared<const std::vector<uint32_t>>(std::move(code));
    std::scoped_lock lock(s.mutex);
    ++s.compiles;
    compiled = true;
    return s.spirv.emplace(hash, ptr).first->second;
}

void releaseShared(VulkanContext& vk)
{
    Shared& s = shared();
    std::vector<std::shared_ptr<Evaluator::State>> live;
    {
        std::scoped_lock lock(s.mutex);
        for (auto& w : s.live)
            if (auto p = w.lock())
                live.push_back(std::move(p));
        s.live.clear();
        for (auto& p : live)
            s.live.push_back(p);
    }
    for (auto& st : live)
        st->release(vk);
    for (auto& [name, k] : s.kernels)
        compute::destroy(vk, k.layout);
    s.kernels.clear();
    compute::destroy(vk, s.passLayout);
    s.passLayout = VK_NULL_HANDLE;
    if (s.setLayout)
        vkDestroyDescriptorSetLayout(vk.device, s.setLayout, nullptr);
    s.setLayout = VK_NULL_HANDLE;
}

void installStopHook()
{
    static std::once_flag once;
    std::call_once(once, [] { Renderer::get().addStopHook([](VulkanContext& vk) { releaseShared(vk); }); });
}
} // namespace

// Leaked on purpose: evaluators may be dropped during static destruction.
Shared& shared()
{
    static Shared* s = new Shared;
    return *s;
}

VkPipeline kernel(VulkanContext& vk, const char* file, const char* entry, uint32_t push, VkPipelineLayout& layout)
{
    Kernel& k = shared().kernels[entry];
    if (!k.layout)
    {
        std::string err;
        k.code = compute::loadSpirv(file, &err);
        if (k.code.empty())
            throw std::runtime_error("texture graph GPU: " + err);
        k.hash = compute::spirvHash(k.code);
        k.push = push;
        k.layout = compute::pipelineLayout(vk, push, kCompute);
    }
    layout = k.layout;
    return compute::sharedPipelines().get(vk, k.layout, compute::layoutKey(push, kCompute), k.code, k.hash, entry);
}

void ensurePassLayout(VulkanContext& vk)
{
    Shared& s = shared();
    static std::mutex m;
    std::scoped_lock lock(m);
    if (s.passLayout)
        return;
    VkDescriptorSetLayoutBinding b[4]{};
    for (uint32_t i = 0; i < 4; ++i)
    {
        b[i].binding = i;
        b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = kCompute;
    }
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 4;
    ci.pBindings = b;
    if (vkCreateDescriptorSetLayout(vk.device, &ci, nullptr, &s.setLayout) != VK_SUCCESS)
        throw std::runtime_error("texture graph GPU: cannot create the pass descriptor layout");
    s.passLayout = compute::pipelineLayout(vk, 0, kCompute, std::span(&s.setLayout, 1));
}

void dispatchLinear(VkCommandBuffer cmd, uint64_t items)
{
    const uint64_t groups = std::max<uint64_t>((items + 63) / 64, 1);
    const uint32_t gx = uint32_t(std::min<uint64_t>(groups, 65535));
    vkCmdDispatch(cmd, gx, uint32_t((groups + gx - 1) / gx), 1);
}

uint32_t groupsX(uint64_t items)
{
    return uint32_t(std::min<uint64_t>(std::max<uint64_t>((items + 63) / 64, 1), 65535));
}

NativeGpuFn findNative(const std::string& type)
{
    Shared& s = shared();
    std::scoped_lock lock(s.mutex);
    auto it = s.natives.find(type);
    return it == s.natives.end() ? NativeGpuFn() : it->second;
}

namespace
{
/** Tells the user, once per node type, that a GPU evaluation runs this native on the CPU, which stalls the viewport. */
void warnCpuRoute(const std::string& type, const std::string& why)
{
    static std::mutex mutex;
    static std::set<std::string> warned;
    {
        std::scoped_lock lock(mutex);
        if (!warned.insert(type).second)
            return;
    }
    log::warn("texture graph: {} runs on the CPU, not the GPU, so texture sets using it are slow and stall the viewport ({})", type, why);
}

/** Picks a native pass's route and, for kernels, compiles its steps. Empty or why it can't run. */
std::string prepareNative(Prepared& prep, const Plan& plan, size_t i)
{
    const Pass& p = plan.passes[i];
    Route route = nativeRoute(p.nativeType);
    std::string cpuWhy = "it has no GPU form";
    if (route == Route::Kernels)
    {
        const BufferDesc& out = plan.buffers.at(size_t(p.output));
        auto param = [&](const std::string& id, int c) {
            auto it = p.paramSlots.find(id);
            return it == p.paramSlots.end() ? 0.0f : plan.uniforms[size_t(it->second) * 4 + size_t(c)];
        };
        tex::NativeSchedule sched = tex::nativeSchedule(p.nativeType, out.width > 0 ? out.width : plan.width, out.height > 0 ? out.height : plan.height, param);
        std::vector<std::pair<Code, uint64_t>> steps;
        for (const tex::NativeStep& st : sched.steps)
        {
            std::string err;
            const std::string source = tex::nativeKernelSource(sched.file, st.entry, &err);
            if (source.empty())
            {
                cpuWhy = err;
                steps.clear();
                break;
            }
            const uint64_t hash = tex::hash64(source);
            bool compiled = false;
            Code code = spirvFor(source, hash, compiled, err);
            if (!code)
                return "native kernel " + sched.file + "." + st.entry + " failed to compile:\n" + err;
            ++(compiled ? prep.compiled : prep.cached);
            steps.emplace_back(std::move(code), hash);
        }
        if (steps.empty())
            route = nativeRoute(p.nativeType, false);
        else
        {
            VkDeviceSize texels = 0;
            for (const auto& sc : sched.scratch)
                texels += (VkDeviceSize(sc[0]) * VkDeviceSize(sc[1]) + 15) & ~VkDeviceSize(15);
            prep.scratchTexels = std::max(prep.scratchTexels, texels);
            prep.schedules[i] = std::move(sched);
            prep.stepCode[i] = std::move(steps);
        }
    }
    if (route == Route::None)
        return "no implementation of native node " + p.nativeType;
    if (route == Route::Cpu)
        warnCpuRoute(p.nativeType, cpuWhy);
    prep.routes[i] = route;
    return {};
}
} // namespace

namespace
{
/** The status bar's progress title while a plan's shaders compile. */
constexpr const char* kCompilingTitle = "Compiling texture shaders";

/** Fused passes whose SPIR-V this process has not compiled yet. */
size_t uncompiledPasses(const Plan& plan, const Prepared& prep)
{
    Shared& s = shared();
    std::scoped_lock lock(s.mutex);
    size_t n = 0;
    for (size_t i = 0; i < plan.passes.size(); ++i)
        if (plan.passes[i].kind != PassKind::Native && !s.spirv.contains(prep.codeHash[i]))
            ++n;
    return n;
}

/**
 * Has the driver build the plan's compute pipelines here, on the calling (worker) thread: built in a render-thread slice,
 * each new one stalls every viewport for as long as the driver takes, which can be most of a second.
 */
void createPipelines(const Plan& plan, const Prepared& prep, std::optional<progress::Scope>& compiling)
{
    LILA_ZONE();
    std::shared_lock deviceUse(compute::offThreadDeviceUse());
    VulkanContext* vk = Renderer::get().context();
    if (!vk)
        return;
    const uint64_t passKey = compute::layoutKey(0, kCompute, "texgraph");
    std::vector<std::tuple<const std::vector<uint32_t>*, uint64_t, const char*>> todo;
    for (size_t i = 0; i < plan.passes.size(); ++i)
    {
        const Pass& p = plan.passes[i];
        if (p.kind != PassKind::Native && prep.code[i] && !compute::sharedPipelines().contains(passKey, p.hash, p.entry.c_str()))
            todo.emplace_back(prep.code[i].get(), p.hash, p.entry.c_str());
        for (const auto& [code, hash] : prep.stepCode[i])
            if (code && !compute::sharedPipelines().contains(passKey, hash, "texMain"))
                todo.emplace_back(code.get(), hash, "texMain");
    }
    if (todo.empty())
        return;
    if (compiling)
        compiling->setTotal(compiling->total() + double(todo.size()));
    else
        compiling.emplace(kCompilingTitle, double(todo.size()), progress::Silent);
    ensurePassLayout(*vk);
    tbb::parallel_for(size_t(0), todo.size(), size_t(1), [&](size_t i) {
        const auto& [code, hash, entry] = todo[i];
        try
        {
            compute::sharedPipelines().get(*vk, shared().passLayout, passKey, *code, hash, entry);
            compiling->step();
        }
        catch (const std::exception&)
        {
            // The render thread retries when it records the pass and reports the failure there.
        }
    });
}
} // namespace

// -------------------------------------------------------------------------------------------------------- Evaluator

Evaluator::Evaluator() : m(std::make_shared<State>())
{
    installStopHook();
    Shared& s = shared();
    std::scoped_lock lock(s.mutex);
    std::erase_if(s.live, [](const std::weak_ptr<State>& w) { return w.expired(); });
    s.live.push_back(m);
}

Evaluator::~Evaluator()
{
    Renderer& r = Renderer::get();
    if (r.running())
        r.runTask([st = m](VulkanContext& vk) {
            st->release(vk);
            return true;
        });
}

namespace
{
std::shared_ptr<const image::Image> blackImage()
{
    static auto black = std::make_shared<const image::Image>(image::Image{1, 1, {0.0f, 0.0f, 0.0f, 1.0f}});
    return black;
}
} // namespace

namespace
{
/** Compiles the plan's passes (blocking) and makes their pipelines, into prep. Empty, or why the plan can't run. */
std::string compilePlan(const Plan& plan, std::unique_ptr<Prepared>& prep)
{
    prep = std::make_unique<Prepared>();
    prep->code.resize(plan.passes.size());
    prep->codeHash.resize(plan.passes.size());
    std::vector<std::string> gpuForms(plan.passes.size());
    prep->gpuForm = true;
    for (size_t i = 0; i < plan.passes.size() && prep->gpuForm; ++i)
        if (plan.passes[i].kind == PassKind::Fused)
        {
            gpuForms[i] = gpuSource(plan.passes[i].source);
            prep->gpuForm = !gpuForms[i].empty();
        }
    prep->routes.assign(plan.passes.size(), Route::None);
    prep->schedules.resize(plan.passes.size());
    prep->stepCode.resize(plan.passes.size());
    for (size_t i = 0; i < plan.passes.size(); ++i)
        if (plan.passes[i].kind == PassKind::Native)
            if (std::string err = prepareNative(*prep, plan, i); !err.empty())
                return err;
    // The other passes compile side by side: each is seconds of Slang the first time it is seen.
    for (size_t i = 0; i < plan.passes.size(); ++i)
        if (plan.passes[i].kind != PassKind::Native)
            prep->codeHash[i] = prep->gpuForm ? tex::hash64(gpuForms[i]) : plan.passes[i].hash;
    std::optional<progress::Scope> compiling;
    if (const size_t n = uncompiledPasses(plan, *prep))
        compiling.emplace(kCompilingTitle, double(n), progress::Silent);
    std::vector<std::string> errors(plan.passes.size());
    std::vector<uint8_t> fresh(plan.passes.size(), 0);
    tbb::parallel_for(size_t(0), plan.passes.size(), size_t(1), [&](size_t i) {
        const Pass& p = plan.passes[i];
        if (p.kind == PassKind::Native)
            return;
        bool compiled = false;
        prep->code[i] = spirvFor(prep->gpuForm ? gpuForms[i] : p.source, prep->codeHash[i], compiled, errors[i]);
        fresh[i] = compiled ? 1 : 0;
        if (compiled && compiling)
            compiling->step();
    });
    for (size_t i = 0; i < plan.passes.size(); ++i)
    {
        const Pass& p = plan.passes[i];
        if (p.kind == PassKind::Native)
            continue;
        if (!prep->code[i])
            return "pass for " + p.node + " failed to compile:\n" + errors[i];
        ++(fresh[i] ? prep->compiled : prep->cached);
    }
    createPipelines(plan, *prep, compiling);
    compiling.reset();
    return {};
}
} // namespace

std::string precompile(const tex::Plan& plan)
{
    LILA_ZONE();
    if (!plan.error.empty())
        return plan.error;
    if (plan.passes.empty())
        return "the plan has no passes";
    std::unique_ptr<Prepared> prep;
    return compilePlan(plan, prep);
}

std::string Evaluator::setPlan(Plan plan, Inputs inputs, std::vector<StoreTarget> stores)
{
    LILA_ZONE();
    if (!plan.error.empty())
        return plan.error;
    if (plan.passes.empty())
        return "the plan has no passes";
    std::unique_ptr<Prepared> prep;
    if (std::string err = compilePlan(plan, prep); !err.empty())
        return err;

    // Buffers no pass touches and no output names (those of outputs another evaluation makes) take no room and load nothing.
    prep->unused.assign(plan.buffers.size(), 1);
    for (const Pass& p : plan.passes)
    {
        prep->unused[size_t(p.output)] = 0;
        for (int r : p.reads)
            prep->unused[size_t(r)] = 0;
        for (const auto& [id, r] : p.inputBuffers)
            prep->unused[size_t(r)] = 0;
    }
    for (const auto& [name, id] : plan.outputs)
        prep->unused[size_t(id)] = 0;

    std::vector<std::string> warnings;
    prep->sizes.resize(plan.buffers.size());
    for (const BufferDesc& b : plan.buffers)
    {
        if (prep->unused[size_t(b.id)])
        {
            prep->sizes[size_t(b.id)] = {1, 1};
            continue;
        }
        std::pair<int, int> size{b.width, b.height};
        if (b.kind == BufferKind::Image)
        {
            std::shared_ptr<const image::Image> img = blackImage();
            if (!b.name.empty())
            {
                const fs::path path = tex::resolveImagePath(b.name, inputs.imageRoot);
                std::error_code ec;
                const fs::file_time_type time = fs::last_write_time(path, ec);
                const std::string key = path.string();
                std::shared_ptr<const image::Image> cached;
                {
                    std::scoped_lock lock(m->mutex);
                    if (auto it = m->imageCache.find(key); it != m->imageCache.end() && !ec && it->second.time == time)
                        cached = it->second.image;
                }
                if (cached)
                    img = cached;
                else
                {
                    std::string err;
                    image::Image read = image::read(path, &err);
                    if (read.width > 0)
                    {
                        img = std::make_shared<const image::Image>(std::move(read));
                        std::scoped_lock lock(m->mutex);
                        m->imageCache[key] = {time, img};
                    }
                    else
                        warnings.push_back("image " + b.name + ": " + (err.empty() ? "can't read" : err));
                }
            }
            prep->images[b.id] = img;
            size = {img->width, img->height};
        }
        else if (b.kind == BufferKind::External)
        {
            size = {plan.width, plan.height};
            // A GPU image larger than the plan (a preview of a big tile) loads at the plan's size. CPU texels upload
            // as they are, so they keep their own size, as on the CPU.
            if (auto g = inputs.gpuExternals.find(b.name); g != inputs.gpuExternals.end() && g->second)
            {
                if (g->second->width() < plan.width || g->second->height() < plan.height)
                    size = {g->second->width(), g->second->height()};
            }
            else if (auto c = inputs.cpuExternals.find(b.name); c != inputs.cpuExternals.end() && c->second.width > 0)
                size = {c->second.width, c->second.height};
        }
        if (size.first < 1 || size.second < 1)
            size = {plan.width, plan.height};
        prep->sizes[size_t(b.id)] = size;
    }

    // Half texels for buffers that only fused passes touch and that don't need float precision: CPU externals and uv
    // fills (missing mesh maps) hold UDIM coordinates and 32-bit maps, so they stay float.
    prep->half.assign(plan.buffers.size(), 0);
    if (prep->gpuForm && inputs.halfBuffers)
    {
        std::vector<uint8_t> native(plan.buffers.size(), 0);
        for (const Pass& p : plan.passes)
            if (p.kind == PassKind::Native)
            {
                native[size_t(p.output)] = 1;
                for (int r : p.reads)
                    native[size_t(r)] = 1;
            }
        for (const BufferDesc& b : plan.buffers)
        {
            bool h = b.format != "rgba32f" && !native[size_t(b.id)];
            if (b.kind == BufferKind::External)
            {
                auto g = inputs.gpuExternals.find(b.name);
                h = h && g != inputs.gpuExternals.end() && g->second && g->second->format() != TextureFormat::R32F;
            }
            prep->half[size_t(b.id)] = h ? 1 : 0;
        }
    }

    prep->pagePass.assign(plan.passes.size(), 0);
    prep->regionExternal.assign(plan.buffers.size(), 0);
    if (inputs.pages)
    {
        if (!prep->gpuForm)
            return "the plan's passes can't run per page";
        if (!stores.empty())
            return "a pages plan has no stores";
        std::vector<int> readers(plan.buffers.size(), 0);
        for (const Pass& p : plan.passes)
            for (int r : p.reads)
                ++readers[size_t(r)];
        for (size_t i = 0; i < plan.passes.size(); ++i)
        {
            const Pass& p = plan.passes[i];
            prep->pagePass[i] = p.kind == PassKind::Fused && plan.buffers[size_t(p.output)].kind == BufferKind::Output && readers[size_t(p.output)] == 0;
        }
        // An External every reader of is a page pass reading it at its own uv.
        for (const BufferDesc& b : plan.buffers)
        {
            if (b.kind != BufferKind::External)
                continue;
            bool region = readers[size_t(b.id)] > 0;
            for (size_t i = 0; i < plan.passes.size() && region; ++i)
            {
                const Pass& p = plan.passes[i];
                if (std::find(p.reads.begin(), p.reads.end(), b.id) == p.reads.end())
                    continue;
                region = prep->pagePass[i] && std::find(p.local.begin(), p.local.end(), b.id) != p.local.end();
            }
            prep->regionExternal[size_t(b.id)] = region;
        }
        for (size_t b = 0; b < plan.buffers.size(); ++b)
            if (prep->regionExternal[b])
            {
                prep->sizes[b] = {1, 1};
                prep->half[b] = 0;
            }
        for (size_t i = 0; i < plan.passes.size(); ++i)
            if (prep->pagePass[i])
            {
                prep->sizes[size_t(plan.passes[i].output)] = {1, 1};
                prep->half[size_t(plan.passes[i].output)] = 0;
            }
    }

    prep->storeSources.resize(stores.size());
    for (size_t t = 0; t < stores.size(); ++t)
        for (size_t c = 0; c < 4; ++c)
        {
            const std::string& name = stores[t].channels[c].output;
            if (name.empty())
            {
                prep->storeSources[t][c] = -1;
                continue;
            }
            auto it = plan.outputs.find(name);
            if (it == plan.outputs.end())
                return "the plan has no output " + name;
            prep->storeSources[t][c] = it->second;
        }

    prep->plan = std::move(plan);
    prep->inputs = std::move(inputs);
    prep->stores = std::move(stores);
    prep->warnings = std::move(warnings);
    std::scoped_lock lock(m->mutex);
    m->pending = std::move(prep);
    m->pendingExternals.reset();
    m->done = false;
    return {};
}

bool Evaluator::step(VulkanContext& vk, double budgetMs)
{
    LILA_ZONE();
    State& st = *m;
    st.adopt(vk.properties.limits.maxStorageBufferRange);
    {
        std::unique_ptr<std::map<std::string, GpuImageRef>> externals;
        {
            std::scoped_lock lock(st.mutex);
            externals = std::move(st.pendingExternals);
        }
        if (externals && st.cur)
        {
            st.cur->inputs.gpuExternals = std::move(*externals);
            st.refresh = true;
        }
    }
    if (st.cur && st.refresh.exchange(false))
        st.markExternals();
    auto pendingNow = [&] {
        std::scoped_lock lock(st.mutex);
        return st.pending != nullptr;
    };
    if (!st.cur || !st.anyWork())
    {
        st.done = !pendingNow();
        return st.done;
    }
    if (st.trimmed)
    {
        st.dirtyAll();
        st.trimmed = false;
    }
    st.allocate(vk);
    vkResetDescriptorPool(vk.device, st.pool, 0);
    st.framesUsed = 0;
    st.cpuNext = false;
    st.sliceLoaded = st.sliceComputed = st.sliceStored = 0;
    std::vector<compute::Buffer> temps;
    Stats delta;
    double predicted = 0.0, ms = 0.0;
    try
    {
        ms = st.sub.run(vk, [&](VkCommandBuffer cmd) {
            if (!st.recordLoads(vk, cmd, temps, budgetMs))
                return;
            st.arenaFresh = false;
            if (st.uniformsDirty)
            {
                std::memcpy(st.uniformStaging.mapped, st.uniformValues.data(), st.uniformValues.size() * sizeof(float));
                vmaFlushAllocation(vk.allocator, st.uniformStaging.alloc, 0, VK_WHOLE_SIZE);
                compute::copy(cmd, st.uniformStaging, st.uniforms, st.uniformValues.size() * sizeof(float));
                st.uniformsDirty = false;
            }
            compute::memoryBarrier(cmd);
            predicted = st.recordPasses(vk, cmd, budgetMs, delta);
            if (st.passIndex >= st.cur->plan.passes.size())
                st.recordStores(vk, cmd, temps, delta);
        });
    }
    catch (...)
    {
        for (compute::Buffer& b : temps)
            compute::destroy(vk, b);
        throw;
    }
    for (compute::Buffer& b : temps)
        compute::destroy(vk, b);
    if (predicted <= 0.0 && st.sliceLoaded > 0 && ms > 0.5)
        st.loadRate = std::clamp(compute::adaptRate(st.loadRate, double(st.sliceLoaded) / st.loadRate, ms), 1.0e4, 1.0e8);
    if (predicted > 1.0)
        st.rate = compute::adaptRate(st.rate, predicted, ms);
    if (st.cpuNext)
    {
        ms += st.runCpuNative(vk);
        delta.passesRun += 1;
    }
    {
        std::scoped_lock lock(st.mutex);
        st.stats.passesRun += delta.passesRun;
        st.stats.dispatches += delta.dispatches;
        st.stats.stores += delta.stores;
        st.stats.pipelinesCreated += delta.pipelinesCreated;
        st.stats.slices += 1;
        st.stats.gpuMs += ms;
        st.stats.texelsLoaded += st.sliceLoaded;
        st.stats.texelsComputed += st.sliceComputed;
        st.stats.texelsStored += st.sliceStored;
    }
    st.done = !st.anyWork() && !pendingNow();
    return st.done;
}

bool Evaluator::trim(VulkanContext& vk)
{
    State& st = *m;
    if (!idle() || !st.cur || st.cur->inputs.pages || !st.arena.buffer)
        return false;
    st.freeDevice(vk);
    st.trimmed = true;
    st.arenaFresh = true;
    return true;
}

void Evaluator::setExternals(std::map<std::string, GpuImageRef> externals)
{
    std::scoped_lock lock(m->mutex);
    m->pendingExternals = std::make_unique<std::map<std::string, GpuImageRef>>(std::move(externals));
    m->done = false;
}

void Evaluator::refreshExternals()
{
    m->refresh = true;
    m->done = false;
}

bool Evaluator::takeStored(std::array<float, 4>& uv)
{
    State& st = *m;
    if (st.stored.empty())
        return false;
    uv = {st.stored.x0, st.stored.y0, st.stored.x1, st.stored.y1};
    st.stored = {};
    return true;
}

void Evaluator::storeAgain(const std::array<float, 4>& uv)
{
    State& st = *m;
    if (!st.cur)
        return;
    const Rect r{uv[0], uv[1], uv[2], uv[3]};
    for (size_t t = 0; t < st.storeDirty.size(); ++t)
    {
        st.storeRect[t] = st.storeDirty[t] ? st.storeRect[t].unite(r) : r;
        st.storeDirty[t] = 1;
    }
    st.done = false;
}

bool Evaluator::idle() const
{
    std::scoped_lock lock(m->mutex);
    return !m->pending && m->done;
}

Stats Evaluator::stats() const
{
    std::scoped_lock lock(m->mutex);
    return m->stats;
}

std::map<std::string, image::Image> Evaluator::readOutputs()
{
    Renderer& r = Renderer::get();
    if (!r.running())
        throw std::runtime_error("texture graph GPU: the renderer is not running");
    std::map<std::string, image::Image> out;
    std::shared_ptr<State> st = m;
    r.runTask([&out, st](VulkanContext& vk) {
         if (!st->cur || !st->arena.buffer)
             return true;
         VkDeviceSize total = 0;
         std::vector<std::pair<std::string, VkDeviceSize>> places;
         for (const auto& [name, id] : st->cur->plan.outputs)
         {
             places.emplace_back(name, total);
             const auto [w, h] = st->cur->sizes[size_t(id)];
             total += VkDeviceSize(w) * VkDeviceSize(h) * st->elemBytes(size_t(id));
         }
         compute::Buffer host = compute::makeBuffer(vk, total, 0, compute::Host::Read);
         try
         {
             st->sub.run(vk, [&](VkCommandBuffer cmd) {
                 for (const auto& [name, at] : places)
                 {
                     const int id = st->cur->plan.outputs.at(name);
                     const auto [w, h] = st->cur->sizes[size_t(id)];
                     VkBufferCopy region{st->offset[size_t(id)] * kTexel, at, VkDeviceSize(w) * VkDeviceSize(h) * st->elemBytes(size_t(id))};
                     vkCmdCopyBuffer(cmd, st->arena.buffer, host.buffer, 1, &region);
                 }
             });
             vmaInvalidateAllocation(vk.allocator, host.alloc, 0, VK_WHOLE_SIZE);
             for (const auto& [name, at] : places)
             {
                 const size_t id = size_t(st->cur->plan.outputs.at(name));
                 const auto [w, h] = st->cur->sizes[id];
                 image::Image img{w, h, std::vector<float>(size_t(w) * size_t(h) * 4)};
                 const uint8_t* src = static_cast<const uint8_t*>(host.mapped) + at;
                 if (st->cur->half[id])
                     for (size_t i = 0; i < img.rgba.size(); ++i)
                     {
                         uint16_t h16;
                         std::memcpy(&h16, src + i * 2, 2);
                         img.rgba[i] = tex::paint::halfToFloat(h16);
                     }
                 else
                     std::memcpy(img.rgba.data(), src, img.rgba.size() * sizeof(float));
                 out[name] = std::move(img);
             }
         }
         catch (...)
         {
             compute::destroy(vk, host);
             throw;
         }
         compute::destroy(vk, host);
         return true;
     }).get();
    return out;
}

// -------------------------------------------------------------------------------------------------------- free functions

void registerNativeGpu(const std::string& type, NativeGpuFn fn)
{
    Shared& s = shared();
    std::scoped_lock lock(s.mutex);
    s.natives[type] = std::move(fn);
}

bool hasNativeGpu(const std::string& type)
{
    const Route r = nativeRoute(type);
    return r == Route::Registered || r == Route::Kernels || r == Route::Builtin;
}

void setMeshMapSource(MeshMapSource fn)
{
    Shared& s = shared();
    std::scoped_lock lock(s.mutex);
    s.meshMaps = std::move(fn);
}

GpuImageRef meshMap(const std::string& bakeProject, const std::string& map, int udim)
{
    MeshMapSource fn;
    {
        Shared& s = shared();
        std::scoped_lock lock(s.mutex);
        fn = s.meshMaps;
    }
    return fn && !bakeProject.empty() ? fn(bakeProject, map, udim) : nullptr;
}

void meshMapsChanged(const std::string& bakeProject)
{
    std::vector<std::function<void(const std::string&)>> fns;
    {
        Shared& s = shared();
        std::scoped_lock lock(s.mutex);
        for (auto& [id, fn] : s.listeners)
            fns.push_back(fn);
    }
    for (auto& fn : fns)
        fn(bakeProject);
}

int addMeshMapListener(std::function<void(const std::string& bakeProject)> fn)
{
    Shared& s = shared();
    std::scoped_lock lock(s.mutex);
    s.listeners[s.nextListener] = std::move(fn);
    return s.nextListener++;
}

void removeMeshMapListener(int id)
{
    Shared& s = shared();
    std::scoped_lock lock(s.mutex);
    s.listeners.erase(id);
}

namespace
{
/** The plan cut down to the passes the named outputs need. Outputs it drops that a kept pass reads become intermediates. */
Plan outputsOnly(const Plan& plan, std::span<const std::string> names)
{
    Plan p = plan;
    p.outputs.clear();
    std::vector<uint8_t> need(plan.buffers.size(), 0);
    for (const std::string& n : names)
    {
        const int id = plan.outputs.at(n);
        p.outputs[n] = id;
        need[size_t(id)] = 1;
    }
    std::vector<size_t> kept;
    for (size_t i = plan.passes.size(); i-- > 0;)
    {
        const Pass& pass = plan.passes[i];
        if (!need[size_t(pass.output)])
            continue;
        kept.push_back(i);
        for (int r : pass.reads)
            need[size_t(r)] = 1;
        for (const auto& [id, r] : pass.inputBuffers)
            need[size_t(r)] = 1;
    }
    p.passes.clear();
    for (auto i = kept.rbegin(); i != kept.rend(); ++i)
        p.passes.push_back(plan.passes[*i]);
    std::set<int> outs;
    for (const auto& [n, id] : p.outputs)
        outs.insert(id);
    for (BufferDesc& b : p.buffers)
        if (b.kind == BufferKind::Output && !outs.contains(b.id))
            b.kind = BufferKind::Intermediate;
    return p;
}

/** Runs the plan for the named outputs into result, halving the group while its buffers don't fit one GPU buffer. */
void evaluateGroup(const Plan& plan, const tex::EvalOptions& options, std::span<const std::string> names, uint64_t maxArenaBytes, tex::EvalResult& result)
{
    LILA_ZONE();
    const Plan part = names.size() == plan.outputs.size() ? plan : outputsOnly(plan, names);
    Inputs in;
    auto external = [&](int b) {
        const BufferDesc& d = part.buffers[size_t(b)];
        if (d.kind == BufferKind::External && !in.cpuExternals.contains(d.name))
            if (auto it = options.externals.find(d.name); it != options.externals.end())
                in.cpuExternals.emplace(d.name, it->second);
    };
    for (const Pass& p : part.passes)
    {
        for (int b : p.reads)
            external(b);
        for (const auto& [id, b] : p.inputBuffers)
            external(b);
    }
    in.compact = true;
    in.maxArenaBytes = maxArenaBytes;
    in.imageRoot = options.imageRoot;
    in.uvOffset[0] = options.uvOffset[0];
    in.uvOffset[1] = options.uvOffset[1];
    Evaluator ev;
    if (std::string err = ev.setPlan(part, std::move(in)); !err.empty())
        throw std::runtime_error(err);
    try
    {
        Renderer::get().runTask([&ev](VulkanContext& vk) { return ev.step(vk); }).get();
    }
    catch (const ArenaTooLarge&)
    {
        if (names.size() < 2)
            throw;
        const size_t half = names.size() / 2;
        evaluateGroup(plan, options, names.first(half), maxArenaBytes, result);
        evaluateGroup(plan, options, names.subspan(half), maxArenaBytes, result);
        return;
    }
    result.outputs.merge(ev.readOutputs());
    const Stats s = ev.stats();
    result.compiled += s.compiled;
    result.cached += s.cached;
    for (const std::string& w : s.warnings)
        if (std::find(result.warnings.begin(), result.warnings.end(), w) == result.warnings.end())
            result.warnings.push_back(w);
}
} // namespace

tex::EvalResult evaluate(const Plan& plan, const tex::EvalOptions& options, uint64_t maxArenaBytes)
{
    LILA_ZONE();
    if (!plan.error.empty())
        throw std::runtime_error(plan.error);
    Renderer& r = Renderer::get();
    if (!r.running() && !r.start())
        throw std::runtime_error("no GPU: the renderer could not start");
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> names;
    for (const auto& [name, id] : plan.outputs)
        names.push_back(name);
    tex::EvalResult result;
    evaluateGroup(plan, options, names, maxArenaBytes, result);
    result.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return result;
}

std::set<std::string> wholeExternals(const Plan& plan)
{
    std::vector<int> readers(plan.buffers.size(), 0);
    for (const Pass& p : plan.passes)
        for (int r : p.reads)
            ++readers[size_t(r)];
    std::set<std::string> out;
    for (const BufferDesc& b : plan.buffers)
    {
        if (b.kind != BufferKind::External)
            continue;
        for (const Pass& p : plan.passes)
        {
            if (std::find(p.reads.begin(), p.reads.end(), b.id) == p.reads.end())
                continue;
            const bool page = p.kind == PassKind::Fused && plan.buffers[size_t(p.output)].kind == BufferKind::Output && readers[size_t(p.output)] == 0;
            if (!page || std::find(p.local.begin(), p.local.end(), b.id) == p.local.end())
                out.insert(b.name);
        }
    }
    return out;
}

bool recordImageRegion(VulkanContext& vk, VkCommandBuffer cmd, const GpuImageRef& image, int levelWidth, int levelHeight, const std::array<int, 4>& rect,
                       VkDeviceAddress dst, uint32_t dstStride, std::vector<compute::Buffer>& temps)
{
    const int w = rect[2] - rect[0], h = rect[3] - rect[1];
    if (!image || w <= 0 || h <= 0)
        return false;
    std::vector<compute::Buffer> staging;
    const bool ok = image->prepare(vk, cmd, staging);
    temps.insert(temps.end(), staging.begin(), staging.end());
    if (!ok || !image->realized())
        return false;
    levelWidth = std::max(levelWidth, 1);
    levelHeight = std::max(levelHeight, 1);
    // The part of the level the rectangle covers, in image texels; texels outside the level clamp to it.
    const std::array<int, 4> inside{std::clamp(rect[0], 0, levelWidth - 1), std::clamp(rect[1], 0, levelHeight - 1),
                                    std::clamp(rect[2], 1, levelWidth), std::clamp(rect[3], 1, levelHeight)};
    const std::array<int, 4> src = Rect::texels(inside, levelWidth, levelHeight).cover(image->width(), image->height());
    const int sw = std::max(src[2] - src[0], 1), sh = std::max(src[3] - src[1], 1);
    compute::Buffer raw =
        compute::makeBuffer(vk, (VkDeviceSize(sw) * VkDeviceSize(sh) * VkDeviceSize(texelBytes(image->format())) + 3) & ~VkDeviceSize(3), 0, compute::Host::None);
    temps.push_back(raw);
    image->recordReadRect(cmd, raw, 0, src[0], src[1], sw, sh);
    compute::memoryBarrier(cmd);
    const float sx = float(image->width()) / float(levelWidth), sy = float(image->height()) / float(levelHeight);
    const uint64_t texels = uint64_t(w) * uint64_t(h);
    DecodePush push{raw.address,
                    dst,
                    uint32_t(w),
                    uint32_t(h),
                    formatCode(image->format()),
                    usesSrgb(image->format(), image->desc().srgb) ? 1u : 0u,
                    groupsX(texels),
                    dstStride,
                    uint32_t(sw),
                    uint32_t(sh),
                    {sx, sy},
                    {float(rect[0]) * sx - float(src[0]), float(rect[1]) * sy - float(src[1])},
                    0,
                    0};
    VkPipelineLayout layout;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel(vk, "tex_decode.spv", "texDecodeCS", sizeof(DecodePush), layout));
    vkCmdPushConstants(cmd, layout, kCompute, 0, sizeof(push), &push);
    dispatchLinear(cmd, texels);
    return true;
}

std::pair<size_t, size_t> spirvCacheStats()
{
    Shared& s = shared();
    std::scoped_lock lock(s.mutex);
    return {s.compiles, s.hits};
}

void registerTexGpu()
{
    tex::setGpuEvaluator({[](const Plan& p, const tex::EvalOptions& o) { return evaluate(p, o); },
                          [] {
                              Renderer& r = Renderer::get();
                              return r.running() || r.start();
                          }});
}
} // namespace lila::render::texgpu
