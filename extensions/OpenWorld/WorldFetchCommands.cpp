// worldFetch and the fetch half of worldBuild: a world spec's missing real data sized, allowed and downloaded over the
// network (Terrain's HTTP and WorldCover readers), into the paths the spec names.

#include "Http.h"
#include "WorldFetch.h"
#include "WorldScene.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Log.h>
#include <lila/Paths.h>
#include <lila/Prefs.h>
#include <lila/Profile.h>
#include <lila/SceneFile.h>
#include <lila/Tasks.h>
#include <lila/Workspace.h>

#include <QNetworkInformation>

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <optional>
#include <thread>

using namespace lila;

namespace openworld::world
{
namespace fs = std::filesystem;
using T = ArgType;

namespace
{
/** Terrain keeps the OpenTopography key here; the same key serves terrainImportDem, terrainBathymetry and worldFetch. */
constexpr const char* kPrefsFile = "terrain";
constexpr const char* kKeyPref = "/openTopography/apiKey";
/** Python shows this text instead of the result's repr at the prompt (lila.ops); empty when the table is already in Script Output. */
constexpr const char* kEchoKey = "_echo";

/** The download cache Terrain's fetches use: the workspace's terrain/dem_cache, else beside the scene, else temp. */
fs::path downloadCache()
{
    if (const fs::path& root = Workspace::get().root(); !root.empty())
        return root / "terrain" / "dem_cache";
    const fs::path& sceneFile = io::currentPath();
    if (!sceneFile.empty())
        return sceneFile.parent_path() / (sceneFile.stem().string() + "_terrain") / "dem_cache";
    return fs::temp_directory_path() / "lila_terrain" / "dem_cache";
}

/** True on a metered connection; nullopt when Qt has no network information backend here. */
std::optional<bool> meteredConnection()
{
    if (!QNetworkInformation::instance() && !QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Metered))
        return std::nullopt;
    QNetworkInformation* n = QNetworkInformation::instance();
    if (!n || !n->supports(QNetworkInformation::Feature::Metered))
        return std::nullopt;
    return n->isMetered();
}

/** The real network. Whether the connection is metered is read here, on the main thread, as Qt requires. */
FetchNet networkNet()
{
    FetchNet net;
    net.get = [](const std::string& url, const fs::path& dest, std::string* why) {
        const terrain::HttpProgressHook hook(reportFetchBytes);
        return terrain::httpGet(url, dest, why);
    };
    net.landcover = [](const terrain::GeoBox& b) {
        // WorldCover arrives as many ranged reads; their bytes add up to the item's.
        uint64_t done = 0, last = 0;
        const terrain::HttpProgressHook hook([&](uint64_t received, uint64_t) {
            if (received < last)
                done += last;
            last = received;
            return reportFetchBytes(done + received, 0);
        });
        return terrain::fetchWorldCover(b, [](const std::string& url, uint64_t at, uint64_t n, std::vector<uint8_t>* out, std::string* why) {
            return terrain::httpGetRange(url, at, n, out, why);
        });
    };
    const std::optional<bool> metered = meteredConnection();
    net.metered = [metered] { return metered; };
    net.wait = [](int ms) {
        for (int waited = 0; waited < ms && !progress::cancelRequested(); waited += 100)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    };
    return net;
}

/** Each table line as its own Script Output line under a heading, so the columns line up. */
void logTable(const std::string& heading, const std::vector<std::string>& lines)
{
    log::info("{}", heading);
    for (const std::string& l : lines)
        log::info("  {}", l);
}

/** Progress of a fetch as it runs: the progress card's bar and status, throttled Script Output lines. */
class FetchReporter
{
public:
    FetchReporter(const char* cmd, progress::Scope& scope) : mCmd(cmd), mScope(scope) {}

    void operator()(const FetchEvent& e)
    {
        const std::string label = std::format("[{}/{}] {}", e.index, e.count, e.id);
        const auto now = std::chrono::steady_clock::now();
        switch (e.kind)
        {
        case FetchEvent::Kind::Start:
            mLastLog = mRateFrom = now;
            mScope.set(double(e.index - 1));
            say(e.cached ? label + " from the download cache" : label + " downloading, about " + fetchSize(e.expected), true);
            break;
        case FetchEvent::Kind::Bytes:
        {
            if (e.expected > 0)
                mScope.set(double(e.index - 1) + std::min(0.99, double(e.bytes) / double(e.expected)));
            const bool logNow = now - mLastLog >= std::chrono::seconds(3);
            if (!logNow && now - mLastStatus < std::chrono::milliseconds(100))
                break;
            mLastStatus = now;
            std::string line = label + "  " + fetchSize(e.bytes);
            if (e.expected > 0 && e.bytes <= e.expected)
                line += std::format(" of {}{} ({}%)", e.exact ? "" : "about ", fetchSize(e.expected), int(100.0 * double(e.bytes) / double(e.expected)));
            if (const double s = std::chrono::duration<double>(now - mRateFrom).count(); s > 0.5)
                line += ", " + fetchSize(uint64_t(double(e.bytes) / s)) + "/s";
            if (logNow)
                mLastLog = now;
            say(line, logNow);
            break;
        }
        case FetchEvent::Kind::Retry:
            // The rate counts from the attempt that is sending, not from the failed one or the pause.
            mRateFrom = now + std::chrono::milliseconds(e.waitMs);
            mScope.setStatus(std::format("{}: {}; retrying in {} s", label, e.why, e.waitMs / 1000));
            log::warn("{}: {} attempt {} failed ({}); retrying in {} s", mCmd, label, e.attempt, e.why, e.waitMs / 1000);
            break;
        case FetchEvent::Kind::Done:
            mScope.set(double(e.index));
            say(std::format("{} {}, {} in {:.1f} s", label, e.cached ? "copied from the download cache" : "fetched", fetchSize(e.bytes), e.seconds), true);
            mOutcome.push_back({{"id", e.id}, {"state", "fetched"}, {"bytes", e.bytes}, {"cached", e.cached}});
            break;
        case FetchEvent::Kind::Failed:
            mScope.setStatus(label + " failed");
            mOutcome.push_back({{"id", e.id}, {"state", "failed"}});
            break;
        }
    }

    /** The items so far as runFetch reports them, for the table after a failure. */
    const json& outcome() const { return mOutcome; }

private:
    void say(const std::string& line, bool toLog)
    {
        mScope.setStatus(line);
        if (toLog)
            log::info("{}: {}", mCmd, line);
    }

    std::string mCmd;
    progress::Scope& mScope;
    json mOutcome = json::array();
    std::chrono::steady_clock::time_point mLastLog{}, mLastStatus{}, mRateFrom{};
};

fs::path resolveRelative(const std::string& s) { return paths::resolvePath(s); }
} // namespace

json loadSpecFile(const std::string& file, const char* cmd)
{
    const fs::path p = paths::resolvePath(file);
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw CommandError(std::string(cmd) + ": can't read " + paths::toUtf8(p));
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    json j = json::parse(text, nullptr, false, true);
    if (j.is_discarded())
        throw CommandError(std::string(cmd) + ": " + paths::toUtf8(p) + " is not JSON");
    return resolvePaths(std::move(j), paths::toUtf8(p.parent_path()), [](const std::string& rel) {
        std::error_code ec;
        return fs::exists(paths::resolvePath(rel), ec);
    });
}

json fetchWorldData(const json& spec, const FetchFlags& flags, const char* cmd)
{
    LILA_ZONE();
    FetchOptions o;
    o.cache = downloadCache();
    o.offline = flags.offline;
    o.consent = flags.consent;
    try
    {
        o.apiKey = terrain::chooseApiKey(flags.apiKey, PrefStore::get().value<std::string>(kPrefsFile, kKeyPref, std::string()), &o.keyNote);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
    if (!flags.apiKey.empty() && flags.saveKey)
        PrefStore::get().set(kPrefsFile, kKeyPref, o.apiKey);
    const FetchPlan plan = planFetch(spec, o.cache, resolveRelative);
    json est = planJson(plan, o);
    if (!o.keyNote.empty())
        est["keyNote"] = o.keyNote;
    if (flags.estimate)
    {
        logTable(std::format("{}: real data, {}", cmd, plan.missing ? std::to_string(plan.missing) + " missing" : std::string("all present")), fetchTable(plan, o));
        return {{"estimate", est}, {kEchoKey, ""}};
    }
    if (plan.missing == 0)
    {
        log::info("{}: real data all present", cmd);
        json report = runFetch(plan, o, FetchNet{});
        report["estimate"] = est;
        return report;
    }
    logTable(std::format("{}: real data, {} missing", cmd, plan.missing), fetchTable(plan, o));

    const FetchNet net = networkNet();
    progress::Scope scope("Downloading world data", double(plan.missing), progress::Cancellable);
    FetchReporter reporter(cmd, scope);
    o.onEvent = [&reporter](const FetchEvent& e) { reporter(e); };
    o.cancelled = [] { return progress::cancelRequested(); };
    json report;
    try
    {
        // Off the main thread, which keeps painting the progress card (with Abort) and Script Output meanwhile.
        tasks::runAndWait([&] { report = runFetch(plan, o, net); });
    }
    catch (const std::exception& e)
    {
        // Refusals (consent, key, offline) come before any item starts and the plan table above already says it all.
        if (!reporter.outcome().empty())
            logTable(std::format("{}: stopped", cmd), fetchTable(plan, o, &reporter.outcome()));
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
    logTable(std::format("{}: fetched {} of {} missing, {}", cmd, report.value("fetched", 0), plan.missing, fetchSize(report.value("bytes", uint64_t(0)))),
             fetchTable(plan, o, &report["items"]));
    report["estimate"] = est;
    return report;
}

std::string missingData(const json& spec)
{
    FetchOptions o;
    o.cache = downloadCache();
    const FetchPlan plan = planFetch(spec, o.cache, resolveRelative);
    if (plan.missing == 0)
        return {};
    std::string files;
    for (const FetchItem& it : plan.items)
        if (!it.present)
            files += (files.empty() ? "" : ", ") + it.id + " (" + paths::toUtf8(it.file) + ")";
    return std::format("missing data: {}; fetch=True downloads them ({}), estimate=True only reports the sizes", files, planSummary(plan));
}

void registerFetchCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "worldFetch",
        .doc = "Download a world spec's missing real data straight to the paths it names (relative to the spec file): every remix "
               "source's elevation (Copernicus), OSM (Overpass), land cover (WorldCover) and bathymetry (GEBCO through "
               "OpenTopography, with your API key), and the coast's bathymetry grid (its bbox, the bathymetry source's, or all "
               "sources' together). Files already there are left alone. estimate=True only lists each file with its size; a "
               "total over 25 MB, or any download on a metered connection, needs consent=True. Downloads retry and are written "
               "whole, so a run stopped by a failure resumes where it stopped; tiles and areas are kept in the workspace's "
               "terrain/dem_cache (offline=True uses only that). Progress shows per item (bytes, percent, rate, retries) in "
               "Script Output and on the progress card, which can abort it; a table of the items and their status is logged "
               "before and after. A placeholder apiKey (empty, '<...>', 'your...') is refused, and a saved placeholder counts as "
               "no key. Returns every item fetched or present, bytes and the ATTRIBUTION.txt files written. "
               "worldBuild(fetch=True) runs this first.",
        .flags = {{"spec", "s", T::Any, nullptr, "The world spec, a dict."},
                  {"file", "f", T::String, nullptr, "A world spec JSON file; relative paths in it are from its folder."},
                  {"estimate", "es", T::Bool, false, "Only list what is missing and how big the downloads are."},
                  {"consent", "cn", T::Bool, false, "Allow more than 25 MB, or downloads on a metered connection."},
                  {"offline", "off", T::Bool, false, "Use the download cache only."},
                  {"apiKey", "ak", T::String, "", "OpenTopography API key (for GEBCO); default the saved one. Never logged."},
                  {"saveKey", "sk", T::Bool, false, "Save apiKey for later fetches."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            json spec;
            if (a.has("file"))
                spec = loadSpecFile(a.get<std::string>("file"), "worldFetch");
            else if (a.has("spec"))
                spec = a.flags["spec"].is_string() ? json::parse(a.get<std::string>("spec")) : a.flags["spec"];
            else
                throw CommandError("worldFetch: give spec= or file=");
            FetchFlags f;
            f.estimate = a.get<bool>("estimate", false);
            f.consent = a.get<bool>("consent", false);
            f.offline = a.get<bool>("offline", false);
            f.apiKey = a.get<std::string>("apiKey", "");
            f.saveKey = a.get<bool>("saveKey", false);
            json r = fetchWorldData(spec, f, "worldFetch");
            r[kEchoKey] = "";
            return r;
        },
        .examples = {"e = worldFetch(file='worlds/sinaloa_real.json', estimate=True)['estimate']\ne['summary'], e['consentNeeded']",
                     "worldFetch(file='worlds/sinaloa_real.json', consent=True, apiKey='...', saveKey=True)"},
    });
}
} // namespace openworld::world
