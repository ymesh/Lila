#include <doctest/doctest.h>

#include "core/CrashUpload.h"
#include "core/PrefStore.h"
#include "ui/SupportBundle.h"
#include "ui/Telemetry.h"
#include "ui/Updater.h"
#include "ui/WelcomeNews.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

#include <miniz.h>

using namespace lila;
namespace tel = lila::telemetry;
namespace fs = std::filesystem;

namespace
{
/** A per-process root, so concurrent runs of lila_tests (build and build-night) never share a queue file. */
struct TelemetryRoot
{
    fs::path path;
    TelemetryRoot()
    {
        std::random_device rd;
        path = fs::temp_directory_path() / ("lila_test_telemetry_" + std::to_string(rd()));
    }
    ~TelemetryRoot()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

fs::path telemetryDir(const char* name)
{
    static TelemetryRoot root;
    fs::path dir = root.path / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void setOrUnsetEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    if (*value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}
} // namespace

TEST_CASE("telemetry buckets")
{
    CHECK(tel::memoryBucket(4) == "<8");
    CHECK(tel::memoryBucket(16) == "16-31");
    CHECK(tel::memoryBucket(15.9) == "16-31");
    CHECK(tel::memoryBucket(64) == "64-127");
    CHECK(tel::memoryBucket(512) == "128+");
    CHECK(tel::daysBucket(0) == "0");
    CHECK(tel::daysBucket(6) == "1-6");
    CHECK(tel::daysBucket(7) == "7-29");
    CHECK(tel::daysBucket(400) == "90+");
}

TEST_CASE("telemetry install channel")
{
    fs::path root = telemetryDir("channel");
    fs::path steam = root / "SteamLibrary" / "steamapps" / "common" / "Lila";
    fs::create_directories(steam);
    CHECK(tel::installChannel(steam) == "steam");

    fs::path build = root / "src" / "build" / "bin" / "RelWithDebInfo";
    fs::create_directories(build);
    std::ofstream(root / "src" / "build" / "CMakeCache.txt") << "";
    CHECK(tel::installChannel(build) == "source");

    fs::path installed = root / "Programs" / "Lila";
    fs::create_directories(installed);
    CHECK(tel::installChannel(installed) == "installer");
}

TEST_CASE("telemetry environment opt-out")
{
    setOrUnsetEnv("DO_NOT_TRACK", "");
    setOrUnsetEnv("LILA_NO_TELEMETRY", "");
    CHECK_FALSE(tel::optedOutByEnvironment());
    setOrUnsetEnv("DO_NOT_TRACK", "0");
    CHECK_FALSE(tel::optedOutByEnvironment());
    setOrUnsetEnv("DO_NOT_TRACK", "1");
    CHECK(tel::optedOutByEnvironment());
    CHECK_FALSE(tel::enabled());
    setOrUnsetEnv("DO_NOT_TRACK", "");
    setOrUnsetEnv("LILA_NO_TELEMETRY", "1");
    CHECK(tel::optedOutByEnvironment());
    setOrUnsetEnv("LILA_NO_TELEMETRY", "");
}

TEST_CASE("telemetry queue caps, retries and drops")
{
    tel::Store store(telemetryDir("queue"));
    for (size_t i = 0; i < tel::kMaxQueued + 5; ++i)
        store.enqueue({{"event", "e"}, {"n", i}});
    json q = store.queue();
    REQUIRE(q.size() == tel::kMaxQueued);
    CHECK(q[0]["n"] == 5);

    store.dequeue(tel::kMaxQueued - 2);
    REQUIRE(store.queue().size() == 2);

    for (int i = 0; i < tel::kMaxAttempts - 1; ++i)
        store.failed(1);
    q = store.queue();
    REQUIRE(q.size() == 2);
    CHECK(q[0]["attempts"] == tel::kMaxAttempts - 1);
    CHECK_FALSE(q[1].contains("attempts"));
    store.failed(1);
    CHECK(store.queue().size() == 1);

    store.clear();
    CHECK(store.queue().empty());
}

TEST_CASE("telemetry sessions left by a gone process become crashed session ends")
{
    tel::Store store(telemetryDir("sessions"));
    tel::Session gone;
    gone.id = "a";
    gone.pid = 111;
    gone.startedMs = 1'000'000;
    gone.updatedMs = 1'000'000 + 90'000;
    gone.activeMinutes = 1;
    tel::countCommand(gone, "polyCube", true, "ui");
    tel::countCommand(gone, "studioSecretTool", false, "python");
    store.writeSession(gone);

    tel::Session running = gone;
    running.id = "b";
    running.pid = 222;
    store.writeSession(running);

    auto alive = [](int64_t pid, int64_t) { return pid == 222; };
    std::vector<tel::Session> stale = store.takeStaleSessions(333, alive);
    REQUIRE(stale.size() == 1);
    CHECK(stale[0].id == "a");
    CHECK(store.takeStaleSessions(333, alive).empty());
    CHECK(store.takeStaleSessions(222, [](int64_t, int64_t) { return true; }).size() == 1);

    json e = tel::sessionEndEvent(stale[0], "install", true);
    CHECK(e["event"] == "session_end");
    CHECK(e["distinct_id"] == "install");
    const json& p = e["properties"];
    CHECK(p["crashed"] == true);
    CHECK(p["duration_s"] == 90);
    CHECK(p["commands"]["polyCube"] == 1);
    CHECK_FALSE(p["commands"].contains("studioSecretTool"));
    CHECK(p["other_commands"] == 1);
    CHECK(p["command_sources"]["ui"] == 1);
    CHECK(p["command_sources"]["python"] == 1);
    CHECK(p["$geoip_disable"] == true);
}

TEST_CASE("crash report upload strips identifying details")
{
    const std::string report = "Lila crashed: access violation in lila_scene.dll+0x10, at lila::Foo::bar (X:/Lila/src/scene/Foo.cpp:12)\n"
                               "\n[app]\nversion: 0.1.0\n" R"(exe: C:\Users\Jane Doe\Lila\lila.exe)" "\n" R"(configDir: C:\Users\Jane Doe\AppData\Roaming\Lila)" "\n"
                               "\n[system]\nos: Windows 11\nmachine: JANE-PC\n"
                               "\n[context]\nscene: D:/Projects/Secret/hero.lila\ntool: move\n"
                               "\n[stack]\n  lila_scene.dll!lila::Foo::bar (X:/Lila/src/scene/Foo.cpp:12)\n"
                               R"(  opened D:\Projects\Secret\hero.lila for JANE-PC)" "\n"
                               "\n[log]\n  Saved D:/Projects/Secret/hero.lila\n"
                               "\n[commands]\n  polyCube(name='secretThing')\n";
    std::string out = crash::upload::sanitize(report, {"Jane Doe", "JANE-PC"});
    CHECK(out.find("Jane") == std::string::npos);
    CHECK(out.find("JANE-PC") == std::string::npos);
    CHECK(out.find("Secret") == std::string::npos);
    CHECK(out.find("hero") == std::string::npos);
    CHECK(out.find("secretThing") == std::string::npos);
    CHECK(out.find("Foo.cpp:12") != std::string::npos);
    CHECK(out.find("polyCube") != std::string::npos);
    CHECK(out.find("version: 0.1.0") != std::string::npos);
    CHECK(out.find("tool: move") != std::string::npos);
}

TEST_CASE("crash consent round trip and always-send writes the prefs")
{
    fs::path dir = telemetryDir("consent");
    fs::path consent = dir / "crash-consent.json";
    fs::path prefs = dir / "general.json";
    std::ofstream(prefs) << R"({"telemetry": {"usage": true}})";
    crash::upload::writeConsent(consent, {.send = true, .always = false, .prefsFile = prefs});
    crash::upload::Consent c = crash::upload::readConsent(consent);
    CHECK(c.send);
    CHECK_FALSE(c.always);
    CHECK(c.prefsFile == prefs);

    crash::upload::setAlways(consent, true);
    CHECK(crash::upload::readConsent(consent).always);
    std::ifstream in(prefs);
    json j = json::parse(in);
    CHECK(j["telemetry"]["crashAlways"] == true);
    CHECK(j["telemetry"]["usage"] == true);
    CHECK_FALSE(crash::upload::readConsent(dir / "missing.json").send);
}

TEST_CASE("support bundle zips the session and redacts secrets from prefs")
{
    PrefStore::get().start();
    PrefStore::get().set("test_support", "/apiToken", "super-secret-value");
    PrefStore::get().flush();
    fs::path zip = telemetryDir("support") / "bundle.zip";
    std::string error;
    REQUIRE(ui::support::writeBundle(zip, false, &error));

    mz_zip_archive ar{};
    REQUIRE(mz_zip_reader_init_file(&ar, zip.string().c_str(), 0));
    CHECK(mz_zip_reader_locate_file(&ar, "session.txt", nullptr, 0) >= 0);
    CHECK(mz_zip_reader_locate_file(&ar, "README.txt", nullptr, 0) >= 0);
    int prefs = mz_zip_reader_locate_file(&ar, "prefs/test_support.json", nullptr, 0);
    REQUIRE(prefs >= 0);
    size_t size = 0;
    char* data = static_cast<char*>(mz_zip_reader_extract_to_heap(&ar, mz_uint(prefs), &size, 0));
    std::string text(data, size);
    mz_free(data);
    mz_zip_reader_end(&ar);
    CHECK(text.find("super-secret-value") == std::string::npos);
    CHECK(text.find("<redacted>") != std::string::npos);
    PrefStore::get().flush();
    fs::remove(PrefStore::get().userDir() / "test_support.json");
}

TEST_CASE("updater compares versions and reads the manifest")
{
    namespace up = ui::updater;
    CHECK(up::compareVersions("0.10.0", "0.9.2") == 1);
    CHECK(up::compareVersions("0.1.0", "0.1") == 0);
    CHECK(up::compareVersions("1.0.0", "1.0.1") == -1);
    CHECK(up::compareVersions("2", "1.99.99") == 1);

    up::Release r = up::parseManifest(json::parse(R"({"version": "0.2.0", "page": "https://example.com/v0.2.0", "sha256": 5})"));
    CHECK(r.version == "0.2.0");
    CHECK(r.page == "https://example.com/v0.2.0");
    CHECK(r.installer.empty());
    CHECK(r.sha256.empty());
    CHECK(up::parseManifest(json::array()).version.empty());
}

TEST_CASE("welcome changelog card reads the newest release section")
{
    const auto r = ui::news::latestRelease("# Changelog\r\n\r\nIntro.\r\n\r\n## 0.2.0 - 2026-12-01\r\n\r\n"
                                           "- **Texture painting** in the [Texture Set](texturing.md#sets) dock\r\n"
                                           "- `polyBridge` keeps snake_case names\r\n\r\n## 0.1.0 - 2026-11-20\r\n- First\r\n");
    CHECK(r.version == "0.2.0");
    CHECK(r.date == "2026-12-01");
    REQUIRE(r.changes.size() == 2);
    CHECK(r.changes[0] == "Texture painting in the Texture Set dock");
    CHECK(r.changes[1] == "polyBridge keeps snake_case names");
    CHECK(ui::news::latestRelease("# Changelog\n").version.empty());
}

TEST_CASE("welcome news opens on the newest unseen item and rotates the showcase")
{
    const json feed = json::parse(R"({"items": [
        {"id": "update", "date": "2026-11-01", "title": "Huge update"},
        {"id": "art", "date": "2026-10-31", "title": "A ship", "type": "spotlight", "author": "Ana", "until": "2026-11-30"},
        {"id": "older", "date": "2026-10-20", "title": "Older news"},
        {"id": "oldart", "date": "2026-09-01", "title": "A tree", "type": "spotlight", "until": "2026-10-01"},
        {"date": "2026-10-10", "title": "No id"}
    ]})");
    const auto items = ui::news::parse(feed, "", "0.2.0");
    std::vector<ui::news::Item> news, showcase;
    for (const auto& item : items)
        (ui::news::isShowcase(item) ? showcase : news).push_back(item);
    REQUIRE(news.size() == 3);
    REQUIRE(showcase.size() == 2);
    CHECK(news[2].id == "2026-10-10 No id");

    CHECK(ui::news::firstUnseen(news, {}) == 0);
    CHECK(ui::news::firstUnseen(news, {"update"}) == 1);
    CHECK(ui::news::firstUnseen(news, {"update", "older", "2026-10-10 No id"}) == 0);

    CHECK(ui::news::running(showcase[0], "2026-11-30"));
    CHECK_FALSE(ui::news::running(showcase[0], "2026-12-01"));
    CHECK(ui::news::running(news[0], "2030-01-01"));

    CHECK(ui::news::pickShowcase({}, json::object()) == -1);
    CHECK(ui::news::pickShowcase(showcase, json::object()) == 0);
    CHECK(ui::news::pickShowcase(showcase, {{"art", 2}, {"oldart", 1}}) == 1);
    CHECK(ui::news::pickShowcase(showcase, {{"art", 1}, {"oldart", 1}}) == 0);
}

TEST_CASE("welcome news keeps what this version should show, newest first")
{
    const json feed = json::parse(R"({"items": [
        {"id": "old", "date": "2026-10-01", "title": "Old", "image": "assets/news/old.jpg"},
        {"id": "new", "date": "2026-10-30", "title": "New", "type": "spotlight", "author": "Someone", "url": "https://example.com"},
        {"id": "future", "date": "2026-11-01", "title": "Needs 0.3", "minVersion": "0.3.0"},
        {"id": "past", "date": "2026-11-02", "title": "Before 0.2", "maxVersion": "0.1.9"},
        {"id": "untitled", "date": "2026-11-03"}
    ]})");
    auto items = ui::news::parse(feed, "https://lila-3d.github.io/news.json", "0.2.0");
    REQUIRE(items.size() == 2);
    CHECK(items[0].id == "new");
    CHECK(items[0].type == "spotlight");
    CHECK(items[0].author == "Someone");
    CHECK(items[1].id == "old");
    CHECK(items[1].type == "news");
    CHECK(items[1].image == "https://lila-3d.github.io/assets/news/old.jpg");
    // The limit counts news and showcase entries separately.
    CHECK(ui::news::parse(feed, "https://lila-3d.github.io/news.json", "0.2.0", 1).size() == 2);
    CHECK(ui::news::parse(json::array(), "", "0.2.0").empty());
    CHECK(ui::news::parse(json::parse(R"({"items": 5})"), "", "0.2.0").empty());
}
