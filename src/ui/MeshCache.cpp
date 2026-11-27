#include "ui/MeshCache.h"
#include "core/Release.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/Normals.h"
#include "geo/Ops.h"
#include "geo/RenderMesh.h"
#include "ui/ViewportDisplay.h"

#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>

#include <algorithm>
#include <cstring>
#include <array>
#include <cmath>
#include <map>
#include <numbers>
#include <thread>
#include <tuple>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace lila::ui
{
namespace
{
struct Entry
{
    std::weak_ptr<const geo::Mesh> source;
    std::shared_ptr<const render::RenderMesh> built;
    std::shared_ptr<const geo::RenderMesh> pick;
    bool tangents = false;
    /** A deformed copy's layout, its pick data made from it when picking first asks (pick is null until then). */
    std::shared_ptr<const geo::RenderMesh> pickLayout;
    /** The node whose live GPU buffer built draws from, when it has one. */
    scene::NodeId owner = scene::kNoNode;
};

std::unordered_map<const geo::Mesh*, Entry>& cache()
{
    // Never destroyed: at exit the meshes' owners (extension DLLs among them) may already be unloaded.
    static auto& c = *new std::unordered_map<const geo::Mesh*, Entry>();
    return c;
}

/** Static copies of live entries for the other nodes drawing the same mesh, as duplicates share their source's mesh. */
std::unordered_map<const geo::Mesh*, Entry>& staticCopies()
{
    static auto& c = *new std::unordered_map<const geo::Mesh*, Entry>();
    return c;
}

const Entry* staticCopy(const scene::MeshPtr& mesh, const Entry& live)
{
    auto& c = staticCopies();
    if (auto it = c.find(mesh.get()); it != c.end() && it->second.source.lock() == mesh && it->second.tangents == live.tangents)
        return &it->second;
    Entry e = live;
    auto copy = std::make_shared<render::RenderMesh>(*live.built);
    copy->dynamic.reset();
    copy->key = render::RenderMesh::nextKey();
    e.built = std::move(copy);
    e.owner = scene::kNoNode;
    return &(c[mesh.get()] = std::move(e));
}

uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0x100000001b3ull;
}

/** Hash of bytes, chunked in parallel; the same for the same bytes whatever the thread count. */
uint64_t hashBytes(const std::byte* data, size_t size)
{
    constexpr size_t kChunk = 1 << 16;
    std::vector<uint64_t> parts((size + kChunk - 1) / kChunk);
    tbb::parallel_for(size_t(0), parts.size(), [&](size_t i) {
        uint64_t h = 1469598103934665603ull;
        const size_t end = std::min(size, (i + 1) * kChunk);
        size_t b = i * kChunk;
        for (; b + 8 <= end; b += 8)
        {
            uint64_t v;
            std::memcpy(&v, data + b, 8);
            h = mix(h, v);
        }
        for (; b < end; ++b)
            h = mix(h, uint64_t(data[b]));
        parts[i] = h;
    });
    uint64_t h = mix(0, size);
    for (uint64_t p : parts)
        h = mix(h, p);
    return h;
}

/**
 * Everything a render copy depends on except where the points are and the normals: topology and every attribute
 * layer but the corner normals. Two meshes with the same key differ only by deformation.
 */
uint64_t layoutKey(const geo::Mesh& m)
{
    LILA_ZONE();

    // Chunk hashes in parallel, combined in order.
    auto hashRange = [](geo::Index count, auto&& element) {
        constexpr geo::Index kChunk = 1 << 14;
        std::vector<uint64_t> parts((count + kChunk - 1) / kChunk);
        tbb::parallel_for(size_t(0), parts.size(), [&](size_t i) {
            uint64_t h = 1469598103934665603ull;
            for (geo::Index k = geo::Index(i) * kChunk; k < std::min(count, geo::Index(i + 1) * kChunk); ++k)
                h = element(h, k);
            parts[i] = h;
        });
        uint64_t h = mix(0, count);
        for (uint64_t p : parts)
            h = mix(h, p);
        return h;
    };
    uint64_t h = hashRange(m.cornerCapacity(), [&](uint64_t h, geo::Index c) {
        return m.cornerValid(c) ? mix(mix(mix(h, m.cornerPoint(c)), m.cornerNext(c)), m.cornerEdge(c)) : mix(h, geo::kInvalid);
    });
    h = mix(h, hashRange(m.faceCapacity(), [&](uint64_t h, geo::Index f) { return m.faceValid(f) ? mix(mix(h, m.faceCorner(f)), m.faceSize(f)) : mix(h, geo::kInvalid); }));
    h = mix(h, hashRange(m.pointCapacity(), [&](uint64_t h, geo::Index p) { return m.pointValid(p) ? h : mix(h, p); }));
    h = mix(h, m.edgeCapacity());
    const geo::AttributeSet* sets[] = {&m.pointAttrs(), &m.edgeAttrs(), &m.faceAttrs(), &m.cornerAttrs()};
    for (int s = 0; s < 4; ++s)
        for (size_t i = 0; i < sets[s]->layerCount(); ++i)
        {
            const geo::Attribute& a = sets[s]->layer(i);
            if (s == 3 && a.name() == geo::attr::kNormal)
                continue;
            h = mix(h, std::hash<std::string>()(a.name()));
            if (a.size())
                h = mix(h, hashBytes(a.raw(0), a.size() * a.stride()));
        }
    for (const std::string& n : m.materialNames())
        h = mix(h, std::hash<std::string>()(n));
    return h;
}

/** The last few copies built in full, by layoutKey, so a deformed mesh can reuse one's vertex layout. */
struct Layout
{
    uint64_t key = 0;
    Entry entry;
};

std::vector<Layout>& layouts()
{
    // Never destroyed, like cache(): its entries hold weak references to meshes extension DLLs own.
    static auto& l = *new std::vector<Layout>();
    return l;
}

void rememberLayout(uint64_t key, const Entry& e)
{
    // Bounded by count and vertices, since each one keeps a full copy alive after its mesh is gone.
    constexpr size_t kKept = 8, kVertices = 8'000'000;
    auto& l = layouts();
    std::erase_if(l, [&](const Layout& x) { return x.key == key; });
    if (!e.pick || e.pick->positions.size() > kVertices)
        return;
    l.insert(l.begin(), {key, e});
    size_t vertices = 0, kept = 0;
    while (kept < l.size() && kept < kKept && (vertices += l[kept].entry.pick->positions.size()) <= kVertices)
        ++kept;
    l.resize(kept);
}

/**
 * Tangents from the triangles' uv gradients, accumulated per render vertex and made orthogonal to its normal, the
 * bitangent sign kept from the full build. Close to MikkTSpace on a deforming surface at a fraction of its cost.
 */
void fastTangents(const geo::RenderMesh& layout, const std::vector<glm::vec3>& pos, const std::vector<glm::vec3>& nrm, std::vector<glm::vec4>& out)
{
    LILA_ZONE();

    const size_t n = pos.size();
    std::vector<glm::vec3> acc(n, glm::vec3(0.0f));
    const auto& uv = layout.uvs[0];
    const size_t tris = layout.indices.size() / 3;
    for (size_t t = 0; t < tris; ++t)
    {
        const geo::Index a = layout.indices[t * 3], b = layout.indices[t * 3 + 1], c = layout.indices[t * 3 + 2];
        const glm::vec3 e1 = pos[b] - pos[a], e2 = pos[c] - pos[a];
        const glm::vec2 d1 = uv[b] - uv[a], d2 = uv[c] - uv[a];
        const float det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-20f)
            continue;
        const glm::vec3 tangent = (e1 * d2.y - e2 * d1.y) / det;
        acc[a] += tangent;
        acc[b] += tangent;
        acc[c] += tangent;
    }
    out.resize(n);
    tbb::parallel_for(size_t(0), n, [&](size_t v) {
        const glm::vec4 was = v < layout.tangents.size() ? layout.tangents[v] : glm::vec4(1, 0, 0, 1);
        glm::vec3 t = acc[v] - nrm[v] * glm::dot(nrm[v], acc[v]);
        if (glm::dot(t, t) < 1e-20f)
            t = glm::vec3(was) - nrm[v] * glm::dot(nrm[v], glm::vec3(was));
        const float len = glm::length(t);
        out[v] = glm::vec4(len > 1e-12f ? t / len : glm::vec3(1, 0, 0), was.w);
    });
}

/**
 * Vertices a node's deforming mesh draws from, kept on the GPU and rewritten in place each frame (one per node and
 * layout), so a skinned character doesn't upload its whole mesh again every frame.
 */
struct Live
{
    std::shared_ptr<render::DynamicVertices> vertices;
    uint64_t key = 0;
};

std::map<std::pair<scene::NodeId, uint64_t>, Live>& lives()
{
    static std::map<std::pair<scene::NodeId, uint64_t>, Live> l;
    return l;
}

/** Which copy's vertices each live buffer holds now: every snapshot of a node's deformation shares one buffer. */
std::unordered_map<const render::DynamicVertices*, const render::RenderMesh*>& liveHolders()
{
    static std::unordered_map<const render::DynamicVertices*, const render::RenderMesh*> h;
    return h;
}

/**
 * Puts a cached copy's own vertices back in its live buffer when a later snapshot overwrote them, as undoing a
 * component drag returns to a snapshot whose copy shares the buffer the drag kept rewriting.
 */
void restoreLive(const render::RenderMesh& built)
{
    if (!built.dynamic)
        return;
    auto it = liveHolders().find(built.dynamic.get());
    if (it == liveHolders().end() || it->second == &built)
        return;
    LILA_ZONE();
    std::scoped_lock lock(built.dynamic->mutex);
    built.dynamic->vertices.assign(built.vertices.begin(), built.vertices.end());
    built.dynamic->markAllDirty();
    it->second = &built;
}

/** Normals of each render vertex of layout on mesh m. */
std::vector<glm::vec3> vertexNormals(const geo::Mesh& m, const geo::RenderMesh& layout)
{
    std::vector<glm::vec3> computed;
    std::span<const glm::vec3> cornerN = m.cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    if (cornerN.size() != m.cornerCapacity())
    {
        computed = geo::cornerNormals(m);
        cornerN = computed;
    }
    std::vector<glm::vec3> nrm(layout.positions.size());
    tbb::parallel_for(size_t(0), nrm.size(), [&](size_t v) { nrm[v] = cornerN[layout.vertexCorner[v]]; });
    return nrm;
}

/** Pick data for mesh m from a deformed copy's layout: its positions and normals. */
std::shared_ptr<const geo::RenderMesh> pickFrom(const geo::Mesh& m, const geo::RenderMesh& layout)
{
    LILA_ZONE();

    auto pick = std::make_shared<geo::RenderMesh>(layout);
    const std::vector<glm::vec3> nrm = vertexNormals(m, layout);
    tbb::parallel_for(size_t(0), nrm.size(), [&](size_t v) {
        pick->positions[v] = glm::vec3(m.position(layout.vertexPoint[v]));
        pick->normals[v] = nrm[v];
    });
    return pick;
}

/**
 * The mesh's grunge masks per render vertex (render::kGrungeAttrs), from float corner attributes, else point ones;
 * left empty when it has neither.
 */
void fillGrunge(const geo::Mesh& m, const geo::RenderMesh& layout, render::RenderMesh& out)
{
    std::array<std::span<const float>, render::kGrungeMasks> masks;
    bool corners = false, points = false;
    for (int k = 0; k < render::kGrungeMasks; ++k)
        corners = corners || !m.cornerAttrs().get<float>(render::kGrungeAttrs[size_t(k)]).empty();
    for (int k = 0; k < render::kGrungeMasks && !corners; ++k)
        points = points || !m.pointAttrs().get<float>(render::kGrungeAttrs[size_t(k)]).empty();
    out.grunge.clear();
    if (!corners && !points)
        return;
    const auto edgeDistance = m.cornerAttrs().get<glm::vec4>(render::kGrungeEdgeDistanceAttr);
    const auto contactDistance = m.cornerAttrs().get<glm::vec4>(render::kGrungeContactDistanceAttr);
    const auto streakLines = m.cornerAttrs().get<glm::vec4>(render::kGrungeStreakLinesAttr);
    const auto height = m.cornerAttrs().get<float>(render::kGrungeHeightAttr);
    for (int k = 0; k < render::kGrungeMasks; ++k)
        masks[size_t(k)] = (corners ? m.cornerAttrs() : m.pointAttrs()).get<float>(render::kGrungeAttrs[size_t(k)]);
    const auto& source = corners ? layout.vertexCorner : layout.vertexPoint;
    const size_t n = source.size();
    out.grunge.assign(n * render::kGrungeWords, 0u);
    auto unorm = [](float v) { return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f)); };
    tbb::parallel_for(size_t(0), n, [&](size_t v) {
        const size_t e = size_t(source[v]);
        std::array<uint32_t, render::kGrungeMasks> q{};
        for (size_t k = 0; k < q.size(); ++k)
            q[k] = e < masks[k].size() ? unorm(masks[k][e]) : 0u;
        uint32_t* words = &out.grunge[v * render::kGrungeWords];
        for (size_t w = 0; w < 3; ++w)
            words[w] = q[w * 2] | (q[w * 2 + 1] << 16);
        const size_t c = size_t(layout.vertexCorner[v]);
        const glm::vec4 d = c < edgeDistance.size() ? edgeDistance[c] : glm::vec4(render::kGrungeFarEdge);
        words[3] = glm::packHalf2x16(glm::vec2(d.x, d.y));
        words[4] = glm::packHalf2x16(glm::vec2(d.z, d.w));
        const float far = render::kGrungeFarEdge;
        const glm::vec4 cd = c < contactDistance.size() ? contactDistance[c] : glm::vec4(far);
        words[5] = glm::packHalf2x16(glm::vec2(cd.x, cd.y));
        words[6] = glm::packHalf2x16(glm::vec2(cd.z, cd.w));
        const glm::vec4 sl = c < streakLines.size() ? streakLines[c] : glm::vec4(far, 0.0f, far, 0.0f);
        words[7] = glm::packHalf2x16(glm::vec2(sl.x, sl.y));
        words[8] = glm::packHalf2x16(glm::vec2(sl.z, sl.w));
        words[9] = glm::packHalf2x16(glm::vec2(c < height.size() ? height[c] : far, 0.0f));
    });
}

/**
 * A copy of `from`'s render data for a mesh with the same layout key: new positions, normals and tangents only. With
 * an owner node, the vertices go to that node's GPU copy in place instead of a new upload.
 */
Entry rebuildPositions(const scene::MeshPtr& source, const Entry& from, scene::NodeId owner)
{
    LILA_ZONE();

    const geo::Mesh& m = *source;
    const geo::RenderMesh& layout = *from.pick;
    const size_t n = layout.positions.size();
    std::vector<glm::vec3> pos(n);
    tbb::parallel_for(size_t(0), n, [&](size_t v) { pos[v] = glm::vec3(m.position(layout.vertexPoint[v])); });
    const std::vector<glm::vec3> nrm = vertexNormals(m, layout);
    std::vector<glm::vec4> tangents;
    if (from.tangents && !layout.tangents.empty() && !layout.uvs.empty())
        fastTangents(layout, pos, nrm, tangents);

    auto out = std::make_shared<render::RenderMesh>(*from.built);
    fillGrunge(m, layout, *out);
    // The renderer uploads a live buffer's masks once, so meshes carrying them take a new key with their new masks.
    if (!out->grunge.empty())
        owner = scene::kNoNode;
    tbb::parallel_for(size_t(0), n, [&](size_t v) {
        render::MeshVertex& mv = out->vertices[v];
        mv.pos = pos[v];
        mv.normal = nrm[v];
        if (!tangents.empty())
            mv.tangent = tangents[v];
    });
    tbb::parallel_for(geo::Index(0), m.pointCapacity(), [&](geo::Index p) {
        out->edgePoints[p] = m.pointValid(p) ? glm::vec3(m.position(p)) : glm::vec3(0.0f);
    });
    if (n)
    {
        using Bounds = std::pair<glm::vec3, glm::vec3>;
        std::tie(out->boundsMin, out->boundsMax) = tbb::parallel_reduce(
            tbb::blocked_range<size_t>(0, n, 1 << 16), Bounds(pos[0], pos[0]),
            [&](const tbb::blocked_range<size_t>& r, Bounds b) {
                for (size_t i = r.begin(); i < r.end(); ++i)
                    b = {glm::min(b.first, pos[i]), glm::max(b.second, pos[i])};
                return b;
            },
            [](const Bounds& a, const Bounds& b) { return Bounds(glm::min(a.first, b.first), glm::max(a.second, b.second)); });
    }
    if (owner != scene::kNoNode)
    {
        auto& l = lives();
        if (l.size() > 256)
        {
            l.clear();
            liveHolders().clear();
        }
        // Keyed by the full build whose vertices and indices this copies: one layout key can weld into different
        // vertex layouts (stored split normals, tangents), and the renderer keeps one index buffer per live key.
        Live& live = l[{owner, from.built->key}];
        if (!live.vertices)
        {
            live.vertices = std::make_shared<render::DynamicVertices>();
            live.key = render::RenderMesh::nextKey();
        }
        {
            std::scoped_lock lock(live.vertices->mutex);
            live.vertices->vertices.assign(out->vertices.begin(), out->vertices.end());
            live.vertices->markAllDirty();
        }
        out->dynamic = live.vertices;
        out->key = live.key;
        liveHolders()[live.vertices.get()] = out.get();
    }
    else
        out->key = render::RenderMesh::nextKey();
    Entry e{source, out, nullptr, from.tangents};
    e.pickLayout = from.pick;
    e.owner = owner;
    return e;
}

Entry build(const scene::MeshPtr& source, bool tangents)
{
    LILA_ZONE();

    geo::RenderMeshOptions opts;
    opts.maxUvSets = 2;
    opts.colors = true;
    opts.sections = true;
    opts.tangents = tangents;
    opts.splitLayers = {render::kGrungeEdgeDistanceAttr, render::kGrungeContactDistanceAttr, render::kGrungeStreakLinesAttr, render::kGrungeHeightAttr};
    auto pick = std::make_shared<geo::RenderMesh>(geo::buildRenderMesh(*source, opts));
    const geo::RenderMesh& rm = *pick;

    auto out = std::make_shared<render::RenderMesh>();
    size_t n = rm.positions.size();
    out->vertices.resize(n);
    bool uvs = !rm.uvs.empty() && rm.uvs[0].size() == n;
    bool uvs1 = rm.uvs.size() > 1 && rm.uvs[1].size() == n;
    bool colors = rm.colors.size() == n;
    tbb::parallel_for(size_t(0), n, [&](size_t i) {
        render::MeshVertex& v = out->vertices[i];
        v.pos = rm.positions[i];
        v.normal = i < rm.normals.size() ? rm.normals[i] : glm::vec3(0, 1, 0);
        v.tangent = i < rm.tangents.size() ? rm.tangents[i] : glm::vec4(1, 0, 0, 1);
        v.uv = uvs ? rm.uvs[0][i] : glm::vec2(0.0f);
        v.uv1 = uvs1 ? rm.uvs[1][i] : v.uv;
        v.color = colors ? rm.colors[i] : glm::vec4(1.0f);
    });
    fillGrunge(*source, rm, *out);
    out->indices.assign(rm.indices.begin(), rm.indices.end());
    for (const geo::RenderSection& sec : rm.sections)
        out->sections.push_back({uint32_t(std::max(0, sec.material)), sec.firstIndex, sec.indexCount});
    out->edgePoints.resize(source->pointCapacity());
    tbb::parallel_for(geo::Index(0), source->pointCapacity(), [&](geo::Index p) {
        out->edgePoints[p] = source->pointValid(p) ? glm::vec3(source->position(p)) : glm::vec3(0.0f);
    });
    out->edgeMask.assign((rm.triangleSides.size() + 7) / 8, 0u);
    tbb::parallel_for(size_t(0), out->edgeMask.size(), [&](size_t w) {
        uint32_t word = 0;
        for (size_t t = w * 8; t < std::min(w * 8 + 8, rm.triangleSides.size()); ++t)
            word |= uint32_t(rm.triangleSides[t]) << ((t & 7) * 4);
        out->edgeMask[w] = word;
    });
    out->edgeIndices.resize(rm.lineIndices.size());
    tbb::parallel_for(size_t(0), rm.lineIndices.size(), [&](size_t i) { out->edgeIndices[i] = rm.vertexPoint[rm.lineIndices[i]]; });
    if (n)
    {
        using Bounds = std::pair<glm::vec3, glm::vec3>;
        std::tie(out->boundsMin, out->boundsMax) = tbb::parallel_reduce(
            tbb::blocked_range<size_t>(0, n, 1 << 16), Bounds(rm.positions[0], rm.positions[0]),
            [&](const tbb::blocked_range<size_t>& r, Bounds b) {
                for (size_t i = r.begin(); i < r.end(); ++i)
                    b = {glm::min(b.first, rm.positions[i]), glm::max(b.second, rm.positions[i])};
                return b;
            },
            [](const Bounds& a, const Bounds& b) { return Bounds(glm::min(a.first, b.first), glm::max(a.second, b.second)); });
    }
    return {source, out, pick, tangents};
}

/** Drops the entries whose meshes are gone; a whole scene's worth is freed on a thread of its own, off the frame. */
void dropExpired(std::unordered_map<const geo::Mesh*, Entry>& c)
{
    std::vector<Entry> doomed;
    for (auto it = c.begin(); it != c.end();)
        if (it->second.source.expired())
        {
            doomed.push_back(std::move(it->second));
            it = c.erase(it);
        }
        else
            ++it;
    constexpr size_t kOnItsOwn = 64;
    if (doomed.size() >= kOnItsOwn)
        release::laterEach(std::move(doomed));
}

const Entry* entryFor(const scene::MeshPtr& mesh, bool tangents = false, scene::NodeId owner = scene::kNoNode)
{
    if (!mesh)
        return nullptr;
    auto& c = cache();
    auto it = c.find(mesh.get());
    if (it != c.end() && it->second.source.lock() == mesh && (it->second.tangents || !tangents))
    {
        // Another node's live buffer is rewritten by that node's own edits, so anyone else draws a frozen copy.
        if (it->second.built->dynamic && owner != scene::kNoNode && it->second.owner != owner)
            return staticCopy(mesh, it->second);
        restoreLive(*it->second.built);
        return &it->second;
    }
    // Entries of deleted meshes hold gigabytes on dense meshes, so they go as soon as anything new is cached.
    dropExpired(c);
    std::erase_if(staticCopies(), [](const auto& kv) { return kv.second.source.expired(); });
    // A mesh too big for the memory left stays undrawn, reported once, rather than taking the app down.
    // Never destroyed: at exit the meshes' owners (extension DLLs among them) may already be unloaded.
    static auto& failed = *new std::weak_ptr<const geo::Mesh>();
    if (failed.lock() == mesh)
        return nullptr;
    try
    {
        // A deformed mesh (skinning, blend shapes) differs from the last copy only in its points: reuse that copy's
        // layout and skip triangulating and MikkTSpace, which on a dense character cost a frame many times over.
        const uint64_t key = layoutKey(*mesh);
        for (const Layout& l : layouts())
            if (l.key == key && (l.entry.tangents || !tangents) && l.entry.pick && l.entry.built &&
                l.entry.pick->positions.size() == l.entry.built->vertices.size())
                return &(c[mesh.get()] = rebuildPositions(mesh, l.entry, owner));
        Entry e = build(mesh, tangents);
        rememberLayout(key, e);
        return &(c[mesh.get()] = std::move(e));
    }
    catch (const std::bad_alloc&)
    {
        failed = mesh;
        log::error("not enough memory to draw a mesh of {} faces; free some memory and change the mesh to try again", mesh->numFaces());
        return nullptr;
    }
}
} // namespace

void prewarmRenderMeshes(const std::vector<std::pair<scene::MeshPtr, bool>>& meshes)
{
    LILA_ZONE();

    auto& c = cache();
    std::vector<std::pair<scene::MeshPtr, bool>> todo;
    std::unordered_set<const geo::Mesh*> seen;
    for (const auto& [mesh, tangents] : meshes)
    {
        if (!mesh || mesh->numFaces() == 0 || !seen.insert(mesh.get()).second)
            continue;
        if (auto it = c.find(mesh.get()); it != c.end() && it->second.source.lock() == mesh && (it->second.tangents || !tangents))
            continue;
        todo.emplace_back(mesh, tangents);
    }
    constexpr size_t kWorthIt = 16;
    if (todo.size() < kWorthIt)
        return;
    std::vector<std::optional<Entry>> built(todo.size());
    std::vector<uint64_t> keys(todo.size());
    tbb::parallel_for(size_t(0), todo.size(), [&](size_t i) {
        try
        {
            keys[i] = layoutKey(*todo[i].first);
            built[i] = build(todo[i].first, todo[i].second);
        }
        catch (const std::bad_alloc&)
        {
            // entryFor tries this one again on its own and reports it.
        }
    });
    dropExpired(c);
    for (size_t i = 0; i < todo.size(); ++i)
        if (built[i])
        {
            rememberLayout(keys[i], *built[i]);
            c[todo[i].first.get()] = std::move(*built[i]);
        }
}

std::shared_ptr<const render::RenderMesh> renderMeshFor(const scene::MeshPtr& mesh, bool tangents, scene::NodeId owner)
{
    const Entry* e = entryFor(mesh, tangents, owner);
    return e ? e->built : nullptr;
}

bool reuseRenderMesh(const scene::MeshPtr& from, const scene::MeshPtr& to, const std::function<void(UninitVector<render::MeshVertex>&)>& edit)
{
    LILA_ZONE();

    auto& c = cache();
    auto it = c.find(from.get());
    if (!from || !to || it == c.end() || it->second.source.lock() != from)
        return false;
    Entry e = it->second;
    auto copy = std::make_shared<render::RenderMesh>(*e.built);
    copy->key = render::RenderMesh::nextKey();
    edit(copy->vertices);
    e.source = to;
    e.built = std::move(copy);
    c[to.get()] = std::move(e);
    return true;
}

namespace
{
struct Masked
{
    std::weak_ptr<const geo::Mesh> source;
    std::shared_ptr<const render::RenderMesh> base;
    std::shared_ptr<const MaskedMesh> result;
};
using MaskedKey = std::tuple<const geo::Mesh*, uint64_t, const SmoothCage*>;

std::map<MaskedKey, Masked>& maskedCache()
{
    // Never destroyed: at exit the meshes' owners (extension DLLs among them) may already be unloaded.
    static auto& cache = *new std::map<MaskedKey, Masked>();
    return cache;
}

/** Entries of deleted meshes hold gigabytes on dense meshes, so they go as soon as anything new is cached; many at once on a thread of their own. */
void dropExpiredMasked()
{
    auto& cache = maskedCache();
    std::vector<Masked> doomed;
    for (auto it = cache.begin(); it != cache.end();)
        if (it->second.source.expired())
        {
            doomed.push_back(std::move(it->second));
            it = cache.erase(it);
        }
        else
            ++it;
    constexpr size_t kOnItsOwn = 64;
    if (doomed.size() >= kOnItsOwn)
        release::laterEach(std::move(doomed));
}

/** The subset of full's triangles and wire that faces (null: all) shows; reads only, so meshes can be masked in parallel. */
std::shared_ptr<MaskedMesh> buildMasked(const geo::Mesh& mesh, const Entry& full, const std::vector<uint8_t>* faces, const SmoothCage* cageWire)
{
    // Only the layout is read: which face each triangle and edge each line came from.
    const geo::RenderMesh& pd = full.pick ? *full.pick : *full.pickLayout;
    const render::RenderMesh& rm = *full.built;
    auto shown = [&](geo::Index f) { return !faces || (f < faces->size() && (*faces)[f]); };
    auto out = std::make_shared<MaskedMesh>();
    auto sub = std::make_shared<render::RenderMesh>();
    sub->shareVertices = full.built;
    sub->boundsMin = rm.boundsMin;
    sub->boundsMax = rm.boundsMax;
    const size_t tris = rm.indices.size() / 3;
    out->hiddenTriangles.assign(tris, 0);
    for (size_t t = 0; t < tris; ++t)
        out->hiddenTriangles[t] = t < pd.triangleFaces.size() && !shown(pd.triangleFaces[t]);
    std::vector<render::MeshSection> sections = rm.sections;
    if (sections.empty())
        sections.push_back({0, 0, uint32_t(rm.indices.size())});
    sub->indices.reserve(rm.indices.size());
    for (const render::MeshSection& sec : sections)
    {
        render::MeshSection kept{sec.material, uint32_t(sub->indices.size()), 0};
        for (uint32_t i = sec.firstIndex; i + 2 < sec.firstIndex + sec.indexCount && i + 2 < rm.indices.size(); i += 3)
            if (!out->hiddenTriangles[i / 3])
                sub->indices.insert(sub->indices.end(), rm.indices.begin() + i, rm.indices.begin() + i + 3);
        kept.indexCount = uint32_t(sub->indices.size()) - kept.firstIndex;
        if (kept.indexCount)
            sub->sections.push_back(kept);
    }
    // A wire edge shows while any face on it does.
    std::vector<uint8_t> edgeShown;
    if (!pd.lineEdges.empty())
    {
        edgeShown.assign(mesh.edgeCapacity(), 0);
        for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
            if (shown(f) && mesh.faceValid(f))
                mesh.forEachFaceCorner(f, [&](geo::Index c) { edgeShown[mesh.cornerEdge(c)] = 1; });
    }
    for (size_t l = 0; l < pd.lineEdges.size() && 2 * l + 1 < rm.edgeVertexCount(); ++l)
        if (pd.lineEdges[l] < edgeShown.size() && edgeShown[pd.lineEdges[l]] &&
            (!cageWire || (pd.lineEdges[l] < cageWire->map.edges.size() && cageWire->map.edges[pd.lineEdges[l]] != geo::kInvalid)))
        {
            sub->edgePoints.push_back(rm.edgePoint(2 * l));
            sub->edgePoints.push_back(rm.edgePoint(2 * l + 1));
        }
    out->mesh = std::move(sub);
    return out;
}

/** No edge maps onto it, so a mask with it keeps every triangle and drops every wire line. */
const SmoothCage& noEdges()
{
    static const SmoothCage& none = *new SmoothCage();
    return none;
}
} // namespace

std::shared_ptr<const MaskedMesh> maskedRenderMeshFor(const scene::MeshPtr& mesh, uint64_t maskKey, const std::function<std::vector<uint8_t>()>& shownFaces,
                                                      bool tangents, const SmoothCage* cageWire)
{
    LILA_ZONE();

    const Entry* full = entryFor(mesh, tangents);
    if (!full || !(full->pick || full->pickLayout))
        return nullptr;
    auto& cache = maskedCache();
    const MaskedKey key = std::make_tuple(mesh.get(), maskKey, cageWire);
    if (auto it = cache.find(key); it != cache.end() && it->second.source.lock() == mesh && it->second.base == full->built)
        return it->second.result;
    dropExpiredMasked();
    const std::vector<uint8_t> faces = shownFaces ? shownFaces() : std::vector<uint8_t>();
    std::shared_ptr<const MaskedMesh> out = buildMasked(*mesh, *full, shownFaces ? &faces : nullptr, cageWire);
    cache[key] = {mesh, full->built, out};
    return out;
}

void dropExpiredRenderMeshes()
{
    LILA_ZONE();
    dropExpired(cache());
    dropExpired(staticCopies());
    dropExpiredMasked();
}

void prewarmWirelessMeshes(const std::vector<std::pair<scene::MeshPtr, bool>>& meshes)
{
    LILA_ZONE();

    auto& cache = maskedCache();
    const SmoothCage* none = &noEdges();
    std::vector<std::pair<scene::MeshPtr, const Entry*>> todo;
    std::unordered_set<const geo::Mesh*> seen;
    for (const auto& [mesh, tangents] : meshes)
    {
        if (!mesh || !seen.insert(mesh.get()).second)
            continue;
        const Entry* full = entryFor(mesh, tangents);
        if (!full || !(full->pick || full->pickLayout))
            continue;
        if (auto it = cache.find(std::make_tuple(mesh.get(), uint64_t(0), none));
            it != cache.end() && it->second.source.lock() == mesh && it->second.base == full->built)
            continue;
        todo.emplace_back(mesh, full);
    }
    constexpr size_t kWorthIt = 16;
    if (todo.size() < kWorthIt)
        return;
    std::vector<std::shared_ptr<MaskedMesh>> built(todo.size());
    tbb::parallel_for(size_t(0), todo.size(), [&](size_t i) { built[i] = buildMasked(*todo[i].first, *todo[i].second, nullptr, none); });
    dropExpiredMasked();
    for (size_t i = 0; i < todo.size(); ++i)
        cache[std::make_tuple(todo[i].first.get(), uint64_t(0), none)] = {todo[i].first, todo[i].second->built, built[i]};
}

scene::MeshPtr smoothedFor(const scene::MeshPtr& mesh, int level)
{
    if (!mesh || level <= 0)
        return mesh;
    auto smooth = smoothCageFor(mesh, level);
    return smooth ? smooth->mesh : mesh;
}

namespace
{
/** Groups items by key (one per item, kInvalid skipped) into offsets and ids, keys in [0, keys). */
void invert(std::span<const geo::Index> keyOf, size_t keys, std::vector<geo::Index>& offsets, std::vector<geo::Index>& ids)
{
    offsets.assign(keys + 1, 0);
    for (geo::Index k : keyOf)
        if (k < keys)
            ++offsets[k + 1];
    for (size_t k = 0; k < keys; ++k)
        offsets[k + 1] += offsets[k];
    ids.resize(offsets[keys]);
    std::vector<geo::Index> fill(offsets.begin(), offsets.end() - 1);
    for (geo::Index i = 0; i < keyOf.size(); ++i)
        if (keyOf[i] < keys)
            ids[fill[keyOf[i]]++] = i;
}
} // namespace

std::shared_ptr<const SmoothCage> smoothCageFor(const scene::MeshPtr& mesh, int level)
{
    if (!mesh || level <= 0)
        return nullptr;
    struct Smoothed
    {
        std::weak_ptr<const geo::Mesh> source;
        int level = 0;
        bool keepHardEdges = false;
        std::shared_ptr<const SmoothCage> result;
    };
    // Never destroyed: at exit the meshes' owners (extension DLLs among them) may already be unloaded.
    static auto& cache = *new std::unordered_map<const geo::Mesh*, Smoothed>();
    const bool keepHardEdges = PrefStore::get().value<bool>("general", viewportDisplay::kSmoothHardEdges, false);
    auto it = cache.find(mesh.get());
    if (it != cache.end() && it->second.level == level && it->second.keepHardEdges == keepHardEdges && it->second.source.lock() == mesh)
        return it->second.result;
    LILA_ZONE();
    // Entries of deleted meshes hold gigabytes on dense meshes, so they go as soon as anything new is cached.
    std::erase_if(cache, [](const auto& kv) { return kv.second.source.expired(); });
    geo::SubdivideOptions o;
    o.levels = level;
    o.keepHardEdges = keepHardEdges;
    // The preview rebuilds on every edit, and fitting island interiors costs a solve each time.
    o.uvs = geo::SubdivideUvs::Blend;
    auto out = std::make_shared<SmoothCage>();
    auto smoothed = std::make_shared<const geo::Mesh>(geo::subdivide(*mesh, o, &out->map));
    out->cagePoints.assign(smoothed->pointCapacity(), geo::kInvalid);
    for (geo::Index p = 0; p < out->map.points.size(); ++p)
        if (out->map.points[p] < out->cagePoints.size())
            out->cagePoints[out->map.points[p]] = p;
    invert(out->map.edges, mesh->edgeCapacity(), out->edgeOffsets, out->edgeSegments);
    invert(out->map.faces, mesh->faceCapacity(), out->faceOffsets, out->faceParts);
    out->mesh = std::move(smoothed);
    cache[mesh.get()] = {mesh, level, keepHardEdges, out};
    return out;
}

namespace
{
/** Whether the meshes have the same edges between the same point ids, as a stack of deformers alone leaves them. */
bool sameEdges(const geo::Mesh& a, const geo::Mesh& b)
{
    if (a.edgeCapacity() != b.edgeCapacity() || a.pointCapacity() != b.pointCapacity())
        return false;
    for (geo::Index e = 0; e < a.edgeCapacity(); ++e)
        if (a.edgeValid(e) != b.edgeValid(e) || (a.edgeValid(e) && (a.edgePoint(e, 0) != b.edgePoint(e, 0) || a.edgePoint(e, 1) != b.edgePoint(e, 1))))
            return false;
    return true;
}
} // namespace

bool modifiersAddEdges(const scene::MeshPtr& cage, const scene::MeshPtr& result)
{
    return cage && result && cage != result && !sameEdges(*cage, *result);
}

namespace
{
/** Squared distance from p to the segment ab. */
double segmentDistance2(const glm::dvec3& p, const glm::dvec3& a, const glm::dvec3& b)
{
    const glm::dvec3 ab = b - a;
    const double t = glm::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-300), 0.0, 1.0);
    const glm::dvec3 d = a + ab * t - p;
    return glm::dot(d, d);
}

std::shared_ptr<render::RenderMesh> lineMesh(const std::shared_ptr<const render::RenderMesh>& surface)
{
    auto m = std::make_shared<render::RenderMesh>();
    m->shareVertices = surface;
    m->boundsMin = surface->boundsMin;
    m->boundsMax = surface->boundsMax;
    return m;
}
} // namespace

ModifierWire modifierWireFor(const scene::MeshPtr& cage, const scene::MeshPtr& result)
{
    if (!cage || !result || cage == result || result->numFaces() == 0)
        return {};
    struct Built
    {
        std::weak_ptr<const geo::Mesh> cage, result;
        ModifierWire wire;
    };
    // Never destroyed: at exit the meshes' owners (extension DLLs among them) may already be unloaded.
    static auto& cache = *new std::unordered_map<const geo::Mesh*, Built>();
    if (auto it = cache.find(result.get()); it != cache.end() && it->second.result.lock() == result && it->second.cage.lock() == cage)
        return it->second.wire;
    auto surface = renderMeshFor(result);
    if (!surface)
        return {};
    LILA_ZONE();
    std::erase_if(cache, [](const auto& kv) { return kv.second.result.expired() || kv.second.cage.expired(); });
    const geo::Mesh& r = *result;
    const geo::MeshBvh bvh(r);
    auto real = lineMesh(surface), added = lineMesh(surface);

    // Each cage edge, sampled and laid onto the surface. Samples crowd the ends, where the surface turns around a corner,
    // and sit a hair off it so the straight pieces between them don't sink under a curve.
    constexpr int kSamples = 32;
    std::vector<std::pair<glm::dvec3, glm::dvec3>> segments;
    double total = 0.0;
    for (geo::Index e = 0; e < cage->edgeCapacity(); ++e)
    {
        if (!cage->edgeValid(e))
            continue;
        const glm::dvec3 a = cage->position(cage->edgePoint(e, 0)), b = cage->position(cage->edgePoint(e, 1));
        glm::dvec3 prev{0.0};
        for (int i = 0; i <= kSamples; ++i)
        {
            geo::BvhClosest hit;
            const double t = 0.5 - 0.5 * std::cos(std::numbers::pi * double(i) / kSamples);
            const glm::dvec3 at = glm::mix(a, b, t);
            glm::dvec3 on = bvh.closestPoint(at, hit) ? hit.point : at;
            if (const double off = glm::length(at - on); off > 0.0)
                on += (at - on) * (std::min(off * 0.1, 0.002 * glm::length(b - a)) / off);
            if (i > 0)
            {
                segments.push_back({prev, on});
                total += glm::length(on - prev);
            }
            prev = on;
        }
    }
    for (const auto& [a, b] : segments)
    {
        real->edgePoints.push_back(glm::vec3(a));
        real->edgePoints.push_back(glm::vec3(b));
    }

    // Result edges lying on those lines are already drawn by them; the rest are the modifiers' own.
    const double cell = std::max(segments.empty() ? 1.0 : 2.0 * total / double(segments.size()), 1e-6);
    auto cellOf = [cell](const glm::dvec3& p) {
        return std::array<int64_t, 3>{int64_t(std::floor(p.x / cell)), int64_t(std::floor(p.y / cell)), int64_t(std::floor(p.z / cell))};
    };
    std::map<std::array<int64_t, 3>, std::vector<uint32_t>> grid;
    for (uint32_t i = 0; i < segments.size(); ++i)
    {
        const auto lo = cellOf(glm::min(segments[i].first, segments[i].second)), hi = cellOf(glm::max(segments[i].first, segments[i].second));
        for (int64_t x = lo[0]; x <= hi[0]; ++x)
            for (int64_t y = lo[1]; y <= hi[1]; ++y)
                for (int64_t z = lo[2]; z <= hi[2]; ++z)
                    grid[{x, y, z}].push_back(i);
    }
    auto onLine = [&](const glm::dvec3& p, double tolerance) {
        const auto c = cellOf(p);
        for (int64_t x = -1; x <= 1; ++x)
            for (int64_t y = -1; y <= 1; ++y)
                for (int64_t z = -1; z <= 1; ++z)
                    if (auto it = grid.find({c[0] + x, c[1] + y, c[2] + z}); it != grid.end())
                        for (uint32_t i : it->second)
                            if (segmentDistance2(p, segments[i].first, segments[i].second) <= tolerance * tolerance)
                                return true;
        return false;
    };
    for (geo::Index e = 0; e < r.edgeCapacity(); ++e)
    {
        if (!r.edgeValid(e))
            continue;
        const glm::dvec3 a = r.position(r.edgePoint(e, 0)), b = r.position(r.edgePoint(e, 1));
        const double tolerance = 0.02 * glm::length(b - a) + 1e-5;
        if (onLine((a + b) * 0.5, tolerance) && onLine(a, tolerance) && onLine(b, tolerance))
            continue;
        added->edgePoints.push_back(glm::vec3(a));
        added->edgePoints.push_back(glm::vec3(b));
    }
    ModifierWire out{real, added};
    cache[result.get()] = {cage, result, out};
    return out;
}

std::shared_ptr<const render::RenderMesh> wirelessMeshFor(const scene::MeshPtr& mesh, bool tangents)
{
    auto masked = maskedRenderMeshFor(mesh, 0, {}, tangents, &noEdges());
    return masked ? masked->mesh : nullptr;
}

std::shared_ptr<const render::RenderMesh> cageWireMeshFor(const SmoothCage& smooth, bool tangents)
{
    auto masked = maskedRenderMeshFor(smooth.mesh, 0, {}, tangents, &smooth);
    return masked ? masked->mesh : renderMeshFor(smooth.mesh, tangents);
}

std::shared_ptr<const geo::RenderMesh> pickDataFor(const scene::MeshPtr& mesh)
{
    const Entry* e = entryFor(mesh);
    if (!e)
        return nullptr;
    if (!e->pick && e->pickLayout)
        const_cast<Entry*>(e)->pick = pickFrom(*mesh, *e->pickLayout);
    return e->pick;
}

std::optional<RayHit> raycast(const render::RenderMesh& mesh, const glm::dmat4& world, const glm::dvec3& origin, const glm::dvec3& dir,
                              const std::vector<uint8_t>* hidden)
{
    LILA_ZONE();

    glm::dmat4 inv = glm::inverse(world);
    glm::dvec3 o = glm::dvec3(inv * glm::dvec4(origin, 1.0));
    glm::dvec3 d = glm::dvec3(inv * glm::dvec4(dir, 0.0));

    // Slab test against the bounds first; most meshes miss.
    glm::dvec3 lo(mesh.boundsMin), hi(mesh.boundsMax);
    double t0 = 0.0, t1 = 1e300;
    for (int a = 0; a < 3; ++a)
    {
        if (std::abs(d[a]) < 1e-300)
        {
            if (o[a] < lo[a] || o[a] > hi[a])
                return std::nullopt;
            continue;
        }
        double ta = (lo[a] - o[a]) / d[a], tb = (hi[a] - o[a]) / d[a];
        if (ta > tb)
            std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1)
            return std::nullopt;
    }

    using Best = std::pair<double, size_t>; // distance along the local ray, triangle
    size_t tris = mesh.indices.size() / 3;
    Best best = tbb::parallel_reduce(
        tbb::blocked_range<size_t>(0, tris, 4096), Best{1e300, 0},
        [&](const tbb::blocked_range<size_t>& r, Best acc) {
            for (size_t t = r.begin(); t != r.end(); ++t)
            {
                if (hidden && t < hidden->size() && (*hidden)[t])
                    continue;
                // Moller-Trumbore, both sides.
                glm::dvec3 a(mesh.vertices[mesh.indices[t * 3]].pos), b(mesh.vertices[mesh.indices[t * 3 + 1]].pos),
                    c(mesh.vertices[mesh.indices[t * 3 + 2]].pos);
                glm::dvec3 e1 = b - a, e2 = c - a, p = glm::cross(d, e2);
                double det = glm::dot(e1, p);
                if (std::abs(det) < 1e-18)
                    continue;
                double invDet = 1.0 / det;
                glm::dvec3 s = o - a;
                double u = glm::dot(s, p) * invDet;
                if (u < 0.0 || u > 1.0)
                    continue;
                glm::dvec3 q = glm::cross(s, e1);
                double v = glm::dot(d, q) * invDet;
                if (v < 0.0 || u + v > 1.0)
                    continue;
                double dist = glm::dot(e2, q) * invDet;
                if (dist > 0.0 && dist < acc.first)
                    acc = {dist, t};
            }
            return acc;
        },
        [](Best x, Best y) { return x.first <= y.first ? x : y; });
    if (best.first >= 1e300)
        return std::nullopt;
    // Distance is in the local ray's units; convert to world.
    return RayHit{glm::length(glm::dvec3(world * glm::dvec4(d * best.first, 0.0))), uint32_t(best.second)};
}
} // namespace lila::ui
