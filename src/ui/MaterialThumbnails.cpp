#include "ui/MaterialThumbnails.h"

#include "core/MainQueue.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Attribute.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "ops/Command.h"
#include "render/Renderer.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Node.h"
#include "tex/Evaluate.h"
#include "tex/Library.h"
#include "tex/Lint.h"
#include "tex/Plan.h"
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

/** Tiles across the ball's uvs: u runs around it, twice as far as v runs pole to pole, so 4 x 2 keeps texels square. */
constexpr float kBallTilesU = 4.0f;
constexpr float kBallTilesV = 2.0f;
constexpr const char* kVersion = "ball-v1";

struct Source
{
    json doc;
    library::Meta meta;
    /** Set when the material is a texture set of the scene. */
    bool isSet = false;
};

Source resolve(const std::string& material)
{
    Source s;
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

scene::MeshPtr shaderBall()
{
    static scene::MeshPtr ball = [] {
        auto m = std::make_shared<geo::Mesh>(geo::makeSphere({.radius = 50.0, .subdivAxis = 64, .subdivHeight = 32}));
        for (glm::vec2& uv : m->cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0)))
            uv *= glm::vec2(kBallTilesU, kBallTilesV);
        return scene::MeshPtr(m);
    }();
    return ball;
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
    else if (tex::GNode* t = g.node(library::kTilingNode))
    {
        t->values["tilesX"] = 1.0;
        t->values["tilesY"] = 1.0;
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
    req.width = req.height = mesh ? std::max(job.size, 512) : job.size;
    req.outputs = requests;
    job.plan = tex::buildPlan(g, req);
    if (!job.plan.error.empty())
        throw std::runtime_error(job.plan.error);

    render::MeshDraw draw;
    draw.mesh = renderMeshFor(mesh ? mesh : shaderBall(), true);
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
    tex::EvalResult res = tex::evaluate(job.plan, tex::EvalOptions{}, job.backend);
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
        .doc = "Render a material's thumbnail offscreen on the standard shader ball (or mesh= of the scene, at the material's "
               "real-world scale) and cache it by graph hash. material is a library id, .lilamat path or texture set; all=True "
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
