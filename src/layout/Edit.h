#pragma once

#include "core/Export.h"

#include "layout/Fit.h"
#include "layout/Layout.h"
#include "layout/Path.h"
#include "layout/Surface.h"

#include <span>
#include <string>
#include <vector>

/**
 * Editing a layout the way the Layout Pencil does: every edit takes points on the surface (mesh space) and leaves the
 * graph tidy (no unused junctions; two runs meeting smoothly at a junction with nothing else become one run).
 */
namespace lila::layout
{
struct EditContext
{
    const Surface& surface;
    /** Join and pick distance: a few screen pixels in mesh units. */
    double tolerance = 0.1;
    /** Sample spacing for run paths; 0 picks defaultSpacing. */
    double spacing = 0.0;
};

struct StrokeOptions
{
    double cleanup = 0.5;
    double cornerAngle = 50.0;
    bool straight = false;
    bool straightenLast = false;
    /** Line set for new runs; 0 is the layout's active set. */
    uint32_t set = 0;
    /** How far past a line a stroke may end and still be trimmed back to it; 0 is 3 tolerances. */
    double overshoot = 0.0;
};

struct EditResult
{
    bool changed = false;
    /** Runs made or reshaped, for the tool to select. */
    std::vector<uint32_t> runs;
    std::vector<uint32_t> junctions;
    /** Something the user should know (a stroke too short, a run that could not be split). */
    std::string note;
};

/**
 * Adds a drawn stroke. It is cleaned into straight and curved pieces with corner junctions. Its ends join a junction
 * or line within tolerance (a line is split into a T); an end that overshoots a line is trimmed back to it; a stroke
 * that ends where it began closes. Lines it crosses are split at crossing junctions. A stroke that starts on a line
 * heading along it redraws that line from there instead (overdraw), and one that ends on a line arriving along it
 * redraws its end.
 */
LILA_SCENE_API EditResult addStroke(PanelLayout& layout, const EditContext& ctx, std::span<const glm::dvec3> stroke, const StrokeOptions& opts = {});
/** Removes every run the stroke passes within tolerance of. */
LILA_SCENE_API EditResult eraseStroke(PanelLayout& layout, const EditContext& ctx, std::span<const glm::dvec3> stroke);
/** Relaxes the control points of runs within radius of the stroke, by strength 0 to 1. Junctions stay. */
LILA_SCENE_API EditResult smoothStroke(PanelLayout& layout, const EditContext& ctx, std::span<const glm::dvec3> stroke, double radius, double strength);
LILA_SCENE_API EditResult moveJunction(PanelLayout& layout, const EditContext& ctx, uint32_t junction, const glm::dvec3& to);
/** Drags the run at grab to `to`: the nearest control moves, or a new one is added where there is none near. */
LILA_SCENE_API EditResult bendRun(PanelLayout& layout, const EditContext& ctx, uint32_t run, const glm::dvec3& grab, const glm::dvec3& to);
/** Splits a run at the point nearest `at` with a new junction (or `junction` when given). */
LILA_SCENE_API EditResult splitRun(PanelLayout& layout, const EditContext& ctx, uint32_t run, const glm::dvec3& at, uint32_t junction = 0);
LILA_SCENE_API EditResult toggleCorner(PanelLayout& layout, uint32_t junction);
LILA_SCENE_API EditResult deleteRuns(PanelLayout& layout, const EditContext& ctx, std::span<const uint32_t> runs);
/** Removes junctions: one where two runs meet joins them, others take their runs with them. */
LILA_SCENE_API EditResult deleteJunctions(PanelLayout& layout, const EditContext& ctx, std::span<const uint32_t> junctions);
/** Parallel copies of runs `distance` to their side (left of the direction a to b, seen from outside the surface). */
LILA_SCENE_API EditResult offsetRuns(PanelLayout& layout, const EditContext& ctx, std::span<const uint32_t> runs, double distance);
/** Releases runs from their fit: dense controls following their current shape, marked loose. */
LILA_SCENE_API EditResult loosenRuns(PanelLayout& layout, const EditContext& ctx, std::span<const uint32_t> runs);
/** Refits loose runs (all, or the given ones) at a cleanup amount; they stop being loose. */
LILA_SCENE_API EditResult refitRuns(PanelLayout& layout, const EditContext& ctx, double cleanup, std::span<const uint32_t> runs = {});

/** Runs joined to `run` through junctions where exactly two runs meet, in order: the line a double-click picks. */
LILA_SCENE_API std::vector<uint32_t> chainOf(const PanelLayout& layout, uint32_t run);

/** Merges smooth two-run junctions, drops empty runs and unused junctions. Edits call it; exposed for loaders. */
LILA_SCENE_API void tidy(PanelLayout& layout, const EditContext& ctx);
} // namespace lila::layout
