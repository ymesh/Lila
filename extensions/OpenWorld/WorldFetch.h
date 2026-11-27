#pragma once

#include "Fetch.h"
#include "LandCover.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

/**
 * Real data for a world spec in one call: every elevation, OSM, land cover and bathymetry file the spec names that is
 * missing is downloaded straight to that path, after the sizes are known and allowed. Pure apart from the network calls
 * in FetchNet, so tests hand in stand-ins.
 */
namespace openworld::world
{
using json = nlohmann::json;

/** One file a spec names. */
struct FetchItem
{
    /** "<source id>.<kind>", or "terrain.coast.bathymetry" for the terrain spec's coast grid. */
    std::string id;
    /** dem, osm, landcover or bathymetry. */
    std::string kind;
    /** The path as the spec gives it after its relative paths are resolved. */
    std::filesystem::path file;
    terrain::GeoBox box;
    /**
     * Where box came from: source (its bbox), coast (the coast step's own bbox), bathymetry source (the first source
     * carrying a bathymetry grid) or sources (all sources' bboxes together).
     */
    std::string boxFrom;
    bool present = false;
    /** The download cache already holds this area: nothing to download. */
    bool cached = false;
    /** Expected download, bytes; 0 when present or cached. */
    uint64_t bytes = 0;
    /** How bytes was worked out. */
    std::string sizeFrom;
    /** Why it can't be fetched (no bbox); empty when it can. */
    std::string problem;
};

struct FetchPlan
{
    std::vector<FetchItem> items;
    /** Sum over missing items. */
    uint64_t bytes = 0;
    int missing = 0;
    /** A missing bathymetry grid has to come from OpenTopography (not cached). */
    bool keyNeeded = false;
};

/** The network: whole-file GETs, WorldCover's ranged reads, and whether the connection is metered. */
struct FetchNet
{
    terrain::Downloader get;
    std::function<terrain::LandCoverFetch(const terrain::GeoBox&)> landcover;
    /** True on a metered connection, nullopt when unknown. */
    std::function<std::optional<bool>()> metered;
    /** Called between attempts with the pause, ms. */
    std::function<void(int)> wait;
};

/** What runFetch is doing, in order: per item Start, then Bytes as they arrive (and Retry), then Done or Failed. */
struct FetchEvent
{
    enum class Kind
    {
        Start,
        Bytes,
        Retry,
        Done,
        Failed,
    };
    Kind kind = Kind::Start;
    std::string id;
    /** dem, osm, landcover or bathymetry. */
    std::string itemKind;
    /** 1-based among the items to fetch, and how many there are. */
    int index = 0;
    int count = 0;
    /** Received so far (Bytes), written to the spec's path (Done). */
    uint64_t bytes = 0;
    /** The best known size: the server's when it gave one for a single download, else the plan's estimate; 0 unknown. */
    uint64_t expected = 0;
    /** expected came from the server rather than the estimate. */
    bool exact = false;
    /** Start: the download cache holds it, so nothing is downloaded. */
    bool cached = false;
    /** Retry: the attempt that failed and the pause before the next, ms. */
    int attempt = 0;
    int waitMs = 0;
    /** Since the item started. */
    double seconds = 0.0;
    /** Retry and Failed: why. */
    std::string why;
};

struct FetchOptions
{
    /** Download cache (the workspace's terrain/dem_cache): tiles and whole areas are kept and reused. */
    std::filesystem::path cache;
    /** Use the cache only; a missing file that isn't there is an error. */
    bool offline = false;
    /** Allow more than consentMB, or any download on a metered connection. */
    bool consent = false;
    /** OpenTopography key for GEBCO; never logged or put in a message. */
    std::string apiKey;
    /** Tries per download when the server can't be reached or answers 429 or 5xx. */
    int attempts = 3;
    double consentMB = 25.0;
    /** Why the saved key was ignored (a placeholder); added to the missing-key message. */
    std::string keyNote;
    /** Called on the thread running runFetch for every event. */
    std::function<void(const FetchEvent&)> onEvent;
    /** Polled during downloads and between attempts; true stops the run (items fetched so far are kept). */
    std::function<bool()> cancelled;
};

/** Copernicus GLO-30 tiles are about this size each. */
inline constexpr uint64_t kCopernicusTileBytes = 25ull << 20;

/**
 * Every file the spec names (remix sources' dem, osm, landcover, bathymetry; terrain.coast.bathymetry.file), whether it
 * is there and how much a download would be. A file named twice is one item. resolve maps a relative path (a spec given as a dict) to a real one.
 */
FetchPlan planFetch(const json& spec, const std::filesystem::path& cache, const std::function<std::filesystem::path(const std::string&)>& resolve = {});

/** The plan as JSON: items (id, kind, file, state present|missing, bytes, megabytes, sizeFrom, bbox, problem), totals. */
json planJson(const FetchPlan& p, const FetchOptions& o);

/** A download size for people: "12 KB" under 100 KB, else "4.7 MB". */
std::string fetchSize(uint64_t bytes);

/** One line naming the missing items and the total: "coast.dem 25.0 MB, coast.osm 4.7 MB; about 29.7 MB". */
std::string planSummary(const FetchPlan& p);

/**
 * For FetchNet downloads: the bytes so far of the transfer running on this thread (total 0 when unknown), which runFetch
 * turns into Bytes events. False asks the transfer to stop. True outside runFetch.
 */
bool reportFetchBytes(uint64_t received, uint64_t total);

/**
 * The plan, or a run's outcome, as a table for Script Output: item, kind, size, status (present, cached, to download,
 * fetched, failed), a total row and the consent and key notes. outcome is a run's items ({id, state fetched|present|failed,
 * bytes}, as runFetch returns them); without it the table is the plan. One string per line.
 */
std::vector<std::string> fetchTable(const FetchPlan& p, const FetchOptions& o, const json* outcome = nullptr);

/**
 * Downloads every missing item to its file. Refuses before any download when an item can't be fetched, when GEBCO is
 * needed without a key, or when the total is over consentMB (or the connection is metered) without consent; the message
 * names the sizes. Each download is retried (attempts) and written whole (a .part file renamed), so a run stopped by a
 * failure resumes where it stopped. Returns items with state fetched|present, bytes written, attempts and notes, plus
 * the attribution files written. Throws std::runtime_error naming the item and why.
 */
json runFetch(const FetchPlan& p, const FetchOptions& o, const FetchNet& net);
} // namespace openworld::world
