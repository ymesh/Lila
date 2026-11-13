#pragma once

#include "core/Export.h"
#include "core/ExtensionContext.h"
#include "core/Registry.h"

#include "scene/Node.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * The base simulation framework shared by the physics extensions. A solver type is tied to a node type: each node of
 * that type is one simulation, whose solver the extension supplies. The framework owns time and the cache:
 *
 * - Deterministic stepping: a simulation starts at an integer frame and advances by whole steps (an integer number per
 *   frame) from there, or from a cached state that was itself stepped from there, so a frame has exactly one answer
 *   however it is reached. Step times are computed from integers, the solver runs on the main thread in a fixed order,
 *   and its only randomness is noise() with a seed.
 * - Cache: the state at the end of every frame, keyed by a chained hash of the solver's parameters and every input
 *   sampled up to that frame. An edit changes the chain from the first frame whose inputs it touches, so only frames
 *   from there on are recomputed; scrubbing back is a cache read.
 * - Loops: when the active take loops (or the outputs' curves carry a loop key) the period is simulated over and over,
 *   carrying state across the wrap, until the state at the boundary repeats within tolerance (min 2 passes, max the
 *   /sim/loop/maxPasses pref), then once more to record. The residual left at the seam is cross-faded out over the last
 *   /sim/loop/crossfadeFrames frames, so the last frame equals the first; info() reports whether it converged.
 * - Record: recordSimulation bakes the outputs onto a new additive animation layer, one layer per recording.
 */
namespace lila::scene::sim
{
/** What a run of state values is, for the loop convergence test. */
enum class Channel : uint8_t
{
    Position,        // compared against Tolerances::position (scene units)
    Velocity,        // units per second
    Rotation,        // rotation vector, radians; compared in degrees against Tolerances::rotation
    AngularVelocity, // radians per second; compared in degrees per second
    Other,           // compared against Tolerances::other
    Ignore,          // bookkeeping the test skips
};

/** count consecutive state values of one kind, compared as one vector (the length of their difference). */
struct Slot
{
    Channel channel = Channel::Other;
    uint32_t count = 3;
};

struct Tolerances
{
    double position = 0.01;
    double velocity = 0.1;
    double rotation = 0.01;        // degrees
    double angularVelocity = 0.1;  // degrees per second
    double other = 0.001;
};

/** What a solver says about itself once its node is read. */
struct Setup
{
    int64_t startFrame = 0;
    /** Steps per frame, at least 1. */
    int substeps = 1;
    /** The state vector's layout; its size is the sum of the counts. */
    std::vector<Slot> layout;
    /** Nodes the simulation poses, parents before children. Recording keys these. */
    std::vector<NodeId> outputs;
    /** Other nodes whose animation drives it, searched for loop keys along with the outputs and their ancestors. */
    std::vector<NodeId> sources;
    /** Hash of every setting that is not sampled per step (see hashBytes); a change throws the cache away. */
    uint64_t paramsHash = 0;
    /** Hold each output pose this many frames (2 = on twos). Applied when posing and recording. */
    int stepped = 1;
    /**
     * The state may grow past the layout (solver-owned bytes such as a physics engine's snapshot). The layout then
     * covers only its leading values, which are all the loop test compares.
     */
    bool variableState = false;
    /**
     * Every node sample() reads. When given, sampling at other times computes only what these depend on, so a dense
     * skinned mesh the solver never reads isn't recomputed at every sampled step. Empty evaluates the whole scene.
     */
    std::vector<NodeId> reads;
    /** False for a simulation whose state cannot carry across a loop's wrap (a crowd): it never loops. */
    bool loops = true;
};

/** Where a step sits in time; seconds = (startFrame * substeps + index) / (substeps * fps), from integers. */
struct StepInfo
{
    int64_t index = 0; // steps since the start frame (looping: since the period's start, wrapping)
    double seconds = 0.0;
    double dt = 0.0;   // seconds per step
    /** Looping: the period in seconds, so time-based effects such as noise can repeat with it. 0 otherwise. */
    double period = 0.0;
};

/**
 * One simulation's maths, written by an extension. Every call is on the main thread. The framework sets the graph's
 * time and evaluates before sample(); everything else must depend only on its arguments.
 */
class Solver
{
public:
    virtual ~Solver() = default;

    /** Reads the node and the scene it points at. Called before every use; throws std::invalid_argument when unusable. */
    virtual Setup configure(Graph& g) = 0;
    /** Appends the inputs at the graph's current time (animated poses, keyable weights, collider transforms). */
    virtual void sample(Graph& g, std::vector<double>& inputs) = 0;
    /** The state at the start frame, from the inputs sampled there. */
    virtual void reset(const std::vector<double>& inputs, std::vector<double>& state) = 0;
    /** Advances the state one step to inputs, which were sampled at the step's end. */
    virtual void step(const std::vector<double>& inputs, std::vector<double>& state, const StepInfo& at) = 0;
    /** World matrices of the outputs, in Setup::outputs order, from the state and the inputs sampled at that frame. */
    virtual void pose(const std::vector<double>& inputs, const std::vector<double>& state, std::vector<glm::dmat4>& world) = 0;
    /**
     * Asked when the scene changed since the cache was last checked. False keeps the cache without sampling every
     * cached step again, for solvers that can tell from the current time alone that no step's inputs changed.
     */
    virtual bool inputsMayHaveChanged(Graph&) { return true; }
};

/** A node type whose nodes are simulations, and how to make their solvers. */
struct SolverType
{
    /** The node type, or with `matches` set an id for the registry. */
    std::string nodeType;
    std::function<std::unique_ptr<Solver>(Graph& g, NodeId node)> create;
    /** For nodes whose type alone doesn't tell (a geometry-nodes modifier whose group holds a simulation node). */
    std::function<bool(const Node& n)> matches;
};

LILA_SCENE_API Registry<SolverType>& solverTypes();
/** For Lila's own solvers. Throws std::invalid_argument when the node type has one. */
LILA_SCENE_API void registerSolverType(SolverType type);
/**
 * For extensions: owned by ctx. Unloading drops every simulation of the type (and its cache) before the type goes, so
 * no solver outlives the library that made it. Null, with a log message, when the node type has one.
 */
LILA_SCENE_API const SolverType* registerSolverType(ext::Context& ctx, SolverType type);

/** Whether a node is a simulation: its type, or a base of it, has a solver type. */
LILA_SCENE_API bool isSimulation(const Node& n);
/** Simulation nodes in the scene, in id order. */
LILA_SCENE_API std::vector<NodeId> simulations(Graph& g);

enum class LoopMode : uint8_t
{
    Auto, // the active take's loop flag, else the outputs' loop keys
    On,
    Off,
};

struct Info
{
    bool loop = false;
    int64_t loopStart = 0, loopEnd = 0; // frames, when looping
    bool converged = true;              // looping: the boundary state repeated within tolerance
    int passes = 0;                     // looping: periods simulated before the recorded one
    double residual = 0.0;              // looping: largest boundary difference / its tolerance, before the cross-fade
    int64_t startFrame = 0;
    int substeps = 1;
    int64_t steps = 0;                  // solver steps run since the cache was last emptied
    int64_t cachedFrom = 0, cachedTo = -1; // frames held (cachedTo < cachedFrom: none)
    int64_t lastInvalidated = -1;       // first frame the last validation threw away; -1 when none
    uint64_t cacheHash = 0;             // chained input hash of the last cached frame
    std::string error;                  // why the last run failed, if it did
    std::vector<NodeId> outputs;        // Setup::outputs as last configured
};

/** A simulated frame: the state at its end and the outputs' world matrices (stepped output applied). */
struct Frame
{
    int64_t frame = 0;
    uint64_t hash = 0;
    std::vector<double> state;
    std::vector<glm::dmat4> world;
};

/**
 * Makes sure frames up to `frame` are simulated and returns that one, restoring the graph's time afterwards. Before the
 * start frame the first frame stands in; while looping, frames wrap into the period. Throws std::invalid_argument
 * (configure's, or a missing solver) and std::runtime_error when held values would be lost to a time change.
 */
LILA_SCENE_API std::shared_ptr<const Frame> simulate(Graph& g, NodeId node, int64_t frame, LoopMode loop = LoopMode::Auto);
/** The cached frame without simulating, or null when it isn't cached and valid as far as the cache knows. Drawing uses it. */
LILA_SCENE_API std::shared_ptr<const Frame> cached(Graph& g, NodeId node, int64_t frame);
/**
 * Whether simulate() could reach frame from the valid cache within `within` more frames: cheap enough to run while
 * drawing (playback stepping on). Drawing simulates longer runs, such as everything after an edit, later.
 */
LILA_SCENE_API bool extendable(Graph& g, NodeId node, int64_t frame, int64_t within);
/** Empties one simulation's cache, or every one when node is kNoNode. */
LILA_SCENE_API void clearCache(Graph& g, NodeId node = kNoNode);
LILA_SCENE_API Info info(Graph& g, NodeId node);

struct RecordOptions
{
    std::optional<int64_t> start, end; // frames; default the loop period, else the playback range
    /** 0 keys every frame; otherwise keys are removed while the curves stay within this of every frame. */
    double tolerance = 0.0;
    /** Rotation tolerance in degrees, used when tolerance > 0. */
    double rotationTolerance = 0.1;
    /** Output node names to key; empty for all. */
    std::vector<std::string> nodes;
    /** Layer name; default "<node>_sim". Made unique. */
    std::string layer;
    bool translate = true;
    bool rotate = true;
    LoopMode loop = LoopMode::Auto;
};

/**
 * Bakes the simulation onto a new additive animation layer above the others, keying translate and rotate of the
 * outputs so each evaluates to its simulated pose. Undoable as one step by the command that calls it. Returns the
 * layer node.
 */
LILA_SCENE_API NodeId record(Graph& g, NodeId node, const RecordOptions& options);

/** True while the framework moves time to sample a simulation's inputs. */
LILA_SCENE_API bool sampling();

/** The scene's frame rate, and the whole frame nearest a time in seconds. */
LILA_SCENE_API double framesPerSecond();
LILA_SCENE_API int64_t frameOf(double seconds);

/** The loop period of nodes: the active take's range when it loops, else from their loop keys. */
LILA_SCENE_API std::optional<std::pair<int64_t, int64_t>> loopRange(Graph& g, const std::vector<NodeId>& nodes);

/**
 * Smooth value noise in [-1, 1] with one lattice cell per unit of x, the same for the same seed and x on every
 * machine. period > 0 wraps the lattice every period cells, so the noise repeats exactly (for loops).
 */
LILA_SCENE_API double noise(uint64_t seed, double x, int64_t period = 0);
/** Three decorrelated noise() channels. */
LILA_SCENE_API glm::dvec3 noise3(uint64_t seed, double x, int64_t period = 0);
/** FNV-1a over raw bytes, chained from h. */
LILA_SCENE_API uint64_t hashBytes(const void* data, size_t size, uint64_t h = 1469598103934665603ull);
inline uint64_t hashDoubles(const std::vector<double>& v, uint64_t h = 1469598103934665603ull)
{
    return hashBytes(v.data(), v.size() * sizeof(double), h);
}

/**
 * A String attr a simulation node type may declare (hidden, stored): record() lists its layers there as JSON, and
 * sampling mutes them, so a simulation never reads its own recordings back as input.
 */
inline constexpr const char* kRecordingsAttr = "simRecordings";

/** Prefs (file "general"). */
inline constexpr const char* kMaxPassesPref = "/sim/loop/maxPasses";
inline constexpr const char* kCrossfadePref = "/sim/loop/crossfadeFrames";

/** simulate, recordSimulation, clearSimCache, simulationInfo. */
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::sim
