// The landform nodes: analytic erosion, amplify, debris flow, hydraulic (droplet) erosion, deposition, snow, the erosion
// filter, the shaping operators and the Erode Preset chain. Each wraps one function of Landform.h, ErosionFilter.h or
// Shaping.h; terrainLandform and terrainErode build groups out of them.

#include "ErosionFilter.h"
#include "Landform.h"
#include "LandformCommands.h"
#include "LandformGpu.h"
#include "Shaping.h"

#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/GpuCompute.h>
#include <lila/Log.h>
#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

using namespace lila;

namespace terrain
{
namespace lnodes_
{
using gn::ExecContext;
using gn::Geometry;
using gn::GeometryPtr;
using gn::PropDecl;
using gn::SocketDecl;
using Socket = gn::SocketType;

constexpr const char* kLandCategory = "Terrain";

SocketDecl num(std::string id, Socket t, nlohmann::json def, std::string doc, std::optional<double> lo = std::nullopt, std::optional<double> hi = std::nullopt)
{
    SocketDecl s{.id = std::move(id), .type = t, .defaultValue = std::move(def), .doc = std::move(doc)};
    s.min = lo;
    s.max = hi;
    return s;
}

SocketDecl hfIn() { return {.id = "heightfield", .type = Socket::Geometry}; }
SocketDecl hfOut() { return {.id = "heightfield", .type = Socket::Geometry}; }

PropDecl deviceProp() { return {.id = "device", .options = {"cpu", "gpu", "auto"}, .defaultValue = "cpu", .doc = "Where it runs: cpu is exact and the same everywhere."}; }

Heightfield takeHeightfield(const ExecContext& c, GeometryPtr& g)
{
    g = c.geometry("heightfield");
    if (const Heightfield* h = heightfieldOf(*g))
        return *h;
    throw gn::ExecError(c.node.name + ": the geometry has no heightfield (start from Heightfield Grid or Mesh to Heightfield)");
}

void put(ExecContext& c, const GeometryPtr& g, Heightfield h)
{
    Geometry out = *g;
    out.heightfield = std::make_shared<const Heightfield>(std::move(h));
    c.out("heightfield", gn::makeGeometry(std::move(out)));
}

/** The node's device choice: gpu throws without a GPU, auto takes it when there is one. */
bool wantsGpu(const ExecContext& c)
{
    const std::string d = c.prop("device");
    if (d == "gpu" && !gpu::available())
        throw gn::ExecError(c.node.name + ": no GPU here (it needs Lila's renderer running); set device to cpu or auto");
    return d == "gpu" || (d == "auto" && gpu::available());
}

/** A GPU step that failed: an error for device gpu, else a warning saying the CPU ran it. */
void gpuFailed(const ExecContext& c, const std::string& err)
{
    if (c.prop("device") == "gpu")
        throw gn::ExecError(c.node.name + ": " + err);
    log::warn("{}: the GPU failed ({}); ran on the CPU", c.node.name, err);
}

/** A node that reads the heightfield, changes it with fn and outputs it. */
gn::NodeDecl simple(std::string type, std::string label, std::string doc, std::vector<SocketDecl> inputs, std::function<void(ExecContext&, Heightfield&)> fn,
                    std::vector<PropDecl> props = {})
{
    inputs.insert(inputs.begin(), hfIn());
    return {.type = std::move(type),
            .label = std::move(label),
            .category = kLandCategory,
            .doc = std::move(doc),
            .props = std::move(props),
            .sockets = {.inputs = std::move(inputs), .outputs = {hfOut()}},
            .exec = [fn = std::move(fn)](ExecContext& c) {
                LILA_ZONE();
                GeometryPtr g;
                Heightfield h = takeHeightfield(c, g);
                fn(c, h);
                put(c, g, std::move(h));
            }};
}

uint32_t seedOf(const ExecContext& c) { return uint32_t(c.i("seed")); }

struct Reports
{
    std::mutex mutex;
    std::map<std::pair<uint64_t, std::string>, nlohmann::json> map;
};

Reports& reports()
{
    static Reports r;
    return r;
}

std::vector<gn::NodeDecl> decls()
{
    std::vector<gn::NodeDecl> d;
    const AnalyticParams ap;
    d.push_back(simple("TerrainAnalyticErosion", "Analytic Erosion",
                       "Stream-power erosion solved for an age instead of stepped: valleys cut back from the edge as age grows, "
                       "toward the steady state where uplift and erosion balance. Every valley drains. One drainage pass, so it is "
                       "fast at any age. Keeps the height range. Writes flow, lakes, wear and deposit.",
                       {num("age", Socket::Float, ap.age, "0 is the input, 1 the steady state almost everywhere.", 0.0, 4.0),
                        num("strength", Socket::Float, ap.strength, "Erodibility; the hardness mask slows it.", 0.0),
                        num("areaExponent", Socket::Float, ap.areaExponent, "How much bigger rivers cut faster (0.3 to 0.7).", 0.1, 1.0),
                        num("hardnessMask", Socket::String, "", "Mask of hard rock (1) that erodes slower."),
                        num("upliftMask", Socket::String, "", "Mask scaling the uplift that builds ranges; empty is uniform."),
                        num("keepRange", Socket::Bool, true, "Rescale to the input's height range.")},
                       [](ExecContext& c, Heightfield& h) {
                           AnalyticParams p;
                           p.age = c.f("age");
                           p.strength = c.f("strength");
                           p.areaExponent = c.f("areaExponent");
                           p.hardnessMask = c.s("hardnessMask");
                           p.upliftMask = c.s("upliftMask");
                           p.keepRange = c.b("keepRange");
                           erodeAnalytic(h, p);
                       }));
    const AmplifyParams am;
    d.push_back(simple("TerrainAmplify", "Amplify Terrain",
                       "More samples over the same area, keeping the drainage: the heights upsampled smoothly, detail added where "
                       "the ground slopes, then eroded at the new scale so the detail runs along the valleys.",
                       {num("factor", Socket::Int, am.factor, "Samples per input cell along each side.", 1, 8),
                        num("detail", Socket::Float, am.detail, "Detail height, as a fraction of the drop over one input cell.", 0.0),
                        num("age", Socket::Float, am.age, "Age of the fine erosion.", 0.0, 4.0), num("seed", Socket::Int, 0, "Random seed.")},
                       [](ExecContext& c, Heightfield& h) {
                           AmplifyParams p;
                           p.factor = int(std::clamp<int64_t>(c.i("factor"), 1, 8));
                           p.detail = c.f("detail");
                           p.age = c.f("age");
                           p.seed = seedOf(c);
                           if (int64_t(h.nx - 1) * p.factor + 1 > 16385 || int64_t(h.nz - 1) * p.factor + 1 > 16385)
                               throw gn::ExecError(c.node.name + ": the result would pass 16385 samples a side; lower factor");
                           h = amplified(h, p);
                       }));
    const DebrisParams dp;
    d.push_back(simple("TerrainDebrisFlow", "Debris Flow",
                       "Steep gullies with small catchments scour and their material runs out, settling as cones where the "
                       "ground eases. Writes wear, deposit and talus.",
                       {num("strength", Socket::Float, dp.strength, "How much is scoured.", 0.0),
                        num("scourAngle", Socket::Float, dp.scourAngle, "Steeper than this scours, degrees.", 1.0, 89.0),
                        num("settleAngle", Socket::Float, dp.settleAngle, "Gentler than this, the flow settles, degrees.", 0.5, 89.0),
                        num("maxArea", Socket::Float, dp.maxArea, "Catchments larger than this are rivers, m2.", 1.0),
                        num("iterations", Socket::Int, dp.iterations, "Passes.", 1, 50)},
                       [](ExecContext& c, Heightfield& h) {
                           DebrisParams p;
                           p.strength = c.f("strength");
                           p.scourAngle = c.f("scourAngle");
                           p.settleAngle = c.f("settleAngle");
                           p.maxArea = c.f("maxArea");
                           p.iterations = int(std::clamp<int64_t>(c.i("iterations"), 1, 50));
                           debrisFlow(h, p);
                       }));
    const DropletParams wp;
    d.push_back(simple("TerrainHydraulic", "Hydraulic Erosion",
                       "Rills from many water drops: each runs downhill, picks up material where it speeds up and drops it where "
                       "it slows. Writes wet (where water ran), wear and deposit.",
                       {num("density", Socket::Float, wp.density, "Drops per sample.", 0.0, 10.0),
                        num("maxSteps", Socket::Int, wp.maxSteps, "Longest run of a drop, in samples.", 1, 120),
                        num("inertia", Socket::Float, wp.inertia, "How much a drop keeps its direction, 0 to 1.", 0.0, 0.99),
                        num("capacity", Socket::Float, wp.capacity, "How much a drop can carry.", 0.0),
                        num("erodeRate", Socket::Float, wp.erodeRate, "How fast it picks up, 0 to 1.", 0.0, 1.0),
                        num("depositRate", Socket::Float, wp.depositRate, "How fast it drops, 0 to 1.", 0.0, 1.0),
                        num("evaporation", Socket::Float, wp.evaporation, "Water lost per step, 0 to 1.", 0.0, 1.0),
                        num("radius", Socket::Int, wp.radius, "Erosion footprint, samples.", 1, 4),
                        num("strength", Socket::Float, wp.strength, "Scales what drops cut and lay down.", 0.0),
                        num("seed", Socket::Int, 0, "Random seed.")},
                       [](ExecContext& c, Heightfield& h) {
                           DropletParams p;
                           p.density = c.f("density");
                           p.maxSteps = int(std::clamp<int64_t>(c.i("maxSteps"), 1, 120));
                           p.inertia = c.f("inertia");
                           p.capacity = c.f("capacity");
                           p.erodeRate = c.f("erodeRate");
                           p.depositRate = c.f("depositRate");
                           p.evaporation = c.f("evaporation");
                           p.radius = int(std::clamp<int64_t>(c.i("radius"), 1, 4));
                           p.strength = c.f("strength");
                           p.seed = seedOf(c);
                           droplets(h, p);
                       }));
    const DepositionParams sp;
    d.push_back(simple("TerrainDeposition", "Deposition",
                       "Material weathered off slopes rides the drainage and settles where the flow slows: fans at valley mouths, "
                       "filled valley floors and lakes. Writes sediment (depth laid down), deposit and wear.",
                       {num("supply", Socket::Float, sp.supply, "Weathered off a slope of 1 per pass, cm.", 0.0),
                        num("transport", Socket::Float, sp.transport, "How much the flow carries.", 0.0),
                        num("areaExponent", Socket::Float, sp.areaExponent, "How much more big rivers carry.", 0.1, 1.0),
                        num("depositRate", Socket::Float, sp.depositRate, "Share of the excess dropped per sample.", 0.0, 1.0),
                        num("fillLakes", Socket::Bool, sp.fillLakes, "Fill lakes with what reaches them."),
                        num("hardnessMask", Socket::String, "", "Mask of hard rock (1) that weathers slower."),
                        num("iterations", Socket::Int, sp.iterations, "Passes.", 1, 50)},
                       [](ExecContext& c, Heightfield& h) {
                           DepositionParams p;
                           p.supply = c.f("supply");
                           p.transport = c.f("transport");
                           p.areaExponent = c.f("areaExponent");
                           p.depositRate = c.f("depositRate");
                           p.fillLakes = c.b("fillLakes");
                           p.hardnessMask = c.s("hardnessMask");
                           p.iterations = int(std::clamp<int64_t>(c.i("iterations"), 1, 50));
                           deposition(h, p);
                       }));
    const SnowParams np;
    d.push_back(simple("TerrainSnow", "Snow",
                       "Snow above the snowline on slopes that hold it, deeper in hollows, sliding off steep ground. Writes snow; "
                       "Add Depth also raises the ground by it.",
                       {num("autoSnowline", Socket::Bool, true, "Snowline at the 60th percentile height."),
                        num("snowline", Socket::Float, 0.0, "Height where snow starts, cm."),
                        num("band", Socket::Float, np.band, "Height from no cover to full, cm; 0 is a tenth of the relief.", 0.0),
                        num("maxSlope", Socket::Float, np.maxSlope, "Steepest slope that holds snow, degrees.", 1.0, 89.0),
                        num("hollows", Socket::Float, np.hollows, "Extra snow in hollows, 0 to 1.", 0.0, 1.0),
                        num("depth", Socket::Float, np.depth, "Depth at full cover, cm.", 1.0),
                        num("addDepth", Socket::Bool, np.addDepth, "Raise the ground by the snow."),
                        num("slide", Socket::Int, np.slide, "Slide steps.", 0, 200)},
                       [](ExecContext& c, Heightfield& h) {
                           SnowParams p;
                           if (!c.b("autoSnowline"))
                               p.snowline = c.f("snowline");
                           p.band = c.f("band");
                           p.maxSlope = c.f("maxSlope");
                           p.hollows = c.f("hollows");
                           p.depth = c.f("depth");
                           p.addDepth = c.b("addDepth");
                           p.slide = int(std::clamp<int64_t>(c.i("slide"), 0, 200));
                           snow(h, p);
                       }));
    const ErosionFilterParams fp;
    d.push_back(simple(
        "TerrainErosionFilter", "Erosion Filter",
        "Eroded-looking gullies per sample, without simulating: branching channels down every slope, deepest where it is "
        "steepest. Instant and the same across tiles, for previews, distant terrain and endless worlds. Writes wear.",
        {num("scale", Socket::Float, fp.scale, "Spacing of the largest gullies, cm.", 1.0), num("octaves", Socket::Int, fp.octaves, "Levels of branching.", 1, 10),
         num("strength", Socket::Float, fp.strength, "Gully depth as a fraction of their spacing.", 0.0),
         num("lacunarity", Socket::Float, fp.lacunarity, "Spacing ratio between levels.", 1.1, 4.0),
         num("gain", Socket::Float, fp.gain, "Depth ratio between levels.", 0.0, 1.0),
         num("slopeFull", Socket::Float, fp.slopeFull, "Slope (rise over run) where gullies reach full depth.", 0.01),
         num("seed", Socket::Int, 0, "Random seed.")},
        [](ExecContext& c, Heightfield& h) {
            ErosionFilterParams p;
            p.scale = c.f("scale");
            p.octaves = int(std::clamp<int64_t>(c.i("octaves"), 1, 10));
            p.strength = c.f("strength");
            p.lacunarity = c.f("lacunarity");
            p.gain = c.f("gain");
            p.slopeFull = c.f("slopeFull");
            p.seed = seedOf(c);
            if (wantsGpu(c))
            {
                std::string err;
                if (erosionFilterGpu(h, p, &err))
                    return;
                gpuFailed(c, err);
            }
            erosionFilter(h, p);
        },
        {deviceProp()}));

    const TerraceParams tp;
    d.push_back(simple("TerrainTerrace", "Terrace", "Steps the ground into flat treads and risers every step cm.",
                       {num("step", Socket::Float, tp.step, "Step height, cm.", 1.0), num("sharpness", Socket::Float, tp.sharpness, "0 soft to 1 sheer risers.", 0.0, 1.0),
                        num("offset", Socket::Float, tp.offset, "Height of a tread, cm."), num("mask", Socket::String, "", "Mask scaling it; empty is everywhere."),
                        num("strength", Socket::Float, tp.strength, "0 to 1.", 0.0, 1.0)},
                       [](ExecContext& c, Heightfield& h) {
                           TerraceParams p;
                           p.step = c.f("step");
                           p.sharpness = c.f("sharpness");
                           p.offset = c.f("offset");
                           p.mask = c.s("mask");
                           p.strength = c.f("strength");
                           terrace(h, p);
                       }));
    const StrataParams rp;
    d.push_back(simple("TerrainStrata", "Strata",
                       "Rock layers tilted across the terrain: hard bands stand out as benches, and the strata mask (1 hard) "
                       "feeds erosion's hardness.",
                       {num("thickness", Socket::Float, rp.thickness, "Layer thickness, cm.", 1.0), num("dip", Socket::Float, rp.dip, "Tilt, degrees.", -80.0, 80.0),
                        num("dipDirection", Socket::Float, rp.dipDirection, "Tilt direction, degrees from +X toward +Z."),
                        num("strength", Socket::Float, rp.strength, "How far hard bands stand out, as a fraction of the thickness.", 0.0),
                        num("noise", Socket::Float, rp.noise, "Waviness, as a fraction of the thickness.", 0.0), num("seed", Socket::Int, 0, "Random seed.")},
                       [](ExecContext& c, Heightfield& h) {
                           StrataParams p;
                           p.thickness = c.f("thickness");
                           p.dip = c.f("dip");
                           p.dipDirection = c.f("dipDirection");
                           p.strength = c.f("strength");
                           p.noise = c.f("noise");
                           p.seed = seedOf(c);
                           strata(h, p);
                       }));
    const FoldParams fo;
    d.push_back(simple("TerrainFold", "Fold", "Buckles the ground into parallel ridges with bent crests.",
                       {num("wavelength", Socket::Float, fo.wavelength, "Ridge spacing, cm.", 1.0), num("amplitude", Socket::Float, fo.amplitude, "Height, cm."),
                        num("direction", Socket::Float, fo.direction, "Degrees from +X toward +Z the folds run across."),
                        num("bend", Socket::Float, fo.bend, "Crest bend, as a fraction of the wavelength.", 0.0), num("seed", Socket::Int, 0, "Random seed.")},
                       [](ExecContext& c, Heightfield& h) {
                           FoldParams p;
                           p.wavelength = c.f("wavelength");
                           p.amplitude = c.f("amplitude");
                           p.direction = c.f("direction");
                           p.bend = c.f("bend");
                           p.seed = seedOf(c);
                           fold(h, p);
                       }));
    const WarpParams wa;
    d.push_back(simple("TerrainWarp", "Warp Terrain", "Pulls heights and masks sideways by noise, so straight features meander.",
                       {num("amount", Socket::Float, wa.amount, "How far, cm.", 0.0), num("featureSize", Socket::Float, wa.featureSize, "Size of the swirls, cm.", 1.0),
                        num("seed", Socket::Int, 0, "Random seed.")},
                       [](ExecContext& c, Heightfield& h) {
                           WarpParams p;
                           p.amount = c.f("amount");
                           p.featureSize = c.f("featureSize");
                           p.seed = seedOf(c);
                           warp(h, p);
                       }));
    const RidgeSharpenParams rs;
    d.push_back(simple("TerrainRidgeSharpen", "Ridge Sharpen", "Pushes up ground that stands above its surroundings, so crests read sharp.",
                       {num("amount", Socket::Float, rs.amount, "How much.", 0.0), num("radius", Socket::Float, rs.radius, "Surroundings radius, cm.", 1.0)},
                       [](ExecContext& c, Heightfield& h) {
                           RidgeSharpenParams p;
                           p.amount = c.f("amount");
                           p.radius = c.f("radius");
                           ridgeSharpen(h, p);
                       }));
    const CliffParams cp;
    d.push_back(simple("TerrainCliff", "Cliff", "Ground steeper than the threshold becomes sheer rock bands. Writes cliff.",
                       {num("threshold", Socket::Float, cp.threshold, "Slope where cliffs start, degrees.", 1.0, 89.0),
                        num("step", Socket::Float, cp.step, "Rock band height, cm.", 1.0), num("strength", Socket::Float, cp.strength, "0 to 1.", 0.0, 1.0),
                        num("seed", Socket::Int, 0, "Random seed.")},
                       [](ExecContext& c, Heightfield& h) {
                           CliffParams p;
                           p.threshold = c.f("threshold");
                           p.step = c.f("step");
                           p.strength = c.f("strength");
                           p.seed = seedOf(c);
                           cliff(h, p);
                       }));

    d.push_back({.type = "TerrainErodePreset",
                 .label = "Erode Preset",
                 .category = kLandCategory,
                 .doc = "The whole erosion chain as one age: macro form (analytic stream power at low resolution), amplified back "
                        "keeping its drainage, debris flow and scree, rills, then deposition (and snow on alpine). Age 0 leaves the "
                        "ground alone. Writes flow, lakes, wear, deposit, sediment, wet and talus (and snow).",
                 .props = {deviceProp()},
                 .sockets = {.inputs = {hfIn(), num("preset", Socket::String, "natural", "natural, alpine, desert or hills."),
                                        num("age", Socket::Float, 0.5, "0 is off, 1 is very old.", 0.0, 2.0), num("seed", Socket::Int, 0, "Random seed."),
                                        num("hardnessMask", Socket::String, "", "Mask of hard rock (1)."),
                                        num("keepForm", Socket::Bool, false, "Keep landforms wider than formRadius as they are (real elevation)."),
                                        num("formRadius", Socket::Float, 30000.0, "Widest landform the chain may change, cm.", 100.0)},
                             .outputs = {hfOut()}},
                 .exec = [](ExecContext& c) {
                     LILA_ZONE();
                     GeometryPtr g;
                     Heightfield h = takeHeightfield(c, g);
                     PresetParams p;
                     p.preset = c.s("preset");
                     if (!isErodePreset(p.preset))
                         throw gn::ExecError(c.node.name + ": unknown preset '" + p.preset + "' (natural, alpine, desert, hills)");
                     p.age = c.f("age");
                     p.seed = seedOf(c);
                     p.hardnessMask = c.s("hardnessMask");
                     p.keepForm = c.b("keepForm");
                     p.formRadius = c.f("formRadius");
                     p.gpu = wantsGpu(c);
                     PresetReport report;
                     erodePreset(h, p, &report);
                     for (const std::string& f : report.fallbacks)
                         gpuFailed(c, f);
                     nlohmann::json r = report.toJson();
                     r["preset"] = p.preset;
                     r["age"] = p.age;
                     publishPresetReport(c.env.owner, c.node.name, std::move(r));
                     put(c, g, std::move(h));
                 }});
    return d;
}
} // namespace lnodes_

void publishPresetReport(uint64_t modifier, const std::string& node, nlohmann::json report)
{
    lnodes_::Reports& r = lnodes_::reports();
    std::lock_guard lock(r.mutex);
    r.map[{modifier, node}] = std::move(report);
}

nlohmann::json presetReport(uint64_t modifier, const std::string& node)
{
    lnodes_::Reports& r = lnodes_::reports();
    std::lock_guard lock(r.mutex);
    auto it = r.map.find({modifier, node});
    return it == r.map.end() ? nlohmann::json() : it->second;
}

void runThermalNode(gn::ExecContext& c, Heightfield& h)
{
    const double base = c.f("talusAngle");
    const int iterations = int(std::clamp<int64_t>(c.i("iterations"), 0, 10000));
    std::vector<std::pair<std::string, double>> materials;
    if (const std::string m = c.s("materialMask"); !m.empty())
    {
        if (!h.findMask(m))
            throw gn::ExecError(c.node.name + ": no mask '" + m + "' on the heightfield");
        materials.push_back({m, c.f("materialAngle")});
    }
    const std::vector<float> angles = talusAngles(h, base, materials);
    if (lnodes_::wantsGpu(c))
    {
        std::string err;
        if (thermalMaterialGpu(h, angles, iterations, &err))
            return;
        lnodes_::gpuFailed(c, err);
    }
    thermalMaterial(h, angles, iterations);
}

bool registerLandformNodes(ext::Context& ctx)
{
    for (gn::NodeDecl d : lnodes_::decls())
        if (!gn::registerDecl(ctx, std::move(d)))
            return false;
    return true;
}
} // namespace terrain
