// World Remix: sources read and placed, heights composed through the warp and the pasted regions, holes filled, seams
// blended and eroded; then the features (Features.cpp) and their repairs (Repair.cpp).

#include "RemixWork.h"

#include "Coast.h"
#include "Erosion.h"
#include "ErosionFilter.h"
#include "ErosionGpu.h"
#include "Inputs.h"
#include "LandCover.h"
#include "Landform.h"
#include "Osm.h"
#include "Shaping.h"

#include <lila/Log.h>
#include <lila/Paths.h>
#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <format>
#include <list>
#include <mutex>
#include <set>

namespace fs = std::filesystem;

namespace openworld
{
using namespace detail;

namespace
{
constexpr const char* kCodeMask = "worldcover";
constexpr double kRemixDeg = 0.017453292519943295;

double metresPerDegLat(double lat) { return 111132.92 - 559.82 * std::cos(2 * lat * kRemixDeg) + 1.175 * std::cos(4 * lat * kRemixDeg); }
double metresPerDegLon(double lat) { return 111412.84 * std::cos(lat * kRemixDeg) - 93.5 * std::cos(3 * lat * kRemixDeg); }

/** A Local georeference with latitude and longitude ll at local point at, as terrainImportOsm(origin=) builds it. */
terrain::Georef localGeoref(glm::dvec2 ll, Vec2 at)
{
    terrain::Georef g;
    g.kind = terrain::ProjectionKind::Local;
    g.latMid = ll.x;
    g.x0 = ll.y;
    g.y0 = ll.x;
    g.mx = metresPerDegLon(ll.x);
    g.my = metresPerDegLat(ll.x);
    g.origin = at;
    return g;
}

double smooth01(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

fs::path resolved(const std::string& f) { return lila::paths::resolvePath(f); }

int64_t stampOf(const std::string& f)
{
    if (f.empty())
        return 0;
    std::error_code ec;
    const auto t = fs::last_write_time(resolved(f), ec);
    return ec ? -1 : int64_t(t.time_since_epoch().count());
}

/** The window of a geographic Dem inside bbox. */
terrain::Dem cropDem(const terrain::Dem& d, const std::vector<double>& bbox)
{
    if (bbox.size() != 4 || !d.geographic || d.dx <= 0.0 || d.dy <= 0.0)
        return d;
    const int x0 = std::clamp(int(std::floor((bbox[0] - d.x0) / d.dx)), 0, d.width - 1);
    const int x1 = std::clamp(int(std::ceil((bbox[2] - d.x0) / d.dx)), 0, d.width - 1);
    const int y0 = std::clamp(int(std::floor((d.y0 - bbox[3]) / d.dy)), 0, d.height - 1);
    const int y1 = std::clamp(int(std::ceil((d.y0 - bbox[1]) / d.dy)), 0, d.height - 1);
    if (x1 - x0 < 1 || y1 - y0 < 1)
        throw std::runtime_error("bbox does not overlap the elevation data");
    terrain::Dem c = d;
    c.width = x1 - x0 + 1;
    c.height = y1 - y0 + 1;
    c.x0 = d.x0 + x0 * d.dx;
    c.y0 = d.y0 - y0 * d.dy;
    c.metres.resize(size_t(c.width) * c.height);
    c.filled.clear();
    for (int y = 0; y < c.height; ++y)
        for (int x = 0; x < c.width; ++x)
            c.metres[size_t(y) * c.width + x] = d.metres[size_t(y + y0) * d.width + x + x0];
    return c;
}

/** How deep sea the elevation holds flat comes out at least, cm. */
constexpr double kFlatSeaDepth = 100.0;

/** WorldCover's permanent water class (the code mask holds code / 255). */
constexpr long kWaterCode = 80;

/**
 * Ground within tolerance of sea level and joined to the data's edge through such ground: sea a land DEM leaves flat.
 * Land cover that says water counts up to a few metres: elevation models hold lagoons and estuaries at their own small
 * heights, which would otherwise stand out of the sea as blocks.
 */
std::vector<float> flatSeaOf(const Heightfield& h, double tolerance)
{
    std::vector<float> out(h.size(), 0.0f);
    const std::vector<float>* code = h.findMask(kCodeMask);
    const double waterTolerance = std::max(tolerance, 300.0);
    std::deque<size_t> open;
    auto push = [&](int x, int z) {
        const size_t i = h.index(x, z);
        const bool water = code && std::lround((*code)[i] * 255.0f) == kWaterCode && std::abs(h.heights[i]) <= waterTolerance;
        if (out[i] == 0.0f && (std::abs(h.heights[i]) <= tolerance || water))
        {
            out[i] = 1.0f;
            open.push_back(i);
        }
    };
    for (int x = 0; x < h.nx; ++x)
        push(x, 0), push(x, h.nz - 1);
    for (int z = 0; z < h.nz; ++z)
        push(0, z), push(h.nx - 1, z);
    while (!open.empty())
    {
        const size_t i = open.front();
        open.pop_front();
        const int x = int(i % size_t(h.nx)), z = int(i / size_t(h.nx));
        if (x > 0)
            push(x - 1, z);
        if (x + 1 < h.nx)
            push(x + 1, z);
        if (z > 0)
            push(x, z - 1);
        if (z + 1 < h.nz)
            push(x, z + 1);
    }
    bool any = false;
    for (float v : out)
        any = any || v > 0.0f;
    return any ? out : std::vector<float>();
}

/** Fractal noise about -1 to 1, featureSize cm. */
double fbm(Vec2 p, double featureSize, uint32_t seed)
{
    double sum = 0.0, amp = 1.0, norm = 0.0, f = 1.0 / featureSize;
    for (int o = 0; o < 4; ++o)
    {
        sum += terrain::gradientNoise(p.x * f, p.y * f, seed + uint32_t(o) * 7919u) * amp;
        norm += amp;
        amp *= 0.5;
        f *= 2.0;
    }
    return sum / norm;
}

/** A bathymetry grid's height at a latitude and longitude, metres; NaN outside it or in a void. */
double gridHeight(const terrain::Dem& g, LatLon ll)
{
    const double dx = g.dx != 0.0 ? g.dx : 1.0 / terrain::kGebcoPerDegree, dy = g.dy != 0.0 ? g.dy : 1.0 / terrain::kGebcoPerDegree;
    double cx, cy;
    if (g.geographic)
    {
        cx = (ll.lon - g.x0) / dx;
        cy = (g.y0 - ll.lat) / dy;
    }
    else
    {
        bool south = false;
        const int zone = terrain::utmZoneOfEpsg(g.epsg, &south);
        const glm::dvec2 en = terrain::latLonToUtm(ll, zone ? zone : terrain::utmZone(ll), south);
        cx = (en.x - g.x0) / dx;
        cy = (g.y0 - en.y) / dy;
    }
    if (!(cx >= 0.0 && cy >= 0.0 && cx <= g.width - 1 && cy <= g.height - 1))
        return std::numeric_limits<double>::quiet_NaN();
    const int x0 = std::min(int(cx), g.width - 2), y0 = std::min(int(cy), g.height - 2);
    const double fx = cx - x0, fy = cy - y0;
    auto at = [&](int x, int y) { return double(g.metres[size_t(y) * size_t(g.width) + size_t(x)]); };
    return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
}
} // namespace

// ---------------------------------------------------------------------------------------------------- sources

std::shared_ptr<const SourceData> loadSource(const Source& s, double landTolerance)
{
    LILA_ZONE();

    struct Key
    {
        std::string text;
        bool operator<(const Key& o) const { return text < o.text; }
    };
    static std::mutex lock;
    static std::map<Key, std::shared_ptr<const SourceData>> cache;
    static std::deque<Key> order;
    std::string key = s.dem + "|" + std::to_string(stampOf(s.dem)) + "|" + s.osm + "|" + std::to_string(stampOf(s.osm)) + "|" + s.landcover + "|" +
                      std::to_string(stampOf(s.landcover)) + "|" + s.bathymetry + "|" + std::to_string(stampOf(s.bathymetry)) + "|" +
                      std::to_string(landTolerance);
    for (double v : s.bbox)
        key += "|" + std::to_string(v);
    {
        std::lock_guard l(lock);
        if (auto it = cache.find({key}); it != cache.end())
        {
            auto copy = std::make_shared<SourceData>(*it->second);
            copy->id = s.id;
            return copy;
        }
    }

    auto out = std::make_shared<SourceData>();
    out->id = s.id;
    if (s.dem.empty() && s.osm.empty())
        throw std::runtime_error("source " + s.id + ": give dem= or osm= (or remixFetch with bbox=)");
    if (!s.osm.empty())
    {
        const fs::path p = resolved(s.osm);
        if (!fs::exists(p))
            throw std::runtime_error("source " + s.id + ": no OSM file " + lila::paths::toUtf8(p));
        out->osm = readOsmDoc(p, &out->notes);
    }
    if (!s.dem.empty())
    {
        const fs::path p = resolved(s.dem);
        if (!fs::exists(p))
            throw std::runtime_error("source " + s.id + ": no elevation file " + lila::paths::toUtf8(p));
        terrain::Dem dem = cropDem(terrain::readDem(p), s.bbox);
        if (!dem.geographic && dem.epsg == 0)
            out->notes.push_back("source " + s.id + ": elevation is not in degrees; its OSM data and land cover can't be placed on it");
        terrain::DemOptions o;
        out->hf = terrain::demToTerrain(std::move(dem), o, terrain::ProjectionKind::Local, 0, true, &out->geo);
        out->hasDem = true;
    }
    else
    {
        // No elevation: flat ground at 1 m over the bbox or the OSM data's extent.
        double w = 180, so = 90, e = -180, n = -90;
        if (s.bbox.size() == 4)
            std::tie(w, so, e, n) = std::tuple(s.bbox[0], s.bbox[1], s.bbox[2], s.bbox[3]);
        else
            for (const OsmDoc::Node& nd : out->osm.nodes)
            {
                w = std::min(w, nd.at.lon), e = std::max(e, nd.at.lon);
                so = std::min(so, nd.at.lat), n = std::max(n, nd.at.lat);
            }
        if (!(e > w && n > so))
            throw std::runtime_error("source " + s.id + ": no elevation and no area (give bbox=)");
        const glm::dvec2 centre((so + n) * 0.5, (w + e) * 0.5);
        const double width = (e - w) * metresPerDegLon(centre.x) * 100.0, depth = (n - so) * metresPerDegLat(centre.x) * 100.0;
        const double cell = 3000.0;
        out->hf = terrain::Heightfield();
        out->hf.cell = cell;
        out->hf.nx = std::max(2, int(std::ceil(width / cell)) + 1);
        out->hf.nz = std::max(2, int(std::ceil(depth / cell)) + 1);
        out->hf.origin = -out->hf.extent() * 0.5;
        out->hf.heights.assign(out->hf.size(), 100.0f);
        out->geo = localGeoref(centre, Vec2(0.0));
        out->notes.push_back("source " + s.id + ": no elevation file; flat ground at 1 m");
    }
    if (!s.bathymetry.empty())
    {
        const fs::path p = resolved(s.bathymetry);
        if (!fs::exists(p))
            throw std::runtime_error("source " + s.id + ": no bathymetry file " + lila::paths::toUtf8(p));
        auto grid = std::make_shared<terrain::Dem>(terrain::readDem(p));
        bool south = false;
        if (grid->width < 2 || grid->height < 2 || (!grid->geographic && terrain::utmZoneOfEpsg(grid->epsg, &south) == 0))
            throw std::runtime_error("source " + s.id + ": " + p.filename().string() + " is not a geographic or UTM grid; bathymetry needs one");
        out->bathymetry = std::move(grid);
    }
    if (!s.landcover.empty())
    {
        const fs::path p = resolved(s.landcover);
        if (!fs::exists(p))
            throw std::runtime_error("source " + s.id + ": no land cover file " + lila::paths::toUtf8(p));
        terrain::rasterizeWorldCover(out->hf, terrain::readDem(p), out->geo, kCodeMask, true);
    }
    if (!out->osm.empty())
    {
        const terrain::OsmExtract x = terrain::extractOsm(toOsmData(out->osm));
        if (!x.areas.empty())
            terrain::rasterizeLandcover(out->hf, x.areas, out->geo);
    }
    // After the land cover, which tells water the elevation holds above sea level.
    if (out->hasDem)
        out->flatSea = flatSeaOf(out->hf, landTolerance);

    std::lock_guard l(lock);
    cache[{key}] = out;
    order.push_back({key});
    while (order.size() > 8)
    {
        cache.erase(order.front());
        order.pop_front();
    }
    return out;
}

Vec2 placeOnBase(const Source& s, double horizontal, Vec2 local)
{
    const double k = s.scale > 0.0 ? s.scale : horizontal;
    return rotated(local * k, s.angle) + s.at;
}

Vec2 baseToSource(const Source& s, double horizontal, Vec2 base)
{
    const double k = s.scale > 0.0 ? s.scale : horizontal;
    return rotated(base - s.at, -s.angle) / k;
}

Polygon footprintOf(const Source& s, double horizontal, const Heightfield& hf)
{
    const Vec2 a = hf.origin, b = hf.origin + hf.extent();
    return {placeOnBase(s, horizontal, a), placeOnBase(s, horizontal, {b.x, a.y}), placeOnBase(s, horizontal, b), placeOnBase(s, horizontal, {a.x, b.y})};
}

// ---------------------------------------------------------------------------------------------------- work helpers

namespace detail
{
void Work::weights(Vec2 base, double* into, SeamAt* seams) const
{
    double under = 0.0;
    const double wet = dry(base);
    for (size_t s = 0; s < data.size(); ++s)
    {
        into[s] = 0.0;
        if (seams)
            seams[s] = SeamAt();
        if (!data[s])
            continue;
        const Heightfield& h = data[s]->hf;
        const Vec2 l = baseToSource(spec.sources[s], spec.horizontal, base) - h.origin, e = h.extent();
        if (l.x < 0.0 || l.y < 0.0 || l.x > e.x || l.y > e.y)
            continue;
        const SourceEdge* E = s < edges.size() ? &edges[s] : nullptr;
        double d = 1e300;
        const double side[4] = {l.x, l.y, e.x - l.x, e.y - l.y};
        for (int k = 0; k < 4; ++k)
            if (!E || E->open[k])
                d = std::min(d, side[k] * scales[s]);
        if (E && E->inside)
            d = std::min(d, bilinear(*E->inside, h, l + h.origin));
        if (d <= 0.0)
            continue;
        double wt = d >= 1e299 ? 1.0 : smooth01(d / (E ? E->feather : feather));
        if (E && E->seam > 0.0 && under > 0.0 && d < 1e299)
        {
            // Over the land under it the source blends in across a band that wanders in from its edge.
            const double wander = E->noise > 0.0 ? E->noise * smooth01(0.5 + 1.2 * fbm(base, 2.0 * (E->noise + E->seam), spec.seams.seed + 31u * uint32_t(s + 1))) : 0.0;
            const double blend = smooth01((d - wander) / E->seam);
            if (seams)
                seams[s] = {d, wander, E->seam, wt, under, wet};
            wt *= 1.0 - under * (1.0 - blend);
        }
        into[s] = wt * wet;
        under = std::max(under, into[s]);
    }
}

double Work::weight(size_t s, Vec2 base) const
{
    if (!data[s])
        return 0.0;
    double all[64];
    std::vector<double> many;
    double* w = all;
    if (data.size() > 64)
    {
        many.resize(data.size());
        w = many.data();
    }
    weights(base, w);
    return w[s];
}

double Work::vertical(size_t s) const { return s < edges.size() ? edges[s].vertical : spec.vertical; }

double Work::dry(Vec2 base) const
{
    double land = 1.0;
    for (const Region& r : spec.regions)
        if (r.op == RegionOp::Sea)
            if (const double sd = signedDistance(r.polygon, base); sd > 0.0)
                land *= 1.0 - smooth01(sd / std::max(spec.seamOf(r), 1.0));
    return land;
}

bool Work::owns(size_t s, Vec2 base) const
{
    if (!data[s])
        return false;
    // Inclusive of the edge: features drawn to the data's border stay.
    const Heightfield& h = data[s]->hf;
    const Vec2 l = baseToSource(spec.sources[s], spec.horizontal, base) - h.origin, e = h.extent();
    const double tol = 1e-6 * std::max(e.x, e.y);
    if (l.x < -tol || l.y < -tol || l.x > e.x + tol || l.y > e.y + tol)
        return false;
    if (s + 1 >= data.size())
        return true;
    std::vector<double> wt(data.size());
    weights(base, wt.data());
    for (size_t k = s + 1; k < data.size(); ++k)
        if (wt[k] >= 0.5)
            return false;
    return true;
}

bool Work::punched(Vec2 base) const
{
    for (const Region& r : spec.regions)
        if (punches(r.op) && inside(r.polygon, base))
            return true;
    return false;
}

bool Work::holed(Vec2 base) const
{
    for (const Region& r : spec.regions)
        if (fills(r.op) && inside(r.polygon, base))
            return true;
    return false;
}

bool Work::onSeaEdge(Vec2 base) const
{
    for (const Region& r : spec.regions)
        if (r.op == RegionOp::Sea && distanceTo(r.polygon, base) < 100.0)
            return true;
    return false;
}

bool Work::covered(Vec2 q, int above) const
{
    for (size_t i = size_t(above + 1); i < spec.regions.size(); ++i)
        if (pastes(spec.regions[i].op) && inside(targets[i], q))
            return true;
    return false;
}

double bilinear(const std::vector<float>& v, const Heightfield& h, Vec2 p)
{
    const double gx = std::clamp((p.x - h.origin.x) / h.cell, 0.0, double(h.nx - 1));
    const double gz = std::clamp((p.y - h.origin.y) / h.cell, 0.0, double(h.nz - 1));
    const int x0 = std::min(int(gx), h.nx - 2), z0 = std::min(int(gz), h.nz - 2);
    const double fx = gx - x0, fz = gz - z0;
    const auto at = [&](int x, int z) { return double(v[size_t(z) * h.nx + x]); };
    return (at(x0, z0) * (1 - fx) + at(x0 + 1, z0) * fx) * (1 - fz) + (at(x0, z0 + 1) * (1 - fx) + at(x0 + 1, z0 + 1) * fx) * fz;
}

size_t nearestIndex(const Heightfield& h, Vec2 p)
{
    const int x = std::clamp(int(std::lround((p.x - h.origin.x) / h.cell)), 0, h.nx - 1);
    const int z = std::clamp(int(std::lround((p.y - h.origin.y) / h.cell)), 0, h.nz - 1);
    return h.index(x, z);
}

int64_t Features::addNode(Vec2 at, OsmTags tags)
{
    const int64_t id = nextId++;
    nodeAt[id] = nodes.size();
    nodes.push_back({id, at, std::move(tags)});
    return id;
}
} // namespace detail

// ---------------------------------------------------------------------------------------------------- composing

namespace
{
/** Every mask the sources carry, the code mask (nearest, never blended) marked. */
struct MaskSet
{
    std::vector<std::string> names;
    std::vector<bool> code;
    /** The seabed mask's index (laid by the remix, not read from sources), or -1. */
    int seabed = -1;
    /** [source][mask] the source's values, or null. */
    std::vector<std::vector<const std::vector<float>*>> of;
};

MaskSet masksOf(const Work& w)
{
    MaskSet m;
    std::set<std::string> names;
    for (const auto& d : w.data)
        if (d)
            for (const auto& [n, v] : d->hf.masks)
                if (n != "voids")
                    names.insert(n);
    if (!w.seaFloor.empty())
        names.insert("seabed");
    m.names.assign(names.begin(), names.end());
    for (const std::string& n : m.names)
        m.code.push_back(n == kCodeMask);
    if (!w.seaFloor.empty())
        m.seabed = int(std::find(m.names.begin(), m.names.end(), "seabed") - m.names.begin());
    m.of.resize(w.data.size());
    for (size_t s = 0; s < w.data.size(); ++s)
        for (const std::string& n : m.names)
            m.of[s].push_back(w.data[s] ? w.data[s]->hf.findMask(n) : nullptr);
    return m;
}

struct Sample
{
    double height = 0.0;
    double seam = 0.0;
    /** The sources' erosion shares blended as their heights are. */
    double erosion = 0.0;
    bool covered = false;
};

/** The base map at b: sources blended in order (later on top) over the sea floor; flat sea in a source takes the floor too. */
Sample baseSample(const Work& w, const MaskSet& m, Vec2 b, float* masks);

Sample baseSample(const Work& w, const MaskSet& m, Vec2 b, float* masks)
{
    Sample out;
    const bool floor = !w.seaFloor.empty();
    const double floorHeight = floor ? w.seaFloor.heightAt(b.x, b.y) : w.spec.map.seaDepth;
    const double floorMask = floor ? bilinear(*w.seaFloor.findMask("seabed"), w.seaFloor, b) : 0.0;
    out.height = floorHeight;
    for (size_t k = 0; k < m.names.size(); ++k)
        masks[k] = int(k) == m.seabed ? float(floorMask) : 0.0f;
    bool under = false;
    double weightsAt[64];
    std::vector<double> many;
    double* W = weightsAt;
    if (w.data.size() > 64)
    {
        many.resize(w.data.size());
        W = many.data();
    }
    SeamAt seamsAt[64];
    std::vector<SeamAt> manySeams;
    SeamAt* S = seamsAt;
    if (w.data.size() > 64)
    {
        manySeams.resize(w.data.size());
        S = manySeams.data();
    }
    w.weights(b, W, S);
    const double grade = w.spec.seams.maxSlope > 0.0 ? w.spec.seams.maxSlope : kSeamGrade;
    for (size_t s = 0; s < w.data.size(); ++s)
    {
        const double wt = W[s];
        if (wt <= 0.0)
            continue;
        const Heightfield& h = w.data[s]->hf;
        const Vec2 l = baseToSource(w.spec.sources[s], w.spec.horizontal, b);
        double hs = h.heightAt(l.x, l.y) * w.vertical(s);
        const double flat = floor && !w.data[s]->flatSea.empty() ? bilinear(w.data[s]->flatSea, h, l) : 0.0;
        if (flat > 0.0)
            // Never shallower than kFlatSeaDepth: where the floor reads as land (a grid cell over a lagoon) the water
            // would otherwise stand as islands at the elevation's own height.
            hs += (std::min({hs, floorHeight, -kFlatSeaDepth}) - hs) * flat;
        double wtHeight = wt;
        if (const SeamAt& sa = S[s]; sa.seam > 0.0 && under)
        {
            // A big step between this source and the land under it (a city on its hills over a plain at another scale)
            // ramps over a band wide enough to keep it under the seam grade, up to four seams.
            const double band = std::clamp(std::abs(hs - out.height) / grade, sa.seam, 4.0 * sa.seam);
            wtHeight = sa.base * (1.0 - sa.under * (1.0 - smooth01((sa.d - sa.wander) / band))) * sa.wet;
        }
        out.height += (hs - out.height) * wtHeight;
        out.erosion += ((s < w.edges.size() ? w.edges[s].erosion : 1.0) - out.erosion) * wt;
        for (size_t k = 0; k < m.names.size(); ++k)
        {
            if (int(k) == m.seabed)
            {
                masks[k] = float(masks[k] + (flat * floorMask - masks[k]) * wt);
                continue;
            }
            const std::vector<float>* v = m.of[s][k];
            if (m.code[k])
            {
                if (wt >= 0.5)
                    masks[k] = v ? (*v)[nearestIndex(h, l)] : 0.0f;
                continue;
            }
            const double value = v ? bilinear(*v, h, l) : 0.0;
            masks[k] = float(masks[k] + (value - masks[k]) * wt);
        }
        if (wt < 1.0 && under)
            out.seam = std::max(out.seam, 1.0 - std::abs(2.0 * wt - 1.0));
        under = true;
        out.covered = true;
    }
    return out;
}

/** Sea regions' seam bands, where the land meets the sea they make: detail and erosion shape the new shore. */
void markSeaSeams(const Work& w, const Heightfield& h, std::vector<float>& seam)
{
    for (const Region& reg : w.spec.regions)
    {
        if (reg.op != RegionOp::Sea)
            continue;
        const double band = std::max(w.spec.seamOf(reg), h.cell);
        Vec2 lo(1e300), hi(-1e300);
        for (Vec2 p : reg.polygon)
            lo = glm::min(lo, p), hi = glm::max(hi, p);
        lo -= Vec2(band), hi += Vec2(band);
        const int xa = std::max(0, int(std::floor((lo.x - h.origin.x) / h.cell))), xb = std::min(h.nx - 1, int(std::ceil((hi.x - h.origin.x) / h.cell)));
        const int za = std::max(0, int(std::floor((lo.y - h.origin.y) / h.cell))), zb = std::min(h.nz - 1, int(std::ceil((hi.y - h.origin.y) / h.cell)));
        tbb::parallel_for(za, zb + 1, [&](int z) {
            for (int x = xa; x <= xb; ++x)
                if (const double sd = std::abs(signedDistance(reg.polygon, h.samplePos(x, z))); sd < band)
                {
                    float& v = seam[h.index(x, z)];
                    v = std::max(v, float(1.0 - sd / band));
                }
        });
    }
}

/** How a pasted region meets the ground round it: the median height step along its edge, the step left, the band it needs. */
struct PasteLevel
{
    double offset = 0.0, residual = 0.0, band = 0.0;
};

PasteLevel pasteLevel(const Work& w, const MaskSet& m, const Heightfield& h, const Polygon& t, const Rigid& motion, double extent)
{
    std::vector<float> mk(m.names.size());
    std::vector<double> steps;
    for (size_t k = 0; k < t.size(); ++k)
    {
        const Vec2 a = t[k], b = t[(k + 1) % t.size()];
        const int n = std::max(1, int(glm::length(b - a) / h.cell));
        for (int i = 0; i < n; ++i)
        {
            const Vec2 q = a + (b - a) * (double(i) / n);
            steps.push_back(baseSample(w, m, motion.inverse(q), mk.data()).height - h.heightAt(q.x, q.y));
        }
    }
    PasteLevel lv;
    if (steps.empty())
        return lv;
    std::nth_element(steps.begin(), steps.begin() + steps.size() / 2, steps.end());
    lv.offset = steps[steps.size() / 2];
    for (double s : steps)
        lv.residual = std::max(lv.residual, std::abs(s - lv.offset));
    // A quarter of the region at most: past that the paste would be mostly seam.
    lv.band = std::min(lv.residual / (w.spec.seams.maxSlope > 0.0 ? w.spec.seams.maxSlope : kSeamGrade), 0.25 * extent);
    return lv;
}

/** Push-pull: unknown samples (known 0) filled smoothly from the known ones, then relaxed. */
void fillHoles(std::vector<float>& v, const std::vector<uint8_t>& hole, int nx, int nz)
{
    struct Level
    {
        int nx, nz;
        std::vector<double> sum, weight;
    };
    std::vector<Level> levels;
    levels.push_back({nx, nz, std::vector<double>(size_t(nx) * nz), std::vector<double>(size_t(nx) * nz)});
    for (size_t i = 0; i < v.size(); ++i)
        if (!hole[i])
        {
            levels[0].sum[i] = v[i];
            levels[0].weight[i] = 1.0;
        }
    while (levels.back().nx > 2 || levels.back().nz > 2)
    {
        const Level& f = levels.back();
        Level c{(f.nx + 1) / 2, (f.nz + 1) / 2, {}, {}};
        c.sum.assign(size_t(c.nx) * c.nz, 0.0);
        c.weight.assign(size_t(c.nx) * c.nz, 0.0);
        for (int z = 0; z < f.nz; ++z)
            for (int x = 0; x < f.nx; ++x)
            {
                const size_t fi = size_t(z) * f.nx + x, ci = size_t(z / 2) * c.nx + x / 2;
                c.sum[ci] += f.sum[fi];
                c.weight[ci] += f.weight[fi];
            }
        levels.push_back(std::move(c));
    }
    for (Level& l : levels)
        for (size_t i = 0; i < l.sum.size(); ++i)
            if (l.weight[i] > 0.0)
            {
                l.sum[i] /= l.weight[i];
                l.weight[i] = std::min(1.0, l.weight[i]);
            }
    for (size_t k = levels.size() - 1; k-- > 0;)
    {
        Level& f = levels[k];
        const Level& c = levels[k + 1];
        for (int z = 0; z < f.nz; ++z)
            for (int x = 0; x < f.nx; ++x)
            {
                const size_t i = size_t(z) * f.nx + x;
                if (f.weight[i] >= 1.0)
                    continue;
                const double gx = std::clamp((x - 0.5) * 0.5, 0.0, double(c.nx - 1)), gz = std::clamp((z - 0.5) * 0.5, 0.0, double(c.nz - 1));
                const int x0 = std::min(int(gx), std::max(0, c.nx - 2)), z0 = std::min(int(gz), std::max(0, c.nz - 2));
                const int x1 = std::min(x0 + 1, c.nx - 1), z1 = std::min(z0 + 1, c.nz - 1);
                const double fx = gx - x0, fz = gz - z0;
                const auto at = [&](int xx, int zz) { return c.sum[size_t(zz) * c.nx + xx]; };
                const double up = (at(x0, z0) * (1 - fx) + at(x1, z0) * fx) * (1 - fz) + (at(x0, z1) * (1 - fx) + at(x1, z1) * fx) * fz;
                f.sum[i] = f.sum[i] * f.weight[i] + up * (1.0 - f.weight[i]);
                f.weight[i] = 1.0;
            }
    }
    for (size_t i = 0; i < v.size(); ++i)
        if (hole[i])
            v[i] = float(levels[0].sum[i]);
    // Relaxed toward a membrane so the fill meets its rim without a crease.
    std::vector<float> next = v;
    for (int it = 0; it < 40; ++it)
    {
        tbb::parallel_for(1, nz - 1, [&](int z) {
            for (int x = 1; x < nx - 1; ++x)
            {
                const size_t i = size_t(z) * nx + x;
                if (hole[i])
                    next[i] = 0.25f * (v[i - 1] + v[i + 1] + v[i - nx] + v[i + nx]);
            }
        });
        std::swap(v, next);
        for (size_t i = 0; i < v.size(); ++i)
            if (!hole[i])
                next[i] = v[i];
    }
}

/** Code masks into holes: each hole sample takes its nearest known sample's code. */
void fillNearest(std::vector<float>& v, const std::vector<uint8_t>& hole, int nx, int nz)
{
    std::vector<uint8_t> done(v.size());
    std::deque<size_t> q;
    for (size_t i = 0; i < v.size(); ++i)
        if (!hole[i])
        {
            done[i] = 1;
            q.push_back(i);
        }
    while (!q.empty())
    {
        const size_t i = q.front();
        q.pop_front();
        const int x = int(i % nx), z = int(i / nx);
        for (auto [dx, dz] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}})
        {
            const int xx = x + dx, zz = z + dz;
            if (xx < 0 || zz < 0 || xx >= nx || zz >= nz)
                continue;
            const size_t j = size_t(zz) * nx + xx;
            if (!done[j])
            {
                done[j] = 1;
                v[j] = v[i];
                q.push_back(j);
            }
        }
    }
}

/** Chamfer distance in cells from the nearest set sample (0 on them). */
std::vector<float> distanceFrom(const std::vector<uint8_t>& set, int nx, int nz)
{
    const float big = 1e9f, a = 1.0f, d = 1.41421356f;
    std::vector<float> dist(set.size(), big);
    for (size_t i = 0; i < set.size(); ++i)
        if (set[i])
            dist[i] = 0.0f;
    auto relax = [&](int x, int z, int dx, int dz, float c) {
        const int xx = x + dx, zz = z + dz;
        if (xx < 0 || zz < 0 || xx >= nx || zz >= nz)
            return;
        float& t = dist[size_t(z) * nx + x];
        t = std::min(t, dist[size_t(zz) * nx + xx] + c);
    };
    for (int z = 0; z < nz; ++z)
        for (int x = 0; x < nx; ++x)
        {
            relax(x, z, -1, 0, a), relax(x, z, 0, -1, a), relax(x, z, -1, -1, d), relax(x, z, 1, -1, d);
        }
    for (int z = nz - 1; z >= 0; --z)
        for (int x = nx - 1; x >= 0; --x)
        {
            relax(x, z, 1, 0, a), relax(x, z, 0, 1, a), relax(x, z, 1, 1, d), relax(x, z, -1, 1, d);
        }
    return dist;
}

/**
 * Seam bands get detail and Terrain's erosion re-run there, held outside: heights outside the bands are kept exactly.
 * Returns the erosion steps run.
 */
int erodeSeams(const RemixSpec& spec, Heightfield& h)
{
    LILA_ZONE();

    const std::vector<float>* seam = h.findMask("remixSeam");
    // The map's own erosion pass covers the seams too.
    const int steps = spec.erosion.age > 0.0 ? 0 : spec.seams.erosion;
    if (!seam || (steps <= 0 && spec.seams.detail <= 0.0))
        return 0;
    int x0 = h.nx, z0 = h.nz, x1 = -1, z1 = -1;
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
            if ((*seam)[h.index(x, z)] > 0.0f)
            {
                x0 = std::min(x0, x), x1 = std::max(x1, x);
                z0 = std::min(z0, z), z1 = std::max(z1, z);
            }
    if (x1 < 0)
        return 0;
    const int margin = 4;
    x0 = std::max(0, x0 - margin), z0 = std::max(0, z0 - margin), x1 = std::min(h.nx - 1, x1 + margin), z1 = std::min(h.nz - 1, z1 + margin);
    if (x1 - x0 < 2 || z1 - z0 < 2)
        return 0;
    Heightfield c;
    c.nx = x1 - x0 + 1;
    c.nz = z1 - z0 + 1;
    c.cell = h.cell;
    c.origin = h.samplePos(x0, z0);
    c.heights.resize(c.size());
    std::vector<float>& hold = c.mask("remixHold");
    std::vector<float>& holdHeight = c.mask("remixHoldHeight");
    std::vector<float> s(c.size());
    for (int z = 0; z < c.nz; ++z)
        for (int x = 0; x < c.nx; ++x)
        {
            const size_t i = c.index(x, z), j = h.index(x + x0, z + z0);
            s[i] = (*seam)[j];
            c.heights[i] = h.heights[j];
            hold[i] = 1.0f - s[i];
            holdHeight[i] = h.heights[j];
        }
    if (spec.seams.detail > 0.0)
    {
        // Gullies running down the slope, so the detail drains with the land instead of standing on it as bumps.
        terrain::ErosionFilterParams fp;
        fp.scale = c.cell * 16.0;
        fp.strength = 0.4 * spec.seams.detail;
        fp.seed = spec.seams.seed + 101u;
        std::vector<float> offset(c.size(), 0.0f);
        tbb::parallel_for(1, c.nz - 1, [&](int z) {
            for (int x = 1; x < c.nx - 1; ++x)
            {
                const size_t i = c.index(x, z);
                if (s[i] <= 0.0f)
                    continue;
                const double gx = (c.heights[i + 1] - c.heights[i - 1]) / (2.0 * c.cell), gz = (c.heights[i + c.nx] - c.heights[i - c.nx]) / (2.0 * c.cell);
                const Vec2 q = c.samplePos(x, z);
                offset[i] = float(s[i] * terrain::erosionFilterOffset(fp, q.x, q.y, gx, gz));
            }
        });
        for (size_t i = 0; i < c.size(); ++i)
            c.heights[i] += offset[i];
    }
    if (steps > 0)
    {
        terrain::ErosionParams p;
        p.iterations = steps;
        p.constraintMask = "remixHold";
        p.constraintHeightMask = "remixHoldHeight";
        terrain::erode(c, p);
    }
    for (int z = 0; z < c.nz; ++z)
        for (int x = 0; x < c.nx; ++x)
        {
            const size_t i = c.index(x, z), j = h.index(x + x0, z + z0);
            h.heights[j] = float(h.heights[j] + (c.heights[i] - h.heights[j]) * s[i]);
        }
    return steps;
}

/**
 * The whole map eroded: erosion-filter gullies, then Terrain's Erode preset, on a copy of the heights; the change is laid
 * back by each sample's erosion share (remixErosion), and the sea (below 0) is held. Writes wear and deposit from the
 * change. Returns the preset's report.
 */
json erodeMap(const RemixSpec& spec, Heightfield& h)
{
    LILA_ZONE();

    const RemixSpec::Erosion& e = spec.erosion;
    const std::vector<float>* share = h.findMask("remixErosion");
    if (!e.on() || !share)
        return json();
    Heightfield c;
    c.nx = h.nx, c.nz = h.nz, c.cell = h.cell, c.origin = h.origin;
    c.heights = h.heights;
    json report = json::object();
    if (e.detail > 0.0)
    {
        terrain::ErosionFilterParams fp;
        fp.scale = e.detailScale;
        fp.strength = e.detail;
        fp.seed = e.seed + 7u;
        terrain::erosionFilter(c, fp);
        c.masks.clear();
    }
    if (e.age > 0.0)
    {
        terrain::PresetParams p;
        p.preset = e.preset;
        p.age = e.age;
        p.seed = e.seed;
        p.keepForm = true;
        p.formRadius = e.form;
        p.gpu = terrain::gpuErosionAvailable();
        terrain::PresetReport r;
        terrain::erodePreset(c, p, &r);
        report = r.toJson();
    }
    std::vector<float> lost(h.size()), gained(h.size());
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
        {
            const size_t i = h.index(x, z);
            // Land only, fading in over the first 2 m above the sea, so the shore and the floor stay where the remix laid them.
            const double land = smooth01(std::min(h.heights[i], c.heights[i]) / 200.0);
            const float change = float((c.heights[i] - h.heights[i]) * (*share)[i] * land);
            h.heights[i] += change;
            lost[i] = std::max(0.0f, -change);
            gained[i] = std::max(0.0f, change);
        }
    });
    auto normalised = [](std::vector<float>& v) {
        float top = 0.0f;
        for (float a : v)
            top = std::max(top, a);
        if (top > 0.0f)
            for (float& a : v)
                a /= top;
    };
    normalised(lost);
    normalised(gained);
    h.masks["wear"] = std::move(lost);
    h.masks["deposit"] = std::move(gained);
    return report;
}

/**
 * builtUp presence on a source's grid: its built-up land and its water (a harbour, the sea off the front, a river through
 * town), gaps narrower than twice close (source cm) filled and holes shut (islands off the front), reaching margin past
 * it; as a signed distance into it in source cm (negative outside).
 */
std::vector<float> builtUpInside(const SourceData& d, double close, double margin)
{
    const Heightfield& hf = d.hf;
    const std::vector<float>* urban = hf.findMask("landcover_urban");
    const std::vector<float>* water = hf.findMask("landcover_water");
    std::vector<uint8_t> built(hf.size(), 0);
    for (size_t i = 0; i < hf.size(); ++i)
        built[i] = (urban && (*urban)[i] >= 0.5f) || (water && (*water)[i] >= 0.5f) || (!d.flatSea.empty() && d.flatSea[i] >= 0.5f);
    const std::vector<float> out = distanceFrom(built, hf.nx, hf.nz);
    std::vector<uint8_t> grown(hf.size());
    for (size_t i = 0; i < hf.size(); ++i)
        grown[i] = out[i] * hf.cell <= close + margin;
    // Holes inside the town (parks, a river bend) are the town's: only open land reached from the grid's edge stays out.
    std::vector<uint8_t> open(hf.size(), 0);
    std::deque<size_t> q;
    auto seed = [&](int x, int z) {
        const size_t i = hf.index(x, z);
        if (!grown[i] && !open[i])
            open[i] = 1, q.push_back(i);
    };
    for (int x = 0; x < hf.nx; ++x)
        seed(x, 0), seed(x, hf.nz - 1);
    for (int z = 0; z < hf.nz; ++z)
        seed(0, z), seed(hf.nx - 1, z);
    while (!q.empty())
    {
        const size_t i = q.front();
        q.pop_front();
        const int x = int(i % size_t(hf.nx)), z = int(i / size_t(hf.nx));
        if (x > 0)
            seed(x - 1, z);
        if (x + 1 < hf.nx)
            seed(x + 1, z);
        if (z > 0)
            seed(x, z - 1);
        if (z + 1 < hf.nz)
            seed(x, z + 1);
    }
    const std::vector<float> into = distanceFrom(open, hf.nx, hf.nz);
    std::vector<uint8_t> shut(hf.size());
    for (size_t i = 0; i < hf.size(); ++i)
        shut[i] = !open[i];
    const std::vector<float> outOf = distanceFrom(shut, hf.nx, hf.nz);
    std::vector<float> inside(hf.size());
    for (size_t i = 0; i < hf.size(); ++i)
        inside[i] = float((open[i] ? -outOf[i] : into[i]) * hf.cell - close);
    return inside;
}
} // namespace

namespace detail
{
void prepareSources(Work& w, Vec2 lo, Vec2 hi)
{
    LILA_ZONE();

    const RemixSpec& spec = w.spec;
    const bool mapGiven = spec.map.size.x > 0.0 && spec.map.size.y > 0.0;
    w.edges.assign(w.data.size(), SourceEdge());
    for (size_t s = 0; s < w.data.size(); ++s)
    {
        const Source& src = spec.sources[s];
        SourceEdge& E = w.edges[s];
        E.seam = src.seam > 0.0 ? src.seam : 0.0;
        E.feather = E.seam > 0.0 ? std::min(w.feather, E.seam) : w.feather;
        E.noise = src.seamNoise >= 0.0 ? src.seamNoise : E.seam;
        E.vertical = src.vertical > 0.0 ? src.vertical : spec.vertical;
        E.erosion = src.erosion;
        if (!w.data[s])
            continue;
        const Heightfield& hf = w.data[s]->hf;
        if (mapGiven)
        {
            // An edge the map ends inside of never shows, so it keeps the land full to the map's border.
            const Vec2 e = hf.extent();
            for (int k = 0; k < 4; ++k)
                E.open[k] = false;
            for (Vec2 c : {lo, Vec2(hi.x, lo.y), hi, Vec2(lo.x, hi.y)})
            {
                const Vec2 l = baseToSource(src, spec.horizontal, c) - hf.origin;
                E.open[0] = E.open[0] || l.x < 0.0;
                E.open[1] = E.open[1] || l.y < 0.0;
                E.open[2] = E.open[2] || l.x > e.x;
                E.open[3] = E.open[3] || l.y > e.y;
            }
        }
        if (src.extent == "builtUp")
        {
            const double k = w.scales[s];
            const double close = 0.5 * std::max(E.seam, E.feather) / k;
            std::vector<float> inside = builtUpInside(*w.data[s], close, src.margin / k);
            for (float& v : inside)
                v = float(v * k);
            E.inside = std::make_shared<const std::vector<float>>(std::move(inside));
            if (!hf.findMask("landcover_urban") && w.out)
                w.out->notes.push_back("source " + src.id + ": extent builtUp but no built-up land cover; the source is left out");
        }
    }
}
} // namespace detail

/**
 * Sources placed finer than the map, low-passed to the map's cell before they are sampled: a point sample of a 5 m grid
 * every 9 m keeps every other ridge crest and gully as spikes, and every other land cover pixel as steps.
 */
void prefilterSources(Work& w, double cell, RemixResult& out)
{
    LILA_ZONE();

    for (size_t s = 0; s < w.data.size(); ++s)
    {
        if (!w.data[s])
            continue;
        const Heightfield& hf = w.data[s]->hf;
        const double ratio = cell / (hf.cell * w.scales[s]);
        if (ratio <= 1.3)
            continue;
        const int r = std::max(1, int(std::ceil((ratio - 1.0) * 0.5)));
        auto d = std::make_shared<SourceData>(*w.data[s]);
        d->hf.heights = terrain::boxBlurred(hf.heights, hf.nx, hf.nz, r, 1);
        for (auto& [name, v] : d->hf.masks)
            if (name != kCodeMask && v.size() == hf.size())
                v = terrain::boxBlurred(v, hf.nx, hf.nz, r, 1);
        if (!d->flatSea.empty())
            d->flatSea = terrain::boxBlurred(d->flatSea, hf.nx, hf.nz, r, 1);
        w.data[s] = d;
        out.notes.push_back("source " + w.spec.sources[s].id + ": " + std::format("{:.1f}", hf.cell * w.scales[s] * 0.01) + " m on the map, filtered to the " +
                            std::format("{:.1f}", cell * 0.01) + " m cell");
    }
}

/**
 * Sea floor depths carried smoothly from the grids into the part of the map no grid reaches, so the floor has no step
 * where a grid's box ends. Only depths are carried: grid land would raise the open sea.
 */
void extendGrids(terrain::Dem& g, RemixResult& out)
{
    std::vector<uint8_t> hole(g.metres.size());
    size_t depths = 0, outside = 0;
    for (size_t i = 0; i < g.metres.size(); ++i)
    {
        hole[i] = !(g.metres[i] < 0.0f);
        depths += !hole[i];
        outside += std::isnan(g.metres[i]);
    }
    if (depths == 0 || outside == 0)
        return;
    std::vector<float> carried = g.metres;
    fillHoles(carried, hole, g.width, g.height);
    for (size_t i = 0; i < g.metres.size(); ++i)
        if (std::isnan(g.metres[i]))
            g.metres[i] = std::min(carried[i], 0.0f);
    out.notes.push_back("bathymetry: depths carried from the grids over the " + std::to_string(int(std::lround(100.0 * double(outside) / double(g.metres.size())))) +
                        "% of the map no grid reaches");
}

/**
 * The sea floor on the base map (Terrain's bathymetry): land is what the sources make above sea level, sea the rest; the
 * sources' bathymetry grids, resampled through their placement, are blended in from the shelf profile. Into w.seaFloor.
 */
void layBathymetry(Work& w, RemixResult& out, const Heightfield& grid)
{
    LILA_ZONE();

    if (!w.spec.bathymetryOn())
    {
        out.stats["bathymetry"] = {{"source", "flat"}, {"seaDepth", w.spec.map.seaDepth}};
        out.notes.push_back("bathymetry off: the sea floor is flat at " + std::to_string(int(std::round(w.spec.map.seaDepth))) + " cm where no source covers");
        return;
    }
    Heightfield sf;
    sf.nx = grid.nx, sf.nz = grid.nz, sf.cell = grid.cell, sf.origin = grid.origin;
    sf.heights.assign(sf.size(), 0.0f);
    bool anyGrid = false;
    for (const auto& d : w.data)
        anyGrid = anyGrid || (d && d->bathymetry);
    terrain::Dem g;
    if (anyGrid)
    {
        g.width = sf.nx, g.height = sf.nz, g.geographic = true, g.epsg = 4326;
        const LatLon nw = out.geo.toGeographic(sf.origin);
        g.x0 = nw.lon, g.y0 = nw.lat;
        g.dx = sf.cell * 0.01 / out.geo.mx, g.dy = sf.cell * 0.01 / out.geo.my;
        g.metres.assign(sf.size(), std::numeric_limits<float>::quiet_NaN());
    }
    tbb::parallel_for(0, sf.nz, [&](int z) {
        for (int x = 0; x < sf.nx; ++x)
        {
            const Vec2 b = sf.samplePos(x, z);
            const size_t i = sf.index(x, z);
            double height = 0.0;
            std::vector<double> W(w.data.size());
            w.weights(b, W.data());
            for (size_t s = 0; s < w.data.size(); ++s)
            {
                const double wt = W[s];
                if (wt <= 0.0)
                    continue;
                const Heightfield& h = w.data[s]->hf;
                const Vec2 l = baseToSource(w.spec.sources[s], w.spec.horizontal, b);
                double hs = h.heightAt(l.x, l.y) * w.vertical(s);
                if (!w.data[s]->flatSea.empty())
                    hs *= 1.0 - bilinear(w.data[s]->flatSea, h, l);
                height += (hs - height) * wt;
            }
            sf.heights[i] = float(height);
            if (!anyGrid)
                continue;
            // Later sources blend over earlier ones across their feather, as their elevation does.
            double depth = 0.0;
            bool have = false;
            for (size_t s = 0; s < w.data.size(); ++s)
                if (w.data[s] && w.data[s]->bathymetry)
                {
                    const Vec2 l = baseToSource(w.spec.sources[s], w.spec.horizontal, b);
                    const double v = gridHeight(*w.data[s]->bathymetry, w.data[s]->geo.toGeographic(l));
                    if (std::isnan(v))
                        continue;
                    depth = have ? depth + (v * w.vertical(s) - depth) * W[s] : v * w.vertical(s);
                    have = true;
                }
            if (have)
                g.metres[i] = float(depth);
        }
    });
    if (anyGrid)
        extendGrids(g, out);
    terrain::BathymetrySpec bs = terrain::BathymetrySpec::fromJson(w.spec.bathymetry);
    bs.file.clear();
    bs.seaLevel = 0.0;
    const terrain::BathymetryResult r = terrain::applyBathymetry(sf, bs, anyGrid ? &g : nullptr, anyGrid ? &out.geo : nullptr);
    out.stats["bathymetry"] = {{"source", r.source},      {"seaSamples", r.seaSamples}, {"gridSamples", r.gridSamples},
                               {"maxDepth", r.maxDepth}, {"meanDepth", r.meanDepth},   {"grids", anyGrid}};
    if (!anyGrid && r.seaSamples > 0)
        out.notes.push_back("bathymetry: no source has a bathymetry grid; the sea floor is the shelf profile");
    for (const std::string& n : r.notes)
        out.notes.push_back("bathymetry: " + n);
    w.seaFloor = std::move(sf);
}

// ---------------------------------------------------------------------------------------------------- run

std::shared_ptr<RemixResult> runRemix(const RemixSpec& spec, const RunOptions& o)
{
    LILA_ZONE();

    const auto t0 = std::chrono::steady_clock::now();
    auto ms = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
    if (spec.sources.empty())
        throw std::runtime_error("the remix has no sources (remixSource adds one)");
    Work w(spec);
    w.out = std::make_shared<RemixResult>();
    RemixResult& out = *w.out;
    json timing = json::object();

    // Sources.
    double finest = 1e300;
    for (const Source& s : spec.sources)
    {
        std::shared_ptr<const SourceData> d;
        try
        {
            d = loadSource(s, spec.bathymetry.value("landTolerance", 50.0));
        }
        catch (const std::exception& e)
        {
            out.errors.push_back(e.what());
            lila::log::warn("remix: {}", e.what());
        }
        w.data.push_back(d);
        const double k = s.scale > 0.0 ? s.scale : spec.horizontal;
        w.scales.push_back(k);
        w.footprints.push_back(d ? footprintOf(s, spec.horizontal, d->hf) : Polygon{});
        if (d)
        {
            for (const std::string& n : d->notes)
                out.notes.push_back(n);
            finest = std::min(finest, d->hf.cell * k);
        }
    }
    if (finest >= 1e300)
        throw std::runtime_error("no source could be read: " + (out.errors.empty() ? std::string() : out.errors.front()));
    timing["sources"] = ms();

    // The output grid.
    Vec2 lo(1e300), hi(-1e300);
    auto grow = [&](Vec2 p) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    };
    for (size_t r = 0; r < spec.regions.size(); ++r)
        w.targets.push_back(spec.regions[r].target());
    if (spec.map.size.x > 0.0 && spec.map.size.y > 0.0)
    {
        lo = spec.map.center - spec.map.size * 0.5;
        hi = spec.map.center + spec.map.size * 0.5;
    }
    else
    {
        for (const Polygon& f : w.footprints)
            for (Vec2 p : f)
                grow(p);
        for (const Polygon& t : w.targets)
            for (Vec2 p : t)
                grow(p);
        for (const Pin& p : spec.pins)
            grow(p.to);
    }
    double cell = spec.map.cell > 0.0 ? spec.map.cell : finest;
    const Vec2 size = hi - lo;
    if (!(size.x > 0.0 && size.y > 0.0))
        throw std::runtime_error("the remix covers no area");
    if (std::max(size.x, size.y) / cell + 1.0 > spec.map.maxResolution)
    {
        const double was = cell;
        cell = std::max(size.x, size.y) / double(spec.map.maxResolution - 1);
        out.notes.push_back("cell " + std::to_string(int(std::round(was))) + " cm would pass map.maxResolution; using " + std::to_string(int(std::round(cell))) + " cm");
    }
    Heightfield& h = out.after;
    h.cell = cell;
    h.nx = std::max(2, int(std::ceil(size.x / cell - 1e-6)) + 1);
    h.nz = std::max(2, int(std::ceil(size.y / cell - 1e-6)) + 1);
    const Vec2 centre = (lo + hi) * 0.5;
    h.origin = centre - h.extent() * 0.5;
    out.lo = h.origin;
    out.hi = h.origin + h.extent();
    // Sources fade in over a tenth of the smallest one (at least a seam width), so a source's edge meets the sea or its
    // neighbour as a slope rather than a cliff.
    w.feather = 1e300;
    for (size_t s = 0; s < w.data.size(); ++s)
        if (w.data[s])
        {
            const Vec2 e = w.data[s]->hf.extent() * w.scales[s];
            w.feather = std::min(w.feather, std::min(std::max(spec.seams.width, 0.1 * std::min(e.x, e.y)), 0.25 * std::min(e.x, e.y)));
        }
    w.feather = std::max(w.feather, cell);
    prefilterSources(w, cell, out);
    prepareSources(w, out.lo, out.hi);

    // Output georeference: the first readable source's centre (or the spec's origin) at map (0, 0).
    glm::dvec2 ll0(0.0);
    if (spec.origin)
        ll0 = *spec.origin;
    else
        for (size_t s = 0; s < w.data.size(); ++s)
            if (w.data[s])
            {
                const terrain::Heightfield& sh = w.data[s]->hf;
                const LatLon c = w.data[s]->geo.toGeographic(sh.origin + sh.extent() * 0.5);
                ll0 = {c.lat, c.lon};
                // The source's centre sits at its placement, not at (0, 0); shift so (0, 0) is where that latitude lands.
                const terrain::Georef g = localGeoref(ll0, spec.sources[s].at);
                const LatLon at0 = g.toGeographic(Vec2(0.0));
                ll0 = {at0.lat, at0.lon};
                break;
            }
    const LatLon llc = localGeoref(ll0, Vec2(0.0)).toGeographic(h.origin + h.extent() * 0.5);
    out.originLatLon = {llc.lat, llc.lon};
    out.geo = localGeoref(out.originLatLon, h.origin + h.extent() * 0.5);

    // The warp.
    std::vector<Warp::Handle> handles, rigid;
    for (const Region& r : spec.regions)
        if (warps(r.op))
        {
            handles.push_back({r.polygon, r.rigid()});
            rigid.push_back({r.target(), r.rigid()});
        }
    const double falloff = spec.warp.falloff > 0.0 ? spec.warp.falloff : 0.25 * std::max(size.x, size.y);
    out.footprints = w.footprints;
    out.feather = w.feather;
    w.warp = std::make_shared<Warp>(handles, spec.pins, falloff, spec.warp.maxSteps, out.lo, out.hi);
    out.warp = w.warp;
    w.inverse = InverseGrid(*w.warp, out.lo, out.hi, std::min(std::max(h.nx, h.nz), 257), rigid);
    timing["warp"] = ms();

    layBathymetry(w, out, h);
    timing["bathymetry"] = ms();

    // Background and base map.
    const MaskSet m = masksOf(w);
    const size_t nm = m.names.size();
    h.heights.assign(h.size(), 0.0f);
    std::vector<std::vector<float>> masks(nm, std::vector<float>(h.size(), 0.0f));
    Heightfield& before = out.before;
    before.nx = h.nx, before.nz = h.nz, before.cell = h.cell, before.origin = h.origin;
    before.heights.assign(h.size(), 0.0f);
    std::vector<std::vector<float>> beforeMasks(nm, std::vector<float>(h.size(), 0.0f));
    std::vector<uint8_t> hole(h.size(), 0);
    std::vector<float> seam(h.size(), 0.0f);
    std::vector<float> erosionShare(h.size(), 0.0f);
    tbb::parallel_for(0, h.nz, [&](int z) {
        std::vector<float> mk(nm);
        for (int x = 0; x < h.nx; ++x)
        {
            const size_t i = h.index(x, z);
            const Vec2 q = h.samplePos(x, z);
            const Sample sb = baseSample(w, m, q, mk.data());
            before.heights[i] = float(sb.height);
            for (size_t k = 0; k < nm; ++k)
                beforeMasks[k][i] = mk[k];
            const Vec2 b = w.inverse(q);
            if (w.holed(b))
            {
                hole[i] = 1;
                continue;
            }
            const Sample s = sb.covered && b == q ? sb : baseSample(w, m, b, mk.data());
            h.heights[i] = float(s.height);
            seam[i] = float(s.seam);
            erosionShare[i] = float(s.erosion);
            for (size_t k = 0; k < nm; ++k)
                masks[k][i] = mk[k];
        }
    });
    timing["background"] = ms();

    // Holes where regions were cut out or deleted: filled as land and made seams.
    size_t holes = 0;
    for (uint8_t v : hole)
        holes += v;
    if (holes)
    {
        fillHoles(h.heights, hole, h.nx, h.nz);
        fillHoles(erosionShare, hole, h.nx, h.nz);
        for (size_t k = 0; k < nm; ++k)
            if (m.code[k])
                fillNearest(masks[k], hole, h.nx, h.nz);
            else
                fillHoles(masks[k], hole, h.nx, h.nz);
        const std::vector<float> d = distanceFrom(hole, h.nx, h.nz);
        const double band = std::max(1.0, spec.seams.width / h.cell);
        for (size_t i = 0; i < h.size(); ++i)
            seam[i] = std::max(seam[i], float(1.0 - std::min(1.0, d[i] / band)));
    }
    timing["holes"] = ms();

    // Pasted regions, in order, blended in over their seam band.
    size_t pasted = 0;
    json regionLevels = json::array();
    for (size_t r = 0; r < spec.regions.size(); ++r)
    {
        const Region& reg = spec.regions[r];
        if (!pastes(reg.op))
            continue;
        const Polygon& t = w.targets[r];
        const Rigid motion = reg.rigid();
        double band = std::max(spec.seamOf(reg), h.cell);
        Vec2 tlo(1e300), thi(-1e300);
        for (Vec2 p : t)
            tlo = glm::min(tlo, p), thi = glm::max(thi, p);
        double offset = 0.0;
        if (reg.matchHeight)
        {
            const PasteLevel lv = pasteLevel(w, m, h, t, motion, std::min(thi.x - tlo.x, thi.y - tlo.y));
            offset = lv.offset;
            band = std::max(band, lv.band);
            regionLevels.push_back({{"region", reg.id}, {"offset", std::round(offset)}, {"seam", std::round(band)}, {"stepLeft", std::round(lv.residual)}});
        }
        tlo -= Vec2(band), thi += Vec2(band);
        const int xa = std::max(0, int(std::floor((tlo.x - h.origin.x) / h.cell))), xb = std::min(h.nx - 1, int(std::ceil((thi.x - h.origin.x) / h.cell)));
        const int za = std::max(0, int(std::floor((tlo.y - h.origin.y) / h.cell))), zb = std::min(h.nz - 1, int(std::ceil((thi.y - h.origin.y) / h.cell)));
        tbb::parallel_for(za, zb + 1, [&](int z) {
            std::vector<float> mk(nm);
            for (int x = xa; x <= xb; ++x)
            {
                const size_t i = h.index(x, z);
                const Vec2 q = h.samplePos(x, z);
                const double sd = signedDistance(t, q);
                if (std::abs(sd) < band)
                    seam[i] = std::max(seam[i], float(1.0 - std::abs(sd) / band));
                if (sd <= 0.0)
                    continue;
                const double wt = smooth01(sd / band);
                const Sample s = baseSample(w, m, motion.inverse(q), mk.data());
                h.heights[i] = float(h.heights[i] + (s.height - offset - h.heights[i]) * wt);
                erosionShare[i] = float(erosionShare[i] + (s.erosion - erosionShare[i]) * wt);
                for (size_t k = 0; k < nm; ++k)
                    masks[k][i] = m.code[k] ? (wt >= 0.5 ? mk[k] : masks[k][i]) : float(masks[k][i] + (mk[k] - masks[k][i]) * wt);
            }
        });
        ++pasted;
    }
    timing["paste"] = ms();
    markSeaSeams(w, h, seam);
    if (!regionLevels.empty())
        out.stats["regionLevels"] = regionLevels;

    for (size_t k = 0; k < nm; ++k)
    {
        h.masks[m.names[k]] = std::move(masks[k]);
        before.masks[m.names[k]] = std::move(beforeMasks[k]);
    }
    h.masks["remixSeam"] = seam;
    // Where each builtUp source is, for settlements that keep to it (worldBuild's within).
    for (size_t s = 0; s < w.data.size(); ++s)
        if (w.data[s] && spec.sources[s].extent == "builtUp")
        {
            std::vector<float>& pm = h.mask("remixPresent_" + spec.sources[s].id);
            tbb::parallel_for(0, h.nz, [&](int z) {
                std::vector<double> W(w.data.size());
                for (int x = 0; x < h.nx; ++x)
                {
                    w.weights(w.inverse(h.samplePos(x, z)), W.data());
                    pm[h.index(x, z)] = float(W[s]);
                }
            });
        }
    std::vector<float>& holeMask = h.mask("remixHole");
    for (size_t i = 0; i < h.size(); ++i)
        holeMask[i] = hole[i] ? 1.0f : 0.0f;
    const int eroded = erodeSeams(spec, h);
    timing["seams"] = ms();
    if (spec.erosion.on())
    {
        h.masks["remixErosion"] = std::move(erosionShare);
        out.stats["erosion"] = erodeMap(spec, h);
        timing["erosion"] = ms();
    }

    // Features and their repairs.
    Features f;
    if (o.features)
    {
        moveFeatures(w, f);
        timing["features"] = ms();
        if (spec.seams.roads)
            reconnectRoads(w, f);
        timing["roads"] = ms();
        if (spec.seams.rivers)
            repairRivers(w, f);
        timing["rivers"] = ms();
        if (spec.seams.coast)
            mergeCoast(w, f);
        timing["coast"] = ms();
        assessRivers(w, f);
    }
    finishFeatures(w, f);
    timing["total"] = ms();

    size_t seamSamples = 0;
    for (float v : seam)
        seamSamples += v > 0.0f;
    const glm::dvec2 range = h.range();
    out.stats["grid"] = {{"resolution", {h.nx, h.nz}}, {"cell", h.cell}, {"min", {out.lo.x, out.lo.y}}, {"max", {out.hi.x, out.hi.y}}, {"heights", {range.x, range.y}}};
    out.stats["sources"] = spec.sources.size() - out.errors.size();
    out.stats["regions"] = spec.regions.size();
    out.stats["pasted"] = pasted;
    out.stats["pins"] = spec.pins.size();
    out.stats["warp"] = {{"steps", w.warp->steps()}, {"minStepJacobian", w.warp->minStepJacobian()}, {"falloff", falloff}};
    out.stats["holeSamples"] = holes;
    out.stats["seamSamples"] = seamSamples;
    out.stats["seamErosion"] = eroded;
    out.stats["masks"] = m.names;
    out.stats["origin"] = {out.originLatLon.x, out.originLatLon.y};
    out.stats["ms"] = timing;
    return w.out;
}

std::shared_ptr<const RemixResult> cachedRemix(const RemixSpec& spec)
{
    static std::mutex lock;
    static std::list<std::pair<std::string, std::shared_ptr<const RemixResult>>> cache;
    std::string key = toJson(spec).dump();
    for (const Source& s : spec.sources)
        key += "|" + std::to_string(stampOf(s.dem)) + "|" + std::to_string(stampOf(s.osm)) + "|" + std::to_string(stampOf(s.landcover)) + "|" +
               std::to_string(stampOf(s.bathymetry));
    {
        std::lock_guard l(lock);
        for (auto it = cache.begin(); it != cache.end(); ++it)
            if (it->first == key)
            {
                cache.splice(cache.begin(), cache, it);
                return cache.front().second;
            }
    }
    std::shared_ptr<const RemixResult> r = runRemix(spec);
    std::lock_guard l(lock);
    cache.emplace_front(key, r);
    while (cache.size() > 4)
        cache.pop_back();
    return r;
}

// ---------------------------------------------------------------------------------------------------- contours

std::vector<std::vector<Vec2>> contourLines(const Heightfield& h, double level, Vec2 lo, Vec2 hi)
{
    LILA_ZONE();

    const int x0 = std::clamp(int(std::floor((lo.x - h.origin.x) / h.cell)), 0, h.nx - 1);
    const int x1 = std::clamp(int(std::ceil((hi.x - h.origin.x) / h.cell)), 0, h.nx - 1);
    const int z0 = std::clamp(int(std::floor((lo.y - h.origin.y) / h.cell)), 0, h.nz - 1);
    const int z1 = std::clamp(int(std::ceil((hi.y - h.origin.y) / h.cell)), 0, h.nz - 1);
    // A crossing lives on a grid edge: horizontal edges keyed 2 * index, vertical ones 2 * index + 1.
    auto val = [&](int x, int z) { return double(h.at(x, z)) - level; };
    std::unordered_map<int64_t, Vec2> point;
    std::unordered_map<int64_t, std::vector<int64_t>> link;
    auto crossing = [&](int xa, int za, int xb, int zb, int64_t key) {
        if (point.count(key))
            return;
        const double a = val(xa, za), b = val(xb, zb);
        const double t = a / (a - b);
        point[key] = h.samplePos(xa, za) + (h.samplePos(xb, zb) - h.samplePos(xa, za)) * t;
    };
    for (int z = z0; z < z1; ++z)
        for (int x = x0; x < x1; ++x)
        {
            const double c[4] = {val(x, z), val(x + 1, z), val(x + 1, z + 1), val(x, z + 1)};
            std::vector<int64_t> keys;
            const int64_t top = 2 * int64_t(h.index(x, z)), bottom = 2 * int64_t(h.index(x, z + 1));
            const int64_t left = 2 * int64_t(h.index(x, z)) + 1, right = 2 * int64_t(h.index(x + 1, z)) + 1;
            if ((c[0] > 0) != (c[1] > 0))
                crossing(x, z, x + 1, z, top), keys.push_back(top);
            if ((c[1] > 0) != (c[2] > 0))
                crossing(x + 1, z, x + 1, z + 1, right), keys.push_back(right);
            if ((c[3] > 0) != (c[2] > 0))
                crossing(x, z + 1, x + 1, z + 1, bottom), keys.push_back(bottom);
            if ((c[0] > 0) != (c[3] > 0))
                crossing(x, z, x, z + 1, left), keys.push_back(left);
            if (keys.size() == 2)
            {
                link[keys[0]].push_back(keys[1]);
                link[keys[1]].push_back(keys[0]);
            }
            else if (keys.size() == 4)
            {
                // Saddle: pair by the centre's side.
                const bool centre = (c[0] + c[1] + c[2] + c[3]) * 0.25 > 0;
                const bool pairTopRight = (c[0] > 0) != centre;
                const int64_t a = keys[0], b = keys[1], cc = keys[2], d = keys[3];
                if (pairTopRight)
                    link[a].push_back(b), link[b].push_back(a), link[cc].push_back(d), link[d].push_back(cc);
                else
                    link[a].push_back(d), link[d].push_back(a), link[b].push_back(cc), link[cc].push_back(b);
            }
        }
    std::vector<std::vector<Vec2>> lines;
    std::set<int64_t> used;
    auto walk = [&](int64_t start) {
        std::vector<Vec2> line{point[start]};
        used.insert(start);
        int64_t at = start;
        for (;;)
        {
            int64_t next = -1;
            for (int64_t n : link[at])
                if (!used.count(n))
                {
                    next = n;
                    break;
                }
            if (next < 0)
            {
                for (int64_t n : link[at])
                    if (n == start && line.size() > 2)
                        line.push_back(point[start]);
                break;
            }
            used.insert(next);
            line.push_back(point[next]);
            at = next;
        }
        return line;
    };
    for (const auto& [k, l] : link)
        if (l.size() == 1 && !used.count(k))
            lines.push_back(walk(k));
    for (const auto& [k, l] : link)
        if (!used.count(k))
            lines.push_back(walk(k));
    return lines;
}
} // namespace openworld
