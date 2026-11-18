#pragma once

#include "core/Export.h"

#include "brush/Brush.h"
#include "geo/Bvh.h"
#include "tex/PaintStrokes.h"

#include <glm/glm.hpp>

#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

/**
 * Where a paint buffer's texels are on the surface: a mesh's triangles as floats, and per UDIM tile and resolution an
 * atlas that rasterises the triangles' UVs page by page (128x128 texels, on first use) into the triangle and
 * barycentrics owning each texel. Texels within the padding of an island take the nearest point of the nearest
 * triangle, so strokes reach into the padding the way dilation would and seams match on both sides (both are painted
 * by 3D distance). CPU and deterministic: the same mesh and resolution always give the same texels.
 */
namespace lila::tex::paint
{
inline constexpr uint32_t kNoTriangle = 0xFFFFFFFFu;

/** Texel owned by no triangle, or by one's padding. */
inline constexpr uint32_t kTexelPadding = 1u;

/** One texel's place on the surface: 16 bytes, uploaded to the GPU as it is. */
struct GeoTexel
{
    uint32_t triangle = kNoTriangle;
    float u = 0.0f, v = 0.0f;
    uint32_t flags = 0;
};
static_assert(sizeof(GeoTexel) == 16);

/** A mesh's triangles in the order of its triangulation (MeshBvh::triangles()), three corners each, object space. */
struct LILA_SCENE_API PaintMesh
{
    std::vector<glm::vec3> positions;
    /** Smooth point normals: facing tests should not stop at hard edges. */
    std::vector<glm::vec3> normals;
    /** MikkTSpace corner tangents (w the bitangent sign); zero without uvs. */
    std::vector<glm::vec4> tangents;
    std::vector<glm::vec2> uvs;
    /** Per triangle: its face uses the painted material. */
    std::vector<uint8_t> paintable;
    /** Per triangle: the mesh face it came from and that face's material slot (-1 without one). */
    std::vector<uint32_t> faces;
    std::vector<int32_t> materials;
    /** Per corner (three per triangle), the mesh point it is: what a topological symmetry map pairs. */
    std::vector<uint32_t> points;
    std::shared_ptr<const geo::MeshBvh> bvh;
    /** Hash of the triangulation, its uvs and what is paintable: equal signatures, the same dab triangles and barycentrics. */
    uint64_t signature = 0;

    size_t triangleCount() const { return paintable.size(); }
    glm::vec3 point(uint32_t tri, float u, float v) const;
    glm::vec3 normal(uint32_t tri, float u, float v) const;
    glm::vec3 point(const Dab& d) const { return point(d.triangle, d.u, d.v); }
    glm::vec2 uv(uint32_t tri, float u, float v) const;

    /** Per triangle, its UV island: triangles joined by edges equal in position and uv. Ids start at 0, in triangle order. */
    std::vector<uint32_t> islands() const;
    /** Paintable triangles: all, one's island, one's material slot, or those of the given faces. Ascending. */
    std::vector<uint32_t> allTriangles() const;
    std::vector<uint32_t> islandOf(uint32_t tri) const;
    std::vector<uint32_t> materialOf(uint32_t tri) const;
    std::vector<uint32_t> ofFaces(std::span<const uint32_t> faces) const;
    /** The triangle and barycentrics at a uv point (inside, or within maxDistance uv of an edge); false when none. */
    bool atUv(const glm::vec2& uv, uint32_t& tri, float& u, float& v, float maxDistance = 0.0f) const;

    /**
     * The painted part of a mesh: faces whose material slot is `material` (or one of several joined by
     * kMaterialSeparator, a set driving more than one), or every face when none uses it (or material is empty). Uses
     * uv0. bvh must be built from mesh.
     */
    static std::shared_ptr<const PaintMesh> build(const geo::Mesh& mesh, std::shared_ptr<const geo::MeshBvh> bvh, std::string_view material);
};

/** Joins the materials of a set that drives several, in PaintMesh::build's material key. */
inline constexpr char kMaterialSeparator = ';';

/** A dab's centre and the distance its tip can reach. */
struct DabReach
{
    glm::vec3 center{0.0f};
    float radius = 0.0f;
};

/**
 * A brush dab as a stroke record dab: on the closest surface point within its radius (symmetry copies may sit off the
 * surface), its tip angle measured from tipFrame there. False when no paintable surface is that close.
 */
LILA_SCENE_API bool recordDab(const PaintMesh& mesh, const brush::Dab& d, Dab& out);

/**
 * For a tileable texture: the dab's copies one uv square over (across u, v or both) where it reaches past an edge, on
 * the surface that holds the uv it wraps to. Empty when it reaches no edge.
 */
LILA_SCENE_API std::vector<Dab> tileWrapped(const PaintMesh& mesh, const Dab& d);

class LILA_SCENE_API SurfaceAtlas
{
public:
    SurfaceAtlas(std::shared_ptr<const PaintMesh> mesh, int udim, int width, int height, int padding = kDefaultPadding);

    int udim() const { return mUdim; }
    int width() const { return mWidth; }
    int height() const { return mHeight; }
    int pagesX() const { return mPagesX; }
    int pagesY() const { return mPagesY; }
    const PaintMesh& mesh() const { return *mMesh; }

    /** Whether any triangle (or its padding) reaches the page. */
    bool pageUsed(uint32_t page) const { return page < mPageTris.size() && !mPageTris[page].empty(); }
    /** The page's texels, row 0 at the top (v = 1), rasterised on first use. Not thread safe. */
    const std::vector<GeoTexel>& page(uint32_t page);
    /** Rasterises the pages not yet rasterised, in parallel, so the page() calls after it are lookups. Not thread safe. */
    void preparePages(std::span<const uint32_t> pages);
    size_t pagesRasterised() const { return mPages.size(); }

    /**
     * For every page the dabs reach, the indices of the dabs reaching it (offset by first), in order. A dab reaches a
     * page when a triangle near its centre (within reach) has texels or padding there. Triangles skip marks (non-zero)
     * reach nothing.
     */
    std::map<uint32_t, std::vector<uint32_t>> pagesFor(std::span<const DabReach> dabs, uint32_t first = 0, std::span<const uint8_t> skip = {}) const;
    /** Pages holding texels (or padding) of a triangle that keep says yes to, ascending. */
    std::vector<uint32_t> pagesWhere(const std::function<bool(uint32_t tri)>& keep) const;

    /** Texel (x, y) of the tile to uv (texel centre), and back. */
    glm::vec2 uvOf(double x, double y) const;
    glm::dvec2 texelOf(const glm::vec2& uv) const;

private:
    std::shared_ptr<const PaintMesh> mMesh;
    int mUdim = 1001, mWidth = 0, mHeight = 0, mPadding = 0, mPagesX = 0, mPagesY = 0;
    glm::vec2 mOrigin{0.0f};
    /** Triangles per page, ascending. */
    std::vector<std::vector<uint32_t>> mPageTris;
    struct Box
    {
        glm::vec3 lo{1e30f}, hi{-1e30f};
        void extend(const glm::vec3& p)
        {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
    };
    /** Per triangle, its pages (CSR) and the 3D bounds of its texels in each. */
    std::vector<uint32_t> mTriPageStart;
    std::vector<uint32_t> mTriPages;
    std::vector<Box> mTriPageBoxes;
    std::map<uint32_t, std::vector<GeoTexel>> mPages;

    std::vector<GeoTexel> rasterise(uint32_t page) const;
};
} // namespace lila::tex::paint
