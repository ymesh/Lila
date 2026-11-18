#pragma once

#include "core/Export.h"
#include "render/FrameState.h"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace lila::render::ocio
{
/** The built-in config, OpenColorIO's latest studio config. */
inline constexpr const char* kBuiltinConfig = "ocio://studio-config-latest";

/** What a config offers, for menus; ok false with error when it can't be loaded. */
struct ConfigInfo
{
    bool ok = false;
    std::string error;
    std::string name;        // the config's own name, else its path
    std::string workingSpace; // the colour space Lila's scene-linear Rec.709 maps to
    std::vector<std::string> displays, views, looks, colorSpaces; // views of the display asked for (else the default one)
    std::string defaultDisplay, defaultView;
};

/** The config at path (kBuiltinConfig, an ocio:// URI or a file), cached until the file changes. */
LILA_UI_API ConfigInfo info(const std::string& config, const std::string& display = {});

/**
 * Lila's scene-linear Rec.709 through a config's display and view (and a look), baked into a 65^3 LUT over a log2
 * shaper from 2^shaperMin to 2^shaperMax so HDR survives; its output is display code values. Cached by request; null
 * with err when the config, display or view doesn't load. Empty display or view take the config's defaults.
 */
LILA_UI_API std::shared_ptr<const ViewTransform> viewTransform(const std::string& config, const std::string& display, const std::string& view,
                                                              const std::string& look, std::string* err = nullptr);

/** Pixels (RGBA floats, alpha untouched) from a config colour space into Lila's scene-linear Rec.709. False with err on failure. */
LILA_UI_API bool toWorking(const std::string& config, const std::string& colorSpace, std::span<float> rgba, std::string* err = nullptr);

/** Scene-linear pixels through a baked view transform on the CPU, as the compose pass applies it: display code values out. */
LILA_UI_API void applyView(const ViewTransform& t, std::span<float> rgba);
} // namespace lila::render::ocio
