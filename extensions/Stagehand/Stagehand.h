#pragma once

#include <RigidScene.h>

#include <lila/Graph.h>
#include <lila/Node.h>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace lila::ext
{
class Context;
}

/**
 * Stagehand: set dressing with physics. Things settle, get pushed, wiggled, dropped and shaken into place, as commands
 * and as a live viewport tool, over one world built from the scene (World below).
 */
namespace stagehand
{
using lila::scene::Graph;
using lila::scene::Node;
using lila::scene::NodeId;
namespace physics = lila::physics;
using json = nlohmann::json;

inline constexpr double kPi = std::numbers::pi;

inline json v3(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }

/** s with every character outside [A-Za-z0-9] made '_', and prefix put in front when it doesn't start with a letter. */
inline std::string identifier(const std::string& s, const char* prefix)
{
    std::string out;
    for (char c : s)
        out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    if (out.empty() || !std::isalpha(static_cast<unsigned char>(out[0])))
        out = prefix + out;
    return out;
}

/** The body settings a material starts from. */
struct Material
{
    const char* name;
    double density; // kg/m3
    double friction;
    double bounce;
    const char* doc;
};

/** wood, book, paper, cardboard, stone, ceramic, metal, glass, plastic, rubber, fabric. */
const std::vector<Material>& materials();
/** Null for an unknown name. */
const Material* material(const std::string& name);

/** The body node type: a child of a mesh carrying its physics settings, so duplicates, references and library assets keep them. */
inline constexpr const char* kBodyType = "stagehandBody";

enum class ShapeChoice : int
{
    Auto = 0, // a box when the mesh nearly fills one, else its convex hull
    Box = 1,
    Hull = 2,
    Sphere = 3,
    Capsule = 4,
    Mesh = 5, // the triangles themselves; never moves
    Decompose = 6, // convex parts following a concave mesh
};

enum class RoleChoice : int
{
    Dynamic = 0,
    Static = 1,
    Kinematic = 2,
    Off = 3, // ignored by physics
};

/** Enum names by ShapeChoice and RoleChoice value. */
inline constexpr const char* kShapeNames[] = {"auto", "box", "hull", "sphere", "capsule", "mesh", "decompose"};
inline constexpr const char* kRoleNames[] = {"dynamic", "static", "kinematic", "off"};

/** A mesh's resolved physics settings: its body node's, or the defaults when it has none. */
struct BodySettings
{
    bool hasNode = false;
    RoleChoice role = RoleChoice::Static;
    std::string material = "wood";
    double density = 600.0;
    double friction = 0.6;
    double bounce = 0.1;
    ShapeChoice shape = ShapeChoice::Auto;
    double rounding = 0.2; // cm
    int maxParts = 16;
};

/** The body node under a mesh, or null. */
Node* bodyNode(Graph& g, NodeId mesh);
BodySettings bodySettings(Graph& g, NodeId mesh);

/** A mesh node's world matrix split into what physics moves (position, rotation) and what it keeps (scale). */
struct Frame
{
    glm::dvec3 position{0.0};
    glm::dquat rotation{1, 0, 0, 0};
    glm::dvec3 scale{1.0};
};
Frame frameOf(const glm::dmat4& world);
glm::dmat4 matrixOf(const Frame& f);

/**
 * The collision shape of a mesh's evaluated output in its own space, scale applied. Built once per mesh content and
 * kept in a bounded cache, so settling again (or a thousand copies of one prop) costs nothing. A static terrain mesh
 * becomes a heightfield.
 */
physics::Shape shapeOf(Graph& g, NodeId mesh, ShapeChoice choice, double rounding, bool forStatic, int maxParts = 16);
/** shapeOf for a mesh value at a scale, without a node (and so never a heightfield). */
physics::Shape shapeOfMesh(const lila::scene::MeshPtr& mesh, const glm::dvec3& scale, ShapeChoice choice, double rounding, bool forStatic, int maxParts = 16);

/** A mesh's content hash and local bounds, worked out once per evaluated mesh. */
struct MeshFacts
{
    uint64_t hash = 0;
    glm::dvec3 lo{0.0}, hi{0.0};
    bool any = false;
};
MeshFacts meshFacts(const lila::scene::MeshPtr& mesh);
/** World bounds of a mesh node's output (its local bounds' corners transformed). False for an empty mesh. */
bool worldBoundsOf(Graph& g, NodeId id, glm::dvec3& lo, glm::dvec3& hi);

/** What a world is made of: the movers simulate, the colliders only get hit. */
struct WorldOptions
{
    std::vector<NodeId> movers;
    /** Always in the world; with collidersGiven they are the only colliders, else every visible mesh near the movers joins. */
    std::vector<NodeId> colliders;
    bool collidersGiven = false;
    /** Meshes with a dynamic body join as movers too, so a push carries through a whole shelf. */
    bool includeDynamicBodies = true;
    bool ground = true;
    double groundHeight = 0.0;
    glm::dvec3 gravity{0.0, -981.0, 0.0};
    /** Colliders further than this from every mover are left out, cm. */
    double reach = 300.0;
    int collisionSteps = 2;
    int velocitySteps = 12;
    /** Contacts solve as if no body were more than this many times heavier than the other (physics::Settings). */
    double maxMassRatio = 50.0;
};

/**
 * The scene's meshes as one physics world, mapped back to their nodes. Built once, stepped, then read back as world
 * matrices or written to the transforms.
 */
class World
{
public:
    World(Graph& g, const WorldOptions& o);

    physics::RigidScene& scene() { return *mScene; }
    const physics::RigidScene& scene() const { return *mScene; }

    /** Settles until at rest or the cap, seconds. */
    physics::SettleResult settle(double cap);

    /** Bodies in order: movers first, then colliders. */
    size_t movers() const { return mMoverCount; }
    NodeId node(int body) const { return mNodes[size_t(body)]; }
    /** The body of a node, or -1. */
    int bodyOf(NodeId n) const;

    /** Where the world now has a body's node, as a world matrix (its scale kept). */
    glm::dmat4 worldMatrix(int body) const;
    /** The node's world matrix when the world was built. */
    const glm::dmat4& startMatrix(int body) const { return mStart[size_t(body)]; }

    /** Sets each mover's transform to where the world has it. Records undo unless the stack is off. */
    void write(Graph& g) const;
    /** Puts each mover's transform back where it started. */
    void revert(Graph& g) const;

private:
    std::unique_ptr<physics::RigidScene> mScene;
    std::vector<NodeId> mNodes;
    std::vector<glm::dmat4> mStart;
    std::vector<glm::dvec3> mScale;
    size_t mMoverCount = 0;
};

/** Sets a node's transform so its world matrix becomes `world`, keeping its pivots. Undoable. */
void setWorld(Graph& g, NodeId id, const glm::dmat4& world);

/** Whether a node is a mesh that physics can see (visible, not switched off). */
bool physical(Graph& g, NodeId id);

/**
 * The meshes a push or shake carries to: everything touching the seeds within margin cm, then everything touching
 * those, while no bigger than the biggest seed times sizeRatio, and never what they stand on (a book row topples, the
 * shelf stays put). Meshes whose body says static or off never join.
 */
std::vector<NodeId> neighbours(Graph& g, const std::vector<NodeId>& seeds, double margin, double sizeRatio, const std::set<NodeId>& exclude);

bool registerNodes(lila::ext::Context& ctx);
void registerCommands(lila::ext::Context& ctx);
void registerUi(lila::ext::Context& ctx);
/** overlap::providers' "stagehand": penetration depth against a heightfield, for other extensions (Terrain's cliff kits). */
void registerPenetrationCheck(lila::ext::Context& ctx);
/** The stagehandAsset node type, and the library and placement commands (Place.cpp). */
bool registerLibraryNodes(lila::ext::Context& ctx);
void registerLibraryCommands(lila::ext::Context& ctx);
/** Row Along Line, Stack Items, Pile Items, Scatter Props and Settle Instances (ArrangeNodes.cpp). */
bool registerArrangeNodes(lila::ext::Context& ctx);
/** Contact Wear, Dent At Contacts, Sag Under Load and Variant Pool (WearNodes.cpp). */
bool registerWearNodes(lila::ext::Context& ctx);
/** A library asset's hidden master root in the scene, loaded on first use. Throws CommandError for an unknown id. */
NodeId libraryMaster(Graph& g, const std::string& id);
/** Every mesh at or under a node. */
std::vector<NodeId> meshesOf(Graph& g, NodeId root);
/** The first visible surface straight down from `above` cm over a point, skipping some meshes. */
std::optional<physics::RayHit> surfaceUnder(Graph& g, const glm::dvec3& point, const std::set<NodeId>& skip, double above = 1e5);
/** Bounds of every mesh under root, in root's own space. False when there are none. */
bool boundsUnder(Graph& g, NodeId root, glm::dvec3& lo, glm::dvec3& hi);
/** Placement helpers and modular snap points (LevelKit.cpp), and the Kit Place tool (KitTool.cpp). */
bool registerKitNodes(lila::ext::Context& ctx);
void registerKitCommands(lila::ext::Context& ctx);
void registerKitUi(lila::ext::Context& ctx);
/**
 * The stagehandAssembly node type; body shapes as UCX_ collision in Unreal exports, body settings on Unreal placements,
 * assemblyCreate and unrealPlaceAssembly (UnrealExport.cpp).
 */
bool registerUnrealNodes(lila::ext::Context& ctx);
/** The stagehandAssembly tag under a group, or null; its kind (levelInstance or packedLevelActor), empty when none. */
Node* assemblyTag(Graph& g, NodeId root);
std::string assemblyKind(Graph& g, NodeId root);
/** Marks a group as an assembly with a fresh key. Undoable. */
void tagAssembly(Graph& g, NodeId root, const std::string& kind, const std::string& level = {});
void registerUnrealExport(lila::ext::Context& ctx);
/** instancesToObjects (Realize.cpp). */
void registerRealizeCommands(lila::ext::Context& ctx);
/** fillShelf, scatterProps, pileProps, stackProps, pathProps, debrisProps (ArrangeCommands.cpp). */
void registerArrangeCommands(lila::ext::Context& ctx);
/** arrangementPresets, arrangePreset, arrangementSave (Presets.cpp). */
void registerPresetCommands(lila::ext::Context& ctx);
/** The Library panel and dragging assets from it into viewports (LibraryUi.cpp). */
void registerLibraryUi(lila::ext::Context& ctx);
} // namespace stagehand
