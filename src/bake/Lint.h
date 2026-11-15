#pragma once

#include "core/Export.h"

#include "bake/BakeTypes.h"
#include "scene/Node.h"

#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * bakeLint: problems that spoil a bake, found before it runs. bake() lints first and lists what it finds in its warnings.
 *
 *   hardEdges        hard edges that are not UV seams (the normal map can't hide the shading split)
 *   uvOverlap        UV islands overlapping within a texture by more than a texel, including across groups writing the same file
 *   uvMissing        lows without uv0, triangles big enough to see collapsed to a sliver of UV
 *   texelDensity     islands whose texels per cm are far from the rest of their mesh
 *   mirroredTangents triangles whose UV winding disagrees with their tangent sign (a mirrored island baked as unmirrored), islands folding over themselves
 *   highRange        lows whose rays find no high (or few) within the frontal and rear distances
 *   cageContainment  high points outside the cage (beyond frontal) or deeper than rear
 *   triangulation    non-planar faces whose triangles depend on the exporter's split (only when named)
 *   cageTopology     an editable cage that doesn't match its low
 */
namespace lila::bake
{
struct BakeInputs;
class Backend;

struct LILA_SCENE_API LintIssue
{
    std::string check;
    /** warning or info. */
    std::string severity = "warning";
    std::string group;
    std::string mesh;
    std::string message;
    json data = json::object();

    json toJson() const;
};

struct LILA_SCENE_API LintReport
{
    std::vector<LintIssue> issues;

    size_t warnings() const;
    /** {issues, warnings, checks}. */
    json toJson() const;
    /** One line per warning, "lint <check>: <message>", for a bake's warnings. */
    std::vector<std::string> lines() const;
};

/** The check names above, in order. */
LILA_SCENE_API const std::vector<std::string>& lintChecks();

struct LintOptions
{
    /** Checks to run; empty runs all but triangulation. */
    std::vector<std::string> checks;
    /** Islands whose density is beyond median x this (or below median / this) are outliers. */
    double densityTolerance = 2.0;
    /** Rays per low for highRange; high points sampled for cageContainment. */
    int raySamples = 512;
    int pointSamples = 20000;
};

/**
 * Lints prepared inputs. backend, prepared with the same inputs, traces the highRange rays; null skips that check (as
 * do inputs built without highs, which also skip cageContainment).
 */
LILA_SCENE_API LintReport lintInputs(const BakeInputs& inputs, const ProjectSettings& project, const Backend* backend, const LintOptions& options = {});

/** The checks that need the scene meshes (triangulation, cageTopology, previewMesh), appended to r. Main thread. */
LILA_SCENE_API void lintScene(scene::Graph& g, const BakeInputs& inputs, const ProjectSettings& project, LintReport& r,
                              const LintOptions& options = {});

/** lintInputs plus the checks that need the scene meshes (triangulation, cageTopology, previewMesh). */
LILA_SCENE_API LintReport lint(scene::Graph& g, const BakeInputs& inputs, const ProjectSettings& project, const Backend* backend,
                               const LintOptions& options = {});
} // namespace lila::bake
