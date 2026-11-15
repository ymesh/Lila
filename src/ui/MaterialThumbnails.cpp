#include "ui/MaterialThumbnails.h"

#include "bake/Backend.h"
#include "bake/Bake.h"
#include "bake/BakeInputs.h"
#include "bake/BakePipeline.h"
#include "bake/Maps.h"
#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "geo/Attribute.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "ops/Command.h"
#include "render/Renderer.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Node.h"
#include "tex/Evaluate.h"
#include "tex/Generators.h"
#include "tex/Layers.h"
#include "tex/Library.h"
#include "tex/Lint.h"
#include "tex/Plan.h"
#include "tex/SmartMaterial.h"
#include "tex/TextureSet.h"
#include "ui/Camera.h"
#include "ui/Environments.h"
#include "ui/MeshCache.h"
#include "uv/Metrics.h"

#include <QCoreApplication>
#include <QImage>

#include <algorithm>
#include <chrono>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <cmath>
#include <cstring>
#include <set>
#include <format>
#include <fstream>
#include <stdexcept>

namespace lila::ui::materialThumbs
{
namespace
{
namespace fs = std::filesystem;
namespace library = tex::library;
using json = nlohmann::json;

/** A library material's tiles across the ball's uv square, which spans the ball's circumference. */
constexpr double kBallTiles = 4.0;
/** The ball's maps and mesh maps, square. */
constexpr int kBallTexels = 1024;
/** Centimetres one uv unit spans on the ball (its circumference), so texels stay square. */
constexpr double kBallUvCm = 2.0 * 3.14159265358979 * 45.0;
constexpr const char* kVersion = "ball-v2";
constexpr std::string_view kSmartPrefix = "smart:";

struct Source
{
    json doc;
    library::Meta meta;
    /** Set when the material is a texture set of the scene. */
    bool isSet = false;
};

/** A smart material as a set's graph: its layer alone on a fresh stack, or a smart mask on a light fill over a dark base. */
json smartDocument(const std::string& name)
{
    tex::SmartMaterial sm = tex::loadSmartMaterial(name);
    tex::SetSettings settings = tex::SetSettings::makeDefault();
    settings.uvWidth = settings.uvHeight = kBallUvCm;
    tex::LayerStack stack;
    if (sm.kind == "mask")
    {
        stack.base["baseColor"] = json::array({0.04, 0.04, 0.045, 1.0});
        tex::Layer fill;
        fill.id = stack.newId('L');
        fill.name = "Mask";
        fill.material = {{"baseColor", json::array({0.85, 0.85, 0.85, 1.0})}};
        fill.masks = tex::instantiateMasks(sm.masks, stack);
        stack.layers.push_back(std::move(fill));
    }
    else
        stack.layers.push_back(tex::instantiateLayer(sm.layer, stack));
    tex::StackCompile c = tex::compileStack(stack, settings, tex::Graph{});
    return {{"graph", c.graph.toJson()}, {"settings", settings.toJson()}};
}

Source resolve(const std::string& material)
{
    Source s;
    if (material.starts_with(kSmartPrefix))
    {
        s.doc = smartDocument(material.substr(kSmartPrefix.size()));
        s.meta.name = material.substr(kSmartPrefix.size());
        s.isSet = true;
        return s;
    }
    if (std::optional<library::Entry> e = library::find(material))
    {
        s.doc = library::document(*e);
        s.meta = e->meta;
        s.isSet = e->tier == "scene";
        return s;
    }
    if (scene::Node* n = tex::findSet(scene::Graph::current(), material))
    {
        s.doc = {{"graph", tex::readGraph(*n).toJson()}, {"settings", tex::readSettings(*n).toJson()}};
        s.meta.name = n->name();
        s.isSet = true;
        return s;
    }
    fs::path p = paths::fromUtf8(material);
    std::error_code ec;
    if (fs::is_regular_file(p, ec))
    {
        std::ifstream in(p, std::ios::binary);
        s.doc = json::parse(in, nullptr, false);
        if (s.doc.is_discarded() || !s.doc.contains("graph"))
            throw std::runtime_error(material + " isn't a .lilamat file");
        s.meta = library::Meta::fromJson(s.doc.value("meta", json::object()));
        return s;
    }
    throw std::runtime_error("no library material, texture set or file '" + material + "'");
}

std::string keyOf(const Source& s, const Options& o, const std::string& meshKey)
{
    std::string text = s.doc.value("graph", json()).dump() + s.doc.value("settings", json()).dump() + std::format("|{}|{}|{}|{}", s.meta.scaleCm, o.size, meshKey, kVersion);
    return std::format("{:016x}", tex::hash64(text));
}

uint8_t unorm(float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

/** One texture from evaluated channels: fn writes texel i's four bytes from the outputs. */
render::TexturePtr makeTexture(int w, int h, bool srgb, const std::function<void(size_t, uint8_t*)>& fn)
{
    auto t = std::make_shared<render::TextureImage>();
    t->width = w;
    t->height = h;
    t->srgb = srgb;
    t->rgba.resize(size_t(w) * size_t(h) * 4);
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i)
        fn(i, &t->rgba[i * 4]);
    return t;
}

render::Material materialFrom(const tex::EvalResult& res, int w, int h)
{
    render::Material m;
    m.flags = render::MatTwoSided;
    m.baseColor = glm::vec3(1.0f);
    m.roughness = 1.0f;
    m.metallic = 1.0f;
    auto out = [&](const char* name) -> const image::Image* {
        auto it = res.outputs.find(name);
        return it == res.outputs.end() ? nullptr : &it->second;
    };
    if (const image::Image* bc = out("baseColor"))
    {
        m.baseColorMap = makeTexture(w, h, true, [bc](size_t i, uint8_t* o) {
            for (int c = 0; c < 3; ++c)
                o[c] = unorm(float(tex::lint::toSrgb(bc->rgba[i * 4 + size_t(c)])));
            o[3] = 255;
        });
        m.flags |= render::MatBaseColorMap;
    }
    if (const image::Image* n = out("normal"))
    {
        m.normalMap = makeTexture(w, h, false, [n](size_t i, uint8_t* o) {
            for (int c = 0; c < 3; ++c)
                o[c] = unorm(n->rgba[i * 4 + size_t(c)]);
            o[3] = 255;
        });
        m.flags |= render::MatNormalMap | render::MatNormalGreenUp;
    }
    const image::Image* ao = out("ao");
    const image::Image* rough = out("roughness");
    const image::Image* metal = out("metallic");
    if (ao || rough || metal)
    {
        m.ormMap = makeTexture(w, h, false, [=](size_t i, uint8_t* o) {
            o[0] = ao ? unorm(ao->rgba[i * 4]) : 255;
            o[1] = rough ? unorm(rough->rgba[i * 4]) : 140;
            o[2] = metal ? unorm(metal->rgba[i * 4]) : 0;
            o[3] = 255;
        });
        m.flags |= render::MatOcclusionMap | render::MatRoughnessMap | render::MatMetallicMap;
    }
    if (const image::Image* e = out("emissive"))
    {
        bool lit = false;
        for (size_t i = 0; i < e->rgba.size() && !lit; ++i)
            lit = i % 4 != 3 && e->rgba[i] > 1e-4f;
        if (lit)
        {
            m.emissiveMap = makeTexture(w, h, true, [e](size_t i, uint8_t* o) {
                for (int c = 0; c < 3; ++c)
                    o[c] = unorm(float(tex::lint::toSrgb(e->rgba[i * 4 + size_t(c)])));
                o[3] = 255;
            });
            m.emissive = glm::vec3(1.0f);
            m.flags |= render::MatEmissiveMap;
        }
    }
    return m;
}

/** A profile point: distance from the axis, height along it, and whether the surface creases there. */
struct ProfilePoint
{
    double radial;
    double axial;
    bool hard = false;
};

/** Polygons gathered for Mesh::fromPolygons, with a uv per corner and the point pairs whose edges are hard. */
struct Soup
{
    std::vector<glm::dvec3> points;
    std::vector<geo::Index> sizes;
    std::vector<geo::Index> indices;
    std::vector<glm::vec2> uvs;
    std::set<std::pair<geo::Index, geo::Index>> hard;
};

/**
 * Revolves a profile, listed from the +axis pole to the -axis pole, about axis through origin. u runs once around, v from
 * vStart along the profile's length in uv units of kBallUvCm.
 */
void revolve(Soup& s, const std::vector<ProfilePoint>& profile, glm::dvec3 origin, glm::dvec3 axis, int segments, double vStart)
{
    axis = glm::normalize(axis);
    const glm::dvec3 e1 = glm::normalize(glm::cross(std::abs(axis.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0), axis));
    const glm::dvec3 e2 = glm::cross(axis, e1);
    std::vector<std::vector<geo::Index>> rings;
    std::vector<double> v;
    double along = vStart;
    for (size_t i = 0; i < profile.size(); ++i)
    {
        const ProfilePoint& p = profile[i];
        if (i > 0)
            along += std::hypot(p.radial - profile[i - 1].radial, p.axial - profile[i - 1].axial) / kBallUvCm;
        v.push_back(along);
        std::vector<geo::Index> ring;
        const int count = p.radial <= 1e-9 ? 1 : segments;
        for (int j = 0; j < count; ++j)
        {
            const double phi = 2.0 * 3.14159265358979 * double(j) / double(segments);
            ring.push_back(geo::Index(s.points.size()));
            s.points.push_back(origin + axis * p.axial + (e1 * std::cos(phi) + e2 * std::sin(phi)) * p.radial);
        }
        if (p.hard && count > 1)
            for (int j = 0; j < count; ++j)
                s.hard.insert(std::minmax(ring[size_t(j)], ring[size_t((j + 1) % count)]));
        rings.push_back(std::move(ring));
    }
    for (size_t i = 0; i + 1 < rings.size(); ++i)
        for (int j = 0; j < segments; ++j)
        {
            const float u0 = float(j) / float(segments), u1 = float(j + 1) / float(segments), um = (u0 + u1) * 0.5f;
            const float va = float(v[i]), vb = float(v[i + 1]);
            auto at = [&](size_t r, int k) { return rings[r].size() == 1 ? rings[r][0] : rings[r][size_t(k % segments)]; };
            // Counter-clockwise seen from outside: down the profile first, then around.
            const std::pair<geo::Index, glm::vec2> quad[4] = {
                {at(i, j), {rings[i].size() == 1 ? um : u0, va}},
                {at(i + 1, j), {rings[i + 1].size() == 1 ? um : u0, vb}},
                {at(i + 1, j + 1), {u1, vb}},
                {at(i, j + 1), {u1, va}},
            };
            geo::Index n = 0;
            for (int k = 0; k < 4; ++k)
            {
                if ((k == 2 && rings[i + 1].size() == 1) || (k == 3 && rings[i].size() == 1))
                    continue;
                s.indices.push_back(quad[k].first);
                s.uvs.push_back(quad[k].second);
                ++n;
            }
            s.sizes.push_back(n);
        }
}

/** A sphere profile of radius r from polar angle a0 to a1 (degrees from the +axis pole), every step degrees. */
void arc(std::vector<ProfilePoint>& out, double r, double a0, double a1, double step, bool hardEnds)
{
    const int n = std::max(1, int(std::ceil((a1 - a0) / step)));
    for (int i = 0; i <= n; ++i)
    {
        const double a = glm::radians(a0 + (a1 - a0) * double(i) / double(n));
        out.push_back({r * std::sin(a), r * std::cos(a), hardEnds && (i == 0 || i == n)});
    }
}

/**
 * The preview ball: a 45 cm sphere with a recessed cap, a raised ring and grooves turned towards the camera, on a stepped
 * pedestal. Both share one non-overlapping uv square, so mesh maps bake on it.
 */
scene::MeshPtr previewBall()
{
    static scene::MeshPtr ball = [] {
        Soup s;
        std::vector<ProfilePoint> sphere;
        arc(sphere, 41.5, 0.0, 15.0, 3.0, true);
        arc(sphere, 47.0, 15.0, 21.0, 2.0, true);
        arc(sphere, 45.0, 21.0, 33.0, 3.0, true);
        arc(sphere, 43.5, 33.0, 35.5, 2.5, true);
        arc(sphere, 45.0, 35.5, 62.0, 3.0, true);
        arc(sphere, 43.8, 62.0, 64.0, 2.0, true);
        arc(sphere, 45.0, 64.0, 180.0, 3.0, true);
        sphere.front().hard = sphere.back().hard = false;
        const glm::dvec3 centre(0.0, 54.0, 0.0);
        const glm::dvec3 facing(std::sin(glm::radians(28.0)) * std::cos(glm::radians(14.0)), std::sin(glm::radians(14.0)),
                                std::cos(glm::radians(28.0)) * std::cos(glm::radians(14.0)));
        revolve(s, sphere, centre, facing, 128, 0.02);
        const std::vector<ProfilePoint> pedestal = {
            {0.0, 11.0}, {24.0, 11.0, true}, {24.0, 9.5, true}, {31.0, 8.0, true}, {31.0, 5.5, true},
            {37.0, 5.5, true}, {40.0, 3.0, true}, {40.0, 0.0, true}, {0.0, 0.0},
        };
        revolve(s, pedestal, glm::dvec3(0.0), glm::dvec3(0, 1, 0), 128, 0.62);

        auto m = std::make_shared<geo::Mesh>(geo::Mesh::fromPolygons(s.points, s.sizes, s.indices));
        std::span<glm::vec2> uv = m->cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
        size_t next = 0;
        for (geo::Index f = 0; f < m->faceCapacity(); ++f)
            if (m->faceValid(f))
                for (geo::Index c : m->faceCorners(f))
                    uv[c] = s.uvs[next++];
        std::span<bool> hard = m->edgeAttrs().getOrAdd<bool>(geo::attr::kHard);
        for (geo::Index e = 0; e < m->edgeCapacity(); ++e)
            if (m->edgeValid(e))
                hard[e] = s.hard.contains(std::minmax(m->edgePoint(e, 0), m->edgePoint(e, 1)));
        return scene::MeshPtr(m);
    }();
    return ball;
}

/** The ball's baked mesh maps (curvature, ao, ...) by bake map id, the external names texture graphs read them under. */
struct BallMaps
{
    std::mutex mutex;
    bool baked = false;
    std::map<std::string, image::Image> externals;
};

BallMaps& ballMaps()
{
    static BallMaps m;
    return m;
}

/** The ball as its own low and high: the inputs buildInputs would make for a group baking it onto itself. */
bake::BakeInputs ballBakeInputs()
{
    const geo::Mesh& mesh = *previewBall();
    bake::BakeInputs in;
    in.project = "PreviewBall";
    bake::GroupInputs gi;
    gi.name = "Ball";
    gi.settings.lows = {"PreviewBall"};
    bake::HighEntry self;
    self.self = true;
    gi.settings.highs = {self};
    gi.lows.push_back(bake::lowFromMesh(mesh, glm::dmat4(1.0), "PreviewBall"));
    bake::HighInput high;
    high.soup = bake::soupFromMesh(mesh, glm::dmat4(1.0), "PreviewBall");
    high.source = "PreviewBall";
    high.sourceTriangles = high.soup.triangleCount();
    gi.highMin = in.highMin = high.soup.boundsMin;
    gi.highMax = in.highMax = high.soup.boundsMax;
    gi.frontal = gi.rear = 0.5f;
    gi.highs.push_back(std::move(high));
    gi.highNodes = {"PreviewBall"};
    gi.highSelfLow = {0};
    in.groups.push_back(std::move(gi));
    bake::buildLinks(in);
    return in;
}

/** Worker: bakes the ball's mesh maps the first time; empty when baking failed (graphs then read their defaults). */
const std::map<std::string, image::Image>& ballExternals()
{
    BallMaps& b = ballMaps();
    std::scoped_lock lock(b.mutex);
    if (b.baked)
        return b.externals;
    b.baked = true;
    try
    {
        LILA_ZONE_N("materialThumbs::bakeBall");
        bake::registerBuiltinMaps();
        bake::ProjectSettings ps = bake::ProjectSettings::defaults();
        ps.maps.clear();
        for (const tex::MeshRole& r : tex::meshRoles())
            if (bake::findMap(r.bakeMap) && std::ranges::none_of(ps.maps, [&](const bake::MapSettings& m) { return m.map == r.bakeMap; }))
            {
                bake::MapSettings m;
                m.map = r.bakeMap;
                m.suffix = r.bakeMap;
                ps.maps.push_back(std::move(m));
            }
        ps.output.width = ps.output.height = kBallTexels;
        ps.output.supersampling = 1;
        ps.output.padding = 8;
        ps.output.format = "exr";
        ps.output.name = "{mesh}_{map}_{udim}";
        bake::BakeInputs in = ballBakeInputs();
        std::unique_ptr<bake::Backend> backend;
        try
        {
            backend = bake::createBackend(bake::preferredBackend());
        }
        catch (const std::exception&)
        {
        }
        if (!backend)
            backend = bake::createBackend("cpu");
        backend->prepare(in);
        std::map<std::string, image::Image> byMap;
        std::mutex texMutex;
        bake::BakeOptions opt;
        opt.folder = std::filesystem::temp_directory_path() / "lila_preview_ball";
        opt.keepTexture = [&](const bake::TextureBuffer& t) {
            if (!t.def)
                return;
            image::Image img{t.width, t.height, std::vector<float>(t.value.size() * 4)};
            std::memcpy(img.rgba.data(), t.value.data(), t.value.size() * sizeof(glm::vec4));
            std::scoped_lock l(texMutex);
            byMap[t.def->id] = std::move(img);
        };
        progress::Scope scope("Preview ball maps", 1.0, progress::Silent);
        bake::runBake(in, ps, opt, *backend);
        b.externals = std::move(byMap);
    }
    catch (const std::exception& e)
    {
        log::warn("Material thumbnails: the preview ball's mesh maps didn't bake ({}); generators read flat maps", e.what());
    }
    return b.externals;
}

void applyLook(render::FrameState& s)
{
    s.background = glm::vec3(0.16f);
    s.envBackground = false;
    s.grid = false;
    s.groundShadow = 0.0f;
    s.ambientOcclusion = true;
    s.aoRadius = 10.0f;
    s.msaaSamples = 8;
    s.lighting = render::FrameState::Lighting::Environment;
    // The viewports' environment: the renderer holds one at a time, and a different one would rebuild it per thumbnail.
    s.envPath = environments::resolve(PrefStore::get().value<std::string>("general", "/viewport/env/path", ""));
}
} // namespace

fs::path cacheDir() { return paths::userConfigDir() / "cache" / "materialThumbs"; }

fs::path cachedFile(const std::string& material, int size)
{
    try
    {
        Options o;
        o.size = size;
        fs::path f = cacheDir() / (keyOf(resolve(material), o, {}) + ".png");
        std::error_code ec;
        return fs::exists(f, ec) ? f : fs::path();
    }
    catch (const std::exception&)
    {
        return {};
    }
}

namespace
{
/** Everything a thumbnail needs, gathered on the main thread, so the rest can run on the worker. */
struct Job
{
    Result result;
    tex::Plan plan;
    std::string backend;
    render::FrameState frame;
    size_t slotCount = 1;
    int size = 256;
    /** On the preview ball, so graphs read its baked mesh maps. */
    bool ball = false;
    fs::path copyTo;
};

void copyOut(const Result& r, const fs::path& to)
{
    if (to.empty())
        return;
    std::error_code ec;
    if (to.has_parent_path())
        fs::create_directories(to.parent_path(), ec);
    fs::copy_file(r.file, to, fs::copy_options::overwrite_existing, ec);
}

/** Main thread: resolves the material and builds the plan and frame. False when the cache already has it. */
bool prepare(const std::string& material, const Options& o, Job& job)
{
    LILA_ZONE();
    Source src = resolve(material);
    tex::Graph g = library::graphOf(src.doc);
    tex::SetSettings settings = library::settingsOf(src.doc);
    job.size = std::clamp(o.size, 32, 2048);
    job.copyTo = o.copyTo;
    job.backend = o.backend;

    scene::Graph& sg = scene::Graph::current();
    scene::Node* target = nullptr;
    scene::MeshPtr mesh;
    std::string meshKey;
    if (!o.mesh.empty())
    {
        sg.evaluate();
        target = sg.find(o.mesh);
        if (!target || !target->type().isA("mesh"))
            throw std::runtime_error(o.mesh + " is not a mesh");
        mesh = target->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        if (!mesh)
            throw std::runtime_error(o.mesh + " has no mesh");
        meshKey = std::format("{}:{}:{}", target->path(), mesh->numFaces(), static_cast<const void*>(mesh.get()));
        // A library material lands at its real-world scale; a set already carries the tiling it was applied with.
        if (!src.isSet && src.meta.tileable)
        {
            glm::dmat3 lin(target->as<glm::dmat4>(scene::Transform::kWorldMatrix));
            double tiles = library::tilesFor(src.meta.scaleCm, uv::texelDensity(*mesh, geo::attr::uvName(0), {}, 1.0, lin));
            library::setTiling(g, tiles, tiles);
        }
    }
    else if (!src.isSet)
    {
        try
        {
            library::setTiling(g, kBallTiles, kBallTiles);
        }
        catch (const std::invalid_argument&)
        {
        }
    }

    job.result.key = keyOf(src, o, meshKey);
    job.result.file = cacheDir() / (job.result.key + ".png");
    std::error_code ec;
    if (!o.force && fs::exists(job.result.file, ec))
    {
        job.result.cached = true;
        copyOut(job.result, o.copyTo);
        return false;
    }

    std::vector<tex::OutputRequest> requests;
    for (const tex::OutputRequest& q : tex::channelRequests(g, settings))
        if (q.name == "baseColor" || q.name == "roughness" || q.name == "metallic" || q.name == "normal" || q.name == "ao" || q.name == "emissive")
            requests.push_back(q);
    tex::PlanRequest req;
    req.width = req.height = mesh ? std::max(job.size, 512) : kBallTexels;
    job.ball = !mesh;
    req.outputs = requests;
    job.plan = tex::buildPlan(g, req);
    if (!job.plan.error.empty())
        throw std::runtime_error(job.plan.error);

    render::MeshDraw draw;
    draw.mesh = renderMeshFor(mesh ? mesh : previewBall(), true);
    if (!draw.mesh)
        throw std::runtime_error("no render mesh");
    job.slotCount = mesh ? std::max<size_t>(1, mesh->materialNames().size()) : 1;
    draw.flags = render::DrawShaded;
    glm::dvec3 lo(draw.mesh->boundsMin), hi(draw.mesh->boundsMax);
    if (target)
    {
        glm::dmat4 world = target->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        draw.world = world;
        glm::dvec3 a(1e300), b(-1e300);
        for (int i = 0; i < 8; ++i)
        {
            glm::dvec3 c(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z);
            glm::dvec3 w = glm::dvec3(world * glm::dvec4(c, 1.0));
            a = glm::min(a, w);
            b = glm::max(b, w);
        }
        lo = a;
        hi = b;
    }

    Camera cam;
    cam.zUp = target && PrefStore::get().value<std::string>("general", "/scene/upAxis", "y") == "z";
    cam.yaw = target ? 35.0 : 0.0;
    cam.pitch = target ? -20.0 : -12.0;
    glm::dvec3 centre = (lo + hi) * 0.5;
    double radius = std::max(0.5, glm::length(hi - lo) * 0.5);
    cam.frame(centre, target ? radius : radius * 0.82, 1.0);
    cam.nearClip = std::max(0.01, std::min(cam.distance * 0.001, 1.0));

    job.frame.view = cam.view();
    job.frame.proj = cam.proj(1.0);
    job.frame.eye = cam.eye();
    job.frame.zUp = cam.zUp;
    applyLook(job.frame);
    job.frame.meshes.push_back(std::move(draw));
    return true;
}

/** Any thread but the main and render threads: evaluates the graph, renders and writes the image. Throws std::runtime_error. */
Result execute(Job& job)
{
    LILA_ZONE();
    render::Renderer& rend = render::Renderer::get();
    if (!rend.running() && !rend.start())
        throw std::runtime_error("the GPU renderer could not start");
    tex::EvalOptions options;
    if (job.ball)
        options.externals = ballExternals();
    tex::EvalResult res = tex::evaluate(job.plan, options, job.backend);
    render::Material mat = materialFrom(res, job.plan.width, job.plan.height);
    job.frame.meshes.edit().front().materials.assign(job.slotCount, mat);
    std::vector<uint8_t> bgra;
    std::string err;
    if (!rend.renderOffscreen(std::move(job.frame), job.size, job.size, bgra, &err))
        throw std::runtime_error("render failed: " + err);
    QImage img(bgra.data(), job.size, job.size, job.size * 4, QImage::Format_ARGB32);
    std::error_code ec;
    fs::create_directories(cacheDir(), ec);
    if (!img.convertToFormat(QImage::Format_RGB32).save(QString::fromStdWString(job.result.file.wstring()), "PNG"))
        throw std::runtime_error("can't write " + paths::toUtf8(job.result.file));
    copyOut(job.result, job.copyTo);
    return job.result;
}

/**
 * One worker runs thumbnail jobs in order and exits when the queue empties. The thread holds the queue alive itself, so
 * a job still running at exit never touches a destroyed static.
 */
struct Worker
{
    std::mutex mutex;
    std::deque<std::function<void()>> queue;
    bool running = false;
};

void enqueue(std::function<void()> fn)
{
    static auto worker = std::make_shared<Worker>();
    std::scoped_lock lock(worker->mutex);
    worker->queue.push_back(std::move(fn));
    if (std::exchange(worker->running, true))
        return;
    std::thread([w = worker] {
        while (true)
        {
            std::function<void()> next;
            {
                std::scoped_lock l(w->mutex);
                if (w->queue.empty())
                {
                    w->running = false;
                    return;
                }
                next = std::move(w->queue.front());
                w->queue.pop_front();
            }
            next();
        }
    }).detach();
}
} // namespace

void renderAsync(const std::string& material, const Options& o, Done done)
{
    auto job = std::make_shared<Job>();
    try
    {
        if (!prepare(material, o, *job))
        {
            mainq::post([done, r = job->result] { done(r, {}); });
            return;
        }
    }
    catch (const std::exception& e)
    {
        mainq::post([done, err = std::string(e.what())] { done({}, err); });
        return;
    }
    enqueue([job, done] {
        try
        {
            Result r = execute(*job);
            mainq::post([done, r] { done(r, {}); });
        }
        catch (const std::exception& e)
        {
            mainq::post([done, err = std::string(e.what())] { done({}, err); });
        }
    });
}

Result render(const std::string& material, const Options& o)
{
    LILA_ZONE();
    auto job = std::make_shared<Job>();
    if (!prepare(material, o, *job))
        return job->result;
    auto promise = std::make_shared<std::promise<Result>>();
    std::future<Result> result = promise->get_future();
    enqueue([job, promise] {
        try
        {
            promise->set_value(execute(*job));
        }
        catch (...)
        {
            promise->set_exception(std::current_exception());
        }
    });
    // The window thread keeps its messages flowing while it waits, so nothing the render thread does can wait on it.
    const bool pump = mainq::isMainThread() && QCoreApplication::instance();
    while (result.wait_for(std::chrono::milliseconds(pump ? 5 : 50)) != std::future_status::ready)
        if (pump)
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    return result.get();
}

void registerCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "renderMaterialThumbnail",
        .doc = "Render a material's thumbnail offscreen on the preview ball (or mesh= of the scene, at the material's "
               "real-world scale) and cache it by graph hash. material is a library id, .lilamat path, texture set or "
               "smart:<name> (a smart material or mask); all=True "
               "renders every library material. Returns {file, cached} (a list with all).",
        .positionalDoc = "material",
        .flags = {{"size", "", T::Int, 256, "Image size, px.", {}, {}, 32.0, 2048.0},
                  nodeFlag({"mesh", "", T::String, nullptr, "Render on this mesh object instead of the ball."}, "mesh"),
                  {"path", "", T::String, nullptr, "Also copy the image here."},
                  {"force", "", T::Bool, false, "Render even when cached."},
                  {"backend", "", T::String, nullptr, "gpu or cpu for the graph.", {"gpu", "cpu"}},
                  {"all", "", T::Bool, false, "Every library material (the scene's sets excluded)."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Options o;
            o.size = a.get<int>("size", 256);
            o.mesh = a.get<std::string>("mesh", "");
            o.force = a.get<bool>("force", false);
            o.backend = a.get<std::string>("backend", "");
            auto one = [&](const std::string& m) -> json {
                Result r = render(m, o);
                return {{"material", m}, {"file", paths::toUtf8(r.file)}, {"cached", r.cached}};
            };
            if (a.get<bool>("all", false))
            {
                json out = json::array();
                for (const library::Entry& e : library::all())
                {
                    if (e.tier == "scene")
                        continue;
                    try
                    {
                        out.push_back(one(e.id));
                    }
                    catch (const std::exception& ex)
                    {
                        out.push_back({{"material", e.id}, {"error", ex.what()}});
                    }
                }
                return out;
            }
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("renderMaterialThumbnail: material required");
            if (a.has("path"))
                o.copyTo = paths::fromUtf8(a.get<std::string>("path"));
            try
            {
                return one(a.positional[0].get<std::string>());
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("renderMaterialThumbnail: ") + e.what());
            }
        },
    });
}
} // namespace lila::ui::materialThumbs
