#pragma once

#include "core/Export.h"

#include "gn/Geometry.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace lila::gn
{
/**
 * A dense grid: a density (fog), a signed distance (negative inside) or a vector field. Voxel (i, j, k) is
 * values[i + nx * (j + ny * k)] and its centre sits at indexToWorld * (i, j, k, 1), so the matrix carries the voxel size
 * and any transform. Outside the grid every value reads as background. A vector grid keeps its vectors in vectors and
 * their lengths in values, so anything reading only values sees its magnitude; its background is the zero vector.
 */
struct LILA_SCENE_API Volume
{
    enum class Kind : uint8_t
    {
        Density,
        Sdf,
        Vector,
    };

    std::string name = "density";
    Kind kind = Kind::Density;
    glm::ivec3 dims{0};
    glm::dmat4 indexToWorld{1.0};
    std::vector<float> values;
    std::vector<glm::vec3> vectors;
    float background = 0.0f;

    /** An axis-aligned grid whose voxel centres cover the box, filled with background. Throws past about 64 million voxels. */
    static Volume grid(const geo::Bounds& box, double voxelSize, Kind kind, float background);

    bool empty() const { return values.empty(); }
    size_t size() const { return values.size(); }
    bool isSdf() const { return kind == Kind::Sdf; }
    bool isVector() const { return kind == Kind::Vector; }
    size_t index(int i, int j, int k) const { return size_t(i) + size_t(dims.x) * (size_t(j) + size_t(dims.y) * size_t(k)); }
    bool inside(int i, int j, int k) const { return i >= 0 && j >= 0 && k >= 0 && i < dims.x && j < dims.y && k < dims.z; }
    float at(int i, int j, int k) const { return inside(i, j, k) ? values[index(i, j, k)] : background; }
    glm::vec3 vectorAt(int i, int j, int k) const { return inside(i, j, k) && !vectors.empty() ? vectors[index(i, j, k)] : glm::vec3(0.0f); }
    /** Makes this a vector grid holding v (one per voxel), with values set to their lengths. */
    void setVectors(std::vector<glm::vec3> v);

    glm::dvec3 worldOf(const glm::dvec3& voxel) const { return glm::dvec3(indexToWorld * glm::dvec4(voxel, 1.0)); }
    glm::dvec3 voxelOf(const glm::dvec3& world) const;
    /** Trilinear, background outside. */
    double sample(const glm::dvec3& world) const;
    double sampleNearest(const glm::dvec3& world) const;
    /** Trilinear vector, zero outside; the zero vector on a scalar grid. */
    glm::dvec3 sampleVector(const glm::dvec3& world) const;
    glm::dvec3 sampleVectorNearest(const glm::dvec3& world) const;
    /** Edge length of a voxel in world units (the cube root of its volume). */
    double voxelSize() const;
    /** World bounds of the voxel centres. */
    geo::Bounds bounds() const;
    /** These values at another grid's voxel centres. */
    std::vector<float> resampled(const Volume& target) const;
    std::vector<glm::vec3> resampledVectors(const Volume& target) const;
    /** World-space step along each voxel axis, for finite differences: the columns of the grid's linear part. */
    glm::dmat3 axes() const { return glm::dmat3(indexToWorld); }
};

LILA_SCENE_API GeometryPtr volumeGeometry(Volume v);

/**
 * Narrow-band signed distance to a closed mesh, band voxels either side of the surface; further voxels hold plus or minus
 * the band distance. Inside is decided by ray parity along each grid row.
 */
LILA_SCENE_API Volume meshToSdf(const geo::Mesh& m, double voxelSize, int band);

/** Spheres around points, as a narrow-band signed distance. radii is one per point, or one for all. */
LILA_SCENE_API Volume pointsToSdf(std::span<const glm::dvec3> points, std::span<const double> radii, double voxelSize, int band);

/**
 * The surface where the grid crosses threshold, as quads (surface nets). Solid is below threshold for a signed distance
 * and above it for a density; the grid's border is closed against the background.
 */
LILA_SCENE_API geo::Mesh volumeToMesh(const Volume& v, double threshold);

/**
 * Fog from a signed distance: density inside, kept to the voxels within interiorBand of the surface unless fill is set.
 * Voxels outside read 0.
 */
LILA_SCENE_API Volume sdfToFog(const Volume& sdf, double density, double interiorBand, bool fill);
} // namespace lila::gn
