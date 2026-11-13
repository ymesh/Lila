// A ragdoll on lila_physics: the rig's bodies come first in the scene (so its pair filter is indexed by body), then the
// colliders; each body with a parent hangs from it by a swing-twist joint. Single-threaded, so the order of every solver
// operation is fixed and the results are bitwise reproducible.

#include "World.h"

#include <lila/Simulation.h>

#include <RigidScene.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace physanim
{
namespace
{
namespace ph = lila::physics;

constexpr double kM = 0.01; // cm to m
ph::Shape bodyShape(const BodyDef& b)
{
    const ShapeDesc& s = b.shape;
    ph::Shape out;
    out.center = s.offset;
    out.rotation = glm::normalize(s.rotation);
    const double r = std::max(s.radius, 0.05);
    switch (s.kind)
    {
    case ShapeKind::Capsule:
        out.kind = ph::Shape::Kind::Capsule;
        out.halfHeight = std::max(s.length * 0.5, 1e-3);
        out.radius = r;
        // Capsules lie along Y; a marker's along X.
        out.rotation = glm::normalize(s.rotation * glm::angleAxis(-kPi * 0.5, glm::dvec3(0, 0, 1)));
        break;
    case ShapeKind::Box:
    {
        out.kind = ph::Shape::Kind::Box;
        out.halfExtents = glm::max(s.extents * 0.5, glm::dvec3(0.05));
        out.rounding = std::min(1.0, std::min({out.halfExtents.x, out.halfExtents.y, out.halfExtents.z}) * 0.25);
        break;
    }
    case ShapeKind::Sphere:
        out.kind = ph::Shape::Kind::Sphere;
        out.radius = r;
        break;
    case ShapeKind::ConvexHull:
        if (s.hull.size() >= 4)
        {
            out.kind = ph::Shape::Kind::Hull;
            out.points = std::make_shared<const std::vector<glm::dvec3>>(s.hull);
            out.rounding = 0.2;
        }
        else
        {
            out.kind = ph::Shape::Kind::Sphere;
            out.radius = r;
        }
        break;
    }
    return out;
}

/** Implicit spring-damper acceleration toward a target: stable at any stiffness for the step it is taken over. */
double springGain(double frequency, double damping, double dt, double& dampGain)
{
    const double w = 2.0 * kPi * std::max(frequency, 0.0);
    const double k = w * w, c = 2.0 * std::max(damping, 0.0) * w;
    const double d = 1.0 + c * dt + k * dt * dt;
    dampGain = c / d;
    return k / d;
}

/** Whether a contact holds bodies i and j (or a parent or child of either) against each other, so they never fight it. */
bool held(const RigDef& rig, int i, int j)
{
    auto near = [&](int x, int body) { return x == body || rig.bodies[size_t(x)].parent == body || rig.bodies[size_t(body)].parent == x; };
    for (const ContactDef& c : rig.contacts)
        if ((near(i, c.a) && near(j, c.b)) || (near(j, c.a) && near(i, c.b)))
            return true;
    return false;
}

ph::Pose poseOf(const glm::dmat4& m) { return {glm::dvec3(m[3]), glm::normalize(glm::quat_cast(glm::dmat3(m)))}; }

BodyState stateOf(const ph::RawState& s) { return {s.position, s.rotation, s.velocity, s.angularVelocity}; }
} // namespace

void initJolt() { ph::initJolt(); }

struct Shapes::Impl
{
    std::vector<ph::Shape> bodies;
    std::vector<ph::Shape> colliders;
};

Shapes::Shapes(const RigDef& rig) : impl(std::make_unique<Impl>())
{
    for (const BodyDef& b : rig.bodies)
        impl->bodies.push_back(bodyShape(b));
    for (const ColliderDef& c : rig.colliders)
    {
        ph::Shape s;
        s.kind = ph::Shape::Kind::Mesh;
        s.points = c.triangles;
        impl->colliders.push_back(std::move(s));
    }
}

Shapes::~Shapes() = default;

struct World::Impl
{
    const RigDef& rig;
    WorldOptions options;
    std::unique_ptr<ph::RigidScene> scene;
    /** cm/s: the velocity spring damping is measured against (a carrier's), so riding along costs no drag. */
    glm::dvec3 frameVelocity{0.0};
    std::vector<int> joints; // per body, -1 for a root
    int grabBody = -1;
    glm::dvec3 grabLocal{0.0}, grabTarget{0.0};

    Impl(const RigDef& r, const WorldOptions& o) : rig(r), options(o) {}

    bool dynamic(size_t i) const { return scene->desc(int(i)).role == ph::Role::Dynamic; }

    /** Pushes a body toward a world pose with a spring, as an acceleration of its centre of mass and its turning. */
    void spring(size_t i, const glm::dvec3* target, const glm::dquat* targetRot, double frequency, double damping, double dt)
    {
        if (frequency <= 0.0 || !dynamic(i))
            return;
        double c = 0.0;
        const double k = springGain(frequency, damping, dt, c);
        const int body = int(i);
        const ph::Pose now = scene->pose(body);
        glm::dvec3 linear(0.0), angular(0.0);
        if (target)
            linear = k * (*target - now.position) - c * (scene->velocity(body) - frameVelocity);
        if (targetRot)
            angular = k * rotationVector(*targetRot * glm::conjugate(now.rotation)) - c * scene->angularVelocity(body);
        scene->accelerate(body, linear, angular);
    }

    /**
     * A grapple contact: a spring between a point on each body, as equal and opposite forces, so each yields by its mass
     * (a kinematic side doesn't yield). With a held rotation, a turning spring shared the same way.
     */
    void contact(const ContactDef& c, double weight, double frequency, double dt)
    {
        weight = std::clamp(weight, 0.0, 1.0);
        if (weight <= 0.0 || frequency <= 0.0)
            return;
        const bool da = dynamic(size_t(c.a)), db = dynamic(size_t(c.b));
        if (!da && !db)
            return;
        double damp = 0.0;
        const double k = springGain(frequency, 1.0, dt, damp);
        const ph::Pose pa = scene->pose(c.a), pb = scene->pose(c.b);
        const glm::dvec3 a = pa.position + pa.rotation * c.pointA, b = pb.position + pb.rotation * c.pointB;
        const double ma = scene->mass(c.a), mb = scene->mass(c.b);
        const double mass = da && db ? ma * mb / std::max(ma + mb, 1e-9) : da ? ma : mb;
        const glm::dvec3 force = (k * (b - a) - damp * (scene->pointVelocity(c.b, b) - scene->pointVelocity(c.a, a))) * mass * weight;
        if (da)
            scene->push(c.a, force, a);
        if (db)
            scene->push(c.b, -force, b);
        if (c.rotation)
        {
            const glm::dvec3 turn = rotationVector(pb.rotation * *c.rotation * glm::conjugate(pa.rotation));
            const glm::dvec3 spin = scene->angularVelocity(c.a) - scene->angularVelocity(c.b);
            const glm::dvec3 accel = (k * turn - damp * spin) * weight;
            const double shareA = da && db ? mb / std::max(ma + mb, 1e-9) : da ? 1.0 : 0.0;
            if (da)
                scene->accelerate(c.a, glm::dvec3(0.0), accel * shareA);
            if (db)
                scene->accelerate(c.b, glm::dvec3(0.0), -accel * (1.0 - shareA));
        }
    }
};

World::World(const RigDef& rig, const Shapes& shapes, const std::vector<BodyState>& bodies, const std::vector<BodyState>& colliders,
             const std::vector<std::string>& constraintStates, const WorldOptions& options)
    : mImpl(std::make_unique<Impl>(rig, options))
{
    Impl& w = *mImpl;
    const size_t n = rig.bodies.size();
    ph::Settings ps;
    ps.gravity = options.live ? glm::dvec3(0.0) : rig.gravity;
    ps.ground = rig.ground;
    ps.groundHeight = rig.groundHeight;
    ps.groundFriction = rig.groundFriction;
    ps.sleep = false;
    ps.recordContacts = false;
    // People-sized bodies: Jolt's own contact tuning.
    ps.penetrationSlop = 2.0;
    ps.speculativeDistance = 2.0;
    ps.sleepSpeed = 3.0;
    ps.velocitySteps = rig.velocitySteps;
    ps.positionSteps = rig.positionSteps;
    ps.largeIslandSplitter = false;
    ps.maxBodies = uint32_t(n + rig.colliders.size() + 4);
    ps.maxPairs = 4096;
    ps.serial = true;
    ps.pairFilter = int(std::max<size_t>(n, 1));
    w.scene = std::make_unique<ph::RigidScene>(ps);
    ph::RigidScene& scene = *w.scene;

    for (size_t i = 0; i < n; ++i)
    {
        const BodyDef& b = rig.bodies[i];
        const BodyState& s = bodies[i];
        const double extra = options.live ? options.liveDamping : 0.0;
        ph::BodyDesc d;
        d.shape = shapes.impl->bodies[i];
        d.position = glm::dvec3(s.position) / kM;
        d.rotation = glm::dquat(s.rotation);
        d.role = b.behaviour == Behaviour::Kinematic ? ph::Role::Kinematic : ph::Role::Dynamic;
        d.density = std::max(b.density, 1e-3);
        d.mass = b.mass;
        d.friction = b.friction;
        d.bounce = b.restitution;
        d.damping = b.linearDamping + extra;
        d.angularDamping = b.angularDamping + extra;
        d.velocity = glm::dvec3(s.velocity) / kM;
        d.angularVelocity = glm::dvec3(s.angularVelocity);
        d.collides = b.group >= 0;
        d.canSleep = false;
        d.continuous = false;
        d.user = int64_t(i);
        scene.add(d);
    }
    for (size_t c = 0; c < rig.colliders.size(); ++c)
    {
        const BodyState& s = colliders[c];
        ph::BodyDesc d;
        d.shape = shapes.impl->colliders[c];
        d.position = glm::dvec3(s.position) / kM;
        d.rotation = glm::dquat(s.rotation);
        d.role = ph::Role::Kinematic;
        d.friction = rig.colliders[c].friction;
        d.bounce = rig.colliders[c].restitution;
        d.canSleep = false;
        d.continuous = false;
        scene.add(d);
    }

    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < i; ++j)
        {
            const BodyDef &a = rig.bodies[i], &b = rig.bodies[j];
            const bool related = a.parent == int(j) || b.parent == int(i);
            const bool grouped = a.group > 0 && a.group == b.group;
            const bool together = !rig.figures || a.skeleton == b.skeleton;
            if ((together && (!rig.selfCollision || related)) || grouped || held(rig, int(i), int(j)))
                scene.setCollides(int(i), int(j), false);
        }

    w.joints.assign(n, -1);
    bool restored = false;
    for (size_t i = 0; i < n; ++i)
    {
        const BodyDef& b = rig.bodies[i];
        if (b.parent < 0)
            continue;
        const BodyState &p = bodies[size_t(b.parent)], &c = bodies[i];
        const glm::dquat pq = glm::dquat(p.rotation), cq = glm::dquat(c.rotation);
        const glm::dquat f1 = pq * b.frameInParent, f2 = cq * b.frameInChild;
        ph::JointDesc j;
        j.kind = ph::JointKind::SwingTwist;
        j.a = b.parent;
        j.b = int(i);
        j.pointA = glm::dvec3(p.position) / kM + pq * b.anchorInParent;
        j.pointB = glm::dvec3(c.position) / kM;
        j.twistAxisA = f1 * glm::dvec3(1, 0, 0);
        // The joint's frames are twist X, normal Y, plane Z: the plane axis is the frames' Z.
        j.planeAxisA = f1 * glm::dvec3(0, 0, 1);
        j.twistAxisB = f2 * glm::dvec3(1, 0, 0);
        j.planeAxisB = f2 * glm::dvec3(0, 0, 1);
        const double open = kPi;
        j.planeHalfCone = std::clamp(b.limited ? b.swing1 : open, 0.01, kPi);
        j.normalHalfCone = std::clamp(b.limited ? b.swing2 : open, 0.01, kPi);
        j.twistMin = -std::clamp(b.limited ? b.twist : open, 0.01, kPi);
        j.twistMax = std::clamp(b.limited ? b.twist : open, 0.01, kPi);
        w.joints[i] = scene.addJoint(j);
        if (i < constraintStates.size() && !constraintStates[i].empty())
        {
            scene.restoreJoint(w.joints[i], std::vector<uint8_t>(constraintStates[i].begin(), constraintStates[i].end()));
            restored = true;
        }
    }
    // Restored impulses only warm-start when the scene believes it stepped before, by the same dt.
    if (options.previousStep > 0.0 && restored)
        scene.primeStep(options.previousStep);
}

World::~World() = default;

void World::step(const Drive& drive, double dt)
{
    Impl& w = *mImpl;
    ph::RigidScene& scene = *w.scene;
    const RigDef& rig = w.rig;
    const size_t n = rig.bodies.size();
    w.frameVelocity = drive.frameVelocity;
    for (size_t c = 0; c < rig.colliders.size() && c < drive.colliders.size(); ++c)
        scene.moveTo(int(n + c), poseOf(rigid(drive.colliders[c])), dt);

    for (size_t i = 0; i < n && i < drive.bodies.size(); ++i)
    {
        const BodyDef& b = rig.bodies[i];
        const ph::Pose target = poseOf(rigid(drive.bodies[i].animated));
        if (b.behaviour == Behaviour::Kinematic)
        {
            scene.moveTo(int(i), target, dt);
            continue;
        }
        const bool poseMatch = b.behaviour == Behaviour::PoseMatch && !w.options.live;
        const double stiffness = drive.bodies[i].stiffness, damping = drive.bodies[i].damping;
        if (w.joints[i] >= 0)
        {
            const bool motor = poseMatch && b.space != PoseSpace::World && stiffness > 0.0;
            glm::dquat local(1, 0, 0, 0);
            if (motor)
            {
                const glm::dquat parentRot = poseOf(rigid(drive.bodies[size_t(b.parent)].animated)).rotation;
                local = glm::normalize(glm::conjugate(b.frameInParent) * glm::conjugate(parentRot) * target.rotation * b.frameInChild);
            }
            scene.setJointMotor(w.joints[i], local, motor ? stiffness : 0.0, damping);
        }
        // World space matching, and the local kind's stand-in on a body with no parent to be local to.
        if (poseMatch && (b.space != PoseSpace::Local || b.parent < 0))
        {
            // In a carrier's space the target is where it sits relative to the carrier at the step's start.
            const glm::dvec3 relative = target.position - drive.frameShift;
            w.spring(i, &relative, &target.rotation, stiffness, damping, dt);
        }
    }

    for (size_t p = 0; p < rig.pins.size() && p < drive.pins.size(); ++p)
    {
        const PinDef& pin = rig.pins[p];
        if (pin.body < 0)
            continue;
        const ph::Pose at = poseOf(rigid(drive.pins[p].world));
        w.spring(size_t(pin.body), &at.position, pin.orient ? &at.rotation : nullptr, drive.pins[p].stiffness, pin.damping, dt);
    }

    if (!w.options.live)
        for (size_t f = 0; f < rig.fields.size() && f < drive.fieldMagnitudes.size(); ++f)
        {
            const FieldDef& fd = rig.fields[f];
            const double mag = drive.fieldMagnitudes[f];
            const double len = glm::length(fd.direction);
            const glm::dvec3 dir = len > 1e-12 ? fd.direction / len : glm::dvec3(1, 0, 0);
            double x = drive.seconds * std::max(fd.frequency, 1e-6);
            int64_t period = 0;
            if (drive.period > 0.0)
            {
                period = std::max<int64_t>(1, std::llround(drive.period * std::max(fd.frequency, 1e-6)));
                x = drive.seconds / drive.period * double(period);
            }
            for (size_t i = 0; i < n; ++i)
            {
                const glm::dvec3 v = scene.velocity(int(i));
                glm::dvec3 a(0.0);
                switch (fd.kind)
                {
                case FieldKind::Gravity: a = dir * mag; break;
                case FieldKind::Wind: a = dir * mag - v; break;
                case FieldKind::Turbulence: a = lila::scene::sim::noise3(fd.seed * 1000003ull + uint64_t(i) + 1, x, period) * mag; break;
                case FieldKind::Drag: a = -v * mag; break;
                }
                scene.accelerate(int(i), a);
            }
        }
    if (!w.options.live && drive.carried != glm::dvec3(0.0))
        for (size_t i = 0; i < n; ++i)
            scene.accelerate(int(i), drive.carried);

    for (size_t k = 0; k < rig.contacts.size() && k < drive.contacts.size(); ++k)
        w.contact(rig.contacts[k], drive.contacts[k].weight, drive.contacts[k].stiffness, dt);

    if (w.grabBody >= 0 && size_t(w.grabBody) < n && w.dynamic(size_t(w.grabBody)))
    {
        const int body = w.grabBody;
        const ph::Pose now = scene.pose(body);
        const glm::dvec3 point = now.position + now.rotation * w.grabLocal;
        double c = 0.0;
        const double k = springGain(4.0, 1.0, dt, c);
        const glm::dvec3 force = (k * (w.grabTarget - point) - c * scene.pointVelocity(body, point)) * scene.mass(body) * 4.0;
        scene.push(body, force, point);
    }

    scene.step(dt);
}

BodyState World::body(size_t i) const { return stateOf(mImpl->scene->raw(int(i))); }

BodyState World::collider(size_t i) const { return stateOf(mImpl->scene->raw(int(mImpl->rig.bodies.size() + i))); }

std::vector<std::string> World::constraintStates() const
{
    std::vector<std::string> out(mImpl->joints.size());
    for (size_t i = 0; i < out.size(); ++i)
        if (mImpl->joints[i] >= 0)
        {
            const std::vector<uint8_t> bytes = mImpl->scene->saveJoint(mImpl->joints[i]);
            out[i].assign(bytes.begin(), bytes.end());
        }
    return out;
}

void World::setGrab(int body, const glm::dvec3& localPoint, const glm::dvec3& target)
{
    mImpl->grabBody = body;
    mImpl->grabLocal = localPoint;
    mImpl->grabTarget = target;
}

size_t World::constraintStateSize() { return ph::RigidScene::jointStateSize(ph::JointKind::SwingTwist); }

std::vector<glm::dvec3> hullTriangles(const std::vector<glm::dvec3>& points) { return ph::hullTriangles(points); }
} // namespace physanim
