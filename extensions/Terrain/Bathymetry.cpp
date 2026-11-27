// Bathymetry: a seabed laid under the flat sea a land DEM leaves, from a GEBCO grid blended in from a shoreline profile.

#include "Coast.h"

#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <deque>

namespace terrain
{
using json = nlohmann::json;

namespace bathy
{
double smooth01(double a, double b, double x)
{
    if (b <= a)
        return x < a ? 0.0 : 1.0;
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/** Felzenszwalb's 1D squared distance transform of f (0 at set samples, inf elsewhere) into d. */
void edt1(const std::vector<double>& f, std::vector<double>& d, std::vector<int>& v, std::vector<double>& z)
{
    const int n = int(f.size());
    int k = 0;
    v[0] = 0;
    z[0] = -1e300;
    z[1] = 1e300;
    int first = -1;
    for (int q = 0; q < n; ++q)
        if (std::isfinite(f[q]))
        {
            first = q;
            break;
        }
    if (first < 0)
    {
        std::fill(d.begin(), d.end(), std::numeric_limits<double>::infinity());
        return;
    }
    v[0] = first;
    for (int q = first + 1; q < n; ++q)
    {
        if (!std::isfinite(f[q]))
            continue;
        double s = ((f[q] + double(q) * q) - (f[v[k]] + double(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
        while (s <= z[k])
        {
            --k;
            s = ((f[q] + double(q) * q) - (f[v[k]] + double(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = 1e300;
    }
    k = 0;
    for (int q = 0; q < n; ++q)
    {
        while (z[k + 1] < q)
            ++k;
        const double dq = q - v[k];
        d[q] = dq * dq + f[v[k]];
    }
}
} // namespace bathy

std::vector<float> distanceToSet(const std::vector<uint8_t>& set, int nx, int nz, double cell)
{
    LILA_ZONE();

    const double inf = std::numeric_limits<double>::infinity();
    std::vector<double> g(size_t(nx) * size_t(nz), inf);
    tbb::parallel_for(0, nx, [&](int x) {
        std::vector<double> f(nz), d(nz), z(nz + 1);
        std::vector<int> v(nz);
        for (int r = 0; r < nz; ++r)
            f[size_t(r)] = set[size_t(r) * size_t(nx) + size_t(x)] ? 0.0 : inf;
        bathy::edt1(f, d, v, z);
        for (int r = 0; r < nz; ++r)
            g[size_t(r) * size_t(nx) + size_t(x)] = d[size_t(r)];
    });
    std::vector<float> out(g.size());
    tbb::parallel_for(0, nz, [&](int r) {
        std::vector<double> f(nx), d(nx), z(nx + 1);
        std::vector<int> v(nx);
        for (int c = 0; c < nx; ++c)
            f[size_t(c)] = g[size_t(r) * size_t(nx) + size_t(c)];
        bathy::edt1(f, d, v, z);
        for (int c = 0; c < nx; ++c)
            out[size_t(r) * size_t(nx) + size_t(c)] = std::isfinite(d[size_t(c)]) ? float(std::sqrt(d[size_t(c)]) * cell) : std::numeric_limits<float>::infinity();
    });
    return out;
}

std::vector<uint8_t> seaOf(const Heightfield& h, double level)
{
    std::vector<uint8_t> sea(h.size(), 0);
    if (const std::vector<float>* m = h.findMask("sea"))
    {
        bool any = false;
        for (size_t i = 0; i < m->size(); ++i)
            if ((*m)[i] > 0.5f)
                any = sea[i] = 1;
        if (any)
            return sea;
    }
    std::deque<uint32_t> open;
    auto push = [&](int x, int z) {
        const size_t i = h.index(x, z);
        if (!sea[i] && h.heights[i] < level)
        {
            sea[i] = 1;
            open.push_back(uint32_t(i));
        }
    };
    for (int x = 0; x < h.nx; ++x)
    {
        push(x, 0);
        push(x, h.nz - 1);
    }
    for (int z = 0; z < h.nz; ++z)
    {
        push(0, z);
        push(h.nx - 1, z);
    }
    while (!open.empty())
    {
        const uint32_t i = open.front();
        open.pop_front();
        const int x = int(i % uint32_t(h.nx)), z = int(i / uint32_t(h.nx));
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx)
                if ((dx || dz) && x + dx >= 0 && z + dz >= 0 && x + dx < h.nx && z + dz < h.nz)
                    push(x + dx, z + dz);
    }
    return sea;
}

// ---------------------------------------------------------------------------------------------------- spec

json BathymetrySpec::toJson() const
{
    return {{"file", file},           {"seaLevel", seaLevel},     {"landTolerance", landTolerance}, {"shoreSlope", shoreSlope},
            {"blendWidth", blendWidth}, {"smoothing", smoothing},   {"depthScale", depthScale},       {"shelfDepth", shelfDepth},
            {"shelfWidth", shelfWidth}, {"dropDepth", dropDepth},   {"slopeWidth", slopeWidth},       {"minDepth", minDepth}};
}

BathymetrySpec BathymetrySpec::fromJson(const json& j)
{
    BathymetrySpec s;
    if (!j.is_object())
        return s;
    s.file = j.value("file", s.file);
    s.seaLevel = j.value("seaLevel", s.seaLevel);
    s.landTolerance = std::max(0.0, j.value("landTolerance", s.landTolerance));
    s.shoreSlope = std::clamp(j.value("shoreSlope", s.shoreSlope), 0.05, 60.0);
    s.blendWidth = std::max(0.0, j.value("blendWidth", s.blendWidth));
    s.smoothing = std::max(0.0, j.value("smoothing", s.smoothing));
    s.depthScale = std::max(0.0, j.value("depthScale", s.depthScale));
    s.shelfDepth = std::max(1.0, j.value("shelfDepth", s.shelfDepth));
    s.shelfWidth = std::max(1.0, j.value("shelfWidth", s.shelfWidth));
    s.dropDepth = std::max(s.shelfDepth, j.value("dropDepth", s.dropDepth));
    s.slopeWidth = std::max(1.0, j.value("slopeWidth", s.slopeWidth));
    s.minDepth = std::max(0.0, j.value("minDepth", s.minDepth));
    return s;
}

uint64_t gebcoEstimateBytes(double west, double south, double east, double north)
{
    const double w = std::max(0.0, east - west) * kGebcoPerDegree + 1.0, h = std::max(0.0, north - south) * kGebcoPerDegree + 1.0;
    return uint64_t(w * h * 4.0);
}

const char* gebcoAttribution()
{
    return "Bathymetry: GEBCO Compilation Group, GEBCO Grid (General Bathymetric Chart of the Oceans, https://www.gebco.net).\n"
           "The GEBCO Grid is in the public domain and may be used free of charge; credit GEBCO as its source and do not suggest "
           "GEBCO endorses this work. Not to be used for navigation.";
}

// ---------------------------------------------------------------------------------------------------- blend

BathymetryResult applyBathymetry(Heightfield& h, const BathymetrySpec& s, const Dem* grid, const Georef* georef)
{
    LILA_ZONE();

    BathymetryResult r;
    if (h.empty())
        return r;
    const size_t n = h.size();
    const double nan = std::numeric_limits<double>::quiet_NaN();

    // The grid's height (local, sea level relative) under each sample, NaN outside it or in its voids.
    std::vector<double> gridH(n, nan);
    double gridCell = 0.0;
    const bool useGrid = grid && georef && grid->width > 1 && grid->height > 1;
    if (grid && !georef)
        r.notes.push_back("the terrain has no georeference, so the bathymetry grid can't be placed; the seabed is the profile alone");
    else if (useGrid && !georef->geographicOk())
        r.notes.push_back("the terrain's projection (EPSG " + std::to_string(georef->foreignEpsg) + ") can't be converted; the seabed is the profile alone");
    if (useGrid && georef->geographicOk())
    {
        const bool geo = grid->geographic;
        const double dx = grid->dx != 0.0 ? grid->dx : 1.0 / kGebcoPerDegree, dy = grid->dy != 0.0 ? grid->dy : 1.0 / kGebcoPerDegree;
        gridCell = geo ? std::abs(dx) * 111320.0 * 100.0 * std::cos(georef->toGeographic(h.origin + h.extent() * 0.5).lat / 57.29577951308232) : std::abs(dx) * 100.0;
        tbb::parallel_for(0, h.nz, [&](int z) {
            for (int x = 0; x < h.nx; ++x)
            {
                const LatLon ll = georef->toGeographic(h.samplePos(x, z));
                double cx, cy;
                if (geo)
                {
                    cx = (ll.lon - grid->x0) / dx;
                    cy = (grid->y0 - ll.lat) / dy;
                }
                else
                {
                    bool south = false;
                    const int zone = utmZoneOfEpsg(grid->epsg, &south);
                    const glm::dvec2 en = latLonToUtm(ll, zone ? zone : utmZone(ll), south);
                    cx = (en.x - grid->x0) / dx;
                    cy = (grid->y0 - en.y) / dy;
                }
                if (cx < 0.0 || cy < 0.0 || cx > grid->width - 1 || cy > grid->height - 1)
                    continue;
                const int x0 = std::min(int(cx), grid->width - 2), y0 = std::min(int(cy), grid->height - 2);
                const double fx = cx - x0, fy = cy - y0;
                auto at = [&](int xx, int yy) { return double(grid->metres[size_t(yy) * size_t(grid->width) + size_t(xx)]); };
                const double v = (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
                if (std::isnan(v))
                    continue;
                gridH[h.index(x, z)] = s.seaLevel + v * 100.0 * (v < 0.0 ? s.depthScale : 1.0);
            }
        });
    }

    // Sea in the DEM: low ground joined to the edge or to deep grid water.
    std::vector<uint8_t> low(n, 0);
    for (size_t i = 0; i < n; ++i)
        low[i] = h.heights[i] <= s.seaLevel + s.landTolerance;
    std::vector<int32_t> label;
    const int count = components(low, h.nx, h.nz, label);
    std::vector<uint8_t> open(size_t(std::max(count, 0)), 0);
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            const size_t i = h.index(x, z);
            if (label[i] < 0)
                continue;
            const bool edge = x == 0 || z == 0 || x == h.nx - 1 || z == h.nz - 1;
            if (edge || (!std::isnan(gridH[i]) && s.seaLevel - gridH[i] > 300.0))
                open[size_t(label[i])] = 1;
        }
    std::vector<uint8_t> sea(n, 0), land(n, 0);
    for (size_t i = 0; i < n; ++i)
    {
        sea[i] = label[i] >= 0 && open[size_t(label[i])];
        land[i] = !sea[i];
    }
    const std::vector<float> dist = distanceToSet(land, h.nx, h.nz, h.cell);

    // The grid smoothed over the sea into a shelf: a separable box blur twice, ignoring voids and land.
    if (useGrid)
    {
        const double radiusCm = s.smoothing > 0.0 ? s.smoothing : std::max(gridCell, h.cell);
        const int rad = std::clamp(int(std::lround(radiusCm / h.cell * 0.5)), 0, 256);
        if (rad > 0)
            for (int pass = 0; pass < 2; ++pass)
                for (int axis = 0; axis < 2; ++axis)
                {
                    std::vector<double> next(gridH);
                    const int lines = axis == 0 ? h.nz : h.nx, len = axis == 0 ? h.nx : h.nz;
                    tbb::parallel_for(0, lines, [&](int l) {
                        auto idx = [&](int k) { return axis == 0 ? h.index(k, l) : h.index(l, k); };
                        double sum = 0.0;
                        int cnt = 0;
                        auto add = [&](int k, int sign) {
                            if (k < 0 || k >= len)
                                return;
                            const size_t i = idx(k);
                            if (sea[i] && !std::isnan(gridH[i]))
                            {
                                sum += sign * gridH[i];
                                cnt += sign;
                            }
                        };
                        for (int k = -rad; k <= rad; ++k)
                            add(k, 1);
                        for (int k = 0; k < len; ++k)
                        {
                            const size_t i = idx(k);
                            if (sea[i] && cnt > 0)
                                next[i] = sum / cnt;
                            add(k + rad + 1, 1);
                            add(k - rad, -1);
                        }
                    });
                    gridH.swap(next);
                }
    }

    const double tanS = std::tan(s.shoreSlope / 57.29577951308232);
    std::vector<float>& seabed = h.mask("seabed");
    double depthSum = 0.0;
    size_t gridUsed = 0;
    for (size_t i = 0; i < n; ++i)
    {
        if (!sea[i])
        {
            seabed[i] = 0.0f;
            continue;
        }
        const double d = std::isfinite(dist[i]) ? double(dist[i]) : 1e12;
        const double profile = s.shelfDepth * (1.0 - std::exp(-d * tanS / s.shelfDepth)) +
                               (s.dropDepth - s.shelfDepth) * bathy::smooth01(s.shelfWidth, s.shelfWidth + s.slopeWidth, d);
        double depth = profile;
        if (!std::isnan(gridH[i]))
        {
            const double g = std::max(s.minDepth, s.seaLevel - gridH[i]);
            const double t = bathy::smooth01(0.0, std::max(s.blendWidth, h.cell), d);
            depth = (1.0 - t) * profile + t * g;
            ++gridUsed;
        }
        depth = std::max(depth, s.minDepth * std::clamp(d / (10.0 * h.cell), 0.0, 1.0));
        const double bed = std::min(double(h.heights[i]), s.seaLevel - depth);
        h.heights[i] = float(bed);
        seabed[i] = 1.0f;
        ++r.seaSamples;
        depthSum += s.seaLevel - bed;
        r.maxDepth = std::max(r.maxDepth, s.seaLevel - bed);
    }
    r.gridSamples = gridUsed;
    r.meanDepth = r.seaSamples ? depthSum / double(r.seaSamples) : 0.0;
    r.source = gridUsed == 0 ? "profile" : gridUsed == r.seaSamples ? "grid" : "grid+profile";
    if (useGrid && gridUsed == 0 && r.seaSamples > 0)
        r.notes.push_back("the bathymetry grid doesn't cover this terrain's sea; the seabed is the profile alone");
    else if (useGrid && gridUsed < r.seaSamples)
        r.notes.push_back("the bathymetry grid covers " + std::to_string(int(100.0 * double(gridUsed) / double(r.seaSamples))) +
                          "% of the sea; the rest is the profile");
    if (r.seaSamples == 0)
        r.notes.push_back("no sea: nothing at or below sea level reaches the edge or deep water");
    return r;
}
} // namespace terrain
