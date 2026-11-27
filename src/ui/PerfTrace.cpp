#include "ui/PerfTrace.h"

#include "core/CrashHandler.h"
#include "core/Paths.h"
#include "core/Profiler.h"
#include "core/Release.h"
#include "core/SystemMemory.h"
#include "core/Version.h"
#include "io/SceneFile.h"
#include "ops/Command.h"
#include "render/Renderer.h"
#include "ui/Actions.h"
#include "ui/FileDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDialog>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <miniz.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <functional>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace lila::ui::perfTrace
{
namespace
{
namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr int kDefaultSeconds = 10;
constexpr int kMaxSeconds = 120;
// Commands may record a whole long operation (a world build runs for many minutes).
constexpr int kMaxCommandSeconds = 3 * 3600;
// Waiting longer than this for tracy-capture to connect means it never will.
constexpr qint64 kConnectTimeoutMs = 15000;

enum class State
{
    Idle,
    Connecting,
    Recording,
    Saving,
    Done,
    Failed
};

const char* stateName(State s)
{
    switch (s)
    {
    case State::Idle: return "idle";
    case State::Connecting: return "connecting";
    case State::Recording: return "recording";
    case State::Saving: return "saving";
    case State::Done: return "done";
    case State::Failed: return "failed";
    }
    return "idle";
}

std::string readmeText()
{
    return "Lila performance trace, " + version::full() +
           "\n\ntrace.tracy     the recording; open it with the Tracy profiler 0.14.1 (github.com/wolfpld/tracy)\n"
           "session.txt     this session: version, system, GPU, recent log and commands\n"
           "info.json       how the recording was made\n";
}

/** Text the trace carries in its header, so it stands on its own without the zip. */
std::string appInfo()
{
    const render::Renderer::GpuInfo gpu = render::Renderer::get().gpuInfo();
    return version::full() + "\nGPU: " + (gpu.name.empty() ? std::string("(renderer not running)") : gpu.name) +
           "\nAdministrator: " + (profiler::elevated() ? "yes" : "no");
}

bool writeZip(const fs::path& zipFile, const fs::path& trace, const json& info, std::string& error)
{
    mz_zip_archive zip{};
    const std::u8string u = zipFile.u8string();
    if (!mz_zip_writer_init_file(&zip, reinterpret_cast<const char*>(u.c_str()), 0))
    {
        error = "Can't write " + paths::toUtf8(zipFile);
        return false;
    }
    auto addText = [&](const char* name, const std::string& text) {
        return mz_zip_writer_add_mem(&zip, name, text.data(), text.size(), MZ_DEFAULT_LEVEL);
    };
    bool ok = addText("README.txt", readmeText()) && addText("session.txt", crash::liveReport()) && addText("info.json", info.dump(2));
#ifdef _WIN32
    FILE* in = _wfopen(trace.c_str(), L"rb");
#else
    FILE* in = std::fopen(trace.c_str(), "rb");
#endif
    if (ok && in)
    {
        std::error_code ec;
        const uintmax_t size = fs::file_size(trace, ec);
        // Tracy compresses its own files; deflating them again only costs time.
        ok = mz_zip_writer_add_cfile(&zip, "trace.tracy", in, size, nullptr, nullptr, 0, MZ_NO_COMPRESSION, nullptr, 0, nullptr, 0);
    }
    else
        ok = false;
    if (in)
        std::fclose(in);
    ok = ok && mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    if (!ok)
    {
        error = "Can't write the trace into " + paths::toUtf8(zipFile);
        std::error_code ec;
        fs::remove(zipFile, ec);
    }
    return ok;
}

fs::path captureExe()
{
#ifdef _WIN32
    return paths::executableDir() / "tracy-capture.exe";
#else
    return paths::executableDir() / "tracy-capture";
#endif
}

/** One recording at a time, shared by the dialog and the commands. */
class Recorder
{
public:
    static Recorder& get()
    {
        static Recorder* r = new Recorder;
        return *r;
    }

    fs::path start(int seconds, bool fromCommand = false)
    {
        if (busy())
            throw std::runtime_error("A recording is already running.");
        if (!profiler::available())
            throw std::runtime_error("This build of Lila has no profiler.");
        const fs::path exe = captureExe();
        std::error_code ec;
        if (!fs::exists(exe, ec))
            throw std::runtime_error("tracy-capture is missing from Lila's folder. Reinstalling Lila puts it back.");
        const uint16_t port = profiler::port();
        if (!port)
            throw std::runtime_error("Lila's profiler isn't listening yet. Try again in a moment.");

        fs::create_directories(folder(), ec);
        const std::string stem = "lila-perf-" + QDateTime::currentDateTime().toString("yyyy-MM-dd-HHmmss").toStdString();
        mTrace = folder() / (stem + ".tracy");
        mZip = folder() / (stem + ".zip");
        mSeconds = recordingSeconds(seconds, fromCommand);
        mError.clear();
        mSystemTracing = false;
        mOutput.clear();
        profiler::setAppInfo(appInfo());

        delete mProcess;
        mProcess = new QProcess;
        mProcess->setProgram(QString::fromStdWString(exe.wstring()));
        mProcess->setArguments({"-a", "127.0.0.1", "-p", QString::number(port), "-o", QString::fromStdWString(mTrace.wstring()), "-f", "-s",
                                QString::number(mSeconds)});
        mProcess->setProcessChannelMode(QProcess::MergedChannels);
#ifdef _WIN32
        mProcess->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) { a->flags |= CREATE_NO_WINDOW; });
#endif
        QObject::connect(mProcess, &QProcess::readyRead, [this] { mOutput += mProcess->readAll(); });
        QObject::connect(mProcess, &QProcess::finished, [this](int code, QProcess::ExitStatus) { finished(code); });
        QObject::connect(mProcess, &QProcess::errorOccurred, [this](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart)
                fail("tracy-capture didn't start: " + mProcess->errorString().toStdString());
        });
        mState = State::Connecting;
        mClock.start();
        mProcess->start();
        mTick.start(100);
        notify();
        return mZip;
    }

    bool busy() const { return mState == State::Connecting || mState == State::Recording || mState == State::Saving; }
    State state() const { return mState; }
    /** Catches up with the recorder when the main thread was too busy for the tick to see it connect. */
    void poll()
    {
        if (mState == State::Connecting || mState == State::Recording)
            tick();
    }
    int secondsLeft() const
    {
        if (mState == State::Connecting)
            return mSeconds;
        if (mState != State::Recording)
            return 0;
        return std::max(0, mSeconds - int(mClock.elapsed() / 1000));
    }
    int seconds() const { return mSeconds; }
    const fs::path& zip() const { return mZip; }
    const std::string& error() const { return mError; }
    bool systemTracing() const { return mSystemTracing; }

    /** Called on every change of state and once a tick while recording. */
    std::function<void()> onChange;

private:
    Recorder()
    {
        QObject::connect(&mTick, &QTimer::timeout, [this] { tick(); });
    }

    void tick()
    {
        if (mState == State::Connecting)
        {
            if (profiler::connected())
            {
                mState = State::Recording;
                mClock.start();
                mSystemTracing = profiler::beginSystemTracing();
            }
            // A recorder that already exited has connected and finished while the main thread was busy; finished() reads its file.
            else if (mClock.elapsed() > kConnectTimeoutMs && mProcess && mProcess->state() == QProcess::Running)
            {
                fail("The recorder couldn't connect to Lila.");
                return;
            }
        }
        notify();
    }

    void finished(int code)
    {
        mTick.stop();
        if (mSystemTracing)
            profiler::endSystemTracing();
        if (mState == State::Failed)
            return;
        std::error_code ec;
        if (!fs::exists(mTrace, ec) || fs::file_size(mTrace, ec) == 0)
        {
            fail("The recorder stopped without writing a trace (exit code " + std::to_string(code) + ").\n" + mOutput.trimmed().right(400).toStdString());
            return;
        }
        mState = State::Saving;
        notify();
        // Lets the dialog show "Saving" before the zip blocks for a moment.
        QTimer::singleShot(0, [this] {
            const json info = {{"version", version::full()},
                               {"seconds", mSeconds},
                               {"administrator", profiler::elevated()},
                               {"systemTracing", mSystemTracing},
                               {"gpu", render::Renderer::get().gpuInfo().name},
                               {"recorded", QDateTime::currentDateTime().toString(Qt::ISODate).toStdString()}};
            std::string error;
            if (writeZip(mZip, mTrace, info, error))
            {
                std::error_code rm;
                fs::remove(mTrace, rm);
                mState = State::Done;
                notify();
            }
            else
                fail(error);
        });
    }

    void fail(std::string error)
    {
        mTick.stop();
        if (mProcess && mProcess->state() != QProcess::NotRunning)
            mProcess->kill();
        if (mSystemTracing)
            profiler::endSystemTracing();
        mError = std::move(error);
        mState = State::Failed;
        notify();
    }

    void notify()
    {
        if (onChange)
            onChange();
    }

    State mState = State::Idle;
    QProcess* mProcess = nullptr;
    QTimer mTick;
    QElapsedTimer mClock;
    QByteArray mOutput;
    fs::path mTrace, mZip;
    int mSeconds = kDefaultSeconds;
    std::string mError;
    bool mSystemTracing = false;
};

#ifdef _WIN32
/** Starts Lila again elevated, reopening the scene and this dialog. False when Windows' prompt was declined. */
bool launchElevated(const fs::path& scene)
{
    std::wstring params;
    if (!scene.empty())
        params = L"\"" + scene.wstring() + L"\" ";
    params += L"-c \"import lila.ops; lila.ops.perfTraceDialog()\"";
    const std::wstring exe = QCoreApplication::applicationFilePath().toStdWString();
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = params.c_str();
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info);
}
#endif

QMainWindow* mainWindow()
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* m = qobject_cast<QMainWindow*>(w))
            return m;
    return nullptr;
}

/** Closes Lila the normal way (it asks to save), then reopens it as administrator. */
void restartElevated()
{
#ifdef _WIN32
    QMainWindow* window = mainWindow();
    if (!window || !window->close())
        return;
    if (!launchElevated(io::currentPath()))
    {
        window->show();
        return;
    }
    QCoreApplication::quit();
#endif
}

QLabel* wrapped(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::RichText);
    return label;
}

class TraceDialog : public QDialog
{
public:
    explicit TraceDialog(QWidget* parent) : QDialog(parent)
    {
        setWindowTitle("Record Performance Trace");
        setAttribute(Qt::WA_DeleteOnClose);
        auto* layout = new QVBoxLayout(this);

        layout->addWidget(wrapped("Records what Lila is doing for a few seconds, so a slow viewport can be tracked down. Press "
                                  "<b>Record</b>, then do whatever is slow (orbit, select, scrub, edit) until the recording stops.",
                                  this));
        layout->addWidget(wrapped("The recording holds timings, the names of Lila's functions, your system and GPU, and this session's "
                                  "log. It holds no scene or model data. Nothing is sent anywhere: attach the zip to your report.",
                                  this));

        auto* admin = new QFrame(this);
        admin->setFrameShape(QFrame::StyledPanel);
        auto* adminLayout = new QVBoxLayout(admin);
        if (profiler::elevated())
        {
            adminLayout->addWidget(wrapped("<b>Running as administrator.</b> The recording also samples where time goes inside Lila and the "
                                           "graphics driver, and notes which programs used the CPU (their names only). When you're done, "
                                           "close Lila and open it normally.",
                                           admin));
        }
        else
        {
            adminLayout->addWidget(wrapped("<b>Optional: run as administrator for more detail.</b> Windows only lets a program sample where "
                                           "its time goes (including inside the graphics driver) and see when other programs take the CPU "
                                           "while it runs as administrator. Without it the recording still works and is still useful.",
                                           admin));
            adminLayout->addWidget(wrapped("Running as administrator changes nothing on your PC: it only lifts that restriction while Lila "
                                           "is open. When you're done, close Lila and open it normally.",
                                           admin));
#ifdef _WIN32
            auto* restart = new QPushButton("Restart as Administrator...", admin);
            restart->setToolTip("Lila closes (asking to save first) and opens again as administrator, with this window. Windows asks you "
                                "to confirm.");
            connect(restart, &QPushButton::clicked, this, [this] {
                close();
                restartElevated();
            });
            auto* row = new QHBoxLayout;
            row->addWidget(restart);
            row->addStretch();
            adminLayout->addLayout(row);
#endif
        }
        layout->addWidget(admin);

        auto* durationRow = new QHBoxLayout;
        durationRow->addWidget(new QLabel("Record for", this));
        mSeconds = new QSpinBox(this);
        mSeconds->setRange(1, kMaxSeconds);
        mSeconds->setValue(kDefaultSeconds);
        mSeconds->setSuffix(" s");
        durationRow->addWidget(mSeconds);
        durationRow->addStretch();
        layout->addLayout(durationRow);

        mProgress = new QProgressBar(this);
        mProgress->setTextVisible(false);
        layout->addWidget(mProgress);
        mStatus = wrapped("", this);
        mStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(mStatus);

        auto* buttons = new QDialogButtonBox(this);
        mRecord = buttons->addButton("Record", QDialogButtonBox::ActionRole);
        mReveal = buttons->addButton("Show in Explorer", QDialogButtonBox::ActionRole);
        mCopy = buttons->addButton("Copy Path", QDialogButtonBox::ActionRole);
        auto* openFolder = buttons->addButton("Open Folder", QDialogButtonBox::ActionRole);
        openFolder->setToolTip("Opens the folder that holds every recording.");
        buttons->addButton(QDialogButtonBox::Close);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
        connect(mRecord, &QPushButton::clicked, this, [this] {
            try
            {
                Recorder::get().start(mSeconds->value());
            }
            catch (const std::exception& e)
            {
                mStatus->setText(QString::fromStdString(e.what()).toHtmlEscaped());
            }
        });
        connect(mReveal, &QPushButton::clicked, this, [] { fileOps::reveal(QString::fromStdWString(Recorder::get().zip().wstring())); });
        connect(openFolder, &QPushButton::clicked, this, [] {
            std::error_code ec;
            fs::create_directories(folder(), ec);
            QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdWString(folder().wstring())));
        });
        connect(mCopy, &QPushButton::clicked, this,
                [] { QApplication::clipboard()->setText(QString::fromStdWString(Recorder::get().zip().wstring())); });
        layout->addWidget(buttons);

        Recorder::get().onChange = [self = QPointer<TraceDialog>(this)] {
            if (self)
                self->refresh();
        };
        refresh();
        resize(520, sizeHint().height());
    }

    ~TraceDialog() override { Recorder::get().onChange = nullptr; }

private:
    void refresh()
    {
        const Recorder& r = Recorder::get();
        const State s = r.state();
        mRecord->setEnabled(!r.busy());
        mSeconds->setEnabled(!r.busy());
        mReveal->setVisible(s == State::Done);
        mCopy->setVisible(s == State::Done);
        mProgress->setVisible(r.busy());
        const QString file = QString::fromStdWString(r.zip().filename().wstring()).toHtmlEscaped();
        switch (s)
        {
        case State::Idle: mStatus->setText(""); break;
        case State::Connecting:
            mProgress->setRange(0, 0);
            mStatus->setText("Starting the recorder...");
            break;
        case State::Recording:
            mProgress->setRange(0, r.seconds());
            mProgress->setValue(r.seconds() - r.secondsLeft());
            mStatus->setText(QString("<b>Recording: %1 s left.</b> Do the slow thing in the viewport now.").arg(r.secondsLeft()));
            break;
        case State::Saving:
            mProgress->setRange(0, 0);
            mStatus->setText("Saving...");
            break;
        case State::Done:
            mStatus->setText("Saved <b>" + file + "</b>. Attach it to your report, with a line on what you were doing." +
                             (r.systemTracing() ? QString() : QString(" (Recorded without administrator detail.)")));
            break;
        case State::Failed: mStatus->setText(QString::fromStdString(r.error()).toHtmlEscaped().replace("\n", "<br>")); break;
        }
    }

    QSpinBox* mSeconds = nullptr;
    QProgressBar* mProgress = nullptr;
    QLabel* mStatus = nullptr;
    QPushButton* mRecord = nullptr;
    QPushButton* mReveal = nullptr;
    QPushButton* mCopy = nullptr;
};

QPointer<TraceDialog> gDialog;
} // namespace

fs::path folder() { return paths::userConfigDir() / "perf-traces"; }

fs::path record(int seconds) { return Recorder::get().start(seconds, true); }

int recordingSeconds(int seconds, bool fromCommand) { return std::clamp(seconds, 1, fromCommand ? kMaxCommandSeconds : kMaxSeconds); }

json status()
{
    Recorder& r = Recorder::get();
    r.poll();
    return {{"state", stateName(r.state())},
            {"secondsLeft", r.secondsLeft()},
            {"path", r.zip().empty() ? std::string() : paths::toUtf8(r.zip())},
            {"error", r.error()},
            {"available", profiler::available()},
            {"elevated", profiler::elevated()},
            {"systemTracing", r.systemTracing()}};
}

void showDialog(QWidget* parent)
{
    if (!gDialog)
        gDialog = new TraceDialog(parent);
    gDialog->show();
    gDialog->raise();
    gDialog->activateWindow();
}

void registerAll(QWidget* mainWindow)
{
    actions::add({"help.perfTrace", "Record Performance Trace...", "Help", [mainWindow] { showDialog(mainWindow); }, nullptr,
                  "Records a few seconds of what Lila is doing, to attach to a report about a slow viewport."});
    ops::registerCommand({
        .name = "perfTraceRecord",
        .doc = "Start recording a performance trace (Help > Record Performance Trace) for `seconds` once the recorder connects. "
               "Returns the zip it will write; perfTraceStatus says when it is done.",
        .flags = {{.name = "seconds", .shortName = "s", .type = ArgType::Int, .defaultValue = kDefaultSeconds, .doc = "How long to record.",
                   .min = 1, .max = kMaxCommandSeconds}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            try
            {
                return paths::toUtf8(record(a.get<int>("seconds", kDefaultSeconds)));
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("perfTraceRecord: ") + e.what());
            }
        },
        .examples = {"lila.ops.perfTraceRecord(seconds=10)"},
    });
    ops::registerCommand({
        .name = "perfTraceStatus",
        .doc = "The performance trace recording: state (idle, connecting, recording, saving, done, failed), secondsLeft, path (the zip), "
               "error, elevated and systemTracing (call-stack sampling and context switches, only when running as administrator).",
        .undoable = false,
        .run = [](const Args&) -> json { return status(); },
    });
    ops::registerCommand({
        .name = "memoryReport",
        .doc = "This process's memory in MB: commitMB (private commit), workingSetMB, releasePending (pieces the background "
               "releaser still has to free). heaps= walks the process heaps (seconds on a big scene) and adds busyMB (allocated), "
               "freeMB (committed but free), segmentsMB, block counts and busyBySizeMB (allocated MB by block size). drain= "
               "first waits for the releaser to finish. types=N lists the N most numerous live objects with virtual functions "
               "by class (shared objects as std::_Ref_count_obj2<T>), to find what holds memory after a scene is replaced. "
               "holders=<class text> follows references back from samples= such objects depth= levels, naming each holder "
               "(class, static variable or block size); it stops the whole process while it scans.",
        .flags = {{"heaps", "h", ArgType::Bool, false, "Walk the process heaps."},
                  {"drain", "d", ArgType::Bool, false, "Wait for the background releaser first."},
                  {.name = "types", .shortName = "t", .type = ArgType::Int, .defaultValue = 0, .doc = "List the most numerous live object types.", .min = 0, .max = 1000},
                  {"holders", "ho", ArgType::String, "", "Trace what holds live objects whose class name contains this text."},
                  {.name = "samples", .shortName = "sa", .type = ArgType::Int, .defaultValue = 16, .doc = "Objects holders= follows.", .min = 1, .max = 4096},
                  {.name = "depth", .shortName = "de", .type = ArgType::Int, .defaultValue = 3, .doc = "Levels of holders to follow.", .min = 1, .max = 8}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            constexpr double kMB = 1048576.0;
            json out = json::object();
            if (a.get<bool>("drain", false))
            {
                QElapsedTimer t;
                t.start();
                release::drain();
                out["drainSeconds"] = double(t.elapsed()) / 1000.0;
            }
            const ProcessMemory pm = processMemory();
            out["commitMB"] = std::round(double(pm.commit) / kMB);
            out["workingSetMB"] = std::round(double(pm.workingSet) / kMB);
            out["releasePending"] = release::pending();
            if (a.get<bool>("heaps", false))
            {
                const HeapUse h = heapUse();
                out["heaps"] = h.heaps;
                out["busyMB"] = std::round(double(h.busy) / kMB);
                out["freeMB"] = std::round(double(h.free) / kMB);
                out["segmentsMB"] = std::round(double(h.committed) / kMB);
                out["busyBlocks"] = h.busyBlocks;
                out["freeBlocks"] = h.freeBlocks;
                json sizes = json::object();
                for (size_t i = 0; i < h.busyBySize.size(); ++i)
                    if (h.busyBySize[i] >= size_t(kMB))
                        sizes[i == 0 ? std::string("<128") : ">=" + std::to_string(size_t(1) << (i + 6))] = std::round(double(h.busyBySize[i]) / kMB);
                out["busyBySizeMB"] = std::move(sizes);
            }
            if (const int top = a.get<int>("types", 0); top > 0)
            {
                json types = json::array();
                for (const HeapType& t : heapTypes(size_t(top)))
                    types.push_back({{"type", t.name}, {"count", t.count}, {"MB", std::round(double(t.bytes) / kMB * 10.0) / 10.0}});
                out["types"] = std::move(types);
            }
            if (const std::string type = a.get<std::string>("holders", ""); !type.empty())
            {
                const HeapHolders h = heapHolders(type, size_t(a.get<int>("samples", 16)), size_t(a.get<int>("depth", 3)));
                json levels = json::array();
                for (const auto& level : h.levels)
                {
                    json l = json::array();
                    for (const HeapHolders::Holder& x : level)
                        l.push_back({{"holder", x.what}, {"count", x.count}});
                    levels.push_back(std::move(l));
                }
                out["holders"] = {{"objects", h.objects}, {"levels", std::move(levels)}};
            }
            return out;
        },
        .examples = {"lila.ops.memoryReport(heaps=True, drain=True)"},
    });
    ops::registerCommand({
        .name = "perfTraceDialog",
        .doc = "Open Help > Record Performance Trace.",
        .undoable = false,
        .run = [mainWindow](const Args&) -> json {
            showDialog(mainWindow);
            return nullptr;
        },
    });
}
} // namespace lila::ui::perfTrace
