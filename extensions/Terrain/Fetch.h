#pragma once

#include "Inputs.h"
#include "Projection.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace terrain
{
/** An area in degrees. */
struct GeoBox
{
    double west = 0.0;
    double south = 0.0;
    double east = 0.0;
    double north = 0.0;

    bool valid() const { return east > west && north > south && south >= -90.0 && north <= 90.0 && west >= -180.0 && east <= 180.0; }
    LatLon centre() const { return {(south + north) * 0.5, (west + east) * 0.5}; }
    /** Width and height in metres at the middle latitude. */
    double widthMetres() const;
    double heightMetres() const;
    /** A box size metres wide and tall centred on c. */
    static GeoBox around(LatLon c, double width, double height);
};

/** Where elevation is fetched from. */
enum class DemSource : uint8_t
{
    /** Copernicus GLO-30 DSM tiles from AWS open data: 1 arc-second, surface incl. trees and buildings. */
    Copernicus,
    /** AWS Terrain Tiles (Terrarium PNG, Web Mercator): mixed sources, fast, any zoom. */
    Terrarium,
    /** The OpenTopography global DEM API (COP30, SRTMGL1, NASADEM, AW3D30 ...), with the user's own API key. */
    OpenTopography,
};

const std::vector<std::string>& demSourceNames();
bool parseDemSource(std::string_view name, DemSource* out);

/** Copernicus GLO-30 tile names covering the box, e.g. Copernicus_DSM_COG_10_N46_00_E007_00_DEM. */
std::vector<std::string> copernicusTileNames(const GeoBox& b);
std::string copernicusUrl(const std::string& tileName);

struct TileXY
{
    int z = 0;
    int x = 0;
    int y = 0;
    auto operator<=>(const TileXY&) const = default;
};

/** Global Web Mercator pixel (256 per tile) of a point at zoom z. */
glm::dvec2 mercatorPixel(LatLon p, int z);
/** The zoom whose pixels are at most spacing metres at the box's middle latitude (0 to 15). */
int terrariumZoom(const GeoBox& b, double spacing);
std::vector<TileXY> terrariumTiles(const GeoBox& b, int z);
std::string terrariumUrl(const TileXY& t);

std::string openTopographyUrl(const GeoBox& b, const std::string& dataset, const std::string& apiKey);
/** url with its API_Key value masked, for messages and logs. */
std::string redactedUrl(const std::string& url);
/** What to tell someone who has no OpenTopography API key: where to get one and how to pass and save it. */
extern const char* const kOpenTopographyKeyHelp;

/** Why key can't be a real OpenTopography key (empty, or a placeholder such as "<your key>", "YOUR_KEY" or "..."), else empty. */
std::string apiKeyProblem(const std::string& key);

/**
 * The OpenTopography key a command uses: the passed one (a placeholder throws, before anything downloads), else the
 * saved one (a saved placeholder counts as no key and *note says so). Save only a passed key that got through here.
 */
std::string chooseApiKey(const std::string& passed, const std::string& saved, std::string* note);
/** Overpass query for everything terrainImportOsm reads, inside the box (JSON output). */
std::string overpassQuery(const GeoBox& b);
/** The Overpass API endpoint taking overpassQuery as data=. */
std::string overpassUrl(const GeoBox& b);

/**
 * Geographic tiles merged onto one grid covering the box, at the finest tile's spacing and aligned with it (so samples
 * are copied, not resampled, when the tiles share a grid). Samples no tile covers are 0 (sea level) and counted in
 * uncovered.
 */
Dem mosaicGeographic(const std::vector<Dem>& tiles, const GeoBox& b, size_t* uncovered = nullptr);
/** Terrarium tiles (zoom z, keyed by tile) resampled onto a geographic grid over the box, spacing metres apart. */
Dem mosaicTerrarium(const std::map<TileXY, Dem>& tiles, int z, const GeoBox& b, double spacing, size_t* uncovered = nullptr);

/**
 * A geographic Dem as a float32 GeoTIFF (deflate, EPSG:4326, pixel is area), which readDem reads back exactly. bytes
 * writes 8-bit unsigned samples instead (rounded, voids as 0), for class maps.
 */
void writeGeoTiff(const std::filesystem::path& path, const Dem& d, bool bytes = false);

/** The attribution the source's licence requires wherever the terrain is shown or shipped. */
std::string demAttribution(DemSource s, const std::string& dataset);
/**
 * Adds or replaces the section titled key in dir/ATTRIBUTION.txt, keeping the other sections. Returns the file.
 * Throws std::runtime_error when it can't be written.
 */
std::filesystem::path writeAttribution(const std::filesystem::path& dir, const std::string& key, const std::string& text);

/** Fetches url into dest; returns the HTTP status (200 ok), or 0 with why set when it never reached the server. */
using Downloader = std::function<int(const std::string& url, const std::filesystem::path& dest, std::string* why)>;

struct FetchRequest
{
    DemSource source = DemSource::Copernicus;
    GeoBox box;
    /** Terrarium: the finest spacing wanted, metres (picks the zoom). */
    double spacing = 30.0;
    /** OpenTopography demtype. */
    std::string dataset = "COP30";
    std::string apiKey;
    /** Downloaded tiles are kept here and reused. */
    std::filesystem::path cache;
    /** Use the cache only; a missing tile is an error. */
    bool offline = false;
};

struct FetchResult
{
    /** In degrees, over the box. */
    Dem dem;
    int tiles = 0;
    int downloaded = 0;
    int cached = 0;
    /** Tiles the source has none of (open sea for Copernicus), filled at sea level. */
    int empty = 0;
    std::vector<std::string> notes;
    std::string attribution;
};

/** The cache file an OpenTopography request is kept in. */
std::filesystem::path openTopographyCacheFile(const FetchRequest& r);

/**
 * Downloads (or reads from the cache) and merges the source's tiles over the box. Throws std::runtime_error. An
 * OpenTopography request needs an API key unless offline (then only the cache is read); messages never carry the key.
 */
FetchResult fetchDem(const FetchRequest& r, const Downloader& download);

/** A place found by name. */
struct Place
{
    std::string name;
    LatLon at;
    GeoBox box;
};

/** Nominatim search URL for one result, JSON v2. */
std::string nominatimUrl(const std::string& query);
std::vector<Place> parseNominatim(std::string_view json);
/** Percent-encodes for a URL query value. */
std::string urlEncode(std::string_view s);
} // namespace terrain
