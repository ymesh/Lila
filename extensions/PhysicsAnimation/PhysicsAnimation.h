#pragma once

#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Node.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

/** Node types, the rig model built from them, and fitting, shared by the scene and UI halves of Physics Animation. */
namespace physanim
{
using lila::scene::Graph;
using lila::scene::kNoNode;
using lila::scene::Node;
using lila::scene::NodeId;

inline constexpr double kPi = std::numbers::pi;

inline constexpr const char* kSolverType = "physicsSolver";
inline constexpr const char* kMarkerType = "physicsMarker";
inline constexpr const char* kPinType = "physicsPin";
inline constexpr const char* kColliderType = "physicsCollider";
inline constexpr const char* kFieldType = "physicsField";

/** General pref: clicking a marker's body selects its joint. */
inline constexpr const char* kProxySelectPref = "/physics/proxySelectsJoint";

enum class Behaviour : int64_t
{
    Kinematic, // follows the animation, pushes the others
    PoseMatch, // simulated, pulled toward the animated pose by springs
    Simulated, // free, only the joint limits hold it
    Proxy,     // drawn and selectable, not simulated
};

enum class ShapeKind : int64_t
{
    Capsule,
    Box,
    Sphere,
    ConvexHull,
};

enum class PoseSpace : int64_t
{
    Local, // joint motors toward the animated rotation relative to the parent body
    World, // springs toward the animated world position and rotation
    Both,
};

enum class FieldKind : int64_t
{
    Gravity,    // extra acceleration along direction
    Wind,       // air moving at direction * magnitude cm/s, dragging the bodies with it
    Turbulence, // seeded noise acceleration, decorrelated per body
    Drag,       // air resistance: magnitude per second
};

// Attribute access by name: the extension's types are found by name, so attributes are too.
int attr(const Node& n, const char* name);
double num(const Node& n, const char* name);
int64_t integer(const Node& n, const char* name);
bool flag(const Node& n, const char* name);
std::string text(const Node& n, const char* name);
/** The node a text attr names (a full path, or a name in older scenes); null when it is gone or ambiguous. */
Node* named(Graph& g, const std::string& path);
glm::dvec3 vec(const Node& n, const char* name);

/** A marker's collision shape in its joint's space, cm. A capsule's axis is the shape frame's X. */
struct ShapeDesc
{
    ShapeKind kind = ShapeKind::Capsule;
    double radius = 5.0;
    double length = 20.0;            // capsule: between the cap centres
    glm::dvec3 extents{10.0};        // box: full size
    glm::dvec3 offset{0.0};          // shape frame origin in joint space
    glm::dquat rotation{1, 0, 0, 0}; // shape frame in joint space
    std::vector<glm::dvec3> hull;    // convex hull points, shape space
    /** Sized from the skeleton's proportions, there being no skinned mesh to fit (not stored). */
    bool fromBones = false;
};

ShapeDesc shapeOf(const Node& marker);
/** Sets the marker's shape attributes (undoably). */
void writeShape(Graph& g, NodeId marker, const ShapeDesc& s);

/** Triangles (three points each) and outline segments (two each) of a shape, in joint space. Cached by content. */
struct ShapeGeometry
{
    std::vector<glm::dvec3> triangles;
    std::vector<glm::dvec3> lines;
};
std::shared_ptr<const ShapeGeometry> geometryOf(const ShapeDesc& s);

/** Distance along a ray to a shape posed at world (scale ignored); nullopt for a miss. */
std::optional<double> rayHit(const ShapeDesc& s, const glm::dmat4& world, const glm::dvec3& origin, const glm::dvec3& dir);

/** World matrix with the scale taken out. */
glm::dmat4 rigid(const glm::dmat4& m);

bool isMarker(const Node& n);
/** The marker's joint (by name), or kNoNode. */
NodeId jointOf(Graph& g, const Node& marker);
/** The first marker (in id order) driving the joint, or kNoNode. */
NodeId markerFor(Graph& g, NodeId joint);
/** The solver a marker, pin, collider or field belongs to: its nearest solver ancestor. */
NodeId solverOf(Graph& g, NodeId node);
/** Children of type, below a solver, in id order. */
std::vector<NodeId> membersOf(Graph& g, NodeId solver, const char* type);

/** A body of the rig: a marker that is not a proxy. */
struct BodyDef
{
    NodeId marker = kNoNode;
    NodeId joint = kNoNode;
    int parent = -1; // the nearest ancestor joint's body
    Behaviour behaviour = Behaviour::PoseMatch;
    PoseSpace space = PoseSpace::Both;
    ShapeDesc shape;
    double mass = 0.0; // kg; 0 derives it from density
    double density = 1000.0;
    double friction = 0.8;
    double restitution = 0.0;
    double linearDamping = 0.05;
    double angularDamping = 0.05;
    int64_t group = 0;
    /** The top joint of the body's skeleton: in a grapple's ragdoll, bodies of different skeletons collide. */
    NodeId skeleton = kNoNode;
    // The joint to the parent body. Frames are rotations: X the twist axis, Y the plane axis.
    glm::dvec3 anchorInParent{0.0}; // cm, parent joint space
    glm::dquat frameInParent{1, 0, 0, 0};
    glm::dquat frameInChild{1, 0, 0, 0};
    bool limited = true;
    double twist = 0.8, swing1 = 0.8, swing2 = 0.8; // half ranges, radians
    int stiffnessAttr = -1, dampingAttr = -1;
};

struct PinDef
{
    NodeId node = kNoNode;
    int body = -1;
    bool orient = false;
    double damping = 1.0;
    int stiffnessAttr = -1;
};

struct ColliderDef
{
    NodeId node = kNoNode;
    NodeId mesh = kNoNode;
    /** Triangles in the mesh's object space with its world scale applied, cm. */
    std::shared_ptr<const std::vector<glm::dvec3>> triangles;
    double friction = 0.8;
    double restitution = 0.0;
};

struct FieldDef
{
    NodeId node = kNoNode;
    FieldKind kind = FieldKind::Wind;
    glm::dvec3 direction{1.0, 0.0, 0.0};
    double frequency = 0.5;
    uint64_t seed = 1;
    int magnitudeAttr = -1;
};

/**
 * A grapple contact between two bodies (of two characters, usually): a spring pulling a point on one to a point on the
 * other, both yielding by mass, with its keyable weight and stiffness sampled each frame.
 */
struct ContactDef
{
    NodeId node = kNoNode;
    int a = -1, b = -1;
    glm::dvec3 pointA{0.0}, pointB{0.0}; // body space, cm
    /** B's frame to A's at full weight, when the contact holds rotation too. */
    std::optional<glm::dquat> rotation;
    int weightAttr = -1, stiffnessAttr = -1;
};

/** Everything a world is built from, read off a solver node and the scene. */
struct RigDef
{
    NodeId solver = kNoNode;
    std::vector<BodyDef> bodies; // parents before children
    std::vector<PinDef> pins;
    std::vector<ColliderDef> colliders;
    std::vector<FieldDef> fields;
    std::vector<ContactDef> contacts;
    /** A grapple's ragdoll: bodies of different skeletons collide whatever selfCollision says. */
    bool figures = false;
    glm::dvec3 gravity{0.0, -980.0, 0.0};
    bool ground = true;
    double groundHeight = 0.0;
    double groundFriction = 0.8;
    bool selfCollision = false;
    int velocitySteps = 10;
    int positionSteps = 2;
    /** Every setting above, for the simulation's parameters hash. */
    uint64_t hash = 0;
};

/** Throws std::invalid_argument when the solver is gone. */
RigDef buildRig(Graph& g, NodeId solver);

/** The rotation of child relative to ancestor with every rotate on the way zeroed: the rest pose the limits sit in. */
glm::dquat restRotation(Graph& g, NodeId ancestor, NodeId child);
/** The child's origin in the ancestor's space at rest. */
glm::dvec3 restOffset(Graph& g, NodeId ancestor, NodeId child);
/** Unit bone axis of a joint in its own space: toward its first child joint, else the shape's X. */
glm::dvec3 boneAxis(Graph& g, NodeId joint);
/** The joint frame of a body (X along the bone) in its joint's space. */
glm::dquat boneFrame(Graph& g, NodeId joint);

/** Twist about X, then swings about Y and Z, of a constraint-space rotation; radians. */
glm::dvec3 swingTwist(const glm::dquat& q);
glm::dquat fromSwingTwist(const glm::dvec3& angles);

/** The rotation vector (axis times angle, radians) of q, taking the shorter way round. */
inline glm::dvec3 rotationVector(const glm::dquat& q0)
{
    glm::dquat q = q0.w < 0.0 ? -q0 : q0;
    const double s = glm::length(glm::dvec3(q.x, q.y, q.z));
    if (s < 1e-12)
        return glm::dvec3(q.x, q.y, q.z) * 2.0;
    return glm::dvec3(q.x, q.y, q.z) * (2.0 * std::atan2(s, q.w) / s);
}

/**
 * Fits a marker's shape to its joint's dominant-influence region of the skinned meshes; with none, bodySizedShape
 * (fromBones set, for the caller to report).
 */
ShapeDesc fitShape(Graph& g, NodeId joint, ShapeKind kind);
/**
 * A shape for a joint with no skin to fit: sized by its label (pelvis, spine, head, hand ...) from the skeleton's height
 * and shoulder span and its bone's length, so a bare skeleton's ragdoll has a body's sizes and mass.
 */
ShapeDesc bodySizedShape(Graph& g, NodeId joint, ShapeKind kind, const glm::dquat& frame);

struct LimitFit
{
    glm::dvec3 orient{0.0}; // degrees: the centre of the motion as twist, swing1, swing2
    double twist = 0.0, swing1 = 0.0, swing2 = 0.0; // half ranges, degrees
    int frames = 0;
};
/** The swing cones and twist range the joint moves through relative to the parent body, over frames start..end. */
LimitFit fitLimits(Graph& g, NodeId marker, int64_t start, int64_t end, double margin);

/** The semantic label of a joint (pelvis, spine_02, hand_l ...): its otherType or type label, else its name. Empty if none. */
std::string semanticLabel(Graph& g, NodeId joint);

/** Default setup for a label: shape, behaviour, limits (degrees). */
struct LabelDefaults
{
    ShapeKind shape = ShapeKind::Capsule;
    Behaviour behaviour = Behaviour::PoseMatch;
    /** Both: local motors alone let a standing body fold under gravity; the world pull holds it up. */
    PoseSpace space = PoseSpace::Both;
    double twist = 30.0, swing1 = 45.0, swing2 = 45.0;
    double stiffness = 6.0;
};
/** Defaults for a label; nullopt for labels a template ragdoll leaves out (fingers, twist and IK joints). */
std::optional<LabelDefaults> labelDefaults(const std::string& label);

bool registerScene(lila::ext::Context& ctx);
/** grappleRagdoll (GrappleRagdoll.cpp). */
void registerGrappleRagdoll(lila::ext::Context& ctx);
void registerUi(lila::ext::Context& ctx);
} // namespace physanim
