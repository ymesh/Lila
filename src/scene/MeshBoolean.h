#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>

/** Mesh booleans on Manifold, with the seam cleanup a modeller would otherwise do by hand. */
namespace lila::scene
{
enum class BooleanOp : uint8_t
{
    Union,
    Difference,
    Intersection,
};

/** "union", "difference", "intersection" (or "intersect"). */
LILA_SCENE_API std::optional<BooleanOp> booleanOpFromName(std::string_view name);

/** Edge bool on the result: the intersection curve, where faces from different operands meet. */
inline constexpr std::string_view kBooleanSeamAttr = "booleanSeam";

struct BooleanOperand
{
    const geo::Mesh* mesh = nullptr;
    glm::dmat4 toWorld{1.0};
};

struct BooleanOptions
{
    BooleanOp op = BooleanOp::Difference;
    /** Weld seam points onto close neighbours and drop straight-through seam points. */
    bool cleanup = true;
    /** Weld distance for cleanup, cm; negative is a tenth of the median seam edge. */
    double snapDistance = -1.0;
    /** Cut the n-gons along the seam into quads and triangles that carry on the edge loops meeting them. */
    bool splitNgons = true;
    /**
     * Remesh a band of faces around the seam into quads that follow it, keeping the band's border points so it welds back.
     * Where the border can't be kept (its edges far from the band's spacing) the n-gon split runs instead.
     */
    bool quadBand = false;
    /** Rings of faces beyond the ones touching the seam that the band takes in. */
    int bandRings = 1;
    /** Seconds the band remesh may take before the n-gon split runs instead; 0 is unlimited. */
    double bandTimeout = 5.0;
    /** Width of a rounded strip along the seam, cm; 0 leaves it sharp. */
    double filletWidth = 0.0;
    int filletSegments = 2;
    /** Mark the fillet's faces with kFilletFaceAttr = 1. */
    bool markFillet = false;
    /** New edges sharper than this many degrees are marked hard. */
    double hardAngle = 30.0;
};

/** Face int attribute on a boolean's result with BooleanOptions::markFillet: 1 on the faces of the fillet strip. */
inline constexpr std::string_view kFilletFaceAttr = "booleanFillet";

struct BooleanResult
{
    geo::Mesh mesh;
    /** Seam edges in the result, before any fillet replaced them. */
    geo::Index seamEdges = 0;
    std::string error;
    /** The operand the error is about, or -1. */
    int badOperand = -1;
    /** The seam band was remeshed into quads (BooleanOptions::quadBand). */
    bool quadBand = false;
    bool ok() const { return error.empty(); }
};

/**
 * Combines the operands in order (the first is the base) into a mesh in toLocal space. Each operand's polygons come
 * back whole away from the seam, with their uvs, the base's colour sets, hard edges, creases, uv seams and materials.
 */
LILA_SCENE_API BooleanResult meshBoolean(std::span<const BooleanOperand> operands, const glm::dmat4& toLocal,
                                         const BooleanOptions& opts = {});
} // namespace lila::scene
