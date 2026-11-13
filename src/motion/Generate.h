#pragma once

#include "core/Export.h"
#include "motion/Engine.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

/**
 * Motion built from chained generations, in SOMA space (meters, Y up, 30 Hz): a Player that plays one generation and
 * records what it shows so the next starts from it, the gait library locomotion targets come from, Spots (target poses
 * placed around an object), and followPaths, which walks any number of characters along paths through their spots in
 * lock step, one batched inference per step.
 */
namespace lila::motion
{
using json = nlohmann::json;

/** Plays generated motion frame by frame and keeps the poses it showed, so the next generation starts from them. */
class LILA_SCENE_API Player
{
public:
    /** Starts holding pose. */
    explicit Player(const Pose& pose);
    /** Replaces the history with poses played before now (oldest first, the last is the current pose). */
    void seed(std::span<const Pose> recent);

    /** A request from the last four poses shown toward target. */
    Request request(std::vector<Pose> target, uint16_t tokens = kDefaultTokens, glm::vec2 heading = {1.0f, 0.0f}) const;
    /**
     * Plays a generation from its start (its first frame is the current pose). The transition point sits transitionEarly
     * frames before its end; skip drops frames already covered while it generated.
     */
    void play(Result result, int transitionEarly = 0, int skip = 0);
    /** Advances one frame and records the pose shown; past the end the last pose holds. */
    const Pose& step(Contacts* contacts = nullptr);
    /** Frames left before the transition point; zero or less means generate now. */
    int framesToTransition() const;
    /** Frames left in the current generation. */
    int framesLeft() const;
    const Pose& current() const;
    /** The last four poses shown, oldest first. */
    std::array<Pose, kPosesPerToken> context() const;
    /** Every pose shown since construction, and its contacts (zero while holding a seeded pose). */
    const std::vector<Pose>& recorded() const { return mRecorded; }
    const std::vector<Contacts>& recordedContacts() const { return mContacts; }
    void clearRecording();

private:
    std::vector<Pose> mHistory; // the last poses shown, newest last
    std::vector<Pose> mRecorded;
    std::vector<Contacts> mContacts;
    Result mPlaying;
    int mFrame = 0;
    int mTransition = 0;
};

/** A pose stored with only the model's joints, the others taken from the skeleton's reference. */
LILA_SCENE_API json poseToJson(const Pose& pose);
LILA_SCENE_API Pose poseFromJson(const json& j, const SomaSkeleton& skeleton = SomaSkeleton::shipped());

struct GaitWindow
{
    /** Where in the gait's cycle the window starts, 0..1. */
    double phase = 0.0;
    /** Four poses at 30 Hz, the first with its root's ground point at the origin facing +Z. */
    std::array<Pose, kPosesPerToken> poses;
};

struct Gait
{
    std::string name;
    /** walk, run, crouch, strafeLeft, strafeRight, backward. */
    std::string style;
    /** Ground speed, m/s, and cycle length, s. */
    double speed = 1.3;
    double period = 1.0;
    std::vector<GaitWindow> windows;
    /**
     * One whole cycle at 30 Hz from a foot strike (the first foot leaving the ground, for a clip of one step per foot), in
     * place facing +Z (root travel removed, sway kept), looping
     * seamlessly: the pose after the last is the first. round(period * 30) poses; empty in a library built without one.
     */
    std::vector<Pose> cycle;
};

/** Frames (fractional, at 30 Hz) where each foot comes down, and leaves the ground, in a run of SOMA poses, from its height. */
struct FootStrikes
{
    std::vector<double> left, right;
    std::vector<double> leftLifts, rightLifts;
};
LILA_SCENE_API FootStrikes footStrikes(const std::vector<Pose>& frames);
/**
 * A gait's cycle length in frames from its strikes: the mean of one foot's strike-to-strike gaps that agree with their
 * median (both feet pooled); 0 when no foot strikes twice.
 */
LILA_SCENE_API double cycleFrames(const FootStrikes& strikes);
/**
 * One seamless cycle from frames: [start, start + length) (fractional frames) resampled to round(length) poses, facing
 * +Z with the first pose's root over the origin, the root's travel over the cycle taken out, and whatever the pose at
 * start + length still differs from the first spread across the cycle so the last pose leads back into the first.
 */
LILA_SCENE_API std::vector<Pose> loopCycle(const std::vector<Pose>& frames, double start, double length);

/** Target poses for locomotion: gaits by style and speed, and the pose to stop in. */
struct LILA_SCENE_API GaitLibrary
{
    std::vector<Gait> gaits;
    Pose idle;
    /** Where crouching comes to rest; the idle pose when the library has none. */
    Pose crouchIdle;
    /** Where the poses came from, as stored with the file. */
    std::string provenance;

    /** The gait of a style nearest a speed (0: the style's own), or null. */
    const Gait* find(std::string_view style, double speed = 0.0) const;
    std::vector<std::string> styles() const;
    /** The pose a style comes to rest in. */
    const Pose& rest(std::string_view style) const { return style == "crouch" && !crouchIdle.empty() ? crouchIdle : idle; }
    json toJson() const;
    /** Throws std::runtime_error. */
    static GaitLibrary fromJson(const json& j);
    static GaitLibrary load(const std::filesystem::path& file);
    /** resources/motion/gaits.json, read once; throws when missing. */
    static const GaitLibrary& shipped();
};

/** Four target poses for a gait at a ground point, facing heading (radians, 0 is +Z), its speed scaled to speed. */
LILA_SCENE_API std::vector<Pose> gaitTarget(const Gait& gait, const GaitWindow& window, glm::vec2 ground, float facing, double speed);

/** Which hands and feet a target holds on its own pose's positions as it is reached. */
enum Pin : uint8_t
{
    kPinLeftHand = 1,
    kPinRightHand = 2,
    kPinLeftFoot = 4,
    kPinRightFoot = 8,
};

/** One step of an interaction: poses to reach, relative to the spot. */
struct SpotTarget
{
    std::string name;
    /** One to four poses at 30 Hz, roots in the spot's frame (origin on the ground, +Z the way it faces). */
    std::vector<Pose> poses;
    int transitionEarly = 0;
    uint16_t tokens = kDefaultTokens;
    uint8_t pins = 0;
    /** Frames to hold the reached pose before the next target. */
    int hold = 0;
};

/** An interaction around an object (sit, vault, duck, step up, pick up): the targets a character performs there. */
struct LILA_SCENE_API Spot
{
    std::string name;
    std::vector<SpotTarget> targets;
    /** Where the poses came from (the author's character and scene, or a mocap source). */
    std::string provenance;

    json toJson() const;
    /** Throws std::runtime_error. */
    static Spot fromJson(const json& j);
    static Spot load(const std::filesystem::path& file);
    void save(const std::filesystem::path& file) const;
};

/** Spots Lila ships (resources/motion/spots/*.lilaspot), by file stem. */
LILA_SCENE_API std::vector<std::pair<std::string, std::filesystem::path>> shippedSpots();

/** A spot's target in SOMA space: its poses placed by the spot's ground position and heading. */
struct PlacedTarget
{
    std::vector<Pose> poses;
    int transitionEarly = 0;
    uint16_t tokens = kDefaultTokens;
    uint8_t pins = 0;
    int hold = 0;
    /** Which spot and target it came from, for reports. */
    std::string label;
};
LILA_SCENE_API std::vector<PlacedTarget> placeSpot(const Spot& spot, glm::vec2 ground, float heading);

/** One stretch of a journey: walk the path, then perform the targets at its end. */
struct Leg
{
    /** Ground points (x, z), meters; empty to perform the targets where the character stands. */
    std::vector<glm::vec2> path;
    std::vector<PlacedTarget> targets;
};

struct PathOptions
{
    std::string style = "walk";
    /** m/s; 0 uses the gait's own. */
    double speed = 0.0;
    /** Strafe styles face this ground direction (x, z); zero faces the way the path leaves the start. */
    glm::vec2 facing{0.0f};
    /** Come to rest at the end of the last leg. */
    bool stop = true;
    /** Seconds of travel the next target sits ahead. */
    double lookahead = 0.9;
    int transitionEarly = 10;
    /** Allowed lengths while walking. */
    uint16_t tokens = tokenMask(7, 11);
    /** Different values give different but equally valid motion. */
    uint32_t variant = 0;
    /** Safety stop, frames. */
    int maxFrames = 30 * 120;
};

struct Journey
{
    Pose start;
    /** Poses shown before start (oldest first) so the first generation continues them; may be empty. */
    std::vector<Pose> before;
    std::vector<Leg> legs;
    PathOptions options;
};

struct Clip
{
    std::vector<Pose> frames;
    std::vector<Contacts> contacts;
    /** Frames at which each target was reached, in order, with its label. */
    std::vector<std::pair<int, std::string>> reached;
    int generations = 0;
    std::string error;
};

/**
 * Every journey at once: all characters step together and those that need motion share one inference. Results line up
 * with journeys. Throws std::invalid_argument for an unknown style.
 */
LILA_SCENE_API std::vector<Clip> followPaths(Engine& engine, std::span<const Journey> journeys, const GaitLibrary& gaits = GaitLibrary::shipped());


/** What a driven character is asked to do this frame. */
struct DriveInput
{
    /** Desired ground direction (x, z) times 0..1 of the gait's speed; zero stands still. */
    glm::vec2 move{0.0f};
    std::string style = "walk";
    /** Keep facing this ground direction while moving (strafing); zero faces the way it moves. */
    glm::vec2 facing{0.0f};
};

/**
 * Steers one character interactively: each tick advances one 30 Hz frame of motion, generating toward a gait target
 * placed ahead along the input, on a worker thread so the frame never waits. Everything shown is recorded.
 */
class LILA_SCENE_API LiveDriver
{
public:
    LiveDriver(Engine& engine, const Pose& start, std::span<const Pose> before, const GaitLibrary& gaits = GaitLibrary::shipped());
    ~LiveDriver();
    LiveDriver(const LiveDriver&) = delete;
    LiveDriver& operator=(const LiveDriver&) = delete;

    void setInput(const DriveInput& input);
    /** One frame: the pose to show. */
    const Pose& tick();
    /** Generate on the calling thread instead (scripts and tests, where frames are stepped without real time). */
    void setSynchronous(bool on) { mSync = on; }
    /** Waits for a generation in flight. */
    void wait();
    const std::vector<Pose>& recorded() const { return mPlayer.recorded(); }
    const std::vector<Contacts>& recordedContacts() const { return mPlayer.recordedContacts(); }
    int generations() const { return mGenerations; }
    /** Frames the last generation took to arrive, in real time. */
    int latencyFrames() const { return mLatency; }
    /** The last generation error, empty when fine. */
    const std::string& error() const { return mError; }

private:
    bool wantsGeneration() const;
    Request nextRequest();
    void collect(bool block);

    Engine& mEngine;
    const GaitLibrary& mGaits;
    Player mPlayer;
    DriveInput mInput, mAsked;
    bool mSync = false;
    bool mStill = true;
    int mFrame = 0, mRequested = -1, mLastGen = -1000, mGenerations = 0, mLatency = 3;
    double mCycle = 0.0;
    std::future<Result> mPending;
    int mPendingEarly = 0;
    std::chrono::steady_clock::time_point mSent;
    std::string mError;
};

/** The heading direction a variant asks the model to align to: (1, 0) for variant 0, turned for others. */
LILA_SCENE_API glm::vec2 variantHeading(uint32_t variant);
} // namespace lila::motion
