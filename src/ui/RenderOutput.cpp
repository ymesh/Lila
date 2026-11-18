#include "ui/RenderOutput.h"

#include <mutex>

#include "core/PrefStore.h"
#include "core/ImageIO.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "core/VideoWriter.h"
#include "io/SceneFile.h"
#include "ops/Command.h"
#include "render/Ocio.h"
#include "render/Renderer.h"
#include "scene/ColorManagement.h"
#include "scene/Anim.h"
#include "scene/Camera.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/NodeBounds.h"
#include "scene/RenderSettings.h"
#include "scene/Transform.h"
#include "scene/Turntable.h"
#include "ui/Camera.h"
#include "ui/TextureCache.h"
#include "ui/ViewportWidget.h"

#include <glm/gtc/matrix_transform.hpp>

#include <QApplication>
#include <QDir>
#include <QImage>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <cmath>
#include <format>
#include <stdexcept>

namespace lila::ui::renderOutput
{
namespace
{
ViewportWidget* activeViewport()
{
    if (!qobject_cast<QApplication*>(QCoreApplication::instance()))
        return nullptr;
    ViewportWidget* visible = nullptr;
    for (QWidget* top : QApplication::topLevelWidgets())
        for (ViewportWidget* v : top->findChildren<ViewportWidget*>())
            if (v->isVisible())
            {
                if (v->hasFocus())
                    return v;
                if (!visible)
                    visible = v;
            }
    return visible;
}

/** The camera node a render looks through, by name or the active view's; null for a built-in view. */
const scene::Node* cameraNode(const scene::Graph& g, const Options& o, const ViewportWidget& vp)
{
    if (!o.camera.empty())
    {
        const scene::Node* n = g.find(o.camera);
        if (!n || !n->type().isA("camera"))
            throw std::runtime_error("no camera called '" + o.camera + "'");
        return n;
    }
    return vp.lookThroughCamera() != scene::kNoNode ? g.node(vp.lookThroughCamera()) : nullptr;
}

/** What a turntable turns around: the selection's bounds, else everything shown. */
geo::Bounds subjectBounds(scene::Graph& g)
{
    geo::Bounds b;
    auto add = [&](scene::NodeId id) {
        const scene::Node* n = g.node(id);
        if (n && n->type().isA("mesh") && scene::effectivelyVisible(g, id, true))
            if (geo::Bounds wb = scene::worldBounds(g, id); wb.valid())
            {
                b.extend(wb.min);
                b.extend(wb.max);
            }
    };
    for (scene::NodeId id : g.selection())
    {
        add(id);
        for (scene::NodeId d : g.descendants(id))
            add(d);
    }
    if (!b.valid())
        for (scene::NodeId id : g.nodes())
            add(id);
    return b;
}

/** cam orbiting the subject at yaw and pitch, framed so it fits at aspect. */
void orbitCamera(Camera& cam, scene::Graph& g, double yaw, double pitch, double aspect)
{
    const geo::Bounds b = subjectBounds(g);
    cam.ortho = false;
    cam.yaw = yaw;
    cam.pitch = std::clamp(pitch, -89.0, 89.0);
    cam.filmFit = scene::FilmFit::Fill;
    cam.overscan = 1.0;
    cam.gateAspect.reset();
    if (b.valid())
        cam.frame(b.center(), std::max(0.5, glm::length(b.size()) * 0.5), aspect);
    cam.nearClip = std::max(0.01, std::min(cam.distance * 0.001, 1.0));
}

/**
 * Turns the environment and the scene's lights by degrees around the up axis through the subject's centre, as a
 * turntable spinning the light does. The lights' shadow maps stay fit to the scene, which turns inside them.
 */
void spinLighting(render::FrameState& s, scene::Graph& g, double degrees)
{
    s.envRotation += float(degrees);
    const geo::Bounds b = subjectBounds(g);
    const glm::dvec3 centre = b.valid() ? glm::dvec3(b.center()) : glm::dvec3(0.0);
    const glm::dvec3 up = s.zUp ? glm::dvec3(0, 0, 1) : glm::dvec3(0, 1, 0);
    // The environment turns the other way round the up axis from a rotation of the world by the same angle.
    const glm::dmat4 turn = glm::rotate(glm::dmat4(1.0), glm::radians(-degrees), up);
    const glm::dmat4 about = glm::translate(glm::dmat4(1.0), centre) * turn * glm::translate(glm::dmat4(1.0), -centre);
    for (render::LightDraw& l : s.lights)
    {
        l.position = glm::dvec3(about * glm::dvec4(l.position, 1.0));
        l.direction = glm::vec3(turn * glm::dvec4(l.direction, 0.0));
        l.right = glm::vec3(turn * glm::dvec4(l.right, 0.0));
        l.shadowViewProj = l.shadowViewProj * glm::inverse(about);
    }
}

/** s as the id pass draws it: unlit flat colours per object over black, with nothing between them and the file. */
void makeIdPass(render::FrameState& s, const scene::Graph& g)
{
    s.lighting = render::FrameState::Lighting::Flat;
    s.exposure = 1.0f;
    s.gamma = 1.0f;
    s.post = {};
    s.post.tonemapper = render::PostSettings::Linear;
    s.post.dither = false;
    s.fog.on = false;
    s.lights.clear();
    s.envBackground = false;
    s.background = glm::vec3(0.0f);
    s.ambientOcclusion = false;
    s.groundShadow = 0.0f;
    s.lensRadius = 0.0f;
    for (render::MeshDraw& d : s.meshes.edit())
    {
        const scene::Node* n = d.node ? g.node(scene::NodeId(d.node)) : nullptr;
        d.baseColor = idColor(n ? n->path() : std::string());
        d.materials.clear();
        d.vertexColors = render::VertexColorMode::Off;
    }
}

/** Restores the scene's time when a render that stepped through it ends, however it ends. */
struct TimeGuard
{
    double saved;
    ~TimeGuard()
    {
        scene::Graph& g = scene::Graph::current();
        if (g.time() != saved)
        {
            g.setTime(saved);
            g.evaluate();
        }
    }
};

std::string lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

} // namespace

std::string framePath(const std::string& pattern, int frame)
{
    std::string p = pattern;
    size_t first = p.find('#');
    if (first == std::string::npos)
    {
        const size_t dot = p.find_last_of('.');
        const size_t slash = p.find_last_of("/\\");
        const size_t at = dot != std::string::npos && (slash == std::string::npos || dot > slash) ? dot : p.size();
        p.insert(at, ".####");
        first = at + 1;
    }
    size_t last = first;
    while (last < p.size() && p[last] == '#')
        ++last;
    std::string number = std::format("{:0{}}", frame, int(last - first));
    return p.replace(first, last - first, number);
}

glm::vec3 idColor(const std::string& path)
{
    uint32_t h = 2166136261u;
    for (unsigned char ch : path)
        h = (h ^ ch) * 16777619u;
    // Golden-ratio hue steps from the hash, at full saturation and a value well inside the linear range.
    const float hue = std::fmod(float(h & 0xffff) / 65536.0f * 0.618034f * 7.0f, 1.0f);
    const float v = 0.35f + 0.3f * float((h >> 16) & 0xff) / 255.0f;
    const float k = hue * 6.0f;
    const glm::vec3 rgb = glm::clamp(glm::abs(glm::mod(k + glm::vec3(0.0f, 4.0f, 2.0f), 6.0f) - 3.0f) - 1.0f, 0.0f, 1.0f);
    return rgb * v;
}

std::string defaultPattern()
{
    const std::filesystem::path scene = io::currentPath();
    const std::filesystem::path dir = scene.empty() ? paths::fromUtf8(QDir::homePath().toStdString()) / "Lila Renders" : scene.parent_path() / "renders";
    const std::string stem = scene.empty() ? "untitled" : scene.stem().string();
    return (dir / (stem + ".####.png")).generic_string();
}

std::string defaultVideo()
{
    std::string p = defaultPattern();
    const size_t hash = p.find('#');
    if (hash != std::string::npos)
        p.erase(hash > 0 && p[hash - 1] == '.' ? hash - 1 : hash);
    return p + ".mp4";
}

std::string withSuffix(std::string path, const std::string& suffix)
{
    if (suffix.empty() || path.empty())
        return path;
    const size_t slash = path.find_last_of("/\\");
    const size_t name = slash == std::string::npos ? 0 : slash + 1;
    const size_t hash = path.find('#', name);
    const size_t dot = path.find_last_of('.');
    size_t at = hash != std::string::npos ? hash : dot != std::string::npos && dot > name ? dot : path.size();
    if (hash != std::string::npos && at > name && (path[at - 1] == '.' || path[at - 1] == '_'))
        --at;
    path.insert(at, suffix);
    return path;
}

namespace
{
/** A flag's value, else the Render panel's setting for it (saved with the scene), else fallback. */
json setting(const Args& a, const char* key, const json& fallback)
{
    if (a.has(key))
        return a.flags[key];
    const json& o = scene::renderSettings().output;
    if (auto it = o.find(key); it != o.end() && !it->is_null() && (!it->is_string() || !it->get<std::string>().empty()))
        return *it;
    return fallback;
}

/** Where a run of frames goes: path and video when the call names either, else the Render panel's by its Write choice. */
std::pair<std::string, std::string> targets(const Args& a, const std::string& suffix)
{
    if (a.has("path") || a.has("video"))
        return {a.get<std::string>("path", ""), a.get<std::string>("video", "")};
    const std::string what = setting(a, "write", "video").get<std::string>();
    std::pair<std::string, std::string> out;
    if (what != "video")
        out.first = withSuffix(setting(a, "path", defaultPattern()).get<std::string>(), suffix);
    if (what != "images")
        out.second = withSuffix(setting(a, "video", defaultVideo()).get<std::string>(), suffix);
    return out;
}
} // namespace

namespace
{
/** MurmurHash3 (x86, 32-bit) of a name, as Cryptomatte hashes object and material names. */
uint32_t murmur3(std::string_view s, uint32_t seed = 0)
{
    const auto* data = reinterpret_cast<const uint8_t*>(s.data());
    const size_t n = s.size();
    uint32_t h = seed;
    const uint32_t c1 = 0xcc9e2d51, c2 = 0x1b873593;
    auto rotl = [](uint32_t x, int r) { return (x << r) | (x >> (32 - r)); };
    size_t i = 0;
    for (; i + 4 <= n; i += 4)
    {
        uint32_t k;
        std::memcpy(&k, data + i, 4);
        k *= c1;
        k = rotl(k, 15);
        k *= c2;
        h ^= k;
        h = rotl(h, 13);
        h = h * 5 + 0xe6546b64;
    }
    uint32_t k = 0;
    switch (n & 3)
    {
    case 3: k ^= uint32_t(data[i + 2]) << 16; [[fallthrough]];
    case 2: k ^= uint32_t(data[i + 1]) << 8; [[fallthrough]];
    case 1:
        k ^= data[i];
        k *= c1;
        k = rotl(k, 15);
        k *= c2;
        h ^= k;
    }
    h ^= uint32_t(n);
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

/** A Cryptomatte id: the name's hash as a float, kept off denormals, infinities and NaNs as the spec asks. */
uint32_t cryptoHash(const std::string& name)
{
    uint32_t h = murmur3(name);
    const uint32_t exponent = (h >> 23) & 255;
    if (exponent == 0 || exponent == 255)
        h ^= 1u << 23;
    return h;
}

/** The colour a flat id render writes for index i (1-based), as the 8-bit sRGB code values r, g, b of its bytes. */
glm::vec3 indexColor(uint32_t i)
{
    auto lin = [](uint32_t code) {
        const float c = float(code) / 255.0f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    return {lin(i & 255u), lin((i >> 8) & 255u), lin((i >> 16) & 255u)};
}

/**
 * One Cryptomatte layer: renders the frame flat-coloured by object (or material) at a supersampled size, then per output
 * pixel ranks the names that cover it by coverage. Writes `name`00 and `name`01 (four ranks) and the manifest attributes.
 */
void cryptoLayer(const render::FrameState& base, const scene::Graph& g, bool byMaterial, int w, int h, const std::string& name,
                 std::vector<image::ExrChannel>& channels, std::vector<image::ExrAttribute>& attrs)
{
    render::FrameState s = base;
    std::vector<std::string> names{""};
    std::unordered_map<std::string, uint32_t> index;
    auto idOf = [&](const std::string& n) {
        auto [it, added] = index.emplace(n, uint32_t(names.size()));
        if (added)
            names.push_back(n);
        return it->second;
    };
    s.lighting = render::FrameState::Lighting::Flat;
    s.exposure = 1.0f;
    s.gamma = 1.0f;
    s.post = {};
    s.post.tonemapper = render::PostSettings::Linear;
    s.post.dither = false;
    s.fog.on = false;
    s.lights.clear();
    s.envBackground = false;
    s.background = glm::vec3(0.0f);
    s.transparentBackground = false;
    s.ambientOcclusion = false;
    s.groundShadow = 0.0f;
    s.lensRadius = 0.0f;
    s.taa = false;
    s.accumulateOnly = false;
    s.reflections = render::FrameState::Reflections::Off;
    s.viewportDof = s.viewportMotionBlur = false;
    for (render::MeshDraw& d : s.meshes.edit())
    {
        const scene::Node* n = d.node ? g.node(scene::NodeId(d.node)) : nullptr;
        d.vertexColors = render::VertexColorMode::Off;
        if (!byMaterial)
        {
            d.baseColor = indexColor(idOf(n ? n->name() : std::string()));
            d.materials.clear();
            continue;
        }
        const scene::MeshPtr* mesh = n && n->type().isA("mesh") ? &n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh) : nullptr;
        std::vector<render::Material> flat;
        for (size_t slot = 0; slot < std::max<size_t>(d.materials.size(), 1); ++slot)
        {
            render::Material m;
            m.baseColor = indexColor(idOf(mesh && *mesh ? scene::materials::slotName(**mesh, int32_t(slot)) : std::string("Default")));
            flat.push_back(m);
        }
        d.baseColor = flat.front().baseColor;
        d.materials = std::move(flat);
    }
    // Several samples per pixel, as many as the size limit allows, give each name's coverage.
    const int ss = std::clamp(16384 / std::max(w, h), 1, 4);
    render::Renderer::OffscreenResult res;
    std::string err;
    if (!render::Renderer::get().renderOffscreenFrames({s}, w * ss, h * ss, 1, false, res, &err))
        throw std::runtime_error("cryptomatte render failed: " + err);
    const int W = w * ss;
    const size_t count = size_t(w) * h;
    std::array<std::vector<float>, 8> out;
    for (auto& c : out)
        c.assign(count, 0.0f);
    tbb::parallel_for(0, h, [&](int y) {
        for (int x = 0; x < w; ++x)
        {
            std::array<std::pair<uint32_t, int>, 16> hits{};
            int kinds = 0, total = 0;
            for (int sy = 0; sy < ss; ++sy)
                for (int sx = 0; sx < ss; ++sx)
                {
                    const uint8_t* p = res.bgra.data() + (size_t(y * ss + sy) * W + size_t(x * ss + sx)) * 4;
                    const uint32_t id = uint32_t(p[2]) | uint32_t(p[1]) << 8 | uint32_t(p[0]) << 16;
                    ++total;
                    // Edges the multisampling blended decode to no name, or the wrong one rarely; both are skipped as background.
                    if (id == 0 || id >= names.size())
                        continue;
                    int k = 0;
                    while (k < kinds && hits[size_t(k)].first != id)
                        ++k;
                    if (k == kinds && kinds < 16)
                        hits[size_t(kinds++)] = {id, 0};
                    if (k < kinds)
                        ++hits[size_t(k)].second;
                }
            std::sort(hits.begin(), hits.begin() + kinds, [](const auto& a, const auto& b) { return a.second > b.second; });
            const size_t i = size_t(y) * w + x;
            for (int r = 0; r < std::min(kinds, 4); ++r)
            {
                out[size_t(r) * 2][i] = std::bit_cast<float>(cryptoHash(names[hits[size_t(r)].first]));
                out[size_t(r) * 2 + 1][i] = float(hits[size_t(r)].second) / float(total);
            }
        }
    });
    static const char* kRgba[] = {"R", "G", "B", "A"};
    for (int c = 0; c < 8; ++c)
        channels.push_back({name + (c < 4 ? "00." : "01.") + kRgba[c % 4], std::move(out[size_t(c)]), true});
    json manifest = json::object();
    for (size_t i = 1; i < names.size(); ++i)
    {
        char hex[9];
        std::snprintf(hex, sizeof(hex), "%08x", cryptoHash(names[i]));
        manifest[names[i]] = hex;
    }
    char key[9];
    std::snprintf(key, sizeof(key), "%08x", murmur3(name));
    const std::string prefix = "cryptomatte/" + std::string(key, 7) + "/";
    attrs.push_back(image::exrString(prefix + "name", name));
    attrs.push_back(image::exrString(prefix + "hash", "MurmurHash3_32"));
    attrs.push_back(image::exrString(prefix + "conversion", "uint32_to_float32"));
    attrs.push_back(image::exrString(prefix + "manifest", manifest.dump()));
}

/**
 * Depth (cm along the view, 1e10 where nothing was drawn), world normals from the depth, and the camera's motion since
 * the previous frame (pixels, x right, y up), from the beauty's depth readback.
 */
void depthLayers(const std::vector<float>& depth, int w, int h, const glm::dmat4& viewProj, const glm::dmat4& proj, const std::optional<glm::dmat4>& prevViewProj,
                 const std::vector<std::string>& passes, std::vector<image::ExrChannel>& channels)
{
    if (depth.size() != size_t(w) * h)
        return;
    const glm::dmat4 inv = glm::inverse(viewProj), invProj = glm::inverse(proj);
    auto want = [&](const char* p) { return std::ranges::find(passes, p) != passes.end(); };
    auto world = [&](int x, int y) {
        const double d = depth[size_t(y) * w + x];
        const glm::dvec4 q = inv * glm::dvec4((x + 0.5) / w * 2.0 - 1.0, (y + 0.5) / h * 2.0 - 1.0, d, 1.0);
        return glm::dvec3(q) / q.w;
    };
    const size_t count = size_t(w) * h;
    if (want("depth"))
    {
        std::vector<float> z(count);
        for (size_t i = 0; i < count; ++i)
        {
            const double d = depth[i];
            if (d <= 0.0)
            {
                z[i] = 1e10f;
                continue;
            }
            const int x = int(i % w), y = int(i / w);
            const glm::dvec4 v = invProj * glm::dvec4((x + 0.5) / w * 2.0 - 1.0, (y + 0.5) / h * 2.0 - 1.0, d, 1.0);
            z[i] = float(-v.z / v.w);
        }
        channels.push_back({"depth.Z", std::move(z), true});
    }
    if (want("normal"))
    {
        std::array<std::vector<float>, 3> n;
        for (auto& c : n)
            c.assign(count, 0.0f);
        tbb::parallel_for(0, h, [&](int y) {
            for (int x = 0; x < w; ++x)
            {
                if (depth[size_t(y) * w + x] <= 0.0)
                    continue;
                const glm::dvec3 p = world(x, y);
                // The nearer neighbour on each axis, so edges keep their own surface's normal.
                auto side = [&](int dx, int dy) -> std::optional<glm::dvec3> {
                    const int qx = x + dx, qy = y + dy;
                    if (qx < 0 || qy < 0 || qx >= w || qy >= h || depth[size_t(qy) * w + qx] <= 0.0)
                        return std::nullopt;
                    return world(qx, qy) - p;
                };
                auto pick = [&](std::optional<glm::dvec3> a, std::optional<glm::dvec3> b, double sign) -> glm::dvec3 {
                    if (a && b)
                        return glm::length(*a) < glm::length(*b) ? *a : -*b * sign * sign;
                    return a ? *a : b ? -*b : glm::dvec3(0.0);
                };
                const glm::dvec3 dx = pick(side(1, 0), side(-1, 0), 1.0), dy = pick(side(0, 1), side(0, -1), 1.0);
                glm::dvec3 nn = glm::cross(dy, dx);
                if (glm::dot(nn, nn) < 1e-20)
                    continue;
                nn = glm::normalize(nn);
                const glm::dvec4 eye = inv * glm::dvec4(0.0, 0.0, 1.0, 1.0);
                if (glm::dot(nn, glm::dvec3(eye) / eye.w - p) < 0.0)
                    nn = -nn;
                const size_t i = size_t(y) * w + x;
                for (int c = 0; c < 3; ++c)
                    n[size_t(c)][i] = float(nn[c]);
            }
        });
        for (int c = 0; c < 3; ++c)
            channels.push_back({std::string("N.") + "XYZ"[c], std::move(n[size_t(c)]), false});
    }
    if (want("motion") && prevViewProj)
    {
        std::vector<float> mx(count, 0.0f), my(count, 0.0f);
        tbb::parallel_for(0, h, [&](int y) {
            for (int x = 0; x < w; ++x)
            {
                const size_t i = size_t(y) * w + x;
                const double d = std::max(double(depth[i]), 1e-7);
                const glm::dvec4 q = inv * glm::dvec4((x + 0.5) / w * 2.0 - 1.0, (y + 0.5) / h * 2.0 - 1.0, d, 1.0);
                const glm::dvec4 prev = *prevViewProj * glm::dvec4(glm::dvec3(q) / q.w, 1.0);
                if (prev.w <= 0.0)
                    continue;
                const double px = (prev.x / prev.w * 0.5 + 0.5) * w, py = (prev.y / prev.w * 0.5 + 0.5) * h;
                mx[i] = float(x + 0.5 - px);
                my[i] = float(py - (y + 0.5));
            }
        });
        channels.push_back({"motion.X", std::move(mx), false});
        channels.push_back({"motion.Y", std::move(my), false});
    }
}
} // namespace

Frame render(const Options& o, double seconds)
{
    LILA_ZONE();

    render::Renderer& r = render::Renderer::get();
    if (!r.running() && !r.start())
        throw std::runtime_error("the GPU renderer could not start");
    ViewportWidget* vp = activeViewport();
    if (!vp)
        throw std::runtime_error("rendering needs a viewport (the Lila window)");
    scene::Graph& g = scene::Graph::current();
    const scene::RenderSettings& settings = scene::renderSettings();
    const double scale = std::clamp(o.scale, 0.05, 8.0);
    const int w = std::clamp(int(std::lround((o.width > 0 ? o.width : settings.width) * scale)), 16, 16384);
    const int h = std::clamp(int(std::lround((o.height > 0 ? o.height : settings.height) * scale)), 16, 16384);

    TimeGuard restore{g.time()};
    const int samples = std::max(o.samples, 1);
    const scene::Node* camNode = cameraNode(g, o, *vp);
    double shutter = 0.0;
    if (o.motionBlur)
        shutter = (camNode ? scene::camera::lens(*camNode).shutterAngle : 180.0) / 360.0 / std::max(scene::anim::playback().fps, 1.0);
    const int moments = o.motionBlur && shutter > 0.0 ? std::clamp(o.motionSamples, 1, samples) : 1;

    std::vector<render::FrameState> frames;
    for (int k = 0; k < moments; ++k)
    {
        // Moments spread evenly over a shutter centred on the frame.
        const double t = moments > 1 ? seconds + shutter * ((double(k) + 0.5) / double(moments) - 0.5) : seconds;
        if (g.time() != t)
            g.setTime(t, false);
        g.evaluate();
        ViewportWidget::RenderView view;
        view.width = w;
        view.height = h;
        view.camera = vp->camera();
        if (o.orbit)
            orbitCamera(view.camera, g, o.orbitYaw, o.orbitPitch, double(w) / double(h));
        else if (const scene::Node* cam = cameraNode(g, o, *vp))
        {
            view.camera.lookThrough(*cam, cam->as<glm::dmat4>(scene::Transform::kWorldMatrix));
            view.lens = scene::camera::lens(*cam);
        }
        render::FrameState s = vp->renderFrame(view);
        if (o.environmentSpin != 0.0)
            spinLighting(s, g, o.environmentSpin);
        if (o.pass == "id")
            makeIdPass(s, g);
        else if (o.pass == "wireframe")
            for (render::MeshDraw& d : s.meshes.edit())
            {
                d.flags |= render::DrawWire;
                d.wireColor = glm::vec4(0.02f, 0.02f, 0.02f, 1.0f);
            }
        s.taa = true;
        s.accumulateOnly = true;
        s.transparentBackground = o.transparent;
        s.post.effects = true;
        frames.push_back(std::move(s));
    }
    textures::waitForLoads(30.0);

    // The passes follow the middle moment of the shutter, as the beauty's sharpest point does.
    const render::FrameState middle = frames[frames.size() / 2];
    std::optional<glm::dmat4> prevViewProj;
    if (std::ranges::find(o.passes, "motion") != o.passes.end())
        if (const scene::Node* cam = cameraNode(g, o, *vp); cam && !o.orbit)
        {
            const double fps = std::max(scene::anim::playback().fps, 1.0);
            g.setTime(seconds - 1.0 / fps, false);
            g.evaluate();
            Camera prev = vp->camera();
            prev.lookThrough(*cam, cam->as<glm::dmat4>(scene::Transform::kWorldMatrix));
            prevViewProj = prev.proj(double(w) / double(h)) * prev.view();
            g.setTime(seconds, false);
            g.evaluate();
        }
    render::Renderer::OffscreenResult result;
    std::string err;
    const int each = (samples + moments - 1) / moments;
    const bool depthWanted = std::ranges::any_of(o.passes, [](const std::string& p) { return p == "depth" || p == "normal" || p == "motion"; });
    if (!r.renderOffscreenFrames(std::move(frames), w, h, each, o.hdr || depthWanted, result, &err))
        throw std::runtime_error("render failed: " + err);
    Frame f;
    f.width = w;
    f.height = h;
    f.bgra = std::move(result.bgra);
    f.hdr = std::move(result.hdr);
    if (depthWanted)
        depthLayers(result.depth, w, h, middle.proj * middle.view, middle.proj, prevViewProj, o.passes, f.layers);
    if (std::ranges::find(o.passes, "cryptoObject") != o.passes.end())
        cryptoLayer(middle, g, false, w, h, "CryptoObject", f.layers, f.layerAttrs);
    if (std::ranges::find(o.passes, "cryptoMaterial") != o.passes.end())
        cryptoLayer(middle, g, true, w, h, "CryptoMaterial", f.layers, f.layerAttrs);
    return f;
}

namespace
{
std::mutex gLastMutex;
QImage gLast;
} // namespace

QImage lastImage()
{
    std::lock_guard lock(gLastMutex);
    return gLast;
}

void write(const Frame& f, const std::string& path, bool transparent)
{
    {
        std::lock_guard lock(gLastMutex);
        gLast = QImage(f.bgra.data(), f.width, f.height, f.width * 4, QImage::Format_ARGB32).copy();
    }
    const std::string ext = lower(paths::fromUtf8(path).extension().string());
    std::error_code ec;
    if (auto parent = paths::fromUtf8(path).parent_path(); !parent.empty())
        std::filesystem::create_directories(parent, ec);
    if (ext == ".jpg" || ext == ".jpeg")
    {
        QImage img(f.bgra.data(), f.width, f.height, f.width * 4, QImage::Format_ARGB32);
        if (!img.convertToFormat(QImage::Format_RGB32).save(QString::fromStdString(path), "JPG", 95))
            throw std::runtime_error("cannot write " + path);
        return;
    }
    image::Image out;
    out.width = f.width;
    out.height = f.height;
    out.rgba.resize(size_t(f.width) * f.height * 4);
    const size_t count = size_t(f.width) * f.height;
    // EXR keeps scene-linear light for compositing unless colour management asks for the picture as the view shows it.
    namespace cm = scene::colorManagement;
    const bool exrDisplay = cm::settings(scene::Graph::current(), PrefStore::get().value<std::string>("general", cm::kConfigPref, "")).exrDisplay;
    if (ext == ".exr" && !f.hdr.empty() && !exrDisplay)
        out.rgba = f.hdr;
    else
        for (size_t i = 0; i < count; ++i)
        {
            const float a = transparent ? f.bgra[i * 4 + 3] / 255.0f : 1.0f;
            // The picture is over black where coverage is partial; straight alpha divides that back out.
            const float unmul = a > 1e-3f ? 1.0f / a : 0.0f;
            out.rgba[i * 4 + 0] = std::min(f.bgra[i * 4 + 2] / 255.0f * unmul, 1.0f);
            out.rgba[i * 4 + 1] = std::min(f.bgra[i * 4 + 1] / 255.0f * unmul, 1.0f);
            out.rgba[i * 4 + 2] = std::min(f.bgra[i * 4 + 0] / 255.0f * unmul, 1.0f);
            out.rgba[i * 4 + 3] = a;
        }
    if (ext == ".exr" && !f.layers.empty())
    {
        // Multilayer: the beauty as R, G, B, A, the passes beside it under their layer names.
        std::vector<image::ExrChannel> channels;
        const size_t px = size_t(f.width) * f.height;
        for (int c = 0; c < 4; ++c)
        {
            image::ExrChannel ch{std::string(1, "RGBA"[c]), std::vector<float>(px), false};
            for (size_t i = 0; i < px; ++i)
                ch.data[i] = out.rgba[i * 4 + size_t(c)];
            channels.push_back(std::move(ch));
        }
        for (const image::ExrChannel& l : f.layers)
            channels.push_back(l);
        std::vector<image::ExrAttribute> attrs = f.layerAttrs;
        attrs.push_back(image::exrChromaticitiesRec709());
        image::writeExrChannels(paths::fromUtf8(path), f.width, f.height, std::move(channels), attrs);
        return;
    }
    if (!f.layers.empty())
        log::warn("render: compositing passes need an .exr; {} has the beauty only", path);
    image::WriteOptions wo;
    wo.bits = ext == ".exr" ? 16 : 8;
    image::write(paths::fromUtf8(path), out, wo);
}

namespace
{
/** Where a run of frames goes: numbered images, an .mp4, or both. */
struct Sequence
{
    const char* command = "";
    std::string pattern, video, codec = "h264";
    double quality = 0.6, fps = 24.0;
    int count = 0;
    bool transparent = false;
};

/**
 * Renders seq.count frames with renderAt(i) (the frame and the number its image is named by) and writes them, with a
 * cancellable progress bar. Returns {frames, path (first and last image), video}.
 */
json writeSequence(const Sequence& seq, const std::function<std::pair<Frame, int>(int)>& renderAt)
{
    progress::Scope scope("Rendering", double(seq.count), progress::Cancellable);
    std::unique_ptr<video::Writer> encoder;
    json written = json::array();
    try
    {
        for (int i = 0; i < seq.count; ++i)
        {
            scope.checkCancel();
            scope.setStatus(std::format("frame {} of {}", i + 1, seq.count));
            auto [f, number] = renderAt(i);
            if (!seq.pattern.empty())
            {
                const std::string path = framePath(seq.pattern, number);
                write(f, path, seq.transparent);
                if (i == 0 || i + 1 == seq.count)
                    written.push_back(path);
            }
            if (!seq.video.empty())
            {
                std::string err;
                if (!encoder)
                {
                    video::Options vo;
                    vo.width = f.width;
                    vo.height = f.height;
                    vo.fps = seq.fps;
                    vo.codec = seq.codec;
                    vo.quality = seq.quality;
                    if (auto parent = paths::fromUtf8(seq.video).parent_path(); !parent.empty())
                    {
                        std::error_code ec;
                        std::filesystem::create_directories(parent, ec);
                    }
                    encoder = video::Writer::open(paths::fromUtf8(seq.video), vo, &err);
                    if (!encoder)
                        throw std::runtime_error(err);
                }
                if (!encoder->add(f.bgra.data(), &err))
                    throw std::runtime_error(err);
            }
            scope.step();
        }
        if (encoder)
        {
            std::string err;
            if (!encoder->finish(&err))
                throw std::runtime_error(err);
        }
    }
    catch (const progress::Cancelled&)
    {
        if (encoder)
            encoder->finish();
        throw CommandError(std::string(seq.command) + ": cancelled");
    }
    catch (const std::runtime_error& e)
    {
        throw CommandError(std::string(seq.command) + ": " + e.what());
    }
    return json{{"frames", seq.count}, {"path", written}, {"video", seq.video}};
}

/**
 * renderTurntable with spin=turntable: the turntable node turns its members (and the lights, with lightsFollow) one loop
 * through the view's own camera, as a presenter framed it. Its live state is put back afterwards.
 */
json renderTurntableNode(const Args& a, Options o, const Sequence& seq, int frames, double turns, double seconds)
{
    namespace tt = scene::turntable;
    scene::Graph& g = scene::Graph::current();
    tt::Turntable* t = nullptr;
    if (a.has("turntable"))
    {
        scene::Node* n = g.find(a.get<std::string>("turntable"));
        t = n ? tt::get(g, n->id()) : nullptr;
        if (!t)
            throw CommandError("renderTurntable: no turntable called '" + a.get<std::string>("turntable") + "'");
    }
    for (scene::NodeId id : g.selection())
        if (!t)
            if (scene::NodeId on = tt::get(g, id) ? id : tt::turntableOf(g, id); on != scene::kNoNode)
                t = tt::get(g, on);
    if (!t && !tt::all(g).empty())
        t = tt::all(g).front();
    if (!t)
        throw CommandError("renderTurntable: spin=turntable needs a turntable; put the model on one first");
    if (!t->as<bool>(tt::Turntable::kIncludeInRenders))
        throw CommandError("renderTurntable: " + t->path() + " has includeInRenders off");
    o.orbit = false;
    const double angle0 = t->angle;
    const bool playing = t->playing;
    const scene::NodeId id = t->id();
    t->playing = false;
    struct Restore
    {
        scene::Graph& g;
        scene::NodeId id;
        double angle;
        bool playing;
        ~Restore()
        {
            if (tt::Turntable* t = tt::get(g, id))
            {
                tt::setAngle(g, id, angle);
                t->playing = playing;
            }
        }
    } restore{g, id, angle0, playing};
    const double sign = tt::cruiseSpeed(*t) < 0.0 ? -1.0 : 1.0;
    return writeSequence(seq, [&](int i) {
        tt::setAngle(g, id, angle0 + sign * 360.0 * turns * double(i) / double(frames));
        return std::pair{render(o, seconds), i};
    });
}
} // namespace

void registerCommands()
{
    using T = ArgType;
    auto renderFlags = [] {
        return std::vector<FlagDef>{
            nodeFlag({"camera", "c", T::String, nullptr, "Camera to render through; the active view's camera when left out."}, "camera"),
            {"width", "w", T::Int, nullptr, "Pixels; the scene's render resolution when left out.", {}, {}, 16.0, 16384.0},
            {"height", "h", T::Int, nullptr, "Pixels; the scene's render resolution when left out.", {}, {}, 16.0, 16384.0},
            {"scale", "", T::Float, nullptr, "Percent of the resolution (screen percentage); 100 by default.", {}, {}, 5.0, 800.0},
            {"samples", "s", T::Int, nullptr, "Jittered frames averaged per picture: antialiasing, depth of field and motion blur; 64 by default.", {}, {}, 1.0,
             4096.0},
            {"motionBlur", "mb", T::Bool, nullptr, "Blur what moves across the camera's shutter."},
            {"transparent", "tr", T::Bool, nullptr, "No backdrop: alpha is coverage (png, tif, exr, psd)."},
            {"renderPass", "rp", T::String, "beauty",
             "beauty; wireframe (beauty with every edge drawn, for breakdowns); or id (each object a flat colour from its path, for compositing mattes).",
             {"beauty", "wireframe", "id"}},
            {"passes", "ps", T::String, nullptr,
             "Compositing passes beside the beauty in a multilayer EXR, comma separated: depth, normal, motion, cryptoObject, "
             "cryptoMaterial, or all."},
        };
    };
    auto optionsFrom = [](const Args& a) {
        Options o;
        o.camera = setting(a, "camera", "").get<std::string>();
        o.width = a.get<int>("width", 0);
        o.height = a.get<int>("height", 0);
        o.scale = setting(a, "scale", 100.0).get<double>() / 100.0;
        o.samples = setting(a, "samples", 64).get<int>();
        o.motionBlur = setting(a, "motionBlur", false).get<bool>();
        o.transparent = setting(a, "transparent", false).get<bool>();
        o.pass = a.get<std::string>("renderPass", "beauty");
        std::vector<std::string> names;
        {
            std::string list = setting(a, "passes", "").get<std::string>(), item;
            for (char ch : list + ",")
                if (ch == ',')
                {
                    names.push_back(item);
                    item.clear();
                }
                else
                    item += ch;
        }
        for (std::string p : names)
        {
            std::erase_if(p, [](unsigned char c) { return std::isspace(c); });
            if (p == "all")
                o.passes = {"depth", "normal", "motion", "cryptoObject", "cryptoMaterial"};
            else if (p == "depth" || p == "normal" || p == "motion" || p == "cryptoObject" || p == "cryptoMaterial")
                o.passes.push_back(p);
            else if (!p.empty())
                throw CommandError("render: no pass named " + p + "; depth, normal, motion, cryptoObject, cryptoMaterial or all");
        }
        return o;
    };

    ops::registerCommand({
        .name = "ocioInfo",
        .doc = "What an OpenColorIO config offers: its displays, the views of a display, looks and colour spaces, and the colour space "
               "Lila's scene-linear Rec.709 maps to. The scene's config (or the built-in one) when config is left out.",
        .flags = {
            {"config", "cfg", T::String, nullptr, "An .ocio file or ocio:// URI; the scene's when left out."},
            {"display", "dsp", T::String, nullptr, "The display whose views to list; the default display when left out."},
        },
        .run = [](const Args& a) -> json {
            namespace cm = scene::colorManagement;
            const cm::Settings c = cm::settings(scene::Graph::current(), PrefStore::get().value<std::string>("general", cm::kConfigPref, ""));
            const render::ocio::ConfigInfo info = render::ocio::info(a.get<std::string>("config", c.config), a.get<std::string>("display", c.display));
            if (!info.ok)
                throw std::runtime_error("OCIO config failed: " + info.error);
            return {{"name", info.name},
                    {"workingSpace", info.workingSpace},
                    {"displays", info.displays},
                    {"defaultDisplay", info.defaultDisplay},
                    {"views", info.views},
                    {"defaultView", info.defaultView},
                    {"looks", info.looks},
                    {"colorSpaces", info.colorSpaces}};
        },
    });

    std::vector<FlagDef> single = renderFlags();
    single.push_back({"path", "p", T::String, nullptr,
                      "Image to write: png, jpg, tga, tif, exr (linear light) or psd. The Render panel's images, numbered, when left out."});
    single.push_back({"frame", "f", T::Float, nullptr, "Frame to render; the current one when left out."});
    ops::registerCommand({
        .name = "render",
        .doc = "Render one frame of the scene through a camera, with the post process and lens, and write it. Returns the path.",
        .flags = single,
        .undoable = false,
        .run = [optionsFrom](const Args& a) -> json {
            Options o = optionsFrom(a);
            const double seconds = a.has("frame") ? scene::anim::secondsOf(a.get<double>("frame")) : scene::Graph::current().time();
            const std::string path = a.has("path") ? a.get<std::string>("path")
                                                   : framePath(setting(a, "path", defaultPattern()).get<std::string>(),
                                                               int(std::lround(scene::anim::frameOf(seconds))));
            o.hdr = lower(paths::fromUtf8(path).extension().string()) == ".exr";
            try
            {
                write(render(o, seconds), path, o.transparent);
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("render: ") + e.what());
            }
            return path;
        },
    });

    std::vector<FlagDef> seq = renderFlags();
    const FlagDef writeFlag{"write", "wr", T::String, nullptr,
                            "With path and video left out: images, video or both, at the Render panel's paths. The Render panel's choice "
                            "by default, else video.",
                            {"images", "video", "both"}};
    seq.push_back({"path", "p", T::String, nullptr, "Images to write, #### standing for the frame number (added when missing)."});
    seq.push_back({"video", "v", T::String, nullptr, "An .mp4 to encode the frames into."});
    seq.push_back(writeFlag);
    seq.push_back({"codec", "", T::String, nullptr, "Video codec: h264 (default) or hevc.", {"h264", "hevc"}});
    seq.push_back({"quality", "q", T::Float, nullptr, "Video quality, 0 to 1 (0.6 by default).", {}, {}, 0.0, 1.0});
    seq.push_back({"start", "st", T::Float, nullptr, "First frame; the playback range's when left out."});
    seq.push_back({"end", "en", T::Float, nullptr, "Last frame; the playback range's when left out."});
    seq.push_back({"step", "by", T::Float, nullptr, "Frames between renders (1 by default).", {}, {}, 0.01});
    ops::registerCommand({
        .name = "renderSequence",
        .doc = "Render a frame range through a camera into an image sequence, an .mp4, or both. Returns {frames, path, video}.",
        .flags = seq,
        .undoable = false,
        .run = [optionsFrom](const Args& a) -> json {
            const auto [pattern, videoPath] = targets(a, "");
            if (pattern.empty() && videoPath.empty())
                throw CommandError("renderSequence: give path= (images), video= (.mp4) or both");
            Options o = optionsFrom(a);
            o.hdr = !pattern.empty() && lower(paths::fromUtf8(pattern).extension().string()) == ".exr";
            const scene::anim::Playback& pb = scene::anim::playback();
            const double start = setting(a, "start", std::round(scene::anim::frameOf(pb.minTime))).get<double>();
            const double end = setting(a, "end", std::round(scene::anim::frameOf(pb.maxTime))).get<double>();
            const double step = std::max(setting(a, "step", 1.0).get<double>(), 0.01);
            if (end < start)
                throw CommandError("renderSequence: end is before start");
            const int count = int(std::floor((end - start) / step + 1e-6)) + 1;

            Sequence seq;
            seq.command = "renderSequence";
            seq.pattern = pattern;
            seq.video = videoPath;
            seq.codec = setting(a, "codec", "h264").get<std::string>();
            seq.quality = setting(a, "quality", 0.6).get<double>();
            seq.fps = pb.fps / step;
            seq.count = count;
            seq.transparent = o.transparent;
            return writeSequence(seq, [&](int i) {
                const double frame = start + step * i;
                return std::pair{render(o, scene::anim::secondsOf(frame)), int(std::lround(frame))};
            });
        },
    });

    std::vector<FlagDef> tt = renderFlags();
    tt.push_back({"path", "p", T::String, nullptr, "Images to write, #### standing for the frame number."});
    tt.push_back({"video", "v", T::String, nullptr, "An .mp4 to encode the turntable into."});
    tt.push_back(writeFlag);
    tt.push_back({"codec", "", T::String, nullptr, "Video codec: h264 (default) or hevc.", {"h264", "hevc"}});
    tt.push_back({"quality", "q", T::Float, nullptr, "Video quality, 0 to 1 (0.6 by default).", {}, {}, 0.0, 1.0});
    tt.push_back({"frames", "fr", T::Int, nullptr, "Frames for the whole turn; 120 by default.", {}, {}, 2.0, 10000.0});
    tt.push_back({"turns", "", T::Float, nullptr, "Turns over the frames; 1 by default.", {}, {}, 0.01, 100.0});
    tt.push_back({"spin", "", T::String, nullptr,
                  "object (the model turns in fixed light), camera (the camera circles, the light stays), lights, or turntable (a "
                  "turntable node turns its members through the view's own camera).",
                  {"object", "camera", "lights", "turntable"}});
    tt.push_back({"turntable", "tt", T::String, nullptr, "With spin=turntable: the turntable; the selected one or the first by default."});
    tt.push_back({"pitch", "", T::Float, nullptr, "Camera elevation, degrees (negative looks down); -15 by default.", {}, {}, -89.0, 89.0});
    tt.push_back({"yaw", "", T::Float, nullptr, "Starting angle around the up axis, degrees; 35 by default.", {}, {}, -360.0, 360.0});
    ops::registerCommand({
        .name = "renderTurntable",
        .doc = "Render a loop turning around the selection (or everything shown), framed to fit, into images, an .mp4, or both. The last frame stops "
               "one step short of the first, so the video loops. Returns {frames, path, video}.",
        .flags = tt,
        .undoable = false,
        .run = [optionsFrom](const Args& a) -> json {
            // The Render panel's paths get their own turntable names, so a turntable never overwrites the shot.
            const auto [pattern, videoPath] = targets(a, "_turntable");
            if (pattern.empty() && videoPath.empty())
                throw CommandError("renderTurntable: give path= (images), video= (.mp4) or both");
            Options o = optionsFrom(a);
            o.hdr = !pattern.empty() && lower(paths::fromUtf8(pattern).extension().string()) == ".exr";
            o.orbit = true;
            o.orbitPitch = setting(a, "pitch", -15.0).get<double>();
            const int frames = std::max(setting(a, "frames", 120).get<int>(), 2);
            const double turns = setting(a, "turns", 1.0).get<double>();
            const std::string spin = setting(a, "spin", "object").get<std::string>();
            const double yaw0 = setting(a, "yaw", 35.0).get<double>();
            Sequence seq;
            seq.command = "renderTurntable";
            seq.pattern = pattern;
            seq.video = videoPath;
            seq.codec = setting(a, "codec", "h264").get<std::string>();
            seq.quality = setting(a, "quality", 0.6).get<double>();
            seq.fps = scene::anim::playback().fps;
            seq.count = frames;
            seq.transparent = o.transparent;
            const double now = scene::Graph::current().time();
            if (spin == "turntable")
                return renderTurntableNode(a, o, seq, frames, turns, now);
            return writeSequence(seq, [&](int i) {
                const double angle = 360.0 * turns * double(i) / double(frames);
                Options f = o;
                // Turning the model in fixed light is the camera and the light circling together.
                f.orbitYaw = spin == "lights" ? yaw0 : yaw0 + angle;
                f.environmentSpin = spin == "camera" ? 0.0 : -angle;
                return std::pair{render(f, now), i};
            });
        },
    });
}

} // namespace lila::ui::renderOutput
