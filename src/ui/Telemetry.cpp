#include "ui/Telemetry.h"

#include "core/CrashUpload.h"
#include "core/FileBytes.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/SystemMemory.h"
#include "core/Version.h"
#include "extensions/ExtensionManager.h"
#include "io/Recovery.h"
#include "link/UnrealLink.h"
#include "ops/Command.h"
#include "render/Renderer.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScreen>
#include <QSysInfo>
#include <QTimeZone>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <memory>

namespace lila::telemetry
{
namespace
{
/** The ingest Worker (services/telemetry-worker). LILA_TELEMETRY_URL overrides it. */
constexpr const char* kEndpoint = "https://lila-telemetry.lila3d.workers.dev/e";
constexpr int kFlushIntervalMs = 30 * 60 * 1000;
constexpr int kFirstFlushDelayMs = 10 * 1000;

bool envSet(const char* name)
{
    const char* v = std::getenv(name);
    return v && *v && std::string_view(v) != "0";
}

std::string newUuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
}

std::string isoTime(int64_t ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(Qt::ISODateWithMs).toStdString();
}

std::string endpoint()
{
    if (const char* v = std::getenv("LILA_TELEMETRY_URL"); v && *v)
        return v;
    return kEndpoint;
}

json readJson(const fs::path& file)
{
    std::ifstream in(file);
    if (!in)
        return nullptr;
    return json::parse(in, nullptr, false);
}

/** Writes beside and renames over, so a crash mid-write leaves the old file. */
void writeJson(const fs::path& file, const json& j)
{
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
            return;
        out << j.dump();
    }
    renameRetrying(tmp, file);
}
} // namespace

const std::vector<std::pair<std::string, std::string>>& roleChoices()
{
    static const std::vector<std::pair<std::string, std::string>> v = {
        {"Prefer not to say", ""},          {"3D / environment artist", "artist3d"}, {"Character artist", "character"},
        {"Technical artist", "techArt"},     {"Animator / rigger", "animation"},     {"Game designer", "design"},
        {"Programmer", "programmer"},        {"Student", "student"},                 {"Hobbyist", "hobbyist"},
        {"Other", "other"},
    };
    return v;
}

const std::vector<std::pair<std::string, std::string>>& teamSizeChoices()
{
    static const std::vector<std::pair<std::string, std::string>> v = {
        {"Prefer not to say", ""}, {"Just me", "1"}, {"2-10", "2-10"}, {"11-50", "11-50"}, {"51-250", "51-250"}, {"250+", "250+"},
    };
    return v;
}

const std::vector<std::pair<std::string, std::string>>& previousDccChoices()
{
    static const std::vector<std::pair<std::string, std::string>> v = {
        {"Prefer not to say", ""}, {"Maya", "maya"},   {"Blender", "blender"}, {"3ds Max", "max"},
        {"Houdini", "houdini"},    {"Modo", "modo"},   {"Other", "other"},     {"None, Lila is my first", "none"},
    };
    return v;
}

bool optedOutByEnvironment()
{
    return envSet("DO_NOT_TRACK") || envSet("LILA_NO_TELEMETRY");
}

bool enabled()
{
    return !optedOutByEnvironment() && PrefStore::get().value<bool>("general", ui::kTelemetryUsagePref, false);
}

std::string crashSendMode()
{
    PrefStore& prefs = PrefStore::get();
    if (prefs.isLocked("general", ui::kTelemetryCrashPref) && !prefs.value<bool>("general", ui::kTelemetryCrashPref, false))
        return "off";
    std::string mode = prefs.value<std::string>("general", kCrashSendPref, "");
    if (mode == "auto" || mode == "ask" || mode == "off")
        return mode;
    // An install that ticked the crashReports switch has already said to send.
    return prefs.value<bool>("general", ui::kTelemetryCrashPref, false) ? "auto" : "";
}

const std::vector<std::pair<std::string, std::string>>& crashSendChoices()
{
    static const std::vector<std::pair<std::string, std::string>> choices = {
        {"Send automatically", "auto"}, {"Ask me each time", "ask"}, {"Never send", "off"}};
    return choices;
}

std::string installId()
{
    PrefStore& prefs = PrefStore::get();
    std::string id = prefs.value<std::string>("general", kInstallIdPref, "");
    if (!id.empty())
        return id;
    id = newUuid();
    prefs.set("general", kInstallIdPref, id);
    if (prefs.value("general", kInstalledAtPref).is_null())
        prefs.set("general", kInstalledAtPref, QDateTime::currentSecsSinceEpoch());
    return id;
}

std::string resetInstallId()
{
    std::string id = newUuid();
    PrefStore::get().set("general", kInstallIdPref, id);
    PrefStore::get().remove("general", kOnboardingSentPref);
    return id;
}

std::string memoryBucket(double gb)
{
    if (gb < 7.5)
        return "<8";
    if (gb < 15.5)
        return "8-15";
    if (gb < 31.5)
        return "16-31";
    if (gb < 63.5)
        return "32-63";
    if (gb < 127.5)
        return "64-127";
    return "128+";
}

std::string daysBucket(int64_t days)
{
    if (days < 1)
        return "0";
    if (days < 7)
        return "1-6";
    if (days < 30)
        return "7-29";
    if (days < 90)
        return "30-89";
    return "90+";
}

std::string installChannel(const fs::path& exeDir)
{
    std::string lower = paths::toUtf8(exeDir);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (lower.find("steamapps") != std::string::npos)
        return "steam";
    std::error_code ec;
    fs::path dir = exeDir;
    for (int up = 0; up < 4 && !dir.empty(); ++up, dir = dir.parent_path())
    {
        if (fs::exists(dir / "CMakeCache.txt", ec))
            return "source";
        if (dir == dir.parent_path())
            break;
    }
    return "installer";
}

json Session::toJson() const
{
    return {{"id", id},
            {"pid", pid},
            {"startedMs", startedMs},
            {"updatedMs", updatedMs},
            {"activeMinutes", activeMinutes},
            {"commands", commands},
            {"otherCommands", otherCommands},
            {"sources", sources},
            {"unrealCalls", unrealCalls},
            {"unrealVersions", unrealVersions}};
}

Session Session::fromJson(const json& j)
{
    Session s;
    if (!j.is_object())
        return s;
    s.id = j.value("id", std::string());
    s.pid = j.value("pid", int64_t{0});
    s.startedMs = j.value("startedMs", int64_t{0});
    s.updatedMs = j.value("updatedMs", int64_t{0});
    s.activeMinutes = j.value("activeMinutes", 0);
    s.commands = j.value("commands", std::map<std::string, int>{});
    s.otherCommands = j.value("otherCommands", 0);
    s.sources = j.value("sources", std::map<std::string, int>{});
    s.unrealCalls = j.value("unrealCalls", 0);
    s.unrealVersions = j.value("unrealVersions", std::vector<std::string>{});
    return s;
}

void countCommand(Session& s, std::string_view name, bool builtIn, std::string_view source)
{
    if (builtIn)
        ++s.commands[std::string(name)];
    else
        ++s.otherCommands;
    ++s.sources[std::string(source)];
}

json makeEvent(std::string_view name, const std::string& distinctId, int64_t timeMs, json properties)
{
    properties["$lib"] = "lila";
    properties["version"] = version::number();
    // Events arrive from the ingest Worker's address, so PostHog's own lookup would place everyone there.
    properties["$geoip_disable"] = true;
    return {{"event", name}, {"distinct_id", distinctId}, {"timestamp", isoTime(timeMs)}, {"properties", std::move(properties)}};
}

json sessionEndEvent(const Session& s, const std::string& distinctId, bool crashed)
{
    json props = {{"session", s.id},
                  {"duration_s", std::max<int64_t>(0, (s.updatedMs - s.startedMs) / 1000)},
                  {"active_minutes", s.activeMinutes},
                  {"crashed", crashed},
                  {"commands", s.commands},
                  {"other_commands", s.otherCommands},
                  {"command_sources", s.sources},
                  {"unreal_calls", s.unrealCalls},
                  {"unreal_versions", s.unrealVersions}};
    return makeEvent("session_end", distinctId, s.updatedMs, std::move(props));
}

json Store::queue() const
{
    json q = readJson(mDir / "queue.json");
    return q.is_array() ? q : json::array();
}

void Store::writeQueue(const json& q) const
{
    writeJson(mDir / "queue.json", q);
}

void Store::enqueue(json event)
{
    json q = queue();
    q.push_back(std::move(event));
    if (q.size() > kMaxQueued)
        q.erase(q.begin(), q.begin() + std::ptrdiff_t(q.size() - kMaxQueued));
    writeQueue(q);
}

void Store::dequeue(size_t count)
{
    json q = queue();
    q.erase(q.begin(), q.begin() + std::ptrdiff_t(std::min(count, q.size())));
    writeQueue(q);
}

void Store::failed(size_t count)
{
    json q = queue();
    json kept = json::array();
    for (size_t i = 0; i < q.size(); ++i)
    {
        json& e = q[i];
        if (i < count)
        {
            int attempts = e.value("attempts", 0) + 1;
            if (attempts >= kMaxAttempts)
                continue;
            e["attempts"] = attempts;
        }
        kept.push_back(std::move(e));
    }
    writeQueue(kept);
}

void Store::clear()
{
    std::error_code ec;
    fs::remove(mDir / "queue.json", ec);
}

void Store::writeSession(const Session& s)
{
    writeJson(mDir / "sessions" / (std::to_string(s.pid) + ".json"), s.toJson());
}

void Store::removeSession(int64_t pid)
{
    std::error_code ec;
    fs::remove(mDir / "sessions" / (std::to_string(pid) + ".json"), ec);
}

std::vector<Session> Store::takeStaleSessions(int64_t selfPid, const std::function<bool(int64_t, int64_t)>& alive)
{
    std::vector<Session> out;
    std::error_code ec;
    for (fs::directory_iterator it(mDir / "sessions", ec), end; !ec && it != end; it.increment(ec))
    {
        if (it->path().extension() != ".json")
            continue;
        Session s = Session::fromJson(readJson(it->path()));
        if (s.pid != selfPid && alive(s.pid, s.startedMs))
            continue;
        std::error_code rm;
        fs::remove(it->path(), rm);
        if (!s.id.empty())
            out.push_back(std::move(s));
    }
    return out;
}

namespace
{
std::string source(CallSource src)
{
    if (src == CallSource::UI)
        return "ui";
    return ops::agentCalling() ? "mcp" : "python";
}

bool builtInCommand(std::string_view name)
{
    const CommandDef* def = ops::find(name);
    return def && !def->scripted && (def->owner.empty() || def->owner == "lila");
}

json sessionStartProperties(const Session& s)
{
    render::Renderer::GpuInfo gpu = render::Renderer::get().gpuInfo();
    int64_t installedAt = PrefStore::get().value<int64_t>("general", kInstalledAtPref, QDateTime::currentSecsSinceEpoch());
    return {{"session", s.id},
            {"build", version::buildId()},
            {"os", QSysInfo::prettyProductName().toStdString()},
            {"cpu_arch", QSysInfo::currentCpuArchitecture().toStdString()},
            {"gpu", gpu.name},
            {"gpu_discrete", gpu.discrete},
            {"vram_gb", memoryBucket(double(gpu.vramBytes) / double(1ull << 30))},
            {"ram_gb", memoryBucket(double(physicalMemoryBytes()) / double(1ull << 30))},
            {"displays", int(QGuiApplication::screens().size())},
            {"locale", QLocale::system().name().toStdString()},
            {"channel", installChannel(paths::executableDir())},
            {"days_since_install", daysBucket((QDateTime::currentSecsSinceEpoch() - installedAt) / 86400)},
            {"extensions", int(extensions::ExtensionManager::get().loaded().size())}};
}

/** Marks the minute active on any keyboard, mouse, wheel or tablet input. */
class ActivityFilter : public QObject
{
public:
    using QObject::QObject;
    int64_t lastInputMs = 0;

protected:
    bool eventFilter(QObject* watched, QEvent* e) override
    {
        switch (e->type())
        {
        case QEvent::KeyPress:
        case QEvent::MouseButtonPress:
        case QEvent::MouseMove:
        case QEvent::Wheel:
        case QEvent::TabletPress:
        case QEvent::TabletMove:
            lastInputMs = io::recovery::nowMs();
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, e);
    }
};

struct State
{
    std::unique_ptr<Store> store;
    bool running = false; // a session is being counted
    Session session;
    QPointer<ActivityFilter> activity;
    QPointer<QTimer> minute;
    QPointer<QTimer> flushTimer;
    QPointer<QNetworkAccessManager> nam;
    bool sending = false;
    int listener = 0;
    int prefsListener = 0;
};

State& state()
{
    static State s;
    return s;
}

/** Queues the onboarding answers once per distinct set, as person properties. */
void queueOnboarding(Store& store)
{
    PrefStore& prefs = PrefStore::get();
    json answers = json::object();
    for (auto [name, key] : {std::pair{"role", kRolePref}, std::pair{"team_size", kTeamSizePref}, std::pair{"previous_dcc", kPreviousDccPref}})
        if (std::string v = prefs.value<std::string>("general", key, ""); !v.empty())
            answers[name] = v;
    if (answers.empty() || prefs.value("general", kOnboardingSentPref) == answers)
        return;
    store.enqueue(makeEvent("onboarding", installId(), io::recovery::nowMs(), {{"$set", answers}}));
    prefs.set("general", kOnboardingSentPref, answers);
}

void flush()
{
    State& st = state();
    std::string url = endpoint();
    if (!st.store || st.sending || url.empty() || !enabled())
        return;
    json q = st.store->queue();
    if (q.empty())
        return;
    json batch = json::array();
    for (const json& e : q)
    {
        json clean = e;
        clean.erase("attempts");
        batch.push_back(std::move(clean));
    }
    if (!st.nam)
        st.nam = new QNetworkAccessManager(qApp);
    QNetworkRequest req{QUrl(QString::fromStdString(url))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setTransferTimeout(10000);
    st.sending = true;
    QNetworkReply* reply = st.nam->post(req, QByteArray::fromStdString(json{{"batch", batch}}.dump()));
    size_t count = q.size();
    QObject::connect(reply, &QNetworkReply::finished, qApp, [reply, count] {
        reply->deleteLater();
        State& s = state();
        s.sending = false;
        if (!s.store)
            return;
        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError && status >= 200 && status < 300)
            s.store->dequeue(count);
        else
            s.store->failed(count);
    });
}

void tickMinute()
{
    State& st = state();
    if (!st.running)
        return;
    int64_t now = io::recovery::nowMs();
    if (st.activity && now - st.activity->lastInputMs < 60 * 1000)
        ++st.session.activeMinutes;
    st.session.updatedMs = now;
    st.store->writeSession(st.session);
}

void beginSession()
{
    State& st = state();
    if (st.running || !st.store)
        return;
    st.running = true;
    st.session = Session{};
    st.session.id = newUuid();
    st.session.pid = io::recovery::currentPid();
    st.session.startedMs = st.session.updatedMs = io::recovery::nowMs();
    st.store->writeSession(st.session);
    // GPU details are filled once the renderer has picked a device.
    QTimer::singleShot(kFirstFlushDelayMs, qApp, [] {
        State& s = state();
        if (!s.running)
            return;
        s.store->enqueue(makeEvent("session_start", installId(), s.session.startedMs, sessionStartProperties(s.session)));
        queueOnboarding(*s.store);
        flush();
    });
}

void endSession(bool keepEvents)
{
    State& st = state();
    if (!st.running)
        return;
    st.running = false;
    st.store->removeSession(st.session.pid);
    if (keepEvents)
    {
        st.session.updatedMs = io::recovery::nowMs();
        st.store->enqueue(sessionEndEvent(st.session, installId(), false));
    }
}

/** What the crash watchdog reads when Lila dies (see CrashUpload.h). */
void writeCrashConsent()
{
    PrefStore& prefs = PrefStore::get();
    crash::upload::Consent c;
    const std::string mode = crashSendMode();
    c.offer = !optedOutByEnvironment() && mode != "off";
    c.send = c.offer && mode == "auto";
    c.userOff = mode == "off" && !optedOutByEnvironment() && !prefs.isLocked("general", kCrashSendPref) &&
                !prefs.isLocked("general", ui::kTelemetryCrashPref);
    c.prefsFile = prefs.userDir() / "general.json";
    crash::upload::writeConsent(prefs.userDir() / "telemetry" / crash::upload::kConsentFile, c);
}

void followPref()
{
    State& st = state();
    if (!st.store)
        return;
    writeCrashConsent();
    if (enabled())
    {
        beginSession();
        queueOnboarding(*st.store);
    }
    else if (st.running)
    {
        // Turned off: nothing collected so far leaves the machine.
        endSession(false);
        st.store->clear();
    }
}
} // namespace

void start()
{
    State& st = state();
    if (st.store)
        return;
    st.store = std::make_unique<Store>(PrefStore::get().userDir() / "telemetry");

    std::vector<Session> crashed = st.store->takeStaleSessions(io::recovery::currentPid(), io::recovery::processAlive);
    if (enabled())
        for (const Session& s : crashed)
            st.store->enqueue(sessionEndEvent(s, installId(), true));

    st.activity = new ActivityFilter(qApp);
    qApp->installEventFilter(st.activity);
    st.minute = new QTimer(qApp);
    QObject::connect(st.minute, &QTimer::timeout, qApp, [] { tickMinute(); });
    st.minute->start(60 * 1000);
    st.flushTimer = new QTimer(qApp);
    QObject::connect(st.flushTimer, &QTimer::timeout, qApp, [] { flush(); });
    st.flushTimer->start(kFlushIntervalMs);

    st.listener = ops::addExecuteListener([](const ExecuteEvent& e) {
        State& s = state();
        if (!s.running || !e.done)
            return;
        countCommand(s.session, e.name, builtInCommand(e.name), source(e.source));
        if (!e.ok || !e.name.starts_with("unreal"))
            return;
        ++s.session.unrealCalls;
        if (s.session.unrealVersions.empty())
            for (const link::Instance& i : link::discover(false))
                if (!i.engineVersion.empty() &&
                    std::find(s.session.unrealVersions.begin(), s.session.unrealVersions.end(), i.engineVersion) == s.session.unrealVersions.end())
                    s.session.unrealVersions.push_back(i.engineVersion);
    });
    st.prefsListener = PrefStore::get().onChanged([](const std::string& file) {
        if (file == "general")
            followPref();
    });
    followPref();
    if (!st.running)
        st.store->clear();
}

void stop()
{
    State& st = state();
    if (!st.store)
        return;
    endSession(enabled());
    ops::removeExecuteListener(st.listener);
    PrefStore::get().removeOnChanged(st.prefsListener);
    delete st.minute;
    delete st.flushTimer;
    if (st.activity)
    {
        qApp->removeEventFilter(st.activity);
        delete st.activity;
    }
    st.store.reset();
}

void showPending(QWidget* parent)
{
    State& st = state();
    json shown = json::object();
    shown["queued"] = st.store ? st.store->queue() : json::array();
    if (st.running)
    {
        Session preview = st.session;
        preview.updatedMs = io::recovery::nowMs();
        shown["this session, when Lila closes"] = sessionEndEvent(preview, installId(), false);
    }
    else
    {
        shown["note"] = enabled() ? "Starting." : "Usage statistics are off: nothing is collected or sent.";
    }

    auto* dlg = new QDialog(parent);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle("What Lila Sends");
    dlg->resize(640, 560);
    auto* layout = new QVBoxLayout(dlg);
    auto* text = new QPlainTextEdit(dlg);
    text->setReadOnly(true);
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    text->setPlainText(QString::fromStdString(shown.dump(2)));
    layout->addWidget(text);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::close);
    layout->addWidget(buttons);
    dlg->show();
}
} // namespace lila::telemetry
