#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

class QWidget;

/**
 * Update checks against latest.json on the newest GitHub release, attached by Release.cmd or the release workflow:
 *   {"version": "0.2.0", "notes": "...", "page": "<release page>", "installer": "<Setup.exe url>", "sha256": "...", "size": 123,
 *    "patches": [{"from": "0.1.9", "installer": "<Patch.exe url>", "sha256": "...", "size": 45}]}
 * A newer version shows a notice. With an installer URL, Lila downloads it (progress in the status bar), checks the hash
 * and runs it silently once the last Lila has closed (after a crash, at the next clean exit), or closes Lila right away
 * for Install Now; without one it only opens the release page. A patch from the running version is used instead of the
 * full installer when every file listed in the install's lila-files.json still matches. Off for Steam and source builds
 * (Steam updates itself), with /update/check off, or with LILA_NO_UPDATE_CHECK set. LILA_UPDATE_URL points it at another
 * manifest.
 */
namespace lila::ui::updater
{
inline constexpr const char* kCheckPref = "/update/check";
inline constexpr const char* kSkipPref = "/update/skipVersion";
/** The downloaded, verified installer waiting for Lila to close. */
inline constexpr const char* kPendingPref = "/update/pendingInstaller";
/** The installer last started at exit: still running the old version next time means it didn't go in. */
inline constexpr const char* kAttemptedPref = "/update/attemptedInstaller";
/** A version whose patch failed to install; that version's update uses the full installer. */
inline constexpr const char* kPatchFailedPref = "/update/patchFailed";
/** The list of files and hashes each install carries, written by scripts/installer/build.ps1. */
inline constexpr const char* kFilesManifest = "lila-files.json";

/** -1, 0 or 1 comparing dotted versions ("0.10.0" > "0.9.2"); missing parts count as 0. */
LILA_UI_API int compareVersions(std::string_view a, std::string_view b);

/** Updates one exact earlier version to the release's version. */
struct Patch
{
    std::string from;
    std::string installer;
    std::string sha256;
    std::uint64_t size = 0;
};

struct Release
{
    std::string version;
    std::string notes;
    std::string page;
    std::string installer;
    std::string sha256;
    /** Installer size in bytes, 0 when the manifest leaves it out. */
    std::uint64_t size = 0;
    std::vector<Patch> patches;
};

/** Empty version when the manifest is unusable. */
LILA_UI_API Release parseManifest(const nlohmann::json& j);

/**
 * Files listed in a lila-files.json manifest ({"version": ..., "files": {"<relative path>": {"size": n, "sha256": ...}}})
 * that are missing from `dir` or differ from it. Files the manifest doesn't list are ignored.
 */
LILA_UI_API std::vector<std::string> changedFiles(const std::filesystem::path& dir, const nlohmann::json& manifest);

/** The startup check, a few seconds after launch. Quiet when there's nothing new or the check fails. */
LILA_UI_API void startAutoCheck(QWidget* window);
/** Help > Check for Updates: also says when Lila is up to date or the check failed. */
LILA_UI_API void checkNow(QWidget* window);
/** Help > Reinstall Lila: downloads the newest full installer and runs it over the install, whatever its state. */
LILA_UI_API void reinstall(QWidget* window);
/**
 * Asked as the main window closes: false keeps Lila open. While an update downloads it offers to close and install once
 * the download finishes, or to quit without it.
 */
LILA_UI_API bool confirmQuit(QWidget* window);
/** Runs a downloaded update's installer silently, detached, once no other Lila is running; call as Lila exits. */
LILA_UI_API void installPendingOnExit();
/**
 * True while an update is installing: Lila must not start on files being replaced. It asks the installer to start
 * Lila when it finishes; the caller exits.
 */
LILA_UI_API bool deferToRunningInstall();
} // namespace lila::ui::updater
