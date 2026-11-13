#pragma once

#include "PhysicsAnimation.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/**
 * A ragdoll's world on lila_physics, the rigid-body scene every physics extension shares. Everything here is in
 * centimetres except BodyState, which is Jolt's own floats.
 */
namespace physanim
{
/** Jolt's own state of a body, as the floats it holds, so writing it back is exact. Metres and radians. */
struct BodyState
{
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 angularVelocity{0.0f};
};

/** Registers Jolt's types, allocator and job system; idempotent, torn down when the extension unloads. */
void initJolt();

/** The collision shapes of a rig, made once and shared by every world built from it. */
class Shapes
{
public:
    explicit Shapes(const RigDef& rig);
    ~Shapes();
    Shapes(const Shapes&) = delete;
    Shapes& operator=(const Shapes&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl;
};

/** What one step of a world is driven by: the animation and the keyable values at the step's end. */
struct Drive
{
    struct BodyDrive
    {
        glm::dmat4 animated{1.0}; // the joint's animated world matrix, cm
        double stiffness = 0.0;   // pose match spring frequency, Hz
        double damping = 1.0;     // ratio
    };
    std::vector<BodyDrive> bodies;
    struct PinDrive
    {
        glm::dmat4 world{1.0};
        double stiffness = 0.0;
    };
    std::vector<PinDrive> pins;
    std::vector<glm::dmat4> colliders;
    std::vector<double> fieldMagnitudes;
    struct ContactDrive
    {
        double weight = 0.0;
        double stiffness = 0.0; // Hz
    };
    std::vector<ContactDrive> contacts;
    /** cm/s^2 every simulated body is accelerated by: the share of a carrier's acceleration it is carried along with. */
    glm::dvec3 carried{0.0};
    /** cm/s: pose match springs damp the bodies' velocity relative to this (a carrier's), not to the world. */
    glm::dvec3 frameVelocity{0.0};
    /** cm the carrier moves over this step: pose match targets are taken back by it, so they ride with the bodies. */
    glm::dvec3 frameShift{0.0};
    double seconds = 0.0;
    double period = 0.0;
};

struct WorldOptions
{
    /** Live mode: gravity off, pose matching off, extra damping, so the ragdoll stays where it is dragged. */
    bool live = false;
    double liveDamping = 4.0;
    /** The dt of the step before the restored state, so its joint impulses warm-start the next one. 0: none. */
    double previousStep = 0.0;
};

/** A physics scene holding a rig's bodies, joints and colliders, stepped single-threaded. */
class World
{
public:
    /** Bodies and colliders at the given states; constraintStates restores the joints' warm-start impulses (empty: none). */
    World(const RigDef& rig, const Shapes& shapes, const std::vector<BodyState>& bodies, const std::vector<BodyState>& colliders,
          const std::vector<std::string>& constraintStates, const WorldOptions& options = {});
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    /** One step of dt seconds toward the drive: kinematic targets, pose match motors and springs, pins, fields. */
    void step(const Drive& drive, double dt);

    BodyState body(size_t i) const;
    BodyState collider(size_t i) const;
    /** Each joint constraint's solver state (warm-start impulses), in body order; empty strings for bodies without one. */
    std::vector<std::string> constraintStates() const;

    /** Live mode: pull body i's point (body space, cm) toward a world target (cm). A negative body lets go. */
    void setGrab(int body, const glm::dvec3& localPoint, const glm::dvec3& target);
    /** Byte size of a joint constraint's saved state, the same for every one. */
    static size_t constraintStateSize();

    struct Impl;

private:
    std::unique_ptr<Impl> mImpl;
};

/** Triangles (three points each, shape space, cm) of a convex hull of points, for drawing and picking. */
std::vector<glm::dvec3> hullTriangles(const std::vector<glm::dvec3>& points);
} // namespace physanim
