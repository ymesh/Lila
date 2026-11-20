#pragma once

#include "GenCommon.h"

/**
 * Windows: openings found in walls (a Blockwork carve, a Masonry opening, a hole in any wall mesh), then a frame, sill,
 * glass, boards and an interior fitted into each.
 *
 * Openings are found per wall slab (faces facing about the same horizontal way, front and back together): the slab's
 * faces are drawn into a grid on the wall plane, and empty areas closed in on the left, right and top are openings
 * (doors when they reach the bottom). Their outline is traced from the grid and snapped to the wall's own vertices, so
 * a box carve gives its exact rectangle and an arch a polygon.
 *
 * Pieces are fitted by their bounds. Frame members lie along X (scaled to each side), Y their face width, Z their depth.
 * Sills lie along X, Y up, Z out of the wall. Glass and boards: X across, Y up (boards: Y their width), thin along Z.
 */
namespace weathering::window
{
using gen::Bounds;
using gen::Index;
using gen::Mesh;

struct Opening
{
    /** World = base + u * U + v * V + w * N; w = 0 is the outer face, w = -depth the inner one. U = V x N. */
    glm::dvec3 base{0.0}, U{1, 0, 0}, V{0, 1, 0}, N{0, 0, 1};
    double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
    double depth = 0.0;
    std::vector<glm::dvec2> outline; // (u, v), counter-clockwise seen from outside
    bool door = false;
    bool rectangular = true;
    glm::dvec3 at(double u, double v, double w) const { return base + U * u + V * v + N * w; }
};

struct FindOptions
{
    double minSize = 15.0;     // cm, narrower or shorter areas are not openings
    double maxThickness = 150.0; // cm, faces further apart along a wall's normal are different walls
    double cell = 0.0;         // grid cell, cm; 0 picks one from the wall's size
    bool flip = false;         // outside is the other side
};

/** The openings in a world-space wall mesh. hostCenter decides which side is outside (away from it). */
std::vector<Opening> findOpenings(const Mesh& wall, const glm::dvec3& hostCenter, const FindOptions& o);

enum class Glass
{
    Intact,
    Cracked,
    Broken,
    Boarded,
    None,
    Mixed,
};
const char* glassName(Glass g);
bool parseGlass(const std::string& s, Glass* out);

enum class Interior
{
    Room,
    Card,
    Mesh,
    None,
};
bool parseInterior(const std::string& s, Interior* out);

struct Options
{
    double frameInset = 4.0;   // cm in from the outer face to the frame's front
    double frameScale = 1.0;   // frame and sill cross-section
    int columns = 1, rows = 1; // panes across and up
    bool sill = true;
    double sillProject = 4.0;  // cm the sill stands out of the wall face
    Glass glass = Glass::Intact;
    double damage = 0.3;       // mixed glass, board gaps and tilt
    double neatness = 0.3;     // 1: boards straight and even
    double boardOverhang = 8.0; // cm boards reach past the opening each side
    Interior interior = Interior::Room;
    double interiorDepth = 300.0; // cm behind the wall
    bool doors = false;        // fill openings that reach the bottom too
    uint32_t seed = 1;
};

/** Slots, in the order a layout's references are numbered. */
enum Slot
{
    Frame,
    Sill,
    Pane,
    Cracked,
    Broken,
    Board,
    InteriorPiece,
    RoomMesh, // generated: a unit room box, scaled per window
    CardMesh, // generated: a unit card, scaled per window
    SlotCount,
};

struct Pieces
{
    std::vector<Bounds> slot[SlotCount];
    std::vector<double> weights[SlotCount];
};

struct Placed
{
    glm::dmat4 xf{1.0};
    int slot = 0;
    int piece = 0;
    int window = 0;
    float random = 0.0f;
};

struct Result
{
    std::vector<Opening> openings; // the ones filled
    std::vector<Placed> placed;
    int doorsSkipped = 0;
    int panes = 0, cracked = 0, broken = 0, boarded = 0;
};

Result layout(std::span<const Opening> openings, const Pieces& pieces, const Options& o);

/** The generated room: a box from (-0.5, 0, -1) to (0.5, 1, 0) seen from inside (faces inward), with uvs. */
Mesh unitRoom(const std::string& material);
/** The generated card: a unit quad facing +Z, centred, with uvs. */
Mesh unitCard(const std::string& material);
} // namespace weathering::window
