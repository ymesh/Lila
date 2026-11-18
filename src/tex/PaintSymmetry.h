#pragma once

#include "core/Export.h"

#include "brush/Brush.h"
#include "geo/Symmetry.h"
#include "tex/PaintSurface.h"

#include <nlohmann/json.hpp>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <vector>

/**
 * Symmetry for texture painting: the copies a stroke paints besides itself, the way brush::symmetricDabs numbers them
 * (radial copies first, then each one's mirror). Every painting kind uses it: dabs are copied on the surface (mirror
 * copies through the mesh's point pairs when a geo::SymmetryMap is given, so deformed meshes still mirror exactly),
 * fills copy their seeds, clones their sources, and projections project once per copy through the transformed view.
 * Records keep the copies they made and this spec, so replay never depends on the mesh's symmetry settings later.
 */
namespace lila::tex::paint
{
/** A mirror in uv space: across u (or v) = center, or across each uv island's own middle. */
struct UvMirror
{
    /** 0 mirrors u, 1 mirrors v. */
    int axis = 0;
    double center = 0.5;
    bool perIsland = false;

    bool operator==(const UvMirror&) const = default;
};

struct LILA_SCENE_API PaintSymmetry
{
    std::optional<geo::SymmetryPlane> mirror;
    /** Every copy above, again mirrored in uv space: for symmetric unwraps, whatever the mesh's shape. */
    std::optional<UvMirror> uvMirror;
    brush::Radial radial;
    /** The mesh's radial symmetry: when set, its elements are the copies, in place of mirror and radial. */
    std::optional<geo::RadialSymmetry> group;
    /** Point pairs for topological mirror copies; not part of the record. */
    std::shared_ptr<const geo::SymmetryMap> map;

    bool any() const { return group.has_value() || mirror.has_value() || radial.any() || uvMirror.has_value(); }
    /** Copies before the uv mirror doubles them. */
    uint32_t spatialCount() const;
    /** Copies including the original. */
    uint32_t count() const;
    /** Whether copy k is a reflection (its tip turns the other way). */
    bool mirrored(uint32_t copy) const;
    /** Object space to object space: where copy k puts a point of the original. Copy 0 is the identity. */
    glm::dmat4 transform(uint32_t copy) const;

    /**
     * {"mirror": [nx, ny, nz, offset] or null, "radial": {count, axis}}, or {"group": {origin, axis, reference, count,
     * mirror}} for a mesh's radial symmetry; null when there is no symmetry.
     */
    nlohmann::json toJson() const;
    static PaintSymmetry fromJson(const nlohmann::json& j);

    /** The dab (copy 0) followed by its copies on the surface; copies with no surface near them are left out. */
    std::vector<Dab> copies(const PaintMesh& mesh, const Dab& d) const;
    /** The point on the surface copy k of (tri, u, v) lands on; false when none is near. */
    bool copyPoint(const PaintMesh& mesh, uint32_t tri, float u, float v, uint32_t copy, Dab& out) const;

private:
    struct Index;
    mutable std::shared_ptr<Index> mIndex;
    mutable const PaintMesh* mIndexed = nullptr;
};

/**
 * The symmetry a brush asks for on a mesh: its mirror across the mesh's symmetry plane (topological through the mesh's
 * stored point pairs when it has them) and radial copies.
 */
LILA_SCENE_API PaintSymmetry brushSymmetry(const brush::BrushSettings& b, const geo::Mesh* mesh, int axis, double offset);

/**
 * The same for a mesh's symmetry setting (nullopt when off). With radial symmetry the brush's mirror switch turns on
 * the mesh's radial group, copied through its orbits, in place of the brush's own radial copies.
 */
LILA_SCENE_API PaintSymmetry brushSymmetry(const brush::BrushSettings& b, const geo::Mesh* mesh, const std::optional<geo::SymmetrySpec>& spec);
} // namespace lila::tex::paint
