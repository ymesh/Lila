#include "app/Host.h"
#include "core/CrashHandler.h"
#include "core/Version.h"
#include "ui/AppMain.h"
#include "ui/Updater.h"

#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace lila;

int main(int argc, char** argv)
{
    crash::install();
    host::Options opts = host::parse(argc, argv);
#if defined(_WIN32) && defined(LILA_GUI_SUBSYSTEM)
    // The GUI build has no console of its own; batch output goes to the shell that launched it, if any.
    if ((opts.batch || opts.printVersion) && AttachConsole(ATTACH_PARENT_PROCESS))
    {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
#endif
    if (opts.printVersion)
    {
        std::printf("Lila %s\n", version::full().c_str());
        return 0;
    }

    if (!opts.batch && !opts.fromSetup && ui::updater::deferToRunningInstall())
        return 0;
    host::startCore();
    ui::registerBatchCommands();
    const std::filesystem::path startupScene = host::openStartupWorkspace(opts);
    const bool pythonOk = host::startPython(opts);
    const int rc = opts.batch ? host::runBatch(opts, pythonOk) : ui::runApp(argc, argv, opts, startupScene);
    host::shutdown();
    return rc;
}
