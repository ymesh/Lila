#include <doctest/doctest.h>

#include "geo/Mesh.h"
#include "ops/Command.h"
#include "pointcloud/Octree.h"
#include "pointcloud/PointCloudNode.h"
#include "pointcloud/PointSource.h"
#include "scene/Georef.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <E57Format/E57SimpleWriter.h>
#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <random>

using namespace lila;
using namespace lila::pointcloud;
namespace fs = std::filesystem;

namespace
{
fs::path dir(const char* name)
{
    const fs::path d = fs::temp_directory_path() / "lila_pointcloud" / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

/** A gently rolling surface of n points, 100 m across, around (e, n, h) in metres, with colour by position. */
std::vector<RawPoint> terrain(size_t count, const glm::dvec3& at, uint64_t seed = 7)
{
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> u(-50.0, 50.0);
    std::vector<RawPoint> out(count);
    for (RawPoint& p : out)
    {
        const double x = u(rng), y = u(rng);
        p.position = at + glm::dvec3(x, y, 2.0 * std::sin(x * 0.1) * std::cos(y * 0.07));
        p.rgb[0] = uint8_t(128 + x), p.rgb[1] = uint8_t(128 + y), p.rgb[2] = 200;
        p.intensity = uint16_t((x + 50.0) * 600.0);
        p.classification = 2;
    }
    return out;
}

/** A PointSource over points in memory. */
class MemorySource final : public PointSource
{
public:
    explicit MemorySource(std::vector<RawPoint> pts, bool bounds) : mPts(std::move(pts))
    {
        mInfo.count = mPts.size();
        mInfo.color = true;
        if (bounds)
        {
            glm::dvec3 lo(1e300), hi(-1e300);
            for (const RawPoint& p : mPts)
                lo = glm::min(lo, p.position), hi = glm::max(hi, p.position);
            mInfo.bounds = std::make_pair(lo, hi);
        }
    }
    const SourceInfo& info() const override { return mInfo; }
    bool next(std::vector<RawPoint>& out, size_t max) override
    {
        out.assign(mPts.begin() + std::ptrdiff_t(mAt), mPts.begin() + std::ptrdiff_t(std::min(mPts.size(), mAt + max)));
        mAt += out.size();
        return !out.empty();
    }
    void rewind() override { mAt = 0; }

private:
    std::vector<RawPoint> mPts;
    SourceInfo mInfo;
    size_t mAt = 0;
};

/** Checks the cache holds every point once, each inside its node's cube, and that nodes nest. */
void checkTree(const PointCloudFile& f, uint64_t expected)
{
    uint64_t total = 0;
    for (const OctreeNode& n : f.nodes())
    {
        total += n.count;
        int child = 0;
        for (int o = 0; o < 8; ++o)
            if (n.childMask & (1u << o))
            {
                const OctreeNode& c = f.nodes()[n.firstChild + uint32_t(child++)];
                CHECK(c.size == doctest::Approx(n.size * 0.5));
                CHECK(c.depth == n.depth + 1);
            }
    }
    CHECK(total == expected);
    CHECK(f.pointCount() == expected);
    // The root is a coarse sample: no more than one point per grid cell.
    CHECK(f.nodes()[0].count <= uint32_t(kGrid) * kGrid * kGrid);
}
} // namespace

TEST_CASE("LAS and LAZ write and read back with their CRS")
{
    const fs::path d = dir("las");
    const std::vector<RawPoint> pts = terrain(5000, {500000.0, 4649776.0, 100.0});
    const std::string wkt = scene::georef::wkt("EPSG:32633");
    REQUIRE_FALSE(wkt.empty());
    for (const char* name : {"t.las", "t.laz"})
    {
        CAPTURE(name);
        writeLas(d / name, pts, wkt);
        std::unique_ptr<PointSource> s = openSource(d / name);
        CHECK(s->info().count == pts.size());
        CHECK(s->info().color);
        CHECK(scene::georef::crsInfo(s->info().crs).authority == "EPSG:32633");
        std::vector<RawPoint> back;
        REQUIRE(s->next(back, 100));
        CHECK(glm::length(back[0].position - pts[0].position) < 0.001);
        CHECK(back[0].rgb[0] == pts[0].rgb[0]);
        CHECK(back[0].intensity == pts[0].intensity);
    }
}

TEST_CASE("the octree keeps every point once, in memory and out of core")
{
    const fs::path d = dir("octree");
    const std::vector<RawPoint> pts = terrain(300000, {0.0, 0.0, 0.0});
    for (const bool outOfCore : {false, true})
    {
        CAPTURE(outOfCore);
        MemorySource src(pts, !outOfCore);
        BuildOptions o;
        o.toLocal = glm::dmat4(100.0);
        o.toLocal[3][3] = 1.0;
        if (outOfCore)
            o.inMemory = 50000;
        const fs::path out = d / (outOfCore ? "file.lilapc" : "memory.lilapc");
        const BuildStats st = buildOctree(src, out, o);
        CHECK(st.points == pts.size());
        CHECK(st.nodes > 8);
        const PointCloudFile f(out);
        checkTree(f, pts.size());
        // Points come back within a quantisation step of where they were.
        const OctreeNode& root = f.nodes()[0];
        const std::vector<PackedPoint> rp = f.read(0);
        REQUIRE_FALSE(rp.empty());
        const glm::dvec3 p = PointCloudFile::position(root, rp[0]);
        CHECK(p.x >= root.min.x - 1e-9);
        CHECK(p.x <= root.min.x + root.size + 1e-9);
        CHECK(!fs::exists(fs::path(out).concat(".build")));
    }
}

TEST_CASE("a georeferenced LAZ imports at its place and exports back to the same coordinates")
{
    scene::georef::setCurrent(std::nullopt);
    const fs::path d = dir("import");
    const std::vector<RawPoint> pts = terrain(20000, {500000.0, 4649776.0, 100.0});
    writeLas(d / "site.laz", pts, scene::georef::wkt("EPSG:32633"));
    const json r = ops::execute("importPointCloud", {{"args", {(d / "site.laz").string()}}});
    CHECK(r["points"].get<uint64_t>() == pts.size());
    CHECK(r["georeference"].get<std::string>().find("Set the scene's georeference") != std::string::npos);
    const auto g = scene::georef::current();
    REQUIRE(g);
    CHECK(scene::georef::crsInfo(g->crs).authority == "EPSG:32633");
    CHECK(glm::length(glm::dvec2(g->origin) - glm::dvec2(500000.0, 4649800.0)) < 200.0);

    // A second import of the same scan reuses its cache.
    const json again = ops::execute("importPointCloud", {{"args", {(d / "site.laz").string()}}});
    CHECK(again["reused"].get<bool>());

    const std::string node = r["node"].get<std::string>();
    const json info = ops::execute("pointCloudInfo", {{"args", {node}}});
    CHECK(info["points"].get<uint64_t>() == pts.size());

    // Extracted points sit on the grid where the file had them.
    const json mesh = ops::execute("pointCloudExtract", {{"args", {node}}, {"spacing", 0.0}});
    scene::Graph& graph = scene::Graph::current();
    graph.evaluate();
    const scene::MeshPtr m = graph.find(mesh.get<std::string>())->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    CHECK(size_t(m->numPoints()) == pts.size());

    const json out = ops::execute("exportPointCloud", {{"args", {(d / "back.las").string(), node}}});
    CHECK(out["points"].get<uint64_t>() == pts.size());
    std::unique_ptr<PointSource> s = openSource(d / "back.las");
    std::vector<RawPoint> back, all;
    while (s->next(back, 65536))
        all.insert(all.end(), back.begin(), back.end());
    REQUIRE(all.size() == pts.size());
    glm::dvec3 lo(1e300), hi(-1e300), wlo(1e300), whi(-1e300);
    for (size_t i = 0; i < pts.size(); ++i)
    {
        lo = glm::min(lo, all[i].position), hi = glm::max(hi, all[i].position);
        wlo = glm::min(wlo, pts[i].position), whi = glm::max(whi, pts[i].position);
    }
    // Same extent to within the cache's quantisation (100 m / 65535 at the finest level, 2 mm at the root) and LAS's 1 mm.
    CHECK(glm::length(lo - wlo) < 0.01);
    CHECK(glm::length(hi - whi) < 0.01);

    // A clip box around the middle keeps about the middle eighth.
    const std::string box = ops::execute("pointCloudClipBox", {{"args", {node}}}).get<std::string>();
    const json clipped = ops::execute("exportPointCloud", {{"args", {(d / "clip.las").string(), node}}});
    const double kept = double(clipped["points"].get<uint64_t>()) / double(pts.size());
    CHECK(kept > 0.08);
    CHECK(kept < 0.35);
    CHECK_FALSE(box.empty());
    scene::georef::setCurrent(std::nullopt);
}

namespace
{
/** A synthetic survey of count points over 2 km square of rolling ground with box buildings, streamed without holding it. */
class SurveySource final : public PointSource
{
public:
    SurveySource(uint64_t count, const glm::dvec3& at) : mAt(at)
    {
        mInfo.count = count;
        mInfo.color = true;
        mInfo.intensity = true;
        mInfo.classification = true;
        mInfo.bounds = std::make_pair(at + glm::dvec3(-1000.0, -1000.0, -20.0), at + glm::dvec3(1000.0, 1000.0, 60.0));
    }
    const SourceInfo& info() const override { return mInfo; }
    bool next(std::vector<RawPoint>& out, size_t max) override
    {
        out.clear();
        std::uniform_real_distribution<double> u(-1000.0, 1000.0), jitter(-0.02, 0.02);
        while (out.size() < max && mMade < mInfo.count)
        {
            const double x = u(mRng), y = u(mRng);
            double z = 8.0 * std::sin(x * 0.004) * std::cos(y * 0.003) + jitter(mRng);
            uint8_t cls = 2;
            // A building every 100 m, 30 m square, 20 m tall: its roof.
            const double bx = std::fmod(x + 1000.0, 100.0), by = std::fmod(y + 1000.0, 100.0);
            if (bx > 35.0 && bx < 65.0 && by > 35.0 && by < 65.0)
                z += 20.0, cls = 6;
            RawPoint& p = out.emplace_back();
            p.position = mAt + glm::dvec3(x, y, std::clamp(z, -19.0, 59.0));
            p.classification = cls;
            p.rgb[0] = uint8_t(cls == 6 ? 190 : 90 + (int(x) & 31)), p.rgb[1] = uint8_t(cls == 6 ? 80 : 120 + (int(y) & 31)), p.rgb[2] = uint8_t(cls == 6 ? 70 : 60);
            p.intensity = uint16_t(20000 + (int(x * 7.0) & 0x3fff));
            ++mMade;
        }
        return !out.empty();
    }
    void rewind() override
    {
        mMade = 0;
        mRng.seed(11);
    }

private:
    SourceInfo mInfo;
    glm::dvec3 mAt;
    std::mt19937_64 mRng{11};
    uint64_t mMade = 0;
};
} // namespace

// Writes LILA_POINTCLOUD_BENCH (a .laz path) with LILA_POINTCLOUD_POINTS points (default 100 million) at UTM 33N and
// builds its cache, printing the times. Run by hand: lila_tests -tc="*survey*" --no-skip.
TEST_CASE("a hundred-million-point survey writes and builds" * doctest::skip())
{
    const char* path = std::getenv("LILA_POINTCLOUD_BENCH");
    if (!path)
        return;
    const uint64_t count = std::getenv("LILA_POINTCLOUD_POINTS") ? std::stoull(std::getenv("LILA_POINTCLOUD_POINTS")) : 100'000'000ull;
    const auto t0 = std::chrono::steady_clock::now();
    if (!fs::exists(path))
    {
        SurveySource src(count, {500000.0, 4649776.0, 100.0});
        writeLas(path, src, scene::georef::wkt("EPSG:32633"));
    }
    const auto t1 = std::chrono::steady_clock::now();
    std::unique_ptr<PointSource> laz = openSource(path);
    CHECK(laz->info().count == count);
    BuildOptions o;
    o.toLocal = glm::translate(glm::dmat4(100.0), -glm::dvec3(500000.0, 4649776.0, 100.0));
    o.toLocal[3][3] = 1.0;
    const fs::path cache = fs::path(path).replace_extension(".bench.lilapc");
    const BuildStats st = buildOctree(*laz, cache, o);
    const auto t2 = std::chrono::steady_clock::now();
    CHECK(st.points == count);
    const PointCloudFile f(cache);
    checkTree(f, count);
    MESSAGE("write " << std::chrono::duration<double>(t1 - t0).count() << " s, build " << std::chrono::duration<double>(t2 - t1).count() << " s, "
                     << st.nodes << " nodes, depth " << st.depth);
}

TEST_CASE("an E57 scan reads through its pose, with its CRS, and imports")
{
    const fs::path d = dir("e57");
    const fs::path path = d / "scan.e57";
    {
        e57::WriterOptions wo;
        wo.guid = "{6FD5B4E8-0D4E-4B5B-9C6E-6A3D7A1B2C3D}";
        wo.coordinateMetadata = scene::georef::wkt("EPSG:32633");
        e57::Writer w(path.string(), wo);
        e57::Data3D h;
        h.guid = "{0F1B2C3D-4E5F-4061-8273-94A5B6C7D8E9}";
        h.pointCount = 1000;
        h.pointFields.cartesianXField = h.pointFields.cartesianYField = h.pointFields.cartesianZField = true;
        h.pointFields.colorRedField = h.pointFields.colorGreenField = h.pointFields.colorBlueField = true;
        h.pointFields.intensityField = true;
        h.colorLimits.colorRedMaximum = h.colorLimits.colorGreenMaximum = h.colorLimits.colorBlueMaximum = 255;
        h.intensityLimits.intensityMaximum = 1.0;
        // The scanner stood at UTM (500100, 4649800, 120): its points are relative to it.
        h.pose.translation.x = 500100.0;
        h.pose.translation.y = 4649800.0;
        h.pose.translation.z = 120.0;
        e57::Data3DPointsDouble buf(h);
        for (size_t i = 0; i < size_t(h.pointCount); ++i)
        {
            buf.cartesianX[i] = double(i % 10);
            buf.cartesianY[i] = double(i / 10 % 10);
            buf.cartesianZ[i] = double(i / 100);
            buf.colorRed[i] = 200, buf.colorGreen[i] = 100, buf.colorBlue[i] = 50;
            buf.intensity[i] = 0.5;
        }
        const int64_t scan = w.NewData3D(h);
        e57::CompressedVectorWriter cw = w.SetUpData3DPointsData(scan, size_t(h.pointCount), buf);
        cw.write(size_t(h.pointCount));
        cw.close();
        w.Close();
    }
    std::unique_ptr<PointSource> s = openSource(path);
    CHECK(s->info().count == 1000);
    CHECK(scene::georef::crsInfo(s->info().crs).authority == "EPSG:32633");
    std::vector<RawPoint> pts;
    REQUIRE(s->next(pts, 1 << 20));
    REQUIRE(pts.size() == 1000);
    CHECK(glm::length(pts[999].position - glm::dvec3(500109.0, 4649809.0, 129.0)) < 1e-9);
    CHECK(pts[0].rgb[0] == 200);
    CHECK(pts[0].intensity == doctest::Approx(32767).epsilon(0.01));

    scene::georef::setCurrent(std::nullopt);
    const json r = ops::execute("importPointCloud", {{"args", {path.string()}}});
    CHECK(r["points"].get<uint64_t>() == 1000);
    REQUIRE(scene::georef::current());
    scene::georef::setCurrent(std::nullopt);
}

TEST_CASE("E57 files written by other tools read with their scaled-integer coordinates and colours")
{
    const fs::path data = fs::path(LILA_TEST_E57_DATA);
    {
        std::unique_ptr<PointSource> s = openSource(data / "bunnyInt32.e57");
        CHECK(s->info().count == 30571);
        std::vector<RawPoint> pts, all;
        while (s->next(pts, 1 << 14))
            all.insert(all.end(), pts.begin(), pts.end());
        REQUIRE(all.size() == 30571);
        glm::dvec3 lo(1e300), hi(-1e300);
        for (const RawPoint& p : all)
        {
            lo = glm::min(lo, p.position);
            hi = glm::max(hi, p.position);
        }
        // The file's own cartesianBounds, in metres.
        CHECK(lo.x == doctest::Approx(-0.094689).epsilon(1e-4));
        CHECK(hi.x == doctest::Approx(0.061009).epsilon(1e-4));
        CHECK(lo.y == doctest::Approx(0.040011).epsilon(1e-4));
        CHECK(hi.y == doctest::Approx(0.187321).epsilon(1e-4));
        CHECK(lo.z == doctest::Approx(-0.061873).epsilon(1e-4));
        CHECK(hi.z == doctest::Approx(0.058799).epsilon(1e-4));
    }
    {
        std::unique_ptr<PointSource> s = openSource(data / "ColourRepresentation.e57");
        std::vector<RawPoint> pts, all;
        while (s->next(pts, 1 << 14))
            all.insert(all.end(), pts.begin(), pts.end());
        CHECK(all.size() == 153);
        bool coloured = false;
        for (const RawPoint& p : all)
        {
            CHECK(glm::all(glm::lessThanEqual(glm::abs(p.position), glm::dvec3(0.5 + 1e-6))));
            coloured |= p.rgb[0] != p.rgb[1] || p.rgb[1] != p.rgb[2];
        }
        CHECK(coloured);
    }
    scene::georef::setCurrent(std::nullopt);
    const json r = ops::execute("importPointCloud", {{"args", {(fs::path(LILA_TEST_E57_DATA) / "bunnyInt32.e57").string()}},
                                                     {"cache", (fs::temp_directory_path() / "lila_bunny.lilapc").string()}});
    CHECK(r["points"].get<uint64_t>() == 30571);
}

TEST_CASE("relocateScene moves a point cloud and its clip box")
{
    ops::execute("newScene", json::object());
    scene::georef::setCurrent(std::nullopt);
    const json r = ops::execute("importPointCloud", {{"args", {(fs::path(LILA_TEST_E57_DATA) / "bunnyInt32.e57").string()}},
                                                     {"cache", (fs::temp_directory_path() / "lila_bunny_relocate.lilapc").string()}});
    const std::string cloud = r["node"].get<std::string>();
    const std::string box = ops::execute("pointCloudClipBox", {{"args", {cloud}}}).get<std::string>();
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const glm::dvec3 cloudAt(g.find(cloud)->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
    const glm::dvec3 boxAt(g.find(box)->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
    ops::execute("relocateScene", {{"offset", json::array({1000.0, 0.0, -500.0})}, {"nodes", {cloud, box}}});
    g.evaluate();
    CHECK(glm::length(glm::dvec3(g.find(cloud)->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]) - (cloudAt + glm::dvec3(1000, 0, -500))) < 1e-9);
    CHECK(glm::length(glm::dvec3(g.find(box)->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]) - (boxAt + glm::dvec3(1000, 0, -500))) < 1e-9);
}

TEST_CASE("relocating a georeferenced scene keeps the point cloud at its place on the Earth")
{
    ops::execute("newScene", json::object());
    scene::georef::setCurrent(scene::georef::Georeference{"EPSG:32632", {696700.0, 5077800.0, 1300.0}});
    const json r = ops::execute("importPointCloud", {{"args", {(fs::path(LILA_TEST_E57_DATA) / "bunnyInt32.e57").string()}},
                                                     {"cache", (fs::temp_directory_path() / "lila_bunny_georelocate.lilapc").string()}});
    const std::string cloud = r["node"].get<std::string>();
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const bool zUp = scene::georef::sceneZUp();
    auto worldOf = [&] { return glm::dvec3(g.find(cloud)->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]); };
    const glm::dvec3 at = worldOf();
    const glm::dvec3 onEarth = scene::georef::toProjected(*scene::georef::current(), at, zUp);

    // The whole scene moves; the georeference origin takes the opposite, so the cloud's projected coordinates stay.
    const json moved = ops::execute("relocateScene", {{"offset", json::array({-120000.0, 300.0, 45000.0})}});
    g.evaluate();
    CHECK(glm::length(worldOf() - (at + glm::dvec3(-120000.0, 300.0, 45000.0))) < 1e-9);
    REQUIRE(scene::georef::current());
    CHECK(moved.contains("georeferenceOrigin"));
    CHECK(glm::length(scene::georef::toProjected(*scene::georef::current(), worldOf(), zUp) - onEarth) < 1e-9);

    ops::execute("undo", json::object());
    g.evaluate();
    CHECK(glm::length(worldOf() - at) < 1e-9);
    CHECK(scene::georef::current()->origin == glm::dvec3(696700.0, 5077800.0, 1300.0));

    // Moving the cloud alone changes where it is on the Earth and leaves the georeference.
    ops::execute("relocateScene", {{"offset", json::array({1000.0, 0.0, 0.0})}, {"nodes", {cloud}}});
    CHECK(scene::georef::current()->origin == glm::dvec3(696700.0, 5077800.0, 1300.0));
    ops::execute("undo", json::object());

    // The CRS grid can't turn, so a whole-scene move with a rotation is refused and nothing changes.
    ops::execute("spaceLocator", {{"name", "Turned"}});
    ops::execute("setAttr", {{"args", {"Turned.rotate", json::array({0.0, 30.0, 30.0})}}});
    const json turn = {{"args", {"Turned"}}, {"from", "pivot"}, {"rotation", "full"}};
    std::string why;
    try
    {
        ops::execute("relocateScene", turn);
    }
    catch (const CommandError& e)
    {
        why = e.what();
    }
    CHECK(why.find("georeferenced") != std::string::npos);
    CHECK(scene::georef::current()->origin == glm::dvec3(696700.0, 5077800.0, 1300.0));
    scene::georef::setCurrent(std::nullopt);
    CHECK_NOTHROW(ops::execute("relocateScene", turn));
}
