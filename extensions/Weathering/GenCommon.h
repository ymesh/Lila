#pragma once

#include <lila/Mesh.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

/**
 * Shared geometry for the generators (roofs, windows, wires, rooftops, trusses): fitting a piece into a box by its bounds,
 * and the small procedural shapes placeholders are built from. Pure functions over geo::Mesh, so tests compile them in.
 *
 * Fitting: a piece is any mesh. fitPiece takes its bounds to a box centred on the origin; place() then turns, scales and
 * moves that box to where the layout wants it. A chunky toon tile and a scanned one go through the same steps.
 */
namespace weathering::gen
{
using lila::geo::Bounds;
using lila::geo::Index;
using lila::geo::Mesh;

/** Bounds of a mesh's valid points; an empty mesh gives a 1 cm cube round the origin. */
Bounds boundsOf(const Mesh& m);

/** How a piece's bounds map onto a centred box: toBox moves its bounds' centre to the origin (and turns it), size is the box. */
struct Fit
{
    glm::dmat4 toBox{1.0};
    glm::dvec3 size{1.0};
};

/**
 * The piece's bounds as a centred box. lengthAlongZ turns the piece a quarter round Y when its X side is longer than its
 * Z side, so the longer horizontal side always runs along Z; lengthAlongX does the opposite. Never mirrors.
 */
Fit fitPiece(const Bounds& b, bool lengthAlongZ = false, bool lengthAlongX = false);

/** A fitted piece placed: its box centred at `center`, box X/Y/Z along ax/ay/az (a right-handed frame), scaled per axis. */
glm::dmat4 place(const Fit& f, const glm::dvec3& center, const glm::dvec3& ax, const glm::dvec3& ay, const glm::dvec3& az,
                 const glm::dvec3& scale = glm::dvec3(1.0));

/** Rotation by `degrees` about a unit axis through a point. */
glm::dmat4 rotateAbout(const glm::dvec3& point, const glm::dvec3& axis, double degrees);

/** Unit vector, or the fallback for a zero one. */
glm::dvec3 unit(const glm::dvec3& v, const glm::dvec3& fallback = glm::dvec3(0, 1, 0));

/** Any unit vector at right angles to a unit one. */
glm::dvec3 perpendicular(const glm::dvec3& v);

// Building blocks for placeholders and generated geometry. Each appends closed shells with outward faces and uv0.

/** An axis-aligned box from lo to hi, through xf. */
void addBox(Mesh& m, const glm::dvec3& lo, const glm::dvec3& hi, const glm::dmat4& xf = glm::dmat4(1.0));

/** A closed polygon in XY (counter-clockwise), extruded along Z from z0 to z1 and capped, through xf. */
void addExtrusion(Mesh& m, std::span<const glm::dvec2> profile, double z0, double z1, const glm::dmat4& xf = glm::dmat4(1.0));

/** A square beam of side `size` from a to b, reaching `extend` past both ends. */
void addBeam(Mesh& m, const glm::dvec3& a, const glm::dvec3& b, double size, double extend = 0.0);

/** A cylinder along Y from y0 to y1, `sides` around, capped, through xf. */
void addCylinder(Mesh& m, double radius, double y0, double y1, int sides, const glm::dmat4& xf = glm::dmat4(1.0));

/** A quad with uvs 0 to 1 (a card), corners counter-clockwise as seen from the side it faces. */
void addQuad(Mesh& m, const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c, const glm::dvec3& d);

/** Gives every face of m the material slot `name` (the face int "material" and materialNames). */
void setMaterial(Mesh& m, const std::string& name);

/** Gives uv0 to the corners of faces that have none, projected on each face's main axis, scale cm per uv unit. */
void boxUvs(Mesh& m, double scale = 100.0);

/** Point in a simple polygon (2D, either winding). */
bool insidePolygon(std::span<const glm::dvec2> poly, const glm::dvec2& p);

/** True when every edge of m has two faces, and the faces form one connected piece. */
bool closedAndConnected(const Mesh& m);

/** Comma separated numbers ("1,3,1"), empty for none. */
std::vector<double> parseNumbers(const std::string& s);
std::string joinNumbers(std::span<const double> v);

/** A deterministic hash of a seed and counters to [0, 1). */
double hash01(uint32_t seed, uint32_t a, uint32_t b = 0, uint32_t c = 0);

/** A weighted pick from weights (missing or empty ones are 1): u in [0, 1). */
int pickWeighted(std::span<const double> weights, int count, double u);
} // namespace weathering::gen
