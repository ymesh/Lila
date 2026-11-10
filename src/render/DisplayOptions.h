#pragma once

#include "render/FrameState.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lila::render
{
/**
 * Sample count for the viewport targets: 1 when antialiasing is off, otherwise the request clamped to 1..8 and
 * rounded down to a count the device supports. supported is a VkSampleCountFlags mask (the bit for n is n itself).
 */
constexpr int msaaSamples(bool enabled, int requested, uint32_t supported)
{
    if (!enabled)
        return 1;
    int n = requested >= 8 ? 8 : requested >= 4 ? 4 : requested >= 2 ? 2 : 1;
    while (n > 1 && !(supported & uint32_t(n)))
        n /= 2;
    return n;
}

/** "environment", "studio", "flat" or "sculpt"; anything else is the environment. */
constexpr FrameState::Lighting lightingFromName(std::string_view name)
{
    return name == "studio"   ? FrameState::Lighting::Studio
           : name == "flat"   ? FrameState::Lighting::Flat
           : name == "sculpt" ? FrameState::Lighting::Sculpt
                              : FrameState::Lighting::Environment;
}

/** Maya's viewport exposure is in stops. */
inline float exposureScale(double stops) { return float(std::exp2(stops)); }

/** An HDRI's environment intensity: its own entry in byHdri (an object of HDRI key to number) if any, else fallback. */
inline float environmentIntensity(const nlohmann::json& byHdri, const std::string& hdri, float fallback)
{
    float v = fallback;
    if (byHdri.is_object())
        if (auto it = byHdri.find(hdri); it != byHdri.end() && it->is_number())
            v = it->get<float>();
    return std::max(v, 0.0f);
}

/** byHdri with hdri's entry set to value, or removed when there is none (so it follows the fallback again). */
inline nlohmann::json withEnvironmentIntensity(nlohmann::json byHdri, const std::string& hdri, std::optional<float> value)
{
    if (!byHdri.is_object())
        byHdri = nlohmann::json::object();
    if (value)
        byHdri[hdri] = std::max(*value, 0.0f);
    else
        byHdri.erase(hdri);
    return byHdri;
}
} // namespace lila::render
