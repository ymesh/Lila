#include "ui/PointCloudDisplay.h"

#include "core/MainQueue.h"
#include "core/Profile.h"
#include "pointcloud/PointCloudNode.h"
#include "scene/Graph.h"
#include "scene/SceneIndex.h"
#include "scene/Transform.h"
#include "ui/DrawOverrides.h"
#include "ui/ViewportTool.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <queue>
#include <thread>

namespace lila::ui::pointCloudDisplay
{
namespace
{
using pointcloud::OctreeNode;
using pointcloud::PointCloudFile;

/** Loaded nodes past this many bytes are dropped, those not drawn longest first. */
constexpr size_t kMemoryBudget = size_t(2) << 30;
/** A node no view has drawn for this long is no longer wanted: its pending load goes, and it may be dropped. */
constexpr uint64_t kStaleMs = 1500;

uint64_t nowMs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Entry
{
    std::shared_ptr<const render::PointChunk> chunk;
    bool pending = false;
    uint64_t lastUsed = 0;
};

struct Cloud
{
    std::shared_ptr<const PointCloudFile> file;
    std::vector<Entry> entries;
};

/** Loads octree nodes on two worker threads; the main thread draws what has arrived and asks again next frame. */
class Streamer
{
public:
    static Streamer& get()
    {
        // Never destroyed: workers may still be finishing a read as statics go away.
        static Streamer* s = new Streamer();
        return *s;
    }

    Cloud& cloud(const std::shared_ptr<const PointCloudFile>& f)
    {
        std::scoped_lock lock(mMutex);
        Cloud& c = mClouds[f.get()];
        if (!c.file)
        {
            c.file = f;
            c.entries.resize(f->nodes().size());
        }
        return c;
    }

    /** The node's points if loaded; otherwise asks for them and returns null. */
    std::shared_ptr<const render::PointChunk> chunk(Cloud& c, uint32_t node, uint64_t frame)
    {
        std::scoped_lock lock(mMutex);
        Entry& e = c.entries[node];
        e.lastUsed = frame;
        if (!e.chunk && !e.pending)
        {
            e.pending = true;
            mQueue.push_back({c.file, node});
            mWake.notify_one();
        }
        return e.chunk;
    }

    /** Drops requests no view has wanted for a while (the views moved on) and memory over budget. frame is in ms. */
    void endFrame(uint64_t frame)
    {
        std::scoped_lock lock(mMutex);
        std::erase_if(mQueue, [&](const Request& r) {
            auto it = mClouds.find(r.file.get());
            if (it == mClouds.end() || it->second.entries[r.node].lastUsed + kStaleMs >= frame)
                return false;
            it->second.entries[r.node].pending = false;
            return true;
        });
        if (mBytes <= kMemoryBudget)
            return;
        std::vector<std::tuple<uint64_t, Cloud*, uint32_t>> loaded;
        for (auto& [key, c] : mClouds)
            for (uint32_t i = 0; i < c.entries.size(); ++i)
                if (c.entries[i].chunk && c.entries[i].lastUsed + kStaleMs < frame)
                    loaded.emplace_back(c.entries[i].lastUsed, &c, i);
        std::ranges::sort(loaded);
        for (const auto& [used, c, i] : loaded)
        {
            if (mBytes <= kMemoryBudget * 9 / 10)
                break;
            mBytes -= c->entries[i].chunk->words.size() * sizeof(uint32_t);
            c->entries[i].chunk.reset();
        }
    }

    /** True once every node in nodes has loaded (or failed to). */
    bool ready(Cloud& c, const std::vector<uint32_t>& nodes)
    {
        std::scoped_lock lock(mMutex);
        return std::ranges::all_of(nodes, [&](uint32_t i) { return c.entries[i].chunk != nullptr; });
    }

    std::pair<size_t, size_t> state()
    {
        std::scoped_lock lock(mMutex);
        return {mQueue.size() + size_t(mBusy.load()), mBytes};
    }

    /** Every loaded chunk of a cloud with its node, for snapping. */
    std::vector<std::pair<uint32_t, std::shared_ptr<const render::PointChunk>>> loaded(const PointCloudFile* f)
    {
        std::scoped_lock lock(mMutex);
        std::vector<std::pair<uint32_t, std::shared_ptr<const render::PointChunk>>> out;
        if (auto it = mClouds.find(f); it != mClouds.end())
            for (uint32_t i = 0; i < it->second.entries.size(); ++i)
                if (it->second.entries[i].chunk)
                    out.emplace_back(i, it->second.entries[i].chunk);
        return out;
    }

private:
    struct Request
    {
        std::shared_ptr<const PointCloudFile> file;
        uint32_t node = 0;
    };

    Streamer()
    {
        for (int i = 0; i < 2; ++i)
            std::thread([this] { work(); }).detach();
    }

    void work()
    {
        for (;;)
        {
            Request r;
            {
                std::unique_lock lock(mMutex);
                mWake.wait(lock, [&] { return !mQueue.empty(); });
                r = std::move(mQueue.front());
                mQueue.pop_front();
                ++mBusy;
            }
            auto chunk = std::make_shared<render::PointChunk>();
            try
            {
                const std::vector<pointcloud::PackedPoint> pts = r.file->read(r.node);
                chunk->words.resize(pts.size() * 3);
                std::memcpy(chunk->words.data(), pts.data(), pts.size() * sizeof(pointcloud::PackedPoint));
                chunk->count = uint32_t(pts.size());
            }
            catch (const std::exception&)
            {
                chunk->count = 0;
            }
            {
                std::scoped_lock lock(mMutex);
                Cloud& c = mClouds[r.file.get()];
                if (r.node < c.entries.size())
                {
                    c.entries[r.node].chunk = chunk;
                    c.entries[r.node].pending = false;
                    mBytes += chunk->words.size() * sizeof(uint32_t);
                }
                --mBusy;
            }
            // One redraw for a burst of arrivals.
            if (!mRedrawQueued.exchange(true))
                mainq::post([this] {
                    mRedrawQueued = false;
                    viewportTools::requestRedraw();
                });
        }
    }

    std::mutex mMutex;
    std::condition_variable mWake;
    std::deque<Request> mQueue;
    std::map<const PointCloudFile*, Cloud> mClouds;
    size_t mBytes = 0;
    std::atomic<int> mBusy{0};
    std::atomic<bool> mRedrawQueued{false};
};


/** False when the cube (cloud space) lies wholly outside the view volume. */
bool inView(const glm::dmat4& toClip, const OctreeNode& n)
{
    int outside[6] = {};
    for (int c = 0; c < 8; ++c)
    {
        const glm::dvec3 p = n.min + glm::dvec3((c & 1) ? n.size : 0.0, (c & 2) ? n.size : 0.0, (c & 4) ? n.size : 0.0);
        const glm::dvec4 q = toClip * glm::dvec4(p, 1.0);
        outside[0] += q.x < -q.w;
        outside[1] += q.x > q.w;
        outside[2] += q.y < -q.w;
        outside[3] += q.y > q.w;
        outside[4] += q.z > q.w; // reversed-Z: beyond the near plane
        outside[5] += q.w <= 0.0;
    }
    return std::none_of(std::begin(outside), std::end(outside), [](int k) { return k == 8; });
}

/** True when the cube (cloud space) may overlap one of the boxes (cloud space to box space). */
bool touchesBox(const std::vector<glm::dmat4>& boxes, const OctreeNode& n)
{
    for (const glm::dmat4& b : boxes)
    {
        glm::dvec3 lo(1e300), hi(-1e300);
        for (int c = 0; c < 8; ++c)
        {
            const glm::dvec3 p = n.min + glm::dvec3((c & 1) ? n.size : 0.0, (c & 2) ? n.size : 0.0, (c & 4) ? n.size : 0.0);
            const glm::dvec3 q(b * glm::dvec4(p, 1.0));
            lo = glm::min(lo, q);
            hi = glm::max(hi, q);
        }
        if (glm::all(glm::lessThanEqual(lo, glm::dvec3(0.5))) && glm::all(glm::greaterThanEqual(hi, glm::dvec3(-0.5))))
            return true;
    }
    return false;
}

/** The nodes to draw: largest on screen first, refined while their points are sparser than the target, up to budget points. */
std::vector<uint32_t> select(const PointCloudFile& f, const glm::dmat4& world, const ViewInfo& view, double heightPx, double targetPx, uint64_t budget,
                             const std::vector<glm::dmat4>* boxes)
{
    LILA_ZONE();
    const std::vector<OctreeNode>& nodes = f.nodes();
    std::vector<uint32_t> out;
    if (nodes.empty())
        return out;
    const glm::dmat4 toClip = view.proj * view.view * world;
    const double scale = std::cbrt(std::abs(glm::determinant(glm::dmat3(world))));
    const glm::dvec3 eyeLocal(glm::inverse(world) * glm::dvec4(view.eye, 1.0));
    const double focal = heightPx * std::abs(view.proj[1][1]) * 0.5;
    const bool ortho = view.ortho;
    auto pixels = [&](const OctreeNode& n) {
        if (ortho)
            return n.spacing() * view.proj[1][1] * 0.5 * heightPx * scale;
        const glm::dvec3 centre = n.min + glm::dvec3(n.size * 0.5);
        const double d = std::max(glm::length(eyeLocal - centre) - n.size * 0.866, n.size * 1e-3);
        return n.spacing() * focal / d;
    };
    using Item = std::pair<double, uint32_t>;
    std::priority_queue<Item> queue;
    queue.push({pixels(nodes[0]), 0});
    uint64_t points = 0;
    while (!queue.empty() && points < budget)
    {
        const auto [px, i] = queue.top();
        queue.pop();
        const OctreeNode& n = nodes[i];
        if (!inView(toClip, n) || (boxes && !touchesBox(*boxes, n)))
            continue;
        out.push_back(i);
        points += n.count;
        if (px <= targetPx || !n.childMask)
            continue;
        uint32_t child = n.firstChild;
        for (int o = 0; o < 8; ++o)
            if (n.childMask & (1u << o))
            {
                queue.push({pixels(nodes[child]), child});
                ++child;
            }
    }
    return out;
}

/** The unit cube's twelve edges, for clip boxes. */
void drawClip(drawOverrides::DrawContext& c)
{
    const glm::vec4 color = c.highlight == drawOverrides::Highlight::None ? glm::vec4(0.95f, 0.55f, 0.1f, 1.0f) : c.color;
    for (int e = 0; e < 12; ++e)
    {
        const int axis = e / 4, a = (e % 4) & 1, b = (e % 4) >> 1;
        glm::dvec3 p0(-0.5), p1(0.5);
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        p0[u] = p1[u] = a ? 0.5 : -0.5;
        p0[v] = p1[v] = b ? 0.5 : -0.5;
        p0[axis] = -0.5;
        p1[axis] = 0.5;
        c.frame.lines.push_back({glm::dvec3(c.world * glm::dvec4(p0, 1.0)), color});
        c.frame.lines.push_back({glm::dvec3(c.world * glm::dvec4(p1, 1.0)), color});
    }
}

/**
 * A click picks the cloud when a loaded point is within kClickPixels of it, by that distance; a marquee when the cloud's
 * centre (its root octree cell's, through its transform) is inside the box.
 */
std::optional<double> pickCloud(const drawOverrides::PickContext& c)
{
    scene::Graph& g = scene::Graph::current();
    if (c.click)
    {
        const std::optional<glm::dvec3> p = nearestPoint(g, c.node.id(), c.view, c.a, drawOverrides::kClickPixels);
        if (!p)
            return std::nullopt;
        const auto s = c.view.project(*p);
        return s ? std::optional<double>(std::hypot(s->x() - c.a.x(), s->y() - c.a.y())) : std::nullopt;
    }
    std::shared_ptr<const PointCloudFile> f = pointcloud::cacheOf(c.node);
    if (!f || f->nodes().empty())
        return std::nullopt;
    const pointcloud::OctreeNode& root = f->nodes().front();
    const glm::dvec3 centre = root.min + glm::dvec3(root.size * 0.5);
    const auto s = c.view.project(glm::dvec3(c.world * glm::dvec4(centre, 1.0)));
    return s && QRectF(c.a, c.b).normalized().contains(*s) ? std::optional<double>(0.0) : std::nullopt;
}

std::optional<double> pickClip(const drawOverrides::PickContext& c)
{
    std::optional<double> best;
    for (int e = 0; e < 12; ++e)
    {
        const int axis = e / 4, a = (e % 4) & 1, b = (e % 4) >> 1;
        glm::dvec3 p0(-0.5), p1(0.5);
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        p0[u] = p1[u] = a ? 0.5 : -0.5;
        p0[v] = p1[v] = b ? 0.5 : -0.5;
        p0[axis] = -0.5;
        p1[axis] = 0.5;
        const auto s0 = c.view.project(glm::dvec3(c.world * glm::dvec4(p0, 1.0)));
        const auto s1 = c.view.project(glm::dvec3(c.world * glm::dvec4(p1, 1.0)));
        if (!s0 || !s1)
            continue;
        if (!c.click)
        {
            const QRectF r = QRectF(c.a, c.b).normalized();
            if (r.contains(*s0) && r.contains(*s1))
                best = 0.0;
            continue;
        }
        const glm::dvec2 a2(s0->x(), s0->y()), b2(s1->x(), s1->y()), p(c.a.x(), c.a.y());
        const double t = std::clamp(glm::dot(p - a2, b2 - a2) / std::max(glm::dot(b2 - a2, b2 - a2), 1e-9), 0.0, 1.0);
        const double d = glm::length(p - (a2 + (b2 - a2) * t));
        if (d <= drawOverrides::kClickPixels && (!best || d < *best))
            best = d;
    }
    return best;
}
} // namespace

void draw(render::FrameState& s, const ViewInfo& view, double heightPx, scene::Graph& g, const std::function<bool(scene::NodeId)>& shown,
          const std::function<glm::vec3(scene::NodeId)>& tint, bool complete)
{
    LILA_ZONE();
    using pointcloud::PointCloud;
    // The scene index caches the clouds, so a scene without any pays nothing per frame and starts no loader threads.
    const std::vector<scene::Node*>& clouds =
        scene::sceneIndex::matching(g, "pointCloud", [](const scene::Node& node) { return node.type().isA("pointCloud"); });
    if (clouds.empty())
        return;
    const uint64_t frame = nowMs();
    Streamer& st = Streamer::get();
    for (scene::Node* n : clouds)
    {
        const scene::NodeId id = n->id();
        if (!shown(id))
            continue;
        std::shared_ptr<const PointCloudFile> f = pointcloud::cacheOf(*n);
        if (!f || f->nodes().empty())
            continue;
        Cloud& cloud = st.cloud(f);
        render::PointCloudDraw d;
        d.world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        d.pointSize = float(std::max(n->as<double>(PointCloud::kPointSize), 0.5));
        d.edl = float(std::clamp(n->as<double>(PointCloud::kEyeDomeLighting), 0.0, 4.0));
        d.colorMode = render::PointCloudDraw::ColorMode(std::clamp<int64_t>(n->as<int64_t>(PointCloud::kColorMode), 0, 4));
        if (d.colorMode == render::PointCloudDraw::Rgb && !f->hasColor())
            d.colorMode = render::PointCloudDraw::Intensity;
        d.color = glm::vec3(n->as<glm::dvec3>(PointCloud::kColor));
        d.clipMode = render::PointCloudDraw::ClipMode(std::clamp<int64_t>(n->as<int64_t>(PointCloud::kClipMode), 0, 2));
        d.clips = pointcloud::clipBoxes(g, *n);
        if (d.clips.size() > 8)
            d.clips.resize(8);
        d.tint = tint(id);
        d.node = id;
        d.upAxis = s.zUp ? 2 : 1;
        // Elevation spans the cloud's world height.
        if (const nlohmann::json& lo = f->metadata().value("boundsMin", nlohmann::json()); lo.is_array())
        {
            const nlohmann::json& hi = f->metadata()["boundsMax"];
            double a = 1e300, b = -1e300;
            for (int c = 0; c < 8; ++c)
            {
                const glm::dvec3 p((c & 1 ? hi : lo)[0].get<double>(), (c & 2 ? hi : lo)[1].get<double>(), (c & 4 ? hi : lo)[2].get<double>());
                const double h = glm::dvec3(d.world * glm::dvec4(p, 1.0))[d.upAxis];
                a = std::min(a, h);
                b = std::max(b, h);
            }
            d.elevation = {float(a), float(b)};
        }
        // Clip boxes in the cloud's space for culling nodes the boxes leave out.
        std::vector<glm::dmat4> local;
        if (d.clipMode == render::PointCloudDraw::ClipInside && !d.clips.empty())
            for (const glm::dmat4& b : d.clips)
                local.push_back(b * d.world);
        const double detail = std::clamp(n->as<double>(PointCloud::kDetail), 0.05, 16.0);
        const uint64_t budget = uint64_t(std::clamp(n->as<double>(PointCloud::kBudget), 0.1, 200.0) * 1e6);
        // Points a little closer than their size, so a surface reads solid.
        const std::vector<uint32_t> nodes = select(*f, d.world, view, heightPx, std::max(double(d.pointSize), 1.0) * 0.7 / detail, budget, local.empty() ? nullptr : &local);
        for (uint32_t i : nodes)
            st.chunk(cloud, i, frame);
        // A render waits for its detail rather than showing what happens to be loaded.
        if (complete)
        {
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (!st.ready(cloud, nodes) && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        for (uint32_t i : nodes)
            if (std::shared_ptr<const render::PointChunk> c = st.chunk(cloud, i, frame))
                d.chunks.push_back({c, f->nodes()[i].min, f->nodes()[i].size});
        if (!d.chunks.empty())
            s.pointClouds.push_back(std::move(d));
    }
    st.endFrame(frame);
}

std::optional<glm::dvec3> nearestPoint(scene::Graph& g, scene::NodeId id, const ViewInfo& view, QPointF px, double radiusPx)
{
    LILA_ZONE();
    const scene::Node* n = g.node(id);
    if (!n || !n->type().isA("pointCloud"))
        return std::nullopt;
    std::shared_ptr<const PointCloudFile> f = pointcloud::cacheOf(*n);
    if (!f)
        return std::nullopt;
    const glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    const glm::dmat4 toClip = view.proj * view.view * world;
    std::optional<glm::dvec3> best;
    double bestPx = radiusPx;
    for (const auto& [i, chunk] : Streamer::get().loaded(f.get()))
    {
        const OctreeNode& node = f->nodes()[i];
        if (!inView(toClip, node))
            continue;
        const auto* pts = reinterpret_cast<const pointcloud::PackedPoint*>(chunk->words.data());
        for (uint32_t k = 0; k < chunk->count; ++k)
        {
            const glm::dvec3 p = PointCloudFile::position(node, pts[k]);
            const glm::dvec4 c = toClip * glm::dvec4(p, 1.0);
            if (c.w <= 1e-9)
                continue;
            const double x = (c.x / c.w * 0.5 + 0.5) * view.width, y = (c.y / c.w * 0.5 + 0.5) * view.height;
            const double d = std::hypot(x - px.x(), y - px.y());
            if (d < bestPx)
            {
                bestPx = d;
                best = glm::dvec3(world * glm::dvec4(p, 1.0));
            }
        }
    }
    return best;
}

std::pair<size_t, size_t> streaming() { return Streamer::get().state(); }

void registerUi()
{
    if (!drawOverrides::registry().find("lila.pointCloudClip"))
        drawOverrides::add({.id = "lila.pointCloudClip", .nodeType = "pointCloudClip", .draw = drawClip, .pick = pickClip});
    if (!drawOverrides::registry().find("lila.pointCloud"))
        drawOverrides::add({.id = "lila.pointCloud", .nodeType = "pointCloud", .draw = [](drawOverrides::DrawContext&) {}, .pick = pickCloud});
}
} // namespace lila::ui::pointCloudDisplay
