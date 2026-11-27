#pragma once

#include "core/Export.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

/**
 * Sending crash reports to Sentry, from the lilacrash watchdog once Lila has died.
 *
 * Lila keeps <userDir>/telemetry/crash-consent.json up to date with the user's choice; the watchdog reads it at crash
 * time, so a choice made after startup still counts. The crash dialog offers sending unless the environment or the
 * studio forbids it, and sends by itself once the user has said to. The minidump goes to Sentry's minidump endpoint (Sentry
 * symbolicates it and keeps only the event) with report.txt attached after sanitize() has taken paths, the user and
 * machine names out of it.
 */
namespace lila::crash::upload
{
namespace fs = std::filesystem;

inline constexpr const char* kConsentFile = "crash-consent.json";
/** Under the telemetry folder: {"url", "event"} per running Lila, for sendCrashedSession(). */
inline constexpr const char* kCrashEventsDir = "crash-events";

struct Consent
{
    bool offer = false; // the crash dialog may offer sending: no environment opt-out, not locked off by the studio
    bool send = false;  // send without asking
    bool userOff = false; // the user chose never to send, and may change it; false where a studio or the environment decided
    fs::path prefsFile; // the user's general.json, where the choice is kept
};

LILA_CRASHKIT_API Consent readConsent(const fs::path& file);
LILA_CRASHKIT_API void writeConsent(const fs::path& file, const Consent& c);
/** Records "send without asking" in both the consent file and the user's prefs. */
LILA_CRASHKIT_API void setSend(const fs::path& consentFile, bool on);

/**
 * report.txt as it may leave the machine: identifying fields dropped, every literal in `secrets` (user and machine
 * names) replaced, and file paths cut to the file name for code files and to "<path>" for anything else.
 */
LILA_CRASHKIT_API std::string sanitize(std::string_view reportText, const std::vector<std::string>& secrets);

#ifdef _WIN32
/** POSTs reportDir's minidump and sanitized report. Writes reportDir/.sent with Sentry's event id on success. */
LILA_CRASHKIT_API bool send(const fs::path& reportDir, std::string* eventId = nullptr, std::string* error = nullptr);
LILA_CRASHKIT_API bool alreadySent(const fs::path& reportDir);

/** POSTs a JSON body to an http(s) URL; true on a 2xx answer. */
LILA_CRASHKIT_API bool postJson(const std::string& url, const std::string& body, std::string* error = nullptr);

/**
 * The crashed session's usage event, which Lila keeps ready in <telemetryDir>/crash-events/<pid>.json while usage
 * statistics are on. POSTs it and, on success, removes it and Lila's session file so the next start doesn't send it again.
 * Returns false when there is nothing to send or the send failed.
 */
LILA_CRASHKIT_API bool sendCrashedSession(const fs::path& telemetryDir, unsigned long pid, std::string* error = nullptr);
#endif
} // namespace lila::crash::upload
