#pragma once

#include <lila/PanelLayout.h>

#include <string>
#include <vector>

/** Fasteners: rivets, bolts and screws set along a Panel Layout's lines or into its panel corners. */
namespace plating
{
enum class FastenerKind
{
    Rivet,
    HexBolt,
    Screw,
    Washer,
};

enum class Placement
{
    /** On each line. */
    OnLines,
    /** Beside each line, on both sides: plate borders. */
    BothSides,
    /** In each panel corner, where lines meet. */
    Corners,
};

struct FastenerOptions
{
    FastenerKind kind = FastenerKind::Rivet;
    Placement placement = Placement::BothSides;
    /** Head diameter, cm. */
    double size = 0.6;
    /** Head height, cm. */
    double height = 0.25;
    /** Distance between fasteners along a line, cm. */
    double spacing = 3.0;
    /** Distance from the line (BothSides) or from the corner (Corners), cm. */
    double inset = 0.8;
    /** Only lines in this set; empty for every line. */
    std::string set;
    /** How far the head sinks into the surface, cm. */
    double sink = 0.05;
    /** Depth of a round recess pressed into the surface under each head, cm; 0 for none. */
    double recess = 0.0;
};

/** Where one fastener goes, in the layout mesh's space. */
struct FastenerSpot
{
    glm::dvec3 position{0.0};
    glm::dvec3 normal{0.0, 1.0, 0.0};
    glm::dvec3 tangent{1.0, 0.0, 0.0};
};

/** Where the fasteners go on the layout, before they are seated on a surface. */
std::vector<FastenerSpot> fastenerSpots(const lila::layout::Resolved& layout, const FastenerOptions& opts);

/** One fastener's mesh at the origin, standing on +Y, its slot or flats along +X. */
lila::geo::Mesh fastenerMesh(const FastenerOptions& opts);

/**
 * Each fastener's frame in the layout mesh's space, seated on `surface`: columns x (along the line), y (the surface
 * normal), z, and the position its base sits at. The spot is dropped along its normal onto the surface, so fasteners sit
 * on plate tops rather than inside them.
 */
std::vector<glm::dmat4> fastenerPlacements(const lila::layout::Resolved& layout, const lila::geo::Mesh& surface, const FastenerOptions& opts);

struct FastenersResult
{
    /** The fasteners' heads, merged. */
    lila::geo::Mesh mesh;
    /** The surface with a recess pressed under each head; empty when there are no recesses. */
    lila::geo::Mesh surface;
    int count = 0;
    std::vector<std::string> warnings;
};

/** The fasteners seated on `surface` (the mesh under them, plates included), with their recesses when asked. */
FastenersResult makeFasteners(const lila::layout::Resolved& layout, const lila::geo::Mesh& surface, const FastenerOptions& opts);
} // namespace plating
