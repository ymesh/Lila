#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "render/Renderer.h"

#include <QApplication>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
std::filesystem::path exeDir()
{
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
#else
    return std::filesystem::current_path();
#endif
}

bool hasOffscreenPlugin()
{
    std::error_code ec;
    for (const char* name : {"qoffscreen.dll", "qoffscreend.dll", "libqoffscreen.so", "libqoffscreen.dylib"})
        if (std::filesystem::exists(exeDir() / "platforms" / name, ec))
            return true;
    return false;
}
} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    // A test fails to the console, never to a dialog.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    if (!hasOffscreenPlugin())
    {
        std::fprintf(stderr, "lila_dock_tests: skipped, no offscreen Qt platform plugin in %s/platforms\n", exeDir().string().c_str());
        return 0;
    }
    // Keep tests out of the real user prefs.
    auto prefs = std::filesystem::temp_directory_path() / "lila_dock_tests";
    std::filesystem::create_directories(prefs);
#ifdef _WIN32
    _putenv_s("QT_QPA_PLATFORM", "offscreen");
    _putenv_s("LILA_CONFIG_DIR", prefs.string().c_str());
#else
    setenv("QT_QPA_PLATFORM", "offscreen", 1);
    setenv("LILA_CONFIG_DIR", prefs.string().c_str(), 1);
#endif
    QApplication app(argc, argv);
    doctest::Context ctx(argc, argv);
    const int result = ctx.run();
    // Stopped while its threads still run: left to the static destructors, the device is torn down after the process
    // has already killed them, which crashes in the driver.
    lila::render::Renderer::get().stop();
    return result;
}
