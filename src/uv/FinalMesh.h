#pragma once

#include "core/Export.h"
#include "geo/Ops.h"
#include "uv/Chart.h"
#include "uv/Optimize.h"

#include <complex>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lila::uv
{
/** One modifier of a mesh's stack as exported, run on a copy of its input. */
struct StackStep
{
    std::string name;
    std::function<geo::Mesh(const geo::Mesh&)> apply;
    /** A subdivision: runStack adds the Fit Surface spacing layers to the stack's input (see geo::addFitSpacing). */
    bool subdivides = false;
};
using Stack = std::vector<StackStep>;

/** A map entry: it scales and rotates a uv read as u + iv (the 2x2 block [re -im; im re]). */
using FinalWeight = std::complex<double>;

/** w times the uv, read as u + iv. */
inline glm::dvec2 weigh(FinalWeight w, glm::dvec2 v)
{
    return {w.real() * v.x - w.imag() * v.y, w.real() * v.y + w.imag() * v.x};
}

/** How a FinalMap relates cage uvs to the exported mesh's. */
enum class FinalMethod : uint8_t
{
    Cage,   // the stack leaves the surface alone, or its uvs can't be traced: solve on the cage
    Probed, // the stack blends cage uvs linearly; the map was measured by probe uv layers sent through it
};

/**
 * A Fit Surface subdivision's fill of one exported uv island: its interior corners' uvs as the island's harmonic fill of
 * its border groups' uvs. Indices are corners of whatever the fill sits in (the exported mesh, or a FinalIsland).
 */
struct FinalFill
{
    std::shared_ptr<const geo::FitFill> fill;
    /** Per border group, a corner whose row holds the group's uv. */
    std::vector<Index> border;
    /** Per interior group (in the fill's order), its corners: interiorStart into interiorCorners, one extra at the end. */
    std::vector<Index> interiorStart;
    std::vector<Index> interiorCorners;
};

/**
 * The exported mesh's corner uvs as a sparse linear map of the cage's: exported uv = sum of weight * cage uv over the
 * row's cage corners, each weight a scale and rotation (a Fit Surface subdivision rebuilds island corners by turning
 * their border edge). Holds for any stack whose modifiers only blend corner uvs (bevel, subdivision, mirror...), for a
 * fixed cage topology and seam layout. Columns are cage corners standing for their whole uv wedge. The interior corners
 * of a Fit Surface fill have empty rows: their uvs come from the fill.
 */
struct LILA_SCENE_API FinalMap
{
    FinalMethod method = FinalMethod::Cage;
    /** Why the cage is used although the stack changes the surface, naming the modifier; empty otherwise. */
    std::string warning;
    /** The stack's output for the cage. Null when the stack is empty. */
    std::shared_ptr<const geo::Mesh> exported;
    /** Per exported corner (capacity), its entries in cols and weights; one extra at the end. */
    std::vector<Index> rowStart;
    std::vector<Index> cols;
    std::vector<FinalWeight> weights;
    std::vector<FinalFill> fills;
    /** Per exported face (capacity), the cage face it came from. */
    std::vector<Index> sourceFace;
    /** Face rings around an exported corner's cage face that held its whole row. */
    int rings = 0;
    /** Stack runs the probes took. */
    int runs = 0;

    bool active() const { return method == FinalMethod::Probed; }
    const char* methodName() const { return active() ? "probed" : "cage"; }
};

/**
 * The exported mesh and which cage face each of its faces came from, from one run of the stack, without the uv map:
 * enough to measure exported uvs per cage face (distortion, texel density). Exported is null when the stack leaves the
 * surface alone.
 */
LILA_SCENE_API FinalMap traceFaces(const geo::Mesh& cage, const Stack& stack);

/** The exported faces that came from the cage faces (all live exported faces when empty), ascending. */
LILA_SCENE_API std::vector<Index> exportedFaces(const FinalMap& map, std::span<const Index> cageFaces);

/** True when the stack's output differs from the cage in topology or point positions, so uvs land on another surface. */
LILA_SCENE_API bool changesSurface(const geo::Mesh& cage, const geo::Mesh& exported);

/** Runs the stack on a copy of the mesh, with Fit Surface spacing layers added at its start when a step subdivides. */
LILA_SCENE_API geo::Mesh runStack(const geo::Mesh& mesh, const Stack& stack);

/** Measures the map. Cage (with no warning) when the stack leaves the surface unchanged. */
LILA_SCENE_API FinalMap buildFinalMap(const geo::Mesh& cage, std::string_view uvSet, const Stack& stack);

/** The map's exported uv per corner (capacity) for the cage's uvs in the set. */
LILA_SCENE_API std::vector<glm::dvec2> exportedUvs(const FinalMap& map, const geo::Mesh& cage, std::string_view uvSet);

/**
 * The exported triangles of one chart, with their uvs as a map of the chart's vertices. Fills read local corners; a
 * border group whose corners lie in another chart's faces gets a local corner of its own that no triangle uses.
 */
struct LILA_SCENE_API FinalIsland
{
    std::vector<glm::dvec3> pos;    // per local corner
    std::vector<Index> corner;      // per local corner: the exported corner it stands for
    std::vector<glm::dvec2> offset; // per local corner: the part of its uv from cage wedges outside the chart
    std::vector<Index> rowStart;    // per local corner, into cols and weights; one extra at the end
    std::vector<Index> cols;        // chart vertices
    std::vector<FinalWeight> weights;
    std::vector<std::array<Index, 3>> tris; // local corners
    std::vector<Index> triFace;             // exported face per triangle
    std::vector<FinalFill> fills;

    double area3d() const;
    /** Exported uv per local corner for the chart's current uvs. */
    std::vector<glm::dvec2> uvs(const Chart& chart) const;
    /** Signed exported uv area for the chart's current uvs. */
    double areaUv(const Chart& chart) const;
};

/** Per chart, its exported triangles: each exported face goes to the chart holding most of its corners' weight. */
LILA_SCENE_API std::vector<FinalIsland> finalIslands(const FinalMap& map, const geo::Mesh& cage, std::span<const Chart> charts,
                                                     std::string_view uvSet);

/**
 * The chart vertices the exported uvs barely read (the inside of an island a Fit Surface subdivision fills from its
 * border, reached at most through a bevel's sliver) placed as the harmonic fill of the others and the pins on the cage.
 * Solves move the others over the folded map, so the cage layout keeps following the exported one instead of flinging
 * weak vertices far to nudge the export.
 */
class LILA_SCENE_API UninfluencedFill
{
public:
    UninfluencedFill(const Chart& chart, const FinalIsland& island);
    ~UninfluencedFill();

    bool empty() const { return mUnknown.empty(); }
    /** Per chart vertex, whether solves place it (the rest are filled). */
    const std::vector<uint8_t>& solved() const { return mSolved; }
    /** The island with the filled vertices' weights moved onto the solved ones through the fill. */
    FinalIsland fold(const FinalIsland& island) const;
    /** Rewrites the filled entries of uv (any affine frame of the chart's uvs) from the others. */
    void apply(std::span<glm::dvec2> uv) const;

private:
    struct Solver;
    std::vector<uint8_t> mSolved;
    std::vector<Index> mUnknown;
    std::unique_ptr<Solver> mSolver;
};
/** A chart vertex no exported uv reads with at least this weight is filled rather than solved. */
inline constexpr double kWeakInfluence = 0.2;

/** Distortion of the exported triangles for the chart's current uvs. */
LILA_SCENE_API ChartStats finalStats(const FinalIsland& island, const Chart& chart);

/**
 * Exported triangles squeezed under kCollapsedFraction of the uv area their island's density gives them, as bake lint
 * counts them (uvMissing).
 */
LILA_SCENE_API size_t collapsedTriangles(const FinalIsland& island, const Chart& chart);
/** The same count for exported uvs measured on the exported mesh (per exported corner, capacity). */
LILA_SCENE_API size_t collapsedTriangles(const FinalIsland& island, std::span<const glm::vec2> exportedUv);
inline constexpr double kCollapsedFraction = 0.02;

/** Sets chart.densityArea so areaUv() / densityArea() is the exported island's texel density. */
LILA_SCENE_API void setDensityArea(Chart& chart, const FinalIsland& island);

struct FinalSolveOptions
{
    /** Isometric pulls each exported triangle to its own shape at the island's density (as rigid as possible). */
    OptimizeMode mode = OptimizeMode::Isometric;
    double mix = 0.5;
    int iterations = 30;
    bool usePins = true;
};

/**
 * Moves the chart's uvs so the exported triangles have the least distortion (local/global through the map, the
 * factorisation reused every iteration). Keeps the island's centroid and its density on the exported mesh; with two or
 * more pins those hold the place and scale instead. Steps never fold an exported triangle when the start has none.
 */
LILA_SCENE_API OptimizeResult solveFinal(Chart& chart, const FinalIsland& island, const FinalSolveOptions& opts = {});

struct FinalRefineResult
{
    FinalMethod method = FinalMethod::Cage;
    std::string warning;
    size_t islands = 0;
    size_t failed = 0;
    size_t collapsedBefore = 0;
    size_t collapsedAfter = 0;
    double distortionBefore = 0.0; // area weighted angle distortion of the exported triangles
    double distortionAfter = 0.0;
};

/**
 * Re-solves the uv islands of the faces (all when empty) against the exported mesh and writes them back, then measures the
 * export the written uvs give with one run of the stack and puts back any island that came out with more collapsed
 * triangles than it started with.
 * Does nothing when the stack leaves the surface alone.
 */
LILA_SCENE_API FinalRefineResult refineFinal(geo::Mesh& cage, std::span<const Index> faces, std::string_view uvSet, const Stack& stack,
                                             const FinalSolveOptions& opts = {});

/**
 * Re-solves only the islands of the faces (all when empty) whose exported triangles collapse, checked like refineFinal:
 * for tools that move uvs some other way first, like Live Relax.
 */
LILA_SCENE_API FinalRefineResult repairFinal(geo::Mesh& cage, std::span<const Index> faces, std::string_view uvSet, const Stack& stack,
                                             const FinalSolveOptions& opts = {});

/** Moves an island that grew past its UDIM tile back inside, shrinking it only when it is bigger than the tile. */
LILA_SCENE_API void keepInUdimTile(Chart& chart, const glm::ivec2& tile);

/** Sets densityArea on each chart from the exported mesh, when the stack changes the surface. Returns the map used. */
LILA_SCENE_API FinalMap applyDensityAreas(const geo::Mesh& cage, std::span<Chart> charts, std::string_view uvSet, const Stack& stack);
} // namespace lila::uv
