// Real-world elevation by area: tile names and URLs for Copernicus GLO-30, AWS Terrain Tiles and OpenTopography, the tile
// cache, merging tiles onto one grid, GeoTIFF writing, attribution files and place-name lookup results.

#include "Fetch.h"

#include <lila/Profile.h>

#include <miniz.h>
#include <nlohmann/json.hpp>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace terrain
{
namespace fetchdetail
{
namespace fs = std::filesystem;
constexpr double kFetchPi = 3.14159265358979323846;
constexpr double kMaxMercatorLat = 85.0511287798;

double perDegLat(double lat)
{
    const double p = lat * kFetchPi / 180.0;
    return 111132.92 - 559.82 * std::cos(2 * p) + 1.175 * std::cos(4 * p);
}

double perDegLon(double lat)
{
    const double p = lat * kFetchPi / 180.0;
    return 111412.84 * std::cos(p) - 93.5 * std::cos(3 * p);
}

/** The covering grid of a box for tiles with this spacing and a sample at (ax, ay). */
void alignedGrid(const GeoBox& b, double ax, double ay, double dx, double dy, Dem& out)
{
    out.dx = dx;
    out.dy = dy;
    out.x0 = ax + std::ceil((b.west - ax) / dx - 1e-9) * dx;
    out.y0 = ay - std::ceil((ay - b.north) / dy - 1e-9) * dy;
    out.width = int(std::floor((b.east - out.x0) / dx + 1e-9)) + 1;
    out.height = int(std::floor((out.y0 - b.south) / dy + 1e-9)) + 1;
    if (out.width < 2 || out.height < 2)
        throw std::runtime_error("the area is smaller than two samples of the data");
    if (uint64_t(out.width) * uint64_t(out.height) > (uint64_t(1) << 28))
        throw std::runtime_error("the area is " + std::to_string(out.width) + " x " + std::to_string(out.height) + " samples; take a smaller one");
    out.geographic = true;
    out.epsg = 4326;
    out.metres.assign(size_t(out.width) * size_t(out.height), 0.0f);
}

std::string readText(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return f ? std::string((std::istreambuf_iterator<char>(f)), {}) : std::string();
}

void putU16(std::vector<uint8_t>& o, uint16_t v)
{
    o.push_back(uint8_t(v));
    o.push_back(uint8_t(v >> 8));
}

void putU32(std::vector<uint8_t>& o, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        o.push_back(uint8_t(v >> (8 * i)));
}

void putF64(std::vector<uint8_t>& o, double v)
{
    const uint64_t u = std::bit_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i)
        o.push_back(uint8_t(u >> (8 * i)));
}
} // namespace fetchdetail

using namespace fetchdetail;

double GeoBox::widthMetres() const { return (east - west) * perDegLon(centre().lat); }
double GeoBox::heightMetres() const { return (north - south) * perDegLat(centre().lat); }

GeoBox GeoBox::around(LatLon c, double width, double height)
{
    const double hw = width * 0.5 / perDegLon(c.lat), hh = height * 0.5 / perDegLat(c.lat);
    return {std::max(-180.0, c.lon - hw), std::max(-90.0, c.lat - hh), std::min(180.0, c.lon + hw), std::min(90.0, c.lat + hh)};
}

const std::vector<std::string>& demSourceNames()
{
    static const std::vector<std::string> n = {"copernicus", "terrarium", "opentopography"};
    return n;
}

bool parseDemSource(std::string_view name, DemSource* out)
{
    const auto& n = demSourceNames();
    auto it = std::find(n.begin(), n.end(), name);
    if (it == n.end())
        return false;
    *out = DemSource(it - n.begin());
    return true;
}

std::vector<std::string> copernicusTileNames(const GeoBox& b)
{
    std::vector<std::string> out;
    const int lat0 = int(std::floor(b.south)), lat1 = int(std::floor(b.north - 1e-9));
    const int lon0 = int(std::floor(b.west)), lon1 = int(std::floor(b.east - 1e-9));
    for (int lat = lat1; lat >= lat0; --lat)
        for (int lon = lon0; lon <= lon1; ++lon)
        {
            char name[96];
            std::snprintf(name, sizeof name, "Copernicus_DSM_COG_10_%c%02d_00_%c%03d_00_DEM", lat < 0 ? 'S' : 'N', std::abs(lat), lon < 0 ? 'W' : 'E', std::abs(lon));
            out.push_back(name);
        }
    return out;
}

std::string copernicusUrl(const std::string& tileName) { return "https://copernicus-dem-30m.s3.amazonaws.com/" + tileName + "/" + tileName + ".tif"; }

glm::dvec2 mercatorPixel(LatLon p, int z)
{
    const double n = 256.0 * std::ldexp(1.0, z);
    const double lat = std::clamp(p.lat, -kMaxMercatorLat, kMaxMercatorLat) * kFetchPi / 180.0;
    return {(p.lon + 180.0) / 360.0 * n, (1.0 - std::log(std::tan(lat) + 1.0 / std::cos(lat)) / kFetchPi) * 0.5 * n};
}

int terrariumZoom(const GeoBox& b, double spacing)
{
    const double equator = 2 * kFetchPi * 6378137.0 / 256.0;
    const double c = std::cos(std::clamp(b.centre().lat, -kMaxMercatorLat, kMaxMercatorLat) * kFetchPi / 180.0);
    for (int z = 0; z <= 15; ++z)
        if (equator * c / std::ldexp(1.0, z) <= std::max(spacing, 1e-3))
            return z;
    return 15;
}

std::vector<TileXY> terrariumTiles(const GeoBox& b, int z)
{
    const glm::dvec2 nw = mercatorPixel({b.north, b.west}, z), se = mercatorPixel({b.south, b.east}, z);
    const int last = (1 << z) - 1;
    std::vector<TileXY> out;
    const int x0 = std::clamp(int(std::floor((nw.x - 1.0) / 256.0)), 0, last), x1 = std::clamp(int(std::floor((se.x + 1.0) / 256.0)), 0, last);
    const int y0 = std::clamp(int(std::floor((nw.y - 1.0) / 256.0)), 0, last), y1 = std::clamp(int(std::floor((se.y + 1.0) / 256.0)), 0, last);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            out.push_back({z, x, y});
    return out;
}

std::string terrariumUrl(const TileXY& t)
{
    return "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/" + std::to_string(t.z) + "/" + std::to_string(t.x) + "/" + std::to_string(t.y) + ".png";
}

std::string urlEncode(std::string_view s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s)
    {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += char(c);
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string openTopographyUrl(const GeoBox& b, const std::string& dataset, const std::string& apiKey)
{
    char box[200];
    std::snprintf(box, sizeof box, "south=%.7f&north=%.7f&west=%.7f&east=%.7f", b.south, b.north, b.west, b.east);
    return "https://portal.opentopography.org/API/globaldem?demtype=" + urlEncode(dataset) + "&" + box + "&outputFormat=GTiff&API_Key=" + urlEncode(apiKey);
}

std::string redactedUrl(const std::string& url)
{
    const size_t at = url.find("API_Key=");
    if (at == std::string::npos)
        return url;
    const size_t from = at + 8, to = url.find('&', from);
    return url.substr(0, from) + "***" + (to == std::string::npos ? std::string() : url.substr(to));
}

const char* const kOpenTopographyKeyHelp =
    "OpenTopography needs your API key (free: sign in at https://portal.opentopography.org, then My Account > Request API Key); "
    "pass apiKey='...' with saveKey=True once and later fetches use the saved key";

std::string apiKeyProblem(const std::string& key)
{
    std::string lower;
    for (char c : key)
        if (!std::isspace(static_cast<unsigned char>(c)))
            lower += char(std::tolower(static_cast<unsigned char>(c)));
    if (lower.empty())
        return "it is empty";
    if (lower.find_first_of("<>") != std::string::npos)
        return "it contains < or >";
    if (lower.find("your") != std::string::npos)
        return "it contains \"your\"";
    if (lower.find_first_not_of(".*") == std::string::npos)
        return "it is only dots or stars";
    return {};
}

std::string chooseApiKey(const std::string& passed, const std::string& saved, std::string* note)
{
    if (!passed.empty())
    {
        if (const std::string why = apiKeyProblem(passed); !why.empty())
            throw std::runtime_error("apiKey looks like a placeholder, not an OpenTopography key (" + why +
                                     "); nothing was downloaded or saved. Get your key at https://portal.opentopography.org (My Account > Request API Key)");
        return passed;
    }
    if (saved.empty())
        return {};
    if (const std::string why = apiKeyProblem(saved); !why.empty())
    {
        if (note)
            *note = "the saved OpenTopography key is a placeholder (" + why + "), so it was ignored; pass your real key as apiKey= with saveKey=True to replace it";
        return {};
    }
    return saved;
}

std::string overpassQuery(const GeoBox& b)
{
    char box[160];
    std::snprintf(box, sizeof box, "(%.7f,%.7f,%.7f,%.7f)", b.south, b.west, b.north, b.east);
    const std::string bb = box;
    std::string q = "[out:json][timeout:300];(";
    for (const char* sel : {"way[\"highway\"]", "way[\"waterway\"]", "way[\"natural\"]", "way[\"landuse\"]", "way[\"leisure\"]",
                            "way[\"building\"]", "way[\"building:part\"]", "way[\"amenity\"]", "way[\"shop\"]", "way[\"tourism\"]",
                            "way[\"office\"]", "way[\"man_made\"]", "way[\"aeroway\"]", "way[\"harbour\"]", "way[\"place\"]",
                            "node[\"amenity\"]", "node[\"shop\"]", "node[\"tourism\"]", "node[\"office\"]", "node[\"place\"]",
                            "node[\"craft\"]", "node[\"man_made\"=\"lighthouse\"]", "node[\"natural\"~\"peak|bay|beach\"]",
                            "relation[\"type\"=\"multipolygon\"][\"natural\"]", "relation[\"type\"=\"multipolygon\"][\"landuse\"]",
                            "relation[\"type\"=\"multipolygon\"][\"building\"]", "relation[\"type\"=\"multipolygon\"][\"amenity\"]",
                            "relation[\"type\"=\"multipolygon\"][\"leisure\"]", "relation[\"type\"=\"multipolygon\"][\"place\"]",
                            "relation[\"type\"=\"multipolygon\"][\"man_made\"]"})
        q += std::string(sel) + bb + ";";
    return q + ");out body;>;out skel qt;";
}

std::string overpassUrl(const GeoBox& b) { return "https://overpass-api.de/api/interpreter?data=" + urlEncode(overpassQuery(b)); }

std::filesystem::path openTopographyCacheFile(const FetchRequest& r)
{
    char name[160];
    std::snprintf(name, sizeof name, "%s_%.5f_%.5f_%.5f_%.5f.tif", r.dataset.c_str(), r.box.west, r.box.south, r.box.east, r.box.north);
    return r.cache / "opentopography" / name;
}

Dem mosaicGeographic(const std::vector<Dem>& tiles, const GeoBox& b, size_t* uncovered)
{
    LILA_ZONE();

    const Dem* ref = nullptr;
    for (const Dem& t : tiles)
    {
        if (!t.geographic || !(t.dx > 0.0 && t.dy > 0.0))
            throw std::runtime_error("a tile has no geographic georeference");
        if (!ref || t.dx * t.dy < ref->dx * ref->dy)
            ref = &t;
    }
    if (!ref)
        throw std::runtime_error("no tiles");
    Dem out;
    alignedGrid(b, ref->x0, ref->y0, ref->dx, ref->dy, out);
    std::vector<size_t> missing(size_t(out.height), 0);
    tbb::parallel_for(0, out.height, [&](int r) {
        const double lat = out.y0 - out.dy * r;
        for (int c = 0; c < out.width; ++c)
        {
            const double lon = out.x0 + out.dx * c;
            const Dem* hit = nullptr;
            for (const Dem& t : tiles)
                if (lon >= t.x0 - t.dx * 0.5 && lon < t.x0 + t.dx * (double(t.width) - 0.5) && lat <= t.y0 + t.dy * 0.5 && lat > t.y0 - t.dy * (double(t.height) - 0.5))
                {
                    hit = &t;
                    break;
                }
            const size_t k = size_t(r) * size_t(out.width) + size_t(c);
            if (!hit)
            {
                ++missing[size_t(r)];
                continue;
            }
            const double fx = std::clamp((lon - hit->x0) / hit->dx, 0.0, double(hit->width - 1));
            const double fy = std::clamp((hit->y0 - lat) / hit->dy, 0.0, double(hit->height - 1));
            const int x0 = std::min(int(fx), hit->width - 2), y0 = std::min(int(fy), hit->height - 2);
            const double tx = fx - x0, ty = fy - y0;
            auto at = [&](int i, int j) { return double(hit->metres[size_t(j) * size_t(hit->width) + size_t(i)]); };
            // Exact on shared grids: skip the zero-weight neighbours so a void beside a sample doesn't spread into it.
            double sum = 0.0, wsum = 0.0;
            const double wts[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
            const double vals[4] = {at(x0, y0), at(x0 + 1, y0), at(x0, y0 + 1), at(x0 + 1, y0 + 1)};
            bool isVoid = false;
            for (int q = 0; q < 4; ++q)
                if (wts[q] > 1e-9)
                {
                    if (!std::isfinite(vals[q]))
                        isVoid = true;
                    sum += wts[q] * vals[q];
                    wsum += wts[q];
                }
            out.metres[k] = isVoid ? std::numeric_limits<float>::quiet_NaN() : float(sum / wsum);
        }
    });
    if (uncovered)
    {
        *uncovered = 0;
        for (size_t m : missing)
            *uncovered += m;
    }
    return out;
}

Dem mosaicTerrarium(const std::map<TileXY, Dem>& tiles, int z, const GeoBox& b, double spacing, size_t* uncovered)
{
    LILA_ZONE();

    const LatLon mid = b.centre();
    Dem out;
    alignedGrid(b, b.west, b.north, spacing / perDegLon(mid.lat), spacing / perDegLat(mid.lat), out);
    const int last = (1 << z) * 256 - 1;
    std::vector<size_t> missing(size_t(out.height), 0);
    auto pixel = [&](int gx, int gy, bool& ok) -> double {
        gx = std::clamp(gx, 0, last);
        gy = std::clamp(gy, 0, last);
        auto it = tiles.find({z, gx / 256, gy / 256});
        if (it == tiles.end() || it->second.width != 256 || it->second.height != 256)
        {
            ok = false;
            return 0.0;
        }
        return double(it->second.metres[size_t(gy % 256) * 256 + size_t(gx % 256)]);
    };
    tbb::parallel_for(0, out.height, [&](int r) {
        for (int c = 0; c < out.width; ++c)
        {
            const glm::dvec2 g = mercatorPixel({out.y0 - out.dy * r, out.x0 + out.dx * c}, z) - 0.5;
            const int x0 = int(std::floor(g.x)), y0 = int(std::floor(g.y));
            const double tx = g.x - x0, ty = g.y - y0;
            bool ok = true;
            const double v = (pixel(x0, y0, ok) * (1 - tx) + pixel(x0 + 1, y0, ok) * tx) * (1 - ty) + (pixel(x0, y0 + 1, ok) * (1 - tx) + pixel(x0 + 1, y0 + 1, ok) * tx) * ty;
            if (!ok)
                ++missing[size_t(r)];
            out.metres[size_t(r) * size_t(out.width) + size_t(c)] = ok ? float(v) : 0.0f;
        }
    });
    if (uncovered)
    {
        *uncovered = 0;
        for (size_t m : missing)
            *uncovered += m;
    }
    return out;
}

void writeGeoTiff(const fs::path& path, const Dem& d, bool bytes)
{
    LILA_ZONE();

    if (!d.geographic || d.width < 2 || d.height < 2)
        throw std::runtime_error("writeGeoTiff takes a geographic grid");
    const int rowsPerStrip = 16;
    const int strips = (d.height + rowsPerStrip - 1) / rowsPerStrip;
    std::vector<uint8_t> o = {'I', 'I', 42, 0, 0, 0, 0, 0};
    std::vector<uint32_t> offsets, counts;
    for (int s = 0; s < strips; ++s)
    {
        const int rows = std::min(rowsPerStrip, d.height - s * rowsPerStrip);
        const size_t samples = size_t(rows) * size_t(d.width), size = samples * (bytes ? 1 : 4);
        const float* first = d.metres.data() + size_t(s) * rowsPerStrip * size_t(d.width);
        const uint8_t* src = reinterpret_cast<const uint8_t*>(first);
        std::vector<uint8_t> narrow;
        if (bytes)
        {
            narrow.resize(samples);
            for (size_t i = 0; i < samples; ++i)
                narrow[i] = std::isfinite(first[i]) ? uint8_t(std::clamp(std::lround(first[i]), 0L, 255L)) : 0;
            src = narrow.data();
        }
        mz_ulong packed = mz_compressBound(mz_ulong(size));
        std::vector<uint8_t> buf(packed);
        if (mz_compress2(buf.data(), &packed, src, mz_ulong(size), 6) != MZ_OK)
            throw std::runtime_error("deflate failed");
        offsets.push_back(uint32_t(o.size()));
        counts.push_back(uint32_t(packed));
        o.insert(o.end(), buf.begin(), buf.begin() + ptrdiff_t(packed));
        if (o.size() & 1)
            o.push_back(0);
    }
    auto array = [&](auto&& write) {
        const uint32_t at = uint32_t(o.size());
        write();
        if (o.size() & 1)
            o.push_back(0);
        return at;
    };
    const uint32_t offsetsAt = array([&] {
        for (uint32_t v : offsets)
            putU32(o, v);
    });
    const uint32_t countsAt = array([&] {
        for (uint32_t v : counts)
            putU32(o, v);
    });
    const uint32_t scaleAt = array([&] {
        putF64(o, d.dx);
        putF64(o, d.dy);
        putF64(o, 0.0);
    });
    const uint32_t tieAt = array([&] {
        for (double v : {0.0, 0.0, 0.0, d.x0 - d.dx * 0.5, d.y0 + d.dy * 0.5, 0.0})
            putF64(o, v);
    });
    const uint16_t keys[] = {1, 1, 0, 3, 1024, 0, 1, 2, 1025, 0, 1, 1, 2048, 0, 1, 4326};
    const uint32_t keysAt = array([&] {
        for (uint16_t v : keys)
            putU16(o, v);
    });
    struct Entry
    {
        uint16_t tag, type;
        uint32_t count, value;
    };
    const bool oneStrip = strips == 1;
    const Entry entries[] = {
        {256, 4, 1, uint32_t(d.width)},
        {257, 4, 1, uint32_t(d.height)},
        {258, 3, 1, bytes ? 8u : 32u},
        {259, 3, 1, 8},
        {262, 3, 1, 1},
        {273, 4, uint32_t(strips), oneStrip ? offsets[0] : offsetsAt},
        {277, 3, 1, 1},
        {278, 4, 1, uint32_t(rowsPerStrip)},
        {279, 4, uint32_t(strips), oneStrip ? counts[0] : countsAt},
        {284, 3, 1, 1},
        {339, 3, 1, bytes ? 1u : 3u},
        {33550, 12, 3, scaleAt},
        {33922, 12, 6, tieAt},
        {34735, 3, uint32_t(std::size(keys)), keysAt},
    };
    const uint32_t ifd = uint32_t(o.size());
    putU16(o, uint16_t(std::size(entries)));
    for (const Entry& e : entries)
    {
        putU16(o, e.tag);
        putU16(o, e.type);
        putU32(o, e.count);
        if (e.type == 3 && e.count == 1)
        {
            putU16(o, uint16_t(e.value));
            putU16(o, 0);
        }
        else
            putU32(o, e.value);
    }
    putU32(o, 0);
    for (int i = 0; i < 4; ++i)
        o[size_t(4 + i)] = uint8_t(ifd >> (8 * i));
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.write(reinterpret_cast<const char*>(o.data()), std::streamsize(o.size())))
        throw std::runtime_error("can't write " + path.string());
}

std::string demAttribution(DemSource s, const std::string& dataset)
{
    const std::string copernicus30 = "Produced using Copernicus WorldDEM-30 (c) DLR e.V. 2010-2014 and (c) Airbus Defence and Space GmbH 2014-2018 "
                                     "provided under COPERNICUS by the European Union and ESA; all rights reserved.";
    switch (s)
    {
    case DemSource::Copernicus:
        return copernicus30 + "\nCopernicus DEM GLO-30, from the Registry of Open Data on AWS (https://registry.opendata.aws/copernicus-dem/).";
    case DemSource::Terrarium:
        return "Terrain Tiles, from the Registry of Open Data on AWS (https://registry.opendata.aws/terrain-tiles/). Sources: "
               "ArcticDEM terrain data DEM(s) were created from DigitalGlobe, Inc., imagery and funded under National Science "
               "Foundation awards 1043681, 1559691, and 1542736; Australia terrain data (c) Commonwealth of Australia (Geoscience "
               "Australia) 2017; Austria terrain data (c) offene Daten Osterreichs, Digitales Gelandemodell (DGM) Osterreich; Canada "
               "terrain data contains information licensed under the Open Government Licence - Canada; Europe terrain data produced "
               "using Copernicus data and information funded by the European Union - EU-DEM layers; Global ETOPO1 terrain data U.S. "
               "National Oceanic and Atmospheric Administration; Mexico terrain data source: INEGI, Continental relief, 2016; New "
               "Zealand terrain data Copyright 2011 Crown copyright (c) Land Information New Zealand and the New Zealand Government "
               "(All rights reserved); Norway terrain data (c) Kartverket; United Kingdom terrain data (c) Environment Agency "
               "copyright and/or database right 2015. All rights reserved; United States 3DEP (formerly NED) and global GMTED2010 "
               "and SRTM terrain data courtesy of the U.S. Geological Survey.";
    default: break;
    }
    std::string ds = dataset;
    std::string text = "Elevation data from OpenTopography (https://opentopography.org), dataset " + ds + ".\n";
    if (ds == "COP30")
        text += copernicus30;
    else if (ds == "COP90")
        text += "Produced using Copernicus WorldDEM-90 (c) DLR e.V. 2010-2014 and (c) Airbus Defence and Space GmbH 2014-2018 provided under "
                "COPERNICUS by the European Union and ESA; all rights reserved.";
    else if (ds.starts_with("SRTM"))
        text += "Shuttle Radar Topography Mission (SRTM) data courtesy of NASA JPL and the U.S. Geological Survey.";
    else if (ds == "NASADEM")
        text += "NASADEM courtesy of NASA JPL.";
    else if (ds == "AW3D30" || ds == "AW3D30_E")
        text += "ALOS World 3D - 30m (AW3D30) (c) JAXA.";
    else
        text += "Cite the dataset as its page on opentopography.org asks.";
    return text;
}

fs::path writeAttribution(const fs::path& dir, const std::string& key, const std::string& text)
{
    const fs::path file = dir / "ATTRIBUTION.txt";
    const std::string intro = "Data in this workspace's terrains needs this attribution wherever they are shown or shipped (credits, "
                              "about screen or documentation).\n";
    std::vector<std::pair<std::string, std::string>> sections;
    std::istringstream in(readText(file));
    std::string line, current;
    bool inSection = false;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() > 6 && line.starts_with("== ") && line.ends_with(" =="))
        {
            sections.push_back({line.substr(3, line.size() - 6), {}});
            inSection = true;
            continue;
        }
        if (inSection)
            sections.back().second += line + "\n";
    }
    std::string body = text;
    while (!body.empty() && body.back() == '\n')
        body.pop_back();
    body += "\n";
    auto it = std::find_if(sections.begin(), sections.end(), [&](const auto& s) { return s.first == key; });
    if (it != sections.end())
        it->second = body;
    else
        sections.push_back({key, body});
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f)
        throw std::runtime_error("can't write " + file.string());
    f << intro;
    for (auto& [k, v] : sections)
    {
        while (v.size() > 1 && v.ends_with("\n\n"))
            v.pop_back();
        f << "\n== " << k << " ==\n" << v;
    }
    return file;
}

FetchResult fetchDem(const FetchRequest& r, const Downloader& download)
{
    LILA_ZONE();

    if (!r.box.valid())
        throw std::runtime_error("the area is not a valid box [west, south, east, north] in degrees");
    FetchResult res;
    res.attribution = demAttribution(r.source, r.dataset);
    // One tile: a cached file, a marker that the source has none, or a download.
    auto obtain = [&](const std::string& url, const fs::path& file, bool* none) {
        fs::path marker = file;
        marker += ".none";
        *none = false;
        ++res.tiles;
        if (fs::exists(file))
        {
            ++res.cached;
            return;
        }
        if (fs::exists(marker))
        {
            *none = true;
            ++res.empty;
            return;
        }
        if (r.offline || !download)
            throw std::runtime_error("offline and " + file.filename().string() + " isn't in the cache (" + r.cache.string() + ")");
        fs::create_directories(file.parent_path());
        std::string why;
        fs::path part = file;
        part += ".part";
        const int status = download(url, part, &why);
        if (status == 200)
        {
            fs::rename(part, file);
            ++res.downloaded;
            return;
        }
        std::error_code ec;
        fs::remove(part, ec);
        if (status == 404 || (status == 403 && r.source != DemSource::OpenTopography))
        {
            std::ofstream(marker) << status;
            *none = true;
            ++res.empty;
            return;
        }
        const std::string shown = redactedUrl(url);
        throw std::runtime_error(status ? "the server answered " + std::to_string(status) + (why.empty() ? std::string() : ": " + why) + " for " + shown
                                        : "can't download " + shown + (why.empty() ? std::string() : ": " + why));
    };

    size_t uncovered = 0;
    if (r.source == DemSource::Copernicus)
    {
        const std::vector<std::string> names = copernicusTileNames(r.box);
        if (names.size() > 64)
            throw std::runtime_error("that is " + std::to_string(names.size()) + " Copernicus tiles (each 1 degree, about 25 MB); take a smaller area");
        std::vector<Dem> tiles;
        for (const std::string& n : names)
        {
            const fs::path file = r.cache / "copernicus" / (n + ".tif");
            bool none = false;
            obtain(copernicusUrl(n), file, &none);
            if (!none)
                tiles.push_back(readDem(file, DemFormat::GeoTiff));
        }
        if (tiles.empty())
            throw std::runtime_error("Copernicus has no tiles over this area (open sea)");
        res.dem = mosaicGeographic(tiles, r.box, &uncovered);
        if (res.empty)
            res.notes.push_back(std::to_string(res.empty) + " of " + std::to_string(res.tiles) + " Copernicus tiles don't exist (open sea); that area is at sea level 0");
    }
    else if (r.source == DemSource::Terrarium)
    {
        const int z = terrariumZoom(r.box, r.spacing);
        const std::vector<TileXY> list = terrariumTiles(r.box, z);
        if (list.size() > 400)
            throw std::runtime_error("that is " + std::to_string(list.size()) + " Terrain Tiles at zoom " + std::to_string(z) + "; take a smaller area or a larger spacing");
        std::map<TileXY, Dem> tiles;
        for (const TileXY& t : list)
        {
            const fs::path file = r.cache / "terrarium" / std::to_string(t.z) / std::to_string(t.x) / (std::to_string(t.y) + ".png");
            bool none = false;
            obtain(terrariumUrl(t), file, &none);
            if (!none)
                tiles[t] = readDem(file, DemFormat::Terrarium);
        }
        res.dem = mosaicTerrarium(tiles, z, r.box, r.spacing, &uncovered);
        res.notes.push_back("Terrain Tiles zoom " + std::to_string(z) + " (" + std::to_string(int(std::lround(2 * kFetchPi * 6378137.0 / 256.0 * std::cos(r.box.centre().lat * kFetchPi / 180.0) / std::ldexp(1.0, z)))) + " m pixels)");
    }
    else
    {
        const fs::path file = openTopographyCacheFile(r);
        if (r.apiKey.empty() && !r.offline && !fs::exists(file))
            throw std::runtime_error(kOpenTopographyKeyHelp);
        bool none = false;
        obtain(openTopographyUrl(r.box, r.dataset, r.apiKey), file, &none);
        if (none)
            throw std::runtime_error("OpenTopography has no " + r.dataset + " data for this area");
        Dem d = readDem(file, DemFormat::GeoTiff);
        if (!d.geographic)
            throw std::runtime_error("OpenTopography returned projected data; ask for a global dataset (COP30, SRTMGL1, NASADEM, AW3D30)");
        res.dem = mosaicGeographic({d}, r.box, &uncovered);
    }
    if (uncovered)
        res.notes.push_back(std::to_string(uncovered) + " samples had no data and are at sea level 0");
    return res;
}

std::string nominatimUrl(const std::string& query)
{
    return "https://nominatim.openstreetmap.org/search?format=jsonv2&limit=1&q=" + urlEncode(query);
}

std::vector<Place> parseNominatim(std::string_view text)
{
    std::vector<Place> out;
    const nlohmann::json j = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    if (!j.is_array())
        throw std::runtime_error("unexpected answer from the place search");
    auto num = [](const nlohmann::json& v) { return v.is_string() ? std::strtod(v.get<std::string>().c_str(), nullptr) : v.is_number() ? v.get<double>() : 0.0; };
    for (const nlohmann::json& e : j)
    {
        Place p;
        p.name = e.value("display_name", std::string());
        p.at = {num(e.value("lat", nlohmann::json())), num(e.value("lon", nlohmann::json()))};
        if (e.contains("boundingbox") && e["boundingbox"].is_array() && e["boundingbox"].size() == 4)
        {
            const nlohmann::json& b = e["boundingbox"];
            p.box = {num(b[2]), num(b[0]), num(b[3]), num(b[1])};
        }
        out.push_back(std::move(p));
    }
    return out;
}
} // namespace terrain
