#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Paths.h"
#include "io/FileTranslators.h"
#include "io/Recovery.h"
#include "io/SceneFile.h"
#include "io/SceneHistory.h"
#include "scene/Graph.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace lila;
using json = nlohmann::json;
using namespace testutil;
namespace fs = std::filesystem;
namespace history = io::history;
namespace recovery = io::recovery;

namespace
{
fs::path historyDir(const char* name)
{
    fs::path dir = fs::temp_directory_path() / "lila_tests" / "history" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void touchBinary(const fs::path& p, const std::string& text = "x")
{
    std::ofstream(p, std::ios::binary) << text;
}

void ageFile(const fs::path& p, std::chrono::seconds by)
{
    fs::last_write_time(p, fs::last_write_time(p) - by);
}
} // namespace

TEST_CASE("increment naming keeps padding and skips taken names")
{
    fs::path dir = historyDir("increment");
    CHECK(history::nextIncrement(dir / "scene.lila").filename() == "scene_v002.lila");
    CHECK(history::nextIncrement(dir / "scene_v002.lila").filename() == "scene_v003.lila");
    CHECK(history::nextIncrement(dir / "scene_v9.lilaa").filename() == "scene_v10.lilaa");
    CHECK(history::nextIncrement(dir / "scene_v0099.lila").filename() == "scene_v0100.lila");
    CHECK(history::nextIncrement(dir / "shot_v999.lila").filename() == "shot_v1000.lila");

    touchBinary(dir / "scene_v003.lila");
    touchBinary(dir / "scene_v004.lilaa");
    CHECK(history::nextIncrement(dir / "scene_v002.lila").filename() == "scene_v005.lila");
    CHECK(history::nextIncrement(dir / "scene.lila").filename() == "scene_v002.lila");

    auto v = history::parseVersionedStem("my_scene_v012");
    CHECK(v.base == "my_scene");
    CHECK(v.number == 12);
    CHECK(v.width == 3);
    CHECK(history::parseVersionedStem("_v2").width == 0);
    CHECK(history::parseVersionedStem("scene_vx").base == "scene_vx");

    touchBinary(dir / "scene.lila");
    touchBinary(dir / "scene_v003.lila");
    touchBinary(dir / "other_v002.lila");
    auto inc = history::increments(dir / "scene_v003.lila");
    REQUIRE(inc.size() == 2);
    CHECK(inc[0].filename() == "scene.lila");
    CHECK(inc[1].filename() == "scene_v004.lilaa");
}

TEST_CASE("history names parse and sort, and pruning removes the oldest")
{
    fs::path dir = historyDir("rotation");
    auto p = history::parseName("scene.20261022-140312.autosave.lila", "scene");
    REQUIRE(p);
    CHECK(p->stamp == "20261022-140312");
    CHECK(p->tag == "autosave");
    CHECK(p->counter == 0);
    p = history::parseName("scene.20261022-140312-3.emergency.lila", "scene");
    REQUIRE(p);
    CHECK(p->counter == 3);
    CHECK(p->tag == "emergency");
    p = history::parseName("my.scene.20261022-140312.lilaa", "my.scene");
    REQUIRE(p);
    CHECK(p->tag.empty());
    CHECK_FALSE(history::parseName("scene.20261022-140312.autosave.lila", "scen"));
    CHECK_FALSE(history::parseName("scene.2026-140312.lila", "scene"));
    CHECK_FALSE(history::parseName("scene.20261022-140312.fbx", "scene"));
    CHECK(history::timestamp().size() == 15);

    // Same second twice: the second gets a counter and still sorts after the first.
    fs::path a = history::uniqueName(dir, "scene", "20261022-100000", "autosave");
    touchBinary(a);
    fs::path b = history::uniqueName(dir, "scene", "20261022-100000", "autosave");
    CHECK(b.filename() == "scene.20261022-100000-2.autosave.lila");
    touchBinary(b);
    for (const char* stamp : {"20261021-090000", "20261022-110000", "20261020-235959"})
        touchBinary(history::uniqueName(dir, "scene", stamp, "autosave"));
    touchBinary(history::uniqueName(dir, "scene", "20261019-000000", "emergency"));
    touchBinary(dir / "scene_other.20261019-000000.autosave.lila");

    auto all = history::entries(dir, "scene", "autosave");
    REQUIRE(all.size() == 5);
    CHECK(all.front().filename() == "scene.20261020-235959.autosave.lila");
    CHECK(all[2] == a);
    CHECK(all[3] == b);
    CHECK(all.back().filename() == "scene.20261022-110000.autosave.lila");

    CHECK(history::prune(dir, "scene", "autosave", 3) == 2);
    all = history::entries(dir, "scene", "autosave");
    REQUIRE(all.size() == 3);
    CHECK(all.front() == a);
    CHECK(history::entries(dir, "scene", "emergency").size() == 1);
    CHECK(fs::exists(dir / "scene_other.20261019-000000.autosave.lila"));
    CHECK(history::prune(dir, "scene", "autosave", 0) == 0);
}

TEST_CASE("backups copy the previous version, keep N, and leave read-only originals in place")
{
    fs::path dir = historyDir("backup");
    fs::path ws = dir / "ws";
    fs::create_directories(ws / "chars");
    fs::path scene = ws / "chars" / "hero.lila";
    fs::path base = ws / ".lila" / "backups";

    fs::path folder = history::folderFor(base, scene, ws);
    CHECK(folder == base / "chars" / "hero");
    CHECK(history::folderFor(base, ws / "top.lila", ws) == base / "top");
    fs::path outside = history::folderFor(base, dir / "elsewhere" / "hero.lila", ws);
    CHECK(outside.parent_path() == base);
    CHECK(outside.filename().string().rfind("hero.", 0) == 0);
    CHECK(outside != history::folderFor(base, dir / "other" / "hero.lila", ws));
    CHECK(history::folderFor(base, {}, ws) == base / "untitled");

    CHECK(history::backup(scene, folder, 3).empty());
    touchBinary(scene, "v1");
    // Perforce leaves files read-only until checkout; the backup must still work and the original must stay.
    fs::permissions(scene, fs::perms::owner_write, fs::perm_options::remove);
    fs::path first = history::backup(scene, folder, 3);
    REQUIRE(!first.empty());
    CHECK(fs::exists(scene));
    CHECK(slurp(first) == "v1");
    auto parsed = history::parseName(first.filename().string(), "hero");
    REQUIRE(parsed);
    CHECK(parsed->tag.empty());
    CHECK(first.extension() == ".lila");
    fs::permissions(scene, fs::perms::owner_write, fs::perm_options::add);

    for (int i = 2; i <= 5; ++i)
    {
        touchBinary(scene, "v" + std::to_string(i));
        history::backup(scene, folder, 3);
    }
    auto kept = history::entries(folder, "hero", "");
    REQUIRE(kept.size() == 3);
    CHECK(slurp(kept.front()) == "v3");
    CHECK(slurp(kept.back()) == "v5");

    fs::path text = ws / "chars" / "hero.lilaa";
    touchBinary(text, "ascii");
    CHECK(history::backup(text, folder, 3).extension() == ".lilaa");
}

TEST_CASE("saving backs up the previous file; autosave never touches the scene file")
{
    fs::path dir = historyDir("save");
    fs::path scene = dir / "shot.lila";
    std::error_code ec;
    fs::remove_all(history::backupFolder(scene), ec);
    fs::remove_all(history::autosaveFolder(scene), ec);

    run("newScene");
    run("spaceLocator", {{"name", "A"}});
    run("saveScene", {{"args", {scene.generic_string()}}});
    CHECK(history::entries(history::backupFolder(scene), "shot", "").empty());
    std::string saved = slurp(scene);
    auto savedTime = fs::last_write_time(scene);

    run("spaceLocator", {{"name", "B"}});
    CHECK(io::modified());
    io::autosave();
    fs::path folder = history::autosaveFolder(scene);
    for (int i = 0; i < 100 && history::entries(folder, "shot", "autosave").empty(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(history::entries(folder, "shot", "autosave").size() == 1);
    CHECK(io::modified());
    CHECK(io::currentPath() == scene);
    CHECK(slurp(scene) == saved);
    CHECK(fs::last_write_time(scene) == savedTime);
    CHECK(history::entries(history::backupFolder(scene), "shot", "").empty());

    // Nothing changed since the last autosave: no new file.
    io::autosave();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(history::entries(folder, "shot", "autosave").size() == 1);

    // An emergency save goes beside the autosaves, leaving the scene file and the modified flag alone.
    fs::path emergency = io::emergencySave();
    REQUIRE(!emergency.empty());
    CHECK(history::parseName(emergency.filename().string(), "shot")->tag == "emergency");
    CHECK(io::modified());
    CHECK(slurp(scene) == saved);

    run("saveScene");
    auto backups = history::entries(history::backupFolder(scene), "shot", "");
    REQUIRE(backups.size() == 1);
    CHECK(slurp(backups[0]) == saved);
    CHECK(!io::modified());

    // Recovered as the real scene: modified, so Save writes the original path.
    io::loadVersion(emergency, scene, true);
    CHECK(io::currentPath() == scene);
    CHECK(io::modified());
    CHECK(run("objExists", {{"args", {"B"}}}).get<bool>());
    io::loadVersion(backups[0], scene, false, "shot.lila (backup)");
    CHECK(io::currentPath().empty());
    CHECK(io::displayName() == "shot.lila (backup)");
    CHECK_FALSE(run("objExists", {{"args", {"B"}}}).get<bool>());

    run("openScene", {{"args", {scene.generic_string()}}});
    fs::path next = fs::path(run("incrementAndSave").get<std::string>());
    CHECK(next.filename() == "shot_v002.lila");
    CHECK(io::currentPath() == next);
    CHECK(fs::exists(scene));
    auto versions = run("sceneVersions");
    CHECK(versions.size() >= 1);
}

TEST_CASE("unclean exit detection")
{
    fs::path dir = historyDir("sessions");
    fs::path scenes = historyDir("sessions_scenes");
    fs::path scene = scenes / "level.lila";
    touchBinary(scene);
    int64_t started = recovery::nowMs() - 60000;
    auto writeSession = [&](int64_t pid, const fs::path& scenePath, const fs::path& folder) {
        json s = {{"pid", pid}, {"started", started}, {"scenes", {{{"path", scenePath.generic_string()}, {"folders", {folder.generic_string()}}}}}};
        std::ofstream(dir / (std::to_string(pid) + ".json")) << s.dump();
    };
    fs::path folder = scenes / "auto";
    fs::create_directories(folder);
    writeSession(111, scene, folder);
    writeSession(222, scene, folder);
    std::ofstream(dir / "333.json") << "{ truncated";

    auto alive = [](int64_t pid, int64_t) { return pid == 222; };
    auto stale = recovery::staleSessions(dir, 999, alive);
    REQUIRE(stale.size() == 2);
    bool sawCrashed = false;
    for (auto& s : stale)
        if (s.pid == 111)
        {
            sawCrashed = true;
            REQUIRE(s.scenes.size() == 1);
            CHECK(s.scenes[0].scene == scene);
            CHECK(s.scenes[0].folders.at(0) == folder);
        }
    CHECK(sawCrashed);
    // Our own pid in a file means a dead process that had it before us.
    CHECK(recovery::staleSessions(dir, 222, alive).size() == 3);
    CHECK(recovery::processAlive(recovery::currentPid(), recovery::nowMs()));
    // A day back, so a slow run never reaches it from this process's own start.
    CHECK_FALSE(recovery::processAlive(recovery::currentPid(), recovery::nowMs() - 24LL * 3600 * 1000));

    std::vector<recovery::Session> crashed;
    for (auto& s : stale)
        if (s.pid == 111)
            crashed.push_back(s);

    // An autosave older than the saved scene is not offered.
    fs::path old = history::uniqueName(folder, "level", "20261022-100000", "autosave");
    touchBinary(old);
    ageFile(old, std::chrono::seconds(30));
    CHECK(recovery::findRecoverable(crashed).empty());

    // Newer than the saved file: offered, newest first across autosaves and emergency saves.
    ageFile(scene, std::chrono::seconds(50));
    auto found = recovery::findRecoverable(crashed);
    REQUIRE(found.size() == 1);
    CHECK(found[0].file == old);
    CHECK(found[0].kind == history::Kind::Autosave);
    fs::path em = history::uniqueName(folder, "level", "20261022-100001", "emergency");
    touchBinary(em);
    found = recovery::findRecoverable(crashed);
    REQUIRE(found.size() == 1);
    CHECK(found[0].file == em);
    CHECK(found[0].kind == history::Kind::Emergency);
    CHECK(found[0].savedTime > 0);

    // Autosaves from before the session started belong to an earlier one.
    ageFile(scene, std::chrono::hours(2));
    ageFile(old, std::chrono::hours(1));
    ageFile(em, std::chrono::hours(1));
    CHECK(recovery::findRecoverable(crashed).empty());

    // A scene whose file was never written still recovers.
    recovery::Session untitled;
    untitled.startedMs = started;
    fs::path uf = scenes / "untitled_auto";
    fs::create_directories(uf);
    untitled.scenes.push_back({{}, {uf}});
    touchBinary(history::uniqueName(uf, "untitled", history::timestamp(), "autosave"));
    found = recovery::findRecoverable({untitled});
    REQUIRE(found.size() == 1);
    CHECK(found[0].scene.empty());
    CHECK(found[0].savedTime == 0);

    recovery::forget(crashed);
    CHECK_FALSE(fs::exists(dir / "111.json"));
    CHECK(fs::exists(dir / "222.json"));

    // Our own session file comes and goes.
    fs::path mine = historyDir("sessions_mine");
    recovery::beginSession(mine);
    fs::path own = mine / (std::to_string(recovery::currentPid()) + ".json");
    CHECK(fs::exists(own));
    recovery::noteScene(scene, folder);
    CHECK(slurp(own).find("level.lila") != std::string::npos);
    recovery::endSession();
    CHECK_FALSE(fs::exists(own));
}

TEST_CASE("undoing back to the opened state clears the modified flag; selection never sets it")
{
    fs::path scene = historyDir("dirty") / "shot.lila";
    run("newScene");
    run("spaceLocator", {{"name", "A"}});
    run("saveScene", {{"args", {scene.generic_string()}}});
    run("openScene", {{"args", {scene.generic_string()}}, {"force", true}});
    CHECK_FALSE(io::modified());

    run("select", {{"args", {"A"}}});
    CHECK_FALSE(io::modified());

    run("move", {{"args", {"A"}}, {"by", {1, 2, 3}}});
    run("spaceLocator", {{"name", "B"}});
    CHECK(io::modified());
    run("undo");
    CHECK(io::modified());
    run("undo");
    CHECK_FALSE(io::modified());
    run("redo");
    CHECK(io::modified());
    run("undo");
    run("select", {{"args", json::array()}});
    CHECK_FALSE(io::modified());
}

TEST_CASE("flushUndo stops undo at that point and keeps a saved scene unmodified")
{
    fs::path scene = historyDir("flush") / "shot.lila";
    run("newScene");
    run("spaceLocator", {{"name", "A"}});
    run("saveScene", {{"args", {scene.generic_string()}}});
    run("openScene", {{"args", {scene.generic_string()}}, {"force", true}});
    run("flushUndo");
    CHECK_FALSE(io::modified());
    run("spaceLocator", {{"name", "B"}});
    run("flushUndo");
    CHECK(io::modified());
    CHECK_THROWS(run("undo"));
}

namespace
{
/** Holds two shared buffers, like a setAttr holding the mesh before and after. */
struct BlobAction final : UndoAction
{
    std::shared_ptr<int> before, after;
    BlobAction(std::shared_ptr<int> b, std::shared_ptr<int> a) : before(std::move(b)), after(std::move(a)) {}
    void undo() override {}
    void redo() override {}
    size_t bytes() const override { return 0; }
    void sharedBytes(UndoShared& s) const override
    {
        s.add(before.get(), 1000);
        s.add(after.get(), 1000);
    }
};
} // namespace

TEST_CASE("undo memory budget counts shared buffers once and evicts the oldest steps, keeping the base state")
{
    UndoStack& u = UndoStack::get();
    u.clear();
    std::vector<std::shared_ptr<int>> blobs;
    for (int i = 0; i < 5; ++i)
        blobs.push_back(std::make_shared<int>(i));
    std::vector<uint64_t> ids;
    for (int i = 0; i < 4; ++i)
    {
        u.record(std::make_unique<BlobAction>(blobs[i], blobs[i + 1]));
        ids.push_back(u.stateId());
    }
    CHECK(u.undoCount() == 4);
    CHECK(u.memoryBytes() == 5000);

    u.setByteBudget(3500);
    CHECK(u.undoCount() == 2);
    CHECK(u.memoryBytes() == 3000);
    CHECK(u.stateId() == ids[3]);
    u.undo();
    u.undo();
    CHECK_FALSE(u.canUndo());
    CHECK(u.stateId() == ids[1]);
    CHECK(u.memoryBytes() == 3000);

    // A new step drops the redo steps and their buffers.
    u.record(std::make_unique<BlobAction>(blobs[2], blobs[0]));
    CHECK(u.redoCount() == 0);
    CHECK(u.memoryBytes() == 2000);

    // The newest step always stays, even over budget.
    u.setByteBudget(10);
    CHECK(u.undoCount() == 1);

    u.setByteBudget(0);
    u.clear();
    CHECK(u.memoryBytes() == 0);
}

TEST_CASE("undoJump undoes and redoes to a position with one notification")
{
    run("newScene");
    run("flushUndo");
    for (const char* name : {"A", "B", "C"})
        run("spaceLocator", {{"name", name}});
    UndoStack& u = UndoStack::get();
    json info = run("undoInfo", {{"history", true}});
    REQUIRE(info["steps"].size() == 3);
    CHECK(info["position"] == 3);
    CHECK(info["memory"].get<size_t>() > 0);

    static int notified = 0;
    static bool counting = false;
    u.addListener([] {
        if (counting)
            ++notified;
    });
    counting = true;
    notified = 0;
    CHECK(run("undoJump", {{"position", 1}}) == 1);
    CHECK(notified == 1);
    auto& g = scene::Graph::current();
    CHECK(g.find("A"));
    CHECK_FALSE(g.find("B"));
    CHECK_FALSE(g.find("C"));
    CHECK(u.redoCount() == 2);

    run("undoJump", {{"position", 3}});
    CHECK(notified == 2);
    CHECK(g.find("C"));
    run("undoJump", {{"position", 0}});
    CHECK_FALSE(g.find("A"));
    counting = false;
    CHECK_THROWS(run("undoJump", {{"position", 4}}));
}

TEST_CASE("recovered scenes are kept apart from their autosaves until pruned or cleared")
{
    fs::path dir = historyDir("recovered");
    fs::path source = historyDir("recovered_source");
    fs::path scene = source / "level.lila";
    fs::path older = source / "level.20261001-100000.autosave.lila";
    fs::path newer = source / "untitled.20261020-100000.emergency.lila";
    touchBinary(older, "older");
    touchBinary(newer, "newer");
    const int64_t now = 2000000000;
    std::vector<recovery::Candidate> found = {{scene, older, history::Kind::Autosave, now - 40 * 86400, 0},
                                              {{}, newer, history::Kind::Emergency, now - 86400, 0}};

    REQUIRE(recovery::keep(found, dir).size() == 2);
    CHECK(recovery::keep(found, dir).empty());
    // The autosave folder pruning its originals does not touch the kept copies.
    fs::remove(older);
    fs::remove(newer);
    auto kept = recovery::recovered(dir);
    REQUIRE(kept.size() == 2);
    CHECK(kept[0].scene.empty());
    CHECK(kept[0].kind == history::Kind::Emergency);
    CHECK(slurp(kept[0].file) == "newer");
    CHECK(kept[1].scene == scene);
    CHECK(slurp(kept[1].file) == "older");

    CHECK(recovery::prune(0, dir, now) == 0);
    CHECK(recovery::prune(30, dir, now) == 1);
    kept = recovery::recovered(dir);
    REQUIRE(kept.size() == 1);
    CHECK(kept[0].kind == history::Kind::Emergency);
    CHECK(recovery::clear(dir) == 1);
    CHECK(recovery::recovered(dir).empty());
    CHECK(fs::is_empty(dir));
}

TEST_CASE("opening an import-only format starts a new untitled scene")
{
    namespace tr = io::translators;
    tr::registerBuiltins();
    tr::add({.id = "testImportOnly",
             .label = "Test import",
             .extensions = {".tstimp"},
             .import = {.run = [](const fs::path&, const json&) -> json { return run("spaceLocator", {{"name", "Imported"}}); }}});
    CHECK(tr::forPath(tr::Op::Open, "a.lila")->id == "lila");
    CHECK(tr::forPath(tr::Op::Open, "a.tstimp")->id == "testImportOnly");
    CHECK(tr::forPath(tr::Op::Open, "a.fbx")->id == "fbx");
    std::string filter = tr::dialogFilter(tr::Op::Open, "All supported");
    CHECK(filter.starts_with("All supported ("));
    CHECK(filter.find("*.fbx") != std::string::npos);
    CHECK(filter.find("Test import (*.tstimp)") != std::string::npos);
    CHECK(filter.find("Lila scenes") < filter.find("FBX ("));

    fs::path dir = historyDir("open_import");
    fs::path saved = dir / "before.lila";
    run("newScene");
    run("spaceLocator", {{"name", "Old"}});
    run("saveScene", {{"args", {saved.generic_string()}}});
    fs::path file = dir / "thing.tstimp";
    touchBinary(file);
    run("openScene", {{"args", {file.generic_string()}}});
    scene::Graph& g = scene::Graph::current();
    CHECK(g.find("Imported"));
    CHECK_FALSE(g.find("Old"));
    CHECK(io::currentPath().empty());
    CHECK(io::modified());
    CHECK_FALSE(UndoStack::get().canUndo());
    tr::remove("testImportOnly");
}
