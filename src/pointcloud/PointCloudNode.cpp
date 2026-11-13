#include "pointcloud/PointCloudNode.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "link/UnrealLink.h"
#include "math/Unreal.h"
#include "ops/Command.h"
#include "scene/GeoSidecar.h"
#include "scene/Georef.h"
#include "scene/Relocate.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cctype>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_set>

namespace lila::pointcloud
{
namespace fs = std::filesystem;
using scene::AttrDef;
using scene::Graph;
using scene::Node;
using scene::NodeId;
using scene::PlugType;
using scene::Transform;

namespace
{
const std::vector<std::string> kColorModes = {"rgb", "intensity", "elevation", "classification", "single"};
const std::vector<std::string> kClipModes = {"off", "inside", "outside"};

std::vector<AttrDef> cloudAttrs()
{
    using namespace scene;
    std::vector<AttrDef> a(size_t(int(PointCloud::kCount) - int(Transform::kCount)));
    auto at = [&](int attr) -> AttrDef& { return a[size_t(attr - Transform::kCount)]; };
    at(PointCloud::kCacheFile) = {.name = "cacheFile", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrFile,
                                  .doc = "The .lilapc cache the points stream from (importPointCloud makes it)."};
    at(PointCloud::kSourceFile) = {.name = "sourceFile", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrFile,
                                   .doc = "The scan the cache was built from (LAS, LAZ or E57)."};
    at(PointCloud::kPointSize) = {.name = "pointSize", .type = PlugType::Float, .defaultValue = 2.0, .flags = AttrInput | AttrStorable | AttrChannelBox,
                                  .doc = "Point size in device pixels."};
    at(PointCloud::kColorMode) = {.name = "colorMode", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable,
                                  .enumNames = kColorModes,
                                  .enumDocs = {"The scan's colours.", "Intensity as grey.", "Height as a ramp.", "LAS class colours.", "One colour."},
                                  .doc = "What the points are coloured by."};
    at(PointCloud::kColor) = {.name = "color", .type = PlugType::Vec3, .defaultValue = glm::dvec3(0.8), .flags = AttrInput | AttrStorable | AttrColor,
                              .doc = "Colour in single mode (linear)."};
    at(PointCloud::kBudget) = {.name = "pointBudget", .type = PlugType::Float, .defaultValue = 8.0, .flags = AttrInput | AttrStorable,
                               .doc = "Most points drawn, in millions; the nearest detail is drawn first."};
    at(PointCloud::kDetail) = {.name = "detail", .type = PlugType::Float, .defaultValue = 1.0, .flags = AttrInput | AttrStorable,
                               .doc = "Level of detail: points a pixel apart at 1, coarser below, finer above (up to the budget)."};
    at(PointCloud::kClipBoxes) = {.name = "clipBoxes", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable,
                                  .doc = "pointCloudClip nodes, by name, separated by spaces."};
    at(PointCloud::kClipMode) = {.name = "clipMode", .type = PlugType::Enum, .defaultValue = int64_t(1), .flags = AttrInput | AttrStorable,
                                 .enumNames = kClipModes, .enumDocs = {"Boxes are ignored.", "Only points inside a box show.", "Points inside the boxes are cut away."},
                                 .doc = "How the clip boxes cut the cloud."};
    at(PointCloud::kEyeDomeLighting) = {.name = "eyeDomeLighting", .type = PlugType::Float, .defaultValue = 0.3,
                                        .flags = AttrInput | AttrStorable | AttrChannelBox,
                                        .doc = "Shades the points by how much nearer their neighbours are, so a scan without normals reads as "
                                               "surfaces. 0 turns it off.",
                                        .minValue = 0.0, .maxValue = 4.0};
    return a;
}

std::mutex gCacheMutex;
std::map<std::string, std::weak_ptr<const PointCloudFile>>& openCaches()
{
    static auto* m = new std::map<std::string, std::weak_ptr<const PointCloudFile>>();
    return *m;
}

json mat(const glm::dmat4& m)
{
    json j = json::array();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            j.push_back(m[c][r]);
    return j;
}

Node& cloudNamed(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n || !n->type().isA("pointCloud"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a point cloud");
    return *n;
}

std::string cloudArg(const Args& a, const char* cmd)
{
    std::vector<std::string> objs = a.objects();
    if (objs.empty())
        throw CommandError(std::string(cmd) + ": name a point cloud or select one");
    return objs[0];
}

/** Every point of the cloud in world space, cut by its clip boxes and thinned to one per spacing cube (0 keeps all). */
/**
 * Points in Unreal level coordinates (cm) from the scene's grid onto the globe frame Cesium lays the level out on. One
 * linear map about the cloud's centre: the grid's convergence and scale there, exact to about 2 cm half a kilometre out.
 */
void moveOntoGlobe(std::vector<RawPoint>& pts)
{
    const std::optional<scene::georef::Georeference> geo = scene::georef::current();
    const std::optional<scene::georef::GlobeFrame> globe = geo ? scene::georef::globeFrame(*geo) : std::nullopt;
    if (!globe || pts.empty())
        return;
    glm::dvec3 lo(std::numeric_limits<double>::max()), hi(std::numeric_limits<double>::lowest());
    for (const RawPoint& p : pts)
    {
        lo = glm::min(lo, p.position);
        hi = glm::max(hi, p.position);
    }
    const glm::dvec3 c = (lo + hi) * 0.5, at = globe->toGlobe(c);
    constexpr double step = 100.0;
    const glm::dmat3 j((globe->toGlobe(c + glm::dvec3(step, 0, 0)) - at) / step, (globe->toGlobe(c + glm::dvec3(0, step, 0)) - at) / step,
                       (globe->toGlobe(c + glm::dvec3(0, 0, step)) - at) / step);
    for (RawPoint& p : pts)
        p.position = at + j * (p.position - c);
}

std::vector<RawPoint> gather(Graph& g, const Node& n, double spacing, bool clip)
{
    LILA_ZONE();
    std::shared_ptr<const PointCloudFile> f = cacheOf(n);
    if (!f)
        throw CommandError("the point cloud's cache can't be read: " + n.as<std::string>(PointCloud::kCacheFile));
    g.evaluate();
    const glm::dmat4 world = n.as<glm::dmat4>(Transform::kWorldMatrix);
    const std::vector<glm::dmat4> boxes = clip ? clipBoxes(g, n) : std::vector<glm::dmat4>();
    const int64_t mode = n.as<int64_t>(PointCloud::kClipMode);
    std::vector<RawPoint> out;
    std::unordered_set<uint64_t> taken;
    progress::Scope scope("Reading points", double(f->nodes().size()));
    for (uint32_t i = 0; i < f->nodes().size(); ++i)
    {
        const OctreeNode& node = f->nodes()[i];
        for (const PackedPoint& p : f->read(i))
        {
            const glm::dvec3 w(world * glm::dvec4(PointCloudFile::position(node, p), 1.0));
            if (!boxes.empty() && mode != PointCloud::ClipOff)
            {
                bool inside = false;
                for (const glm::dmat4& b : boxes)
                {
                    const glm::dvec3 q(b * glm::dvec4(w, 1.0));
                    inside = inside || (std::abs(q.x) <= 0.5 && std::abs(q.y) <= 0.5 && std::abs(q.z) <= 0.5);
                }
                if (inside != (mode == PointCloud::ClipInside))
                    continue;
            }
            if (spacing > 0.0)
            {
                const glm::i64vec3 c(glm::floor(w / spacing));
                const uint64_t key = (uint64_t(c.x) * 73856093ull) ^ (uint64_t(c.y) * 19349663ull) ^ (uint64_t(c.z) * 83492791ull);
                if (!taken.insert(key).second)
                    continue;
            }
            RawPoint& r = out.emplace_back();
            r.position = w;
            r.rgb[0] = p.r, r.rgb[1] = p.g, r.rgb[2] = p.b;
            r.classification = p.classification;
            r.intensity = p.intensity;
        }
        scope.step();
    }
    return out;
}

/** Where a source's cache goes: beside it, else in the user cache folder when that folder can't be written. */
fs::path cachePathFor(const fs::path& source)
{
    fs::path beside = fs::path(source).replace_extension(".lilapc");
    std::error_code ec;
    const fs::path probe = fs::path(beside).concat(".probe");
    if (std::FILE* f = std::fopen(probe.string().c_str(), "wb"))
    {
        std::fclose(f);
        fs::remove(probe, ec);
        return beside;
    }
    const fs::path dir = paths::userConfigDir() / "cache" / "pointclouds";
    fs::create_directories(dir, ec);
    return dir / (source.stem().string() + "-" + std::to_string(std::hash<std::string>()(source.string())) + ".lilapc");
}

json importCloud(const Args& a)
{
    LILA_ZONE();
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError("importPointCloud: path required");
    const fs::path path = paths::fromUtf8(a.positional[0].get<std::string>());
    if (!fs::exists(path))
        throw CommandError("importPointCloud: no file " + path.string());
    std::unique_ptr<PointSource> src;
    try
    {
        src = openSource(path);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string("importPointCloud: ") + e.what());
    }
    const SourceInfo info = src->info();

    // The file's CRS: its own, else a .prj or metadata.xml beside it.
    std::string crs = info.crs;
    std::optional<glm::dvec3> sidecarOrigin;
    if (crs.empty())
        if (auto found = scene::geoSidecar::find(path))
        {
            crs = found->georeference.crs;
            if (found->hasOffset)
                sidecarOrigin = found->georeference.origin;
        }
    glm::dvec3 centre(0.0);
    if (info.bounds)
        centre = (info.bounds->first + info.bounds->second) * 0.5;
    else
    {
        std::vector<RawPoint> first;
        if (src->next(first, 4096) && !first.empty())
        {
            for (const RawPoint& p : first)
                centre += p.position / double(first.size());
        }
        src->rewind();
    }
    // Absolute coordinates are rebased on the rounded centre, which becomes the file's origin.
    const glm::dvec3 rebase = glm::round(centre / 100.0) * 100.0;
    const bool zUp = scene::georef::sceneZUp();
    glm::dmat4 toLocal(1.0);
    std::string message;
    const bool useGeo = a.get<bool>("georeference", true) && !crs.empty() && scene::georef::usable(scene::georef::crsInfo(crs));
    if (useGeo)
    {
        scene::georef::Georeference file{crs, sidecarOrigin ? *sidecarOrigin + rebase : rebase};
        const scene::georef::Placement placed = scene::georef::place(file);
        toLocal = placed.matrix * scene::georef::localFrame(file, zUp) * glm::translate(glm::dmat4(1.0), -rebase);
        message = placed.message;
    }
    else
    {
        // No usable CRS: metres, Z up, as scanners write them, rebased so the points keep their precision.
        const double s = a.get<double>("unitScale", 100.0);
        glm::dmat4 axes(s);
        axes[3][3] = 1.0;
        if (!zUp)
            axes = glm::dmat4(glm::dvec4(s, 0, 0, 0), glm::dvec4(0, 0, -s, 0), glm::dvec4(0, s, 0, 0), glm::dvec4(0, 0, 0, 1));
        toLocal = axes * glm::translate(glm::dmat4(1.0), -rebase);
        message = std::string(crs.empty() ? "No CRS in the file" : "The file's CRS is not projected") + ": not georeferenced, rebased on its centre (" +
                  std::to_string(int64_t(rebase.x)) + ", " + std::to_string(int64_t(rebase.y)) + ", " + std::to_string(int64_t(rebase.z)) + ")";
    }

    fs::path cache = a.has("cache") ? paths::fromUtf8(a.get<std::string>("cache")) : cachePathFor(path);
    bool reuse = false;
    std::error_code ec;
    if (!a.get<bool>("rebuild") && fs::exists(cache, ec) && fs::last_write_time(cache, ec) >= fs::last_write_time(path, ec))
        try
        {
            const PointCloudFile existing(cache);
            reuse = existing.metadata().value("toLocal", json()) == mat(toLocal) && existing.metadata().value("source", "") == paths::toUtf8(path);
        }
        catch (const std::exception&)
        {
        }
    BuildStats stats;
    if (!reuse && fs::exists(cache, ec) && !fs::remove(cache, ec))
    {
        // An open cache (a cloud still drawing from it) can't be replaced on Windows: build beside it.
        fs::path next;
        for (int n = 2; n < 100 && (next.empty() || fs::exists(next, ec)); ++n)
            next = fs::path(cache).replace_filename(cache.stem().string() + "-" + std::to_string(n) + ".lilapc");
        log::info("importPointCloud: {} is in use; building {} instead", cache.filename().string(), next.filename().string());
        cache = next;
    }
    if (!reuse)
    {
        progress::Scope scope("Building the point cloud cache for " + path.filename().string(), 1000.0);
        BuildOptions o;
        o.toLocal = toLocal;
        o.metadata = {{"source", paths::toUtf8(path)}, {"toLocal", mat(toLocal)}, {"crs", crs}, {"georeferenced", useGeo}};
        o.progress = [&scope](double f) {
            scope.set(f * 1000.0);
            return !scope.cancelled();
        };
        std::string err;
        progress::runOnWorker([&] {
            try
            {
                stats = buildOctree(*src, cache, o);
            }
            catch (const std::exception& e)
            {
                err = e.what();
            }
        });
        if (!err.empty())
            throw CommandError("importPointCloud: " + err);
    }

    Graph& g = Graph::current();
    Graph::Batch batch(g);
    std::string wanted = a.has("name") ? a.get<std::string>("name") : path.stem().string();
    // Node names take letters, digits and underscores: a scan's file name often has dashes.
    for (char& c : wanted)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            c = '_';
    Node* n = g.createNode(*scene::types::find("pointCloud"), Graph::validName(wanted) ? wanted : "PointCloud", scene::kNoNode, true);
    g.setValue({n->id(), PointCloud::kCacheFile, -1}, paths::storedPath(cache));
    g.setValue({n->id(), PointCloud::kSourceFile, -1}, paths::storedPath(path));
    g.select({n->id()});
    log::info("importPointCloud: {}", message);
    std::shared_ptr<const PointCloudFile> f = cacheOf(*n);
    return {{"node", n->path()},
            {"cache", paths::toUtf8(cache)},
            {"reused", reuse},
            {"points", f ? f->pointCount() : 0},
            {"nodes", f ? f->nodes().size() : 0},
            {"format", info.format},
            {"seconds", stats.seconds},
            {"georeference", message}};
}
} // namespace

std::shared_ptr<const PointCloudFile> cacheOf(const Node& n)
{
    const std::string stored = n.as<std::string>(PointCloud::kCacheFile);
    if (stored.empty())
        return nullptr;
    const fs::path p = paths::resolvePath(stored);
    std::scoped_lock lock(gCacheMutex);
    std::weak_ptr<const PointCloudFile>& slot = openCaches()[paths::toUtf8(p)];
    if (auto f = slot.lock())
        return f;
    try
    {
        auto f = std::make_shared<const PointCloudFile>(p);
        slot = f;
        return f;
    }
    catch (const std::exception& e)
    {
        log::warn("point cloud {}: {}", n.path(), e.what());
        return nullptr;
    }
}

std::vector<glm::dmat4> clipBoxes(Graph& g, const Node& cloud)
{
    std::vector<glm::dmat4> out;
    std::istringstream in(cloud.as<std::string>(PointCloud::kClipBoxes));
    for (std::string name; in >> name;)
        if (const Node* b = g.find(name); b && b->type().isA("pointCloudClip") && b->as<bool>(PointCloudClip::kEnabled))
            out.push_back(glm::inverse(b->as<glm::dmat4>(Transform::kWorldMatrix)));
    return out;
}

void registerTypes()
{
    if (scene::types::find("pointCloud"))
        return;
    scene::NodeType t;
    t.name = "pointCloud";
    t.icon = "grain";
    t.base = scene::types::find("transform");
    t.dag = true;
    t.doc = "A scan streamed from its cache by level of detail: LAS, LAZ or E57, hundreds of millions of points.";
    t.ownAttrs = cloudAttrs();
    t.create = [] { return std::make_unique<PointCloud>(); };
    scene::types::registerType(std::move(t));

    scene::NodeType c;
    c.name = "pointCloudClip";
    c.icon = "box";
    c.base = scene::types::find("transform");
    c.dag = true;
    c.doc = "A box that keeps or cuts away the points of the point clouds that name it.";
    c.ownAttrs = {{.name = "enabled", .type = PlugType::Bool, .defaultValue = true, .flags = scene::AttrInput | scene::AttrStorable | scene::AttrChannelBox,
                   .doc = "The box cuts while on."}};
    c.create = [] { return std::make_unique<PointCloudClip>(); };
    scene::types::registerType(std::move(c));
    // Points are cloud-local and boxes are transforms, so both follow their own transforms when relocateScene moves them.
    scene::relocate::registerNeutral("pointCloud");
    scene::relocate::registerNeutral("pointCloudClip");
}

void registerCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "importPointCloud",
        .doc = "Import a LAS, LAZ or E57 scan as a point cloud. Its points are sorted into a level-of-detail cache (.lilapc, beside "
               "the scan) that the view streams from, so clouds of hundreds of millions of points open at once; a cache newer than "
               "the scan is reused.\n\n"
               "A CRS in the file (or a .prj beside it) sets the scene's georeference or moves the cloud into it; without one the "
               "cloud is read as metres, Z up, rebased on its centre. The log says which. Returns the node, cache and counts.",
        .positionalDoc = "path",
        .flags = {{"name", "n", T::String, nullptr, "Node name; the file's by default."},
                  {"cache", "", T::String, nullptr, "Where the cache goes, instead of beside the scan."},
                  {"rebuild", "", T::Bool, false, "Build the cache again even when it is up to date."},
                  {"georeference", "", T::Bool, true, "Place it by its CRS."},
                  {"unitScale", "", T::Float, 100.0, "cm per file unit when the file has no CRS.", {}, "", 1e-6}},
        .run = importCloud,
        .examples = {"importPointCloud(\"D:/scans/site.laz\")"},
    });
    ops::registerCommand({
        .name = "pointCloudInfo",
        .doc = "A point cloud's counts, bounds (node space, cm) and source: {points, nodes, depth, color, crs, format, boundsMin, boundsMax}.",
        .positionalDoc = "pointCloud",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const Node& n = cloudNamed(g, cloudArg(a, "pointCloudInfo"), "pointCloudInfo");
            std::shared_ptr<const PointCloudFile> f = cacheOf(n);
            if (!f)
                throw CommandError("pointCloudInfo: the cache can't be read");
            int depth = 0;
            for (const OctreeNode& o : f->nodes())
                depth = std::max(depth, int(o.depth));
            json j = f->metadata();
            j.erase("toLocal");
            j["nodes"] = f->nodes().size();
            j["depth"] = depth;
            return j;
        },
    });
    ops::registerCommand({
        .name = "pointCloudClipBox",
        .doc = "Add a clip box to a point cloud, around its middle half, and select it. Move, turn and scale it like any object; "
               "clipMode on the cloud keeps the inside or cuts it away. Returns the box's name.",
        .positionalDoc = "pointCloud",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& n = cloudNamed(g, cloudArg(a, "pointCloudClipBox"), "pointCloudClipBox");
            std::shared_ptr<const PointCloudFile> f = cacheOf(n);
            glm::dvec3 lo(-50.0), hi(50.0);
            if (f)
                if (const json& a0 = f->metadata().value("boundsMin", json()); a0.is_array())
                {
                    const json& b0 = f->metadata()["boundsMax"];
                    lo = {a0[0].get<double>(), a0[1].get<double>(), a0[2].get<double>()};
                    hi = {b0[0].get<double>(), b0[1].get<double>(), b0[2].get<double>()};
                }
            g.evaluate();
            const glm::dmat4 world = n.as<glm::dmat4>(Transform::kWorldMatrix);
            Graph::Batch batch(g);
            Node* box = g.createNode(*scene::types::find("pointCloudClip"), n.name() + "_clip", scene::kNoNode, true);
            g.setValue({box->id(), Transform::kTranslate, -1}, glm::dvec3(world * glm::dvec4((lo + hi) * 0.5, 1.0)));
            g.setValue({box->id(), Transform::kScale, -1}, glm::max((hi - lo) * 0.5, glm::dvec3(1.0)));
            std::string list = n.as<std::string>(PointCloud::kClipBoxes);
            list += (list.empty() ? "" : " ") + box->path();
            g.setValue({n.id(), PointCloud::kClipBoxes, -1}, list);
            if (n.as<int64_t>(PointCloud::kClipMode) == PointCloud::ClipOff)
                g.setValue({n.id(), PointCloud::kClipMode, -1}, int64_t(PointCloud::ClipInside));
            g.select({box->id()});
            return box->path();
        },
    });
    ops::registerCommand({
        .name = "pointCloudExtract",
        .doc = "Make a mesh of a point cloud's points (loose vertices with their colours in colorSet), cut by its clip boxes and "
               "thinned to one point per spacing cube: for snapping, retopology, or meshing elsewhere. Returns the mesh's name.",
        .positionalDoc = "pointCloud",
        .flags = {{"spacing", "s", T::Float, 5.0, "Keep one point per cube this many cm across; 0 keeps every point.", {}, "", 0.0},
                  {"clip", "", T::Bool, true, "Apply the cloud's clip boxes."},
                  {"maxPoints", "", T::Int, 20000000, "Refuse more than this many points.", {}, "", 1}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const Node& n = cloudNamed(g, cloudArg(a, "pointCloudExtract"), "pointCloudExtract");
            std::vector<RawPoint> pts = gather(g, n, a.get<double>("spacing"), a.get<bool>("clip"));
            if (pts.size() > size_t(a.get<int64_t>("maxPoints")))
                throw CommandError("pointCloudExtract: " + std::to_string(pts.size()) + " points; raise spacing or maxPoints");
            geo::Mesh mesh;
            std::vector<glm::vec4> colors;
            colors.reserve(pts.size());
            for (const RawPoint& p : pts)
            {
                mesh.addPoint(p.position);
                colors.emplace_back(p.rgb[0] / 255.0f, p.rgb[1] / 255.0f, p.rgb[2] / 255.0f, 1.0f);
            }
            geo::colors::add(mesh, "colorSet");
            geo::colors::setFromPoints(mesh, "colorSet", colors);
            Graph::Batch batch(g);
            Node* m = g.createNode(*scene::types::find("mesh"), n.name() + "_points", scene::kNoNode, true);
            g.setValue({m->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
            g.select({m->id()});
            return m->path();
        },
    });
    ops::registerCommand({
        .name = "unrealSendPointCloud",
        .doc = "Send a point cloud to Unreal's open level through its LiDAR Point Cloud plugin: cut by its clip boxes, thinned to "
               "spacing, written as LAS in the level's coordinates and shown on a LidarPointCloudActor named after the cloud. A "
               "georeferenced scene puts the level on its place on the Earth first. Returns what Unreal made.",
        .positionalDoc = "pointCloud",
        .flags = {{"spacing", "s", T::Float, 2.0, "Keep one point per cube this many cm across; 0 keeps every point.", {}, "", 0.0},
                  {"clip", "", T::Bool, true, "Apply the cloud's clip boxes."},
                  {"folder", "", T::String, "/Game/Lila/PointClouds", "Content folder for the asset."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const Node& n = cloudNamed(g, cloudArg(a, "unrealSendPointCloud"), "unrealSendPointCloud");
            std::vector<RawPoint> pts = gather(g, n, a.get<double>("spacing"), a.get<bool>("clip"));
            try
            {
                link::Link::get().ensureConnected();
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("not connected to Unreal: ") + e.what());
            }
            std::shared_ptr<link::RpcClient> client = link::Link::get().client();
            if (!client)
                throw CommandError("not connected to Unreal");
            const json features = link::Link::get().status().value("features", json::array());
            if (std::ranges::find(features, json("pointClouds")) == features.end())
                throw CommandError("unrealSendPointCloud: the LilaLink plugin predates point clouds; rebuild it from Lila's ue/LilaLink");
            json out;
            if (json geo = scene::georef::exportJson(); !geo.is_null())
                out["georeference"] = client->call("setGeoreference", geo, link::placementTimeoutMs(0)).result;
            // Unreal's axes and metres, from the level's origin (the scene's), as the LiDAR plugin reads LAS.
            for (RawPoint& p : pts)
            {
                const math::Vector u = math::toUnreal(math::Vector(p.position));
                p.position = glm::dvec3(u.x, u.y, u.z);
            }
            if (out.contains("georeference") && out["georeference"].value("cesium", false))
                moveOntoGlobe(pts);
            // LAS is X east, Y north: the LiDAR plugin turns Y into Unreal's south itself.
            for (RawPoint& p : pts)
                p.position = glm::dvec3(p.position.x, -p.position.y, p.position.z) / 100.0;
            const fs::path file = link::cacheDir() / (n.name() + ".las");
            std::error_code ec;
            fs::create_directories(file.parent_path(), ec);
            writeLas(file, pts, std::string());
            link::RpcResult r = client->call("importPointCloud",
                                             {{"file", paths::toUtf8(file)}, {"folder", a.get<std::string>("folder")}, {"name", n.name()}, {"label", n.name()}},
                                             link::placementTimeoutMs(pts.size() / 1000));
            if (!r.ok)
                throw CommandError("unrealSendPointCloud: " + r.error);
            out["points"] = pts.size();
            out["unreal"] = r.result;
            return out;
        },
    });
    ops::registerCommand({
        .name = "exportPointCloud",
        .doc = "Write a point cloud to LAS or LAZ (by extension), cut by its clip boxes and thinned to spacing: in the scene's CRS at "
               "its real coordinates when the scene is georeferenced (the WKT goes in the file), else in metres Z up. Unreal's LiDAR "
               "Point Cloud plugin reads it. Returns {path, points}.",
        .positionalDoc = "path pointCloud",
        .flags = {{"spacing", "s", T::Float, 0.0, "Keep one point per cube this many cm across; 0 keeps every point.", {}, "", 0.0},
                  {"clip", "", T::Bool, true, "Apply the cloud's clip boxes."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.size() < 1 || !a.positional[0].is_string())
                throw CommandError("exportPointCloud: path required");
            const fs::path path = paths::fromUtf8(a.positional[0].get<std::string>());
            Graph& g = Graph::current();
            const std::string name = a.positional.size() > 1 ? a.positional[1].get<std::string>() : cloudArg(Args{}, "exportPointCloud");
            const Node& n = cloudNamed(g, name, "exportPointCloud");
            std::vector<RawPoint> pts = gather(g, n, a.get<double>("spacing"), a.get<bool>("clip"));
            const std::optional<scene::georef::Georeference> geo = scene::georef::current();
            const bool zUp = scene::georef::sceneZUp();
            for (RawPoint& p : pts)
                p.position = geo ? scene::georef::toProjected(*geo, p.position, zUp)
                                 : (zUp ? p.position / 100.0 : glm::dvec3(p.position.x, -p.position.z, p.position.y) / 100.0);
            try
            {
                writeLas(path, pts, geo ? scene::georef::wkt(geo->crs) : std::string());
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("exportPointCloud: ") + e.what());
            }
            return {{"path", paths::toUtf8(path)}, {"points", pts.size()}, {"georeferenced", geo.has_value()}};
        },
    });
}
} // namespace lila::pointcloud
