#include "gn/Volume.h"

#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/SurfaceNets.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace lila::gn
{
namespace volume_detail
{
constexpr size_t kMaxVoxels = size_t(64) << 20;
} // namespace volume_detail

Volume Volume::grid(const geo::Bounds& box, double voxelSize, Kind kind, float background)
{
    if (!(voxelSize > 0.0))
        throw std::runtime_error("voxel size must be above zero");
    Volume v;
    v.kind = kind;
    v.name = kind == Kind::Sdf ? "sdf" : "density";
    v.background = background;
    glm::dvec3 size = box.valid() ? box.size() : glm::dvec3(0.0);
    glm::dvec3 origin = box.valid() ? box.min : glm::dvec3(0.0);
    for (int a = 0; a < 3; ++a)
        v.dims[a] = int(std::ceil(size[a] / voxelSize - 1e-9)) + 1;
    if (double(v.dims.x) * v.dims.y * v.dims.z > double(volume_detail::kMaxVoxels))
        throw std::runtime_error("the volume would have over 64 million voxels; raise the voxel size");
    v.indexToWorld = glm::dmat4(voxelSize);
    v.indexToWorld[3] = glm::dvec4(origin, 1.0);
    v.values.assign(size_t(v.dims.x) * v.dims.y * v.dims.z, background);
    return v;
}

glm::dvec3 Volume::voxelOf(const glm::dvec3& world) const { return glm::dvec3(glm::inverse(indexToWorld) * glm::dvec4(world, 1.0)); }

double Volume::sample(const glm::dvec3& world) const
{
    glm::dvec3 p = voxelOf(world);
    glm::dvec3 f = glm::floor(p);
    glm::dvec3 t = p - f;
    int i = int(f.x), j = int(f.y), k = int(f.z);
    double c00 = glm::mix(double(at(i, j, k)), double(at(i + 1, j, k)), t.x);
    double c10 = glm::mix(double(at(i, j + 1, k)), double(at(i + 1, j + 1, k)), t.x);
    double c01 = glm::mix(double(at(i, j, k + 1)), double(at(i + 1, j, k + 1)), t.x);
    double c11 = glm::mix(double(at(i, j + 1, k + 1)), double(at(i + 1, j + 1, k + 1)), t.x);
    return glm::mix(glm::mix(c00, c10, t.y), glm::mix(c01, c11, t.y), t.z);
}

double Volume::sampleNearest(const glm::dvec3& world) const
{
    glm::dvec3 p = glm::round(voxelOf(world));
    return at(int(p.x), int(p.y), int(p.z));
}

void Volume::setVectors(std::vector<glm::vec3> v)
{
    kind = Kind::Vector;
    background = 0.0f;
    vectors = std::move(v);
    values.resize(vectors.size());
    for (size_t n = 0; n < vectors.size(); ++n)
        values[n] = glm::length(vectors[n]);
}

glm::dvec3 Volume::sampleVector(const glm::dvec3& world) const
{
    if (vectors.empty())
        return glm::dvec3(0.0);
    glm::dvec3 p = voxelOf(world);
    glm::dvec3 f = glm::floor(p);
    glm::dvec3 t = p - f;
    int i = int(f.x), j = int(f.y), k = int(f.z);
    auto at3 = [&](int a, int b, int c) { return glm::dvec3(vectorAt(a, b, c)); };
    glm::dvec3 c00 = glm::mix(at3(i, j, k), at3(i + 1, j, k), t.x);
    glm::dvec3 c10 = glm::mix(at3(i, j + 1, k), at3(i + 1, j + 1, k), t.x);
    glm::dvec3 c01 = glm::mix(at3(i, j, k + 1), at3(i + 1, j, k + 1), t.x);
    glm::dvec3 c11 = glm::mix(at3(i, j + 1, k + 1), at3(i + 1, j + 1, k + 1), t.x);
    return glm::mix(glm::mix(c00, c10, t.y), glm::mix(c01, c11, t.y), t.z);
}

glm::dvec3 Volume::sampleVectorNearest(const glm::dvec3& world) const
{
    glm::dvec3 p = glm::round(voxelOf(world));
    return glm::dvec3(vectorAt(int(p.x), int(p.y), int(p.z)));
}

double Volume::voxelSize() const { return std::cbrt(std::abs(glm::determinant(glm::dmat3(indexToWorld)))); }

geo::Bounds Volume::bounds() const
{
    geo::Bounds b;
    if (empty())
        return b;
    for (int n = 0; n < 8; ++n)
        b.extend(worldOf({(n & 1) ? dims.x - 1 : 0, (n & 2) ? dims.y - 1 : 0, (n & 4) ? dims.z - 1 : 0}));
    return b;
}

std::vector<float> Volume::resampled(const Volume& target) const
{
    std::vector<float> out(target.size(), background);
    tbb::parallel_for(0, target.dims.z, [&](int k) {
        for (int j = 0; j < target.dims.y; ++j)
            for (int i = 0; i < target.dims.x; ++i)
                out[target.index(i, j, k)] = float(sample(target.worldOf({i, j, k})));
    });
    return out;
}

std::vector<glm::vec3> Volume::resampledVectors(const Volume& target) const
{
    std::vector<glm::vec3> out(target.size(), glm::vec3(0.0f));
    tbb::parallel_for(0, target.dims.z, [&](int k) {
        for (int j = 0; j < target.dims.y; ++j)
            for (int i = 0; i < target.dims.x; ++i)
                out[target.index(i, j, k)] = glm::vec3(sampleVector(target.worldOf({i, j, k})));
    });
    return out;
}

GeometryPtr volumeGeometry(Volume v)
{
    Geometry g;
    g.volume = std::make_shared<const Volume>(std::move(v));
    return makeGeometry(std::move(g));
}

Volume meshToSdf(const geo::Mesh& m, double voxelSize, int band)
{
    LILA_ZONE();

    band = std::max(band, 1);
    const double bandDist = band * voxelSize;
    geo::Bounds b = m.bounds();
    if (!b.valid())
        return Volume::grid({}, voxelSize, Volume::Kind::Sdf, float(bandDist));
    glm::dvec3 pad(bandDist + voxelSize);
    b.min -= pad;
    b.max += pad;
    Volume v = Volume::grid(b, voxelSize, Volume::Kind::Sdf, float(bandDist));
    geo::MeshBvh bvh(m);
    if (bvh.empty())
        return v;

    const int nx = v.dims.x;
    tbb::parallel_for(0, v.dims.y * v.dims.z, [&](int row) {
        int j = row % v.dims.y, k = row / v.dims.y;
        // One ray along the row; a voxel is inside when an odd number of crossings lie before it. The row is nudged off the
        // grid lines so it doesn't run exactly along the shared edges of axis-aligned faces.
        glm::dvec3 origin = v.worldOf({-1.0, j + 1.37e-5, k + 2.71e-5});
        glm::dvec3 step = v.worldOf({0.0, j + 1.37e-5, k + 2.71e-5}) - origin;
        double stepLen = glm::length(step);
        glm::dvec3 dir = step / stepLen;
        double length = (nx + 1) * stepLen, eps = 1e-6 * stepLen, travelled = 0.0;
        std::vector<double> crossings;
        for (int guard = 0; guard < 4096 && travelled < length; ++guard)
        {
            geo::BvhHit hit;
            if (!bvh.raycast(origin + dir * travelled, dir, hit, length - travelled, true))
                break;
            crossings.push_back((travelled + hit.t) / stepLen - 1.0);
            travelled += hit.t + eps;
        }
        size_t passed = 0;
        for (int i = 0; i < nx; ++i)
        {
            while (passed < crossings.size() && crossings[passed] < double(i))
                ++passed;
            geo::BvhClosest c;
            double d = bvh.closestPoint(v.worldOf({i, j, k}), c, bandDist) ? std::min(c.distance, bandDist) : bandDist;
            v.values[v.index(i, j, k)] = float((passed & 1) ? -d : d);
        }
    });
    return v;
}

Volume pointsToSdf(std::span<const glm::dvec3> points, std::span<const double> radii, double voxelSize, int band)
{
    LILA_ZONE();

    band = std::max(band, 1);
    const double bandDist = band * voxelSize;
    auto radius = [&](size_t p) { return radii.empty() ? 0.0 : std::max(0.0, radii[radii.size() == 1 ? 0 : p]); };
    geo::Bounds b;
    double maxR = 0.0;
    for (size_t p = 0; p < points.size(); ++p)
    {
        b.extend(points[p]);
        maxR = std::max(maxR, radius(p));
    }
    if (!b.valid())
        return Volume::grid({}, voxelSize, Volume::Kind::Sdf, float(bandDist));
    glm::dvec3 pad(maxR + bandDist + voxelSize);
    b.min -= pad;
    b.max += pad;
    Volume v = Volume::grid(b, voxelSize, Volume::Kind::Sdf, float(bandDist));
    tbb::parallel_for(0, v.dims.z, [&](int k) {
        for (size_t p = 0; p < points.size(); ++p)
        {
            double r = radius(p), reach = r + bandDist;
            glm::dvec3 lo = v.voxelOf(points[p] - glm::dvec3(reach)), hi = v.voxelOf(points[p] + glm::dvec3(reach));
            if (k < int(std::floor(lo.z)) || k > int(std::ceil(hi.z)))
                continue;
            int i0 = std::max(0, int(std::floor(lo.x))), i1 = std::min(v.dims.x - 1, int(std::ceil(hi.x)));
            int j0 = std::max(0, int(std::floor(lo.y))), j1 = std::min(v.dims.y - 1, int(std::ceil(hi.y)));
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i)
                {
                    double d = std::clamp(glm::length(v.worldOf({i, j, k}) - points[p]) - r, -bandDist, bandDist);
                    float& cell = v.values[v.index(i, j, k)];
                    cell = std::min(cell, float(d));
                }
        }
    });
    return v;
}

geo::Mesh volumeToMesh(const Volume& v, double threshold)
{
    LILA_ZONE();

    if (v.empty())
        return {};
    // Solid is negative for surface nets; a one-voxel border of background closes the surface at the grid's edge.
    const bool sdf = v.isSdf();
    const glm::ivec3 n = v.dims + 2;
    std::vector<float> padded(size_t(n.x) * size_t(n.y) * size_t(n.z));
    tbb::parallel_for(0, n.z, [&](int k) {
        for (int j = 0; j < n.y; ++j)
            for (int i = 0; i < n.x; ++i)
            {
                const double val = v.at(i - 1, j - 1, k - 1);
                padded[size_t(i) + size_t(n.x) * (size_t(j) + size_t(n.y) * size_t(k))] = float(sdf ? val - threshold : threshold - val);
            }
    });
    const geo::SurfaceNets nets = geo::surfaceNets({n, padded});
    std::vector<glm::dvec3> pts(nets.cells.size());
    for (size_t p = 0; p < pts.size(); ++p)
        pts[p] = v.worldOf(nets.point(p) - 1.0);
    std::vector<geo::Index> quads = nets.quads;
    if (glm::determinant(glm::dmat3(v.indexToWorld)) < 0.0)
        for (size_t q = 0; q + 3 < quads.size(); q += 4)
            std::swap(quads[q + 1], quads[q + 3]);
    std::vector<geo::Index> sizes(quads.size() / 4, 4);
    return geo::Mesh::fromPolygons(pts, sizes, quads);
}

Volume sdfToFog(const Volume& sdf, double density, double interiorBand, bool fill)
{
    Volume v = sdf;
    v.kind = Volume::Kind::Density;
    v.name = "density";
    v.background = 0.0f;
    v.vectors.clear();
    const float d = float(density), band = float(std::max(interiorBand, 0.0));
    tbb::parallel_for(size_t(0), v.values.size(), [&](size_t n) {
        const float x = sdf.values[n];
        v.values[n] = x < 0.0f && (fill || -x <= band) ? d : 0.0f;
    });
    return v;
}
} // namespace lila::gn
