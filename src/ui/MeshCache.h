#pragma once

#include "core/Export.h"

#include "geo/Ops.h"
#include "render/RenderMesh.h"
#include "scene/Plug.h"

#include <functional>
#include <optional>
#include <span>

namespace lila::geo
{
struct RenderMesh;
}

namespace lila::ui
{
/**
 * GPU-ready copy of a scene mesh, built once per mesh snapshot and shared by every viewport, triangles grouped by
 * material slot. tangents adds MikkTSpace tangents (for normal maps); a copy that has them serves both. owner, the node
 * drawing it, lets a deforming mesh rewrite that node's GPU vertices in place each frame instead of uploading anew.
 */
LILA_UI_API std::shared_ptr<const render::RenderMesh> renderMeshFor(const scene::MeshPtr& mesh, bool tangents = false,
                                                                    scene::NodeId owner = scene::kNoNode);

/** The mesh subdivided for smooth preview, cached per snapshot and level. Level 0 returns the mesh itself. */
LILA_UI_API scene::MeshPtr smoothedFor(const scene::MeshPtr& mesh, int level);

/** A smooth preview and where its cage's components lie on it, so they can be drawn and picked on the smooth surface. */
struct SmoothCage
{
    scene::MeshPtr mesh;
    geo::SubdivideMap map;
    /** Per smoothed point: the cage point it is, kInvalid for a new one. */
    std::vector<geo::Index> cagePoints;
    std::vector<geo::Index> edgeOffsets, edgeSegments;
    std::vector<geo::Index> faceOffsets, faceParts;

    /** The smoothed edges that run along cage edge e. */
    std::span<const geo::Index> segments(geo::Index e) const
    {
        return e + 1 < edgeOffsets.size() ? std::span(edgeSegments).subspan(edgeOffsets[e], edgeOffsets[e + 1] - edgeOffsets[e]) : std::span<const geo::Index>();
    }
    /** The smoothed faces that make up cage face f. */
    std::span<const geo::Index> parts(geo::Index f) const
    {
        return f + 1 < faceOffsets.size() ? std::span(faceParts).subspan(faceOffsets[f], faceOffsets[f + 1] - faceOffsets[f]) : std::span<const geo::Index>();
    }
};

/** smoothedFor(mesh, level) with its maps back to mesh; null for level 0. */
LILA_UI_API std::shared_ptr<const SmoothCage> smoothCageFor(const scene::MeshPtr& mesh, int level);

/** Whether a modifier stack changed the cage's edges (a bevel, a subdivide), rather than only moving its points. */
LILA_UI_API bool modifiersAddEdges(const scene::MeshPtr& cage, const scene::MeshPtr& result);
/**
 * Wire for a modifier result, as line-only render meshes in the result's space: real is each cage edge laid onto the
 * result's surface (a bevelled edge runs along the middle of its bevel), added is every result edge that is not on one
 * of those (the bevel's own edges).
 */
struct ModifierWire
{
    std::shared_ptr<const render::RenderMesh> real, added;
};
LILA_UI_API ModifierWire modifierWireFor(const scene::MeshPtr& cage, const scene::MeshPtr& result);

/** renderMeshFor(mesh) without its wire lines, for a surface whose edges are drawn by another draw. */
LILA_UI_API std::shared_ptr<const render::RenderMesh> wirelessMeshFor(const scene::MeshPtr& mesh, bool tangents = false);

/** renderMeshFor(smooth.mesh) with its wire cut down to the edges that run along the cage's. */
LILA_UI_API std::shared_ptr<const render::RenderMesh> cageWireMeshFor(const SmoothCage& smooth, bool tangents = false);

/** Triangle-to-face, vertex-to-point and line-to-edge maps for the same render copy, used to pick components. */
LILA_UI_API std::shared_ptr<const geo::RenderMesh> pickDataFor(const scene::MeshPtr& mesh);

/**
 * Gives `to` the render copy of `from` with its vertices passed through edit, for a snapshot that differs from `from`
 * only in vertex colors, so it needn't be rebuilt. False when `from` has no copy yet.
 */
LILA_UI_API bool reuseRenderMesh(const scene::MeshPtr& from, const scene::MeshPtr& to, const std::function<void(UninitVector<render::MeshVertex>&)>& edit);

/** A render copy drawing only some faces of its mesh, sharing the full copy's vertices on the GPU. */
struct MaskedMesh
{
    std::shared_ptr<const render::RenderMesh> mesh;
    /** 1 per triangle of the full copy that is hidden, for raycast. */
    std::vector<uint8_t> hiddenTriangles;
};

/**
 * The faces shownFaces() marks (1 per face of mesh; called only on a cache miss, empty shows all) as a triangle and wire
 * subset of renderMeshFor(mesh), cached per mesh snapshot and maskKey so the mask is only applied once per change.
 * cageWire, when mesh is its smoothed mesh, keeps only the wire along the cage's edges.
 */
LILA_UI_API std::shared_ptr<const MaskedMesh> maskedRenderMeshFor(const scene::MeshPtr& mesh, uint64_t maskKey, const std::function<std::vector<uint8_t>()>& shownFaces,
                                                                  bool tangents = false, const SmoothCage* cageWire = nullptr);

struct RayHit
{
    double distance = 0.0; // world units
    uint32_t triangle = 0;
};

/** Nearest hit along a world-space ray, or nullopt. Triangles come from the render copy; hidden (1 per triangle) ones are skipped. */
LILA_UI_API std::optional<RayHit> raycast(const render::RenderMesh& mesh, const glm::dmat4& world, const glm::dvec3& origin, const glm::dvec3& dir,
                                          const std::vector<uint8_t>* hidden = nullptr);
} // namespace lila::ui
