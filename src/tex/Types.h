#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/**
 * Texture graphs: procedural materials as node graphs evaluated per texel on the GPU (or on the CPU through Slang's host
 * target). Port values are functions of a uv coordinate, so warps and transforms stay exact at any resolution.
 */
namespace lila::tex
{
using json = nlohmann::json;

/** What flows along a link. gray and float are the same value (gray is meant to stay in 0 to 1); sdf2d is a signed distance. */
enum class PortType : uint8_t
{
    Float,
    Gray,
    Color,
    Vector,
    Sdf2d,
    Material,
};

/** A node's settings. All but Code live in the graph's uniform buffer, so editing one never recompiles. */
enum class ParamType : uint8_t
{
    Float,
    Int,
    Enum,
    Bool,
    Color,
    Gradient,
    Curve,
    Image,
    Seed,
    /** Slang source spliced into the node's body wherever `$id` appears. The one parameter whose edits recompile. */
    Code,
    /**
     * A scene node's name, for the panel to read scene data from (a camera's pose) into the node's inputs. Its options name
     * the node types it takes ("camera"). The graph itself never reads it.
     */
    Node,
    /** Text the planner reads (a Text node's words and font); never in the source or the uniforms. */
    String,
};

LILA_SCENE_API const char* portTypeName(PortType t);
LILA_SCENE_API bool parsePortType(std::string_view name, PortType* out);
LILA_SCENE_API const char* paramTypeName(ParamType t);
LILA_SCENE_API bool parseParamType(std::string_view name, ParamType* out);

/** Whether an output of type from can feed an input of type to (with a conversion where they differ). */
LILA_SCENE_API bool linkable(PortType from, PortType to);
/** The Slang type a port's values have: float, float4, float3 or TexMaterial. */
LILA_SCENE_API const char* slangType(PortType t);
/** The Slang sampler interface for a port type: ITexFloat, ITexColor, ITexVector, ITexMaterial. */
LILA_SCENE_API const char* samplerInterface(PortType t);

/** Gradient and curve parameters hold at most this many stops or points (their uniform space is fixed). */
inline constexpr int kMaxGradientStops = 16;
inline constexpr int kMaxCurvePoints = 16;

/** Uniform slots (float4 each) a parameter of the type takes. */
LILA_SCENE_API int paramSlots(ParamType t);

/** A port value as four floats: numbers broadcast to gray, arrays of 3 or 4, "#rrggbb[aa]" in sRGB. False on a bad value. */
LILA_SCENE_API bool valueToFloat4(const json& v, PortType t, float out[4]);
/** Whether v fits a parameter of the type (options for enums). */
LILA_SCENE_API bool paramValueValid(ParamType t, const json& v, const std::vector<std::string>& options);
} // namespace lila::tex
