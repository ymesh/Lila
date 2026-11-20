#pragma once

#include <lila/Mesh.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

/**
 * Edge decals and edge chipping, the geometry half: which edges are sharp, how they join into runs, strips and swept
 * pieces laid along those runs, and chips cut out of convex edges. Everything here is plain mesh in, mesh out, so the
 * live nodes in Edges.cpp and the tests share it.
 */
namespace weathering::edges
{
using lila::geo::Index;
using lila::geo::Mesh;

enum EdgeKind : unsigned
{
    kConvex = 1,
    kConcave = 2,
};

/** Edge bool Blockwork sets where a block lies on another: never decorated or chipped. */
inline constexpr const char* kContactEdgeAttr = "blockworkContact";
/** Face bool or int a mesh may carry for faces nobody sees. */
inline constexpr const char* kHiddenFaceAttr = "hidden";

struct SharpEdge
{
    Index edge = lila::geo::kInvalid;
    EdgeKind kind = kConvex;
    /** Degrees between the two face normals. */
    double angle = 0.0;
};

/**
 * Manifold edges whose faces meet at more than angle degrees, of the kinds asked for. Skips edges touching a tool face
 * (a material named like nodraw, clip or skip), a hidden face, and edges marked kContactEdgeAttr.
 */
std::vector<SharpEdge> sharpEdges(const Mesh& m, double angle, unsigned kinds);

/** True when a face's material is a Blockwork tool material or the face is marked hidden. */
bool skippedFace(const Mesh& m, Index f);

/** A run of sharp edges of one kind, in order: points[i] to points[i + 1] is edges[i]; closed runs repeat no point. */
struct Chain
{
    std::vector<Index> points;
    std::vector<Index> edges;
    EdgeKind kind = kConvex;
    bool closed = false;
};

/** Joins sharp edges into runs that go on through a point while they turn by less than splitAngle degrees. */
std::vector<Chain> chainEdges(const Mesh& m, const std::vector<SharpEdge>& edges, double splitAngle);

/** One strip of a sheet the decals pick from: its uv rect and world size, and which edge kinds it suits. */
struct DecalTrim
{
    std::string name;
    glm::dvec2 uvMin{0.0};
    glm::dvec2 uvMax{1.0};
    /** cm along the strip, cm across it. */
    glm::dvec2 size{100.0, 10.0};
    /** The trim runs along V on the sheet (taller than wide). */
    bool alongV = false;
    unsigned kinds = kConvex | kConcave;
};

struct StripOptions
{
    /** cm across the edge, half on each face. */
    double width = 8.0;
    /** cm off the surface. */
    double offset = 0.05;
    /** cm of strip per decal piece; 0 takes it from the trim's aspect at this width. */
    double segment = 0.0;
    /** Share of pieces placed, 0 to 1; the rest leave gaps. */
    double coverage = 0.8;
    /** cm over which a run narrows to nothing at its ends. */
    double fade = 6.0;
    /** Each piece's width varies by up to this fraction. */
    double jitter = 0.25;
    uint32_t seed = 1;
    /** Slot name for the strip faces. */
    std::string material;
};

struct StripStats
{
    size_t chains = 0;
    size_t pieces = 0;
    size_t convex = 0;
    size_t concave = 0;
};

/**
 * Decal strips over the chains of m (in the space the strips are wanted in): each run cut into pieces, each piece mapped
 * onto a trim picked at random from those suiting its edge kind, the crest on the edge and half the width on each face.
 * UV0 maps the trims; colour alpha is 1 inside a run and fades with the width at its ends. Empty when no trim suits.
 */
Mesh decalStrips(const Mesh& m, const std::vector<Chain>& chains, const std::vector<DecalTrim>& trims, const StripOptions& o,
                 StripStats* stats = nullptr);

/** A mesh swept along edges: it runs along +X, +Y points out of the surface, Z goes across; y = z = 0 sits on the edge. */
struct SweepPiece
{
    Mesh mesh;
    double weight = 1.0;
};

struct SweepOptions
{
    /** Scale of the pieces across the edge (Y and Z); along the edge each piece stretches to fit its stretch of run. */
    double scale = 1.0;
    /** cm off the edge, out of the surface. */
    double offset = 0.0;
    /** Share of pieces placed, 0 to 1. */
    double coverage = 1.0;
    uint32_t seed = 1;
};

/** User pieces bent along the chains, one after another, picked by weight. */
Mesh sweepPieces(const Mesh& m, const std::vector<Chain>& chains, const std::vector<SweepPiece>& pieces, const SweepOptions& o,
                 StripStats* stats = nullptr);

struct ChipOptions
{
    /** Degrees faces must meet at for an edge to chip. */
    double angle = 40.0;
    /** cm along the edge per chip slot. */
    double size = 8.0;
    /** cm the deepest chip reaches into the solid from the edge. */
    double depth = 2.5;
    /** Share of slots chipped, 0 to 1. */
    double amount = 0.5;
    /** 0 noisy realistic breaks, 1 clean faceted chunks. */
    double shape = 0.0;
    /** cm bevel on the chipped edges first; 0 for none. */
    double bevel = 0.0;
    /** cm per UV unit on the cut faces, projected in world space. */
    double uvScale = 100.0;
    /** Slot for the cut faces; empty uses the slot of the face each cut breaks. */
    std::string material;
    uint32_t seed = 1;
    /** Most chips cut in one mesh. */
    size_t maxChips = 4000;
};

struct ChipResult
{
    Mesh mesh;
    size_t chips = 0;
    size_t edges = 0;
    std::string warning;
};

/**
 * Chips convex edges: cutters (user meshes when given, else generated) placed along each sharp convex edge and taken out
 * with a boolean, so the mesh stays closed. No cut reaches more than depth from the edge. Cut faces get uv0 projected in
 * world space (toWorld) along the axis nearest their normal.
 */
ChipResult chipEdges(const Mesh& in, const glm::dmat4& toWorld, const ChipOptions& o, const std::vector<Mesh>& cutters = {});

/** The generated cutter for one chip: centred on the origin, X along the edge, its points within radius of the X axis. */
Mesh chipCutter(double length, double radius, double shape, uint32_t seed, uint32_t index);
} // namespace weathering::edges
