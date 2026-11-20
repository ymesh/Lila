#pragma once

#include "GenCommon.h"

/**
 * Lattice trusses along curves: chords running along the curve, rings at each level and bracing on every face between
 * levels, each member a square beam, unioned into one closed mesh. Radio masts, pylons, crane jibs, bridge girders,
 * stylized towers. The Curve to Truss geometry node and the lattice tower placeholders both build with this.
 */
namespace weathering::truss
{
using gen::Bounds;
using gen::Index;
using gen::Mesh;

enum class Bracing
{
    X,      // both diagonals on every face
    Zigzag, // one diagonal, alternating
    None,
};
const char* bracingName(Bracing b);
bool parseBracing(const std::string& s, Bracing* out);

struct Options
{
    int chords = 4;          // 3 or more
    double width = 60.0;     // cm, side of the cross-section at the curve's start
    double endWidth = -1.0;  // cm at its end; negative keeps width (a taper otherwise)
    double member = 4.0;     // cm, side of each member's square section
    double segment = 80.0;   // cm between levels
    Bracing bracing = Bracing::X;
    bool rings = true;       // members round each level
    bool unite = true;       // one closed mesh (otherwise overlapping beams)
};

struct Result
{
    Mesh mesh;
    int members = 0;
    std::string warning;
};

/** A truss along each polyline (world or local, Y is the up reference). */
Result build(std::span<const std::vector<glm::dvec3>> lines, const Options& o);
} // namespace weathering::truss
