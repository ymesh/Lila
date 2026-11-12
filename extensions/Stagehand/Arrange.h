#pragma once

#include <RigidScene.h>

#include <lila/Mesh.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

/**
 * Arrangements: where copies of a few items go, before or without physics. Rows along a line (books on a shelf),
 * stacks, piles dropped into a footprint and scatters over a surface, then a settle that lets them rest and records
 * how they touched. Plain functions over bounds and transforms; the geometry nodes and commands wrap them.
 *
 * Items stand Y up. A row lays items with their local X along the line, so a book's thickness runs along the shelf.
 */
namespace stagehand::arrange
{
/** An item's bounds in its own space. */
struct Item
{
    glm::dvec3 lo{-0.5}, hi{0.5};
    glm::dvec3 size() const { return hi - lo; }
};

enum class Pose : int
{
    Upright = 0,
    Leaning = 1,
    Lying = 2,
};

struct Placed
{
    glm::dmat4 transform{1.0};
    int item = 0;
    Pose pose = Pose::Upright;
    /** 0 to 1, per placement, for variation downstream (colour, wear). */
    float random = 0.0f;
};

struct RowOptions
{
    double gap = 0.3;          // cm between items
    double gapJitter = 0.5;    // 0 to 1 of gap, either way
    double scaleMin = 0.9, scaleMax = 1.1; // uniform scale range per item
    double leanChance = 0.08;  // an item leans on its neighbour
    double leanAngle = 14.0;   // degrees, at most
    double lyingChance = 0.04; // a small stack of items lying flat starts here
    int lyingMax = 4;          // items in such a stack
    double fill = 1.0;         // fraction of the line used, from its start
    double inset = 0.0;        // cm kept clear at both ends
    double depthJitter = 0.4;  // cm, back and forth across the line
    uint32_t seed = 1;
};

/** Items packed along a polyline (Y up), their bottoms on it. */
std::vector<Placed> row(std::span<const glm::dvec3> line, std::span<const Item> items, const RowOptions& o);

struct PathOptions
{
    double spacing = 100.0;     // cm between items along the line
    double spacingJitter = 0.3; // 0 to 1 of spacing, either way
    double offset = 0.0;        // cm to the line's right (negative: left)
    double spread = 0.0;        // cm of random sideways wander, either way
    double turn = 15.0;         // degrees of random turn about up, either way
    bool along = true;          // items' X along the line; false turns each at random all the way round
    double scaleMin = 0.9, scaleMax = 1.1;
    uint32_t seed = 1;
};

/** Items spaced along a polyline (litter along a kerb, posts along a fence), their bottoms on it. */
std::vector<Placed> path(std::span<const glm::dvec3> line, std::span<const Item> items, const PathOptions& o);

struct DebrisOptions
{
    double amount = 6.0;      // chips per piece of median size, on average; bigger pieces shed more
    double size = 0.12;       // chip size as a fraction of its piece's size
    double minSize = 2.0, maxSize = 40.0; // cm
    double spread = 1.0;      // how far out they land, in piece radii beyond the piece
    uint32_t seed = 1;
};

/**
 * Chips around and beside broken pieces (fracture pieces, a ruin's stones), sized from the piece they came off and
 * stacked loosely above the ground next to it, ready to drop: settle() lands them against the pieces. pieces holds each
 * piece's world bounds (lo, hi).
 */
std::vector<Placed> debris(std::span<const std::pair<glm::dvec3, glm::dvec3>> pieces, std::span<const Item> items, const DebrisOptions& o);

struct StackOptions
{
    int count = 6;
    double offset = 1.0;      // cm of sideways wander per level
    double turn = 6.0;        // degrees of turn per level
    double scaleMin = 0.95, scaleMax = 1.05;
    uint32_t seed = 1;
};

/** Items stacked straight up from a point, each on the one below. */
std::vector<Placed> stack(const glm::dvec3& base, std::span<const Item> items, const StackOptions& o);

struct PileOptions
{
    int count = 30;
    double radius = 40.0;     // cm footprint
    double gap = 1.0;         // cm between items as they start, so nothing starts inside another
    double scaleMin = 0.85, scaleMax = 1.15;
    uint32_t seed = 1;
};

/** Items in tumbled poses in layers above a footprint, ready to drop: settle() turns them into a heap. */
std::vector<Placed> pile(const glm::dvec3& center, std::span<const Item> items, const PileOptions& o);

struct ScatterOptions
{
    int maxCount = 200;
    double density = 4.0;     // per square metre
    double spacing = 1.0;     // times each item's footprint radius kept clear
    double slopeMax = 35.0;   // degrees from up
    double align = 0.0;       // 0 upright, 1 along the surface normal
    double turn = 180.0;      // degrees of random turn about up, either way
    double scaleMin = 0.8, scaleMax = 1.2;
    /** A point float attribute (0 to 1) on the surface that scales the density; empty for none. */
    std::string mask;
    /** World boxes (lo, hi) nothing is placed in or under: furniture, walls, other props. */
    std::vector<std::pair<glm::dvec3, glm::dvec3>> avoid;
    uint32_t seed = 1;
};

/** Items over a mesh's faces, no two closer than their footprints, on slopes gentle enough. */
std::vector<Placed> scatter(const lila::geo::Mesh& surface, std::span<const Item> items, const ScatterOptions& o);

struct SettleOptions
{
    double seconds = 10.0;
    bool ground = false;
    double groundHeight = 0.0;
    glm::dvec3 gravity{0.0, -981.0, 0.0};
    double density = 600.0;
    double friction = 0.6;
    double bounce = 0.05;
    double rounding = 0.2; // as a physics body's default, so dressCheck sees the same shapes
};

/** Where a settle left each placement, and how they touched on the way. */
struct Settled
{
    std::vector<glm::dmat4> transforms;
    std::vector<uint8_t> atRest;
    /** Hardest hit per placement, kg cm/s. */
    std::vector<float> impact;
    /** Body index of each contact refers to placements; colliders are -2, the ground -1. */
    std::vector<lila::physics::Contact> contacts;
    std::vector<double> masses;
    double seconds = 0.0;
};

/**
 * Lets placements fall and rest against the colliders (triangle meshes in world space). itemMeshes holds each item's
 * mesh at scale 1; each placement's scale is applied to its collision shape.
 */
Settled settle(std::span<const glm::dmat4> transforms, std::span<const int> items, std::span<const lila::geo::Mesh* const> itemMeshes,
               std::span<const lila::geo::Mesh* const> colliders, const SettleOptions& o);

/** Bounds of a mesh's valid points. */
Item itemOf(const lila::geo::Mesh& m);
} // namespace stagehand::arrange
