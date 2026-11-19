#include "tex/Evaluate.h"
#include "tex/PaintStrokes.h"

#include "core/Paths.h"
#include "core/Profile.h"
#include "core/Workspace.h"
#include "io/SceneFile.h"
#include "tex/Natives.h"
#include "tex/SlangRt.h"

#include <tbb/parallel_for.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace lila::tex
{
namespace fs = std::filesystem;

namespace
{
int wrap(int i, int n) { return ((i % n) + n) % n; }

/** Separable Gaussian, radius in uv (fraction of the width), wrapping at the edges. */
void blurCpu(const NativeCall& c)
{
    auto it = c.inputs.find("input");
    if (it == c.inputs.end())
        return;
    const NativeBuffer& in = it->second;
    const NativeBuffer& out = c.output;
    const int w = out.width, h = out.height;
    const float sigma = std::max(1e-4f, c.param("radius") * float(w) / 3.0f);
    const int r = std::min(std::max(w, h), int(std::ceil(sigma * 3.0f)));
    std::vector<float> kernel(size_t(2 * r + 1));
    float sum = 0.0f;
    for (int k = -r; k <= r; ++k)
        sum += kernel[size_t(k + r)] = std::exp(-0.5f * float(k * k) / (sigma * sigma));
    for (float& k : kernel)
        k /= sum;
    // Input may be a different size: resample nearest onto the output grid first.
    std::vector<float> src(size_t(w) * size_t(h) * 4), tmp(src.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const int sx = x * in.width / w, sy = y * in.height / h;
            std::memcpy(&src[(size_t(y) * w + x) * 4], &in.rgba[(size_t(sy) * in.width + sx) * 4], sizeof(float) * 4);
        }
    tbb::parallel_for(0, h, [&](int y) {
        for (int x = 0; x < w; ++x)
        {
            float acc[4] = {0, 0, 0, 0};
            for (int k = -r; k <= r; ++k)
            {
                const float* s = &src[(size_t(y) * w + wrap(x + k, w)) * 4];
                for (int ch = 0; ch < 4; ++ch)
                    acc[ch] += kernel[size_t(k + r)] * s[ch];
            }
            std::memcpy(&tmp[(size_t(y) * w + x) * 4], acc, sizeof(acc));
        }
    });
    tbb::parallel_for(0, h, [&](int y) {
        for (int x = 0; x < w; ++x)
        {
            float acc[4] = {0, 0, 0, 0};
            for (int k = -r; k <= r; ++k)
            {
                const float* s = &tmp[(size_t(wrap(y + k, h)) * w + x) * 4];
                for (int ch = 0; ch < 4; ++ch)
                    acc[ch] += kernel[size_t(k + r)] * s[ch];
            }
            std::memcpy(&out.rgba[(size_t(y) * w + x) * 4], acc, sizeof(acc));
        }
    });
}

std::map<std::string, NativeCpuFn>& natives()
{
    static std::map<std::string, NativeCpuFn> m = [] {
        std::map<std::string, NativeCpuFn> table = {{"blur", blurCpu}};
        addBuiltinNatives(table);
        return table;
    }();
    return m;
}

GpuEvaluator& gpuEvaluator()
{
    static GpuEvaluator e;
    return e;
}
} // namespace

image::Image fitSourceImage(image::Image img, const std::string& name, int planWidth, int planHeight, size_t maxTexels, std::string* warning)
{
    if (img.width <= 0 || img.height <= 0)
        return img;
    int w = std::min(img.width, std::max(planWidth, 1)), h = std::min(img.height, std::max(planHeight, 1));
    if (maxTexels > 0 && size_t(w) * size_t(h) > maxTexels)
    {
        const double k = std::sqrt(double(maxTexels) / (double(w) * double(h)));
        w = std::max(1, int(w * k));
        h = std::max(1, int(h * k));
        if (warning)
            *warning = "image " + name + ": " + std::to_string(img.width) + " x " + std::to_string(img.height) + " read at " + std::to_string(w) + " x " +
                       std::to_string(h) + ", the most one GPU evaluation holds. Split it into UDIM tiles (<UDIM> in the file name) for full detail";
    }
    if (w == img.width && h == img.height)
        return img;
    return image::downsampled(img, w, h);
}

fs::path resolveImagePath(const std::string& path, const fs::path& root)
{
    fs::path p(path);
    if (p.is_absolute())
        return p;
    if (!root.empty())
        return root / p;
    if (Workspace::get().isOpen() && fs::exists(Workspace::get().root() / p))
        return Workspace::get().root() / p;
    const fs::path scene = io::currentPath().empty() ? fs::path() : io::currentPath().parent_path() / p;
    if (!scene.empty() && fs::exists(scene))
        return scene;
    // Shipped images such as strokes/strokes_soft.png, so library materials can name them portably.
    if (fs::exists(paths::resourcesDir() / p))
        return paths::resourcesDir() / p;
    return scene.empty() ? p : scene;
}

void registerNativeCpu(const std::string& type, NativeCpuFn fn) { natives()[type] = std::move(fn); }

NativeCpuFn nativeCpu(const std::string& type)
{
    auto it = natives().find(type);
    return it == natives().end() ? NativeCpuFn() : it->second;
}

void setGpuEvaluator(GpuEvaluator e) { gpuEvaluator() = std::move(e); }

EvalResult evaluate(const Plan& plan, const EvalOptions& options, const std::string& backend, std::string* used)
{
    if (backend != "" && backend != "gpu" && backend != "cpu")
        throw std::runtime_error("backend must be gpu or cpu, not " + backend);
    const GpuEvaluator& gpu = gpuEvaluator();
    const bool haveGpu = gpu.evaluate && (!gpu.available || gpu.available());
    if (backend == "gpu" && !haveGpu)
        throw std::runtime_error("no GPU to evaluate on (the renderer can't start)");
    const bool onGpu = backend == "gpu" || (backend.empty() && haveGpu);
    if (used)
        *used = onGpu ? "gpu" : "cpu";
    return onGpu ? gpu.evaluate(plan, options) : evaluateCpu(plan, options);
}

EvalResult evaluateCpu(const Plan& plan, const EvalOptions& options)
{
    LILA_ZONE();
    if (!plan.error.empty())
        throw std::runtime_error(plan.error);
    const auto t0 = std::chrono::steady_clock::now();
    const slangrt::CacheStats before = slangrt::cacheStats();
    EvalResult result;

    // Place every buffer in one arena; images and externals bring their own sizes.
    std::vector<image::Image> sourced(plan.buffers.size());
    std::vector<size_t> offset(plan.buffers.size());
    std::vector<std::pair<int, int>> size(plan.buffers.size());
    size_t total = 0;
    for (const BufferDesc& b : plan.buffers)
    {
        int w = b.width, h = b.height;
        if (b.kind == BufferKind::Image)
        {
            std::string err;
            image::Image img = b.name.empty() ? image::Image{}
                                              : fitSourceImage(image::read(resolveImagePath(b.name, options.imageRoot), &err), b.name, plan.width,
                                                               plan.height, 0, nullptr);
            if (img.width <= 0)
            {
                if (!b.name.empty())
                    result.warnings.push_back("image " + b.name + ": " + (err.empty() ? "can't read" : err));
                img = {1, 1, {0.0f, 0.0f, 0.0f, 1.0f}};
            }
            sourced[size_t(b.id)] = std::move(img);
        }
        else if (b.kind == BufferKind::External)
        {
            auto it = options.externals.find(b.name);
            if (it != options.externals.end() && it->second.width > 0)
                sourced[size_t(b.id)] = it->second;
            else if (std::string channel; paint::parseExternalName(b.name, nullptr, &channel))
            {
                // A paint channel never painted covers nothing; a mask never painted masks nothing.
                const float v = channel == paint::kMaskChannel ? 1.0f : 0.0f;
                sourced[size_t(b.id)] = image::Image{1, 1, {v, v, v, v}};
            }
            else
            {
                image::Image img{plan.width, plan.height, std::vector<float>(size_t(plan.width) * plan.height * 4)};
                for (int y = 0; y < plan.height; ++y)
                    for (int x = 0; x < plan.width; ++x)
                    {
                        float* t = &img.rgba[(size_t(y) * plan.width + x) * 4];
                        t[0] = options.uvOffset[0] + (float(x) + 0.5f) / float(plan.width);
                        t[1] = options.uvOffset[1] + 1.0f - (float(y) + 0.5f) / float(plan.height);
                        t[2] = 0.0f;
                        t[3] = 1.0f;
                    }
                sourced[size_t(b.id)] = std::move(img);
            }
        }
        if (b.kind == BufferKind::Image || b.kind == BufferKind::External)
            w = sourced[size_t(b.id)].width, h = sourced[size_t(b.id)].height;
        offset[size_t(b.id)] = total;
        size[size_t(b.id)] = {w, h};
        total += size_t(w) * size_t(h);
    }
    if (total >= (size_t(1) << 32))
        throw std::runtime_error("texture graph buffers exceed 4G texels");
    std::vector<float> arena(std::max<size_t>(total, 1) * 4, 0.0f);
    for (const BufferDesc& b : plan.buffers)
        if (!sourced[size_t(b.id)].rgba.empty())
            std::memcpy(&arena[offset[size_t(b.id)] * 4], sourced[size_t(b.id)].rgba.data(), sourced[size_t(b.id)].rgba.size() * sizeof(float));

    std::vector<float> uniforms = plan.uniforms;
    if (uniforms.empty())
        uniforms.assign(4, 0.0f);
    for (const BufferDesc& b : plan.buffers)
    {
        const uint32_t d[4] = {uint32_t(offset[size_t(b.id)]), uint32_t(size[size_t(b.id)].first), uint32_t(size[size_t(b.id)].second), 0u};
        std::memcpy(&uniforms[size_t(b.slot) * 4], d, sizeof(d));
    }

    for (const Pass& pass : plan.passes)
    {
        const BufferDesc& outDesc = plan.buffers.at(size_t(pass.output));
        const int w = size[size_t(outDesc.id)].first, h = size[size_t(outDesc.id)].second;
        float* out = &arena[offset[size_t(outDesc.id)] * 4];
        if (pass.kind == PassKind::Native)
        {
            auto it = natives().find(pass.nativeType);
            if (it == natives().end())
                throw std::runtime_error("no CPU implementation of native node " + pass.nativeType);
            NativeCall call{plan, pass, uniforms, {}, {out, w, h}};
            for (const auto& [id, b] : pass.inputBuffers)
                call.inputs[id] = {&arena[offset[size_t(b)] * 4], size[size_t(b)].first, size[size_t(b)].second};
            it->second(call);
            continue;
        }
        std::string err;
        auto kernel = slangrt::cpuKernel(pass.source, pass.hash, &err);
        if (!kernel)
            throw std::runtime_error("pass for " + pass.node + " failed to compile:\n" + err);
        slangrt::PassHeader header{{float(w), float(h), 1.0f / float(w), 1.0f / float(h)}, {options.uvOffset[0], options.uvOffset[1], 1.0f, 1.0f}};
        const slangrt::PassHeader* headerPtr = &header;
        slangrt::BufferView u{uniforms.data(), uniforms.size() / 4};
        slangrt::BufferView a{arena.data(), arena.size() / 4};
        slangrt::BufferView o{out, size_t(w) * size_t(h)};
        std::vector<uint8_t> globals(kernel->globalsSize, 0);
        std::memcpy(&globals[kernel->offPass], &headerPtr, sizeof(headerPtr));
        std::memcpy(&globals[kernel->offU], &u, sizeof(u));
        if (kernel->offArena != SIZE_MAX)
            std::memcpy(&globals[kernel->offArena], &a, sizeof(a));
        std::memcpy(&globals[kernel->offOut], &o, sizeof(o));
        const uint32_t groupsX = uint32_t((w + 7) / 8), groupsY = uint32_t((h + 7) / 8);
        tbb::parallel_for(uint32_t(0), groupsY, [&](uint32_t gy) {
            slangrt::Varying v{{0, gy, 0}, {groupsX, gy + 1, 1}};
            std::vector<uint8_t> local = globals;
            kernel->fn(&v, nullptr, local.data());
        });
    }

    for (const auto& [name, id] : plan.outputs)
    {
        const auto [w, h] = size[size_t(id)];
        image::Image img{w, h, std::vector<float>(size_t(w) * size_t(h) * 4)};
        std::memcpy(img.rgba.data(), &arena[offset[size_t(id)] * 4], img.rgba.size() * sizeof(float));
        result.outputs[name] = std::move(img);
    }
    const slangrt::CacheStats after = slangrt::cacheStats();
    result.compiled = after.compiles - before.compiles;
    result.cached = after.hits - before.hits;
    result.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return result;
}
} // namespace lila::tex
