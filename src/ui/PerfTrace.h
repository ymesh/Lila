#pragma once

#include "core/Export.h"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <string>

class QWidget;

/**
 * Help > Record Performance Trace: records a few seconds of Lila's profiler zones (CPU, the viewport's GPU passes,
 * frame numbers) with the bundled tracy-capture over 127.0.0.1, then zips the trace with the session report for a bug
 * report. Run as administrator, the trace also gets call-stack sampling and context switches.
 */
namespace lila::ui::perfTrace
{
/** The action, its Help menu entry and the perfTrace* commands. */
LILA_UI_API void registerAll(QWidget* mainWindow);

LILA_UI_API void showDialog(QWidget* parent);

/** Starts a recording of `seconds` once the capture connects. Returns the zip it will write; throws std::runtime_error. */
LILA_UI_API std::filesystem::path record(int seconds);
/** How long a recording asked for `seconds` runs: at least 1, at most 2 minutes from the dialog or 3 hours from a command. */
LILA_UI_API int recordingSeconds(int seconds, bool fromCommand);

/** state (idle, connecting, recording, saving, done, failed), secondsLeft, path, error, elevated, systemTracing. */
LILA_UI_API nlohmann::json status();

/** Where recordings go: <user config>/perf-traces. */
LILA_UI_API std::filesystem::path folder();
} // namespace lila::ui::perfTrace
