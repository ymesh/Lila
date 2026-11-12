#pragma once

#include "core/Export.h"

#include "bake/BakeTypes.h"
#include "bake/Maps.h"
#include "bake/TexelRaster.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace lila::bake
{
struct BakeInputs;

/**
 * One output texture while it is being baked: per-texel accumulators that every group writing into it adds to, then the
 * resolved values. Rows written by different bands never overlap, so bands accumulate without locks.
 */
struct LILA_SCENE_API TextureBuffer
{
    std::filesystem::path file;
    const MapDef* def = nullptr;
    MapSettings settings;
    int width = 0;
    int height = 0;
    int udim = 1001;
    int bits = 8;
    std::string format = "png";
    int samplesPerTexel = 1;

    // Accumulators.
    std::vector<glm::vec4> sum;
    std::vector<float> weight;
    /** Samples that landed in each texel, valid or not. */
    std::vector<uint16_t> samples;
    /** Resolve::Exact: the winning packed RGBA8 colour and its vote count. */
    std::vector<uint32_t> exact;
    std::vector<uint16_t> votes;

    // Resolved.
    std::vector<glm::vec4> value;
    std::vector<uint8_t> covered;

    /** What the texture holds, for the bake result. */
    std::set<std::string> groups;
    std::set<std::string> meshes;
    std::set<std::string> materials;
    json meta = json::object();

    void allocate();
    size_t texelCount() const { return size_t(width) * size_t(height); }
    /** Fills value and covered from the accumulators (by def->resolve). */
    void resolve();
};

/** Packs 0-1 RGBA into 8-bit channels and back, the exact values Resolve::Exact maps write. */
LILA_SCENE_API uint32_t packRgba8(const glm::vec4& c);
LILA_SCENE_API glm::vec4 unpackRgba8(uint32_t c);

/** A map of a device-resident bake, as planned. */
struct ResidentMap
{
    const MapDef* def = nullptr;
    MapSettings settings;
};

/** One UDIM tile of one low for ResidentBake::bakeTile. */
struct ResidentTile
{
    size_t group = 0;
    size_t low = 0;
    int udim = 1001;
    const TexelRaster* raster = nullptr;
    CageParams cage;
    /** targets[map][low material slot]: the texture each resident map writes there, null for none. */
    std::vector<std::vector<TextureBuffer*>> targets;
    /** Called after each band with its texel rows, on the calling thread; may throw progress::Cancelled. */
    std::function<void(int rows)> onBand;
    /** Polled between GPU slices, from any thread: true stops the tile with progress::Cancelled. */
    std::function<bool()> cancelled;
    /**
     * Incremental rebakes: one flag per texel tile of maskTile x maskTile texels, row major from the top left, and only
     * flagged tiles are rasterised and traced. Null bakes the whole tile.
     */
    const std::vector<uint8_t>* mask = nullptr;
    int maskTile = 32;
};

struct ResidentStats
{
    uint64_t samples = 0;
    uint64_t hits = 0;
    uint64_t backfaces = 0;
    uint64_t reaimed = 0;
    uint64_t probed = 0;
    /** Texel tiles (ResidentTile::maskTile square) that had triangles to rasterise. */
    uint64_t tiles = 0;
};

/**
 * A bake kept on the device from raster to resolved texture: samples, hits and per-texel accumulators never leave it and
 * only finished textures are read back. Kernels match the CPU ones (MapDef::cpu), which stay the reference.
 */
class LILA_SCENE_API ResidentBake
{
public:
    virtual ~ResidentBake() = default;
    /** Texel rows per band for a width and samples per texel, from the device's memory budget. */
    virtual int bandRows(int width, int samplesPerTexel) const = 0;
    /** Whether textures of this many texels in all fit on the device at once (they all stay there until finished). */
    virtual bool fits(size_t /*texels*/) const { return true; }
    /** Rasterises, traces and runs every resident map of one tile into its textures' device accumulators. */
    virtual ResidentStats bakeTile(const ResidentTile& tile) = 0;
    /**
     * Resolves a texture this bake wrote into value and covered and frees its device memory. When the map's finalize has
     * a device version it also finalizes (meta included) and dilates there and returns true; false leaves both to the
     * caller (MapDef::finalize, then Backend::dilate).
     */
    virtual bool finish(TextureBuffer& tex, int padding) = 0;
    /** Seconds per stage for BakeResult::timings (GPU time by step: raster, trace, map:<id>, accumulate, resolve, ...). */
    virtual json timings() const { return json::object(); }
};

/**
 * Where bakes run. The CPU backend is always registered and is the reference; a GPU backend registers itself under its
 * own name when the renderer starts and is picked by the bake command's backend flag.
 *
 * Contract, per bake: prepare once with the immutable inputs, then any number of traceProjection and shade calls from
 * worker threads (each on its own spans), then dilate per texture. Kernels a backend lacks fall back to MapDef::cpu.
 */
class LILA_SCENE_API Backend
{
public:
    virtual ~Backend() = default;

    virtual std::string name() const = 0;
    /** How it traces, for display: cpu, rt (ray query) or compute; a GPU backend that fell back reports cpu. */
    virtual std::string mode() const { return name(); }
    /** Builds acceleration structures: one per high, kept per group so groups never see each other. */
    virtual void prepare(const BakeInputs& inputs) = 0;
    /** One hit per sample, the nearest along each sample's ray among the group's highs only. Thread safe. */
    virtual void traceProjection(size_t group, std::span<const TexelSample> samples, std::span<HitRecord> hits) const = 0;
    /** Runs a map kernel. The default calls def.cpu with this backend's tracer. Thread safe. */
    virtual void shade(const MapDef& def, const MapContext& ctx, std::span<const TexelSample> samples, std::span<const HitRecord> hits,
                       std::span<glm::vec4> out, std::span<uint8_t> valid) const;
    /** Fills uncovered texels within padding (-1 everything) from their nearest covered texel, values copied exactly. */
    virtual void dilate(TextureBuffer& tex, int padding) const;
    /** Secondary rays for kernels, or null. */
    virtual const Tracer* tracer() const { return nullptr; }
    /** Texel rows per band for a texture width and samples per texel; GPU backends size bands by their memory budget. */
    virtual int bandRows(int /*width*/, int /*samplesPerTexel*/) const { return 32; }
    /** Every kernel of the band last traced on this thread has run; a GPU backend frees what it kept for it. */
    virtual void endBand(std::span<const TexelSample> /*samples*/) const {}
    /** True when beginResident can bake this map with these options on the device. */
    virtual bool residentSupports(const MapDef& /*def*/, const MapSettings& /*settings*/) const { return false; }
    /** A device-resident bake of maps (each residentSupports), after prepare; null when the backend has none. */
    virtual std::unique_ptr<ResidentBake> beginResident(const BakeInputs& /*inputs*/, const std::vector<ResidentMap>& /*maps*/,
                                                        const OutputSettings& /*output*/)
    {
        return nullptr;
    }
};

using BackendFactory = std::function<std::unique_ptr<Backend>()>;

/** Registers (or replaces) a backend by name. "cpu" is built in. */
LILA_SCENE_API void registerBackend(const std::string& name, BackendFactory factory);
LILA_SCENE_API void unregisterBackend(const std::string& name);
/** Null when no backend has that name. */
LILA_SCENE_API std::unique_ptr<Backend> createBackend(std::string_view name);
LILA_SCENE_API std::vector<std::string> backendNames();

/**
 * Nearest-texel dilation on the CPU: a breadth-first flood from the covered texels over 8 neighbours, each new texel
 * copying the value of the seed it grew from, so exact maps stay exact and covered texels never change.
 */
LILA_SCENE_API void dilateCpu(std::span<glm::vec4> values, std::span<uint8_t> covered, int width, int height, int padding);
} // namespace lila::bake
