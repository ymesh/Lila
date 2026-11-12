#pragma once

// Garment core shared by the solver, meshing, blocks and UI. Plain data, no Lila SDK types,
// so the solver and its tests build without the scene.

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tailor
{

using Vec2 = std::array<float, 2>;
using Vec3 = std::array<float, 3>;

/** Anisotropic fabric. Stiffness in N/m (membrane) and N*m (bending), density in kg/m^2, lengths in metres. */
struct Fabric
{
    std::string name = "Cotton";
    float stretchWarp = 4000.f;
    float stretchWeft = 3000.f;
    float shear = 300.f;
    float bendWarp = 2e-5f;
    float bendWeft = 2e-5f;
    float bendBias = 1.5e-5f;
    float strainLimit = 0.1f;      // max stretch ratio above rest, hard constraint; 0 = off
    float density = 0.15f;
    float thickness = 0.001f;
    float friction = 0.3f;
    float damping = 0.01f;
    float shrinkWarp = 1.f;        // rest scale multipliers
    float shrinkWeft = 1.f;
};

/** Built-in presets: Cotton, Linen, Denim, Canvas, Wool, Leather, Suede, Silk, Chiffon, Rubber, Burlap, Heavy (chainmail look), Fleece, Plush, Banner, Spandex, Neoprene, Nylon, Twill. */
const std::vector<Fabric>& fabricPresets();
const Fabric* findFabric(const std::string& name);

enum class PanelState : uint8_t
{
    Active,
    Frozen,       // collides, does not move
    Deactivated,  // ignored by the sim
    Strengthened // stiffened while shaping
};

/** A painted patch of shrink (scale < 1) or stretch (> 1) on a panel's rest shape, fading out to its radius. */
struct ShrinkSpot
{
    Vec2 at{0.f, 0.f}; // pattern space, metres
    float radius = 0.05f;
    float scale = 0.9f;
};

/** A pattern piece: closed 2D outline (counter-clockwise, metres, pattern space) plus internal lines and holes. */
struct Panel
{
    std::string name;
    std::vector<Vec2> outline;                  // polyline, curves already sampled
    std::vector<std::vector<Vec2>> holes;
    std::vector<std::vector<Vec2>> internalLines; // fold or crease lines, kept as mesh edges
    float grainAngle = 0.f;                    // radians, warp direction in pattern space
    int fabric = 0;                            // index into Garment::fabrics
    int layer = 0;
    PanelState state = PanelState::Active;
    float particleDistance = 0.02f;            // target edge length for meshing

    // 3D placement: pattern-space point (0,0) lands at origin, pattern X along right, Y along up,
    // then the panel is wrapped around a cylinder of wrapRadius about the up axis (0 = flat).
    Vec3 origin{0.f, 0.f, 0.f};
    Vec3 right{1.f, 0.f, 0.f};
    Vec3 up{0.f, 1.f, 0.f};
    float wrapRadius = 0.f;

    std::vector<float> lineAngles; // fold angle per internal line, radians; NaN or missing keeps the placed angle
    std::vector<ShrinkSpot> shrinkSpots;
    std::vector<int> notches; // outline point indices; seams with the same number of notches on both sides match them
    int twin = -1;            // a mirrored panel whose outline follows every edit to this one
};

/** A run of outline from vertex index `from` to `to` (inclusive, walking forward, wrapping) on a panel. */
struct SeamSide
{
    int panel = -1;
    int from = 0;
    int to = 0;
};

/** Sews two runs together point to point by arc length. M:N chains are several sides per seam end. */
struct Seam
{
    std::vector<SeamSide> a;
    std::vector<SeamSide> b;
    bool reversed = false;  // walk b backwards
    float foldAngle = 0.f;  // rest dihedral across the seam, radians (0 = flat)
    float shrink = 1.f;     // rest length scale across the seam
};

/** Rest-shape scale at a pattern point: every shrink spot over it, each fading smoothly to 1 at its radius. */
inline float shrinkAt(const Panel& p, const Vec2& at)
{
    float scale = 1.f;
    for (const ShrinkSpot& s : p.shrinkSpots)
    {
        const float d = std::hypot(at[0] - s.at[0], at[1] - s.at[1]);
        if (s.radius <= 0.f || d >= s.radius)
            continue;
        const float t = 1.f - d / s.radius, w = t * t * (3.f - 2.f * t);
        scale *= 1.f + (s.scale - 1.f) * w;
    }
    return scale;
}

/**
 * A pattern point held where it was painted: panel, pattern position (metres) and target (garment space, metres).
 * Attached to the body, the target rides the body triangle under it, offset kept, so the pin follows the body as it
 * moves; bodyTris is the body's triangle count then, and a body with another count leaves the pin where it was.
 */
struct GarmentPin
{
    int panel = -1;
    Vec2 at{0.f, 0.f};
    Vec3 target{0.f, 0.f, 0.f};
    int bodyTri = -1;
    std::array<float, 3> bodyBary{1.f, 0.f, 0.f};
    Vec3 bodyLocal{0.f, 0.f, 0.f};
    int bodyTris = 0;
};

/** Two pattern points held together like a seam's pair: a pocket's edge on its host, a belt loop's ends, a cord in its channel. */
struct Tack
{
    int panelA = -1;
    Vec2 atA{0.f, 0.f};
    int panelB = -1;
    Vec2 atB{0.f, 0.f};
};

struct Garment
{
    std::vector<Panel> panels;
    std::vector<Seam> seams;
    std::vector<Fabric> fabrics{Fabric{}};
    std::vector<GarmentPin> pins;
    std::vector<Tack> tacks;
};

/** Triangle mesh built from a garment, the solver's input. */
struct ClothMesh
{
    std::vector<Vec3> rest3d;              // placed (initial) positions
    std::vector<Vec2> uv;                  // pattern-space positions, metres (the flat pattern)
    std::vector<std::array<int, 3>> tris;
    std::vector<int> triPanel;
    std::vector<int> vertPanel;
    std::vector<std::array<int, 2>> seamPairs; // vertices to be sewn together
    std::vector<float> seamRestLength;     // usually 0
    std::vector<int> seamOfPair;           // Garment::seams index of each pair, in sewing order along the seam; -1 for a tack
    std::vector<std::array<int, 2>> creaseEdges; // internal-line edges (keep sharp rest angle)
    std::vector<float> creaseAngles;             // per crease edge: its line's fold angle, NaN for the placed angle
};

/** A bending hinge across a folded seam: edge v[0]-v[1] on one side, wings v[2] and v[3] on either panel. */
struct SeamHinge
{
    std::array<int, 4> v{};
    std::array<int, 2> tris{}; // the wings' triangles, for the hinge's area
    float restAngle = 0.f;
    float bend = 0.f; // N*m, the stiffer of the two fabrics times kSeamStiffening
};

/** Hinges that hold every seam with a fold angle at that angle (a pressed hem, a crisp collar). Plain seams get none. */
std::vector<SeamHinge> seamHinges(const ClothMesh& mesh, const Garment& g);

/** An outline edge along a seam with shrink < 1, held to at most limit (metres): an elastic gather. */
struct SeamShrink
{
    int a = -1, b = -1;
    float limit = 0.f;
};

/** Both sides' edges of every seam whose shrink is below 1, at shrink times their rest length. */
std::vector<SeamShrink> seamShrinks(const ClothMesh& mesh, const Garment& g);

/**
 * A render mesh latched to the simulated cloth (APEX's "latch to nearest"): each render point keeps the nearest sim
 * triangle, the barycentric weights of its foot point there, and its offset in that triangle's local frame, so a thick or
 * separately modelled mesh follows the cloth exactly, thickness included.
 */
struct RenderBinding
{
    std::vector<int> tri; // -1 when unbound: local then holds the point itself
    std::vector<std::array<float, 3>> bary;
    std::vector<Vec3> local; // along the foot's tangent, bitangent and smoothed normal
};

/** Binds render points to the sim mesh as it is now (both in the same space). */
RenderBinding bindRenderMesh(const std::vector<Vec3>& simPositions, const std::vector<std::array<int, 3>>& simTris,
                             const std::vector<Vec3>& renderPoints);
/**
 * Binds rigid pieces (buttons, buckles, plates): every connected piece of the render mesh follows the one cloth point
 * nearest its middle, turning with the cloth there but never bending.
 */
RenderBinding bindRigidPieces(const std::vector<Vec3>& simPositions, const std::vector<std::array<int, 3>>& simTris,
                              const std::vector<Vec3>& renderPoints, const std::vector<std::array<int, 3>>& renderTris);
/** The render points for the sim mesh's current positions. */
std::vector<Vec3> deformRenderMesh(const RenderBinding& binding, const std::vector<Vec3>& simPositions,
                                   const std::vector<std::array<int, 3>>& simTris);

/** Where a pattern-space point of a panel lands in 3D as the panel is placed (before any drape). */
Vec3 placePatternPoint(const Panel& p, const Vec2& at);

/** Mesh every active/frozen panel at its particle distance, place it in 3D and resample seams to matching vertex pairs. */
ClothMesh buildClothMesh(const Garment& g);

/** Distance field collider. Negative inside. Sampled on a uniform grid. */
struct Sdf
{
    Vec3 origin{0.f, 0.f, 0.f};
    float cell = 0.01f;
    int nx = 0, ny = 0, nz = 0;
    std::vector<float> values;
    float sample(const Vec3& p, Vec3* gradient = nullptr) const;
};

/** The grid covers the mesh's bounds plus padding, cut to [clipLo, clipHi] when given; empty when they miss. */
Sdf sdfFromMesh(const std::vector<Vec3>& points, const std::vector<std::array<int, 3>>& tris, float cell, float padding,
                const Vec3* clipLo = nullptr, const Vec3* clipHi = nullptr);

struct SolverSettings
{
    float dt = 1.f / 60.f;
    int substeps = 2;
    int iterations = 10;
    Vec3 gravity{0.f, -9.81f, 0.f};
    float collisionThickness = 0.004f;
    bool selfCollision = true;
    float seamCloseRate = 0.25f; // fraction of remaining seam gap closed per step while sewing
    float floorY = -1e30f;       // a ground plane the cloth rests on, metres; far below means none
    uint32_t seed = 1;
};

struct Pin
{
    int vertex = -1;
    Vec3 target{};
};

/**
 * The garment's pins on the meshed cloth: each at the vertex of its panel nearest its pattern point. Given the body as
 * it stands now, attached pins aim where the body has carried them.
 */
std::vector<Pin> resolvePins(const ClothMesh& mesh, const Garment& g, const std::vector<Vec3>* bodyPoints = nullptr,
                             const std::vector<std::array<int, 3>>* bodyTris = nullptr);

/** Attaches every unattached pin to the body triangle nearest its target (body as it stands, garment space metres). */
void attachPins(Garment& g, const std::vector<Vec3>& bodyPoints, const std::vector<std::array<int, 3>>& bodyTris);

class ClothSolver
{
public:
    void reset(const ClothMesh& mesh, const Garment& g, const SolverSettings& s);
    void setColliders(std::vector<const Sdf*> colliders) { colliders_ = std::move(colliders); }
    void setPins(std::vector<Pin> pins) { pins_ = std::move(pins); }
    void step();
    const std::vector<Vec3>& positions() const { return x_; }
    std::vector<Vec3>& positionsMutable() { return x_; }
    const std::vector<Vec3>& velocities() const { return v_; }
    std::vector<Vec3>& velocitiesMutable() { return v_; }
    /** Per-vertex stretch ratio (current / rest), for the fit map. */
    std::vector<float> strainMap() const;

private:
    struct Impl;
    std::vector<Vec3> x_, v_, xPrev_;
    std::vector<const Sdf*> colliders_;
    std::vector<Pin> pins_;
    std::shared_ptr<Impl> impl_;
};

/** A draped garment as a game mesh. Per triangle corner UVs keep panel borders split where seams weld the points. */
struct GarmentSurface
{
    enum Side : uint8_t
    {
        Outer,
        Inner,
        Rim
    };
    std::vector<Vec3> points;
    std::vector<std::array<int, 3>> tris;
    std::vector<std::array<Vec2, 3>> uv;       // the packed layout, or the pattern without one
    std::vector<std::array<Vec2, 3>> fabricUv; // the flat pattern, metres
    std::vector<int> triPanel;
    std::vector<Side> triSide;
};

struct SurfaceOptions
{
    bool weldSeams = true;
    float thickness = 0.f; // metres; 0 leaves a single sheet
};

/**
 * Builds the output surface from the solver's positions. Welding merges every sewn pair into one point at their
 * midpoint. Thickness adds an inner shell along the vertex normals, back towards the wearer, and closes open borders with
 * rims. `packedUv` is per triangle corner (3 per mesh triangle) and may be empty.
 */
GarmentSurface garmentSurface(const ClothMesh& mesh, const std::vector<Vec3>& positions, const std::vector<Vec2>& packedUv,
                              const SurfaceOptions& options = {});

} // namespace tailor
