#pragma once

#include "core/Export.h"

#include "tex/Types.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lila::bake
{
struct TargetMap;
struct TargetProfile;
} // namespace lila::bake

/**
 * Shading channels beyond base colour, roughness, metallic, normal, height, AO, emissive and opacity: sheen, coat,
 * subsurface, anisotropy, transmission, IOR and specular. A texture set gains them as ordinary channels (extra Material
 * Output inputs); the viewport and the path-traced view shade with them, and exports write each under the name the
 * preset's renderer knows it by (OpenPBR, glTF extension property, Unreal material input).
 */
namespace lila::tex::shading
{
struct ChannelInfo
{
    const char* name;  // the set channel
    PortType type;
    int bits;
    std::array<float, 4> fallback; // the value shading uses with no channel
    const char* label;
    /** OpenPBR input, glTF property (extension.texture[.component]), Unreal material input. */
    const char* openPbr;
    const char* gltf;
    const char* unreal;
    /** The {map} token when exported. */
    const char* suffix;
};

/** sheenColor, sheenRoughness, coat, coatRoughness, coatNormal, subsurfaceColor, subsurfaceRadius, anisotropy, anisotropyDirection, transmission, ior, specularWeight, specularColor. */
LILA_SCENE_API std::span<const ChannelInfo> channels();
LILA_SCENE_API const ChannelInfo* find(std::string_view name);

/** The ior channel holds (ior - 1) / 2, so 0.25 is glass's 1.5 and 8-bit channels hold every useful value. */
inline float iorFromChannel(float v) { return 1.0f + 2.0f * v; }
inline float channelFromIor(float ior) { return (ior - 1.0f) * 0.5f; }

/** Which naming a preset follows: "openpbr", "gltf", "unreal", or "" for Lila's own names. */
LILA_SCENE_API std::string familyOf(const bake::TargetProfile& target);
/** The name a family calls a channel by. */
LILA_SCENE_API std::string nameFor(const ChannelInfo& c, std::string_view family);

/**
 * Appends an optional map for each shading channel the preset doesn't list already, its {map} token and slot named the
 * way the preset's renderer names it. exportSet writes those the set has.
 */
LILA_SCENE_API void addExportMaps(std::vector<bake::TargetMap>& maps, const bake::TargetProfile& target);

/** texAddShadingChannels. Called by tex::registerCommands. */
LILA_SCENE_API void registerCommands();
} // namespace lila::tex::shading
