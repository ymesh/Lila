#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

/**
 * The motion engine: Lila's runner for the MotionBricks SOMA model (NVIDIA, NVIDIA Open Model Agreement), a network that
 * generates 0.6 to 1.9 s of human motion from the four poses a character just played (the context) and one to four poses
 * it should reach (the target). Longer motion chains generations: the last four poses played become the next context.
 *
 * Everything here is in SOMA space: Y up, meters, 30 Hz, poses as local joint transforms of a SomaSkeleton, root
 * (Hips) first with its transform in the world. Mapping to and from a Lila character lives in motion/Character.h.
 *
 * Batching: generate() takes any number N of independent requests and runs them as one inference, so many characters
 * (crowds) cost little more than one. Typical use, from one thread or several (Engine is thread-safe; calls serialise
 * on the session):
 *
 *     motion::Engine* engine = motion::Engine::shared();          // null when the model or runtime is missing
 *     if (!engine) log(motion::Engine::status().message);         // says which file is missing, or why
 *     std::vector<motion::Request> batch(n);
 *     for (auto& r : batch) {
 *         r.context = lastFourPlayedPoses;                         // oldest first, ends at the current pose
 *         r.target = {poseToReach};                                // 1..4 poses, the first is the one reached
 *         r.tokens = motion::tokenMask(8, 14);                     // allowed lengths, 6..16 tokens of 4 frames
 *     }
 *     std::vector<motion::Result> out = engine->generate(batch);   // out[i].frames: 18..58 poses at 30 Hz
 *
 * Each result starts at the last context pose and ends at the first target pose (endpoint correction, on by default).
 * foot contacts come per frame. For a real-time player, generate before the current motion runs out, play
 * up to the transition point, and feed what was shown back as context.
 */
namespace lila::motion
{
inline constexpr double kSampleRate = 30.0;
inline constexpr int kPosesPerToken = 4;
inline constexpr int kMinTokens = 6;
inline constexpr int kMaxTokens = 16;
inline constexpr int kBoundaryFrames = kPosesPerToken - 1;
inline constexpr int kRawFrames = kMaxTokens * kPosesPerToken;
inline constexpr int kMaxFrames = kRawFrames - 2 * kBoundaryFrames;
inline constexpr int kModelJoints = 30;
inline constexpr int kPoseFeatures = (kModelJoints - 1) * 3 + kModelJoints * 6;
inline constexpr int kMotionFeatures = 5 + (kModelJoints - 1) * 3 + kModelJoints * 6 + kModelJoints * 3 + 4;
inline constexpr int kTokenChoices = kMaxTokens - kMinTokens + 1;

/** Frames a generation of this many tokens yields after the boundary frames are removed. */
constexpr int framesForTokens(int tokens) { return tokens * kPosesPerToken - 2 * kBoundaryFrames; }

/** A rigid transform: rotation then translation. */
struct Xform
{
    glm::vec3 p{0.0f};
    glm::quat q{1.0f, 0.0f, 0.0f, 0.0f};

    Xform operator*(const Xform& o) const { return {p + q * o.p, q * o.q}; }
    Xform inverse() const
    {
        const glm::quat iq = glm::conjugate(q);
        return {-(iq * p), iq};
    }
};
using Pose = std::vector<Xform>;

/** Foot contact confidence per frame, 0..1: left heel, left toe, right heel, right toe. */
using Contacts = std::array<float, 4>;

/** The model's joints in its order; a skeleton must hold all of them by name, Hips at index 0. */
LILA_SCENE_API const std::array<const char*, kModelJoints>& modelJointNames();

/** A skeleton the model can drive: the SOMA joints, plus any others (fingers), which keep their reference pose. */
struct LILA_SCENE_API SomaSkeleton
{
    std::vector<std::string> names;
    std::vector<int> parents; // parents before children, -1 for the root
    Pose reference;

    int find(std::string_view name) const;
    size_t size() const { return names.size(); }
    /** World transforms of a local pose. */
    Pose world(const Pose& local) const;
    /** Local transforms of a world pose. */
    Pose local(const Pose& world) const;

    /** Reads resources/motion/soma_skeleton.json form; throws std::runtime_error. */
    static SomaSkeleton load(const std::filesystem::path& file);
    /** The SOMA skeleton Lila ships (77 joints, fingers included), loaded once. Throws when the file is missing. */
    static const SomaSkeleton& shipped();
};

/** Bit mask of allowed token counts, bit 0 for 6 tokens. */
constexpr uint16_t tokenMask(int minTokens, int maxTokens)
{
    uint16_t m = 0;
    for (int t = minTokens; t <= maxTokens; ++t)
        if (t >= kMinTokens && t <= kMaxTokens)
            m = uint16_t(m | (1u << (t - kMinTokens)));
    return m;
}
inline constexpr uint16_t kDefaultTokens = tokenMask(8, 14);

/** The token count whose frame count is nearest frames (6..16). */
LILA_SCENE_API int tokensForFrames(double frames);

struct Request
{
    /** The last four poses played, oldest first, at 30 Hz. */
    std::array<Pose, kPosesPerToken> context;
    /** One to four poses to reach, at 30 Hz from the one reached. More poses give the arrival a velocity. */
    std::vector<Pose> target;
    /** Allowed lengths (tokenMask); never empty. */
    uint16_t tokens = kDefaultTokens;
    /** Ground direction (x, z) the last context pose is turned to face for inference. Different values give variants. */
    glm::vec2 heading{1.0f, 0.0f};
    /** Spread a correction over the motion so it starts exactly at the last context pose and ends at the target. */
    bool correctEndpoints = true;
};

struct Result
{
    /** 18..58 local poses at 30 Hz; empty when the request failed. */
    std::vector<Pose> frames;
    std::vector<Contacts> contacts;
    int tokens = 0;
    std::string error;
};

/** The model's input tensors for a batch, as generate() fills them. Public for tests and other runtimes. */
struct LILA_SCENE_API Inputs
{
    int count = 0;
    std::vector<float> globalRoot, localRoot, localPoses; // [N,8,5], [N,8,4], [N,8,267]
    std::vector<int32_t> hasGlobalRoot, hasLocalRoot, hasLocalPoses, allowedTokens; // [N,8] x3, [N,11]
    /** Per request: the heading alignment that maps aligned model space back to SOMA space. */
    std::vector<Xform> alignment;
};

/** Raw outputs: [N,64,366] motion features and [N] token counts. */
struct Outputs
{
    std::vector<float> motion;
    std::vector<int32_t> tokens;
};

/** Validates requests and fills the input tensors. Throws std::invalid_argument naming the bad request. */
LILA_SCENE_API Inputs encode(const SomaSkeleton& skeleton, std::span<const Request> requests);
/** Decodes the outputs into poses and contacts, trims the boundary frames and corrects the endpoints. */
LILA_SCENE_API std::vector<Result> decode(const SomaSkeleton& skeleton, std::span<const Request> requests, const Inputs& in,
                                          const Outputs& out);

enum class Device : uint8_t
{
    Auto, // GPU where it works, else CPU (reported)
    Gpu,
    Cpu,
};

struct EngineOptions
{
    /** Empty: modelPath(). */
    std::filesystem::path model;
    Device device = Device::Auto;
    /** CPU threads; 0 lets the runtime choose. */
    int threads = 0;
};

/** What the engine runs on, or why it can't run. */
struct Status
{
    bool ready = false;
    /** "DirectML" or "CPU". */
    std::string device;
    /** One line for the user: the device and, when it fell back to CPU, why; or what is missing and where it looked. */
    std::string message;
    std::filesystem::path model;
    std::string runtimeVersion;
};

/** Where the model is looked for: LILA_MOTION_MODEL, then models/motionbricks beside the executable, then resources. */
LILA_SCENE_API std::filesystem::path modelPath();

class LILA_SCENE_API Engine
{
public:
    /** Null with the reason in status (a missing model names the path) when it can't load. */
    static std::unique_ptr<Engine> create(const SomaSkeleton& skeleton, const EngineOptions& options, Status* status = nullptr);
    /** One engine on the shipped skeleton and model, made on first use (seconds); null when that failed. */
    static Engine* shared();
    /** The shared engine's status, after trying to load it. */
    static Status status();
    /** Drops the shared engine so the next shared() reloads. */
    static void resetShared();
    /** The device the shared engine asks for (the Motion preference); changing it reloads on next use. */
    static void setSharedDevice(Device device);
    static Device sharedDevice();

    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /** One inference for every request. Results line up with requests; a bad request gets its error and no frames. */
    std::vector<Result> generate(std::span<const Request> requests);
    /** Runs the model on prepared inputs. */
    Outputs run(const Inputs& inputs);

    const SomaSkeleton& skeleton() const;
    const Status& info() const;
    /** Milliseconds of the last generate(): encode, inference, decode. */
    std::array<double, 3> lastTimes() const;

private:
    Engine();
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
} // namespace lila::motion
