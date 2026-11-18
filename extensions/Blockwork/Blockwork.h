#pragma once

// Blockwork: grid-first level blocking. Blocks are ordinary meshes whose points sit in world space, each with a Texture
// Lock modifier that lays its uvs out from per-face mapping, so every mesh edit keeps the texture in place.

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Mesh.h>
#include <lila/Node.h>

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

namespace blockwork
{
using lila::json;

/** The Texture Lock modifier's node type, and the kind addModifier takes. */
inline constexpr const char* kTextureLockType = "textureLockModifier";
inline constexpr const char* kTextureLockKind = "textureLock";

/** The group new blocks go under when no parent is given. */
inline constexpr const char* kDefaultGroup = "Level";


// Per-face texture mapping, face attributes read by the Texture Lock modifier.
/** Int: kMapFace (default), kMapWorld or kMapAxes. */
inline constexpr const char* kMapMode = "bwMap";
/** Float2: uv offset, in uv units. */
inline constexpr const char* kMapOffset = "bwOffset";
/** Float2: cm per texture repeat along u and v; 0 uses the modifier's tile size. */
inline constexpr const char* kMapScale = "bwScale";
/** Float: rotation around the face, degrees. */
inline constexpr const char* kMapRotate = "bwRotate";
/** Float3 each: the u and v axes for kMapAxes, unit length. */
inline constexpr const char* kMapUAxis = "bwUAxis";
inline constexpr const char* kMapVAxis = "bwVAxis";

enum MapMode : int32_t
{
    kMapFace = 0,  // projected on the face's own plane, upright
    kMapWorld = 1, // projected along the world axis nearest the normal
    kMapAxes = 2,  // explicit axes (wrapped across an edge, aligned to an edge)
    kMapStored = 3, // the uvs the mesh stores (hotspotted faces)
};

/** A face's mapping as stored, with defaults filled in. */
struct FaceMapping
{
    int32_t mode = kMapFace;
    glm::vec2 offset{0.0f};
    glm::vec2 scale{0.0f};
    float rotate = 0.0f;
    glm::vec3 uAxis{1.0f, 0.0f, 0.0f};
    glm::vec3 vAxis{0.0f, 1.0f, 0.0f};
};

FaceMapping faceMapping(const lila::geo::Mesh& mesh, lila::geo::Index f);
void setFaceMapping(lila::geo::Mesh& mesh, lila::geo::Index f, const FaceMapping& m);
/** The unit u and v directions the mapping projects along for a face with this normal, before rotation. */
void mappingAxes(const FaceMapping& m, const glm::dvec3& normal, glm::dvec3& u, glm::dvec3& v);
/** The uv of a point on a face with this mapping and normal; tileSize fills a zero scale. */
glm::dvec2 mapPoint(const FaceMapping& m, const glm::dvec3& normal, const glm::dvec3& p, double tileSize);
/** Writes uv0 on every face from its mapping, except faces that keep their stored uvs. */
void applyMapping(lila::geo::Mesh& mesh, double tileSize);

// Shapes.
enum class Shape
{
    Box,
    Wedge,
    Cylinder,
    Arch,
    Stairs,
    Prism,
};
const std::vector<std::string>& shapeNames();
std::optional<Shape> shapeFromName(const std::string& name);

/**
 * A block's footprint: a rectangle from origin along u (size.x) and v (size.y), raised along normal by size.z. u, v and
 * normal are unit and orthogonal; sizes may be negative and are normalised by buildShape.
 */
struct Frame
{
    glm::dvec3 origin{0.0};
    glm::dvec3 u{1.0, 0.0, 0.0};
    glm::dvec3 v{0.0, 0.0, 1.0};
    glm::dvec3 normal{0.0, 1.0, 0.0};
    glm::dvec3 size{100.0};
};

struct ShapeOptions
{
    /** Cylinder and arch sides. */
    int sides = 12;
    /** Cylinder: polygon corners on the drawn radii (false: side centres on them). */
    bool radiusToCorners = true;
    /** Cylinder: leave the caps open. */
    bool skipCaps = false;
    /** Cylinder: caps as one n-gon (false: a fan of triangles to a centre point). */
    bool ngonCaps = true;
    /** Stairs: step count; 0 takes it from stepHeight. */
    int steps = 8;
    double stepHeight = 0.0;
    /** Arch: width of the frame around the opening, cm. */
    double thickness = 25.0;
    /** Arch: the opening itself, rounded at the top of the footprint and as deep as the height, for carving doorways. */
    bool opening = false;
    /** Quarter turns of the shape's direction around the normal (stairs climb, wedge slope, arch opening). */
    int direction = 0;
    /** Prism: the profile, in the frame's (u, v) plane relative to its origin. */
    std::vector<glm::dvec2> profile;
};

/** The block as a mesh in world space with outward faces, or an empty mesh when the frame is degenerate for the shape. */
lila::geo::Mesh buildShape(Shape shape, Frame frame, const ShapeOptions& opts);
/** A closed profile in the (a, b) plane at origin, extruded along c by depth. Concave profiles work. */
lila::geo::Mesh extrudeProfile(std::vector<glm::dvec2> profile, const glm::dvec3& origin, const glm::dvec3& a, const glm::dvec3& b,
                               const glm::dvec3& c, double depth);
/** Marks edges hard where neighbouring faces meet at more than angle degrees. */
void hardenEdges(lila::geo::Mesh& mesh, double angle = 40.0);

// Geometry operations on a block's mesh, all in the mesh's own space.
/** Shell walls of the thickness inside (or outside) the closed mesh; open faces are removed and their rims bridged. */
lila::geo::Mesh hollow(const lila::geo::Mesh& mesh, double thickness, bool inward, const std::vector<lila::geo::Index>& open);
/** Cuts along the plane and keeps the side the normal points to (keep > 0), the other (keep < 0); the cut is capped. */
lila::geo::Mesh clip(const lila::geo::Mesh& mesh, const glm::dvec3& point, const glm::dvec3& normal, int keep);
/** The face of the mesh that point lies on, facing along normal; kInvalid when none does. */
lila::geo::Index faceAt(const lila::geo::Mesh& mesh, const glm::dvec3& point, const glm::dvec3& normal, double tolerance = 0.5);
/** Faces of a whose every corner lies on an opposite-facing coplanar face of b. */
std::vector<lila::geo::Index> coveredFaces(const lila::geo::Mesh& a, const lila::geo::Mesh& b, double tolerance = 0.01);

/** In-plane directions for a footprint on a surface facing normal: the world axis lying in the plane first. */
void planeBasis(const glm::dvec3& normal, glm::dvec3& u, glm::dvec3& v);
/** The footprint's across direction for a side direction u, on a positive world axis whichever way the surface faces. */
glm::dvec3 acrossAxis(const glm::dvec3& normal, const glm::dvec3& u);

// Grid.
/** The grid step, cm, in powers of two from 1/8 to 4096 (default 16). */
double gridStep();
void setGridStep(double step);
/** Absolute snapping (to grid lines) instead of incremental (by grid steps). */
bool gridAbsolute();
double snap(double value, double step);
glm::dvec3 snapPoint(const glm::dvec3& p, double step);

// Scene.
lila::scene::Graph& graph();
/** The block meshes: positional args, the selection, or (all) every block in the scene. */
std::vector<lila::scene::NodeId> blockArgs(const lila::Args& a, const char* cmd, bool allWhenEmpty = false);
bool isBlock(const lila::scene::Node& n);
/** The base mesh direct edits change (below the Texture Lock modifier). */
lila::geo::Mesh baseMesh(lila::scene::Graph& g, lila::scene::NodeId mesh);
void setBaseMesh(lila::scene::Graph& g, lila::scene::NodeId mesh, lila::geo::Mesh m);
/** Makes a block node from a world-space mesh under parent, with its pivots at the centre and a Texture Lock modifier. */
lila::scene::NodeId createBlock(lila::scene::Graph& g, lila::geo::Mesh mesh, const std::string& name, lila::scene::NodeId parent,
                                const std::string& material);
/** The level group (created under the world when missing). */
lila::scene::NodeId levelGroup(lila::scene::Graph& g, const std::string& name);
/** The mesh in world space. */
lila::geo::Mesh worldMesh(lila::scene::Graph& g, lila::scene::NodeId mesh);

bool registerTypes(lila::ext::Context& ctx);
void registerCommands(lila::ext::Context& ctx);
void registerTextureCommands(lila::ext::Context& ctx);
void registerUi(lila::ext::Context& ctx);

/** The texel density new materials map at, px per cm (default 5.12, 512 px a metre). */
double texelDensity();
/** cm per repeat for a material's base colour image at the texel density, or nullopt when it has none to read. */
std::optional<glm::dvec2> materialRepeat(lila::scene::Graph& g, const std::string& material);
/** The material made for (or already using) an image file, created when missing. */
std::string materialForImage(lila::scene::Graph& g, const std::string& path);
} // namespace blockwork
