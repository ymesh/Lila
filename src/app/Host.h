#pragma once

#include "core/CrashHandler.h"
#include "core/Export.h"

#include <filesystem>
#include <string>
#include <vector>

/** What lila.exe and lilabatch.exe share: startup and shutdown of everything below the UI. */
namespace lila::host
{
struct Options
{
    std::string batchScript;
    /** Arguments after the batch script: its sys.argv[1:]. */
    std::vector<std::string> scriptArgs;
    std::string command;
    std::string path;
    bool batch = false;
    /** --from-setup: started by the installer once its files are in place, while it still holds its mutex. */
    bool fromSetup = false;
    /** --batch-run: everything after it, for batch::runCli. */
    std::vector<std::string> batchRun;
    bool batchRunCli = false;
    /** --batch-run-worker <job> <index>: one file of a batch run. */
    std::string workerJob;
    size_t workerIndex = 0;
    /** --change-stream [port]: serve the change stream on 127.0.0.1 during a batch run; -1 when not given. */
    int changeStreamPort = -1;
    /** --version: print it and exit. */
    bool printVersion = false;
};

LILA_SCENE_API Options parse(int argc, char** argv);

/** Prefs, the up axis, node types and every non-UI command. Call right after crash::install. */
LILA_SCENE_API void startCore();

/** Opens the workspace the command line names. A scene file it names is loaded here in batch mode; the GUI gets it back to open once its window is up, under the progress overlay. */
LILA_SCENE_API std::filesystem::path openStartupWorkspace(const Options& o);

/** Python, then the extensions that load at startup. False when Python did not start. */
LILA_SCENE_API bool startPython(const Options& o);

LILA_SCENE_API crash::WatchOptions crashOptions(bool gui);

/** --batch: the watchdog, console progress, then the -c command, the script and a --batch-run. The exit code. */
LILA_SCENE_API int runBatch(const Options& o, bool pythonOk);

/** Sends events::Event::Shutdown, unloads extensions and stops Lila's services. */
LILA_SCENE_API void shutdown();
} // namespace lila::host
