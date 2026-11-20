#pragma once

#include <lila/Mesh.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

/**
 * The geometric half of the live grunge masks: per face corner values 0 to 1 found from the mesh and the objects around
 * it, with no UVs and no bake. The look (level, softness, bands, colours) is the material's, per pixel (render::GrungeLook).
 */
namespace weathering::masks
{
enum Mask
{
    kEdge,
    kCavity,
    kAo,
    kDust,
    kStreak,
    kFade,
    kMaskCount,
};

/** The masks' attribute names, render::kGrungeAttrs order. */
inline constexpr std::array<const char*, kMaskCount> kMaskNames = {"edge", "cavity", "ao", "dust", "streak", "fade"};

struct Settings
{
    /** Wear runs this far from a convex edge, cm. */
    double edgeWidth = 4.0;
    /** Edges whose faces turn more than this, degrees, are convex (wear) or concave (dirt) edges. */
    double edgeAngle = 30.0;
    /** Dirt runs this far from a concave edge or a touching object, cm. */
    double cavityWidth = 10.0;
    /** Occlusion rays reach this far, cm. */
    double aoRadius = 80.0;
    int aoRays = 16;
    /** Dust settles on surfaces open to the sky this far up, cm. */
    double dustCover = 1000.0;
    /** Streaks run this far down from a sill or an overhang, cm. */
    double streakLength = 150.0;
    /** Streaks reach this far past the ends of their source edge, cm. */
    double streakSpread = 5.0;
    /** A sill counts as an opening's when something covers it within this height, cm. */
    double coverHeight = 400.0;
    /** Sides fade this high above the ground or the object below, cm. */
    double fadeHeight = 60.0;
    /** The ground plane's height on the up axis, cm, when groundPlane is on. */
    double groundHeight = 0.0;
    bool groundPlane = true;
    glm::dvec3 up{0.0, 1.0, 0.0};
    uint32_t seed = 0;
};

/** A nearby object: its evaluated mesh and world matrix. */
struct Neighbour
{
    const lila::geo::Mesh* mesh = nullptr;
    glm::dmat4 world{1.0};
};

/** Per mask, one value per corner (corner capacity; dead corners 0). */
struct Result
{
    std::array<std::vector<float>, kMaskCount> corners;
    /**
     * Per corner, signed distances in cm (world) from the corner to up to four lines of its face's sharp convex or
     * boundary edges, kFarEdge for unused slots. Each is linear over a planar face, so interpolated per pixel and taken
     * the min of, it is the distance to the nearest sharp edge of the face: edge wear that holds on an 8-vertex block.
     */
    std::vector<glm::vec4> edgeDistance;
    /**
     * Per corner, signed distances in cm to up to four lines where its face meets something: a concave edge, the ground
     * plane, or a neighbour's surface. Per pixel, cavity dirt and occlusion fall off from the nearest.
     */
    std::vector<glm::vec4> contactDistance;
    /** Per corner, up to two streak sources over its face: (cm below the source, position along it 0 to 1) pairs. */
    std::vector<glm::vec4> streakLines;
    /** Per corner of a side face, cm above the ground or whatever is straight below; kFarEdge elsewhere. */
    std::vector<float> height;
    /** Distinct (point, normal) samples evaluated. */
    size_t samples = 0;
};

/** The six masks over a mesh placed at world among its neighbours. Deterministic for a seed. */
Result compute(const lila::geo::Mesh& mesh, const glm::dmat4& world, std::span<const Neighbour> neighbours, const Settings& s);

/** An edge-distance slot with no sharp edge in it, cm. */
inline constexpr float kFarEdge = 10000.0f;

/**
 * Writes a result as float corner attributes named render::kGrungeAttrs ("grungeEdge", ...), the edge and contact
 * distances and streak lines as vec4 corner attributes and the height as a float one (render::kGrunge*Attr names).
 */
void writeAttributes(lila::geo::Mesh& mesh, const Result& r);
} // namespace weathering::masks
