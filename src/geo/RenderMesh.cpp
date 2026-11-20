#include "geo/RenderMesh.h"

#include "core/Profile.h"
#include "geo/ColorSets.h"

#include <tbb/blocked_range.h>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_scan.h>

#include <cstring>

namespace lila::geo
{
namespace
{
constexpr Index kRenderGrain = 4096;

struct CornerData
{
    std::span<const glm::vec3> normals;
    std::vector<std::span<const glm::vec2>> uvs;
    std::span<const glm::vec4> cornerColor;
    std::span<const glm::vec4> pointColor;
    std::span<const glm::vec4> tangents;
    std::vector<std::span<const glm::vec4>> splitVec4;
    std::vector<std::span<const float>> splitFloat;

    bool same(Index a, Index b) const
    {
        for (const std::span<const glm::vec4>& s : splitVec4)
            if (s[a] != s[b])
                return false;
        for (const std::span<const float>& s : splitFloat)
            if (s[a] != s[b])
                return false;
        if (normals[a] != normals[b])
            return false;
        for (const std::span<const glm::vec2>& uv : uvs)
            if (uv[a] != uv[b])
                return false;
        if (!cornerColor.empty() && cornerColor[a] != cornerColor[b])
            return false;
        if (!tangents.empty() && tangents[a] != tangents[b])
            return false;
        return true;
    }
};
} // namespace

RenderMesh buildRenderMesh(const Mesh& mesh, const RenderMeshOptions& opts)
{
    LILA_ZONE();

    RenderMesh out;
    const Index cc = mesh.cornerCapacity();
    const Index pc = mesh.pointCapacity();

    std::vector<glm::vec3> computedNormals;
    CornerData cd;
    cd.normals = mesh.cornerAttrs().get<glm::vec3>(attr::kNormal);
    if (cd.normals.size() != cc)
    {
        computedNormals = cornerNormals(mesh, opts.normals);
        cd.normals = computedNormals;
    }
    for (const std::string& layer : opts.splitLayers)
    {
        if (const auto v = mesh.cornerAttrs().get<glm::vec4>(layer); !v.empty())
            cd.splitVec4.push_back(v);
        else if (const auto f = mesh.cornerAttrs().get<float>(layer); !f.empty())
            cd.splitFloat.push_back(f);
    }
    for (int i = 0; i < opts.maxUvSets; ++i)
    {
        std::span<const glm::vec2> uv = mesh.cornerAttrs().get<glm::vec2>(attr::uvName(i));
        if (uv.empty())
            break;
        cd.uvs.push_back(uv);
    }
    if (opts.colors)
    {
        cd.cornerColor = mesh.cornerAttrs().get<glm::vec4>(opts.colorLayer.empty() ? colors::layerName(colors::displayed(mesh)) : opts.colorLayer);
        if (cd.cornerColor.empty() && opts.colorLayer.empty())
            cd.pointColor = mesh.pointAttrs().get<glm::vec4>(attr::kColor);
    }

    const Triangulation tris = triangulate(mesh, opts.triangulation);

    std::vector<glm::vec4> computedTangents;
    if (opts.tangents && !cd.uvs.empty())
    {
        computedTangents = cornerTangents(mesh, tris, cd.normals, attr::uvName(0));
        cd.tangents = computedTangents;
    }

    // Pass 1: slot of each corner among the unique vertices of its point. Dead corners and points keep none.
    UninitVector<Index> slot(cc);
    tbb::parallel_for(Index(0), cc, [&](Index c) { slot[c] = kInvalid; });
    UninitVector<Index> base(size_t(pc) + 1);
    tbb::parallel_for(Index(0), pc + 1, [&](Index p) { base[p] = 0; });
    {
        LILA_ZONE_N("RenderMesh::assignSlots");

        tbb::enumerable_thread_specific<std::vector<Index>> scratch;
        tbb::parallel_for(tbb::blocked_range<Index>(0, pc, kRenderGrain), [&](const tbb::blocked_range<Index>& r) {
            std::vector<Index>& reps = scratch.local();
            for (Index p = r.begin(); p < r.end(); ++p)
            {
                if (!mesh.pointValid(p))
                    continue;
                reps.clear();
                mesh.forEachPointCorner(p, [&](Index c) {
                    for (Index k = 0; k < reps.size(); ++k)
                        if (cd.same(reps[k], c))
                        {
                            slot[c] = k;
                            return;
                        }
                    slot[c] = Index(reps.size());
                    reps.push_back(c);
                });
                base[p + 1] = Index(reps.size());
            }
        });
    }
    const Index vertexCount = tbb::parallel_scan(
        tbb::blocked_range<Index>(0, pc + 1, 1 << 16), Index(0),
        [&](const tbb::blocked_range<Index>& r, Index sum, bool final) {
            for (Index i = r.begin(); i < r.end(); ++i)
            {
                sum += base[i];
                if (final)
                    base[i] = sum;
            }
            return sum;
        },
        std::plus<Index>());

    out.positions.resize(vertexCount);
    out.normals.resize(vertexCount);
    out.vertexPoint.resize(vertexCount);
    out.vertexCorner.resize(vertexCount);
    out.uvs.resize(cd.uvs.size());
    for (auto& uv : out.uvs)
        uv.resize(vertexCount);
    const bool hasColor = !cd.cornerColor.empty() || !cd.pointColor.empty();
    if (hasColor)
        out.colors.resize(vertexCount);
    if (!cd.tangents.empty())
        out.tangents.resize(vertexCount);

    // Pass 2: the first corner of each slot writes the vertex.
    UninitVector<Index> cornerVertex(cc);
    tbb::parallel_for(Index(0), cc, [&](Index c) { cornerVertex[c] = kInvalid; });
    {
        LILA_ZONE_N("RenderMesh::fillVertices");

        tbb::parallel_for(tbb::blocked_range<Index>(0, pc, kRenderGrain), [&](const tbb::blocked_range<Index>& r) {
            for (Index p = r.begin(); p < r.end(); ++p)
            {
                if (!mesh.pointValid(p))
                    continue;
                const glm::vec3 pos(mesh.position(p));
                Index next = 0;
                mesh.forEachPointCorner(p, [&](Index c) {
                    const Index v = base[p] + slot[c];
                    cornerVertex[c] = v;
                    if (slot[c] != next)
                        return;
                    ++next;
                    out.positions[v] = pos;
                    out.normals[v] = cd.normals[c];
                    out.vertexPoint[v] = p;
                    out.vertexCorner[v] = c;
                    for (size_t i = 0; i < cd.uvs.size(); ++i)
                        out.uvs[i][v] = cd.uvs[i][c];
                    if (!cd.cornerColor.empty())
                        out.colors[v] = cd.cornerColor[c];
                    else if (!cd.pointColor.empty())
                        out.colors[v] = cd.pointColor[p];
                    if (!cd.tangents.empty())
                        out.tangents[v] = cd.tangents[c];
                });
            }
        });
    }

    out.cornerVertex = std::move(cornerVertex);

    // Triangles, grouped by material with a stable counting sort.
    const Index triCount = tris.triangleCount();
    std::span<const int32_t> material = mesh.faceAttrs().get<int32_t>(attr::kMaterial);
    std::vector<Index> order(triCount);
    if (opts.sections && !material.empty())
    {
        int32_t maxMat = 0;
        for (Index t = 0; t < triCount; ++t)
            maxMat = std::max(maxMat, material[tris.faces[t]]);
        std::vector<Index> count(size_t(maxMat) + 2, 0);
        for (Index t = 0; t < triCount; ++t)
            ++count[size_t(std::max(0, material[tris.faces[t]])) + 1];
        for (size_t m = 1; m < count.size(); ++m)
        {
            if (count[m])
                out.sections.push_back({int32_t(m - 1), count[m - 1] * 3, count[m] * 3});
            count[m] += count[m - 1];
        }
        for (RenderSection& s : out.sections)
            s.firstIndex = count[size_t(s.material)] * 3;
        for (Index t = 0; t < triCount; ++t)
            order[count[size_t(std::max(0, material[tris.faces[t]]))]++] = t;
    }
    else
    {
        for (Index t = 0; t < triCount; ++t)
            order[t] = t;
        if (triCount)
            out.sections.push_back({0, 0, triCount * 3});
    }

    out.indices.resize(size_t(triCount) * 3);
    out.triangleFaces.resize(triCount);
    out.triangleSides.resize(triCount);
    tbb::parallel_for(tbb::blocked_range<Index>(0, triCount, kRenderGrain * 4), [&](const tbb::blocked_range<Index>& r) {
        for (Index i = r.begin(); i < r.end(); ++i)
        {
            const Index t = order[i];
            uint8_t sides = 0;
            for (int k = 0; k < 3; ++k)
            {
                const Index a = tris.corners[size_t(t) * 3 + k], b = tris.corners[size_t(t) * 3 + (k + 1) % 3];
                out.indices[size_t(i) * 3 + k] = out.cornerVertex[a];
                // A side between neighbouring corners of the polygon is one of its edges; the rest split it.
                if (mesh.cornerNext(a) == b || mesh.cornerNext(b) == a)
                    sides |= uint8_t(1u << k);
            }
            out.triangleFaces[i] = tris.faces[t];
            out.triangleSides[i] = sides;
        }
    });

    if (opts.wireframe)
    {
        LILA_ZONE_N("RenderMesh::lines");

        const Index ec = mesh.edgeCapacity();
        UninitVector<Index> lineSlot(size_t(ec) + 1);
        tbb::parallel_scan(
            tbb::blocked_range<Index>(0, ec + 1, 1 << 16), Index(0),
            [&](const tbb::blocked_range<Index>& r, Index sum, bool final) {
                for (Index e = r.begin(); e < r.end(); ++e)
                {
                    if (final)
                        lineSlot[e] = sum;
                    sum += e < ec && mesh.edgeValid(e) && mesh.edgeCorner(e) != kInvalid ? 1 : 0;
                }
                return sum;
            },
            std::plus<Index>());
        out.lineIndices.resize(size_t(lineSlot[ec]) * 2);
        out.lineEdges.resize(lineSlot[ec]);
        tbb::parallel_for(tbb::blocked_range<Index>(0, ec, kRenderGrain * 4), [&](const tbb::blocked_range<Index>& r) {
            for (Index e = r.begin(); e < r.end(); ++e)
            {
                if (lineSlot[e + 1] == lineSlot[e])
                    continue;
                const Index c = mesh.edgeCorner(e);
                const Index l = lineSlot[e];
                out.lineIndices[size_t(l) * 2] = out.cornerVertex[c];
                out.lineIndices[size_t(l) * 2 + 1] = out.cornerVertex[mesh.cornerNext(c)];
                out.lineEdges[l] = e;
            }
        });
    }
    return out;
}
} // namespace lila::geo
