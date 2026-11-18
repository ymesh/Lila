#include "render/Ocio.h"

#include "core/Log.h"
#include "core/Profile.h"

#include <OpenColorIO/OpenColorIO.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>

namespace OCIO = OCIO_NAMESPACE;

namespace lila::render::ocio
{
namespace
{
constexpr int kLutSize = 65;

struct CachedConfig
{
    OCIO::ConstConfigRcPtr config;
    std::filesystem::file_time_type stamp{};
    std::string error;
};

std::mutex gMutex;
std::map<std::string, CachedConfig> gConfigs;
std::map<std::string, std::shared_ptr<const ViewTransform>> gViews;

std::filesystem::file_time_type stampOf(const std::string& path)
{
    std::error_code ec;
    if (path.rfind("ocio://", 0) == 0)
        return {};
    const auto t = std::filesystem::last_write_time(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
    return ec ? std::filesystem::file_time_type{} : t;
}

/** The config at path, loaded once until its file changes. Callers hold gMutex. */
OCIO::ConstConfigRcPtr load(const std::string& path, std::string* err)
{
    const std::string key = path.empty() ? kBuiltinConfig : path;
    const auto stamp = stampOf(key);
    auto it = gConfigs.find(key);
    if (it != gConfigs.end() && it->second.stamp == stamp)
    {
        if (!it->second.config && err)
            *err = it->second.error;
        return it->second.config;
    }
    CachedConfig c;
    c.stamp = stamp;
    try
    {
        c.config = OCIO::Config::CreateFromFile(key.c_str());
    }
    catch (const std::exception& e)
    {
        c.error = e.what();
    }
    if (!c.config && c.error.empty())
        c.error = "cannot load " + key;
    if (!c.config)
        log::error("colour management: OCIO config {}: {}", key, c.error);
    if (err && !c.config)
        *err = c.error;
    gConfigs[key] = c;
    return c.config;
}

bool sameName(const char* a, const std::string& b)
{
    if (!a)
        return false;
    std::string x = a, y = b;
    std::ranges::transform(x, x.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    std::ranges::transform(y, y.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    return x == y;
}

/**
 * The config's colour space for Lila's working space, scene-linear Rec.709: by interop id, then by the names configs
 * commonly give it, then the scene_linear role.
 */
std::string workingSpace(const OCIO::ConstConfigRcPtr& cfg)
{
    static const char* kNames[] = {"lin_rec709_srgb", "Linear Rec.709 (sRGB)", "lin_rec709", "Utility - Linear - sRGB", "Linear sRGB", "linear"};
    const int n = cfg->getNumColorSpaces(OCIO::SEARCH_REFERENCE_SPACE_ALL, OCIO::COLORSPACE_ALL);
    for (int i = 0; i < n; ++i)
    {
        const char* name = cfg->getColorSpaceNameByIndex(OCIO::SEARCH_REFERENCE_SPACE_ALL, OCIO::COLORSPACE_ALL, i);
        OCIO::ConstColorSpaceRcPtr cs = cfg->getColorSpace(name);
        if (cs && cs->getInteropID() && sameName(cs->getInteropID(), "lin_rec709_srgb"))
            return name;
    }
    for (const char* want : kNames)
        if (OCIO::ConstColorSpaceRcPtr cs = cfg->getColorSpace(want))
            return cs->getName();
    return OCIO::ROLE_SCENE_LINEAR;
}

float shaperToLinear(float s, float lo, float hi)
{
    return std::exp2(lo + s * (hi - lo));
}

float linearToShaper(float x, float lo, float hi)
{
    return std::clamp((std::log2(std::max(x, std::exp2(lo))) - lo) / (hi - lo), 0.0f, 1.0f);
}
} // namespace

ConfigInfo info(const std::string& config, const std::string& display)
{
    LILA_ZONE();
    std::lock_guard lock(gMutex);
    ConfigInfo out;
    OCIO::ConstConfigRcPtr cfg = load(config, &out.error);
    if (!cfg)
        return out;
    try
    {
        out.ok = true;
        out.name = cfg->getName() && *cfg->getName() ? cfg->getName() : (config.empty() ? kBuiltinConfig : config);
        out.workingSpace = workingSpace(cfg);
        out.defaultDisplay = cfg->getDefaultDisplay();
        for (int i = 0; i < cfg->getNumDisplays(); ++i)
            out.displays.emplace_back(cfg->getDisplay(i));
        const std::string d = display.empty() || std::ranges::find(out.displays, display) == out.displays.end() ? out.defaultDisplay : display;
        out.defaultView = cfg->getDefaultView(d.c_str());
        for (int i = 0; i < cfg->getNumViews(d.c_str()); ++i)
            out.views.emplace_back(cfg->getView(d.c_str(), i));
        for (int i = 0; i < cfg->getNumLooks(); ++i)
            out.looks.emplace_back(cfg->getLookNameByIndex(i));
        const int n = cfg->getNumColorSpaces(OCIO::SEARCH_REFERENCE_SPACE_ALL, OCIO::COLORSPACE_ACTIVE);
        for (int i = 0; i < n; ++i)
            out.colorSpaces.emplace_back(cfg->getColorSpaceNameByIndex(OCIO::SEARCH_REFERENCE_SPACE_ALL, OCIO::COLORSPACE_ACTIVE, i));
    }
    catch (const std::exception& e)
    {
        out.ok = false;
        out.error = e.what();
    }
    return out;
}

std::shared_ptr<const ViewTransform> viewTransform(const std::string& config, const std::string& display, const std::string& view,
                                                   const std::string& look, std::string* err)
{
    LILA_ZONE();
    std::lock_guard lock(gMutex);
    std::string loadErr;
    OCIO::ConstConfigRcPtr cfg = load(config, &loadErr);
    if (!cfg)
    {
        if (err)
            *err = loadErr;
        return nullptr;
    }
    const std::string key = (config.empty() ? kBuiltinConfig : config) + "|" + std::to_string(stampOf(config).time_since_epoch().count()) + "|" +
                            display + "|" + view + "|" + look;
    if (auto it = gViews.find(key); it != gViews.end())
        return it->second;
    try
    {
        const std::string d = display.empty() ? cfg->getDefaultDisplay() : display;
        const std::string v = view.empty() ? cfg->getDefaultView(d.c_str()) : view;
        OCIO::DisplayViewTransformRcPtr dv = OCIO::DisplayViewTransform::Create();
        dv->setSrc(workingSpace(cfg).c_str());
        dv->setDisplay(d.c_str());
        dv->setView(v.c_str());
        OCIO::LegacyViewingPipelineRcPtr pipeline = OCIO::LegacyViewingPipeline::Create();
        pipeline->setDisplayViewTransform(dv);
        if (!look.empty())
        {
            pipeline->setLooksOverride(look.c_str());
            pipeline->setLooksOverrideEnabled(true);
        }
        OCIO::ConstProcessorRcPtr proc = pipeline->getProcessor(cfg, cfg->getCurrentContext());
        OCIO::ConstCPUProcessorRcPtr cpu = proc->getDefaultCPUProcessor();
        auto t = std::make_shared<ViewTransform>();
        auto lut = std::make_shared<Lut3D>();
        lut->size = kLutSize;
        lut->rgb.resize(size_t(kLutSize) * kLutSize * kLutSize * 3);
        // .cube order: red fastest, then green, then blue.
        for (int b = 0, i = 0; b < kLutSize; ++b)
            for (int g = 0; g < kLutSize; ++g)
                for (int r = 0; r < kLutSize; ++r, ++i)
                {
                    const float step = 1.0f / float(kLutSize - 1);
                    lut->rgb[size_t(i) * 3 + 0] = shaperToLinear(float(r) * step, t->shaperMin, t->shaperMax);
                    lut->rgb[size_t(i) * 3 + 1] = shaperToLinear(float(g) * step, t->shaperMin, t->shaperMax);
                    lut->rgb[size_t(i) * 3 + 2] = shaperToLinear(float(b) * step, t->shaperMin, t->shaperMax);
                }
        OCIO::PackedImageDesc img(lut->rgb.data(), long(kLutSize) * kLutSize, long(kLutSize), 3);
        cpu->apply(img);
        t->lut = std::move(lut);
        t->label = d + " / " + v + (look.empty() ? "" : " / " + look);
        gViews[key] = t;
        return t;
    }
    catch (const std::exception& e)
    {
        log::error("colour management: {} / {}: {}", display, view, e.what());
        if (err)
            *err = e.what();
        return nullptr;
    }
}

bool toWorking(const std::string& config, const std::string& colorSpace, std::span<float> rgba, std::string* err)
{
    LILA_ZONE();
    OCIO::ConstCPUProcessorRcPtr cpu;
    {
        std::lock_guard lock(gMutex);
        OCIO::ConstConfigRcPtr cfg = load(config, err);
        if (!cfg)
            return false;
        try
        {
            cpu = cfg->getProcessor(colorSpace.c_str(), workingSpace(cfg).c_str())->getDefaultCPUProcessor();
        }
        catch (const std::exception& e)
        {
            if (err)
                *err = e.what();
            return false;
        }
    }
    OCIO::PackedImageDesc img(rgba.data(), long(rgba.size() / 4), 1, 4);
    cpu->apply(img);
    return true;
}

void applyView(const ViewTransform& t, std::span<float> rgba)
{
    if (!t.lut || t.lut->size < 2)
        return;
    const int n = t.lut->size;
    const std::vector<float>& lut = t.lut->rgb;
    auto at = [&](int r, int g, int b, int c) { return lut[(size_t(b) * n * n + size_t(g) * n + size_t(r)) * 3 + c]; };
    for (size_t p = 0; p + 3 < rgba.size(); p += 4)
    {
        float f[3];
        int i0[3], i1[3];
        for (int c = 0; c < 3; ++c)
        {
            const float s = linearToShaper(rgba[p + c], t.shaperMin, t.shaperMax) * float(n - 1);
            i0[c] = std::min(int(s), n - 2);
            i1[c] = i0[c] + 1;
            f[c] = s - float(i0[c]);
        }
        for (int c = 0; c < 3; ++c)
        {
            auto lerp = [](float a, float b, float x) { return a + (b - a) * x; };
            const float c00 = lerp(at(i0[0], i0[1], i0[2], c), at(i1[0], i0[1], i0[2], c), f[0]);
            const float c10 = lerp(at(i0[0], i1[1], i0[2], c), at(i1[0], i1[1], i0[2], c), f[0]);
            const float c01 = lerp(at(i0[0], i0[1], i1[2], c), at(i1[0], i0[1], i1[2], c), f[0]);
            const float c11 = lerp(at(i0[0], i1[1], i1[2], c), at(i1[0], i1[1], i1[2], c), f[0]);
            rgba[p + c] = lerp(lerp(c00, c10, f[1]), lerp(c01, c11, f[1]), f[2]);
        }
    }
}
} // namespace lila::render::ocio
