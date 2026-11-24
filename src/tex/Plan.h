#pragma once

#include "core/Export.h"

#include "tex/Graph.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

/**
 * Compiling a texture graph into an evaluation plan: the passes to run, in order, each a Slang compute shader (or a
 * native multi-pass node), the buffers they read and write, and the uniform values.
 *
 * Contract every evaluator (the CPU one in Evaluate.h, the GPU one in the renderer) implements for a fused pass:
 *
 *   binding 0  ConstantBuffer<TexPassHeader> gPass   { float4 res;  // width, height, 1/width, 1/height
 *                                                     float4 uv; }  // uv offset xy, uv scale zw (whole texture: 0,0,1,1)
 *   binding 1  StructuredBuffer<float4> gU           Plan::uniforms, with every buffer descriptor slot filled in
 *   binding 2  StructuredBuffer<float4> gArena       every buffer the pass reads, packed anywhere in one array
 *   binding 3  RWStructuredBuffer<float4> gOut       the pass's output buffer, row-major, row 0 at the top (v = 1)
 *   entry "texMain", [numthreads(8, 8, 1)], dispatched over ceil(width / 8) x ceil(height / 8) groups.
 *
 * A buffer descriptor slot (BufferDesc::slot) holds asuint(arena offset in texels), asuint(width), asuint(height), 0,
 * written by the evaluator once it has placed the buffers. Texel (x, y) of a pass covers
 * uv = gPass.uv.xy + ((x + 0.5) / width, 1 - (y + 0.5) / height) * gPass.uv.zw; sampling a buffer is bilinear and wraps.
 *
 * Values never reach the source: parameters and unlinked inputs are read from gU, so a value edit keeps every pass's
 * source and Plan::structureHash, and only the uniforms change. A link, node or type change gives a new hash, and so does
 * a code parameter's edit (the Pixel Processor's expression), which is spliced into the source.
 */
namespace lila::tex
{
enum class BufferKind : uint8_t
{
    /** Written by a pass and read by later ones. */
    Intermediate,
    /** A requested output (a channel or a previewed socket). May also be read by later passes. */
    Output,
    /** An image parameter's file, loaded by the evaluator at its own size. name is the path. */
    Image,
    /**
     * Supplied by the caller by name (mesh maps: "position", "normal", ...; image parameters set to "external:<name>",
     * such as paint buffers). Missing ones read as (u, v, 0, 1).
     */
    External,
};

struct BufferDesc
{
    int id = 0;
    BufferKind kind = BufferKind::Intermediate;
    /** Output: the output name. Image: the path. External: the map name. Intermediate: the node.socket it caches. */
    std::string name;
    PortType type = PortType::Color;
    /** 0 for Image and External (their own size). */
    int width = 0;
    int height = 0;
    /** Storage the GPU should use: rgba16f or rgba32f. The CPU evaluator always uses float. */
    std::string format = "rgba16f";
    /** The gU slot of its descriptor. */
    int slot = -1;
    /**
     * Image: a pass reads it away from its own uv (tiled, scaled, warped), so the evaluator packs mip levels after it
     * (withMipLevels in Evaluate.h) and sampleImage reads the level the footprint covers.
     */
    bool mips = false;
};

enum class PassKind : uint8_t
{
    Fused,
    Native,
};

struct Pass
{
    PassKind kind = PassKind::Fused;
    /** Fused: the complete Slang source. */
    std::string source;
    std::string entry = "texMain";
    /** Hash of source (fused) or of the native type and its slots: the pipeline cache key. */
    uint64_t hash = 0;
    /** Buffer id written. */
    int output = -1;
    /** Buffer ids read. */
    std::vector<int> reads;
    /**
     * Those of reads sampled only at the pass's own uv (no warp, tiling or neighbour taps on the way), so a changed
     * region of one changes only the same region of the output. Fused passes only.
     */
    std::vector<int> local;
    /** Of the reads not in local, those read only within this many texels of the pass's own uv (neighbour taps). Fused passes only. */
    std::map<int, int> reach;
    /** Native: the node type, its parameters' first gU slots by id, and its input buffers by input id. */
    std::string nativeType;
    std::map<std::string, int> paramSlots;
    std::map<std::string, int> inputBuffers;
    /** The node (path through groups) whose output it computes. Informational. */
    std::string node;
    /** The scheduler's cost estimate per texel. */
    double cost = 0.0;
    /** The gU value slots the pass reads (parameters and unlinked inputs, not buffer descriptors), ascending: a value edit dirties it when one changes. */
    std::vector<int> slots;
};

/** One uniform-backed value: a parameter or an unlinked input of a node. */
struct UniformEntry
{
    /** Node path through groups (Group1/Noise). */
    std::string node;
    std::string id;
    int slot = 0;
    int slots = 1;
};

struct OutputRequest
{
    /** The name the result goes by (a channel name). */
    std::string name;
    std::string node;
    std::string socket;
    /** What the output is stored as. */
    PortType type = PortType::Color;
    /** When the socket is a material: the channel field to take (baseColor, roughness...). */
    std::string field;
};

struct PlanRequest
{
    int width = 1024;
    int height = 1024;
    std::vector<OutputRequest> outputs;
    std::string bufferFormat = "rgba16f";
    /** Scheduler budgets, see Plan.cpp. */
    double cheapCost = 8.0;
    double tapBudget = 256.0;
    double passBudget = 4096.0;
};

struct LILA_SCENE_API Plan
{
    int width = 0;
    int height = 0;
    std::vector<Pass> passes;
    std::vector<BufferDesc> buffers;
    std::vector<UniformEntry> params;
    /** Slot count * 4 floats. Buffer descriptor slots are zero until an evaluator fills them. */
    std::vector<float> uniforms;
    int uniformSlots = 0;
    /** Request output name -> buffer id. */
    std::map<std::string, int> outputs;
    /** Over everything but values: the same for any two graphs that differ only in values. */
    uint64_t structureHash = 0;
    /** Non-empty when the graph can't be compiled; passes is then empty. */
    std::string error;
    std::vector<std::string> warnings;

    const BufferDesc* buffer(int id) const { return id >= 0 && size_t(id) < buffers.size() ? &buffers[size_t(id)] : nullptr; }
    /** Summary for commands and tests: passes (kind, node, cost, reads, output), buffers, slots, hash. */
    json summary() const;
};

/**
 * A setting every computing node (Slang, native, Buffer) takes besides its parameters, kept in its values by id. A node with
 * one off its default caches its outputs in buffers of its own: outputSize scales them against the plan's size (eighth to
 * quadruple), bitDepth stores them at 8 (quantized to 1/255 by fused passes), 16 or 32 bits instead of the plan's format.
 */
struct NodeSetting
{
    std::string id;
    std::string label;
    std::vector<std::string> options;
    std::string defaultValue;
    std::string doc;
};
LILA_SCENE_API const std::vector<NodeSetting>& nodeSettings();
/** Null when id names no node setting. */
LILA_SCENE_API const NodeSetting* nodeSetting(std::string_view id);

/** The standard material channels a Material Output has, in order, with their port types. */
LILA_SCENE_API const std::vector<std::pair<std::string, PortType>>& standardChannels();

/** Requests for a Material Output node's channels (its standard ones plus any extra names given with their types). */
LILA_SCENE_API std::vector<OutputRequest> materialRequests(const Graph& g, const std::vector<std::pair<std::string, PortType>>& channels);

/** Never throws: problems land in Plan::error. */
LILA_SCENE_API Plan buildPlan(const Graph& g, const PlanRequest& request);

/** 64-bit FNV-1a, the hash every plan uses. */
LILA_SCENE_API uint64_t hash64(std::string_view data, uint64_t seed = 1469598103934665603ull);
} // namespace lila::tex
