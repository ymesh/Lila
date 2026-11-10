#pragma once

#include <lila/PanelLayout.h>

#include <string>
#include <vector>

/** Surface patterns: hex, squares, diamond plate, slots, dots and grilles pressed into a Panel Layout's panels. */
namespace plating
{
enum class PatternKind
{
    Hex,
    Squares,
    DiamondPlate,
    Slots,
    Dots,
    Grille,
};

enum class Projection
{
    /** Each panel gets its own axes from its shape, so rows follow the panel. */
    Panel,
    /** The mesh's own axes, from the side each point faces. */
    Triplanar,
};

struct PatternOptions
{
    PatternKind kind = PatternKind::Hex;
    Projection projection = Projection::Panel;
    /** Size of one cell, cm. */
    double scale = 2.0;
    /** How far the pattern presses in, cm; negative raises it. */
    double depth = 0.1;
    /** Width of the pattern's lines or the size of its features, as a fraction of a cell. */
    double width = 0.12;
    /** Turn of the pattern within each panel, degrees. */
    double rotation = 0.0;
    /** The pattern fades out over this distance from the panel lines, cm. */
    double margin = 0.5;
    /** Only panels whose own values say "pattern": true; otherwise every panel. */
    bool pickedOnly = false;
};

/** How far the pattern presses in at a point in cell coordinates: 0 nothing, 1 full depth. */
double patternHeight(PatternKind kind, double u, double v, double width);

/** One panel's pattern axes: an origin and two directions across the panel. */
struct PanelAxes
{
    glm::dvec3 origin{0.0};
    glm::dvec3 u{1.0, 0.0, 0.0};
    glm::dvec3 v{0.0, 0.0, 1.0};
};

/** Axes for every panel of the layout, from the spread of its triangles: u along its longest direction. */
std::vector<PanelAxes> panelAxes(const lila::layout::Resolved& layout);

struct PatternResult
{
    lila::geo::Mesh mesh;
    size_t moved = 0;
    std::vector<std::string> warnings;
};

/** Presses the pattern into a copy of `mesh` (the layout's surface or what was built on it, such as plates). */
PatternResult applyPattern(const lila::layout::Resolved& layout, const lila::geo::Mesh& mesh, const PatternOptions& opts);
} // namespace plating
