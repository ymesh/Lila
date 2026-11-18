#include "tex/PaintSurface.h"

#include "core/Profile.h"
#include "geo/Attribute.h"
#include "geo/Normals.h"
#include "tex/TextureSet.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <stdexcept>

#include <tbb/parallel_for.h>

namespace lila::tex::paint
{
namespace
{
/** Closest point of triangle abc to p in 2D, as barycentric weights. */
glm::dvec3 closestBary(const glm::dvec2& p, const glm::dvec2& a, const glm::dvec2& b, const glm::dvec2& c)
{
    // Ericson, Real-Time Collision Detection 5.1.5, in 2D.
    const glm::dvec2 ab = b - a, ac = c - a, ap = p - a;
    const double d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0)
        return {1, 0, 0};
    const glm::dvec2 bp = p - b;
    const double d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3)
        return {0, 1, 0};
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
    {
        const double v = d1 / (d1 - d3);
        return {1 - v, v, 0};
    }
    const glm::dvec2 cp = p - c;
    const double d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6)
        return {0, 0, 1};
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
    {
        const double w = d2 / (d2 - d6);
        return {1 - w, 0, w};
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
    {
        const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return {0, 1 - w, w};
    }
    const double denom = 1.0 / (va + vb + vc);
    const double v = vb * denom, w = vc * denom;
    return {1 - v - w, v, w};
}
/** The part of a 2D polygon inside the box, Sutherland-Hodgman. */
std::vector<glm::dvec2> clipToBox(std::vector<glm::dvec2> poly, const glm::dvec2& lo, const glm::dvec2& hi)
{
    for (int side = 0; side < 4 && !poly.empty(); ++side)
    {
        const int axis = side / 2;
        const double bound = side % 2 == 0 ? lo[axis] : hi[axis];
        auto inside = [&](const glm::dvec2& p) { return side % 2 == 0 ? p[axis] >= bound : p[axis] <= bound; };
        std::vector<glm::dvec2> out;
        for (size_t i = 0; i < poly.size(); ++i)
        {
            const glm::dvec2 a = poly[i], b = poly[(i + 1) % poly.size()];
            const bool ia = inside(a), ib = inside(b);
            if (ia)
                out.push_back(a);
            if (ia != ib)
                out.push_back(a + (b - a) * ((bound - a[axis]) / (b[axis] - a[axis])));
        }
        poly = std::move(out);
    }
    return poly;
}
} // namespace

glm::vec3 PaintMesh::point(uint32_t tri, float u, float v) const
{
    const glm::vec3* p = &positions[size_t(tri) * 3];
    return p[0] * (1.0f - u - v) + p[1] * u + p[2] * v;
}

glm::vec3 PaintMesh::normal(uint32_t tri, float u, float v) const
{
    const glm::vec3* n = &normals[size_t(tri) * 3];
    const glm::vec3 s = n[0] * (1.0f - u - v) + n[1] * u + n[2] * v;
    return glm::length(s) > 0.0f ? glm::normalize(s) : glm::vec3(0, 1, 0);
}

std::vector<Dab> tileWrapped(const PaintMesh& mesh, const Dab& d)
{
    std::vector<Dab> out;
    if (d.triangle >= mesh.triangleCount())
        return out;
    const glm::vec3* p = &mesh.positions[size_t(d.triangle) * 3];
    const glm::vec2* t = &mesh.uvs[size_t(d.triangle) * 3];
    const double area3 = glm::length(glm::cross(glm::dvec3(p[1] - p[0]), glm::dvec3(p[2] - p[0])));
    const double areaUv = std::abs(double((t[1].x - t[0].x) * (t[2].y - t[0].y) - (t[2].x - t[0].x) * (t[1].y - t[0].y)));
    if (area3 <= 1e-18 || areaUv <= 1e-18)
        return out;
    const float r = float(d.radius * std::sqrt(areaUv / area3));
    const glm::vec2 uv = mesh.uv(d.triangle, d.u, d.v);
    const glm::vec2 f = uv - glm::floor(uv);
    const int du = f.x - r < 0.0f ? 1 : f.x + r > 1.0f ? -1 : 0;
    const int dv = f.y - r < 0.0f ? 1 : f.y + r > 1.0f ? -1 : 0;
    for (const glm::ivec2 o : {glm::ivec2(du, 0), glm::ivec2(0, dv), glm::ivec2(du, dv)})
    {
        if (o == glm::ivec2(0) || (o.x != 0 && du == 0) || (o.y != 0 && dv == 0))
            continue;
        // The copy's centre lies just off the other edge: found on the triangle at that edge, its barycentrics run past it
        // (the triangle's plane carries on), so the dab reaches back in exactly as far as it reached out.
        const glm::vec2 target = uv + glm::vec2(o);
        const glm::vec2 inside = glm::clamp(target, glm::vec2(1e-4f), glm::vec2(1.0f - 1e-4f)) + glm::floor(uv);
        uint32_t tri = 0;
        float u = 0.0f, v = 0.0f;
        if (!mesh.atUv(inside, tri, u, v, 1e-3f) || !mesh.paintable[tri])
            continue;
        const glm::vec2* q = &mesh.uvs[size_t(tri) * 3];
        const glm::vec2 e1 = q[1] - q[0], e2 = q[2] - q[0], rel = target + glm::floor(uv) - q[0];
        const float det = e1.x * e2.y - e2.x * e1.y;
        if (std::abs(det) < 1e-12f)
            continue;
        Dab w = d;
        w.triangle = tri;
        w.u = (rel.x * e2.y - e2.x * rel.y) / det;
        w.v = (e1.x * rel.y - rel.x * e1.y) / det;
        out.push_back(w);
    }
    return out;
}

std::shared_ptr<const PaintMesh> PaintMesh::build(const geo::Mesh& mesh, std::shared_ptr<const geo::MeshBvh> bvh, std::string_view material)
{
    LILA_ZONE();
    if (!bvh)
        throw std::invalid_argument("PaintMesh::build needs the mesh's BVH");
    auto out = std::make_shared<PaintMesh>();
    const geo::Triangulation& tris = bvh->triangles();
    const size_t n = tris.faces.size();
    out->positions.resize(n * 3);
    out->normals.resize(n * 3);
    out->tangents.assign(n * 3, glm::vec4(0.0f));
    out->uvs.assign(n * 3, glm::vec2(0.0f));
    out->paintable.assign(n, 1);
    out->faces.resize(n);
    out->materials.assign(n, -1);
    out->points.resize(n * 3);
    out->bvh = std::move(bvh);

    const std::vector<glm::vec3> pn = geo::pointNormals(mesh);
    std::span<const glm::vec2> uv = mesh.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    std::vector<glm::vec4> tangents;
    if (!uv.empty())
    {
        const std::vector<glm::vec3> cn = geo::cornerNormals(mesh);
        tangents = geo::cornerTangents(mesh, tris, cn);
    }
    std::span<const int32_t> slot = mesh.faceAttrs().get<int32_t>(geo::attr::kMaterial);
    std::vector<char> want(mesh.materialNames().size(), 0);
    bool wantAny = false;
    if (!material.empty() && !slot.empty())
        for (size_t start = 0; start <= material.size();)
        {
            size_t end = material.find(kMaterialSeparator, start);
            if (end == std::string_view::npos)
                end = material.size();
            const std::string_view name = material.substr(start, end - start);
            for (size_t i = 0; i < mesh.materialNames().size(); ++i)
                if (mesh.materialNames()[i] == name)
                    wantAny = want[i] = 1;
            start = end + 1;
        }
    auto wanted = [&](int32_t s) { return s >= 0 && size_t(s) < want.size() && want[size_t(s)]; };
    bool anyUses = false;
    if (wantAny)
        for (size_t t = 0; t < n && !anyUses; ++t)
            anyUses = wanted(slot[tris.faces[t]]);

    for (size_t t = 0; t < n; ++t)
    {
        if (anyUses)
            out->paintable[t] = wanted(slot[tris.faces[t]]) ? 1 : 0;
        out->faces[t] = uint32_t(tris.faces[t]);
        if (!slot.empty())
            out->materials[t] = slot[tris.faces[t]];
        for (size_t k = 0; k < 3; ++k)
        {
            const geo::Index c = tris.corners[t * 3 + k];
            const geo::Index p = mesh.cornerPoint(c);
            out->points[t * 3 + k] = uint32_t(p);
            out->positions[t * 3 + k] = glm::vec3(mesh.position(p));
            out->normals[t * 3 + k] = p < pn.size() ? pn[p] : glm::vec3(0, 1, 0);
            if (!uv.empty())
                out->uvs[t * 3 + k] = uv[c];
            if (c < tangents.size())
                out->tangents[t * 3 + k] = tangents[c];
        }
    }
    uint64_t h = 1469598103934665603ull ^ n;
    auto mix = [&](const void* data, size_t bytes) {
        const auto* b = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i)
            h = (h ^ b[i]) * 1099511628211ull;
    };
    mix(out->points.data(), out->points.size() * sizeof(uint32_t));
    mix(out->uvs.data(), out->uvs.size() * sizeof(glm::vec2));
    mix(out->paintable.data(), out->paintable.size());
    out->signature = h;
    return out;
}

glm::vec2 PaintMesh::uv(uint32_t tri, float u, float v) const
{
    const glm::vec2* t = &uvs[size_t(tri) * 3];
    return t[0] * (1.0f - u - v) + t[1] * u + t[2] * v;
}

std::vector<uint32_t> PaintMesh::islands() const
{
    const size_t n = triangleCount();
    std::vector<uint32_t> parent(n);
    for (size_t i = 0; i < n; ++i)
        parent[i] = uint32_t(i);
    auto find = [&](uint32_t x) {
        while (parent[x] != x)
            x = parent[x] = parent[parent[x]];
        return x;
    };
    // An edge is its two corners' exact position and uv bits, in a fixed order.
    using Corner = std::array<uint32_t, 5>;
    auto corner = [&](size_t c) {
        Corner k;
        std::memcpy(&k[0], &positions[c], 12);
        std::memcpy(&k[3], &uvs[c], 8);
        return k;
    };
    std::map<std::pair<Corner, Corner>, uint32_t> edges;
    for (size_t t = 0; t < n; ++t)
        for (size_t k = 0; k < 3; ++k)
        {
            Corner a = corner(t * 3 + k), b = corner(t * 3 + (k + 1) % 3);
            if (b < a)
                std::swap(a, b);
            auto [it, fresh] = edges.try_emplace({a, b}, uint32_t(t));
            if (!fresh)
            {
                const uint32_t ra = find(it->second), rb = find(uint32_t(t));
                if (ra != rb)
                    parent[std::max(ra, rb)] = std::min(ra, rb);
            }
        }
    std::vector<uint32_t> out(n);
    std::map<uint32_t, uint32_t> ids;
    for (size_t t = 0; t < n; ++t)
        out[t] = ids.try_emplace(find(uint32_t(t)), uint32_t(ids.size())).first->second;
    return out;
}

std::vector<uint32_t> PaintMesh::allTriangles() const
{
    std::vector<uint32_t> out;
    for (size_t t = 0; t < triangleCount(); ++t)
        if (paintable[t])
            out.push_back(uint32_t(t));
    return out;
}

std::vector<uint32_t> PaintMesh::islandOf(uint32_t tri) const
{
    std::vector<uint32_t> out;
    if (tri >= triangleCount())
        return out;
    const std::vector<uint32_t> id = islands();
    for (size_t t = 0; t < triangleCount(); ++t)
        if (paintable[t] && id[t] == id[tri])
            out.push_back(uint32_t(t));
    return out;
}

std::vector<uint32_t> PaintMesh::materialOf(uint32_t tri) const
{
    std::vector<uint32_t> out;
    if (tri >= triangleCount())
        return out;
    for (size_t t = 0; t < triangleCount(); ++t)
        if (paintable[t] && materials[t] == materials[tri])
            out.push_back(uint32_t(t));
    return out;
}

std::vector<uint32_t> PaintMesh::ofFaces(std::span<const uint32_t> wanted) const
{
    std::vector<uint32_t> sorted(wanted.begin(), wanted.end());
    std::sort(sorted.begin(), sorted.end());
    std::vector<uint32_t> out;
    for (size_t t = 0; t < triangleCount(); ++t)
        if (paintable[t] && std::binary_search(sorted.begin(), sorted.end(), faces[t]))
            out.push_back(uint32_t(t));
    return out;
}

bool PaintMesh::atUv(const glm::vec2& at, uint32_t& tri, float& u, float& v, float maxDistance) const
{
    double best = 1e300;
    for (size_t t = 0; t < triangleCount(); ++t)
    {
        if (!paintable[t])
            continue;
        const glm::dvec2 a(uvs[t * 3]), b(uvs[t * 3 + 1]), c(uvs[t * 3 + 2]), p(at);
        const glm::dvec3 w = closestBary(p, a, b, c);
        const double d = glm::length(p - (a * w.x + b * w.y + c * w.z));
        if (d < best && d <= double(maxDistance) + 1e-12)
        {
            best = d;
            tri = uint32_t(t);
            u = float(w.y);
            v = float(w.z);
            if (d == 0.0)
                return true;
        }
    }
    return best < 1e300;
}

bool recordDab(const PaintMesh& mesh, const brush::Dab& d, Dab& out)
{
    geo::BvhClosest hit;
    if (!mesh.bvh || !mesh.bvh->closestPoint(d.center, hit, std::max(d.radius, 1e-9)) || hit.triangle >= mesh.triangleCount() ||
        !mesh.paintable[hit.triangle])
        return false;
    out.triangle = hit.triangle;
    out.u = float(hit.bary.y);
    out.v = float(hit.bary.z);
    out.radius = float(d.radius);
    out.pressure = float(d.pressure);
    out.opacity = float(std::clamp(d.opacity * d.flow, 0.0, 1.0));
    out.copy = d.copy;
    out.tilt = float(d.tilt);
    out.angle = 0.0f;
    if (glm::dot(d.tangent, d.tangent) > 0.5)
    {
        const glm::vec3 n = mesh.normal(out.triangle, out.u, out.v), t0 = tipFrame(n), b0 = glm::cross(n, t0);
        const glm::vec3 t(d.tangent);
        out.angle = std::atan2(glm::dot(t, b0), glm::dot(t, t0));
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------- SurfaceAtlas

SurfaceAtlas::SurfaceAtlas(std::shared_ptr<const PaintMesh> mesh, int udim, int width, int height, int padding)
    : mMesh(std::move(mesh)), mUdim(udim), mWidth(width), mHeight(height), mPadding(std::max(0, padding))
{
    LILA_ZONE();
    if (width < 1 || height < 1)
        throw std::invalid_argument("paint atlas size must be positive");
    mPagesX = (width + kPageSize - 1) / kPageSize;
    mPagesY = (height + kPageSize - 1) / kPageSize;
    const auto [ou, ov] = udimOrigin(udim);
    mOrigin = {float(ou), float(ov)};
    mPageTris.resize(size_t(mPagesX) * size_t(mPagesY));
    const size_t n = mMesh->triangleCount();
    mTriPageStart.assign(n + 1, 0);
    for (size_t t = 0; t < n; ++t)
    {
        mTriPageStart[t] = uint32_t(mTriPages.size());
        if (!mMesh->paintable[t])
            continue;
        glm::dvec2 lo(1e300), hi(-1e300);
        for (int k = 0; k < 3; ++k)
        {
            const glm::dvec2 px = texelOf(mMesh->uvs[t * 3 + size_t(k)]);
            lo = glm::min(lo, px);
            hi = glm::max(hi, px);
        }
        const double pad = double(mPadding) + 1.0;
        const int x0 = std::max(0, int(std::floor(lo.x - pad))), y0 = std::max(0, int(std::floor(lo.y - pad)));
        const int x1 = std::min(mWidth - 1, int(std::ceil(hi.x + pad))), y1 = std::min(mHeight - 1, int(std::ceil(hi.y + pad)));
        if (x0 > x1 || y0 > y1)
            continue;
        glm::dvec2 c[3];
        glm::dvec3 p[3];
        for (int k = 0; k < 3; ++k)
        {
            c[k] = texelOf(mMesh->uvs[t * 3 + size_t(k)]);
            p[k] = glm::dvec3(mMesh->positions[t * 3 + size_t(k)]);
        }
        const double area = (c[1].x - c[0].x) * (c[2].y - c[0].y) - (c[2].x - c[0].x) * (c[1].y - c[0].y);
        for (int py = y0 / kPageSize; py <= y1 / kPageSize; ++py)
            for (int px = x0 / kPageSize; px <= x1 / kPageSize; ++px)
            {
                // Every texel this triangle owns in the page (padding included) lies on the triangle within this piece.
                Box box;
                if (std::abs(area) > 1e-12)
                {
                    const glm::dvec2 blo(px * kPageSize - pad, py * kPageSize - pad), bhi((px + 1) * kPageSize + pad, (py + 1) * kPageSize + pad);
                    const std::vector<glm::dvec2> piece = clipToBox({c[0], c[1], c[2]}, blo, bhi);
                    if (piece.empty())
                        continue;
                    for (const glm::dvec2& q : piece)
                    {
                        const double w1 = ((q.x - c[0].x) * (c[2].y - c[0].y) - (c[2].x - c[0].x) * (q.y - c[0].y)) / area;
                        const double w2 = ((c[1].x - c[0].x) * (q.y - c[0].y) - (q.x - c[0].x) * (c[1].y - c[0].y)) / area;
                        box.extend(glm::vec3(p[0] * (1.0 - w1 - w2) + p[1] * w1 + p[2] * w2));
                    }
                }
                else
                    for (const glm::dvec3& q : p)
                        box.extend(glm::vec3(q));
                const uint32_t page = uint32_t(py * mPagesX + px);
                mTriPages.push_back(page);
                mTriPageBoxes.push_back(box);
                mPageTris[page].push_back(uint32_t(t));
            }
    }
    mTriPageStart[n] = uint32_t(mTriPages.size());
}

glm::vec2 SurfaceAtlas::uvOf(double x, double y) const
{
    return {mOrigin.x + float((x + 0.5) / mWidth), mOrigin.y + 1.0f - float((y + 0.5) / mHeight)};
}

glm::dvec2 SurfaceAtlas::texelOf(const glm::vec2& uv) const
{
    return {(double(uv.x) - mOrigin.x) * mWidth, (mOrigin.y + 1.0 - double(uv.y)) * mHeight};
}

const std::vector<GeoTexel>& SurfaceAtlas::page(uint32_t index)
{
    if (auto it = mPages.find(index); it != mPages.end())
        return it->second;
    return mPages.emplace(index, rasterise(index)).first->second;
}

void SurfaceAtlas::preparePages(std::span<const uint32_t> pages)
{
    std::vector<uint32_t> missing;
    for (uint32_t p : pages)
        if (!mPages.contains(p))
            missing.push_back(p);
    if (missing.size() < 2)
        return;
    LILA_ZONE();
    std::vector<std::vector<GeoTexel>> built(missing.size());
    tbb::parallel_for(size_t(0), missing.size(), [&](size_t i) { built[i] = rasterise(missing[i]); });
    for (size_t i = 0; i < missing.size(); ++i)
        mPages.emplace(missing[i], std::move(built[i]));
}

std::vector<GeoTexel> SurfaceAtlas::rasterise(uint32_t index) const
{
    LILA_ZONE();
    std::vector<GeoTexel> texels(size_t(kPageSize) * kPageSize);
    std::vector<double> best(texels.size(), 1e300);
    const int bx = int(index % uint32_t(mPagesX)) * kPageSize, by = int(index / uint32_t(mPagesX)) * kPageSize;
    const double pad = double(mPadding);
    if (index < mPageTris.size())
        for (uint32_t t : mPageTris[index])
        {
            glm::dvec2 c[3];
            for (int k = 0; k < 3; ++k)
                c[k] = texelOf(mMesh->uvs[size_t(t) * 3 + size_t(k)]);
            const double area = (c[1].x - c[0].x) * (c[2].y - c[0].y) - (c[2].x - c[0].x) * (c[1].y - c[0].y);
            const glm::dvec2 lo = glm::min(c[0], glm::min(c[1], c[2])) - pad, hi = glm::max(c[0], glm::max(c[1], c[2])) + pad;
            const int x0 = std::max(bx, int(std::floor(lo.x))), y0 = std::max(by, int(std::floor(lo.y)));
            const int x1 = std::min({bx + kPageSize - 1, mWidth - 1, int(std::ceil(hi.x))});
            const int y1 = std::min({by + kPageSize - 1, mHeight - 1, int(std::ceil(hi.y))});
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    const glm::dvec2 p(x + 0.5, y + 0.5);
                    const size_t i = size_t(y - by) * kPageSize + size_t(x - bx);
                    glm::dvec3 w(1.0, 0.0, 0.0);
                    double dist = 0.0;
                    bool inside = false;
                    if (std::abs(area) > 1e-12)
                    {
                        const double w1 = ((p.x - c[0].x) * (c[2].y - c[0].y) - (c[2].x - c[0].x) * (p.y - c[0].y)) / area;
                        const double w2 = ((c[1].x - c[0].x) * (p.y - c[0].y) - (p.x - c[0].x) * (c[1].y - c[0].y)) / area;
                        w = {1.0 - w1 - w2, w1, w2};
                        inside = w.x >= 0.0 && w1 >= 0.0 && w2 >= 0.0;
                    }
                    if (!inside)
                    {
                        w = closestBary(p, c[0], c[1], c[2]);
                        dist = glm::length(p - (c[0] * w.x + c[1] * w.y + c[2] * w.z));
                        if (dist > pad)
                            continue;
                    }
                    // Triangles come in ascending order, so ties keep the lowest id.
                    if (dist < best[i])
                    {
                        best[i] = dist;
                        texels[i] = {t, float(w.y), float(w.z), inside ? 0u : kTexelPadding};
                    }
                }
        }
    return texels;
}

std::vector<uint32_t> SurfaceAtlas::pagesWhere(const std::function<bool(uint32_t tri)>& keep) const
{
    std::vector<uint32_t> out;
    for (size_t p = 0; p < mPageTris.size(); ++p)
        for (uint32_t t : mPageTris[p])
            if (keep(t))
            {
                out.push_back(uint32_t(p));
                break;
            }
    return out;
}

std::map<uint32_t, std::vector<uint32_t>> SurfaceAtlas::pagesFor(std::span<const DabReach> dabs, uint32_t first, std::span<const uint8_t> skip) const
{
    LILA_ZONE();
    std::map<uint32_t, std::vector<uint32_t>> out;
    const geo::MeshBvh& bvh = *mMesh->bvh;
    std::vector<uint32_t> seen;
    for (size_t i = 0; i < dabs.size(); ++i)
    {
        geo::Bounds box;
        box.min = glm::dvec3(dabs[i].center - dabs[i].radius);
        box.max = glm::dvec3(dabs[i].center + dabs[i].radius);
        seen.clear();
        const glm::vec3 c = dabs[i].center;
        const float r2 = dabs[i].radius * dabs[i].radius;
        for (geo::Index t : bvh.overlapping(box))
        {
            if (t >= mMesh->triangleCount() || !mMesh->paintable[t] || (t < skip.size() && skip[t]))
                continue;
            for (uint32_t k = mTriPageStart[t]; k < mTriPageStart[size_t(t) + 1]; ++k)
            {
                const Box& b = mTriPageBoxes[k];
                const glm::vec3 d = c - glm::clamp(c, b.lo, b.hi);
                if (glm::dot(d, d) <= r2)
                    seen.push_back(mTriPages[k]);
            }
        }
        std::sort(seen.begin(), seen.end());
        seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
        for (uint32_t p : seen)
            out[p].push_back(first + uint32_t(i));
    }
    return out;
}
} // namespace lila::tex::paint
