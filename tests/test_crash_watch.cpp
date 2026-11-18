// Runs lila_crashtest.exe in each crash mode and checks the watchdog's report. Separate from lila_tests because every
// case really crashes a process (and files a Windows Error Report).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/CrashHandler.h"
#include "core/CrashReport.h"
#include "core/Paths.h"

#include <chrono>
#include <filesystem>
#include <thread>

#include <windows.h>
#include <tlhelp32.h>

using namespace lila;
namespace fs = std::filesystem;

namespace
{
fs::path freshDir(const std::string& name)
{
    // Per process: other test runs may share the temp folder.
    fs::path dir = fs::temp_directory_path() / "lila_tests" / ("crash_" + std::to_string(GetCurrentProcessId())) / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

bool containsText(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}
} // namespace

namespace
{
struct HelperRun
{
    DWORD exitCode = 0;
    DWORD watchdogExit = STILL_ACTIVE;
    fs::path config;
    fs::path report; // the folder, empty when none
    std::string text;
};

DWORD findChild(DWORD parent, const wchar_t* exe)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    DWORD found = 0;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (pe.th32ParentProcessID == parent && _wcsicmp(pe.szExeFile, exe) == 0)
            found = pe.th32ProcessID;
    CloseHandle(snap);
    return found;
}

/** Stacks name source files only when the helper's PDB is beside it, which Release builds don't write. */
bool helperHasSymbols()
{
    return fs::exists(paths::executableDir() / "lila_crashtest.pdb");
}

/** Runs lila_crashtest.exe mode with its own config folder and no UI, and waits for its watchdog to finish. */
HelperRun runHelper(const std::string& mode, bool wer)
{
    HelperRun out;
    out.config = freshDir("run_" + mode);
    wchar_t oldConfig[2048] = {};
    GetEnvironmentVariableW(L"LILA_CONFIG_DIR", oldConfig, DWORD(std::size(oldConfig)));
    SetEnvironmentVariableW(L"LILA_CONFIG_DIR", out.config.c_str());
    SetEnvironmentVariableW(L"LILA_CRASH_NO_UI", L"1");
    SetEnvironmentVariableW(L"LILA_CRASH_WER_WAIT_MS", L"3000");
    SetEnvironmentVariableW(L"LILA_CRASH_REGISTER_WER", wer ? L"1" : L"0");

    fs::path exe = paths::executableDir() / "lila_crashtest.exe";
    std::wstring cmd = L"\"" + exe.wstring() + L"\" " + std::wstring(mode.begin(), mode.end());
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    SetEnvironmentVariableW(L"LILA_CONFIG_DIR", oldConfig[0] ? oldConfig : nullptr);
    SetEnvironmentVariableW(L"LILA_CRASH_NO_UI", nullptr);
    SetEnvironmentVariableW(L"LILA_CRASH_WER_WAIT_MS", nullptr);
    SetEnvironmentVariableW(L"LILA_CRASH_REGISTER_WER", nullptr);
    REQUIRE_MESSAGE(started, "cannot start " << exe.string());

    HANDLE watchdog = nullptr;
    for (int i = 0; i < 100 && !watchdog; ++i)
    {
        if (DWORD id = findChild(pi.dwProcessId, L"lilacrash.exe"))
            watchdog = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK_MESSAGE(watchdog, "the helper never started lilacrash.exe");
    CHECK(WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0);
    GetExitCodeProcess(pi.hProcess, &out.exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (watchdog)
    {
        CHECK_MESSAGE(WaitForSingleObject(watchdog, 60000) == WAIT_OBJECT_0, "the watchdog did not exit after the helper");
        GetExitCodeProcess(watchdog, &out.watchdogExit);
        CloseHandle(watchdog);
    }
    std::vector<fs::path> reports = crash::listReports(out.config / "crashes");
    if (!reports.empty())
    {
        out.report = reports.back();
        out.text = crash::readText(out.report / "report.txt");
    }
    return out;
}

void unregisterWerHelper()
{
    fs::path dll = paths::executableDir() / "lilawer.dll";
    RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\Windows Error Reporting\\RuntimeExceptionHelperModules",
                       dll.c_str());
}

/** What every crash report must carry, whichever way the crash was caught. */
void checkCommon(const HelperRun& run, const char* code)
{
    REQUIRE_MESSAGE(!run.report.empty(), "no crash report under " << run.config.string());
    crash::Report r = crash::parseReport(run.text);
    INFO(run.text.substr(0, 4000));
    CHECK(run.watchdogExit == 1);
    REQUIRE(r.find("crash"));
    CHECK(r.find("crash")->get("exception.code") == code);
    CHECK(containsText(r.summary, code));
    CHECK(containsText(run.text, "crashtest: marker line"));
    CHECK(containsText(run.text, "lila.ops.polyCube(name='crashtestCube')"));
    CHECK(containsText(run.text, "scene: X:/crashtest/scene.lila"));
    CHECK_FALSE(containsText(run.text, "crashtestSecretToken0123456789"));
    REQUIRE(r.find("system"));
    CHECK_FALSE(r.find("system")->get("os").empty());
    CHECK_FALSE(r.find("system")->get("cpu").empty());
    CHECK(r.find("monitors"));
    CHECK_FALSE(r.find("app")->get("uptime").empty());
    CHECK_FALSE(r.find("app")->get("build.id").empty());
    // The session files are consumed.
    CHECK_FALSE(fs::exists(run.config / "sessions" / (r.find("app")->get("pid") + ".crash.txt")));
}
} // namespace

TEST_CASE("crash watchdog: clean exit writes nothing")
{
    HelperRun run = runHelper("clean", false);
    CHECK(run.exitCode == 0);
    CHECK(run.watchdogExit == 0);
    CHECK(run.report.empty());
}

TEST_CASE("crash watchdog: access violation is captured live")
{
    HelperRun run = runHelper("av", false);
    CHECK(run.exitCode == 0xC0000005);
    checkCommon(run, "0xC0000005");
    crash::Report r = crash::parseReport(run.text);
    CHECK(containsText(r.find("crash")->get("detected.by"), "watchdog"));
    CHECK(containsText(r.find("crash")->get("access"), "write of 0x0000000000000000"));
    CHECK(fs::file_size(run.report / "minidump.dmp") > 10000);
    REQUIRE(r.find("stack"));
    if (helperHasSymbols())
        CHECK(containsText(r.summary, "crash_helper.cpp"));
    CHECK(r.find("threads")->lines.size() > r.find("stack")->lines.size());
    CHECK(r.find("modules")->lines.size() > 5);
}

TEST_CASE("crash watchdog: unhandled C++ exception names its type")
{
    HelperRun run = runHelper("throw", false);
    checkCommon(run, "0xE06D7363");
    CHECK(containsText(run.text, "runtime_error"));
}

TEST_CASE("crash watchdog: std::terminate says why")
{
    HelperRun run = runHelper("terminate", false);
    checkCommon(run, "0xE0000001");
    CHECK(containsText(run.text, "note: std::terminate called without an active exception"));
}

TEST_CASE("crash watchdog: stack overflow")
{
    HelperRun run = runHelper("stackoverflow", false);
    CHECK(run.exitCode == 0xC00000FD);
    checkCommon(run, "0xC00000FD");
}

TEST_CASE("crash watchdog: __fastfail is reported through WER")
{
    HelperRun run = runHelper("fastfail", true);
    unregisterWerHelper();
    CHECK(run.exitCode == 0xC0000409);
    checkCommon(run, "0xC0000409");
    crash::Report r = crash::parseReport(run.text);
    // Needs Windows Error Reporting enabled; without it the report has the exit code and the log, but no stack.
    CHECK(containsText(r.find("crash")->get("detected.by"), "Windows Error Reporting"));
    CHECK(containsText(r.find("crash")->get("fastfail.code"), "FAST_FAIL_FATAL_APP_EXIT"));
    CHECK(fs::exists(run.report / "minidump.dmp"));
    if (helperHasSymbols())
        CHECK(containsText(r.summary, "crash_helper.cpp"));
}

TEST_CASE("crash watchdog: heap corruption is reported through WER")
{
    HelperRun run = runHelper("heap", true);
    unregisterWerHelper();
    CHECK(run.exitCode == 0xC0000374);
    checkCommon(run, "0xC0000374");
    CHECK(fs::exists(run.report / "minidump.dmp"));
}
