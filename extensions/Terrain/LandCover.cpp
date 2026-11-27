// ESA WorldCover land cover: tile names and URLs, windowed reads of its cloud-optimised GeoTIFFs over HTTP ranges, and
// class codes placed on a terrain as the worldcover mask and the landcover_<class> masks the OSM import writes.

#include "LandCover.h"

#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <stdexcept>

namespace terrain
{
namespace
{
/** WorldCover's pixel: 1/12000 degree, about 10 m at the equator. */
constexpr double kWorldCoverStep = 1.0 / 12000.0;
constexpr int kWorldCoverTileDegrees = 3;
} // namespace

std::vector<std::string> worldCoverTileNames(const GeoBox& b)
{
    std::vector<std::string> out;
    auto snap = [](double v) { return int(std::floor(v / kWorldCoverTileDegrees)) * kWorldCoverTileDegrees; };
    // The east and north edges are exclusive, so a box ending on a tile edge doesn't take the next tile.
    const int lat1 = snap(std::nextafter(b.north, -90.0)), lon1 = snap(std::nextafter(b.east, -180.0));
    for (int lat = lat1; lat >= snap(b.south); lat -= kWorldCoverTileDegrees)
        for (int lon = snap(b.west); lon <= lon1; lon += kWorldCoverTileDegrees)
        {
            char name[16];
            std::snprintf(name, sizeof name, "%c%02d%c%03d", lat < 0 ? 'S' : 'N', std::abs(lat), lon < 0 ? 'W' : 'E', std::abs(lon));
            out.push_back(name);
        }
    return out;
}

std::string worldCoverUrl(const std::string& tile)
{
    return "https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/ESA_WorldCover_10m_2021_v200_" + tile + "_Map.tif";
}

std::string worldCoverClass(int code)
{
    switch (code)
    {
    case 10: return "forest";
    case 20: return "scrub";
    case 30: return "grassland";
    case 40: return "farmland";
    case 50: return "urban";
    case 60: return "rock";
    case 70: return "glacier";
    case 80: return "water";
    case 90:
    case 95: return "wetland";
    case 100: return "grassland";
    default: return {};
    }
}

const char* worldCoverAttribution()
{
    return "\xC2\xA9 ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021) processed by ESA WorldCover "
           "consortium\nESA WorldCover 10 m 2021 v200, licensed CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/), "
           "https://esa-worldcover.org";
}

LandCoverFetch fetchWorldCover(const GeoBox& b, const RangeDownloader& download)
{
    LILA_ZONE();

    if (!b.valid())
        throw std::runtime_error("the area is not a valid box [west, south, east, north] in degrees");
    const std::vector<std::string> names = worldCoverTileNames(b);
    if (names.size() > 16)
        throw std::runtime_error("that is " + std::to_string(names.size()) + " WorldCover tiles (3 degrees each); take a smaller area");
    // Global pixel indices from the north-west corner of the world, one pixel of margin around the box.
    const int64_t gx0 = int64_t(std::floor((b.west + 180.0) / kWorldCoverStep)) - 1, gx1 = int64_t(std::ceil((b.east + 180.0) / kWorldCoverStep)) + 1;
    const int64_t gy0 = int64_t(std::floor((90.0 - b.north) / kWorldCoverStep)) - 1, gy1 = int64_t(std::ceil((90.0 - b.south) / kWorldCoverStep)) + 1;
    if ((gx1 - gx0) * (gy1 - gy0) > (int64_t(1) << 28))
        throw std::runtime_error("the area is " + std::to_string(gx1 - gx0) + " x " + std::to_string(gy1 - gy0) + " WorldCover samples; take a smaller one");

    LandCoverFetch res;
    Dem& out = res.codes;
    out.width = int(gx1 - gx0);
    out.height = int(gy1 - gy0);
    out.geographic = true;
    out.epsg = 4326;
    out.dx = out.dy = kWorldCoverStep;
    out.x0 = -180.0 + (double(gx0) + 0.5) * kWorldCoverStep;
    out.y0 = 90.0 - (double(gy0) + 0.5) * kWorldCoverStep;
    out.metres.assign(size_t(out.width) * size_t(out.height), 0.0f);

    for (const std::string& tile : names)
    {
        const std::string url = worldCoverUrl(tile);
        ++res.tiles;
        bool missing = false;
        ByteRange range = [&](uint64_t at, uint64_t n, std::vector<uint8_t>& got) {
            std::string why;
            const int status = download(url, at, n, &got, &why);
            if (status == 200 || status == 206)
            {
                res.bytes += got.size();
                return;
            }
            missing = status == 404 || status == 403;
            throw std::runtime_error(status ? "the server answered " + std::to_string(status) + (why.empty() ? std::string() : ": " + why) + " for " + url
                                            : "can't download " + url + (why.empty() ? std::string() : ": " + why));
        };
        std::unique_ptr<GeoTiffWindowReader> reader;
        try
        {
            reader = std::make_unique<GeoTiffWindowReader>(range, tile);
        }
        catch (const std::exception&)
        {
            if (!missing)
                throw;
            ++res.empty;
            continue;
        }
        const Dem& in = reader->info();
        if (!in.geographic || std::abs(in.dx - kWorldCoverStep) > kWorldCoverStep * 1e-6 || std::abs(in.dy - kWorldCoverStep) > kWorldCoverStep * 1e-6)
            throw std::runtime_error(tile + " isn't on WorldCover's 1/12000 degree grid");
        const int64_t tx = std::llround((in.x0 - in.dx * 0.5 + 180.0) / kWorldCoverStep), ty = std::llround((90.0 - in.y0 - in.dy * 0.5) / kWorldCoverStep);
        const int x0 = int(std::clamp<int64_t>(gx0 - tx, 0, in.width)), x1 = int(std::clamp<int64_t>(gx1 - tx, 0, in.width));
        const int y0 = int(std::clamp<int64_t>(gy0 - ty, 0, in.height)), y1 = int(std::clamp<int64_t>(gy1 - ty, 0, in.height));
        if (x0 >= x1 || y0 >= y1)
            continue;
        const Dem w = reader->window(x0, y0, x1, y1);
        for (int r = 0; r < w.height; ++r)
            for (int c = 0; c < w.width; ++c)
            {
                const float v = w.metres[size_t(r) * size_t(w.width) + size_t(c)];
                const int64_t ox = tx + x0 + c - gx0, oy = ty + y0 + r - gy0;
                out.metres[size_t(oy) * size_t(out.width) + size_t(ox)] = std::isfinite(v) ? v : 0.0f;
            }
    }
    if (res.empty == res.tiles)
        throw std::runtime_error("WorldCover has no tiles over this area (open sea)");
    return res;
}

std::array<int, 4> rasterWindow(const Dem& info, const Heightfield& h, const Georef& g)
{
    const Georef dg = georefOf(info);
    if (!dg.geographicOk())
        throw std::runtime_error("the land cover is in a projection Lila doesn't convert (EPSG " + std::to_string(info.epsg) + "); give it in degrees or UTM");
    if (!(info.dx > 0.0 && info.dy > 0.0))
        throw std::runtime_error("the land cover file has no georeference");
    double c0 = 1e300, c1 = -1e300, r0 = 1e300, r1 = -1e300;
    const glm::dvec2 lo = h.origin, hi = h.origin + h.extent();
    for (int i = 0; i <= 16; ++i)
        for (int j = 0; j <= 16; ++j)
        {
            const glm::dvec2 xy = dg.project(g.toGeographic(lo + (hi - lo) * glm::dvec2(i / 16.0, j / 16.0)));
            const double c = (xy.x - info.x0) / info.dx, r = (info.y0 - xy.y) / info.dy;
            c0 = std::min(c0, c);
            c1 = std::max(c1, c);
            r0 = std::min(r0, r);
            r1 = std::max(r1, r);
        }
    auto clampTo = [](double v, int n) { return int(std::clamp(v, 0.0, double(n))); };
    std::array<int, 4> w = {clampTo(std::floor(c0) - 2, info.width), clampTo(std::floor(r0) - 2, info.height), clampTo(std::ceil(c1) + 3, info.width),
                            clampTo(std::ceil(r1) + 3, info.height)};
    if (w[0] >= w[2] || w[1] >= w[3])
        w = {0, 0, 0, 0};
    return w;
}

std::map<std::string, double> rasterizeWorldCover(Heightfield& h, const Dem& codes, const Georef& g, const std::string& mask, bool classes)
{
    LILA_ZONE();

    const Georef dg = georefOf(codes);
    if (!dg.geographicOk())
        throw std::runtime_error("the land cover is in a projection Lila doesn't convert (EPSG " + std::to_string(codes.epsg) + ")");
    auto codeAt = [&](glm::dvec2 p) -> uint8_t {
        const glm::dvec2 xy = dg.project(g.toGeographic(p));
        const int64_t c = std::llround((xy.x - codes.x0) / codes.dx), r = std::llround((codes.y0 - xy.y) / codes.dy);
        if (c < 0 || r < 0 || c >= codes.width || r >= codes.height)
            return 0;
        const float v = codes.metres[size_t(r) * size_t(codes.width) + size_t(c)];
        return std::isfinite(v) ? uint8_t(std::clamp<long>(std::lround(v), 0, 255)) : uint8_t(0);
    };
    std::vector<uint8_t> code(h.size(), 0);
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
            code[h.index(x, z)] = codeAt(h.samplePos(x, z));
    });

    std::map<int, size_t> count;
    for (uint8_t c : code)
        ++count[c];
    std::vector<float>& m = h.mask(mask);
    for (size_t i = 0; i < code.size(); ++i)
        m[i] = float(code[i]) / 255.0f;

    std::map<std::string, double> share;
    const double n = double(h.size());
    for (const auto& [c, k] : count)
    {
        const std::string cls = worldCoverClass(c);
        share[cls.empty() ? "nodata" : cls] += double(k) / n;
    }
    if (!classes)
        return share;
    // Class masks are the share of each sample's cell the class covers: a grid coarser than the 10 m pixels gets
    // fractions instead of whichever pixel its centre lands on, so edges don't step along the grid.
    const double pixelCm = std::abs(codes.dy) * (codes.geographic ? 11113200.0 : 100.0);
    const int sub = std::clamp(int(std::ceil(h.cell / std::max(pixelCm, 1.0) - 1e-6)), 1, 5);
    std::vector<int> slot(256, -1);
    std::vector<std::string> names;
    for (int c = 0; c < 256; ++c)
        if (const std::string cls = worldCoverClass(c); !cls.empty())
        {
            const auto it = std::find(names.begin(), names.end(), cls);
            slot[size_t(c)] = int(it - names.begin());
            if (it == names.end())
                names.push_back(cls);
        }
    std::vector<std::vector<float>> frac(names.size());
    std::vector<uint8_t> seen(names.size(), 0);
    for (const auto& [c, k] : count)
        if (slot[size_t(c)] >= 0)
            seen[size_t(slot[size_t(c)])] = 1;
    std::mutex lock;
    tbb::parallel_for(0, h.nz, [&](int z) {
        std::vector<float> row(names.size() * size_t(h.nx), 0.0f);
        std::vector<uint8_t> rowSeen(names.size(), 0);
        for (int x = 0; x < h.nx; ++x)
        {
            const glm::dvec2 p = h.samplePos(x, z);
            for (int sz = 0; sz < sub; ++sz)
                for (int sx = 0; sx < sub; ++sx)
                {
                    const glm::dvec2 o = sub == 1 ? glm::dvec2(0.0) : (glm::dvec2(sx, sz) + 0.5) / double(sub) - 0.5;
                    const int k = slot[codeAt(p + o * h.cell)];
                    if (k >= 0)
                    {
                        row[size_t(k) * size_t(h.nx) + size_t(x)] += 1.0f / float(sub * sub);
                        rowSeen[size_t(k)] = 1;
                    }
                }
        }
        std::lock_guard l(lock);
        for (size_t k = 0; k < names.size(); ++k)
        {
            if (!rowSeen[k] && !seen[k])
                continue;
            seen[k] = 1;
            if (frac[k].empty())
                frac[k].assign(h.size(), 0.0f);
            std::copy(row.begin() + std::ptrdiff_t(k * size_t(h.nx)), row.begin() + std::ptrdiff_t((k + 1) * size_t(h.nx)), frac[k].begin() + std::ptrdiff_t(h.index(0, z)));
        }
    });
    for (size_t k = 0; k < names.size(); ++k)
        if (seen[k])
            h.masks["landcover_" + names[k]] = frac[k].empty() ? std::vector<float>(h.size(), 0.0f) : std::move(frac[k]);
    return share;
}
} // namespace terrain
