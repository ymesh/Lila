#include "scene/MeshBoolean.h"

#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/ColorSets.h"
#include "geo/Normals.h"
#include "geo/Ops.h"
#include "geo/Triangulate.h"
#include "retopo/Retopo.h"

#include <manifold/manifold.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace lila::scene
{
namespace
{
using geo::Index;
using geo::kInvalid;

constexpr int kProps = 6; // x y z u v point, then rgba per color set
// Scratch attributes while the result is being cleaned up.
constexpr std::string_view kOriginAttr = "booleanOrigin";   // point int: the operand point it is, plus one; 0 is new
constexpr std::string_view kOperandAttr = "booleanOperand"; // face int

/**
 * World-space MeshGL with one vertex per corner (so uvs and colours survive), welded by position for manifoldness.
 * faceID is the polygon's id plus faceOffset, so triangles of one polygon merge back. The point id rides along as a
 * property: Manifold copies it exactly onto points that survive and interpolates it on new ones.
 */
manifold::MeshGL toManifold(const geo::Mesh& mesh, const glm::dmat4& world, uint32_t faceOffset, const std::vector<std::string>& colorSets)
{
    geo::Triangulation tris = geo::triangulate(mesh);
    auto uv = mesh.cornerAttrs().get<glm::vec2>("uv0");
    std::vector<std::span<const glm::vec4>> colors;
    for (const std::string& set : colorSets)
        colors.push_back(geo::colors::values(mesh, set));
    manifold::MeshGL gl;
    gl.numProp = uint32_t(kProps + 4 * colorSets.size());
    std::unordered_map<Index, uint32_t> vertOfCorner;
    for (size_t t = 0; t < tris.faces.size(); ++t)
    {
        for (int k = 0; k < 3; ++k)
        {
            Index c = tris.corners[t * 3 + k];
            auto [it, fresh] = vertOfCorner.try_emplace(c, uint32_t(gl.vertProperties.size() / gl.numProp));
            if (fresh)
            {
                Index point = mesh.cornerPoint(c);
                glm::dvec3 p = glm::dvec3(world * glm::dvec4(mesh.position(point), 1.0));
                glm::vec2 t2 = c < uv.size() ? uv[c] : glm::vec2(0.0f);
                gl.vertProperties.insert(gl.vertProperties.end(), {float(p.x), float(p.y), float(p.z), t2.x, t2.y, float(point)});
                for (const auto& col : colors)
                {
                    glm::vec4 v = c < col.size() ? col[c] : glm::vec4(1.0f);
                    gl.vertProperties.insert(gl.vertProperties.end(), {v.r, v.g, v.b, v.a});
                }
            }
            gl.triVerts.push_back(it->second);
        }
        gl.faceID.push_back(faceOffset + uint32_t(tris.faces[t]));
    }
    gl.Merge();
    return gl;
}

/**
 * Rebuilds a polygon mesh in toLocal space: triangles of the same input polygon merge back, faces carry their operand
 * and material, points that are operand points say which, and edges that were operand edges keep their flags.
 */
geo::Mesh fromManifold(const manifold::MeshGL& gl, const glm::dmat4& toLocal, std::span<const BooleanOperand> operands,
                       std::span<const uint32_t> firstFace)
{
    LILA_ZONE();

    const geo::Mesh& base = *operands[0].mesh;
    auto operandOfFace = [&](uint32_t id) {
        return Index(std::upper_bound(firstFace.begin(), firstFace.end(), id) - firstFace.begin() - 1);
    };

    size_t verts = gl.NumVert();
    std::vector<uint32_t> canon(verts);
    for (size_t v = 0; v < verts; ++v)
        canon[v] = uint32_t(v);
    for (size_t i = 0; i < gl.mergeFromVert.size(); ++i)
        canon[gl.mergeFromVert[i]] = gl.mergeToVert[i];

    std::vector<Index> pointOf(verts, kInvalid);
    std::vector<glm::dvec3> points;
    for (size_t v = 0; v < verts; ++v)
    {
        uint32_t c = canon[v];
        if (pointOf[c] == kInvalid)
        {
            const float* p = &gl.vertProperties[size_t(c) * gl.numProp];
            pointOf[c] = Index(points.size());
            points.push_back(glm::dvec3(toLocal * glm::dvec4(p[0], p[1], p[2], 1.0)));
        }
        pointOf[v] = pointOf[c];
    }
    std::vector<Index> sizes(gl.NumTri(), 3), idx;
    idx.reserve(gl.triVerts.size());
    for (uint32_t v : gl.triVerts)
        idx.push_back(pointOf[v]);
    geo::Mesh m = geo::Mesh::fromPolygons(points, sizes, idx);

    // Which operand point each result point is, if any: its id survived exactly and it sits where that point went.
    std::vector<double> scale(operands.size(), 0.0);
    for (size_t o = 0; o < operands.size(); ++o)
        scale[o] = glm::length(operands[o].mesh->bounds().size()) * 1e-5 + 1e-9;
    auto origin = m.pointAttrs().getOrAdd<int32_t>(kOriginAttr);
    std::vector<Index> operandOfPoint(m.pointCapacity(), kInvalid);
    for (size_t t = 0; t < gl.NumTri(); ++t)
        for (int k = 0; k < 3; ++k)
        {
            uint32_t v = gl.triVerts[t * 3 + k];
            Index p = pointOf[v];
            if (origin[p] != 0)
                continue;
            Index o = operandOfFace(gl.faceID[t]);
            const geo::Mesh& src = *operands[o].mesh;
            float id = gl.vertProperties[size_t(v) * gl.numProp + 5];
            if (id != std::floor(id) || id < 0.0f || Index(id) >= src.pointCapacity() || !src.pointValid(Index(id)))
                continue;
            glm::dvec3 w = glm::dvec3(operands[o].toWorld * glm::dvec4(src.position(Index(id)), 1.0));
            const float* q = &gl.vertProperties[size_t(v) * gl.numProp];
            if (glm::length(w - glm::dvec3(q[0], q[1], q[2])) <= scale[o])
            {
                origin[p] = int32_t(id) + 1;
                operandOfPoint[p] = o;
            }
        }

    const std::vector<std::string> sets = geo::colors::sets(base);
    for (const std::string& set : sets)
        geo::colors::add(m, set);
    auto uv = m.cornerAttrs().getOrAdd<glm::vec2>("uv0");
    std::vector<std::span<glm::vec4>> colors;
    for (const std::string& set : sets)
        colors.push_back(geo::colors::values(m, set));
    auto operandAttr = m.faceAttrs().getOrAdd<int32_t>(kOperandAttr);

    // Materials by name into the base's list, so a cutter's material shows inside the cut.
    bool anyMaterial = false;
    for (const BooleanOperand& op : operands)
        anyMaterial |= op.mesh->faceAttrs().has(geo::attr::kMaterial);
    std::vector<std::vector<int32_t>> materialRemap(operands.size());
    if (anyMaterial)
    {
        m.materialNames() = base.materialNames();
        for (size_t o = 0; o < operands.size(); ++o)
            for (const std::string& name : operands[o].mesh->materialNames())
            {
                auto it = std::find(m.materialNames().begin(), m.materialNames().end(), name);
                if (it == m.materialNames().end())
                    it = m.materialNames().insert(m.materialNames().end(), name);
                materialRemap[o].push_back(int32_t(it - m.materialNames().begin()));
            }
    }
    std::span<int32_t> material = anyMaterial ? m.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial) : std::span<int32_t>{};

    for (Index f = 0; f < m.faceCapacity() && f < gl.NumTri(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        Index o = operandOfFace(gl.faceID[f]);
        operandAttr[f] = int32_t(o);
        if (anyMaterial)
        {
            auto srcMat = operands[o].mesh->faceAttrs().get<int32_t>(geo::attr::kMaterial);
            Index srcFace = gl.faceID[f] - firstFace[o];
            int32_t mi = srcFace < srcMat.size() ? srcMat[srcFace] : 0;
            material[f] = mi >= 0 && size_t(mi) < materialRemap[o].size() ? materialRemap[o][mi] : 0;
        }
        int k = 0;
        m.forEachFaceCorner(f, [&](Index c) {
            const float* p = &gl.vertProperties[size_t(gl.triVerts[f * 3 + k]) * gl.numProp];
            uv[c] = glm::vec2(p[3], p[4]);
            for (size_t s = 0; s < colors.size() && kProps + 4 * s + 3 < gl.numProp; ++s)
                colors[s][c] = glm::vec4(p[kProps + 4 * s], p[kProps + 4 * s + 1], p[kProps + 4 * s + 2], p[kProps + 4 * s + 3]);
            ++k;
        });
    }
    m.colorSetInfo() = base.colorSetInfo();

    // Edges between two points of one operand that were an edge there keep its flags.
    auto hard = m.edgeAttrs().getOrAdd<bool>(geo::attr::kHard);
    auto uvSeam = m.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
    std::span<float> crease;
    for (const BooleanOperand& op : operands)
        if (op.mesh->edgeAttrs().has(geo::attr::kCrease))
            crease = m.edgeAttrs().getOrAdd<float>(geo::attr::kCrease);
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e))
            continue;
        Index a = m.edgePoint(e, 0), b = m.edgePoint(e, 1);
        Index o = operandOfPoint[a];
        if (o == kInvalid || o != operandOfPoint[b])
            continue;
        const geo::Mesh& src = *operands[o].mesh;
        Index se = src.findEdge(Index(origin[a] - 1), Index(origin[b] - 1));
        if (se == kInvalid)
            continue;
        auto sh = src.edgeAttrs().get<bool>(geo::attr::kHard);
        auto ss = src.edgeAttrs().get<bool>(geo::attr::kSeam);
        auto sc = src.edgeAttrs().get<float>(geo::attr::kCrease);
        hard[e] = se < sh.size() && sh[se];
        uvSeam[e] = se < ss.size() && ss[se];
        if (!crease.empty())
            crease[e] = se < sc.size() ? sc[se] : 0.0f;
    }

    // Triangles of the same input polygon are one polygon again: dissolve the edges between them.
    std::vector<Index> inner;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e) || m.edgeFaceCount(e) != 2)
            continue;
        Index c = m.edgeCorner(e);
        Index f0 = m.cornerFace(c), f1 = m.cornerFace(m.cornerRadial(c));
        if (f0 < gl.faceID.size() && f1 < gl.faceID.size() && gl.faceID[f0] == gl.faceID[f1])
            inner.push_back(e);
    }
    geo::dissolveEdges(m, inner, false);
    m.compact();
    return m;
}

bool isNew(std::span<const int32_t> origin, Index p) { return p >= origin.size() || origin[p] == 0; }

/** Marks the edges where faces of different operands meet. Returns them. */
std::vector<Index> tagSeam(geo::Mesh& m)
{
    auto operand = m.faceAttrs().get<int32_t>(kOperandAttr);
    auto seam = m.edgeAttrs().getOrAdd<bool>(kBooleanSeamAttr);
    std::vector<Index> edges;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e))
            continue;
        seam[e] = false;
        if (m.edgeFaceCount(e) != 2)
            continue;
        Index c = m.edgeCorner(e);
        Index f0 = m.cornerFace(c), f1 = m.cornerFace(m.cornerRadial(c));
        if (operand[f0] != operand[f1])
        {
            seam[e] = true;
            edges.push_back(e);
        }
    }
    return edges;
}

/**
 * Welds new points into a neighbour closer than tol (onto an operand point when there is one, so the original shape
 * wins), then drops new points that only carry a straight line on. Kills the slivers a cut leaves near existing points.
 */
void weldSeam(geo::Mesh& m, double tol)
{
    LILA_ZONE();

    std::vector<Index> order;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.edgeLength(e) < tol)
            order.push_back(e);
    std::sort(order.begin(), order.end(), [&](Index a, Index b) { return m.edgeLength(a) < m.edgeLength(b); });

    auto origin = m.pointAttrs().get<int32_t>(kOriginAttr);
    std::vector<Index> target(m.pointCapacity(), kInvalid);
    std::vector<uint8_t> keep(m.pointCapacity(), 0);
    for (Index e : order)
    {
        Index a = m.edgePoint(e, 0), b = m.edgePoint(e, 1);
        bool na = isNew(origin, a), nb = isNew(origin, b);
        if (!na && !nb)
            continue;
        if (na && !nb)
            std::swap(a, b); // b is the new one, a survives
        else if (na && nb && a > b)
            std::swap(a, b);
        if (target[a] != kInvalid || target[b] != kInvalid || keep[b])
            continue;
        target[b] = a;
        keep[a] = 1;
    }
    geo::weldPoints(m, target);

    for (int pass = 0; pass < 8; ++pass)
    {
        origin = m.pointAttrs().get<int32_t>(kOriginAttr);
        std::fill(target.begin(), target.end(), kInvalid);
        target.resize(m.pointCapacity(), kInvalid);
        std::vector<uint8_t> locked(m.pointCapacity(), 0);
        bool any = false;
        for (Index p = 0; p < m.pointCapacity(); ++p)
        {
            if (!m.pointValid(p) || !isNew(origin, p) || locked[p] || m.pointValence(p) != 2)
                continue;
            Index ends[2], n = 0;
            m.forEachPointEdge(p, [&](Index e) {
                if (n < 2)
                    ends[n++] = m.edgeOtherPoint(e, p);
            });
            if (n != 2 || locked[ends[0]] || locked[ends[1]])
                continue;
            glm::dvec3 u = m.position(ends[0]) - m.position(p), v = m.position(ends[1]) - m.position(p);
            double lu = glm::length(u), lv = glm::length(v);
            if (lu < 1e-300 || lv < 1e-300 || glm::dot(u, v) / (lu * lv) > -0.99996) // within about half a degree of straight
                continue;
            target[p] = ends[0];
            locked[p] = locked[ends[0]] = locked[ends[1]] = 1;
            any = true;
        }
        if (!any)
            break;
        geo::weldPoints(m, target);
    }
    m.compact();
}

double medianLength(const geo::Mesh& m, std::span<const Index> edges)
{
    std::vector<double> lengths;
    for (Index e : edges)
        lengths.push_back(m.edgeLength(e));
    if (lengths.empty())
        return 0.0;
    std::nth_element(lengths.begin(), lengths.begin() + lengths.size() / 2, lengths.end());
    return lengths[lengths.size() / 2];
}

/** Faces sharing a point with the given ones, grown rings times. */
std::vector<Index> growFaces(const geo::Mesh& m, std::vector<Index> faces, int rings)
{
    std::vector<uint8_t> in(m.faceCapacity(), 0);
    for (Index f : faces)
        in[f] = 1;
    for (int ring = 0; ring < rings; ++ring)
    {
        std::vector<Index> next = faces;
        for (Index f : faces)
            for (Index p : m.facePoints(f))
                for (Index g : m.pointFaces(p))
                    if (!in[g])
                    {
                        in[g] = 1;
                        next.push_back(g);
                    }
        faces = std::move(next);
    }
    return faces;
}

/**
 * Remeshes the band of faces into quads that follow the seam, keeping the band's border points so the patch welds back
 * into the faces around it. False, leaving m as it was, when the remesh can't keep the border exactly (border edges
 * much longer or shorter than the band's own spacing).
 */
bool requadBand(geo::Mesh& m, std::span<const Index> band, double hardAngle, double timeout)
{
    LILA_ZONE();

    std::vector<std::string> uvSets;
    for (int k = 0; m.cornerAttrs().has(geo::attr::uvName(k)); ++k)
        uvSets.push_back(geo::attr::uvName(k));
    geo::Mesh patch;
    std::vector<Index> toPatch(m.pointCapacity(), kInvalid);
    for (Index f : band)
    {
        std::vector<Index> pts = m.facePoints(f);
        for (Index& p : pts)
        {
            if (toPatch[p] == kInvalid)
                toPatch[p] = patch.addPoint(m.position(p));
            p = toPatch[p];
        }
        Index pf = patch.addFace(pts);
        if (pf == kInvalid)
            continue;
        for (const std::string& set : uvSets)
        {
            auto from = m.cornerAttrs().get<glm::vec2>(set);
            auto to = patch.cornerAttrs().getOrAdd<glm::vec2>(set);
            std::vector<Index> src = m.faceCorners(f), dst = patch.faceCorners(pf);
            for (size_t k = 0; k < src.size() && k < dst.size(); ++k)
                to[dst[k]] = src[k] < from.size() ? from[src[k]] : glm::vec2(0.0f);
        }
    }
    std::vector<glm::dvec3> pins;
    std::vector<Index> borderEdges;
    for (Index e = 0; e < patch.edgeCapacity(); ++e)
        if (patch.edgeValid(e) && patch.isBoundaryEdge(e))
            borderEdges.push_back(e);
    for (Index p = 0; p < patch.pointCapacity(); ++p)
        if (patch.pointValid(p) && patch.isBoundaryPoint(p))
            pins.push_back(patch.position(p));
    if (pins.empty() || borderEdges.empty())
        return false;

    // The band keeps its own density: finer where the seam's edges are shorter than the border's.
    retopo::QuadRemeshOptions o;
    o.targetEdgeLength = medianLength(patch, borderEdges);
    o.adaptivity = 0.0;
    {
        auto density = patch.pointAttrs().getOrAdd<float>("density");
        for (Index p = 0; p < patch.pointCapacity(); ++p)
        {
            if (!patch.pointValid(p))
                continue;
            double sum = 0.0;
            int n = 0;
            patch.forEachPointEdge(p, [&](Index e) {
                sum += patch.edgeLength(e);
                ++n;
            });
            double local = n ? sum / n : o.targetEdgeLength;
            double ratio = o.targetEdgeLength / std::max(local, 1e-9 * o.targetEdgeLength);
            density[p] = float(std::clamp(ratio * ratio, 1.0, 8.0));
        }
    }
    o.pureQuads = false;
    o.sharpAngle = hardAngle;
    o.pinnedPoints = pins;
    o.timeout = timeout;
    retopo::QuadRemeshResult r = retopo::quadRemesh(patch, o);
    if (!r.ok())
        return false;

    // Stitch the patch's border onto the band's: pair points that are each other's nearest, split a patch border edge
    // for each band point left over, and fold extra patch border points into a paired neighbour.
    geo::Mesh& q = r.mesh;
    std::vector<Index> bandPoints;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (toPatch[p] != kInvalid && patch.isBoundaryPoint(toPatch[p]))
            bandPoints.push_back(p);
    auto patchBorder = [&]() {
        std::vector<Index> out;
        for (Index v = 0; v < q.pointCapacity(); ++v)
            if (q.pointValid(v) && q.isBoundaryPoint(v))
                out.push_back(v);
        return out;
    };
    const double reach = 0.6 * o.targetEdgeLength;
    std::vector<Index> pairOf(m.pointCapacity(), kInvalid); // band point -> patch point
    std::vector<Index> border = patchBorder();
    auto nearestBand = [&](const glm::dvec3& at) {
        Index best = kInvalid;
        double d = std::numeric_limits<double>::max();
        for (Index p : bandPoints)
            if (double l = glm::length(m.position(p) - at); l < d)
            {
                d = l;
                best = p;
            }
        return best;
    };
    for (Index p : bandPoints)
    {
        Index best = kInvalid;
        double d = reach;
        for (Index v : border)
            if (double l = glm::length(q.position(v) - m.position(p)); l < d)
            {
                d = l;
                best = v;
            }
        if (best != kInvalid && nearestBand(q.position(best)) == p)
            pairOf[p] = best;
    }
    for (Index p : bandPoints)
    {
        if (pairOf[p] != kInvalid)
            continue;
        Index bestEdge = kInvalid;
        double bestD = std::numeric_limits<double>::max(), bestT = 0.5;
        for (Index e = 0; e < q.edgeCapacity(); ++e)
        {
            if (!q.edgeValid(e) || !q.isBoundaryEdge(e))
                continue;
            glm::dvec3 a0 = q.position(q.edgePoint(e, 0)), a1 = q.position(q.edgePoint(e, 1));
            double t = std::clamp(glm::dot(m.position(p) - a0, a1 - a0) / std::max(glm::dot(a1 - a0, a1 - a0), 1e-300), 0.0, 1.0);
            double d = glm::length(a0 + t * (a1 - a0) - m.position(p));
            if (d < bestD)
            {
                bestD = d;
                bestEdge = e;
                bestT = t;
            }
        }
        if (bestEdge == kInvalid || bestD > o.targetEdgeLength)
            return false;
        pairOf[p] = q.splitEdge(bestEdge, std::clamp(bestT, 0.05, 0.95));
    }
    std::vector<Index> qTarget(q.pointCapacity(), kInvalid);
    std::vector<uint8_t> paired(q.pointCapacity(), 0);
    for (Index p : bandPoints)
    {
        q.setPosition(pairOf[p], m.position(p));
        paired[pairOf[p]] = 1;
    }
    for (Index v : patchBorder())
        if (!paired[v])
        {
            Index into = kInvalid;
            double d = std::numeric_limits<double>::max();
            q.forEachPointEdge(v, [&](Index e) {
                Index w = q.edgeOtherPoint(e, v);
                if (q.isBoundaryEdge(e) && paired[w] && q.edgeLength(e) < d)
                {
                    d = q.edgeLength(e);
                    into = w;
                }
            });
            if (into == kInvalid)
                return false;
            qTarget[v] = into;
        }
    geo::weldPoints(q, qTarget);
    q.compact();
    // A border face that took two of the band's points has six sides or more.
    std::vector<Index> large;
    for (Index f = 0; f < q.faceCapacity(); ++f)
        if (q.faceValid(f) && q.faceSize(f) > 5)
            large.push_back(f);
    geo::splitNgons(q, large);

    // Uvs from the band under each corner, probed a little toward its face's middle so a corner on a uv seam reads its
    // own side.
    if (!uvSets.empty())
    {
        const geo::MeshBvh bvh(patch);
        for (const std::string& set : uvSets)
        {
            auto from = patch.cornerAttrs().get<glm::vec2>(set);
            auto to = q.cornerAttrs().getOrAdd<glm::vec2>(set);
            for (Index f = 0; f < q.faceCapacity(); ++f)
            {
                if (!q.faceValid(f))
                    continue;
                const glm::dvec3 middle = q.faceCentroid(f);
                q.forEachFaceCorner(f, [&](Index c) {
                    glm::dvec3 p = q.position(q.cornerPoint(c));
                    geo::BvhClosest hit;
                    if (bvh.closestPoint(p + 0.05 * (middle - p), hit))
                        to[c] = bvh.interpolate<glm::vec2>(from, hit.triangle, hit.bary);
                });
            }
        }
    }

    // The paired patch points now sit exactly on their band points; weld them by position.
    geo::deleteFaces(m, band);
    const Index offset = m.append(q);
    std::vector<Index> target(m.pointCapacity(), kInvalid);
    for (Index v = offset; v < m.pointCapacity(); ++v)
    {
        if (!m.pointValid(v) || !m.isBoundaryPoint(v))
            continue;
        for (Index p : bandPoints)
            if (m.pointValid(p) && m.position(p) == m.position(v))
            {
                target[v] = p;
                break;
            }
    }
    geo::weldPoints(m, target);
    return true;
}
} // namespace

std::optional<BooleanOp> booleanOpFromName(std::string_view name)
{
    if (name == "union")
        return BooleanOp::Union;
    if (name == "difference")
        return BooleanOp::Difference;
    if (name == "intersection" || name == "intersect")
        return BooleanOp::Intersection;
    return std::nullopt;
}

BooleanResult meshBoolean(std::span<const BooleanOperand> operands, const glm::dmat4& toLocal, const BooleanOptions& opts)
{
    LILA_ZONE();

    BooleanResult r;
    if (operands.empty() || !operands[0].mesh)
    {
        r.error = "no base mesh";
        return r;
    }
    const std::vector<std::string> colorSets = geo::colors::sets(*operands[0].mesh);
    std::vector<uint32_t> firstFace;
    uint32_t offset = 0;
    std::optional<manifold::Manifold> result;
    for (size_t i = 0; i < operands.size(); ++i)
    {
        firstFace.push_back(offset);
        manifold::Manifold solid(toManifold(*operands[i].mesh, operands[i].toWorld, offset, colorSets));
        offset += uint32_t(operands[i].mesh->faceCapacity());
        if (solid.Status() != manifold::Manifold::Error::NoError)
        {
            r.error = "isn't a closed solid (" + std::to_string(int(solid.Status())) + ")";
            r.badOperand = int(i);
            return r;
        }
        if (!result)
            result = std::move(solid);
        else
            result = opts.op == BooleanOp::Union ? *result + solid : opts.op == BooleanOp::Difference ? *result - solid : *result ^ solid;
    }

    geo::Mesh m = fromManifold(result->GetMeshGL(), toLocal, operands, firstFace);
    std::vector<Index> seam = tagSeam(m);
    if (opts.cleanup && !seam.empty())
    {
        double tol = opts.snapDistance >= 0.0 ? opts.snapDistance : 0.1 * medianLength(m, seam);
        if (tol > 0.0)
        {
            weldSeam(m, tol);
            seam = tagSeam(m);
        }
    }
    r.seamEdges = Index(seam.size());

    // Points on the seam can be operand points too, where the cut runs exactly through them.
    std::vector<uint8_t> onSeam(m.pointCapacity(), 0);
    for (Index e : seam)
        onSeam[m.edgePoint(e, 0)] = onSeam[m.edgePoint(e, 1)] = 1;

    if (opts.filletWidth > 0.0 && !seam.empty())
    {
        const std::vector<Index> strip =
            geo::bevelEdges(m, seam, {.offset = opts.filletWidth, .segments = std::max(opts.filletSegments, 1), .smoothingAngle = opts.hardAngle});
        if (opts.markFillet)
        {
            auto fillet = m.faceAttrs().getOrAdd<int32_t>(kFilletFaceAttr);
            for (Index f : strip)
                fillet[f] = 1;
        }
    }

    // Everything the cut made: faces and edges touching the seam or a point no operand had.
    auto origin = m.pointAttrs().get<int32_t>(kOriginAttr);
    auto touchedPoint = [&](Index p) { return isNew(origin, p) || (p < onSeam.size() && onSeam[p]); };
    std::vector<Index> touchedFaces, touchedEdges;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        bool touched = false;
        m.forEachFaceCorner(f, [&](Index c) { touched |= touchedPoint(m.cornerPoint(c)); });
        if (touched)
            touchedFaces.push_back(f);
    }
    bool requadded = false;
    if (opts.quadBand && !touchedFaces.empty())
        requadded = requadBand(m, growFaces(m, touchedFaces, std::max(opts.bandRings, 0)), opts.hardAngle, opts.bandTimeout);
    if (requadded)
        r.quadBand = true;
    else if (opts.splitNgons)
        geo::splitNgons(m, touchedFaces);
    origin = m.pointAttrs().get<int32_t>(kOriginAttr);
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && (touchedPoint(m.edgePoint(e, 0)) || touchedPoint(m.edgePoint(e, 1))))
            touchedEdges.push_back(e);
    geo::setHardEdgesByAngle(m, opts.hardAngle, touchedEdges);

    m.pointAttrs().remove(kOriginAttr);
    m.faceAttrs().remove(kOperandAttr);
    m.compact();
    r.mesh = std::move(m);
    return r;
}
} // namespace lila::scene
