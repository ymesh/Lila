#pragma once

#include "core/Export.h"

#include "bake/BakeTypes.h"
#include "scene/Node.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lila::geo
{
class Mesh;
}

/**
 * What a rebake reuses from the last bake, so changing a map setting and baking again skips the snapshot work: the
 * evaluated highs (bake-only modifier stacks and their soups) and lows, keyed by their source mesh object (the DG hands
 * back the same one until it changes) plus everything else that shapes them. Backends key their own device data
 * (acceleration structures, uploaded buffers) by hashBytes of the soups.
 */
namespace lila::bake
{
struct HighInput;
struct LowInput;
struct MeshSoup;

/** A 64-bit content hash of bytes, a word at a time and in parallel over large inputs. Not cryptographic. */
LILA_SCENE_API uint64_t hashBytes(const void* data, size_t size, uint64_t seed = 0);

template <class T> uint64_t hashVector(const std::vector<T>& v, uint64_t seed = 0) { return hashBytes(v.data(), v.size() * sizeof(T), seed); }

/** Positions, normals, uvs, material slots and colour sets of a soup: what device copies of a high depend on. */
LILA_SCENE_API uint64_t soupHash(const MeshSoup& soup);

/** The identity of a snapshot: the source mesh object and a digest of everything else that shapes it. */
struct SnapshotKey
{
    std::weak_ptr<const geo::Mesh> source;
    const geo::Mesh* sourcePtr = nullptr;
    uint64_t digest = 0;
};

/** Key for a mesh evaluated with world, name and (for highs) a bake-only stack; extra mixes in a cage or other input. */
LILA_SCENE_API SnapshotKey snapshotKey(const std::shared_ptr<const geo::Mesh>& source, const glm::dmat4& world, const std::string& name,
                                       const json& modifiers, uint64_t extra = 0);

/** A cached high snapshot, null when absent or its source mesh has changed since. */
LILA_SCENE_API std::shared_ptr<const HighInput> cachedHigh(const SnapshotKey& key);
LILA_SCENE_API void cacheHigh(const SnapshotKey& key, std::shared_ptr<const HighInput> high);
LILA_SCENE_API std::shared_ptr<const LowInput> cachedLow(const SnapshotKey& key);
LILA_SCENE_API void cacheLow(const SnapshotKey& key, std::shared_ptr<const LowInput> low);

/** What a bake's snapshots lost to the cache's size since beginSnapshotTally. */
struct SnapshotTally
{
    /** Bytes of this bake's own snapshots evicted, or too big to keep: those meshes are prepared again on the next bake. */
    size_t dropped = 0;
    /** What each cache (lows, highs) may hold. */
    size_t budget = 0;
};

/**
 * Starts a bake's tally and reads the budget: the baker/snapshotCacheGb pref, else a quarter of the machine's memory
 * (3 GB at least). Main thread.
 */
LILA_SCENE_API void beginSnapshotTally();
LILA_SCENE_API SnapshotTally snapshotTally();

/** Registers a function every clearBakeCaches call runs (backends drop their device caches). */
LILA_SCENE_API void onClearBakeCaches(std::function<void()> fn);
/** Drops every cached snapshot and asks backends to drop theirs. */
LILA_SCENE_API void clearBakeCaches();
} // namespace lila::bake
