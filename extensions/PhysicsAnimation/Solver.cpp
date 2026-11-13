// Physics Animation: rigid-body ragdolls on the base simulation framework. The node types hold the setup, PhysicsSolver
// reads it into the framework's Solver interface and steps a Jolt world, and the commands build, fit and bake it.

#include "PhysicsAnimation.h"
#include "World.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Node.h>
#include <lila/Relocate.h>
#include <lila/Rig.h>
#include <lila/Simulation.h>
#include <lila/SplineChain.h>
#include <lila/Transform.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace lila;
using namespace lila::scene;

namespace physanim
{
namespace
{
constexpr double kCm = 100.0; // m to cm

AttrDef number(const char* name, double def, const char* doc, uint32_t extra = 0)
{
    return {.name = name, .type = PlugType::Float, .defaultValue = def, .flags = AttrInput | AttrStorable | extra, .doc = doc};
}
AttrDef whole(const char* name, int64_t def, const char* doc)
{
    return {.name = name, .type = PlugType::Int, .defaultValue = def, .flags = AttrInput | AttrStorable, .doc = doc};
}
AttrDef limited(AttrDef a, std::optional<double> lo, std::optional<double> hi = {})
{
    a.minValue = lo;
    a.maxValue = hi;
    return a;
}
AttrDef boolean(const char* name, bool def, const char* doc)
{
    return {.name = name, .type = PlugType::Bool, .defaultValue = def, .flags = AttrInput | AttrStorable, .doc = doc};
}
AttrDef string(const char* name, const char* doc, uint32_t extra = 0)
{
    return {.name = name, .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | extra, .doc = doc};
}
AttrDef choice(const char* name, int64_t def, std::vector<std::string> names, const char* doc)
{
    return {.name = name, .type = PlugType::Enum, .defaultValue = def, .flags = AttrInput | AttrStorable, .enumNames = std::move(names), .doc = doc};
}
AttrDef vector3(const char* name, glm::dvec3 def, const char* doc)
{
    AttrDef a{.name = name, .type = PlugType::Vec3, .defaultValue = def, .flags = AttrInput | AttrStorable, .doc = doc};
    const std::string n = name;
    a.childNames = {n + "X", n + "Y", n + "Z"};
    return a;
}

void putMatrix(std::vector<double>& out, const glm::dmat4& m)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            out.push_back(m[c][r]);
}

glm::dmat4 getMatrix(const double* in)
{
    glm::dmat4 m;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            m[c][r] = in[c * 4 + r];
    return m;
}

glm::dvec3 scaleOf(const glm::dmat4& m) { return {glm::length(glm::dvec3(m[0])), glm::length(glm::dvec3(m[1])), glm::length(glm::dvec3(m[2]))}; }

BodyState stateAt(const glm::dmat4& world)
{
    const glm::dmat4 r = rigid(world);
    BodyState s;
    s.position = glm::vec3(glm::dvec3(r[3]) / kCm);
    s.rotation = glm::normalize(glm::quat(glm::quat_cast(glm::dmat3(r))));
    return s;
}

// State per body: position (cm), velocity (cm/s), rotation vector, angular velocity, then the rotation as Jolt's
// floats. Positions and velocities go through cm and back exactly: a float times 100 and divided by 100 in double
// rounds back to the same float.
constexpr size_t kBodyDoubles = 16;
constexpr size_t kColliderDoubles = 7;

void writeBody(double* s, const BodyState& b)
{
    for (int k = 0; k < 3; ++k)
    {
        s[k] = double(b.position[k]) * kCm;
        s[3 + k] = double(b.velocity[k]) * kCm;
        s[9 + k] = double(b.angularVelocity[k]);
    }
    const glm::dvec3 rv = rotationVector(glm::dquat(b.rotation));
    for (int k = 0; k < 3; ++k)
        s[6 + k] = rv[k];
    s[12] = double(b.rotation.w), s[13] = double(b.rotation.x), s[14] = double(b.rotation.y), s[15] = double(b.rotation.z);
}

BodyState readBody(const double* s)
{
    BodyState b;
    for (int k = 0; k < 3; ++k)
    {
        b.position[k] = float(s[k] / kCm);
        b.velocity[k] = float(s[3 + k] / kCm);
        b.angularVelocity[k] = float(s[9 + k]);
    }
    b.rotation = glm::quat(float(s[12]), float(s[13]), float(s[14]), float(s[15]));
    const float len = glm::length(b.rotation);
    b.rotation = len > 0.0f ? b.rotation / len : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    return b;
}

void writeCollider(double* s, const BodyState& b)
{
    for (int k = 0; k < 3; ++k)
        s[k] = double(b.position[k]);
    s[3] = double(b.rotation.w), s[4] = double(b.rotation.x), s[5] = double(b.rotation.y), s[6] = double(b.rotation.z);
}

BodyState readCollider(const double* s)
{
    BodyState b;
    b.position = glm::vec3(float(s[0]), float(s[1]), float(s[2]));
    b.rotation = glm::quat(float(s[3]), float(s[4]), float(s[5]), float(s[6]));
    return b;
}

/** Joint constraint state as bytes, four to a double so no bit pattern is ever read as a float. */
size_t constraintWords() { return (World::constraintStateSize() + 3) / 4; }

void packBytes(const std::string& bytes, double* out, size_t words)
{
    for (size_t w = 0; w < words; ++w)
    {
        uint32_t v = 0;
        for (size_t b = 0; b < 4; ++b)
            if (const size_t i = w * 4 + b; i < bytes.size())
                v |= uint32_t(static_cast<unsigned char>(bytes[i])) << (8 * b);
        out[w] = double(v);
    }
}

std::string unpackBytes(const double* in, size_t size)
{
    std::string bytes(size, '\0');
    for (size_t i = 0; i < size; ++i)
        bytes[i] = char((uint32_t(in[i / 4]) >> (8 * (i % 4))) & 0xffu);
    return bytes;
}

class PhysicsSolver final : public sim::Solver
{
public:
    explicit PhysicsSolver(NodeId node) : mNode(node) {}

    sim::Setup configure(Graph& g) override
    {
        RigDef rig = buildRig(g, mNode);
        const Node& s = *g.node(mNode);
        // The world refers to the rig and shapes, so it goes with them; every simulate() starts at a frame boundary.
        mWorld.reset();
        if (!mShapes || rig.hash != mRig.hash)
            mShapes = std::make_unique<Shapes>(rig);
        mRig = std::move(rig);
        mEnabled = flag(s, "enabled");
        mSubsteps = int(std::clamp<int64_t>(integer(s, "substeps"), 1, 64));
        Node* carrier = named(g, text(s, "carrier"));
        mCarrier = carrier && carrier->type().isA("transform") ? carrier->id() : kNoNode;
        mCarrierInertia = num(s, "carrierInertia");

        sim::Setup su;
        su.startFrame = integer(s, "startFrame");
        su.substeps = mSubsteps;
        su.stepped = int(std::clamp<int64_t>(integer(s, "stepped"), 1, 12));
        using C = sim::Channel;
        for (const BodyDef& b : mRig.bodies)
        {
            su.layout.insert(su.layout.end(), {{C::Position, 3}, {C::Velocity, 3}, {C::Rotation, 3}, {C::AngularVelocity, 3}, {C::Ignore, 4}});
            su.outputs.push_back(b.joint);
            su.sources.push_back(b.marker);
        }
        if (!mRig.colliders.empty())
            su.layout.push_back({C::Ignore, uint32_t(mRig.colliders.size() * kColliderDoubles)});
        mJoints = 0;
        for (const BodyDef& b : mRig.bodies)
            mJoints += b.parent >= 0;
        su.layout.push_back({C::Ignore, uint32_t(1 + mJoints * constraintWords())});
        // The carrier's last position and velocity, and how many of them are known yet.
        if (mCarrier)
        {
            su.layout.push_back({C::Ignore, 7});
            su.sources.push_back(mCarrier);
        }
        for (const PinDef& p : mRig.pins)
            su.sources.push_back(p.node);
        for (const ColliderDef& c : mRig.colliders)
            su.sources.push_back(c.mesh);
        for (const FieldDef& f : mRig.fields)
            su.sources.push_back(f.node);
        for (const ContactDef& c : mRig.contacts)
            su.sources.push_back(c.node);
        su.reads = su.outputs;
        su.reads.insert(su.reads.end(), su.sources.begin(), su.sources.end());
        uint64_t h = mRig.hash;
        h = sim::hashBytes(&mEnabled, sizeof mEnabled, h);
        h = sim::hashBytes(&mCarrier, sizeof mCarrier, h);
        h = sim::hashBytes(&mCarrierInertia, sizeof mCarrierInertia, h);
        const uint64_t words = constraintWords();
        su.paramsHash = sim::hashBytes(&words, sizeof words, h);
        return su;
    }

    void sample(Graph& g, std::vector<double>& in) override
    {
        auto world = [&](NodeId id) -> glm::dmat4 {
            Node* n = id ? g.node(id) : nullptr;
            return n && n->type().isA("transform") ? std::get<glm::dmat4>(g.value({id, Transform::kWorldMatrix, -1})) : glm::dmat4(1.0);
        };
        auto value = [&](NodeId id, int a, double fallback) {
            return a >= 0 ? std::get<double>(g.value({id, a, -1})) : fallback;
        };
        for (const BodyDef& b : mRig.bodies)
        {
            putMatrix(in, world(b.joint));
            in.push_back(value(b.marker, b.stiffnessAttr, 0.0));
            in.push_back(value(b.marker, b.dampingAttr, 1.0));
        }
        for (const PinDef& p : mRig.pins)
        {
            putMatrix(in, world(p.node));
            in.push_back(value(p.node, p.stiffnessAttr, 0.0));
        }
        for (const ColliderDef& c : mRig.colliders)
            putMatrix(in, world(c.mesh));
        for (const FieldDef& f : mRig.fields)
            in.push_back(value(f.node, f.magnitudeAttr, 0.0));
        if (mCarrier)
            putMatrix(in, world(mCarrier));
        for (const ContactDef& c : mRig.contacts)
        {
            in.push_back(value(c.node, c.weightAttr, 1.0));
            in.push_back(value(c.node, c.stiffnessAttr, 0.0));
        }
    }

    void reset(const std::vector<double>& in, std::vector<double>& state) override
    {
        std::fill(state.begin(), state.end(), 0.0);
        for (size_t i = 0; i < mRig.bodies.size(); ++i)
            writeBody(&state[i * kBodyDoubles], stateAt(getMatrix(&in[i * 18])));
        for (size_t c = 0; c < mRig.colliders.size(); ++c)
            writeCollider(&state[colliderBase() + c * kColliderDoubles], stateAt(getMatrix(&in[colliderInput(c)])));
        if (mCarrier)
        {
            const glm::dvec3 p(getMatrix(&in[carrierInput()])[3]);
            double* c = &state[carrierBase()];
            c[0] = p.x, c[1] = p.y, c[2] = p.z;
            c[6] = 1.0;
        }
        mWorld.reset();
    }

    void step(const std::vector<double>& in, std::vector<double>& state, const sim::StepInfo& at) override
    {
        if (!mEnabled)
        {
            reset(in, state);
            return;
        }
        // A fresh world at the first step of every frame, from the state alone, so stepping on from a cached frame and
        // playing straight through do the same thing. Within a frame the world (and its contact cache) carries on.
        const bool frameStart = at.index % mSubsteps == 1 % mSubsteps;
        if (!mWorld || frameStart || state != mLastState)
            rebuild(state, at.dt);
        Drive d;
        for (size_t i = 0; i < mRig.bodies.size(); ++i)
            d.bodies.push_back({getMatrix(&in[i * 18]), in[i * 18 + 16], in[i * 18 + 17]});
        const size_t pins = mRig.bodies.size() * 18;
        for (size_t p = 0; p < mRig.pins.size(); ++p)
            d.pins.push_back({getMatrix(&in[pins + p * 17]), in[pins + p * 17 + 16]});
        for (size_t c = 0; c < mRig.colliders.size(); ++c)
            d.colliders.push_back(getMatrix(&in[colliderInput(c)]));
        const size_t fields = colliderInput(mRig.colliders.size());
        for (size_t f = 0; f < mRig.fields.size(); ++f)
            d.fieldMagnitudes.push_back(in[fields + f]);
        const size_t contacts = carrierInput() + (mCarrier ? 16 : 0);
        for (size_t c = 0; c < mRig.contacts.size(); ++c)
            d.contacts.push_back({in[contacts + c * 2], in[contacts + c * 2 + 1]});
        d.seconds = at.seconds;
        d.period = at.period;
        // The world already gives the whole carrier's inertia; the share not felt is the share carried along.
        if (mCarrier)
        {
            // The velocity the step starts from, as the bodies' velocities are when the springs damp them.
            const double* c = &state[carrierBase()];
            d.frameVelocity = glm::dvec3(c[3], c[4], c[5]);
            d.carried = carrierAcceleration(in, state, at) * (1.0 - mCarrierInertia);
            d.frameShift = glm::dvec3(c[3], c[4], c[5]) * at.dt;
        }
        mWorld->step(d, at.dt);

        for (size_t i = 0; i < mRig.bodies.size(); ++i)
            writeBody(&state[i * kBodyDoubles], mWorld->body(i));
        for (size_t c = 0; c < mRig.colliders.size(); ++c)
            writeCollider(&state[colliderBase() + c * kColliderDoubles], mWorld->collider(c));
        const std::vector<std::string> cs = mWorld->constraintStates();
        double* blob = &state[constraintBase()];
        blob[0] = 1.0;
        size_t k = 0;
        for (size_t i = 0; i < mRig.bodies.size(); ++i)
            if (mRig.bodies[i].parent >= 0)
                packBytes(cs[i], blob + 1 + (k++) * constraintWords(), constraintWords());
        mLastState = state;
    }

    void pose(const std::vector<double>& in, const std::vector<double>& state, std::vector<glm::dmat4>& world) override
    {
        world.assign(mRig.bodies.size(), glm::dmat4(1.0));
        for (size_t i = 0; i < mRig.bodies.size(); ++i)
        {
            const glm::dmat4 anim = getMatrix(&in[i * 18]);
            if (!mEnabled || mRig.bodies[i].behaviour == Behaviour::Kinematic)
            {
                world[i] = anim;
                continue;
            }
            const double* s = &state[i * kBodyDoubles];
            glm::dmat4 m = glm::mat4_cast(glm::normalize(glm::dquat(s[12], s[13], s[14], s[15])));
            m[3] = glm::dvec4(s[0], s[1], s[2], 1.0);
            const glm::dvec3 sc = scaleOf(anim);
            for (int c = 0; c < 3; ++c)
                m[c] *= sc[c];
            world[i] = m;
        }
    }

    const RigDef& rig() const { return mRig; }

private:
    size_t colliderBase() const { return mRig.bodies.size() * kBodyDoubles; }
    size_t constraintBase() const { return colliderBase() + mRig.colliders.size() * kColliderDoubles; }
    size_t colliderInput(size_t c) const { return mRig.bodies.size() * 18 + mRig.pins.size() * 17 + c * 16; }
    size_t carrierInput() const { return colliderInput(mRig.colliders.size()) + mRig.fields.size(); }
    size_t carrierBase() const { return constraintBase() + 1 + mJoints * constraintWords(); }

    /**
     * The carrier's acceleration, cm/s^2, from its positions at the ends of the last steps. The first steps after a reset,
     * and the step a loop wraps on (where a travelling carrier jumps back), add none.
     */
    glm::dvec3 carrierAcceleration(const std::vector<double>& in, std::vector<double>& state, const sim::StepInfo& at) const
    {
        double* c = &state[carrierBase()];
        const glm::dvec3 p(getMatrix(&in[carrierInput()])[3]), last(c[0], c[1], c[2]), lastVelocity(c[3], c[4], c[5]);
        const bool wrap = at.period > 0.0 && at.index == 0;
        const glm::dvec3 v = wrap ? lastVelocity : (p - last) / at.dt;
        const glm::dvec3 a = c[6] >= 2.0 && !wrap ? (v - lastVelocity) / at.dt : glm::dvec3(0.0);
        c[0] = p.x, c[1] = p.y, c[2] = p.z;
        c[3] = v.x, c[4] = v.y, c[5] = v.z;
        c[6] = std::min(c[6] + 1.0, 2.0);
        return a;
    }

    void rebuild(const std::vector<double>& state, double dt)
    {
        std::vector<BodyState> bodies, colliders;
        for (size_t i = 0; i < mRig.bodies.size(); ++i)
            bodies.push_back(readBody(&state[i * kBodyDoubles]));
        for (size_t c = 0; c < mRig.colliders.size(); ++c)
            colliders.push_back(readCollider(&state[colliderBase() + c * kColliderDoubles]));
        std::vector<std::string> cs(mRig.bodies.size());
        const double* blob = &state[constraintBase()];
        if (blob[0] != 0.0)
        {
            size_t k = 0;
            for (size_t i = 0; i < mRig.bodies.size(); ++i)
                if (mRig.bodies[i].parent >= 0)
                    cs[i] = unpackBytes(blob + 1 + (k++) * constraintWords(), World::constraintStateSize());
        }
        mWorld.reset();
        mWorld = std::make_unique<World>(mRig, *mShapes, bodies, colliders, cs, WorldOptions{.previousStep = dt});
    }

    NodeId mNode = kNoNode;
    RigDef mRig;
    std::unique_ptr<Shapes> mShapes;
    std::unique_ptr<World> mWorld;
    std::vector<double> mLastState;
    size_t mJoints = 0;
    int mSubsteps = 1;
    bool mEnabled = true;
    NodeId mCarrier = kNoNode;
    double mCarrierInertia = 1.0;
};

bool registerTypes(ext::Context& ctx)
{
    const NodeType* transform = types::find("transform");
    if (!transform)
        return false;

    NodeType solver{.name = kSolverType,
                    .base = transform,
                    .dag = true,
                    .doc = "Physics Animation: a rigid-body ragdoll of physics markers on a character's joints, simulated "
                           "deterministically (Jolt) and recorded to an additive animation layer.",
                    .icon = "atom",
                    .create = transform->create};
    solver.ownAttrs = {
        boolean("enabled", true, "Simulate. Off, every joint follows its animation."),
        whole("startFrame", 0, "Frame the simulation starts from, at rest on the animation."),
        limited(whole("substeps", 4, "Physics steps per frame. More is stiffer and more accurate; the result never depends on playback."), 1.0, 64.0),
        vector3("gravity", {0.0, -980.0, 0.0}, "Gravity, cm/s^2."),
        boolean("ground", true, "A ground plane to land on."),
        number("groundHeight", 0.0, "Height of the ground plane, cm."),
        limited(number("groundFriction", 0.8, "Friction of the ground."), 0.0),
        boolean("selfCollision", false, "Bodies of this ragdoll collide with each other (never with their parent's)."),
        limited(whole("velocitySteps", 10, "Solver velocity iterations: more holds the joints tighter."), 2.0, 64.0),
        limited(whole("positionSteps", 2, "Solver position iterations."), 1.0, 32.0),
        limited(whole("stepped", 1, "Hold each simulated pose this many frames: 2 animates on twos."), 1.0, 12.0),
        boolean("preview", true, "Draw the simulated bodies in the viewport, simulating as the time changes."),
        string(sim::kRecordingsAttr, "Layers recorded from this simulation, muted while it samples its inputs.", AttrHidden),
        string("carrier", "Carried motion: a transform (saddle, seat) the ragdoll rides; its acceleration is felt by carrierInertia."),
        limited(number("carrierInertia", 1.0, "Share of the carrier's acceleration the bodies feel: 1 like the world, 0 carried rigidly."), 0.0),
        string("grapple", "A grapple whose contacts hold the bodies to each other as springs (by their weight and stiffness)."),
    };

    NodeType marker{.name = kMarkerType,
                    .base = transform,
                    .dag = true,
                    .doc = "A physics marker: makes its joint a rigid body (capsule, box, sphere or convex hull) with mass, "
                           "friction, joint limits and a behaviour. Its shape doubles as a selectable proxy for the joint.",
                    .icon = "capsule",
                    .create = transform->create};
    marker.ownAttrs = {
        boolean("enabled", true, "Take part in the ragdoll."),
        string("joint", "The joint (or transform) this marker drives."),
        choice("behaviour", 1, {"kinematic", "poseMatch", "simulated", "proxy"},
               "kinematic follows the animation, poseMatch is pulled toward it by springs, simulated is free, proxy is only a "
               "selectable stand-in."),
        choice("poseSpace", 0, {"local", "world", "both"},
               "Pose match toward the animated rotation relative to the parent body (local), the animated world pose, or both."),
        limited(number("poseStiffness", 6.0, "Pose match spring frequency, Hz: higher holds the animation tighter. Keyable.", AttrKeyable), 0.0),
        limited(number("poseDamping", 1.0, "Pose match damping ratio: 1 settles without bouncing. Keyable.", AttrKeyable), 0.0),
        choice("shape", 0, {"capsule", "box", "sphere", "convexHull"}, "Collision shape."),
        limited(number("radius", 5.0, "Capsule and sphere radius, cm."), 0.0),
        limited(number("length", 20.0, "Capsule length between the cap centres, cm."), 0.0),
        limited(vector3("extents", {10.0, 10.0, 10.0}, "Box size, cm."), 0.1),
        vector3("shapeOffset", {0.0, 0.0, 0.0}, "Shape centre in the joint's space, cm."),
        vector3("shapeRotate", {0.0, 0.0, 0.0}, "Shape orientation in the joint's space, degrees (a capsule lies along its X)."),
        string("hullPoints", "Convex hull points in the shape's space, JSON [[x, y, z], ...].", AttrHidden),
        limited(number("mass", 0.0, "Mass, kg. 0 derives it from the density and the shape's volume."), 0.0),
        limited(number("density", 1000.0, "Density, kg/m^3, when mass is 0."), 0.001),
        limited(number("friction", 0.8, "Friction against other bodies and colliders."), 0.0),
        limited(number("restitution", 0.0, "Bounciness: 0 lands dead, 1 bounces back fully."), 0.0, 1.0),
        limited(number("linearDamping", 0.05, "Air resistance on movement."), 0.0),
        limited(number("angularDamping", 0.05, "Air resistance on spinning."), 0.0),
        limited(whole("collisionGroup", 0, "0 collides with everything; markers sharing a group above 0 pass through each other; -1 collides with nothing."), -1.0),
        boolean("limitEnabled", true, "Hold the joint within its swing cones and twist range."),
        limited(number("limitTwist", 45.0, "Twist range either side of the limit frame, degrees."), 0.5, 180.0),
        limited(number("limitSwing1", 45.0, "Swing about the joint frame's Y either side, degrees."), 0.5, 180.0),
        limited(number("limitSwing2", 45.0, "Swing about the joint frame's Z either side, degrees."), 0.5, 180.0),
        vector3("limitOrient", {0.0, 0.0, 0.0}, "Centre of the limits relative to the rest pose: twist, swing1, swing2 in degrees."),
    };

    NodeType pin{.name = kPinType,
                 .base = transform,
                 .dag = true,
                 .doc = "A soft world-space target a marker's body is pulled toward. Animate the pin to steer the ragdoll.",
                 .icon = "pin",
                 .create = transform->create};
    pin.ownAttrs = {
        string("marker", "The marker (or its joint) pulled to the pin."),
        boolean("enabled", true, "Pull."),
        limited(number("stiffness", 4.0, "Spring frequency, Hz. Keyable: key it to 0 to let go.", AttrKeyable), 0.0),
        limited(number("damping", 1.0, "Damping ratio."), 0.0),
        boolean("orient", false, "Match the pin's rotation too."),
    };

    NodeType collider{.name = kColliderType,
                      .base = transform,
                      .dag = true,
                      .doc = "An environment collider: any mesh the ragdoll collides with, following the mesh's transform.",
                      .icon = "box-model",
                      .create = transform->create};
    collider.ownAttrs = {
        string("mesh", "The mesh to collide with."),
        boolean("enabled", true, "Collide."),
        limited(number("friction", 0.8, "Friction."), 0.0),
        limited(number("restitution", 0.0, "Bounciness."), 0.0, 1.0),
    };

    NodeType field{.name = kFieldType,
                   .base = transform,
                   .dag = true,
                   .doc = "A force field on the ragdoll's bodies: extra gravity, wind, seeded turbulence or air drag.",
                   .icon = "wind",
                   .create = transform->create};
    field.ownAttrs = {
        choice("kind", 1, {"gravity", "wind", "turbulence", "drag"}, "What the field does."),
        boolean("enabled", true, "Apply."),
        vector3("direction", {1.0, 0.0, 0.0}, "Direction of gravity or wind, world space."),
        number("magnitude", 200.0, "Gravity cm/s^2, wind speed cm/s, turbulence cm/s^2, or drag per second. Keyable.", AttrKeyable),
        limited(number("frequency", 0.5, "Turbulence: how fast it changes, per second."), 0.0),
        whole("seed", 1, "Turbulence seed: the same seed gives the same noise on every machine."),
    };

    lila::scene::relocate::registerWorldAttrs(solver.name, {.directions = {"gravity"}});
    lila::scene::relocate::registerWorldAttrs(field.name, {.directions = {"direction"}});
    for (const NodeType* t : {&marker, &pin, &collider})
        lila::scene::relocate::registerNeutral(t->name);
    for (NodeType* t : {&solver, &marker, &pin, &collider, &field})
        if (!types::registerType(ctx, std::move(*t)))
            return false;
    return true;
}

// Commands ------------------------------------------------------------------------------------------------------------

Graph& graph() { return Graph::current(); }

Node& need(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    return *n;
}

void setAttr(Graph& g, NodeId id, const char* name, PlugValue v)
{
    const int a = attr(*g.node(id), name);
    if (a >= 0)
        g.setValue({id, a, -1}, std::move(v));
}

NodeId topJoint(Graph& g, NodeId id)
{
    NodeId top = id;
    for (NodeId c = g.parent(id); c; c = g.parent(c))
        if (Node* n = g.node(c); n && n->type().isA("joint"))
            top = c;
    return top;
}

std::vector<NodeId> solvers(Graph& g)
{
    std::vector<NodeId> out;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().isA(kSolverType))
            out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
}

/** A node, or the solver its marker / pin / collider / field belongs to, or the solver of a joint's marker. */
NodeId solverFor(Graph& g, NodeId id)
{
    if (NodeId s = solverOf(g, id))
        return s;
    if (NodeId m = markerFor(g, id))
        return solverOf(g, m);
    return kNoNode;
}

/** The solver named by the flag, else the one the nodes or their skeleton already use, else the scene's only one. */
NodeId pickSolver(Graph& g, const Args& a, const std::vector<NodeId>& nodes, const char* cmd, bool create)
{
    if (a.has("solver"))
    {
        Node& s = need(g, a.get<std::string>("solver"), cmd);
        if (!s.type().isA(kSolverType))
            throw CommandError(std::string(cmd) + ": " + s.path() + " is not a physics solver");
        return s.id();
    }
    for (NodeId n : nodes)
    {
        if (NodeId s = solverFor(g, n))
            return s;
        const NodeId top = topJoint(g, n);
        for (NodeId d : g.descendants(top))
            if (NodeId m = markerFor(g, d))
                if (NodeId s = solverOf(g, m))
                    return s;
    }
    std::vector<NodeId> all = solvers(g);
    if (all.size() == 1)
        return all.front();
    if (!create)
        throw CommandError(std::string(cmd) + ": name a physics solver");
    const std::string base = nodes.empty() ? std::string("ragdoll") : g.node(topJoint(g, nodes.front()))->name();
    return g.createNode(*types::find(kSolverType), base + "_physics")->id();
}

std::vector<NodeId> nodeArgs(Graph& g, const Args& a, const char* cmd)
{
    std::vector<NodeId> out;
    for (const std::string& name : a.objects())
        out.push_back(need(g, name, cmd).id());
    return out;
}

Behaviour behaviourArg(const std::string& s)
{
    if (s == "kinematic")
        return Behaviour::Kinematic;
    if (s == "simulated")
        return Behaviour::Simulated;
    if (s == "proxy")
        return Behaviour::Proxy;
    return Behaviour::PoseMatch;
}

ShapeKind shapeArg(const std::string& s)
{
    if (s == "box")
        return ShapeKind::Box;
    if (s == "sphere")
        return ShapeKind::Sphere;
    if (s == "convexHull")
        return ShapeKind::ConvexHull;
    return ShapeKind::Capsule;
}

std::pair<int64_t, int64_t> rangeArgs(const Args& a)
{
    const json r = ops::execute("playbackOptions", json::object());
    int64_t start = sim::frameOf(r.value("minTime", 0.0)), end = sim::frameOf(r.value("maxTime", 1.0));
    if (a.has("start"))
        start = a.get<int64_t>("start");
    if (a.has("end"))
        end = a.get<int64_t>("end");
    return {start, end};
}

/** Whether the joint's rotation relative to its parent changes over the range, so its limits can be fitted. */
bool rotates(Graph& g, NodeId joint, int64_t start, int64_t end)
{
    const double saved = g.time();
    glm::dquat first(1, 0, 0, 0);
    bool moved = false;
    for (int64_t f = start; f <= end && !moved; ++f)
    {
        g.setTime(double(f) / sim::framesPerSecond());
        g.evaluate();
        const glm::dmat4 m = g.node(joint)->as<glm::dmat4>(Transform::kMatrix);
        const glm::dquat q = glm::normalize(glm::quat_cast(glm::dmat3(rigid(m))));
        if (f == start)
            first = q;
        else
            moved = std::abs(glm::dot(q, first)) < std::cos(1.0 * 3.14159265358979323846 / 360.0);
    }
    g.setTime(saved);
    g.evaluate();
    return moved;
}

void applyLimits(Graph& g, NodeId marker, const LimitFit& f)
{
    setAttr(g, marker, "limitOrient", f.orient);
    setAttr(g, marker, "limitTwist", f.twist);
    setAttr(g, marker, "limitSwing1", f.swing1);
    setAttr(g, marker, "limitSwing2", f.swing2);
}

/** Makes a marker on a joint under the solver, shape fitted, limits from the animation or defaults. */
/** Joints whose shapes the running command sized from the skeleton for want of a skinned mesh, for reportBoneSized. */
std::vector<std::string> sBoneSized;

/** Tells the user which markers were sized from the skeleton rather than fitted to a mesh, then forgets them. */
void reportBoneSized(const char* cmd)
{
    if (sBoneSized.empty())
        return;
    std::string list;
    for (size_t i = 0; i < sBoneSized.size() && i < 6; ++i)
        list += (i ? ", " : "") + sBoneSized[i];
    if (sBoneSized.size() > 6)
        list += ", ...";
    log::write(LogLevel::Warning, std::string(cmd) + ": " + std::to_string(sBoneSized.size()) + " marker shapes have no skinned faces of their own to fit (" + list +
                                      "); they are sized from the skeleton's proportions. Fit Physics Marker refits them once the mesh is skinned to them.");
    sBoneSized.clear();
}

NodeId makeMarker(Graph& g, NodeId solver, NodeId joint, Behaviour behaviour, ShapeKind shape, std::optional<LabelDefaults> defaults,
                  bool fitShapeToMesh, bool fitLimitsToAnim, std::pair<int64_t, int64_t> range)
{
    const std::string jointName = g.node(joint)->name();
    const NodeId m = g.createNode(*types::find(kMarkerType), jointName + "_marker", solver)->id();
    setAttr(g, m, "joint", g.path(joint));
    // No marker above it in the skeleton: it carries the character, so it matches the world pose too.
    bool root = true;
    for (NodeId a = g.parent(joint); a && root; a = g.parent(a))
        if (NodeId other = markerFor(g, a); other && other != m && solverOf(g, other) == solver)
            root = false;
    if (defaults)
    {
        behaviour = defaults->behaviour;
        shape = defaults->shape;
        setAttr(g, m, "poseStiffness", defaults->stiffness);
        setAttr(g, m, "limitTwist", defaults->twist);
        setAttr(g, m, "limitSwing1", defaults->swing1);
        setAttr(g, m, "limitSwing2", defaults->swing2);
        setAttr(g, m, "poseSpace", int64_t(root ? PoseSpace::Both : defaults->space));
    }
    // Local motors alone fold under gravity; the world pull is what keeps a body standing in its pose.
    else
        setAttr(g, m, "poseSpace", int64_t(PoseSpace::Both));
    setAttr(g, m, "behaviour", int64_t(behaviour));
    ShapeDesc s = fitShapeToMesh ? fitShape(g, joint, shape) : shapeOf(*g.node(m));
    if (s.fromBones)
        sBoneSized.push_back(jointName);
    s.kind = shape;
    writeShape(g, m, s);
    if (fitLimitsToAnim && rotates(g, joint, range.first, range.second))
    {
        const LimitFit f = fitLimits(g, m, range.first, range.second, 10.0);
        if (f.frames > 0)
            applyLimits(g, m, f);
    }
    return m;
}

/** Local translate and rotate that put a transform at world under its parent's current world, Euler near now. */
void poseJoint(Graph& g, NodeId id, const glm::dmat4& world)
{
    Node& n = *g.node(id);
    const auto& t = static_cast<const Transform&>(n);
    const glm::dmat4 parentWorld = std::get<glm::dmat4>(g.value({id, Transform::kParentMatrix, -1}));
    const glm::dmat4 local = glm::inverse(parentWorld) * world;
    glm::dmat3 m(local);
    for (int i = 0; i < 3; ++i)
        m[i] /= std::max(glm::length(m[i]), 1e-300);
    const glm::dmat3 r = glm::transpose(glm::dmat3(t.preRotation())) * m * glm::transpose(glm::dmat3(t.rotateAxisMatrix()));
    const glm::dvec3 rotate = eulerNear(r, RotateOrder(n.as<int64_t>(Transform::kRotateOrder)), n.as<glm::dvec3>(Transform::kRotate));
    g.setValue({id, Transform::kTranslate, -1}, glm::dvec3(local[3]));
    g.setValue({id, Transform::kRotate, -1}, rotate);
}

/** Poses the joints (parents first) and keys their translate and rotate at time on the active layer. */
void keyPose(Graph& g, const std::vector<std::pair<NodeId, glm::dmat4>>& pose, double time)
{
    std::vector<std::pair<int, size_t>> order;
    for (size_t i = 0; i < pose.size(); ++i)
    {
        int d = 0;
        for (NodeId c = g.parent(pose[i].first); c; c = g.parent(c))
            ++d;
        order.push_back({d, i});
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    g.setTime(time);
    g.evaluate();
    for (const auto& [d, i] : order)
    {
        const auto& [id, world] = pose[i];
        poseJoint(g, id, world);
        const std::string n = g.node(id)->path();
        json plugs = json::array();
        for (const char* c : {"translateX", "translateY", "translateZ", "rotateX", "rotateY", "rotateZ"})
            plugs.push_back(n + "." + c);
        ops::execute("setKeyframe", {{"args", plugs}, {"time", time}});
    }
}

std::vector<std::string> names(Graph& g, const std::vector<NodeId>& ids)
{
    std::vector<std::string> out;
    for (NodeId id : ids)
        out.push_back(g.node(id)->path());
    return out;
}

/** The meshes skinned to any of the joints. */
std::vector<NodeId> skinnedMeshes(Graph& g, const std::set<NodeId>& joints)
{
    std::vector<NodeId> out;
    for (NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n || !n->type().isA("mesh"))
            continue;
        for (NodeId j : joints)
            if (!rig::influenceWeights(g, id, j).empty())
            {
                out.push_back(id);
                break;
            }
    }
    return out;
}

void registerCommands(ext::Context& ctx)
{
    using T = ArgType;
    const FlagDef solverFlag{"solver", "s", T::String, nullptr, "The physics solver. Default: the one the skeleton uses, else the scene's only one, else a new one."};

    ctx.registerCommand({
        .name = "createPhysicsSolver",
        .doc = "A physics solver: the node a ragdoll's markers, pins, colliders and fields live under. Returns its name.",
        .flags = {{"name", "n", T::String, "physics1", "Name."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Node* s = g.createNode(*types::find(kSolverType), a.get<std::string>("name", "physics1"));
            g.select({s->id()});
            return s->path();
        },
    });

    ctx.registerCommand({
        .name = "assignPhysicsMarker",
        .doc = "Make the joints rigid bodies: a physics marker on each, its shape fitted to the joint's dominant-influence "
               "region of the skinned mesh (a bone-sized shape without one), limits fitted from the animation range when "
               "the joint moves. Returns the markers.",
        .positionalDoc = "joints or transforms (default the selection)",
        .flags = {
            solverFlag,
            {"behaviour", "b", T::String, "poseMatch", "Behaviour.", {"kinematic", "poseMatch", "simulated", "proxy"}},
            {"shape", "sh", T::String, "capsule", "Shape.", {"capsule", "box", "sphere", "convexHull"}},
            {"fitShape", "fs", T::Bool, true, "Fit the shape to the skinned mesh."},
            {"fitLimits", "fl", T::Bool, true, "Fit limits from the animation range where the joint moves."},
            {"start", "", T::Int, nullptr, "First frame for fitting limits. Default: the playback start."},
            {"end", "", T::Int, nullptr, "Last frame for fitting limits. Default: the playback end."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> joints = nodeArgs(g, a, "assignPhysicsMarker");
            if (joints.empty())
                throw CommandError("assignPhysicsMarker: select or name joints");
            for (NodeId j : joints)
                if (!g.node(j)->type().isA("transform"))
                    throw CommandError("assignPhysicsMarker: " + g.node(j)->path() + " is not a joint or transform");
            // Parents first, so each marker sees whether one sits above it.
            std::stable_sort(joints.begin(), joints.end(), [&](NodeId x, NodeId y) {
                int dx = 0, dy = 0;
                for (NodeId c = g.parent(x); c; c = g.parent(c))
                    ++dx;
                for (NodeId c = g.parent(y); c; c = g.parent(c))
                    ++dy;
                return dx < dy;
            });
            const NodeId solver = pickSolver(g, a, joints, "assignPhysicsMarker", true);
            const auto range = rangeArgs(a);
            std::vector<NodeId> made;
            for (NodeId j : joints)
                made.push_back(makeMarker(g, solver, j, behaviourArg(a.get<std::string>("behaviour", "poseMatch")),
                                          shapeArg(a.get<std::string>("shape", "capsule")), std::nullopt, a.get<bool>("fitShape", true),
                                          a.get<bool>("fitLimits", true), range));
            reportBoneSized("assignPhysicsMarker");
            g.select(made);
            return names(g, made);
        },
    });

    ctx.registerCommand({
        .name = "physicsMarkersFromTemplate",
        .doc = "One-click ragdoll: a marker on every joint below the root that carries a semantic label (pelvis, spine_02, "
               "hand_l ...: from Rig Templates' naming, the joint's type label or its name), with that label's default shape, "
               "behaviour and limits, shapes fitted to the skinned mesh. Fingers, twist and IK joints are left out. Returns the markers.",
        .positionalDoc = "a joint of the skeleton (default the selection)",
        .flags = {
            solverFlag,
            {"fitLimits", "fl", T::Bool, false, "Fit limits from the animation range instead of the label defaults."},
            {"start", "", T::Int, nullptr, "First frame for fitting limits."},
            {"end", "", T::Int, nullptr, "Last frame for fitting limits."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> given = nodeArgs(g, a, "physicsMarkersFromTemplate");
            if (given.empty())
                throw CommandError("physicsMarkersFromTemplate: select or name a joint of the skeleton");
            const NodeId top = topJoint(g, given.front());
            std::vector<NodeId> joints{top};
            for (NodeId d : g.descendants(top))
                if (Node* n = g.node(d); n && n->type().isA("joint"))
                    joints.push_back(d);
            std::vector<std::pair<NodeId, LabelDefaults>> labelled;
            for (NodeId j : joints)
                if (auto d = labelDefaults(semanticLabel(g, j)))
                    if (!markerFor(g, j))
                        labelled.push_back({j, *d});
            if (labelled.empty())
                throw CommandError("physicsMarkersFromTemplate: no joint below " + g.node(top)->path() + " has a semantic label");
            std::stable_sort(labelled.begin(), labelled.end(), [&](const auto& x, const auto& y) {
                int dx = 0, dy = 0;
                for (NodeId c = g.parent(x.first); c; c = g.parent(c))
                    ++dx;
                for (NodeId c = g.parent(y.first); c; c = g.parent(c))
                    ++dy;
                return dx < dy;
            });
            const NodeId solver = pickSolver(g, a, {top}, "physicsMarkersFromTemplate", true);
            const auto range = rangeArgs(a);
            std::vector<NodeId> made;
            for (const auto& [j, d] : labelled)
                made.push_back(makeMarker(g, solver, j, d.behaviour, d.shape, d, true, a.get<bool>("fitLimits", false), range));
            reportBoneSized("physicsMarkersFromTemplate");
            g.select(made);
            return names(g, made);
        },
    });

    ctx.registerCommand({
        .name = "fitPhysicsMarker",
        .doc = "Refit markers: the shape to the joint's dominant-influence region of the skinned mesh, and/or the swing "
               "cones and twist range to the motion over the animation range. Returns the markers.",
        .positionalDoc = "markers or their joints (default the selection)",
        .flags = {
            {"shape", "sh", T::String, nullptr, "Change the shape kind too.", {"capsule", "box", "sphere", "convexHull"}},
            {"fitShape", "fs", T::Bool, true, "Refit the shape."},
            {"limits", "l", T::Bool, true, "Refit the limits from the animation."},
            {"margin", "mg", T::Float, 10.0, "Degrees added around the motion.", {}, {}, 0.0},
            {"start", "", T::Int, nullptr, "First frame. Default: the playback start."},
            {"end", "", T::Int, nullptr, "Last frame. Default: the playback end."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> markers;
            for (NodeId id : nodeArgs(g, a, "fitPhysicsMarker"))
                if (isMarker(*g.node(id)))
                    markers.push_back(id);
                else if (NodeId m = markerFor(g, id))
                    markers.push_back(m);
            if (markers.empty())
                throw CommandError("fitPhysicsMarker: select or name markers or joints with markers");
            const auto range = rangeArgs(a);
            for (NodeId m : markers)
            {
                const NodeId joint = jointOf(g, *g.node(m));
                if (!joint)
                    throw CommandError("fitPhysicsMarker: " + g.node(m)->path() + " has no joint");
                if (a.get<bool>("fitShape", true))
                {
                    const ShapeKind kind = a.has("shape") ? shapeArg(a.get<std::string>("shape")) : shapeOf(*g.node(m)).kind;
                    const ShapeDesc fitted = fitShape(g, joint, kind);
                    if (fitted.fromBones)
                        sBoneSized.push_back(g.node(joint)->name());
                    writeShape(g, m, fitted);
                }
                if (a.get<bool>("limits", true))
                {
                    const LimitFit f = fitLimits(g, m, range.first, range.second, a.get<double>("margin", 10.0));
                    if (f.frames > 0)
                        applyLimits(g, m, f);
                }
            }
            reportBoneSized("fitPhysicsMarker");
            return names(g, markers);
        },
    });

    ctx.registerCommand({
        .name = "retargetPhysicsMarker",
        .doc = "Point a marker at a different joint, keeping its settings (refit the shape with fit). Returns the marker.",
        .positionalDoc = "marker (or its joint), then the new joint",
        .flags = {{"fit", "f", T::Bool, false, "Refit the shape to the new joint."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> ids = nodeArgs(g, a, "retargetPhysicsMarker");
            if (ids.size() != 2)
                throw CommandError("retargetPhysicsMarker: give the marker and the new joint");
            NodeId m = isMarker(*g.node(ids[0])) ? ids[0] : markerFor(g, ids[0]);
            if (!m)
                throw CommandError("retargetPhysicsMarker: " + g.node(ids[0])->path() + " has no physics marker");
            if (!g.node(ids[1])->type().isA("transform"))
                throw CommandError("retargetPhysicsMarker: " + g.node(ids[1])->path() + " is not a joint or transform");
            setAttr(g, m, "joint", g.path(ids[1]));
            if (a.get<bool>("fit", false))
                writeShape(g, m, fitShape(g, ids[1], shapeOf(*g.node(m)).kind));
            return g.node(m)->path();
        },
    });

    ctx.registerCommand({
        .name = "createPhysicsPin",
        .doc = "A soft pin pulling a marker's body toward a world-space target, placed at the joint. Animate it to steer "
               "the ragdoll. Returns the pin.",
        .positionalDoc = "marker or joint (default the selection)",
        .flags = {{"stiffness", "st", T::Float, 4.0, "Spring frequency, Hz.", {}, {}, 0.0}, {"orient", "o", T::Bool, false, "Match rotation too."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> ids = nodeArgs(g, a, "createPhysicsPin");
            if (ids.size() != 1)
                throw CommandError("createPhysicsPin: give one marker or joint");
            NodeId m = isMarker(*g.node(ids[0])) ? ids[0] : markerFor(g, ids[0]);
            if (!m)
                throw CommandError("createPhysicsPin: " + g.node(ids[0])->path() + " has no physics marker");
            const NodeId solver = solverOf(g, m);
            const NodeId joint = jointOf(g, *g.node(m));
            Node* p = g.createNode(*types::find(kPinType), g.node(m)->name() + "_pin", solver);
            const NodeId pid = p->id();
            setAttr(g, pid, "marker", g.path(m));
            setAttr(g, pid, "stiffness", a.get<double>("stiffness", 4.0));
            setAttr(g, pid, "orient", a.get<bool>("orient", false));
            g.evaluate();
            if (joint)
            {
                const glm::dmat4 world = g.node(joint)->as<glm::dmat4>(Transform::kWorldMatrix);
                const glm::dmat4 parent = g.node(solver)->as<glm::dmat4>(Transform::kWorldMatrix);
                g.setValue({pid, Transform::kTranslate, -1}, glm::dvec3(glm::inverse(parent) * world[3]));
            }
            g.select({pid});
            return g.node(pid)->path();
        },
    });

    ctx.registerCommand({
        .name = "createPhysicsCollider",
        .doc = "Make meshes environment colliders the ragdoll lands on and pushes against. Returns the collider nodes.",
        .positionalDoc = "meshes (default the selection)",
        .flags = {solverFlag},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> meshes = nodeArgs(g, a, "createPhysicsCollider");
            if (meshes.empty())
                throw CommandError("createPhysicsCollider: select or name meshes");
            const NodeId solver = pickSolver(g, a, {}, "createPhysicsCollider", true);
            std::vector<NodeId> made;
            for (NodeId mesh : meshes)
            {
                if (!g.node(mesh)->type().isA("mesh"))
                    throw CommandError("createPhysicsCollider: " + g.node(mesh)->path() + " is not a mesh");
                Node* c = g.createNode(*types::find(kColliderType), g.node(mesh)->name() + "_collider", solver);
                setAttr(g, c->id(), "mesh", g.path(mesh));
                made.push_back(c->id());
            }
            return names(g, made);
        },
    });

    ctx.registerCommand({
        .name = "createPhysicsField",
        .doc = "A force field on the ragdoll: extra gravity, wind, seeded turbulence or air drag. Returns the field.",
        .flags = {solverFlag,
                  {"kind", "k", T::String, "wind", "Kind.", {"gravity", "wind", "turbulence", "drag"}},
                  {"magnitude", "m", T::Float, nullptr, "Strength (see the field's magnitude)."},
                  {"direction", "d", T::Vec3, nullptr, "Direction, world space."},
                  {"seed", "", T::Int, nullptr, "Turbulence seed."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const NodeId solver = pickSolver(g, a, {}, "createPhysicsField", true);
            const std::string kind = a.get<std::string>("kind", "wind");
            Node* f = g.createNode(*types::find(kFieldType), kind + "Field1", solver);
            const NodeId id = f->id();
            setAttr(g, id, "kind", int64_t(kind == "gravity" ? 0 : kind == "turbulence" ? 2 : kind == "drag" ? 3 : 1));
            if (a.has("magnitude"))
                setAttr(g, id, "magnitude", a.get<double>("magnitude"));
            if (a.has("direction"))
            {
                const auto d = a.flags["direction"].get<std::vector<double>>();
                setAttr(g, id, "direction", glm::dvec3(d.at(0), d.at(1), d.at(2)));
            }
            if (a.has("seed"))
                setAttr(g, id, "seed", a.get<int64_t>("seed"));
            g.select({id});
            return g.node(id)->path();
        },
    });

    ctx.registerCommand({
        .name = "recordPhysics",
        .doc = "Bake the ragdoll onto the joints: onto a new additive layer (mute it to compare, delete it to revert), or "
               "replacing their keys over the range. Returns the layer, or the keyed joints when replacing.",
        .positionalDoc = "solver, or its markers or joints (default the selection, else the scene's only solver)",
        .flags = {{"mode", "m", T::String, "additive", "additive | replace", {"additive", "replace"}},
                  {"start", "", T::Int, nullptr, "First frame. Default: the loop or playback range."},
                  {"end", "", T::Int, nullptr, "Last frame."},
                  {"tolerance", "tol", T::Float, 0.0, "Additive only: reduce keys while within this (cm); 0 keys every frame.", {}, {}, 0.0}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const NodeId solver = pickSolver(g, a, nodeArgs(g, a, "recordPhysics"), "recordPhysics", false);
            if (a.get<std::string>("mode", "additive") != "replace")
            {
                sim::RecordOptions o;
                if (a.has("start"))
                    o.start = a.get<int64_t>("start");
                if (a.has("end"))
                    o.end = a.get<int64_t>("end");
                o.tolerance = a.get<double>("tolerance", 0.0);
                try
                {
                    return g.node(sim::record(g, solver, o))->path();
                }
                catch (const std::invalid_argument& e)
                {
                    throw CommandError(std::string("recordPhysics: ") + e.what());
                }
            }
            const auto range = rangeArgs(a);
            std::vector<std::vector<std::pair<NodeId, glm::dmat4>>> frames;
            try
            {
                for (int64_t f = range.first; f <= range.second; ++f)
                {
                    std::shared_ptr<const sim::Frame> fr = sim::simulate(g, solver, f);
                    const sim::Info info = sim::info(g, solver);
                    std::vector<std::pair<NodeId, glm::dmat4>> pose;
                    for (size_t i = 0; i < info.outputs.size() && i < fr->world.size(); ++i)
                        pose.push_back({info.outputs[i], fr->world[i]});
                    frames.push_back(std::move(pose));
                }
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("recordPhysics: ") + e.what());
            }
            const double saved = g.time();
            for (size_t k = 0; k < frames.size(); ++k)
                keyPose(g, frames[k], double(range.first + int64_t(k)) / sim::framesPerSecond());
            g.setTime(saved);
            g.evaluate();
            json out = json::array();
            if (!frames.empty())
                for (const auto& [id, m] : frames.front())
                    out.push_back(g.node(id)->path());
            return out;
        },
    });

    ctx.registerCommand({
        .name = "snapToSimulation",
        .doc = "Key the joints to the simulated pose at a frame (default the current one), on the active layer. Returns the keyed joints.",
        .positionalDoc = "solver, or its markers or joints (default the selection, else the scene's only solver)",
        .flags = {{"frame", "f", T::Int, nullptr, "Frame. Default: the current one."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const NodeId solver = pickSolver(g, a, nodeArgs(g, a, "snapToSimulation"), "snapToSimulation", false);
            const int64_t frame = a.has("frame") ? a.get<int64_t>("frame") : sim::frameOf(g.time());
            std::vector<std::pair<NodeId, glm::dmat4>> pose;
            try
            {
                std::shared_ptr<const sim::Frame> fr = sim::simulate(g, solver, frame);
                const sim::Info info = sim::info(g, solver);
                for (size_t i = 0; i < info.outputs.size() && i < fr->world.size(); ++i)
                    pose.push_back({info.outputs[i], fr->world[i]});
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("snapToSimulation: ") + e.what());
            }
            const double saved = g.time();
            keyPose(g, pose, double(frame) / sim::framesPerSecond());
            g.setTime(saved);
            g.evaluate();
            json out = json::array();
            for (const auto& [id, m] : pose)
                out.push_back(g.node(id)->path());
            return out;
        },
    });

    ctx.registerCommand({
        .name = "physicsApplyPose",
        .doc = "Key joints to world matrices at the current time on the active layer (what Live mode's Apply runs). "
               "Returns the number of joints keyed.",
        .flags = {{"pose", "p", T::Any, nullptr, "{joint: [16 numbers, column-major world matrix]}."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            if (!a.has("pose") || !a.flags["pose"].is_object())
                throw CommandError("physicsApplyPose: give pose as {joint: matrix}");
            std::vector<std::pair<NodeId, glm::dmat4>> pose;
            for (const auto& [name, m] : a.flags["pose"].items())
            {
                Node& n = need(g, name, "physicsApplyPose");
                if (!n.type().isA("transform") || !m.is_array() || m.size() != 16)
                    throw CommandError("physicsApplyPose: " + name + " needs a transform and 16 numbers");
                glm::dmat4 w;
                for (int k = 0; k < 16; ++k)
                    w[k / 4][k % 4] = m[size_t(k)].get<double>();
                pose.push_back({n.id(), w});
            }
            const double t = g.time();
            keyPose(g, pose, t);
            g.setTime(t);
            g.evaluate();
            return pose.size();
        },
    });

    ctx.registerCommand({
        .name = "physicsProxyDisplay",
        .doc = "Show or hide the meshes skinned to a ragdoll's joints, leaving the marker bodies as a light stand-in to "
               "animate with. Returns the meshes changed.",
        .positionalDoc = "solver, or its markers or joints (default the selection, else the scene's only solver)",
        .flags = {{"hideMeshes", "hm", T::Bool, true, "Hide (true) or show (false) the skinned meshes."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const NodeId solver = pickSolver(g, a, nodeArgs(g, a, "physicsProxyDisplay"), "physicsProxyDisplay", false);
            std::set<NodeId> joints;
            for (NodeId m : membersOf(g, solver, kMarkerType))
                if (NodeId j = jointOf(g, *g.node(m)))
                    joints.insert(j);
            const bool hide = a.get<bool>("hideMeshes", true);
            std::vector<NodeId> meshes = skinnedMeshes(g, joints);
            for (NodeId mesh : meshes)
                g.setValue({mesh, Transform::kVisibility, -1}, !hide);
            return names(g, meshes);
        },
    });

    ctx.registerCommand({
        .name = "selectPhysicsMarkers",
        .doc = "Select the physics markers of joints (or every marker of a solver). Returns them.",
        .positionalDoc = "joints or solvers (default the selection)",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> out;
            for (NodeId id : nodeArgs(g, a, "selectPhysicsMarkers"))
            {
                if (g.node(id)->type().isA(kSolverType))
                    for (NodeId m : membersOf(g, id, kMarkerType))
                        out.push_back(m);
                else if (isMarker(*g.node(id)))
                    out.push_back(id);
                else if (NodeId m = markerFor(g, id))
                    out.push_back(m);
            }
            g.select(out);
            return names(g, out);
        },
    });

    ctx.registerCommand({
        .name = "physicsMarkerInfo",
        .doc = "A marker's joint, semantic label, behaviour, shape and parent body, as the solver reads it.",
        .positionalDoc = "markers or joints (default the selection)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            json out = json::array();
            for (NodeId id : nodeArgs(g, a, "physicsMarkerInfo"))
            {
                const NodeId m = isMarker(*g.node(id)) ? id : markerFor(g, id);
                if (!m)
                    continue;
                const Node& mn = *g.node(m);
                const NodeId joint = jointOf(g, mn);
                json j = {{"marker", mn.path()},
                          {"joint", joint ? g.node(joint)->path() : std::string()},
                          {"label", joint ? semanticLabel(g, joint) : std::string()},
                          {"behaviour", integer(mn, "behaviour")},
                          {"shape", integer(mn, "shape")},
                          {"parent", nullptr}};
                if (NodeId s = solverOf(g, m))
                {
                    const RigDef rig = buildRig(g, s);
                    for (const BodyDef& b : rig.bodies)
                        if (b.marker == m && b.parent >= 0)
                            j["parent"] = g.node(rig.bodies[size_t(b.parent)].marker)->path();
                }
                out.push_back(j);
            }
            return out;
        },
    });
}
/**
 * splineDynamics' jolt mode: a solver of its own with a body per segment of the chain, jointed to the next, its first
 * jointed to a kinematic body on the chain's parent so the chain hangs from the animation. Additive pulls every body
 * toward its animated pose with soft springs; full leaves them free inside their limits.
 */
NodeId attachToSplineChain(Graph& g, NodeId chain, const std::vector<NodeId>& joints, const spline::DynamicsOptions& o)
{
    for (NodeId m : o.colliders)
        if (!g.node(m) || !g.node(m)->type().isA("mesh"))
            throw std::invalid_argument("jolt dynamics collide with meshes; " + (g.node(m) ? g.node(m)->path() : std::string("a collider")) +
                                        " is not one");
    for (NodeId j : joints)
        if (NodeId m = markerFor(g, j))
            throw std::invalid_argument(g.node(j)->path() + " already has a physics marker, " + g.node(m)->path());
    const std::string base = o.name.empty() ? g.node(chain)->name() + "_physics" : o.name;
    const NodeId solver = g.createNode(*types::find(kSolverType), base)->id();
    setAttr(g, solver, "gravity", glm::dvec3(0.0, -980.0, 0.0) * o.gravity.value_or(o.full ? 1.0 : 0.0));
    setAttr(g, solver, "ground", false);
    if (o.carrier)
    {
        setAttr(g, solver, "carrier", g.path(o.carrier));
        setAttr(g, solver, "carrierInertia", o.carrierInertia);
    }
    const std::pair<int64_t, int64_t> range{0, 0};
    if (const NodeId parent = g.parent(joints.front()); parent && g.node(parent)->type().isA("transform"))
    {
        const NodeId m = makeMarker(g, solver, parent, Behaviour::Kinematic, ShapeKind::Sphere, std::nullopt, false, false, range);
        setAttr(g, m, "radius", 1.0);
        setAttr(g, m, "collisionGroup", int64_t(-1));
    }
    const Behaviour behaviour = o.full ? Behaviour::Simulated : Behaviour::PoseMatch;
    for (size_t i = 0; i < joints.size(); ++i)
    {
        const bool anchored = i == 0 && !g.parent(joints.front());
        const NodeId m = makeMarker(g, solver, joints[i], anchored ? Behaviour::Kinematic : behaviour, ShapeKind::Capsule, std::nullopt, true,
                                    false, range);
        setAttr(g, m, "poseStiffness", o.stiffness.value_or(5.0));
        setAttr(g, m, "poseDamping", o.damping.value_or(0.4));
        setAttr(g, m, "limitTwist", o.full ? 60.0 : 30.0);
        setAttr(g, m, "limitSwing1", o.full ? 90.0 : 60.0);
        setAttr(g, m, "limitSwing2", o.full ? 90.0 : 60.0);
        if (o.radius)
            setAttr(g, m, "radius", *o.radius);
    }
    for (NodeId mesh : o.colliders)
    {
        Node* c = g.createNode(*types::find(kColliderType), g.node(mesh)->name() + "_collider", solver);
        setAttr(g, c->id(), "mesh", g.path(mesh));
    }
    return solver;
}
} // namespace

bool registerScene(ext::Context& ctx)
{
    initJolt();
    if (!registerTypes(ctx))
        return false;
    ctx.add(spline::dynamicsProviders(), "splineDynamics", "jolt",
            spline::DynamicsProvider{.name = "jolt",
                                     .doc = "Rigid bodies per segment (Jolt): collides with meshes, pulled to the animation by soft springs.",
                                     .attach = attachToSplineChain});
    if (!sim::registerSolverType(ctx, {.nodeType = kSolverType, .create = [](Graph&, NodeId node) -> std::unique_ptr<sim::Solver> {
                                           return std::make_unique<PhysicsSolver>(node);
                                       }}))
        return false;
    registerCommands(ctx);
    registerGrappleRagdoll(ctx);
    return true;
}
} // namespace physanim

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    if (!physanim::registerScene(ctx))
        return 1;
    physanim::registerUi(ctx);
    return 0;
}
