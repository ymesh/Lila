#include "tex/TextureSet.h"

#include "bake/Bake.h"
#include "bake/BakeNodes.h"
#include "bake/BakePipeline.h"
#include "bake/HeightNormal.h"
#include "bake/MapMath.h"
#include "bake/Maps.h"
#include "bake/MapsInternal.h"
#include "bake/Naming.h"
#include "bake/Targets.h"
#include "core/ImageIO.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "core/Workspace.h"
#include "scene/Anim.h"
#include "scene/Node.h"
#include "tex/Evaluate.h"
#include "tex/Generators.h"
#include "tex/Layers.h"
#include "tex/Sequence.h"
#include "tex/ShadingChannels.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <set>
#include <stdexcept>

namespace lila::tex
{
namespace fs = std::filesystem;

namespace
{
int bitsFor(const std::string& format, int bits)
{
    if (format == "png" || format == "psd")
        return bits >= 16 ? 16 : 8;
    if (format == "exr")
        return bits >= 32 ? 32 : 16;
    if (format == "tga")
        return 8;
    return bits;
}

/** Integer files hold colour maps sRGB-encoded, as every importer expects; float files (EXR, 32-bit TIFF) stay linear. */
bool encodesSrgb(const std::string& format, int bits) { return format != "exr" && bits <= 16; }

void encodeSrgb(image::Image& img)
{
    for (size_t i = 0; i < img.rgba.size(); ++i)
        if (i % 4 != 3)
            img.rgba[i] = bake::mapmath::linearToSrgb(std::clamp(img.rgba[i], 0.0f, 1.0f));
}

/** Colour channels other than normals: graphs make them linear. */
bool isColourChannel(const std::string& name, const Channel* ch)
{
    return ch && ch->type == PortType::Color && name.find("normal") == std::string::npos && name.find("Normal") == std::string::npos;
}

ExternalSource& externalSource()
{
    static ExternalSource fn;
    return fn;
}

uint64_t mixBytes(uint64_t h, const void* data, size_t n)
{
    const auto* p = static_cast<const unsigned char*>(data);
    h ^= n * 0x9E3779B97F4A7C15ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
    {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h ^= w * 0xBF58476D1CE4E5B9ull;
        h = std::rotl(h, 31) * 0x94D049BB133111EBull;
    }
    for (; i < n; ++i)
        h = (h ^ p[i]) * 0x100000001B3ull;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    return h;
}

uint64_t mix(uint64_t h, uint64_t v) { return mixBytes(h, &v, sizeof v); }
uint64_t mix(uint64_t h, std::string_view s) { return mixBytes(h, s.data(), s.size()); }
std::string hex(uint64_t h) { return std::format("{:016x}", h); }

/** Size and modification time: what changes when a file is rewritten. */
uint64_t fileStamp(const fs::path& p)
{
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    if (ec)
        return 0;
    const auto time = fs::last_write_time(p, ec);
    return mix(uint64_t(size), uint64_t(time.time_since_epoch().count()));
}

struct External
{
    image::Image image;
    uint64_t hash = 0;
};

/** The images a plan reads from outside: paint buffers through the external source, mesh maps from the set's bake project (live, else its files). */
std::map<std::string, External> gatherExternals(scene::Node& set, const SetSettings& s, const Plan& plan, int udim)
{
    std::map<std::string, External> out;
    for (const BufferDesc& b : plan.buffers)
    {
        if (b.kind != BufferKind::External || out.contains(b.name))
            continue;
        External e;
        if (externalSource())
            if (image::Image img = externalSource()(set, b.name, udim); img.width > 0)
            {
                e.hash = mixBytes(mix(1, b.name), img.rgba.data(), img.rgba.size() * sizeof(float));
                e.image = std::move(img);
            }
        if (e.image.width <= 0)
            if (image::Image img = readLiveMeshMap(s.bakeProject, b.name, udim); img.width > 0)
            {
                e.hash = mixBytes(mix(3, b.name), img.rgba.data(), img.rgba.size() * sizeof(float));
                e.image = std::move(img);
            }
        if (e.image.width <= 0)
            if (const fs::path file = bakedMapFile(scene::Graph::current(), s.bakeProject, bakeMapName(b.name), udim); !file.empty())
                if (image::Image img = image::read(file); img.width > 0)
                {
                    e.hash = mix(mix(2, paths::toUtf8(file)), fileStamp(file));
                    e.image = std::move(img);
                }
        out.emplace(b.name, std::move(e));
    }
    return out;
}

EvalOptions evalOptionsFor(const std::map<std::string, External>& externals, const Plan& plan, int udim)
{
    EvalOptions opts;
    auto [u, v] = udimOrigin(udim);
    opts.uvOffset[0] = float(u);
    opts.uvOffset[1] = float(v);
    for (const BufferDesc& b : plan.buffers)
        if (b.kind == BufferKind::External)
            if (auto it = externals.find(b.name); it != externals.end() && it->second.image.width > 0)
                opts.externals[b.name] = it->second.image;
    return opts;
}

/** Everything a plan's result depends on: its structure, its values, its size and tile, and the images it reads. */
uint64_t planKey(const Plan& p, const std::map<std::string, External>& externals, int udim)
{
    uint64_t h = mix(p.structureHash, uint64_t(udim));
    h = mix(mix(h, uint64_t(p.width)), uint64_t(p.height));
    h = mixBytes(h, p.uniforms.data(), p.uniforms.size() * sizeof(float));
    for (const BufferDesc& b : p.buffers)
    {
        if (b.kind == BufferKind::External)
            if (auto it = externals.find(b.name); it != externals.end())
                h = mix(mix(h, b.name), it->second.hash);
        if (b.kind == BufferKind::Image)
            h = mix(mix(h, b.name), fileStamp(resolveImagePath(b.name, {})));
    }
    return h;
}

struct TileResult
{
    Tile tile;
    EvalResult eval;
    Plan plan;
};

/** Evaluates every requested channel of every tile. */
std::vector<TileResult> evaluateTiles(scene::Node& set, const Graph& stored, const SetSettings& s, const std::vector<std::string>& channels, int width,
                                      int height, const std::string& backend = {}, std::string* used = nullptr)
{
    Graph g = stored;
    followLiveFrame(g, s);
    sequence::followTime(g, scene::Graph::current(), set, scene::Graph::current().time());
    std::vector<OutputRequest> requests;
    for (const OutputRequest& r : channelRequests(g, s))
        if (channels.empty() || std::find(channels.begin(), channels.end(), r.name) != channels.end())
            requests.push_back(r);
    if (requests.empty())
        throw std::invalid_argument(g.firstOfType(kMaterialOutputType) ? "no such channels" : "the graph has no Material Output");
    std::vector<TileResult> out;
    for (const Tile& t : s.tiles)
    {
        auto [w, h] = s.tileSize(t);
        PlanRequest req;
        req.width = width > 0 ? width : w;
        req.height = height > 0 ? height : h;
        req.outputs = requests;
        TileResult tr{t, {}, buildPlan(g, req)};
        if (!tr.plan.error.empty())
            throw std::invalid_argument(tr.plan.error);
        const auto externals = gatherExternals(set, s, tr.plan, t.udim);
        tr.eval = evaluate(tr.plan, evalOptionsFor(externals, tr.plan, t.udim), backend, used);
        out.push_back(std::move(tr));
    }
    return out;
}

json warningsOf(const std::vector<TileResult>& tiles)
{
    json w = json::array();
    for (const TileResult& t : tiles)
    {
        for (const std::string& s : t.plan.warnings)
            w.push_back(s);
        for (const std::string& s : t.eval.warnings)
            w.push_back(s);
    }
    return w;
}

/** Where the hashes of a set's last export into a folder are kept: local state, never next to the textures. */
fs::path exportManifestPath(const fs::path& folder, const std::string& set, const std::string& preset)
{
    const fs::path base = Workspace::get().isOpen() ? Workspace::get().stateDir() : paths::userConfigDir();
    return base / "cache" / "texexport" / (hex(mix(0, paths::toUtf8(folder.lexically_normal()))) + "_" + bake::fileSafe(set) + "_" + preset + ".json");
}

json readJsonFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return json::object();
    json j = json::parse(f, nullptr, false);
    return j.is_object() ? j : json::object();
}

/** One file an export preset writes per tile. */
struct MapJob
{
    std::string map;
    std::vector<bake::PackChannel> layout;
    bool flipGreen = false;
    std::string suffix;
    std::string format;
    int bits = 8;
    bool linear = true;
};
} // namespace

fs::path bakedMapFile(scene::Graph& g, const std::string& project, const std::string& map, int udim)
{
    if (project.empty())
        return {};
    for (scene::NodeId id : bake::projects(g))
    {
        scene::Node* n = g.node(id);
        if (!n || n->name() != project)
            continue;
        json r = bake::lastResult(id);
        if (!r.is_object() || !r.contains("outputs"))
            r = bake::readManifest(*n);
        if (!r.is_object())
            return {};
        fs::path any;
        int count = 0;
        for (const json& o : r.value("outputs", json::array()))
        {
            if (o.value("map", std::string()) != map || !o.value("layer", std::string()).empty())
                continue;
            const fs::path file = paths::fromUtf8(o.value("file", std::string()));
            if (o.value("udim", 1001) == udim)
                return file;
            any = file;
            ++count;
        }
        // A single-tile bake feeds every tile of a set that has more.
        return count == 1 ? any : fs::path();
    }
    return {};
}

json bakedPositionMeta(scene::Graph& g, const std::string& project)
{
    if (project.empty())
        return json();
    for (scene::NodeId id : bake::projects(g))
    {
        scene::Node* n = g.node(id);
        if (!n || n->name() != project)
            continue;
        json r = bake::lastResult(id);
        if (!r.is_object() || !r.contains("outputs"))
            r = bake::readManifest(*n);
        if (!r.is_object())
            return json();
        for (const json& o : r.value("outputs", json::array()))
        {
            const json meta = o.value("meta", json::object());
            if (o.value("map", std::string()) != "position" || !meta.is_object())
                continue;
            const json a = meta.value("boundsMin", json()), b = meta.value("boundsMax", json());
            if (a.is_array() && b.is_array() && a.size() == 3 && b.size() == 3)
                return meta;
        }
        return json();
    }
    return json();
}

json bakedMapMeta(scene::Graph& g, const std::string& project, const std::string& map)
{
    if (project.empty())
        return json();
    if (json live = liveMeshMapMeta(project, map); live.is_object() && !live.empty())
        return live;
    for (scene::NodeId id : bake::projects(g))
    {
        scene::Node* n = g.node(id);
        if (!n || n->name() != project)
            continue;
        json r = bake::lastResult(id);
        if (!r.is_object() || !r.contains("outputs"))
            r = bake::readManifest(*n);
        if (!r.is_object())
            return json();
        for (const json& o : r.value("outputs", json::array()))
            if (o.value("map", std::string()) == map && o.value("meta", json()).is_object())
                return o["meta"];
        return json();
    }
    return json();
}

json positionFrame(scene::Graph& g, const std::string& project)
{
    json live = liveMeshMapMeta(project, "position");
    const json a = live.is_object() ? live.value("boundsMin", json()) : json(), b = live.is_object() ? live.value("boundsMax", json()) : json();
    if (a.is_array() && b.is_array() && a.size() == 3 && b.size() == 3)
        return live;
    return bakedPositionMeta(g, project);
}

bool bakedPositionBounds(scene::Graph& g, const std::string& project, double lo[3], double hi[3])
{
    const json meta = bakedPositionMeta(g, project);
    if (meta.is_null())
        return false;
    for (size_t k = 0; k < 3; ++k)
    {
        lo[k] = meta["boundsMin"][k].get<double>();
        hi[k] = meta["boundsMax"][k].get<double>();
    }
    return true;
}

void setExternalSource(ExternalSource fn) { externalSource() = std::move(fn); }

std::map<std::string, image::Image> evaluateOutputs(scene::Node& set, const std::vector<OutputRequest>& requests, int udim, int width, int height,
                                                    const std::string& backend, std::vector<std::string>* warnings)
{
    return evaluateGraphOutputs(set, readGraph(set), requests, udim, width, height, backend, warnings);
}

std::map<std::string, image::Image> evaluateGraphOutputs(scene::Node& set, const Graph& stored, const std::vector<OutputRequest>& requests, int udim,
                                                         int width, int height, const std::string& backend, std::vector<std::string>* warnings)
{
    LILA_ZONE();
    SetSettings s = readSettings(set);
    Graph g = stored;
    followLiveFrame(g, s);
    sequence::followTime(g, scene::Graph::current(), set, scene::Graph::current().time());
    const Tile* tile = &s.tiles.front();
    for (const Tile& t : s.tiles)
        if (t.udim == udim)
            tile = &t;
    auto [tw, th] = s.tileSize(*tile);
    PlanRequest req;
    req.width = width > 0 ? width : tw;
    req.height = height > 0 ? height : th;
    req.outputs = requests;
    const Plan plan = buildPlan(g, req);
    if (!plan.error.empty())
        throw std::invalid_argument(plan.error);
    const auto externals = gatherExternals(set, s, plan, tile->udim);
    EvalResult r = evaluate(plan, evalOptionsFor(externals, plan, tile->udim), backend);
    if (warnings)
    {
        warnings->insert(warnings->end(), plan.warnings.begin(), plan.warnings.end());
        warnings->insert(warnings->end(), r.warnings.begin(), r.warnings.end());
    }
    return std::move(r.outputs);
}

json evaluateSet(scene::Node& set, const EvaluateOptions& options)
{
    LILA_ZONE();
    Graph g = readGraph(set);
    SetSettings s = readSettings(set);
    std::string backend;
    std::vector<TileResult> tiles = evaluateTiles(set, g, s, options.channels, options.width, options.height, options.backend, &backend);
    fs::path folder = bake::resolveFolder(options.folder.empty() ? "textures/" + set.name() : options.folder);
    std::error_code ec;
    fs::create_directories(folder, ec);
    json files = json::array();
    std::vector<fs::path> written;
    double ms = 0.0;
    size_t compiled = 0, cached = 0, passes = 0;
    for (const TileResult& t : tiles)
    {
        ms += t.eval.milliseconds;
        compiled += t.eval.compiled;
        cached += t.eval.cached;
        passes += t.plan.passes.size();
        for (const auto& [name, img] : t.eval.outputs)
        {
            const Channel* ch = s.channel(name);
            std::string file = bake::fileSafe(set.name()) + "_" + name + (s.tiles.size() > 1 ? "." + std::to_string(t.tile.udim) : std::string()) + "." +
                               options.format;
            image::WriteOptions wo;
            wo.bits = bitsFor(options.format, options.bits > 0 ? options.bits : (ch ? ch->bits : 8));
            bake::prepareOutputs({folder / file});
            if (isColourChannel(name, ch) && encodesSrgb(options.format, wo.bits))
            {
                image::Image encoded = img;
                encodeSrgb(encoded);
                image::write(folder / file, encoded, wo);
            }
            else
                image::write(folder / file, img, wo);
            written.push_back(folder / file);
            files.push_back(paths::toUtf8(folder / file));
        }
    }
    bake::addOutputs(written);
    return {{"files", files}, {"passes", passes}, {"milliseconds", ms}, {"compiled", compiled}, {"cached", cached}, {"warnings", warningsOf(tiles)}, {"backend", backend}};
}

json exportFlipbook(scene::Node& set, const FlipbookOptions& options)
{
    LILA_ZONE();
    const SetSettings s = readSettings(set);
    const Graph g = readGraph(set);
    if (!(options.step > 0.0) || options.endFrame < options.startFrame)
        throw std::invalid_argument("the frame range is empty");
    std::vector<double> frames;
    for (double f = options.startFrame; f <= options.endFrame + 1e-6; f += options.step)
        frames.push_back(f);
    if (frames.size() > 4096)
        throw std::invalid_argument(std::to_string(frames.size()) + " frames is more than a sheet holds (4096)");
    const int count = int(frames.size());
    const int columns = options.columns > 0 ? std::min(options.columns, count) : int(std::ceil(std::sqrt(double(count))));
    const int rows = (count + columns - 1) / columns;
    const auto [tw, th] = s.tileSize(s.tiles.front());
    const int cw = std::max(4, options.cellSize);
    const int ch = std::max(4, int(std::lround(double(cw) * th / std::max(tw, 1))));
    if (size_t(cw) * columns > 16384 || size_t(ch) * rows > 16384)
        throw std::invalid_argument("the sheet would be " + std::to_string(cw * columns) + " x " + std::to_string(ch * rows) +
                                    ": use a smaller cell or fewer frames");

    // Time moves without notifying anyone and comes back, so keyed parameters and sequences read each frame.
    scene::Graph& scene = scene::Graph::current();
    struct RestoreTime
    {
        scene::Graph& g;
        double seconds;
        ~RestoreTime() { g.setTime(seconds, false); }
    } restore{scene, scene.time()};

    std::map<std::string, image::Image> sheets;
    json warnings = json::array();
    std::string backend;
    for (int i = 0; i < count; ++i)
    {
        scene.setTime(scene::anim::secondsOf(frames[size_t(i)]), false);
        std::vector<TileResult> tiles = evaluateTiles(set, g, s, options.channels, cw, ch, options.backend, &backend);
        if (tiles.empty())
            throw std::invalid_argument("no channels to export");
        if (i == 0)
            warnings = warningsOf(tiles);
        const int ox = (i % columns) * cw, oy = (i / columns) * ch;
        for (const auto& [name, img] : tiles.front().eval.outputs)
        {
            if (img.width != cw || img.height != ch)
                continue;
            image::Image& sheet = sheets[name];
            if (sheet.rgba.empty())
                sheet = image::Image{cw * columns, ch * rows, std::vector<float>(size_t(cw) * columns * size_t(ch) * rows * 4, 0.0f)};
            for (int y = 0; y < ch; ++y)
                std::memcpy(&sheet.rgba[(size_t(oy + y) * sheet.width + ox) * 4], &img.rgba[size_t(y) * cw * 4], size_t(cw) * 4 * sizeof(float));
        }
    }

    const fs::path folder = bake::resolveFolder(options.folder.empty() ? "textures/" + set.name() : options.folder);
    std::error_code ec;
    fs::create_directories(folder, ec);
    json files = json::array();
    std::vector<fs::path> written;
    for (auto& [name, sheet] : sheets)
    {
        const Channel* channel = s.channel(name);
        const fs::path file = folder / (bake::fileSafe(set.name()) + "_" + name + "_flipbook_" + std::to_string(columns) + "x" + std::to_string(rows) +
                                        "." + options.format);
        image::WriteOptions wo;
        wo.bits = bitsFor(options.format, options.bits > 0 ? options.bits : (channel ? channel->bits : 8));
        bake::prepareOutputs({file});
        if (isColourChannel(name, channel) && encodesSrgb(options.format, wo.bits))
            encodeSrgb(sheet);
        image::write(file, sheet, wo);
        written.push_back(file);
        files.push_back({{"file", paths::toUtf8(file)}, {"channel", name}});
    }
    bake::addOutputs(written);
    return {{"files", files}, {"columns", columns}, {"rows", rows}, {"frames", count}, {"fps", scene::anim::playback().fps / options.step},
            {"cell", {cw, ch}}, {"warnings", warnings}, {"backend", backend}};
}

json exportSet(scene::Node& set, const ExportOptions& options)
{
    LILA_ZONE();
    SetSettings s = readSettings(set);
    Graph g = readGraph(set);
    followLiveFrame(g, s);
    sequence::followTime(g, scene::Graph::current(), set, scene::Graph::current().time());
    const std::string presetName = options.preset.empty() ? s.exportPreset : options.preset;
    std::optional<bake::TargetProfile> target = bake::findTarget(presetName);
    if (!target)
        throw std::invalid_argument("no export preset (bake target) named " + presetName);
    std::vector<OutputRequest> requests = channelRequests(g, s);
    if (requests.empty())
        throw std::invalid_argument(g.firstOfType(kMaterialOutputType) ? "the set has no channels" : "the graph has no Material Output");

    std::string tmpl = target->output.value("name", std::string("T_{[!M_]material}_{map}"));
    const std::string format = target->output.value("format", std::string("png"));
    const int outBits = target->output.value("bits", 8);
    const std::string udimStyle = target->output.value("udimNaming", std::string("1001"));
    fs::path folder = bake::resolveFolder(options.folder.empty() ? std::string("textures") : options.folder);
    std::error_code ec;
    fs::create_directories(folder, ec);

    json warnings = json::array();
    if (options.needVariantToken && !bake::hasToken(tmpl, "variant"))
    {
        tmpl += "_{variant}";
        warnings.push_back("the " + presetName + " name template has no {variant}, so _{variant} was added to tell the variants apart");
    }
    // Normals from height: the normal goes out as detail over the height, which goes out with it at 16 bits.
    bool fromHeight = options.normalFromHeight.value_or(s.normalFromHeight);
    if (fromHeight && !s.channel("height"))
    {
        warnings.push_back("normalFromHeight: the set has no height channel, so the normal is written whole");
        fromHeight = false;
    }
    // A set rebuilding from the baked height exports that height: it is the one the material rebuilds from.
    if (fromHeight && s.heightSource == "mesh" && g.node("StackRebuildHeight"))
        for (OutputRequest& r : requests)
            if (r.name == "height")
                r = {"height", "StackRebuildHeight", "out", PortType::Gray, {}};
    std::vector<bake::TargetMap> targetMaps = target->maps;
    shading::addExportMaps(targetMaps, *target);
    if (fromHeight && std::ranges::none_of(targetMaps, [](const bake::TargetMap& tm) { return tm.settings.map == "height"; }))
    {
        bake::TargetMap tm;
        tm.settings.map = "height";
        targetMaps.push_back(tm);
    }
    std::vector<MapJob> jobs;
    for (const bake::TargetMap& tm : targetMaps)
    {
        const bake::MapSettings& ms = tm.settings;
        if (!ms.enabled || (tm.optional && !options.includeOptional && !(fromHeight && ms.map == "height")))
            continue;
        const bake::MapDef* def = bake::findMap(ms.map);
        MapJob job;
        job.map = ms.map;
        // Per output channel: the set channel it reads, which component, inverted, or a constant.
        if (def && !def->pack.is_null())
        {
            std::vector<std::string> w;
            job.layout = bake::packChannels(ms, *def, w);
            for (const std::string& x : w)
                warnings.push_back(x);
        }
        else
            for (int c = 0; c < 4; ++c)
                job.layout.push_back({ms.map, json::object(), false, c, false, c == 3 ? 1.0f : 0.0f});

        if (std::ranges::any_of(job.layout, [&](const bake::PackChannel& pc) { return !pc.map.empty() && !s.channel(pc.map); }))
        {
            if (!tm.optional)
                warnings.push_back(ms.map + ": the set has no channel for it, skipped");
            continue;
        }
        job.flipGreen = ms.map == "normal" && !ms.options.value("greenUp", false);
        job.suffix = !ms.suffix.empty() ? ms.suffix : def ? def->suffix : ms.map;
        job.format = ms.format.empty() ? format : ms.format;
        job.linear = !def || def->linear;
        const int asked = ms.bits > 0 ? ms.bits : outBits;
        int bits = asked;
        for (const bake::PackChannel& pc : job.layout)
            if (const Channel* ch = pc.map.empty() ? nullptr : s.channel(pc.map); ch && ms.bits <= 0)
                bits = std::max(bits, std::min(ch->bits, 16));
        if (fromHeight && ms.map == "height")
            bits = std::max(bits, 16);
        job.bits = bitsFor(job.format, bits);
        // Only a depth the preset asks for warns; a deeper channel is written at what the format holds.
        if (job.bits < asked)
            warnings.push_back(ms.map + ": " + job.format + " holds " + std::to_string(job.bits) + " bits, not " + std::to_string(asked) + "; use png, tif or exr for more");
        jobs.push_back(std::move(job));
    }
    // Only a normal map has a green convention; a reader flips the green of any map marked opengl.
    const auto normalFormatOf = [](const MapJob& j) { return j.map == "normal" ? (j.flipGreen ? "directx" : "opengl") : ""; };
    const auto splits = [&](const MapJob& j) {
        return fromHeight && j.map == "normal" && std::ranges::any_of(j.layout, [](const bake::PackChannel& pc) { return pc.map == "normal"; });
    };
    // What a reader needs to rebuild the normal: the height's depth (as a bake's heightRange) and the UV square's size.
    const json heightMeta = fromHeight ? json{{"heightRange", s.heightDepth * 0.5}, {"uvSize", {s.uvWidth, s.uvHeight}}, {"fade", s.normalFade}}
                                       : json::object();
    const auto metaOf = [&](const MapJob& j) {
        if (!fromHeight || (j.map != "normal" && j.map != "height"))
            return json::object();
        json m = heightMeta;
        if (j.map == "normal")
            m["normalFromHeight"] = true;
        return m;
    };

    const fs::path manifestPath = exportManifestPath(folder, set.name(), target->name);
    const json previous = readJsonFile(manifestPath).value("files", json::object());
    json next = json::object();
    json files = json::array(), outputs = json::array(), written = json::array(), unchanged = json::array();
    std::vector<fs::path> writtenPaths;
    std::string backend;

    for (const Tile& t : s.tiles)
    {
        auto [tw, th] = s.tileSize(t);
        PlanRequest all;
        all.width = options.width > 0 ? options.width : tw;
        all.height = options.height > 0 ? options.height : th;
        std::set<std::string> needed;
        for (const MapJob& j : jobs)
            for (const bake::PackChannel& pc : j.layout)
                if (!pc.map.empty())
                    needed.insert(pc.map);
        for (const OutputRequest& r : requests)
            if (needed.contains(r.name))
                all.outputs.push_back(r);
        if (all.outputs.empty())
            continue;
        const Plan full = buildPlan(g, all);
        if (!full.error.empty())
            throw std::invalid_argument(full.error);
        const auto externals = gatherExternals(set, s, full, t.udim);

        std::map<std::string, uint64_t> channelKeys;
        for (const OutputRequest& r : all.outputs)
        {
            PlanRequest one = all;
            one.outputs = {r};
            channelKeys[r.name] = planKey(buildPlan(g, one), externals, t.udim);
        }

        struct Pending
        {
            const MapJob* job;
            fs::path path;
            std::string key;
        };
        std::vector<Pending> dirty;
        for (const MapJob& j : jobs)
        {
            bake::NameTokens tokens;
            tokens.project = set.name();
            tokens.material = set.name();
            tokens.map = j.suffix;
            tokens.ext = j.format;
            tokens.udim = t.udim;
            tokens.udimStyle = udimStyle;
            tokens.width = all.width;
            tokens.height = all.height;
            tokens.variant = options.variant;
            const fs::path path = folder / bake::expandName(tmpl, tokens, s.tiles.size() > 1);
            const std::string file = paths::toUtf8(path);
            uint64_t key = mix(mix(mix(0, file), uint64_t(j.bits)), uint64_t(j.flipGreen));
            if (splits(j))
                key = mix(mix(key, channelKeys["height"]), heightMeta.dump());
            for (const bake::PackChannel& pc : j.layout)
            {
                key = pc.map.empty() ? mix(key, uint64_t(std::bit_cast<uint32_t>(pc.constant))) : mix(key, channelKeys[pc.map]);
                key = mix(mix(key, uint64_t(pc.channel)), uint64_t(pc.invert));
            }
            files.push_back(file);
            const json* before = previous.contains(file) ? &previous[file] : nullptr;
            if (options.incremental && before && before->value("key", std::string()) == hex(key) && fs::exists(path))
            {
                next[file] = *before;
                unchanged.push_back(file);
                outputs.push_back({{"file", file}, {"map", j.map}, {"udim", t.udim}, {"width", all.width}, {"height", all.height},
                                   {"format", j.format}, {"bits", j.bits}, {"colorSpace", j.linear ? "linear" : "srgb"},
                                   {"normalFormat", normalFormatOf(j)}, {"changed", false}, {"meta", metaOf(j)}});
                continue;
            }
            dirty.push_back({&j, path, hex(key)});
        }
        if (dirty.empty())
            continue;

        PlanRequest req = all;
        req.outputs.clear();
        const bool splitting = std::ranges::any_of(dirty, [&](const Pending& d) { return splits(*d.job); });
        for (const OutputRequest& r : all.outputs)
            if ((splitting && r.name == "height") || std::ranges::any_of(dirty, [&](const Pending& d) {
                    return std::ranges::any_of(d.job->layout, [&](const bake::PackChannel& pc) { return pc.map == r.name; });
                }))
                req.outputs.push_back(r);
        const Plan plan = buildPlan(g, req);
        if (!plan.error.empty())
            throw std::invalid_argument(plan.error);
        const EvalResult er = evaluate(plan, evalOptionsFor(externals, plan, t.udim), options.backend, &backend);
        for (const std::string& w : plan.warnings)
            warnings.push_back(w);
        for (const std::string& w : er.warnings)
            warnings.push_back(w);
        // The graph works in OpenGL normals; the job flips green afterwards.
        const image::Image detail = splitting ? bake::detailNormals(er.outputs.at("normal"), er.outputs.at("height"), s.heightSlope(), s.normalFade, true)
                                              : image::Image{};

        for (const Pending& d : dirty)
        {
            const MapJob& j = *d.job;
            const image::Image* any = nullptr;
            for (const bake::PackChannel& pc : j.layout)
                if (!pc.map.empty())
                    any = &er.outputs.at(pc.map);
            const int w = any ? any->width : plan.width, h = any ? any->height : plan.height;
            image::Image out{w, h, std::vector<float>(size_t(w) * size_t(h) * 4, 0.0f)};
            for (size_t c = 0; c < 4 && c < j.layout.size(); ++c)
            {
                const bake::PackChannel& pc = j.layout[c];
                const image::Image* src = pc.map.empty() ? nullptr : splits(j) && pc.map == "normal" ? &detail : &er.outputs.at(pc.map);
                for (size_t i = 0; i < size_t(w) * size_t(h); ++i)
                {
                    float v = src ? src->rgba[i * 4 + size_t(pc.channel)] : pc.constant;
                    if (src && pc.invert)
                        v = 1.0f - v;
                    out.rgba[i * 4 + c] = v;
                }
            }
            if (j.flipGreen)
                for (size_t i = 0; i < size_t(w) * size_t(h); ++i)
                    out.rgba[i * 4 + 1] = 1.0f - out.rgba[i * 4 + 1];
            if (!j.linear && encodesSrgb(j.format, j.bits))
                encodeSrgb(out);

            const std::string file = paths::toUtf8(d.path);
            const std::string content = hex(mixBytes(0, out.rgba.data(), out.rgba.size() * sizeof(float)));
            const json* before = previous.contains(file) ? &previous[file] : nullptr;
            // The inputs moved but the pixels didn't (an edit that cancels out, a rebake to the same maps): keep the file.
            const bool same = options.incremental && before && before->value("content", std::string()) == content && fs::exists(d.path);
            if (!same)
            {
                bake::prepareOutputs({d.path});
                image::WriteOptions wo;
                wo.bits = j.bits;
                image::write(d.path, out, wo);
                writtenPaths.push_back(d.path);
                written.push_back(file);
            }
            else
                unchanged.push_back(file);
            next[file] = {{"key", d.key}, {"content", content}};
            outputs.push_back({{"file", file}, {"map", j.map}, {"udim", t.udim}, {"width", w}, {"height", h}, {"format", j.format}, {"bits", j.bits},
                               {"colorSpace", j.linear ? "linear" : "srgb"}, {"normalFormat", normalFormatOf(j)}, {"changed", !same},
                               {"meta", metaOf(j)}});
        }
    }

    fs::create_directories(manifestPath.parent_path(), ec);
    if (std::ofstream f(manifestPath, std::ios::binary); f)
        f << json{{"set", set.name()}, {"preset", target->name}, {"files", next}}.dump(1);
    bake::addOutputs(writtenPaths);
    return {{"files", files},       {"outputs", outputs}, {"written", written}, {"unchanged", unchanged},
            {"warnings", warnings}, {"preset", target->name}, {"backend", backend}};
}
} // namespace lila::tex
