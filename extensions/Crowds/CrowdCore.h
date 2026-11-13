#pragma once

#include "Orca.h"

#include <lila/Motion.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

/**
 * The crowd runtime, with no scene in it: hundreds of agents, each playing generated motion and asking its behaviour for
 * the next request near the end of it, requests held back up to a lookahead so they share one batched inference (after
 * CrowdBricks by Patch, Apache-2.0). Space is SOMA's: meters, Y up, ground (x, z), 30 Hz motion.
 *
 * Agents far from the focus (or past the generated budget, or when the engine is missing) play gait cycles instead:
 * locomotion built from the gait library's windows, steered directly, no inference. Everything an agent is, between
 * steps, lives in a flat state vector (so the simulation framework can cache and resume it); generated clips sit in a
 * store the state refers to by id, ids handed out in order so a state always names the same clips.
 */
namespace crowds
{
using lila::motion::Contacts;
using lila::motion::Pose;
using lila::motion::Request;
using lila::motion::Result;

/** What makes motion: the engine, or a stand-in in tests. */
class Generator
{
public:
    virtual ~Generator() = default;
    virtual std::vector<Result> generate(std::span<const Request> requests) = 0;
    /** The device, for reports ("DirectML", "CPU"). */
    virtual std::string device() const = 0;
};

enum class Behaviour : uint8_t
{
    Stand,
    Wander,
    GoTo,
    FollowPath,
    Queue,
    Idle, // a group standing round a centre, turning to each other now and then
    Spot, // walk to an interaction spot and perform it (sit, pick up, duck)
    Flee,
    Chase,
};
const char* behaviourName(Behaviour b);
/** Throws std::invalid_argument for an unknown name. */
Behaviour behaviourFrom(const std::string& name);

/** One interaction spot placed in the world: its targets in SOMA space. */
struct PlacedSpot
{
    std::vector<lila::motion::PlacedTarget> targets;
    glm::vec2 at{0.0f};
    float heading = 0.0f;
};

/** Walkable lanes (sidewalks, crosswalks, paths), joined end to end. */
struct Network
{
    struct Lane
    {
        std::vector<glm::vec2> points;
        float width = 2.0f;
        bool crossing = false;
        /** A crossing at a signalled junction: the junction (settlement and index), the phase people walk in, of how many. */
        std::string junction;
        int phase = -1, phases = 0;
    };
    std::vector<Lane> lanes;
    /** Per lane end (lane * 2, +1 for its far end), the lane ends it joins. */
    std::vector<std::vector<int>> joins;

    bool empty() const { return lanes.empty(); }
    /** Joins ends that lie within tolerance of each other (meters). */
    void connect(float tolerance = 0.75f);
    /** The lane point nearest p: lane index and arc length along it; -1 when empty. */
    std::pair<int, float> nearest(glm::vec2 p) const;
    /** A walk from the lane point nearest a to the one nearest b along lanes, as ground points. */
    std::vector<glm::vec2> route(glm::vec2 a, glm::vec2 b) const;
    /** A point on the network, chosen by u in [0, 1) by length. */
    glm::vec2 pointAt(double u, int* lane = nullptr) const;
    double length() const;
};

struct Group
{
    std::string name;
    Behaviour behaviour = Behaviour::Wander;
    std::string style = "walk";
    /** Wander, Idle: centre and radius (meters); Wander picks goals in it, or on the network when it has one. */
    glm::vec2 center{0.0f};
    float radius = 10.0f;
    /** Turns the random directions agents pick (wander goals, spread round a goal), so a turned crowd decides alike. */
    float yaw = 0.0f;
    /** Wander: polygon to stay in, when not empty. */
    std::vector<glm::vec2> area;
    float pauseMin = 1.0f, pauseMax = 4.0f;
    /** GoTo, Flee, Chase: index into the sampled points (a node); -1 uses goal. */
    int input = -1;
    glm::vec2 goal{0.0f};
    /** FollowPath and Queue: the path; Queue's head is its last point. */
    std::vector<glm::vec2> path;
    bool loop = false;
    float spacing = 0.9f;
    /** Queue: seconds each agent at the head takes before it leaves. */
    float serve = 6.0f;
    std::vector<PlacedSpot> spots;
    /** Spot: seconds to stay after the last target (0 for good); then the agent wanders off. */
    float stay = 0.0f;
    /** Flee: distance at which the threat is noticed; Chase: distance at which the target counts as caught. */
    float range = 8.0f;
    /** Chase: chase the nearest agent of this group instead of a point; -1 for none. */
    int prey = -1;
    bool useNetwork = false;
    /** After GoTo, Queue (leaving) and Spot (staying over): what the agent does next. */
    Behaviour then = Behaviour::Stand;
};

struct AgentSpec
{
    glm::vec2 start{0.0f};
    float heading = 0.0f; // radians about +Y, 0 faces +Z
    uint32_t seed = 0;
    int group = 0;
    int character = 0;
    /** Multiplies the gait's speed. */
    float speed = 1.0f;
    float radius = 0.28f;
    /** Seconds after the start before the agent appears. */
    float appear = 0.0f;
    /** FollowPath: where along the path it starts (meters); Queue/Idle: its place. */
    float along = 0.0f;
    int slot = 0;
};

enum class Detail : uint8_t
{
    Auto,      // generated near the focus, within the budget; cycles elsewhere
    Generated, // every agent generates (within the budget)
    Cycles,    // no inference at all
};

struct Options
{
    int batch = 16;
    /** Seconds early an agent may ask for its next motion so batches fill. */
    double lookahead = 0.15;
    double steerDegrees = 35.0;
    double minPlay = 0.35;
    bool avoidance = true;
    orca::Options orca;
    Detail detail = Detail::Auto;
    /** Sampled point the detail is measured from; -1: every agent counts as near. */
    int focusInput = -1;
    double generatedRadius = 25.0;
    int generatedMax = 48;
    uint32_t seed = 1;
};

struct Setup
{
    std::vector<AgentSpec> agents;
    std::vector<Group> groups;
    Network network;
    std::vector<orca::Obstacle> obstacles;
    Options options;
};

/** A generated clip as stored: root positions and the model joints' rotations, 16-bit, plus foot contacts. */
struct PackedClip
{
    int frames = 0;
    std::vector<float> root;        // frames * 3
    std::vector<int16_t> rotations; // frames * 30 * 4 (x, y, z, w)
    std::vector<uint8_t> contacts;  // frames * 4, 0..255
};

/** Locomotion cycle of one gait for agents without inference: in place, facing +Z, 30 Hz. */
struct Cycle
{
    std::string style;
    double speed = 1.3, period = 1.0;
    std::vector<Pose> frames;
};

enum class MotionKind : uint8_t
{
    Clip,  // playing a generated clip
    Hold,  // holding a clip's pose (reached a stand or a seat)
    Cycle, // playing a gait cycle, steered directly
};

/** One agent between steps, flattened into the state vector (every field a double). */
struct AgentState
{
    double alive = 0;
    double x = 0, z = 0;       // ground position shown (root's ground point plus offset)
    double vx = 0, vz = 0;     // smoothed velocity
    double hx = 0, hz = 1;     // facing
    double px = 0, pz = 1;     // heading the current motion was planned for
    double dx = 0, dz = 1;     // desired heading this step
    double ox = 0, oz = 0;     // correction added to the clip's root (pushes from overlaps)
    double motion = 0;         // MotionKind
    double clip = -1, t = 0, transition = 0;
    double prevClip = -1, prevT = 0;
    double cycle = 0, phase = 0; // gait cycle index and phase 0..1 (cycle motion; phase also tracks clips)
    double replan = 0;
    double exact = 0;          // generates only at its transition point (interactions)
    double gait = 0;           // gait index into the library being walked
    double moving = 0;         // the behaviour wants to move
    // Behaviour state.
    double task = 0;           // behaviour-specific phase
    double goalX = 0, goalZ = 0;
    double timer = 0;
    double count = 0;          // goals reached, generations asked, and the like (feeds the seed)
    double route = -1, along = 0;
    double target = -1, hold = 0;
    double generations = 0;
    double label = 0;
    double intent = 0;         // what the next generation is for (walk, stop, turn, target)
    double settle = 0;         // 1: hold when the motion runs out; 2: holding until timer
};
static_assert(sizeof(AgentState) % sizeof(double) == 0);
inline constexpr size_t kAgentDoubles = sizeof(AgentState) / sizeof(double);
/** State header: next clip id, step count. */
inline constexpr size_t kHeaderDoubles = 2;

struct Stats
{
    int64_t generations = 0, batches = 0, fullBatches = 0;
    double inferenceMs = 0.0;
    int generatedAgents = 0, cycleAgents = 0, alive = 0;
    std::string device;
    std::string note; // why agents play cycles, when they do
};

class Runtime
{
public:
    /** gen may be null: every agent plays cycles, and stats().note says so. */
    Runtime(Setup setup, const lila::motion::GaitLibrary& gaits, Generator* gen, std::string whyNoGenerator = {});
    ~Runtime();

    size_t stateSize() const { return kHeaderDoubles + kAgentDoubles * mSetup.agents.size(); }
    /** The state at the start. */
    void reset(std::vector<double>& state);
    /** One step of dt seconds ending at time (seconds since the start), with the points sampled then. */
    void step(std::vector<double>& state, double dt, double time, std::span<const glm::vec2> points);

    /** An agent's SOMA local pose in a state (root on the ground plane, y = 0 at the floor). False when not alive. */
    bool pose(const std::vector<double>& state, size_t agent, Pose& out) const;
    const AgentState& agent(const std::vector<double>& state, size_t i) const;
    /** Foot contacts of an agent's motion now; zero for cycles and holds. */
    Contacts contacts(const std::vector<double>& state, size_t agent) const;
    /** Root world placement (x, z) and facing for drawing far agents without a pose. */
    const Setup& setup() const { return mSetup; }
    const Stats& stats() const { return mStats; }
    const lila::motion::GaitLibrary& gaits() const { return mGaits; }
    const std::vector<Cycle>& cycles() const { return mCycles; }
    /** What an agent is doing, for labels: "walk", "queue", "sit: Sit 2/3". */
    std::string label(const std::vector<double>& state, size_t agent) const;
    /** Clips held now (memory check). */
    size_t clipCount() const { return mClips.size(); }

private:
    struct Want;
    AgentState& at(std::vector<double>& state, size_t i) const;
    void sampleClip(int64_t id, double t, Pose& out, Contacts* contacts = nullptr) const;
    void history(const AgentState& a, std::array<Pose, 4>& ctx) const;
    void cyclePose(const AgentState& a, Pose& out) const;
    void decide(AgentState& a, size_t i, double time, std::span<const glm::vec2> points, std::vector<double>& state);
    bool wantsGenerated(const AgentState& a, size_t i, std::span<const glm::vec2> points) const;
    Request makeRequest(AgentState& a, size_t i, int& early);
    std::vector<glm::vec2> pathFor(const AgentState& a) const;
    int cycleFor(const std::string& style, double speed) const;
    int64_t storeClip(Result&& r, int64_t id);
    int64_t storeRoute(std::vector<glm::vec2> route, int64_t id);
    uint32_t hash(size_t agent, uint32_t salt, double count) const;
    float random(size_t agent, uint32_t salt, double count) const;

    Setup mSetup;
    const lila::motion::GaitLibrary& mGaits;
    Generator* mGen = nullptr;
    std::vector<Cycle> mCycles;
    std::vector<int> mGroupSize;
    std::vector<int> mModelJoints; // SOMA skeleton index per model joint
    std::vector<std::shared_ptr<const PackedClip>> mClips;
    std::vector<std::shared_ptr<const std::vector<glm::vec2>>> mRoutes; // by the same ids, null where a clip sits
    Stats mStats;
};
} // namespace crowds
