// lilacrash.exe: Lila's crash watchdog. Started by Lila with --watch; never shows a window unless Lila crashed.
// --send <report folder> uploads one report to Sentry and exits.
// --show <report folder> [--consent <file>] [--restart <exe>] shows the crash dialog for a report already on disk.

#include "core/CrashUpload.h"
#include "core/CrashWatch.h"

#include <commctrl.h>
#include <shellapi.h>

#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace lila::crash;
namespace fs = std::filesystem;

namespace
{
enum Button
{
    kSendRestart = 100,
    kSendClose,
    kRestart,
};

constexpr const wchar_t* kPrivacyUrl = L"https://lila-3d.github.io/docs/#/privacy";

struct DialogState
{
    fs::path dir;
    std::wstring restart;
    fs::path consentFile;
    upload::Consent consent;
    std::atomic<HWND> hwnd = nullptr; // null once the dialog is gone
    std::thread sender;
    bool sendStarted = false;
    bool declined = false; // a footer link is closing the dialog through a Send button, which must not send

    // What cfg points at; layOut() fills them.
    std::wstring summary, content, sendRestart, sendClose;
    std::vector<TASKDIALOG_BUTTON> buttons;
    TASKDIALOGCONFIG cfg{};
};

/** Fills st.cfg for one of the dialog's two pages: `ask` offers to send, the other only restarts or closes. */
void layOut(DialogState& st, bool ask);

/** Posted, never sent: the sender thread must not wait on a dialog that may be closing. Text must be a literal. */
void setFooter(DialogState& st, const wchar_t* text)
{
    if (HWND h = st.hwnd)
        PostMessageW(h, TDM_SET_ELEMENT_TEXT, TDE_FOOTER, reinterpret_cast<LPARAM>(text));
}

/** Sends on a thread so the dialog stays responsive; the footer says how it went. */
void startSend(DialogState& st)
{
    if (st.sendStarted)
        return;
    st.sendStarted = true;
    setFooter(st, L"Sending the crash report...");
    st.sender = std::thread([&st] {
        bool ok = upload::send(st.dir);
        setFooter(st, ok ? L"Crash report sent. Thank you, it helps get this fixed." : L"The crash report couldn't be sent. Copy it into a bug report instead.");
    });
}

void copyToClipboard(HWND owner, const std::wstring& text)
{
    if (!OpenClipboard(owner))
        return;
    EmptyClipboard();
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t)))
    {
        std::memcpy(GlobalLock(mem), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem))
            GlobalFree(mem);
    }
    CloseClipboard();
}

void restartLila(const DialogState& st)
{
    if (st.restart.empty())
        return;
    std::wstring cmd = L"\"" + st.restart + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(st.restart.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

HRESULT CALLBACK dialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LONG_PTR ref)
{
    auto* st = reinterpret_cast<DialogState*>(ref);
    if (msg == TDN_DESTROYED)
    {
        st->hwnd = nullptr;
        return S_OK;
    }
    if (msg == TDN_CREATED)
    {
        st->hwnd = hwnd;
        if (st->consent.send && !upload::alreadySent(st->dir))
            startSend(*st);
        // Lila was in front a moment ago; a background process may not take focus, so at least come to the top.
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(hwnd);
        return S_OK;
    }
    if (msg == TDN_HYPERLINK_CLICKED)
    {
        const std::wstring link = reinterpret_cast<const wchar_t*>(lParam);
        if (link == L"report")
            ShellExecuteW(hwnd, L"open", (st->dir / L"report.txt").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else if (link == L"folder")
            ShellExecuteW(hwnd, L"open", st->dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else if (link == L"privacy")
            ShellExecuteW(hwnd, L"open", kPrivacyUrl, nullptr, nullptr, SW_SHOWNORMAL);
        else if (link == L"copy")
        {
            copyToClipboard(hwnd, widen(readText(st->dir / L"report.txt")));
            SendMessageW(hwnd, TDM_SET_ELEMENT_TEXT, TDE_FOOTER, reinterpret_cast<LPARAM>(L"The crash report is on the clipboard."));
        }
        else if (link == L"always")
        {
            upload::setSend(st->consentFile, true);
            st->consent.send = true;
            // Nothing is left to ask, so the Send buttons give way to the page with Restart and Close.
            layOut(*st, false);
            st->cfg.pszFooter = L"Sending the crash report...";
            SendMessageW(hwnd, TDM_NAVIGATE_PAGE, 0, reinterpret_cast<LPARAM>(&st->cfg));
            startSend(*st);
        }
        else if (link == L"restart" || link == L"close")
        {
            if (link == L"restart")
                restartLila(*st);
            // The dialog has no title bar close, so it ends the way a button ends it.
            st->declined = true;
            PostMessageW(hwnd, TDM_CLICK_BUTTON, kSendClose, 0);
        }
        return S_OK;
    }
    if (msg != TDN_BUTTON_CLICKED)
        return S_OK;
    // Every button closes the dialog. A send it started carries on, and the watchdog waits for it before exiting.
    const int button = int(wParam);
    if ((button == kSendRestart || button == kSendClose) && !st->declined)
        startSend(*st);
    if (button == kSendRestart || button == kRestart)
        restartLila(*st);
    return S_OK;
}

void layOut(DialogState& st, bool ask)
{
    const bool canRestart = !st.restart.empty();
    st.content = st.summary + L"\n\n<a href=\"report\">Open report</a>     <a href=\"folder\">Open folder</a>     <a href=\"copy\">Copy report</a>";
    if (st.consent.offer)
        st.content += L"     <a href=\"privacy\">What's sent</a>";

    // Sending gets the only large buttons; declining is a text link in the footer.
    const wchar_t* sendNote = L"The call stack and a memory snapshot go to Lila's developer so this crash gets fixed. "
                              L"Your name, computer name and file paths are removed.";
    st.sendRestart = std::wstring(L"Send Report and Restart Lila\n") + sendNote;
    st.sendClose = canRestart ? std::wstring(L"Send Report and Close") : std::wstring(L"Send Report\n") + sendNote;
    st.buttons.clear();
    if (ask && canRestart)
        st.buttons.push_back({kSendRestart, st.sendRestart.c_str()});
    if (ask)
        st.buttons.push_back({kSendClose, st.sendClose.c_str()});
    if (canRestart && !ask)
        st.buttons.push_back({kRestart, L"Restart Lila"});
    TASKDIALOGCONFIG& cfg = st.cfg;
    cfg = {};
    cfg.cbSize = sizeof(cfg);
    // Not cancellable while sending is on offer: no title bar close and no Esc, so leaving without sending is a
    // deliberate click on a footer link.
    cfg.dwFlags = TDF_SIZE_TO_CONTENT | TDF_ENABLE_HYPERLINKS | TDF_USE_COMMAND_LINKS | (ask ? 0 : TDF_ALLOW_DIALOG_CANCELLATION);
    // A Close button only where there is nothing to decide.
    if (!ask)
        cfg.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    cfg.pszWindowTitle = L"Lila";
    cfg.pszMainIcon = TD_ERROR_ICON;
    cfg.pszMainInstruction = L"Lila crashed";
    cfg.pszContent = st.content.c_str();
    // Asked each time: the footer carries the quieter answers, never being asked again and declining.
    cfg.pszFooter = ask && canRestart    ? L"Your unsaved work, if any, is offered when Lila starts again.\n"
                                           L"<a href=\"always\">Always send automatically</a>     <a href=\"restart\">Restart without sending</a>"
                                           L"     <a href=\"close\">Close without sending</a>"
                    : ask                ? L"Your unsaved work, if any, is offered when Lila starts again.\n"
                                           L"<a href=\"always\">Always send automatically</a>     <a href=\"close\">Close without sending</a>"
                    : st.consent.userOff ? L"Your unsaved work, if any, is offered when Lila starts again.\n"
                                           L"Crash reports are set to never send. Change it in Preferences > General > Telemetry."
                                         : L"Your unsaved work, if any, is offered when Lila starts again.";
    cfg.pszFooterIcon = TD_INFORMATION_ICON;
    cfg.pButtons = st.buttons.data();
    cfg.cButtons = UINT(st.buttons.size());
    if (!st.buttons.empty())
        cfg.nDefaultButton = st.buttons.front().nButtonID;
    cfg.pfCallback = dialogProc;
    cfg.lpCallbackData = reinterpret_cast<LONG_PTR>(&st);
}

void showDialog(const watch::Result& res, const std::wstring& restart, const fs::path& consentFile, const upload::Consent& consent)
{
    DialogState st;
    st.dir = res.reportDir;
    st.restart = restart;
    st.consentFile = consentFile;
    st.consent = consent;
    st.summary = widen(res.summary);
    const std::wstring prefix = L"Lila crashed: ";
    if (st.summary.starts_with(prefix))
        st.summary = st.summary.substr(prefix.size());
    layOut(st, consent.offer && !consent.send && !upload::alreadySent(res.reportDir));
    TaskDialogIndirect(&st.cfg, nullptr, nullptr, nullptr);
    // A send still running finishes before the watchdog exits.
    if (st.sender.joinable())
        st.sender.join();
}

HANDLE handleArg(const wchar_t* v)
{
    return reinterpret_cast<HANDLE>(uintptr_t(_wcstoui64(v, nullptr, 10)));
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    watch::Options o;
    std::wstring restart;
    fs::path consentFile;
    fs::path sendDir; // --send <report folder>: upload one report and exit
    fs::path showDir; // --show <report folder>: the crash dialog for a report already on disk
    bool watchMode = false, ui = true;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        bool hasValue = i + 1 < argc;
        if (a == L"--watch")
            watchMode = true;
        else if (a == L"--no-ui")
            ui = false;
        else if (a == L"--full")
            o.fullDump = true;
        else if (a == L"--process" && hasValue)
            o.process = handleArg(argv[++i]);
        else if (a == L"--shared" && hasValue)
            o.shared = handleArg(argv[++i]);
        else if (a == L"--crash" && hasValue)
            o.crashEvent = handleArg(argv[++i]);
        else if (a == L"--done" && hasValue)
            o.doneEvent = handleArg(argv[++i]);
        else if (a == L"--sessions" && hasValue)
            o.sessionsDir = argv[++i];
        else if (a == L"--crashes" && hasValue)
            o.crashesDir = argv[++i];
        else if (a == L"--keep" && hasValue)
            o.keep = size_t(_wtoi(argv[++i]));
        else if (a == L"--restart" && hasValue)
            restart = argv[++i];
        else if (a == L"--consent" && hasValue)
            consentFile = argv[++i];
        else if (a == L"--send" && hasValue)
            sendDir = argv[++i];
        else if (a == L"--show" && hasValue)
            showDir = argv[++i];
    }
    LocalFree(argv);
    if (!sendDir.empty())
        return upload::send(sendDir) ? 0 : 1;
    if (!showDir.empty())
    {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        watch::Result shown{true, showDir, "Lila crashed: access violation reading 0x0000000000000000"};
        showDialog(shown, restart, consentFile, consentFile.empty() ? upload::Consent{} : upload::readConsent(consentFile));
        return 0;
    }
    if (!watchMode || !o.process || o.sessionsDir.empty() || o.crashesDir.empty())
        return 2;
    if (const char* v = std::getenv("LILA_CRASH_NO_UI"); v && *v && *v != '0')
        ui = false;
    if (const char* v = std::getenv("LILA_CRASH_WER_WAIT_MS"); v && *v)
        o.werWaitMs = DWORD(std::strtoul(v, nullptr, 10));

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // The watchdog waits for hours; it should never compete with Lila for the CPU.
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    const DWORD pid = GetProcessId(o.process);
    watch::Result res = watch::run(o);
    if (!res.crashed)
        return 0;
    // Counts the crash in usage statistics now: the next start would, but a user a crash drives away never starts again.
    std::thread usage;
    if (!consentFile.empty())
        usage = std::thread([dir = consentFile.parent_path(), pid] { upload::sendCrashedSession(dir, pid); });
    struct JoinOnExit
    {
        std::thread& t;
        ~JoinOnExit()
        {
            if (t.joinable())
                t.join();
        }
    } joinUsage{usage};
    if (res.reportDir.empty())
        return 0;
    upload::Consent consent = consentFile.empty() ? upload::Consent{} : upload::readConsent(consentFile);
    if (ui)
    {
        SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
        showDialog(res, restart, consentFile, consent);
        writeTextAtomic(res.reportDir / L".acknowledged", "");
    }
    else if (consent.send)
    {
        upload::send(res.reportDir);
    }
    return 1;
}
