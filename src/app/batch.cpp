#include "app/Host.h"
#include "core/CrashHandler.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Version.h"

#include "extensions/Library.h"

#include <cstdio>

using namespace lila;

namespace
{
/**
 * The capture commands render through the UI library, which lilabatch does not link: load it when it sits next to
 * the exe, and run without those commands when it does not.
 */
void loadUiCommands()
{
    const std::filesystem::path dll = paths::executableDir() / LILA_UI_LIBRARY;
    std::string error;
    std::unique_ptr<extensions::Library> ui = extensions::Library::open(dll, &error);
    if (!ui)
    {
        std::fprintf(stderr, "lilabatch: %s did not load (%s): captureView, captureTurntable and the other UI commands are unavailable\n",
                     paths::toUtf8(dll).c_str(), error.c_str());
        return;
    }
    // The commands it registers run until exit.
    ui->leak();
    using RegisterFn = void (*)();
    if (auto fn = reinterpret_cast<RegisterFn>(ui->symbol("lilaUiRegisterBatchCommands")))
        fn();
}
} // namespace

int main(int argc, char** argv)
{
    crash::install();
    host::Options opts = host::parse(argc, argv);
    opts.batch = true;
    if (opts.error.empty() && !opts.exportArgs.empty() && opts.exportOut.empty())
        opts.error = "--export-args needs --export <scene> <out>";
    if (!opts.error.empty())
    {
        std::fprintf(stderr, "lilabatch: %s\nlilabatch --help lists the options.\n", opts.error.c_str());
        return 2;
    }
    if (opts.printHelp)
    {
        std::fputs(host::usage().c_str(), stdout);
        return 0;
    }
    if (opts.printVersion)
    {
        std::printf("Lila %s\n", version::full().c_str());
        return 0;
    }
    if (opts.quiet)
        log::setConsoleLevel(LogLevel::Warning);

    host::startCore();
    loadUiCommands();
    host::openStartupWorkspace(opts);
    const bool pythonOk = host::startPython(opts);
    const int rc = host::runBatch(opts, pythonOk);
    host::shutdown();
    return rc;
}
