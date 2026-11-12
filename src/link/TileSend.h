#pragma once

#include "core/Export.h"
#include "link/RpcClient.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace lila::link
{
namespace fs = std::filesystem;

/** One mesh tile to import, written to disk only when it has to be sent. */
struct TileItem
{
    /** Stable across sends (e.g. "cliff_3/x2_y5"); the manifest is keyed by it. */
    std::string key;
    /** Content hash: a tile whose hash matches the manifest is skipped. */
    uint64_t hash = 0;
    /** importAsset mapping (contentPath, kind, ...); see importParams. */
    json mapping;
    /** materialParams for the tile's slots, so LilaLink assigns them (masters included); null leaves the importer's own. */
    json materials;
    /** Writes the tile's file (FBX) and returns its path. */
    std::function<fs::path()> write;
    /** Runs after the import with the imported asset path, e.g. to place it; throwing fails the tile. */
    std::function<void(const std::string& asset)> after;
    /** The placement group `after` creates (listPlacements); an unchanged tile whose group is gone from the level is placed again. */
    std::string group;
};

struct TileSendOptions
{
    /** Manifest file name under cacheDir()/manifests; one per send target (e.g. "<terrain id>_cliffs"). */
    std::string manifest;
    /** Folder for the manifest; empty is cacheDir()/manifests. */
    fs::path manifestDir;
    /** Sends every tile, ignoring the manifest. */
    bool force = false;
    /** Asks Unreal whether unchanged tiles' assets and placements still exist; a lost asset is imported again, a lost placement placed again. */
    bool verify = true;
    /** Deletes each tile's file once imported, so a large send never holds more than one on disk. */
    bool deleteFiles = true;
    /** Asks Unreal to collect garbage after this many imports (0 never), keeping its memory flat on long sends. */
    int collectEvery = 8;
    /** Called before each tile with (index, total, key); returning false stops the send (resumable later). */
    std::function<bool(size_t, size_t, const std::string&)> progress;
};

struct TileSendResult
{
    size_t sent = 0;
    size_t skipped = 0;
    /** Unchanged tiles imported again because Unreal no longer had the asset (counted in sent too). */
    size_t restored = 0;
    /** Unchanged tiles whose asset was there but whose placement was gone, placed again without importing. */
    size_t replaced = 0;
    size_t failed = 0;
    bool cancelled = false;
    /** Asset per item, in item order (empty for failures and tiles not reached). */
    std::vector<std::string> assets;
    /** Keys the manifest held that are no longer among the items; dropped from it, for the caller to remove in Unreal. */
    std::vector<std::string> stale;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

/**
 * Imports tiles one at a time, skipping those whose hash the manifest already records and that Unreal still has, and
 * writes the manifest after every tile, so an interrupted send resumes where it stopped. A failed tile is reported and
 * the rest continue.
 */
LILA_SCENE_API TileSendResult sendTiles(RpcClient& client, const std::vector<TileItem>& items, const TileSendOptions& opts);

/** FNV-1a over bytes, chained through seed; for building TileItem hashes. */
LILA_SCENE_API uint64_t hashBytes(const void* data, size_t size, uint64_t seed = 14695981039346656037ull);
} // namespace lila::link
