#include "TestUtil.h"
#include "DemRefine.h"
#include "Erosion.h"
#include "Fetch.h"
#include "Inputs.h"
#include "LandCover.h"
#include "Osm.h"
#include "Painted.h"
#include "Projection.h"
#include "Terrain.h"

#include "core/ImageIO.h"
#include "geo/Heightfield.h"

#include <doctest/doctest.h>
#include <miniz.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

using namespace lila;
using namespace terraintest;
using namespace terrain;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
fs::path inputScratch(const std::string& name)
{
    const fs::path d = fs::temp_directory_path() / ("lila_terrain_inputs_" + name);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

double perLat(double lat)
{
    const double p = lat * 0.017453292519943295;
    return 111132.92 - 559.82 * std::cos(2 * p) + 1.175 * std::cos(4 * p);
}

double perLon(double lat)
{
    const double p = lat * 0.017453292519943295;
    return 111412.84 * std::cos(p) - 93.5 * std::cos(3 * p);
}

/** Pearson correlation of two equal-length series. */
double correlation(const std::vector<float>& a, const std::vector<float>& b)
{
    double ma = 0, mb = 0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        ma += a[i];
        mb += b[i];
    }
    ma /= double(a.size());
    mb /= double(b.size());
    double sab = 0, saa = 0, sbb = 0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        sab += (a[i] - ma) * (b[i] - mb);
        saa += (a[i] - ma) * (a[i] - ma);
        sbb += (b[i] - mb) * (b[i] - mb);
    }
    return sab / std::sqrt(saa * sbb);
}

/** Share of interior samples whose gradient is under a fifth of the median: terraces from 8-bit steps show up here. */
double flatShare(const geo::Heightfield& h)
{
    std::vector<double> g;
    for (int z = 1; z < h.nz - 1; ++z)
        for (int x = 1; x < h.nx - 1; ++x)
        {
            const double gx = (h.at(x + 1, z) - h.at(x - 1, z)) * 0.5, gz = (h.at(x, z + 1) - h.at(x, z - 1)) * 0.5;
            g.push_back(std::sqrt(gx * gx + gz * gz));
        }
    std::vector<double> s = g;
    std::nth_element(s.begin(), s.begin() + ptrdiff_t(s.size() / 2), s.end());
    const double median = s[s.size() / 2];
    size_t flat = 0;
    for (double v : g)
        flat += v < 0.2 * median ? 1 : 0;
    return double(flat) / double(g.size());
}

// OSM fixture, written as XML, Overpass JSON and PBF from the same description -----------------------------------

struct FixNode
{
    int64_t id;
    double lat, lon;
};
struct FixWay
{
    int64_t id;
    std::vector<int64_t> refs;
    std::vector<std::pair<std::string, std::string>> tags;
};
struct FixMember
{
    int64_t ref;
    std::string role;
};
struct FixRelation
{
    int64_t id;
    std::vector<FixMember> members;
    std::vector<std::pair<std::string, std::string>> tags;
};

constexpr double kLat0 = 46.5, kLon0 = 7.5;

struct OsmFixture
{
    std::vector<FixNode> nodes;
    std::vector<FixWay> ways;
    std::vector<FixRelation> relations;

    OsmFixture()
    {
        // Node n at (kLat0 + dy * 0.001, kLon0 + dx * 0.001): a 1 km-ish neighbourhood.
        auto node = [&](int64_t id, double dx, double dy) { nodes.push_back({id, kLat0 + dy * 0.001, kLon0 + dx * 0.001}); };
        node(1, -3, 0), node(2, 0, 0.5), node(3, 3, 0);                     // road
        node(4, -3, -2), node(5, 3, -2);                                    // footpath
        node(6, -3, 2), node(7, 0, 2.5), node(8, 3, 3);                     // river
        node(9, -1, -3), node(10, 1, -3.5);                                 // stream
        node(11, 1, 1), node(12, 2, 1), node(13, 2, 2), node(14, 1, 2);     // lake (closed way)
        node(15, -3, 3.5), node(16, 0, 3.6), node(17, 3, 3.5);              // coastline in two ways
        node(20, -3, -1), node(21, -2, -1), node(22, -2, 0), node(23, -3, 0); // forest way
        node(30, -1, -1.5), node(31, 1, -1.5), node(32, 1, -0.5), node(33, -1, -0.5); // wood outer (two ways)
        node(40, -0.5, -1.25), node(41, 0.5, -1.25), node(42, 0.5, -0.75), node(43, -0.5, -0.75); // its hole
        ways.push_back({100, {1, 2, 3}, {{"highway", "primary"}, {"name", "Main & High"}}});
        ways.push_back({101, {4, 5}, {{"highway", "footway"}}});
        ways.push_back({102, {6, 7, 8}, {{"waterway", "river"}, {"name", "Vispa"}}});
        ways.push_back({103, {9, 10}, {{"waterway", "stream"}}});
        ways.push_back({104, {11, 12, 13, 14, 11}, {{"natural", "water"}, {"water", "lake"}, {"name", "Lake"}}});
        ways.push_back({105, {15, 16}, {{"natural", "coastline"}}});
        ways.push_back({106, {16, 17}, {{"natural", "coastline"}}});
        ways.push_back({107, {20, 21, 22, 23, 20}, {{"landuse", "forest"}}});
        ways.push_back({108, {30, 31, 32}, {}});
        ways.push_back({109, {32, 33, 30}, {}});
        ways.push_back({110, {40, 41, 42, 43, 40}, {}});
        relations.push_back({200, {{108, "outer"}, {109, "outer"}, {110, "inner"}}, {{"type", "multipolygon"}, {"natural", "wood"}}});
    }

    std::string xml() const
    {
        std::string s = "<?xml version='1.0' encoding='UTF-8'?>\n<osm version=\"0.6\">\n";
        char buf[256];
        for (const FixNode& n : nodes)
        {
            std::snprintf(buf, sizeof buf, "  <node id=\"%lld\" lat=\"%.7f\" lon=\"%.7f\"/>\n", (long long)n.id, n.lat, n.lon);
            s += buf;
        }
        auto esc = [](std::string v) {
            std::string o;
            for (char c : v)
                o += c == '&' ? std::string("&amp;") : std::string(1, c);
            return o;
        };
        for (const FixWay& w : ways)
        {
            s += "  <way id=\"" + std::to_string(w.id) + "\">\n";
            for (int64_t r : w.refs)
                s += "    <nd ref=\"" + std::to_string(r) + "\"/>\n";
            for (const auto& [k, v] : w.tags)
                s += "    <tag k=\"" + k + "\" v=\"" + esc(v) + "\"/>\n";
            s += "  </way>\n";
        }
        for (const FixRelation& r : relations)
        {
            s += "  <relation id=\"" + std::to_string(r.id) + "\">\n";
            for (const FixMember& m : r.members)
                s += "    <member type=\"way\" ref=\"" + std::to_string(m.ref) + "\" role=\"" + m.role + "\"/>\n";
            for (const auto& [k, v] : r.tags)
                s += "    <tag k=\"" + k + "\" v=\"" + v + "\"/>\n";
            s += "  </relation>\n";
        }
        return s + "</osm>\n";
    }

    std::string overpass() const
    {
        json el = json::array();
        for (const FixWay& w : ways)
        {
            json tags = json::object();
            for (const auto& [k, v] : w.tags)
                tags[k] = v;
            el.push_back({{"type", "way"}, {"id", w.id}, {"nodes", w.refs}, {"tags", tags}});
        }
        for (const FixRelation& r : relations)
        {
            json tags = json::object(), members = json::array();
            for (const auto& [k, v] : r.tags)
                tags[k] = v;
            for (const FixMember& m : r.members)
                members.push_back({{"type", "way"}, {"ref", m.ref}, {"role", m.role}});
            el.push_back({{"type", "relation"}, {"id", r.id}, {"members", members}, {"tags", tags}});
        }
        for (const FixNode& n : nodes)
            el.push_back({{"type", "node"}, {"id", n.id}, {"lat", n.lat}, {"lon", n.lon}});
        return json{{"version", 0.6}, {"elements", el}}.dump();
    }

    std::string pbf() const
    {
        struct W
        {
            std::string s;
            void varint(uint64_t v)
            {
                while (v >= 0x80)
                {
                    s += char(uint8_t(v) | 0x80);
                    v >>= 7;
                }
                s += char(uint8_t(v));
            }
            void key(uint32_t f, uint32_t w) { varint(uint64_t(f) << 3 | w); }
            void bytes(uint32_t f, const std::string& b)
            {
                key(f, 2);
                varint(b.size());
                s += b;
            }
            static uint64_t zz(int64_t v) { return uint64_t((v << 1) ^ (v >> 63)); }
            void packed(uint32_t f, const std::vector<uint64_t>& v)
            {
                W p;
                for (uint64_t x : v)
                    p.varint(x);
                bytes(f, p.s);
            }
        };
        std::vector<std::string> strings = {""};
        auto sid = [&](const std::string& t) {
            auto it = std::find(strings.begin(), strings.end(), t);
            if (it != strings.end())
                return uint64_t(it - strings.begin());
            strings.push_back(t);
            return uint64_t(strings.size() - 1);
        };
        W group;
        {
            W dense;
            std::vector<uint64_t> ids, lats, lons;
            int64_t pid = 0, plat = 0, plon = 0;
            for (const FixNode& n : nodes)
            {
                const int64_t lat = std::llround(n.lat * 1e7), lon = std::llround(n.lon * 1e7);
                ids.push_back(W::zz(n.id - pid));
                lats.push_back(W::zz(lat - plat));
                lons.push_back(W::zz(lon - plon));
                pid = n.id, plat = lat, plon = lon;
            }
            dense.packed(1, ids);
            dense.packed(8, lats);
            dense.packed(9, lons);
            group.bytes(2, dense.s);
        }
        for (const FixWay& w : ways)
        {
            W way;
            way.key(1, 0);
            way.varint(uint64_t(w.id));
            std::vector<uint64_t> keys, vals, refs;
            for (const auto& [k, v] : w.tags)
            {
                keys.push_back(sid(k));
                vals.push_back(sid(v));
            }
            int64_t prev = 0;
            for (int64_t r : w.refs)
            {
                refs.push_back(W::zz(r - prev));
                prev = r;
            }
            way.packed(2, keys);
            way.packed(3, vals);
            way.packed(8, refs);
            group.bytes(3, way.s);
        }
        for (const FixRelation& r : relations)
        {
            W rel;
            rel.key(1, 0);
            rel.varint(uint64_t(r.id));
            std::vector<uint64_t> keys, vals, roles, ids, types;
            for (const auto& [k, v] : r.tags)
            {
                keys.push_back(sid(k));
                vals.push_back(sid(v));
            }
            int64_t prev = 0;
            for (const FixMember& m : r.members)
            {
                roles.push_back(sid(m.role));
                ids.push_back(W::zz(m.ref - prev));
                prev = m.ref;
                types.push_back(1);
            }
            rel.packed(2, keys);
            rel.packed(3, vals);
            rel.packed(8, roles);
            rel.packed(9, ids);
            rel.packed(10, types);
            group.bytes(4, rel.s);
        }
        W table;
        for (const std::string& t : strings)
            table.bytes(1, t);
        W block;
        block.bytes(1, table.s);
        block.bytes(2, group.s);
        block.key(17, 0);
        block.varint(100);
        auto blob = [&](const std::string& type, const std::string& raw, bool compress) {
            W b;
            if (compress)
            {
                mz_ulong n = mz_compressBound(mz_ulong(raw.size()));
                std::string z(n, '\0');
                mz_compress(reinterpret_cast<unsigned char*>(z.data()), &n, reinterpret_cast<const unsigned char*>(raw.data()), mz_ulong(raw.size()));
                z.resize(n);
                b.key(2, 0);
                b.varint(raw.size());
                b.bytes(3, z);
            }
            else
                b.bytes(1, raw);
            W h;
            h.bytes(1, type);
            h.key(3, 0);
            h.varint(b.s.size());
            std::string out;
            const uint32_t len = uint32_t(h.s.size());
            out += char(len >> 24);
            out += char(len >> 16);
            out += char(len >> 8);
            out += char(len);
            return out + h.s + b.s;
        };
        W header;
        header.bytes(4, "OsmSchema-V0.6");
        header.bytes(4, "DenseNodes");
        return blob("OSMHeader", header.s, false) + blob("OSMData", block.s, true);
    }
};

std::map<std::string, int> kindCounts(const OsmExtract& x)
{
    std::map<std::string, int> c;
    for (const OsmFeature& f : x.features)
        ++c[osmFeatureKindNames()[size_t(f.kind)]];
    return c;
}

/** A geographic Dem shaped like a Copernicus GLO-30 tile (pixel-centre grid on whole degrees, north row first), at 10" for size. */
Dem glo30Like(int latDeg, int lonDeg, int n)
{
    Dem d;
    d.width = d.height = n;
    d.geographic = true;
    d.epsg = 4326;
    d.dx = d.dy = 1.0 / double(n);
    d.x0 = lonDeg;
    d.y0 = latDeg + 1.0;
    d.metres.resize(size_t(n) * size_t(n));
    for (int r = 0; r < n; ++r)
        for (int c = 0; c < n; ++c)
        {
            const double lat = d.y0 - d.dy * r, lon = d.x0 + d.dx * c;
            double h = 500.0 + 2000.0 * (lat - latDeg) + 300.0 * (lon - lonDeg);
            if (std::abs(lat - 46.5) < 0.005 && std::abs(lon - 7.5) < 0.005)
                h = 4000.0;
            d.metres[size_t(r) * size_t(n) + size_t(c)] = float(h);
        }
    return d;
}
} // namespace

TEST_CASE("terrain inputs: UTM matches the meridian arc and round trips under a millimetre; ENU round trips and curves")
{
    // Northing on the central meridian is 0.9996 times the WGS84 meridian arc: 4984944.378 m to 45 degrees.
    CHECK(latLonToUtm({45.0, 9.0}, 32, false).y == doctest::Approx(0.9996 * 4984944.378).epsilon(1e-9));
    CHECK(latLonToUtm({45.0, 9.0}, 32, false).x == doctest::Approx(500000.0).epsilon(1e-12));
    CHECK(latLonToUtm({-45.0, 9.0}, 32, true).y == doctest::Approx(10000000.0 - 0.9996 * 4984944.378).epsilon(1e-9));
    CHECK(utmZone({46.5, 7.5}) == 32);
    CHECK(utmZone({60.0, 5.0}) == 32);
    CHECK(utmZone({78.0, 15.0}) == 33);

    double worst = 0.0;
    for (double lat = -79.0; lat <= 83.0; lat += 6.5)
        for (double dl = -5.0; dl <= 5.0; dl += 1.25)
        {
            const int zone = 33;
            const LatLon p{lat, utmCentralMeridian(zone) + dl};
            const glm::dvec2 en = latLonToUtm(p, zone, lat < 0);
            const LatLon q = utmToLatLon(en, zone, lat < 0);
            const glm::dvec2 back = latLonToUtm(q, zone, lat < 0);
            worst = std::max(worst, glm::length(back - en));
            CHECK(q.lat == doctest::Approx(p.lat).epsilon(1e-11));
            CHECK(q.lon == doctest::Approx(p.lon).epsilon(1e-11));
        }
    MESSAGE("UTM round trip worst " << worst * 1000.0 << " mm");
    CHECK(worst < 1e-3);

    const LatLon o{46.5, 7.5};
    for (double e : {-50000.0, -1000.0, 0.0, 2500.0, 50000.0})
        for (double n : {-40000.0, 0.0, 30000.0})
        {
            const LatLon g = enuToGeodetic({e, n}, o);
            const glm::dvec3 enu = geodeticToEnu(g, 0.0, o);
            CHECK(std::abs(enu.x - e) < 1e-4);
            CHECK(std::abs(enu.y - n) < 1e-4);
        }
    // 50 km away the ground has dropped about d^2 / 2R below the tangent plane.
    const double drop = geodeticToEnu(enuToGeodetic({50000.0, 0.0}, o), 0.0, o).z;
    CHECK(drop == doctest::Approx(-50000.0 * 50000.0 / (2.0 * 6378000.0)).epsilon(0.01));
}

TEST_CASE("terrain inputs: GeoTIFFs written for fetched areas read back exactly")
{
    const fs::path dir = inputScratch("tiff");
    Dem d = glo30Like(46, 7, 120);
    d.metres[17] = std::numeric_limits<float>::quiet_NaN();
    writeGeoTiff(dir / "a.tif", d);
    const Dem r = readDem(dir / "a.tif");
    CHECK(r.width == d.width);
    CHECK(r.height == d.height);
    CHECK(r.geographic);
    CHECK(r.epsg == 4326);
    CHECK(r.x0 == doctest::Approx(d.x0).epsilon(1e-12));
    CHECK(r.y0 == doctest::Approx(d.y0).epsilon(1e-12));
    CHECK(r.dx == doctest::Approx(d.dx).epsilon(1e-12));
    CHECK(std::isnan(r.metres[17]));
    bool same = true;
    for (size_t i = 0; i < d.metres.size(); ++i)
        if (i != 17 && r.metres[i] != d.metres[i])
            same = false;
    CHECK(same);
    CHECK(looksLikeElevation(dir / "a.tif"));
    fs::remove_all(dir);
}

TEST_CASE("terrain inputs: tile names, URLs and Web Mercator tile math")
{
    const std::vector<std::string> names = copernicusTileNames({-0.5, -0.5, 0.5, 0.5});
    REQUIRE(names.size() == 4);
    CHECK(names[0] == "Copernicus_DSM_COG_10_N00_00_W001_00_DEM");
    CHECK(names[1] == "Copernicus_DSM_COG_10_N00_00_E000_00_DEM");
    CHECK(names[2] == "Copernicus_DSM_COG_10_S01_00_W001_00_DEM");
    CHECK(names[3] == "Copernicus_DSM_COG_10_S01_00_E000_00_DEM");
    CHECK(copernicusTileNames({7.2, 46.2, 7.8, 46.8}).size() == 1);
    CHECK(copernicusTileNames({7.0, 46.0, 8.0, 47.0}).size() == 1);
    CHECK(copernicusUrl(names[1]) == "https://copernicus-dem-30m.s3.amazonaws.com/Copernicus_DSM_COG_10_N00_00_E000_00_DEM/Copernicus_DSM_COG_10_N00_00_E000_00_DEM.tif");

    CHECK(mercatorPixel({0.0, 0.0}, 1).x == doctest::Approx(256.0));
    CHECK(mercatorPixel({0.0, 0.0}, 1).y == doctest::Approx(256.0));
    CHECK(mercatorPixel({85.0511287798, -180.0}, 3).y == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(terrariumZoom({-0.1, -0.1, 0.1, 0.1}, 30.0) == 13);
    CHECK(terrariumZoom({-0.1, -0.1, 0.1, 0.1}, 40.0) == 12);
    const std::vector<TileXY> tiles = terrariumTiles({7.6, 45.9, 7.7, 46.0}, 12);
    CHECK(!tiles.empty());
    CHECK(tiles.size() <= 9);
    CHECK(terrariumUrl({12, 2134, 1458}) == "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/12/2134/1458.png");
    CHECK(openTopographyUrl({7, 46, 8, 47}, "COP30", "k e y").find("demtype=COP30&south=46.0000000&north=47.0000000&west=7.0000000&east=8.0000000&outputFormat=GTiff&API_Key=k%20e%20y") != std::string::npos);

    // Terrarium tiles whose pixels hold a known function of their Web Mercator position merge back to that function.
    const GeoBox box{7.60, 45.95, 7.70, 46.02};
    const int z = 12;
    std::map<TileXY, Dem> fake;
    auto f = [](double gx, double gy) { return 1000.0 + 0.05 * gx - 0.03 * gy; };
    for (const TileXY& t : terrariumTiles(box, z))
    {
        Dem d;
        d.width = d.height = 256;
        d.metres.resize(256 * 256);
        for (int y = 0; y < 256; ++y)
            for (int x = 0; x < 256; ++x)
                d.metres[size_t(y) * 256 + size_t(x)] = float(f(t.x * 256 + x + 0.5, t.y * 256 + y + 0.5));
        fake[t] = d;
    }
    size_t uncovered = 1;
    const Dem m = mosaicTerrarium(fake, z, box, 30.0, &uncovered);
    CHECK(uncovered == 0);
    CHECK(m.geographic);
    double worst = 0.0;
    for (int r = 0; r < m.height; r += 7)
        for (int c = 0; c < m.width; c += 7)
        {
            const glm::dvec2 g = mercatorPixel({m.y0 - m.dy * r, m.x0 + m.dx * c}, z);
            worst = std::max(worst, std::abs(double(m.metres[size_t(r) * size_t(m.width) + size_t(c)]) - f(g.x, g.y)));
        }
    CHECK(worst < 0.01);

    const std::vector<Place> places = parseNominatim(
        R"([{"lat":"45.9763","lon":"7.6586","display_name":"Matterhorn, Zermatt","boundingbox":["45.9663","45.9863","7.6486","7.6686"]}])");
    REQUIRE(places.size() == 1);
    CHECK(places[0].at.lat == doctest::Approx(45.9763));
    CHECK(places[0].box.west == doctest::Approx(7.6486));
    CHECK(places[0].box.north == doctest::Approx(45.9863));
    CHECK(nominatimUrl("Mount Fuji") == "https://nominatim.openstreetmap.org/search?format=jsonv2&limit=1&q=Mount%20Fuji");
}

TEST_CASE("terrain inputs: a GLO-30-shaped tile imports with the right extent, orientation and height range")
{
    const fs::path dir = inputScratch("glo30");
    const std::string tile = "Copernicus_DSM_COG_10_N46_00_E007_00_DEM";
    fs::create_directories(dir / "copernicus");
    writeGeoTiff(dir / "copernicus" / (tile + ".tif"), glo30Like(46, 7, 360));

    FetchRequest r;
    r.box = {7.2, 46.2, 7.8, 46.8};
    r.cache = dir;
    r.offline = true;
    const FetchResult res = fetchDem(r, nullptr);
    CHECK(res.tiles == 1);
    CHECK(res.cached == 1);
    CHECK(res.downloaded == 0);
    CHECK(res.attribution.find("Copernicus WorldDEM-30") != std::string::npos);
    CHECK(res.dem.width == 217);
    CHECK(res.dem.height == 217);
    CHECK(res.dem.x0 == doctest::Approx(7.2));
    CHECK(res.dem.y0 == doctest::Approx(46.8));
    // Samples land on the tile's own grid, so the merge copies them.
    CHECK(res.dem.metres[0] == doctest::Approx(500.0 + 2000.0 * 0.8 + 300.0 * 0.2).epsilon(1e-6));

    FetchRequest outside = r;
    outside.box = {8.2, 46.2, 8.4, 46.4};
    CHECK_THROWS_WITH_AS(fetchDem(outside, nullptr), doctest::Contains("offline"), std::runtime_error);

    // Local: flattened around the middle, north toward -Z.
    DemOptions o;
    Georef geo;
    geo::Heightfield h = demToTerrain(res.dem, o, ProjectionKind::Local, 0, true, &geo);
    CHECK(h.extent().x == doctest::Approx(0.6 * perLon(46.5) * 100.0).epsilon(0.002));
    CHECK(h.extent().y == doctest::Approx(0.6 * perLat(46.5) * 100.0).epsilon(0.002));
    const glm::dvec2 north = geo.toLocal({46.79, 7.5}), south = geo.toLocal({46.21, 7.5});
    CHECK(north.y < south.y);
    CHECK(h.heightAt(north.x, north.y) == doctest::Approx((500.0 + 2000.0 * 0.79 + 300.0 * 0.5) * 100.0).epsilon(0.002));
    const glm::dvec2 peak = geo.toLocal({46.5, 7.5});
    CHECK(h.heightAt(peak.x, peak.y) == doctest::Approx(400000.0).epsilon(1e-4));
    CHECK(h.range().y == doctest::Approx(400000.0).epsilon(1e-6));
    CHECK(h.range().x == doctest::Approx((500.0 + 2000.0 * 0.2 + 300.0 * 0.2) * 100.0).epsilon(1e-4));
    const LatLon back = geo.toGeographic(peak);
    CHECK(back.lat == doctest::Approx(46.5).epsilon(1e-9));
    CHECK(back.lon == doctest::Approx(7.5).epsilon(1e-9));

    // UTM: metres in zone 32, the peak where UTM puts it, north still toward -Z.
    o.cell = 3000.0;
    geo::Heightfield u = demToTerrain(res.dem, o, ProjectionKind::Utm, 0, true, &geo);
    CHECK(geo.kind == ProjectionKind::Utm);
    CHECK(geo.zone == 32);
    CHECK(u.cell == doctest::Approx(3000.0));
    const glm::dvec2 upeak = geo.toLocal({46.5, 7.5});
    CHECK(u.heightAt(upeak.x, upeak.y) > 350000.0);
    CHECK(geo.toLocal({46.79, 7.5}).y < geo.toLocal({46.21, 7.5}).y);
    CHECK(u.extent().y == doctest::Approx(0.6 * perLat(46.5) * 100.0).epsilon(0.02));
    const LatLon ub = geo.toGeographic(upeak);
    CHECK(ub.lat == doctest::Approx(46.5).epsilon(1e-9));
    CHECK(ub.lon == doctest::Approx(7.5).epsilon(1e-9));

    // ENU: the edges drop below the tangent plane by the earth's curvature.
    geo::Heightfield en = demToTerrain(res.dem, o, ProjectionKind::Enu, 0, true, &geo);
    const glm::dvec2 epeak = geo.toLocal({46.5, 7.5});
    CHECK(en.heightAt(epeak.x, epeak.y) == doctest::Approx(400000.0).epsilon(0.002));

    // The command, offline from the same cache: the terrain, its georeference, the attribution file.
    loadTerrain();
    run("newScene");
    json t = run("terrainImportDem", {{"bbox", {7.2, 46.2, 7.8, 46.8}}, {"cache", dir.string()}, {"offline", true}, {"cellSize", 6000.0}});
    CHECK(t["georef"]["projection"] == "utm");
    CHECK(t["georef"]["zone"] == 32);
    CHECK(t["fetched"]["tiles"] == 1);
    CHECK(t["heightRange"][1].get<double>() > 300000.0);
    CHECK(fs::exists(t["attributionFile"].get<std::string>()));
    json p = run("terrainGeoref", {{"args", {t["mesh"]}}, {"latLon", {{46.79, 7.5}}}});
    const json pt = p["points"][0];
    CHECK(pt[1].get<double>() == doctest::Approx((500.0 + 2000.0 * 0.79 + 300.0 * 0.5) * 100.0).epsilon(0.01));
    json ll = run("terrainGeoref", {{"args", {t["mesh"]}}, {"points", {{pt[0], pt[2]}}}});
    CHECK(ll["latLon"][0][0].get<double>() == doctest::Approx(46.79).epsilon(1e-7));
    fs::remove_all(dir);
}

TEST_CASE("terrain inputs: OSM XML, Overpass JSON and PBF give the same features in the right places")
{
    const OsmFixture fx;
    const OsmExtract a = extractOsm(parseOsmXml(fx.xml()));
    const OsmExtract b = extractOsm(parseOverpassJson(fx.overpass()));
    const OsmExtract c = extractOsm(parseOsmPbf(fx.pbf()));
    const std::map<std::string, int> want = {{"road", 1}, {"path", 1}, {"river", 1}, {"stream", 1}, {"lake", 1}, {"coastline", 1}};
    CHECK(kindCounts(a) == want);
    CHECK(kindCounts(b) == want);
    CHECK(kindCounts(c) == want);
    for (const OsmExtract* x : {&a, &b, &c})
    {
        std::map<std::string, int> areas;
        for (const OsmArea& ar : x->areas)
            ++areas[ar.cls];
        CHECK(areas["forest"] == 2);
        CHECK(areas["water"] == 1);
        const auto road = std::find_if(x->features.begin(), x->features.end(), [](const OsmFeature& f) { return f.kind == OsmFeatureKind::Road; });
        REQUIRE(road != x->features.end());
        CHECK(road->name == "Main & High");
        CHECK(road->cls == "primary");
        CHECK(road->width == doctest::Approx(1400.0));
        REQUIRE(road->points.size() == 3);
        CHECK(road->points[1].lat == doctest::Approx(kLat0 + 0.0005).epsilon(1e-9));
        CHECK(road->points[1].lon == doctest::Approx(kLon0).epsilon(1e-9));
        const auto coast = std::find_if(x->features.begin(), x->features.end(), [](const OsmFeature& f) { return f.kind == OsmFeatureKind::Coastline; });
        CHECK(coast->points.size() == 3);
        const auto lake = std::find_if(x->features.begin(), x->features.end(), [](const OsmFeature& f) { return f.kind == OsmFeatureKind::Lake; });
        CHECK(lake->closed);
        CHECK(lake->points.size() == 4);
    }

    // Placed on a heightfield around the fixture's centre: positions and land cover areas.
    Georef geo;
    geo.kind = ProjectionKind::Local;
    geo.latMid = kLat0;
    geo.x0 = kLon0;
    geo.y0 = kLat0;
    geo.mx = perLon(kLat0);
    geo.my = perLat(kLat0);
    geo::Heightfield h = geo::makeHeightfield(201, 201, 500.0, {-50000.0, -50000.0});
    const glm::dvec2 roadMid = geo.toLocal(a.features[0].points[1]);
    CHECK(roadMid.x == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(roadMid.y == doctest::Approx(-0.0005 * perLat(kLat0) * 100.0).epsilon(1e-9));
    const std::map<std::string, double> cover = rasterizeLandcover(h, a.areas, geo);
    // Forest: the 1 x 1 milli-degree way plus the 2 x 1 relation less its 1 x 0.5 hole.
    const double cellArea = 0.001 * perLon(kLat0) * 100.0 * 0.001 * perLat(kLat0) * 100.0;
    const double total = h.extent().x * h.extent().y;
    CHECK(cover.at("forest") == doctest::Approx((1.0 + 2.0 - 0.5) * cellArea / total).epsilon(0.08));
    const glm::dvec2 hole = geo.toLocal({kLat0 - 0.001, kLon0});
    const glm::dvec2 wood = geo.toLocal({kLat0 - 0.0013, kLon0 + 0.0008});
    CHECK(h.maskAt("landcover_forest", hole.x, hole.y) == doctest::Approx(0.0));
    CHECK(h.maskAt("landcover_forest", wood.x, wood.y) == doctest::Approx(1.0));

    // Cut at the heightfield's edge: a line leaving and coming back is two runs.
    geo::Heightfield small = geo::makeHeightfield(11, 11, 100.0, {0.0, 0.0});
    const auto runs = clipToHeightfield({{-500, 500}, {500, 500}, {1500, 500}, {500, 600}, {500, 900}}, small, false);
    REQUIRE(runs.size() == 2);
    CHECK(runs[0].front().x == doctest::Approx(0.0));
    CHECK(runs[0].back().x == doctest::Approx(1000.0));
    CHECK(runs[1].front().x == doctest::Approx(1000.0));
    CHECK(runs[1].back().y == doctest::Approx(900.0));
}

TEST_CASE("terrain inputs: terrainImportOsm drapes curves on the terrain, adds land cover masks and writes the attribution")
{
    loadTerrain();
    run("newScene");
    const fs::path dir = inputScratch("osm");
    {
        std::ofstream(dir / "area.osm") << OsmFixture().xml();
    }
    json t = run("terrainCreate", {{"size", {1000000.0, 1000000.0}}, {"resolution", 201}, {"amplitude", 20000.0}, {"name", "OsmGround"}});
    const std::string mesh = t["mesh"];
    CHECK_THROWS_WITH_AS(run("terrainImportOsm", {{"args", {(dir / "area.osm").string()}}, {"terrain", mesh}}), doctest::Contains("origin"), std::exception);
    json o = run("terrainImportOsm", {{"args", {(dir / "area.osm").string()}}, {"terrain", mesh}, {"origin", {kLat0, kLon0}}});
    CHECK(o["features"]["road"] == 1);
    CHECK(o["features"]["coastline"] == 1);
    CHECK(o["features"]["lake"] == 1);
    CHECK(o["outside"] == 0);
    CHECK(o["lengths"]["road"].get<double>() == doctest::Approx(2.0 * std::hypot(0.003 * perLon(kLat0), 0.0005 * perLat(kLat0)) * 100.0).epsilon(1e-6));
    CHECK(o["landcover"]["forest"].get<double>() > 0.0);
    CHECK(fs::exists(o["attributionFile"].get<std::string>()));
    {
        std::ifstream f(o["attributionFile"].get<std::string>());
        const std::string text((std::istreambuf_iterator<char>(f)), {});
        CHECK(text.find("OpenStreetMap contributors") != std::string::npos);
    }
    const std::string road = o["curves"]["road"];
    CHECK(run("getAttr", {{"args", {road + ".terrainFeature"}}}) == "road");
    const json feats = json::parse(run("getAttr", {{"args", {road + ".osmFeatures"}}}).get<std::string>());
    CHECK(feats[0]["name"] == "Main & High");
    // Draped: every point sits on the ground.
    const json data = run("curveData", {{"args", {road}}, {"worldSpace", true}});
    double worst = 0.0;
    for (const json& p : data[0]["points"])
        worst = std::max(worst, std::abs(p[1].get<double>() - heightAt(t, p[0].get<double>(), p[2].get<double>())));
    CHECK(worst < 1.0);
    const json info = run("terrainInfo", {{"args", {mesh}}});
    CHECK(info["masks"].contains("landcover_forest"));
    CHECK(info["masks"].contains("landcover_water"));
    fs::remove_all(dir);
}

TEST_CASE("terrain inputs: a crude 8-bit painting has no stepping and keeps its big shapes")
{
    const int n = 256;
    geo::Heightfield truth = geo::makeHeightfield(n, n, 100.0, {0.0, 0.0});
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x)
        {
            const double u = double(x) / n, v = double(z) / n;
            truth.heights[truth.index(x, z)] = float(0.35 + 0.08 * u + 0.05 * std::sin(6.0 * u + 1.0) * std::cos(5.0 * v) + 0.03 * v);
        }
    geo::Heightfield painted = truth;
    for (float& v : painted.heights)
        v = float(std::round(v * 255.0) / 255.0);

    {
        geo::Heightfield probe = painted;
        CHECK(dequantize(probe) == doctest::Approx(1.0 / 255.0));
        MESSAGE("dequantized alone: near-flat share " << flatShare(probe));
    }
    // Blur alone: terraces stay.
    geo::Heightfield blurOnly = painted;
    gaussianApprox(blurOnly.heights, n, n, 1.0);
    ImageOptions o;
    o.mode = ImageMode::Height;
    o.low = 0.0;
    o.high = 100000.0;
    geo::Heightfield ours = painted;
    interpretImage(ours, o);
    const double truthFlat = flatShare(truth), stepped = flatShare(blurOnly), fixedUp = flatShare(ours);
    MESSAGE("near-flat share: smooth original " << truthFlat << ", 8-bit blurred " << stepped << ", dequantized " << fixedUp);
    // Most of the stepping's excess of near-flat samples is gone; what is left sits on the single-level plateaus around
    // the painting's peaks and pits, which 8 bits can't shape.
    CHECK(stepped > 4.0 * truthFlat);
    CHECK(fixedUp - truthFlat < 0.35 * (stepped - truthFlat));
    double worst = 0.0;
    for (size_t k = 0; k < ours.size(); ++k)
        worst = std::max(worst, std::abs(double(ours.heights[k]) / 100000.0 - double(truth.heights[k])));
    CHECK(worst < 1.5 / 255.0);

    // Guide: a crude painting of hard blobs keeps its large shapes through erosion.
    geo::Heightfield crude = geo::makeHeightfield(n, n, 400.0, {0.0, 0.0});
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x)
        {
            double v = 0.2;
            if (std::hypot(x - 80.0, z - 90.0) < 50.0)
                v = 0.8;
            if (std::hypot(x - 170.0, z - 160.0) < 60.0)
                v = 0.6;
            if (std::abs(x - z) < 12)
                v = std::max(v, 0.45);
            crude.heights[crude.index(x, z)] = float(std::round(v * 255.0) / 255.0);
        }
    geo::Heightfield guided = crude;
    o.mode = ImageMode::Guide;
    o.high = 50000.0;
    interpretImage(guided, o);
    std::vector<float> a = crude.heights, b = guided.heights;
    gaussianApprox(a, n, n, 8.0);
    gaussianApprox(b, n, n, 8.0);
    const double r = correlation(a, b);
    MESSAGE("guide low-pass correlation " << r);
    CHECK(r > 0.9);
}

TEST_CASE("terrain inputs: colour sketches become ridges, valleys, rivers and flats that erosion keeps")
{
    const int n = 192;
    image::Image img;
    img.width = img.height = n;
    img.rgba.assign(size_t(n) * n * 4, 1.0f);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x)
        {
            float* p = &img.rgba[(size_t(z) * n + size_t(x)) * 4];
            const float g = float(std::round((0.3 + 0.2 * double(z) / n) * 255.0) / 255.0);
            p[0] = p[1] = p[2] = g;
            if (std::abs(z - 50) <= 2 && x > 20 && x < 170)
                p[0] = 1, p[1] = 0, p[2] = 0;
            if (std::abs(x - 96) <= 1 && z > 80)
                p[0] = 0, p[1] = 0, p[2] = 1;
            if (std::hypot(x - 150.0, z - 130.0) < 15.0)
                p[0] = 1, p[1] = 1, p[2] = 0;
        }
    ImageOptions o;
    o.mode = ImageMode::Sketch;
    o.low = 0.0;
    o.high = 50000.0;
    SketchStats st;
    const geo::Heightfield h = sketchTerrain(img, {0.0, 0.0}, 192000.0, o, &st);
    CHECK(st.ridge > 0);
    CHECK(st.river > 0);
    CHECK(st.flat > 0);
    CHECK(st.levels == doctest::Approx(1.0 / 255.0));
    auto at = [&](int x, int z) { return double(h.at(x, z)); };
    double ridge = 0, beside = 0;
    for (int x = 40; x < 150; ++x)
    {
        ridge += at(x, 50);
        beside += 0.5 * (at(x, 25) + at(x, 75));
    }
    CHECK(ridge > beside + 0.15 * 50000.0 * 110.0);
    // The held crest survives erosion within a few percent of its target.
    const std::vector<float>& target = *h.findMask("constraintHeight");
    double off = 0.0;
    for (int x = 40; x < 150; ++x)
        off = std::max(off, std::abs(at(x, 50) - target[h.index(x, 50)]));
    CHECK(off < 0.03 * 50000.0);
    double river = 0, banks = 0;
    for (int z = 100; z < 180; ++z)
    {
        river += at(96, z);
        banks += 0.5 * (at(86, z) + at(106, z));
    }
    CHECK(river < banks);
    double lo = 1e30, hi = -1e30;
    for (int z = 122; z <= 138; ++z)
        for (int x = 142; x <= 158; ++x)
            if (std::hypot(x - 150.0, z - 130.0) < 8.0)
            {
                lo = std::min(lo, at(x, z));
                hi = std::max(hi, at(x, z));
            }
    CHECK(hi - lo < 0.02 * 50000.0);
}

TEST_CASE("terrain inputs: drawn ridges and rivers hold through erosion")
{
    loadTerrain();
    run("newScene");
    json t = run("terrainCreate", {{"size", {200000.0, 200000.0}}, {"resolution", 129}, {"amplitude", 10000.0}, {"erosion", 40}, {"name", "Constrained"}});
    const std::string mesh = t["mesh"];
    json c = run("terrainConstrain", {{"args", {mesh}}, {"kind", "ridge"}, {"path", {{-60000.0, -20000.0}, {60000.0, -20000.0}}}, {"height", 15000.0}, {"width", 20000.0}});
    CHECK(c["constraints"].size() == 1);
    CHECK(c["coverage"]["ridge"].get<double>() > 0.0);
    double crest = 0.0, off = 0.0;
    for (double x = -50000.0; x <= 50000.0; x += 10000.0)
    {
        crest += heightAt(t, x, -20000.0);
        off += heightAt(t, x, 30000.0);
    }
    CHECK(crest > off + 11 * 8000.0);
    json r = run("terrainConstrain", {{"args", {mesh}}, {"kind", "river"}, {"id", "river"}, {"path", {{-90000.0, 40000.0}, {0.0, 60000.0}, {90000.0, 50000.0}}}, {"height", 3000.0}, {"width", 12000.0}});
    CHECK(r["constraints"].size() == 2);
    // Downstream the bed never climbs.
    double prev = 1e30, rises = 0.0;
    for (double s = 0.0; s <= 1.0; s += 0.05)
    {
        const glm::dvec2 a(-90000.0, 40000.0), b(0.0, 60000.0), e(90000.0, 50000.0);
        const glm::dvec2 p = s < 0.5 ? a + (b - a) * (s * 2.0) : b + (e - b) * ((s - 0.5) * 2.0);
        const double hgt = heightAt(t, p.x, p.y);
        rises = std::max(rises, hgt - prev);
        prev = hgt;
    }
    CHECK(rises < 300.0);
    json gone = run("terrainConstrain", {{"args", {mesh}}, {"remove", "river"}});
    CHECK(gone["constraints"].size() == 1);
}

TEST_CASE("terrain inputs: bare earth takes trees off a hill and keeps the hill; amplify adds detail and keeps the place")
{
    geo::Heightfield h = geo::makeHeightfield(161, 161, 3000.0, {0.0, 0.0});
    std::vector<float> ground(h.size());
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            const double d = std::hypot(x - 80.0, z - 80.0);
            ground[h.index(x, z)] = float(80000.0 * std::exp(-d * d / (2.0 * 30.0 * 30.0)));
            h.heights[h.index(x, z)] = ground[h.index(x, z)];
        }
    std::mt19937 rng(5);
    for (int k = 0; k < 300; ++k)
    {
        // Stands on a jittered grid, two cells square, apart from each other.
        const int x = 6 + (k % 17) * 9 + int(rng() % 3), z = 6 + (k / 17) * 9 + int(rng() % 3);
        if (z > 150)
            break;
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i)
                h.heights[h.index(x + i, z + j)] = ground[h.index(x + i, z + j)] + 1800.0f;
    }
    const BareEarthStats st = bareEarth(h, BareEarthParams{});
    CHECK(st.lowered > 1000);
    double worst = 0.0;
    for (size_t k = 0; k < h.size(); ++k)
        worst = std::max(worst, double(h.heights[k]) - double(ground[k]));
    MESSAGE("bare earth: tallest left " << worst << " cm of 1800");
    CHECK(worst < 400.0);
    CHECK(h.at(80, 80) == doctest::Approx(80000.0).epsilon(0.03));

    geo::Heightfield coarse = geo::makeHeightfield(65, 65, 3000.0, {0.0, 0.0});
    for (int z = 0; z < 65; ++z)
        for (int x = 0; x < 65; ++x)
            coarse.heights[coarse.index(x, z)] = float(30000.0 * std::sin(x / 9.0) * std::cos(z / 11.0) + 400.0 * x);
    AmplifyParams p;
    p.factor = 4;
    const geo::Heightfield fine = amplify(coarse, p);
    CHECK(fine.nx == 257);
    CHECK(fine.cell == doctest::Approx(750.0));
    CHECK(fine.origin == coarse.origin);
    double diff = 0.0, detail = 0.0;
    for (int z = 0; z < 65; ++z)
        for (int x = 0; x < 65; ++x)
        {
            const glm::dvec2 q = coarse.samplePos(x, z);
            diff += std::abs(fine.heightAt(q.x, q.y) - coarse.at(x, z));
        }
    diff /= 65.0 * 65.0;
    const geo::Heightfield smooth = geo::resampled(coarse, 257, 257, 750.0, coarse.origin);
    for (size_t k = 0; k < fine.size(); ++k)
        detail += std::abs(double(fine.heights[k]) - double(smooth.heights[k]));
    detail /= double(fine.size());
    MESSAGE("amplify: mean change at the data's samples " << diff << " cm, new detail " << detail << " cm");
    CHECK(diff < 0.03 * 60000.0);
    CHECK(detail > 20.0);
    const geo::Heightfield again = amplify(coarse, p);
    CHECK(again.heights == fine.heights);
}

TEST_CASE("terrain inputs: attribution sections are added and replaced")
{
    const fs::path dir = inputScratch("attr");
    writeAttribution(dir, "OpenStreetMap", "first");
    writeAttribution(dir, "Copernicus DEM GLO-30", demAttribution(DemSource::Copernicus, ""));
    writeAttribution(dir, "OpenStreetMap", osmAttribution());
    std::string text;
    {
        std::ifstream f(dir / "ATTRIBUTION.txt");
        text.assign((std::istreambuf_iterator<char>(f)), {});
    }
    CHECK(text.find("first") == std::string::npos);
    CHECK(text.find("OpenStreetMap contributors") != std::string::npos);
    CHECK(text.find("Airbus Defence and Space") != std::string::npos);
    CHECK(text.find("== OpenStreetMap ==") < text.find("== Copernicus DEM GLO-30 =="));
    fs::remove_all(dir);
}

TEST_CASE("terrain inputs: terrainReload re-reads a painted image saved again, and live link marks it")
{
    loadTerrain();
    run("newScene");
    const fs::path dir = inputScratch("live");
    auto paint = [&](float level) {
        std::vector<float> rgba(size_t(33 * 33 * 4), 1.0f);
        for (int i = 0; i < 33 * 33; ++i)
            rgba[size_t(i) * 4] = rgba[size_t(i) * 4 + 1] = rgba[size_t(i) * 4 + 2] = level;
        image::writePng(dir / "paint.png", 33, 33, rgba, 16);
    };
    paint(0.25f);
    json t = run("terrainImport", {{"args", {(dir / "paint.png").string()}}, {"mode", "height"}, {"size", 3200.0}, {"high", 1000.0}, {"name", "LivePaint"}});
    CHECK(t["heightRange"][1].get<double>() == doctest::Approx(250.0).epsilon(0.01));
    json live = run("terrainLiveLink", {{"args", {t["mesh"]}}});
    REQUIRE(live["files"].size() == 1);
    CHECK(live["files"][0]["live"] == true);
    paint(0.75f);
    // Make sure the file time moves even on coarse clocks.
    fs::last_write_time(dir / "paint.png", fs::last_write_time(dir / "paint.png") + std::chrono::seconds(2));
    json after = run("terrainReload", {{"args", {t["mesh"]}}});
    CHECK(after["heightRange"][1].get<double>() == doctest::Approx(750.0).epsilon(0.01));
    fs::remove_all(dir);
}

namespace
{
/** The synthetic WorldCover layout: no data along the top, 64-pixel blocks of every class, then scattered codes below. */
int wcCode(int c, int r)
{
    static const int k[] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 95, 100};
    if (r < 16)
        return 0;
    if (r < 512)
        return k[(c / 64 + r / 64) % 11];
    return k[((uint32_t(c) * 2654435761u) ^ (uint32_t(r) * 40503u)) % 11];
}

constexpr double kWcStep = 1.0 / 12000.0;

/** A WorldCover-shaped GeoTIFF: 8-bit codes, deflate tiles, EPSG:4326 at 1/12000 degree, nodata 0, the header at the end. */
void writeWorldCoverLike(const fs::path& path, int n, int tile, double west, double north)
{
    std::vector<uint8_t> o = {'I', 'I', 42, 0, 0, 0, 0, 0};
    auto u16 = [&](uint32_t v) {
        o.push_back(uint8_t(v));
        o.push_back(uint8_t(v >> 8));
    };
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i)
            o.push_back(uint8_t(v >> (8 * i)));
    };
    auto f64 = [&](double v) {
        const uint64_t b = std::bit_cast<uint64_t>(v);
        for (int i = 0; i < 8; ++i)
            o.push_back(uint8_t(b >> (8 * i)));
    };
    auto here = [&] {
        if (o.size() & 1)
            o.push_back(0);
        return uint32_t(o.size());
    };
    const int across = n / tile;
    std::vector<uint32_t> offsets, counts;
    for (int ty = 0; ty < across; ++ty)
        for (int tx = 0; tx < across; ++tx)
        {
            std::vector<uint8_t> raw(size_t(tile) * size_t(tile));
            for (int r = 0; r < tile; ++r)
                for (int c = 0; c < tile; ++c)
                    raw[size_t(r) * size_t(tile) + size_t(c)] = uint8_t(wcCode(tx * tile + c, ty * tile + r));
            mz_ulong packed = mz_compressBound(mz_ulong(raw.size()));
            std::vector<uint8_t> buf(packed);
            REQUIRE(mz_compress2(buf.data(), &packed, raw.data(), mz_ulong(raw.size()), 6) == MZ_OK);
            offsets.push_back(here());
            counts.push_back(uint32_t(packed));
            o.insert(o.end(), buf.begin(), buf.begin() + ptrdiff_t(packed));
        }
    const uint32_t offsetsAt = here();
    for (uint32_t v : offsets)
        u32(v);
    const uint32_t countsAt = here();
    for (uint32_t v : counts)
        u32(v);
    const uint32_t scaleAt = here();
    for (double v : {kWcStep, kWcStep, 0.0})
        f64(v);
    const uint32_t tieAt = here();
    for (double v : {0.0, 0.0, 0.0, west, north, 0.0})
        f64(v);
    const uint32_t keysAt = here();
    for (uint32_t v : {1, 1, 0, 3, 1024, 0, 1, 2, 1025, 0, 1, 1, 2048, 0, 1, 4326})
        u16(v);
    struct Entry
    {
        uint16_t tag, type;
        uint32_t count, value;
    };
    const uint32_t chunks = uint32_t(offsets.size());
    const Entry entries[] = {{256, 3, 1, uint32_t(n)},   {257, 3, 1, uint32_t(n)},       {258, 3, 1, 8},           {259, 3, 1, 8},
                             {262, 3, 1, 1},             {277, 3, 1, 1},                 {284, 3, 1, 1},           {322, 3, 1, uint32_t(tile)},
                             {323, 3, 1, uint32_t(tile)}, {324, 4, chunks, offsetsAt},   {325, 4, chunks, countsAt}, {339, 3, 1, 1},
                             {33550, 12, 3, scaleAt},    {33922, 12, 6, tieAt},          {34735, 3, 16, keysAt},   {42113, 2, 2, uint32_t('0')}};
    const uint32_t ifd = here();
    u16(uint32_t(std::size(entries)));
    for (const Entry& e : entries)
    {
        u16(e.tag);
        u16(e.type);
        u32(e.count);
        if (e.type == 3 && e.count == 1)
        {
            u16(e.value);
            u16(0);
        }
        else
            u32(e.value);
    }
    u32(0);
    for (int i = 0; i < 4; ++i)
        o[size_t(4 + i)] = uint8_t(ifd >> (8 * i));
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(o.data()), std::streamsize(o.size()));
}

/** World [x, z] of a latitude and longitude on a terrain placed around origin. */
json groundOf(const std::string& mesh, double lat, double lon, double lat0, double lon0)
{
    const json p = run("terrainGeoref", {{"args", {mesh}}, {"origin", {lat0, lon0}}, {"latLon", {{lat, lon}}}})["points"][0];
    return {p[0], p[2]};
}
} // namespace

TEST_CASE("terrain inputs: WorldCover tile names, and tiled GeoTIFFs read by window and over ranges")
{
    CHECK(worldCoverTileNames({6.5, 45.5, 7.5, 46.5}) == std::vector<std::string>{"N45E006"});
    CHECK(worldCoverTileNames({6.0, 45.0, 9.0, 48.0}) == std::vector<std::string>{"N45E006"});
    CHECK(worldCoverTileNames({-1.0, -1.0, 1.0, 1.0}) == std::vector<std::string>{"N00W003", "N00E000", "S03W003", "S03E000"});
    CHECK(worldCoverUrl("N45E006") == "https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/ESA_WorldCover_10m_2021_v200_N45E006_Map.tif");
    CHECK(worldCoverClass(10) == "forest");
    CHECK(worldCoverClass(95) == "wetland");
    CHECK(worldCoverClass(0).empty());
    CHECK(std::string(worldCoverAttribution()).find("ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021) processed by ESA WorldCover consortium") != std::string::npos);

    const fs::path dir = inputScratch("worldcover_read");
    const fs::path file = dir / "wc.tif";
    writeWorldCoverLike(file, 1024, 128, 6.0, 48.0);
    const uint64_t fileSize = fs::file_size(file);

    // The whole file through readDem: every code where it was written, no data as a void.
    const Dem all = readDem(file);
    CHECK(all.width == 1024);
    CHECK(all.geographic);
    CHECK(all.epsg == 4326);
    CHECK(all.x0 == doctest::Approx(6.0 + 0.5 * kWcStep).epsilon(1e-12));
    CHECK(all.y0 == doctest::Approx(48.0 - 0.5 * kWcStep).epsilon(1e-12));
    CHECK(std::isnan(all.metres[5]));
    int wrong = 0;
    for (int r = 16; r < 1024; r += 7)
        for (int c = 0; c < 1024; c += 5)
            wrong += all.metres[size_t(r) * 1024 + size_t(c)] != float(wcCode(c, r));
    CHECK(wrong == 0);

    // A window reads only the tiles it touches.
    uint64_t read = 0;
    const ByteRange counted = [&, inner = fileByteRange(file)](uint64_t at, uint64_t n, std::vector<uint8_t>& out) {
        inner(at, n, out);
        read += out.size();
    };
    const GeoTiffWindowReader reader(counted, "wc.tif");
    CHECK(reader.info().width == 1024);
    CHECK(reader.info().metres.empty());
    read = 0;
    const Dem w = reader.window(600, 700, 700, 760);
    CHECK(w.width == 100);
    CHECK(w.height == 60);
    CHECK(w.x0 == doctest::Approx(all.x0 + 600 * kWcStep).epsilon(1e-12));
    CHECK(w.y0 == doctest::Approx(all.y0 - 700 * kWcStep).epsilon(1e-12));
    CHECK(w.metres[size_t(13) * 100 + 42] == float(wcCode(642, 713)));
    CHECK(read < fileSize / 8);

    // Fetched over ranges as tile N45E006; the tiles north and west of it don't exist.
    std::vector<std::string> asked;
    const RangeDownloader fake = [&](const std::string& url, uint64_t at, uint64_t n, std::vector<uint8_t>* out, std::string*) {
        asked.push_back(url);
        if (url != worldCoverUrl("N45E006"))
            return 404;
        fileByteRange(file)(at, n, *out);
        return 206;
    };
    const GeoBox box{5.9995, 47.99, 6.03, 48.0005};
    const LandCoverFetch res = fetchWorldCover(box, fake);
    CHECK(res.tiles == 4);
    CHECK(res.empty == 3);
    CHECK(res.bytes < fileSize / 2);
    CHECK(res.codes.dx == doctest::Approx(kWcStep));
    auto codeAt = [&](double lat, double lon) {
        const int64_t c = std::llround((lon - res.codes.x0) / res.codes.dx), r = std::llround((res.codes.y0 - lat) / res.codes.dy);
        return res.codes.metres[size_t(r) * size_t(res.codes.width) + size_t(c)];
    };
    CHECK(codeAt(48.0 - 100.5 * kWcStep, 6.0 + 200.5 * kWcStep) == float(wcCode(200, 100)));
    CHECK(codeAt(48.0 - 115.5 * kWcStep, 6.0 + 333.5 * kWcStep) == float(wcCode(333, 115)));
    CHECK(codeAt(48.0 - 2.5 * kWcStep, 6.0 + 50.5 * kWcStep) == 0.0f);
    CHECK(codeAt(48.0 + 2.5 * kWcStep, 6.0 + 50.5 * kWcStep) == 0.0f);
    CHECK(codeAt(48.0 - 50.5 * kWcStep, 6.0 - 2.5 * kWcStep) == 0.0f);

    const RangeDownloader sea = [](const std::string&, uint64_t, uint64_t, std::vector<uint8_t>*, std::string*) { return 404; };
    CHECK_THROWS_WITH_AS(fetchWorldCover(box, sea), doctest::Contains("open sea"), std::runtime_error);
    fs::remove_all(dir);
}

TEST_CASE("terrain inputs: terrainImportLandCover puts WorldCover codes on the right cells, writes landcover masks and the attribution")
{
    loadTerrain();
    run("newScene");
    const fs::path dir = inputScratch("worldcover_import");
    const fs::path file = dir / "ESA_WorldCover_10m_2021_v200_N45E006_Map.tif";
    writeWorldCoverLike(file, 1024, 128, 6.0, 48.0);

    // About 4 km square, centred 200 rows down and 300 columns in, so its north and west edges run off the data.
    const double lat0 = 48.0 - 200.0 * kWcStep, lon0 = 6.0 + 300.0 * kWcStep;
    json t = run("terrainCreate", {{"size", {400000.0, 400000.0}}, {"resolution", 201}, {"amplitude", 0.0}, {"name", "CoverGround"}});
    const std::string mesh = t["mesh"];
    json o = run("terrainImportLandCover", {{"args", {file.string()}}, {"terrain", mesh}, {"origin", {lat0, lon0}}});
    CHECK(o["mask"] == "worldcover");
    CHECK(o["nodata"].get<double>() > 0.0);
    CHECK(o["nodata"].get<double>() < 0.2);
    double total = o["nodata"].get<double>();
    for (const auto& [cls, s] : o["landcover"].items())
        total += s.get<double>();
    CHECK(total == doctest::Approx(1.0));
    for (const char* cls : {"forest", "scrub", "grassland", "farmland", "urban", "rock", "glacier", "water", "wetland"})
        CHECK(o["landcover"].contains(cls));
    {
        std::ifstream f(o["attributionFile"].get<std::string>());
        const std::string text((std::istreambuf_iterator<char>(f)), {});
        CHECK(text.find("== ESA WorldCover ==") != std::string::npos);
        CHECK(text.find("ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021) processed by ESA WorldCover consortium") != std::string::npos);
    }

    // Block centres of known classes: (column block, row block) -> code.
    struct Probe
    {
        int bx, by, code;
        const char* cls;
    };
    const Probe probes[] = {{6, 5, 10, "forest"}, {4, 3, 80, "water"}, {5, 3, 90, "wetland"}, {4, 2, 70, "glacier"}, {3, 1, 50, "urban"}, {6, 4, 100, "grassland"}};
    json points = json::array();
    for (const Probe& p : probes)
        points.push_back(groundOf(mesh, 48.0 - (p.by * 64 + 32.5) * kWcStep, 6.0 + (p.bx * 64 + 32.5) * kWcStep, lat0, lon0));
    // North of the data: no code.
    points.push_back(groundOf(mesh, 48.0 + 8.0 * kWcStep, lon0, lat0, lon0));
    const json masks = {"worldcover", "landcover_forest", "landcover_water", "landcover_wetland", "landcover_glacier", "landcover_urban", "landcover_grassland"};
    const json s = run("terrainSample", {{"args", {mesh}}, {"points", points}, {"masks", masks}});
    for (size_t i = 0; i < std::size(probes); ++i)
    {
        const Probe& p = probes[i];
        INFO(p.cls);
        CHECK(s[i]["inside"].get<bool>());
        CHECK(s[i]["worldcover"].get<double>() * 255.0 == doctest::Approx(p.code).epsilon(1e-4));
        for (const char* cls : {"forest", "water", "wetland", "glacier", "urban", "grassland"})
            CHECK(s[i][std::string("landcover_") + cls].get<double>() == doctest::Approx(std::string(cls) == p.cls ? 1.0 : 0.0));
    }
    CHECK(s[std::size(probes)]["inside"].get<bool>());
    CHECK(s[std::size(probes)]["worldcover"].get<double>() == doctest::Approx(0.0));

    // terrainBiomes reads the codes as its default WorldCover classes.
    run("terrainBiomes", {{"args", {mesh}}, {"landcover", {{"mask", "worldcover"}}}});
    const json b = run("terrainSample", {{"args", {mesh}}, {"points", {points[0], points[2]}}, {"masks", {"biome_forest", "biome_wetland"}}});
    CHECK(b[0]["biome_forest"].get<double>() > 0.9);
    CHECK(b[1]["biome_wetland"].get<double>() > 0.9);

    // The node re-reads the file when the terrain re-runs.
    const json reload = run("terrainReload", {{"args", {mesh}}});
    bool listed = false;
    for (const json& f : reload["files"])
        listed = listed || f["file"].get<std::string>().find("ESA_WorldCover") != std::string::npos;
    CHECK(listed);
    const json info = run("terrainInfo", {{"args", {mesh}}});
    CHECK(info["masks"].contains("worldcover"));
    CHECK(info["masks"].contains("landcover_forest"));

    CHECK_THROWS_WITH_AS(run("terrainImportLandCover", {{"args", {file.string()}}, {"terrain", mesh}, {"origin", {10.0, 10.0}}}), doctest::Contains("cover"), std::exception);
    fs::remove_all(dir);
}

TEST_CASE("terrain inputs: network: a real area downloads from AWS Terrain Tiles")
{
    const char* on = std::getenv("LILA_NETWORK_TESTS");
    if (!on || std::string(on) != "1")
    {
        MESSAGE("set LILA_NETWORK_TESTS=1 to download");
        return;
    }
    loadTerrain();
    run("newScene");
    const fs::path dir = inputScratch("network");
    json t = run("terrainImportDem", {{"bbox", {7.64, 45.96, 7.68, 45.99}}, {"source", "terrarium"}, {"spacing", 6000.0}, {"cache", dir.string()}});
    MESSAGE(t.dump());
    // The Matterhorn's summit is 4478 m.
    CHECK(t["heightRange"][1].get<double>() > 420000.0);
    CHECK(t["heightRange"][1].get<double>() < 460000.0);
    fs::remove_all(dir);
}
