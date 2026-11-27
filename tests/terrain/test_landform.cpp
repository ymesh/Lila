#include "TestUtil.h"

#include "ErosionFilter.h"
#include "ErosionGpu.h"
#include "Landform.h"
#include "LandformGpu.h"
#include "Shaping.h"
#include "Water.h"

#include "render/GpuCompute.h"
#include "render/Renderer.h"

#include <tbb/global_control.h>

#include <chrono>
#include <cmath>
#include <set>

using namespace terraintest;
using lila::geo::Heightfield;

namespace
{
Heightfield landRanges(int n, uint32_t seed = 7, double size = 200000.0)
{
    terrain::NoiseParams np;
    np.featureSize = size * 0.3;
    np.amplitude = 20000.0;
    np.seed = seed;
    Heightfield h = lila::geo::makeHeightfieldCovering({0.0, 0.0}, {size, size}, n);
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            const glm::dvec2 p = h.samplePos(x, z);
            h.heights[h.index(x, z)] = float(terrain::noiseHeight(np, p.x, p.y));
        }
    return h;
}

/** Every sample's receiver chain reaches the edge, downhill or level on the filled surface, which is never below the ground. */
void requireDrains(const Heightfield& h)
{
    const terrain::Drainage d = terrain::drain(h);
    size_t bad = 0;
    for (size_t i = 0; i < h.size(); ++i)
    {
        if (d.filled[i] < h.heights[i] || d.filled[i] < d.filled[d.receiver[i]])
            ++bad;
        uint32_t k = uint32_t(i);
        size_t steps = 0;
        while (d.receiver[k] != k && steps++ <= h.size())
            k = d.receiver[k];
        const int x = int(k % uint32_t(h.nx)), z = int(k / uint32_t(h.nx));
        if (!(x == 0 || z == 0 || x == h.nx - 1 || z == h.nz - 1))
            ++bad;
    }
    CHECK(bad == 0);
}

template <class Fn> uint64_t hashAt(int threads, Fn fn)
{
    tbb::global_control limit(tbb::global_control::max_allowed_parallelism, size_t(threads));
    Heightfield h = fn();
    return terrain::heightfieldHash(h);
}

double meanAbsChange(const Heightfield& a, const Heightfield& b)
{
    double s = 0.0;
    for (size_t k = 0; k < a.size(); ++k)
        s += std::abs(double(a.heights[k]) - double(b.heights[k]));
    return s / double(a.size());
}

double maxSlope(const Heightfield& h, int xFrom, int xTo)
{
    double best = 0.0;
    for (int z = 1; z < h.nz - 1; ++z)
        for (int x = std::max(1, xFrom); x <= std::min(h.nx - 2, xTo); ++x)
            for (auto [dx, dz] : {std::pair{1, 0}, {0, 1}, {-1, 0}, {0, -1}})
                best = std::max(best, std::abs(double(h.at(x + dx, z + dz)) - double(h.at(x, z))) / h.cell);
    return best;
}

bool startLandGpu()
{
    auto& r = lila::render::Renderer::get();
    if (r.running() || r.start())
        return true;
    MESSAGE("no GPU for landform GPU tests");
    return false;
}
} // namespace

TEST_CASE("landform: analytic erosion carves more with age, keeps the range, and every sample drains")
{
    const Heightfield h0 = landRanges(129);
    Heightfield young = h0, old = h0;
    terrain::AnalyticParams p;
    p.age = 0.2;
    terrain::erodeAnalytic(young, p);
    p.age = 0.9;
    terrain::erodeAnalytic(old, p);
    const double ys = meanAbsChange(h0, young), os = meanAbsChange(h0, old);
    MESSAGE("analytic mean change: age 0.2 " << ys << " cm, age 0.9 " << os << " cm");
    CHECK(ys > 1.0);
    CHECK(os > ys);
    CHECK(old.range().x == doctest::Approx(h0.range().x).epsilon(1e-4));
    CHECK(old.range().y == doctest::Approx(h0.range().y).epsilon(1e-4));
    for (const char* m : {"flow", "lakes", "wear", "deposit"})
        CHECK(old.findMask(m));
    requireDrains(old);
    Heightfield none = h0;
    p.age = 0.0;
    terrain::erodeAnalytic(none, p);
    CHECK(none.heights == h0.heights);
}

TEST_CASE("landform: amplification doubles the resolution and keeps the large shapes and the main rivers")
{
    const Heightfield lo = landRanges(129, 3);
    terrain::AmplifyParams p;
    p.factor = 2;
    const Heightfield hi = terrain::amplified(lo, p);
    CHECK(hi.nx == 257);
    CHECK(hi.nz == 257);
    CHECK(hi.cell == doctest::Approx(lo.cell / 2));
    // Back on the input grid it still looks like the input.
    Heightfield back = hi;
    back.masks.clear();
    back = lila::geo::resampled(back, lo.nx, lo.nz, lo.cell, lo.origin);
    double sa = 0, sb = 0, sab = 0, saa = 0, sbb = 0;
    const double n = double(lo.size());
    for (size_t k = 0; k < lo.size(); ++k)
    {
        const double a = lo.heights[k], b = back.heights[k];
        sa += a;
        sb += b;
        sab += a * b;
        saa += a * a;
        sbb += b * b;
    }
    const double corr = (sab - sa * sb / n) / std::sqrt((saa - sa * sa / n) * (sbb - sb * sb / n));
    MESSAGE("amplified vs input correlation " << corr);
    CHECK(corr > 0.95);
    // The input's biggest rivers stay rivers: their samples still carry a large drainage area after amplifying.
    const terrain::Drainage dl = terrain::drain(lo), db = terrain::drain(back);
    std::vector<float> sorted = db.area;
    std::sort(sorted.begin(), sorted.end());
    const float bigBack = sorted[size_t(double(sorted.size()) * 0.9)];
    sorted = dl.area;
    std::sort(sorted.begin(), sorted.end());
    const float bigLo = sorted[size_t(double(sorted.size()) * 0.98)];
    size_t rivers = 0, kept = 0;
    for (size_t k = 0; k < lo.size(); ++k)
        if (dl.area[k] >= bigLo)
        {
            ++rivers;
            // A river may shift a sample sideways; look in the 3x3 around it.
            const int x = int(k % size_t(lo.nx)), z = int(k / size_t(lo.nx));
            bool found = false;
            for (int dz = -1; dz <= 1 && !found; ++dz)
                for (int dx = -1; dx <= 1 && !found; ++dx)
                {
                    const int xx = std::clamp(x + dx, 0, lo.nx - 1), zz = std::clamp(z + dz, 0, lo.nz - 1);
                    found = db.area[lo.index(xx, zz)] >= bigBack;
                }
            kept += found;
        }
    MESSAGE("main river samples kept after amplifying: " << kept << " of " << rivers);
    CHECK(double(kept) / double(rivers) > 0.7);
    requireDrains(hi);
}

TEST_CASE("landform: debris flow scours steep ground and lays cones below, writing its masks")
{
    Heightfield h = lila::geo::makeHeightfieldCovering({0.0, 0.0}, {20000.0, 20000.0}, 101);
    // A steep cone on a plain.
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            const double r = glm::length(h.samplePos(x, z)) / 6000.0;
            h.heights[h.index(x, z)] = float(std::max(0.0, 1.0 - r) * 9000.0);
        }
    const Heightfield before = h;
    terrain::DebrisParams p;
    p.strength = 2.0;
    terrain::debrisFlow(h, p);
    CHECK(meanAbsChange(before, h) > 1.0);
    CHECK(h.findMask("wear"));
    CHECK(h.findMask("talus"));
    double wearMax = 0, depMax = 0;
    for (size_t k = 0; k < h.size(); ++k)
    {
        wearMax = std::max(wearMax, double(h.mask("wear")[k]));
        depMax = std::max(depMax, double(h.mask("deposit")[k]));
    }
    CHECK(wearMax > 0.0);
    CHECK(depMax > 0.0);
    requireDrains(h);
}

TEST_CASE("landform: thermal erosion keeps a steeper slope on the hard material and conserves the ground")
{
    Heightfield h = lila::geo::makeHeightfieldCovering({0.0, 0.0}, {10000.0, 10000.0}, 65);
    std::vector<float>& hard = h.mask("hard");
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            // A ridge running along X, hard rock on the +Z half.
            h.heights[h.index(x, z)] = float(std::max(0.0, 4000.0 - std::abs(h.samplePos(x, z).y) * 2.0));
            hard[h.index(x, z)] = x > h.nx / 2 ? 1.0f : 0.0f;
        }
    double before = 0.0;
    for (float v : h.heights)
        before += v;
    const std::vector<float> angles = terrain::talusAngles(h, 30.0, {{"hard", 70.0}});
    terrain::thermalMaterial(h, angles, 200);
    double after = 0.0;
    for (float v : h.heights)
        after += v;
    CHECK(after == doctest::Approx(before).epsilon(1e-4));
    const double soft = maxSlope(h, 1, h.nx / 2 - 4), stiff = maxSlope(h, h.nx / 2 + 5, h.nx - 2);
    MESSAGE("steepest slope soft " << soft << ", hard " << stiff);
    CHECK(soft < std::tan(32.0 * 0.0174533) * 1.15);
    CHECK(stiff > soft * 1.5);
    CHECK(h.findMask("talus"));
}

TEST_CASE("landform: droplets cut rills, write wet, and give the same bytes at any thread count")
{
    auto run = [] {
        Heightfield h = landRanges(193, 11, 60000.0);
        terrain::DropletParams p;
        p.density = 0.5;
        p.seed = 4;
        terrain::droplets(h, p);
        return h;
    };
    const Heightfield a = run();
    const Heightfield base = landRanges(193, 11, 60000.0);
    CHECK(meanAbsChange(base, a) > 0.01);
    CHECK(a.findMask("wet"));
    CHECK(hashAt(1, run) == hashAt(8, run));
    CHECK(hashAt(3, run) == terrain::heightfieldHash(a));
}

TEST_CASE("landform: deposition fills a pit and lays sediment at the valley mouth")
{
    Heightfield h = lila::geo::makeHeightfieldCovering({0.0, 0.0}, {20000.0, 20000.0}, 81);
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            const glm::dvec2 q = h.samplePos(x, z);
            // A valley falling toward -X with a pit in its floor.
            double v = 3000.0 + q.x * 0.15 + std::abs(q.y) * 0.4;
            v -= 800.0 * std::exp(-glm::dot(q, q) / (1500.0 * 1500.0));
            h.heights[h.index(x, z)] = float(v);
        }
    const size_t c = h.index(h.nx / 2, h.nz / 2);
    auto lakeVolume = [](const Heightfield& f) {
        const terrain::Drainage d = terrain::drain(f);
        double v = 0.0;
        for (size_t k = 0; k < f.size(); ++k)
            v += std::max(0.0, double(d.filled[k]) - double(f.heights[k]) - 0.01);
        return v;
    };
    const double before = lakeVolume(h);
    terrain::DepositionParams p;
    p.iterations = 3;
    terrain::deposition(h, p);
    const double after = lakeVolume(h);
    MESSAGE("pit volume " << before << " before, " << after << " after (cm x samples)");
    CHECK(after < before * 0.9);
    CHECK(h.findMask("sediment"));
    CHECK(h.mask("sediment")[c] > 0.0f);
}

TEST_CASE("landform: snow lies above the snowline and off steep ground")
{
    Heightfield h = lila::geo::makeHeightfieldCovering({0.0, 0.0}, {20000.0, 20000.0}, 81);
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            const double px = h.samplePos(x, z).x;
            // Gentle ramp up along X, with a cliff band near the top.
            double v = (px + 10000.0) * 0.2;
            if (px > 6000.0 && px < 7000.0)
                v += (px - 6000.0) * 3.0;
            else if (px >= 7000.0)
                v += 3000.0;
            h.heights[h.index(x, z)] = float(v);
        }
    terrain::SnowParams p;
    p.snowline = 3000.0;
    p.band = 200.0;
    p.slide = 0;
    terrain::snow(h, p);
    const std::vector<float>& s = h.mask("snow");
    auto at = [&](double px) { return s[h.index(int(std::lround((px + 10000.0) / h.cell)), h.nz / 2)]; };
    CHECK(at(-8000.0) == doctest::Approx(0.0));
    CHECK(at(9000.0) > 0.9f);
    CHECK(at(6500.0) < 0.1f);
}

TEST_CASE("landform: the erosion filter cuts gullies on slopes, leaves flats, and matches across tiles")
{
    terrain::ErosionFilterParams p;
    p.scale = 3000.0;
    Heightfield flat = lila::geo::makeHeightfieldCovering({0.0, 0.0}, {10000.0, 10000.0}, 65);
    const Heightfield flat0 = flat;
    terrain::erosionFilter(flat, p);
    CHECK(flat.heights == flat0.heights);

    auto slope = [](glm::dvec2 centre) {
        Heightfield h = lila::geo::makeHeightfieldCovering(centre, {10000.0, 10000.0}, 65);
        for (int z = 0; z < h.nz; ++z)
            for (int x = 0; x < h.nx; ++x)
            {
                const glm::dvec2 q = h.samplePos(x, z);
                h.heights[h.index(x, z)] = float(q.x * 0.7 + q.y * 0.2);
            }
        return h;
    };
    Heightfield a = slope({0.0, 0.0}), b = slope({10000.0, 0.0});
    const Heightfield a0 = a;
    terrain::erosionFilter(a, p);
    terrain::erosionFilter(b, p);
    double mean = 0.0, amp = 0.0;
    for (size_t k = 0; k < a.size(); ++k)
    {
        const double d = double(a.heights[k]) - double(a0.heights[k]);
        mean += d;
        amp = std::max(amp, std::abs(d));
    }
    mean /= double(a.size());
    MESSAGE("filter: largest offset " << amp << " cm, mean " << mean << " cm");
    CHECK(amp > 100.0);
    CHECK(std::abs(mean) < amp * 0.2);
    // a's last column is b's first, on the same world positions; the interior rows see the same gradient.
    for (int z = 1; z < a.nz - 1; ++z)
        CHECK(a.at(a.nx - 1, z) == doctest::Approx(b.at(0, z)).epsilon(1e-5));
}

TEST_CASE("landform: shaping operators terrace, strata, fold, warp, sharpen and cliff")
{
    Heightfield base = landRanges(97, 5, 50000.0);
    {
        Heightfield h = base;
        terrain::TerraceParams p;
        p.step = 1000.0;
        p.sharpness = 1.0;
        terrain::terrace(h, p);
        size_t onStep = 0;
        for (float v : h.heights)
            onStep += std::abs(v / 1000.0 - std::round(v / 1000.0)) < 0.02;
        CHECK(double(onStep) / double(h.size()) > 0.9);
    }
    {
        Heightfield h = base;
        terrain::strata(h, {});
        CHECK(h.findMask("strata"));
        CHECK(meanAbsChange(base, h) > 1.0);
    }
    {
        Heightfield h = base;
        terrain::FoldParams p;
        p.amplitude = 2000.0;
        terrain::fold(h, p);
        CHECK(meanAbsChange(base, h) > 500.0);
    }
    {
        Heightfield h = base;
        terrain::warp(h, {});
        CHECK(h.range().y <= base.range().y + 1.0);
        CHECK(meanAbsChange(base, h) > 1.0);
    }
    {
        Heightfield h = base;
        terrain::ridgeSharpen(h, {});
        CHECK(h.range().y > base.range().y);
    }
    {
        Heightfield h = base;
        terrain::CliffParams p;
        p.threshold = 20.0;
        terrain::cliff(h, p);
        CHECK(h.findMask("cliff"));
        double most = 0.0;
        for (float v : h.mask("cliff"))
            most = std::max(most, double(v));
        CHECK(most > 0.5);
    }
}

TEST_CASE("landform: Erode presets write every mask, drain, do nothing at age 0, and repeat exactly at any thread count")
{
    for (const std::string& preset : terrain::erodePresetNames())
    {
        Heightfield h = landRanges(161, 9, 120000.0);
        terrain::PresetParams p;
        p.preset = preset;
        p.age = 0.6;
        terrain::PresetReport r;
        terrain::erodePreset(h, p, &r);
        for (const char* m : {"flow", "lakes", "wear", "deposit", "sediment", "wet", "talus"})
            CHECK_MESSAGE(h.findMask(m), preset << " lacks " << m);
        if (preset == "alpine")
            CHECK(h.findMask("snow"));
        CHECK(!r.steps.empty());
        requireDrains(h);
    }
    Heightfield h = landRanges(161, 9, 120000.0);
    const uint64_t before = terrain::heightfieldHash(h);
    terrain::PresetParams off;
    off.age = 0.0;
    terrain::erodePreset(h, off);
    CHECK(terrain::heightfieldHash(h) == before);

    auto run = [] {
        Heightfield g = landRanges(400, 2, 200000.0);
        terrain::PresetParams p;
        p.preset = "alpine";
        p.age = 0.7;
        p.seed = 3;
        terrain::erodePreset(g, p);
        return g;
    };
    const uint64_t one = hashAt(1, run), two = hashAt(2, run), many = hashAt(16, run);
    MESSAGE("preset hash 1 thread " << one << ", 2 threads " << two << ", 16 threads " << many);
    CHECK(one == two);
    CHECK(one == many);
}

TEST_CASE("landform: Erode preset keepForm carves along the input's drainage and keeps its wider landforms")
{
    // Real-elevation ranges: the preset's macro step would rebuild them; keepForm only works below formRadius.
    const Heightfield start = landRanges(257, 4, 120000.0);
    Heightfield kept = start, rebuilt = start;
    terrain::PresetParams p;
    p.age = 0.6;
    p.keepForm = true;
    p.formRadius = 6000.0;
    terrain::PresetReport r;
    terrain::erodePreset(kept, p, &r);
    terrain::PresetParams macro = p;
    macro.keepForm = false;
    terrain::erodePreset(rebuilt, macro);
    bool fine = false, hasMacro = false;
    for (const auto& [name, ms] : r.steps)
        fine = fine || name == "fine", hasMacro = hasMacro || name == "macro";
    CHECK(fine);
    CHECK_FALSE(hasMacro);
    // Low-passed past the form radius, the kept ground stays close to the input; the rebuilt one moves far more.
    const int radius = int(std::lround(2.0 * p.formRadius / start.cell));
    const std::vector<float> a = terrain::boxBlurred(start.heights, start.nx, start.nz, radius);
    const std::vector<float> b = terrain::boxBlurred(kept.heights, start.nx, start.nz, radius);
    const std::vector<float> c = terrain::boxBlurred(rebuilt.heights, start.nx, start.nz, radius);
    double keptMoved = 0.0, rebuiltMoved = 0.0, carved = 0.0;
    for (size_t k = 0; k < start.size(); ++k)
    {
        keptMoved += std::abs(b[k] - a[k]);
        rebuiltMoved += std::abs(c[k] - a[k]);
        carved += std::abs(kept.heights[k] - start.heights[k]);
    }
    MESSAGE("large-form change: keepForm " << keptMoved / start.size() << " cm, macro " << rebuiltMoved / start.size() << " cm; carved " << carved / start.size() << " cm");
    CHECK(carved > 0.0);
    CHECK(keptMoved * 3.0 < rebuiltMoved);
    requireDrains(kept);
}

TEST_CASE("landform: preset timing at 1K")
{
    Heightfield h = landRanges(1025, 1, 400000.0);
    terrain::PresetParams p;
    p.age = 0.6;
    terrain::PresetReport r;
    const auto t0 = std::chrono::steady_clock::now();
    terrain::erodePreset(h, p, &r);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("1025x1025 natural preset: " << s << " s, " << r.toJson().dump());
    CHECK(s < 60.0);
}

TEST_CASE("landform: preset timing at 4K" * doctest::skip())
{
    for (const char* preset : {"natural", "alpine"})
    {
        Heightfield h = landRanges(4097, 1, 1600000.0);
        terrain::PresetParams p;
        p.preset = preset;
        p.age = 0.6;
        terrain::PresetReport r;
        const auto t0 = std::chrono::steady_clock::now();
        terrain::erodePreset(h, p, &r);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        MESSAGE("4097x4097 " << preset << " preset: " << s << " s, " << r.toJson().dump());
    }
    Heightfield h = landRanges(4097, 1, 1600000.0);
    terrain::ErosionFilterParams fp;
    const auto t0 = std::chrono::steady_clock::now();
    terrain::erosionFilter(h, fp);
    MESSAGE("4097x4097 erosion filter CPU: " << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s");
    if (startLandGpu())
    {
        Heightfield g = landRanges(4097, 1, 1600000.0);
        const auto t1 = std::chrono::steady_clock::now();
        std::string err;
        REQUIRE_MESSAGE(terrain::erosionFilterGpu(g, fp, &err), err);
        MESSAGE("4097x4097 erosion filter GPU: " << std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count() << " s wall");
    }
}

TEST_CASE("landform: preset step timing at 2049 and 4097, CPU and GPU" * doctest::skip())
{
    const bool gpu = startLandGpu();
    for (int n : {2049, 4097})
    {
        const Heightfield h0 = landRanges(n, 1, 400.0 * double(n - 1));
        auto t0 = std::chrono::steady_clock::now();
        terrain::drain(h0);
        MESSAGE(n << " drain CPU: " << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s");
        for (bool useGpu : {false, true})
        {
            if (useGpu && !gpu)
                continue;
            Heightfield h = h0;
            terrain::PresetParams p;
            p.age = 0.6;
            p.gpu = useGpu;
            terrain::PresetReport r;
            t0 = std::chrono::steady_clock::now();
            terrain::erodePreset(h, p, &r);
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            MESSAGE(n << " natural preset " << (useGpu ? "GPU" : "CPU") << ": " << s << " s, " << r.toJson().dump());
            if (useGpu && n == 4097)
                CHECK(s < 15.0);
        }
    }
}

TEST_CASE("landform gpu: erosion filter matches the CPU within tolerance, material thermal exactly")
{
    if (!startLandGpu())
        return;
    terrain::ErosionFilterParams fp;
    fp.scale = 6000.0;
    Heightfield cpu = landRanges(257), gpu = cpu;
    terrain::erosionFilter(cpu, fp);
    std::string err;
    REQUIRE_MESSAGE(terrain::erosionFilterGpu(gpu, fp, &err), err);
    double worst = 0.0;
    for (size_t k = 0; k < cpu.size(); ++k)
        worst = std::max(worst, std::abs(double(cpu.heights[k]) - double(gpu.heights[k])));
    const glm::dvec2 r = cpu.range();
    MESSAGE("filter gpu vs cpu: worst " << worst << " cm over a " << (r.y - r.x) << " cm range");
    CHECK(worst < (r.y - r.x) * 1e-3);

    if (!lila::gpu::supportsFloat64())
    {
        Heightfield none = landRanges(17);
        CHECK_FALSE(terrain::thermalMaterialGpu(none, std::vector<float>(none.size(), 30.0f), 1, &err));
        return;
    }
    Heightfield tc = landRanges(257, 4), tg = tc;
    tc.mask("hard") = std::vector<float>(tc.size(), 0.0f);
    for (size_t k = 0; k < tc.size(); k += 3)
        tc.mask("hard")[k] = 1.0f;
    tg.masks = tc.masks;
    const std::vector<float> angles = terrain::talusAngles(tc, 30.0, {{"hard", 60.0}});
    terrain::thermalMaterial(tc, angles, 30);
    REQUIRE_MESSAGE(terrain::thermalMaterialGpu(tg, angles, 30, &err), err);
    worst = 0.0;
    for (size_t k = 0; k < tc.size(); ++k)
        worst = std::max(worst, std::abs(double(tc.heights[k]) - double(tg.heights[k])));
    MESSAGE("thermal gpu vs cpu: worst " << worst << " cm");
    CHECK(tc.heights == tg.heights);
}

TEST_CASE("landform gpu: GPU drainage matches drain(), so debris, deposition and the preset match the CPU")
{
    if (!startLandGpu())
        return;
    for (int n : {257, 400})
    {
        const Heightfield h = landRanges(n, uint32_t(n));
        const terrain::Drainage cpu = terrain::drain(h);
        terrain::Drainage gpu;
        std::string err;
        REQUIRE_MESSAGE(terrain::drainGpu(h, gpu, &err), err);
        CHECK(gpu.filled == cpu.filled);
        CHECK(gpu.receiver == cpu.receiver);
        CHECK(gpu.order == cpu.order);
        CHECK(gpu.area == cpu.area);
    }

    auto worstOf = [](const Heightfield& a, const Heightfield& b) {
        double worst = 0.0;
        for (size_t k = 0; k < a.size(); ++k)
            worst = std::max(worst, std::abs(double(a.heights[k]) - double(b.heights[k])));
        return worst;
    };
    // The drainage is identical, so the steps are too; the tolerance only allows for a future GPU fill that rounds.
    const Heightfield h0 = landRanges(300, 11);
    const glm::dvec2 r = h0.range();
    const double tolerance = (r.y - r.x) * 1e-4;
    terrain::DrainSource drains;
    drains.gpu = true;
    Heightfield dc = h0, dg = h0;
    terrain::DebrisParams dp;
    dp.strength = 1.5;
    terrain::debrisFlow(dc, dp);
    terrain::debrisFlow(dg, dp, &drains);
    MESSAGE("debris gpu drainage vs cpu: worst " << worstOf(dc, dg) << " cm over a " << (r.y - r.x) << " cm range");
    CHECK(worstOf(dc, dg) <= tolerance);
    Heightfield pc = h0, pg = h0;
    terrain::DepositionParams sp;
    terrain::deposition(pc, sp);
    terrain::deposition(pg, sp, &drains);
    MESSAGE("deposition gpu drainage vs cpu: worst " << worstOf(pc, pg) << " cm");
    CHECK(worstOf(pc, pg) <= tolerance);
    CHECK(drains.fallback.empty());

    // A step the GPU can't take runs on the CPU and says why.
    const Heightfield tiny = landRanges(2);
    terrain::DrainSource tinyDrains;
    tinyDrains.gpu = true;
    CHECK(tinyDrains(tiny).order == terrain::drain(tiny).order);
    CHECK(tinyDrains.fallback.find("drainage ran on the CPU") == 0);

    Heightfield c = landRanges(513, 5, 200000.0), g = c;
    terrain::PresetParams p;
    p.preset = "alpine";
    p.age = 0.7;
    terrain::erodePreset(c, p);
    p.gpu = true;
    terrain::PresetReport report;
    terrain::erodePreset(g, p, &report);
    CHECK(report.fallbacks.empty() == lila::gpu::supportsFloat64());
    // Drainage and thermal are exact, so the rills and deposition after them take the same channels.
    MESSAGE("preset gpu vs cpu: worst " << worstOf(c, g) << " cm");
    CHECK(c.heights == g.heights);
}

TEST_CASE("landform gpu: drainAuto and the callers on it (water, analytic erosion) match the CPU exactly")
{
    if (!startLandGpu())
        return;
    // Flats, terraces with pits, and a non-square grid: the cases where a fill could round or tie differently.
    std::vector<Heightfield> fields;
    fields.push_back(lila::geo::makeHeightfieldCovering({0.0, 0.0}, {30000.0, 30000.0}, 200));
    Heightfield terraced = landRanges(300, 3);
    for (float& v : terraced.heights)
        v = std::floor(v / 500.0f) * 500.0f;
    fields.push_back(terraced);
    Heightfield wide = lila::geo::makeHeightfieldCovering({0.0, 0.0}, {90000.0, 40000.0}, 451);
    const Heightfield src = landRanges(451, 9);
    for (size_t k = 0; k < wide.size(); ++k)
        wide.heights[k] = src.heights[k % src.size()];
    fields.push_back(wide);
    for (const Heightfield& f : fields)
    {
        const terrain::Drainage cpu = terrain::drain(f), gpu = terrain::drainAuto(f);
        CHECK(gpu.filled == cpu.filled);
        CHECK(gpu.receiver == cpu.receiver);
        CHECK(gpu.order == cpu.order);
        CHECK(gpu.area == cpu.area);
    }

    terrain::WaterSpec spec;
    terrain::RiverSpec river;
    river.id = "main";
    river.source = "auto";
    river.threshold = 3.0e6;
    spec.rivers.push_back(river);
    terrain::LakeSpec lakes;
    lakes.id = "lakes";
    lakes.source = "auto";
    spec.lakes.push_back(lakes);
    Heightfield wg = landRanges(257, 21), wc = wg;
    terrain::applyWater(wg, spec);
    Heightfield ag = landRanges(257, 22), ac = ag;
    terrain::AnalyticParams ap;
    terrain::erodeAnalytic(ag, ap);
    // The same calls with no GPU take drain().
    auto& r = lila::render::Renderer::get();
    r.stop();
    terrain::applyWater(wc, spec);
    terrain::erodeAnalytic(ac, ap);
    CHECK(r.start());
    CHECK(wg.heights == wc.heights);
    CHECK(wg.masks == wc.masks);
    CHECK(ag.heights == ac.heights);
}

TEST_CASE("landform commands: terrainCreate's age, terrainErode presets, and terrainLandform steps")
{
    loadTerrain();
    run("newScene");
    json t = run("terrainCreate", {{"size", {100000.0, 100000.0}}, {"resolution", 129}, {"age", 0.5}, {"name", "Landform"}});
    for (const char* m : {"flow", "wear", "deposit", "sediment", "wet", "talus"})
        CHECK_MESSAGE(t["masks"].contains(m), m);
    json e = run("terrainErode", {{"args", {"Landform"}}, {"preset", "alpine"}, {"age", 0.7}});
    CHECK(e["masks"].contains("snow"));
    CHECK(e["preset"]["stepsMs"].contains("macro"));
    CHECK(e["preset"]["preset"] == "alpine");
    const std::string agedRevision = e["revision"];

    json d = run("terrainLandform", {{"args", {"Landform"}}, {"process", "terrace"}, {"settings", {{"step", 1500.0}}}});
    CHECK(d["node"] == "Terrace");
    CHECK(d["revision"] != agedRevision);
    CHECK_THROWS(run("terrainLandform", {{"args", {"Landform"}}, {"process", "terrace"}, {"settings", {{"bogus", 1}}}}));
    json f = run("terrainLandform", {{"args", {"Landform"}}, {"process", "filter"}, {"settings", {{"scale", 4000.0}}}});
    CHECK(f["ms"].get<double>() >= 0.0);
    json r = run("terrainLandform", {{"args", {"Landform"}}, {"id", "Terrace"}, {"remove", true}});
    CHECK(r["removed"] == "Terrace");
    json s = run("terrainLandform", {{"args", {"Landform"}}, {"process", "strata"}, {"stage", "base"}});
    CHECK(s["masks"].contains("strata"));
    json th = run("terrainLandform", {{"args", {"Landform"}}, {"process", "thermal"}, {"settings", {{"materialMask", "strata"}, {"materialAngle", 65.0}}}});
    CHECK(th["masks"].contains("talus"));

    // The command path repeats exactly at any thread count.
    auto build = [](int threads) {
        tbb::global_control limit(tbb::global_control::max_allowed_parallelism, size_t(threads));
        run("newScene");
        return run("terrainCreate", {{"size", {100000.0, 100000.0}}, {"resolution", 129}, {"age", 0.6}, {"erosionPreset", "desert"}, {"seed", 5}, {"name", "Repeat"}})["revision"]
            .get<std::string>();
    };
    CHECK(build(1) == build(12));
    run("newScene");
}
