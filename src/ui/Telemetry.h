#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

class QWidget;

/**
 * Anonymous usage statistics, opt-in through Preferences > General > Telemetry.
 *
 * Each GUI session is kept in <userDir>/telemetry/sessions/<pid>.json while it runs and becomes a session_end event
 * when Lila exits; one left behind by a process that is gone becomes a crashed session_end at the next start. Events
 * wait in queue.json and go out as one batch, at startup and every half hour. Sending never blocks and fails
 * silently; an event is dropped after kMaxAttempts failed sends. Nothing is sent from lilabatch.
 *
 * Collected: version, OS, GPU, RAM and display count, locale, install channel, command counts (built-in commands by
 * name, others as a total), active minutes, crashes, Unreal link use, and the optional onboarding answers. Never file
 * names, paths, scene content or usernames.
 */
namespace lila::telemetry
{
using json = nlohmann::json;
namespace fs = std::filesystem;

inline constexpr const char* kInstallIdPref = "/telemetry/installId";
inline constexpr const char* kInstalledAtPref = "/telemetry/installedAt";
inline constexpr const char* kRolePref = "/telemetry/onboarding/role";
inline constexpr const char* kTeamSizePref = "/telemetry/onboarding/teamSize";
inline constexpr const char* kPreviousDccPref = "/telemetry/onboarding/previousDcc";
inline constexpr const char* kOnboardingSentPref = "/telemetry/onboarding/sent";
/** "auto" sends crash reports without asking, "ask" offers Send in the crash dialog, "off" never sends. */
inline constexpr const char* kCrashSendPref = "/telemetry/crashSend";
inline constexpr int kMaxAttempts = 5;
inline constexpr size_t kMaxQueued = 500;

/** (shown text, stored value) pairs for the onboarding questions; the first is "not answered". */
LILA_UI_API const std::vector<std::pair<std::string, std::string>>& roleChoices();
LILA_UI_API const std::vector<std::pair<std::string, std::string>>& teamSizeChoices();
LILA_UI_API const std::vector<std::pair<std::string, std::string>>& previousDccChoices();

/** DO_NOT_TRACK or LILA_NO_TELEMETRY set to anything but "" or "0". */
LILA_UI_API bool optedOutByEnvironment();
/**
 * kCrashSendPref's value, or "" while the user has never chosen (which behaves as "ask"). "off" as well where a
 * studio has locked the crashReports switch off.
 */
LILA_UI_API std::string crashSendMode();
/** (shown text, stored value) for the three crash report choices, the recommended one first. */
LILA_UI_API const std::vector<std::pair<std::string, std::string>>& crashSendChoices();
/** The usage pref is on and the environment doesn't opt out. */
LILA_UI_API bool enabled();

/** Random, made on first use and kept in prefs; never derived from the machine or the user. */
LILA_UI_API std::string installId();
/** A new random id: what is sent from now on can't be linked to what was sent before. */
LILA_UI_API std::string resetInstallId();

/** "<8", "8-15", "16-31", "32-63", "64-127", "128+". */
LILA_UI_API std::string memoryBucket(double gb);
/** "0", "1-6", "7-29", "30-89", "90+". */
LILA_UI_API std::string daysBucket(int64_t days);
/** "steam" under steamapps, "source" in a CMake build tree, otherwise "installer". */
LILA_UI_API std::string installChannel(const fs::path& exeDir);

/** One GUI session's counts, as written to its session file. */
struct Session
{
    std::string id;
    int64_t pid = 0;
    int64_t startedMs = 0;
    int64_t updatedMs = 0;
    int activeMinutes = 0;
    std::map<std::string, int> commands;
    int otherCommands = 0; // extensions and scripts: counted, never named
    std::map<std::string, int> sources; // "ui", "python", "mcp"
    int unrealCalls = 0;
    std::vector<std::string> unrealVersions;

    json toJson() const;
    static Session fromJson(const json& j);
};

/** A top-level command run: built-in ones by name, the rest as a count. */
LILA_UI_API void countCommand(Session& s, std::string_view name, bool builtIn, std::string_view source);

/** A PostHog capture event. */
LILA_UI_API json makeEvent(std::string_view name, const std::string& distinctId, int64_t timeMs, json properties);
LILA_UI_API json sessionEndEvent(const Session& s, const std::string& distinctId, bool crashed);

/** The on-disk side, rooted at a folder so tests can use their own. */
class LILA_UI_API Store
{
public:
    explicit Store(fs::path dir) : mDir(std::move(dir)) {}

    json queue() const;
    /** Appends, dropping the oldest past kMaxQueued. */
    void enqueue(json event);
    /** Removes the first count events, after a send that went through. */
    void dequeue(size_t count);
    /** Counts a failed send against the first count events and drops those at kMaxAttempts. */
    void failed(size_t count);
    void clear();

    void writeSession(const Session& s);
    void removeSession(int64_t pid);
    /** Session files of processes no longer running, removed as they are returned. */
    std::vector<Session> takeStaleSessions(int64_t selfPid, const std::function<bool(int64_t pid, int64_t startedMs)>& alive);

    const fs::path& dir() const { return mDir; }

private:
    void writeQueue(const json& q) const;

    fs::path mDir;
};

/** Starts collecting for this GUI session when enabled, and follows the pref from then on. */
LILA_UI_API void start();
/** Ends the session on a clean exit. Writes the queue; sends nothing. */
LILA_UI_API void stop();
/** Shows what is queued and what this session will send, as JSON. */
LILA_UI_API void showPending(QWidget* parent);
} // namespace lila::telemetry
