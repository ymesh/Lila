#include <doctest/doctest.h>

#include "TestUtil.h"

#include "app/Host.h"
#include "batch/BatchCommands.h"
#include "batch/BatchJob.h"
#include "batch/BatchRunner.h"
#include "core/Paths.h"
#include "io/ExportOnSave.h"
#include "io/FileTranslators.h"
#include "io/SceneFile.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

using namespace lila;
using namespace lila::batch;
using namespace testutil;

namespace
{
fs::path tempDir(const char* name)
{
    fs::path d = fs::temp_directory_path() / "lila_batch_tests" / name;
    std::error_code ec;
    if (fs::exists(d))
    {
        for (auto& e : fs::recursive_directory_iterator(d, ec))
            fs::permissions(e.path(), fs::perms::owner_write, fs::perm_options::add, ec);
        fs::remove_all(d, ec);
    }
    fs::create_directories(d);
    return d;
}

void touch(const fs::path& p, const std::string& text = "x")
{
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

std::vector<std::string> relativeNames(const std::vector<Item>& items, const fs::path& base)
{
    std::vector<std::string> out;
    for (const Item& i : items)
        out.push_back(paths::toUtf8(paths::relativeTo(i.path, base)));
    return out;
}

/** Workers that act out a behaviour per file name, writing their result like a real one, from threads of their own. */
class FakeLauncher final : public Launcher
{
public:
    std::atomic<int> live{0};
    std::atomic<int> peak{0};
    std::atomic<int> started{0};

    class FakeWorker final : public Worker
    {
    public:
        FakeWorker(FakeLauncher& owner, WorkerTask task) : mOwner(owner)
        {
            const std::string name = task.jobFile.empty() ? std::string() : readName(task);
            mThread = std::thread([this, task, name] {
                const bool hang = name.find("hang") != std::string::npos;
                for (int i = 0; i < (hang ? 2000 : 3) && !mKilled; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (!mKilled && name.find("crash") == std::string::npos)
                {
                    FileResult r;
                    r.status = name.find("bad") != std::string::npos ? Status::Failed : Status::Done;
                    r.message = r.status == Status::Failed ? "boom" : "";
                    r.results = {{"name", name}};
                    std::ofstream(task.resultFile) << toJson(r).dump();
                }
                mCode = mKilled ? 1 : name.find("crash") != std::string::npos ? int(0xC0000005) : 0;
                mOwner.live--;
                mDone = true;
            });
        }
        ~FakeWorker() override { mThread.join(); }
        bool exited(int& code) override
        {
            if (!mDone)
                return false;
            code = mCode;
            return true;
        }
        void kill() override
        {
            mKilled = true;
            while (!mDone)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

    private:
        static std::string readName(const WorkerTask& t)
        {
            json job = json::parse(slurp(t.jobFile));
            return paths::toUtf8(paths::fromUtf8(job["items"][t.index]["path"].get<std::string>()).filename());
        }
        FakeLauncher& mOwner;
        std::thread mThread;
        std::atomic<bool> mKilled{false};
        std::atomic<bool> mDone{false};
        int mCode = 0;
    };

    std::unique_ptr<Worker> start(const WorkerTask& t, std::string*) override
    {
        int now = ++live;
        int p = peak.load();
        while (now > p && !peak.compare_exchange_weak(p, now))
        {
        }
        ++started;
        return std::make_unique<FakeWorker>(*this, t);
    }
};

Spec fakeSpec(const fs::path& dir, std::vector<std::string> files)
{
    Spec s;
    s.script = "pass";
    for (const std::string& f : files)
    {
        touch(dir / f);
        s.inputs.push_back(paths::toUtf8(dir / f));
    }
    return s;
}
} // namespace

TEST_CASE("batch globs")
{
    CHECK(globMatch("*.ma", "hero.ma"));
    CHECK(globMatch("*.MA", "Hero.ma"));
    CHECK_FALSE(globMatch("*.ma", "hero.mb"));
    CHECK_FALSE(globMatch("*.ma", "sub/hero.ma"));
    CHECK(globMatch("**/*.ma", "hero.ma"));
    CHECK(globMatch("**/*.ma", "a/b/hero.ma"));
    CHECK(globMatch("props/**", "props/a/b.lila"));
    CHECK(globMatch("char_??.lila", "char_01.lila"));
    CHECK_FALSE(globMatch("char_??.lila", "char_1.lila"));
    CHECK(globMatch("char_[0-9]*", "char_7x"));
    CHECK_FALSE(globMatch("char_[!0-9]*", "char_7x"));
    CHECK(globMatch("a\\b.lila", "a/b.lila"));
    CHECK(globMatch("[ab", "[ab"));
    CHECK(normalizeExtension("MA") == ".ma");
    CHECK(normalizeExtension("*.lila") == ".lila");
}

TEST_CASE("batch gather walks, filters and de-duplicates")
{
    fs::path d = tempDir("gather");
    for (const char* f : {"a.lila", "b.lilaa", "c.ma", "notes.txt", "skip_me.lila", "hero_old.lila", "sub/d.lila", "sub/deep/e.lila",
                          ".lila/autosave/x.lila"})
        touch(d / f);

    Spec s;
    s.inputs = {paths::toUtf8(d)};
    s.extensions = {"lila", ".lilaa", "MA"};
    CHECK(relativeNames(gather(s), d) == std::vector<std::string>{"a.lila", "b.lilaa", "c.ma", "hero_old.lila", "skip_me.lila"});

    s.depth = 1;
    CHECK(relativeNames(gather(s), d).size() == 6);
    s.depth = -1;
    std::vector<std::string> all = relativeNames(gather(s), d);
    CHECK(all.size() == 7);
    CHECK(std::find(all.begin(), all.end(), ".lila/autosave/x.lila") == all.end());

    s.excludePrefix = "SKIP";
    s.excludeSuffix = "_old";
    s.exclude = {"sub/deep/*"};
    CHECK(relativeNames(gather(s), d) == std::vector<std::string>{"a.lila", "b.lilaa", "c.ma", "sub/d.lila"});
    s.include = {"*.lila"};
    CHECK(relativeNames(gather(s), d) == std::vector<std::string>{"a.lila", "sub/d.lila"});
    s.excludeContains = "a";
    CHECK(relativeNames(gather(s), d) == std::vector<std::string>{"sub/d.lila"});

    Spec g;
    g.inputs = {paths::toUtf8(d) + "/**/*.lila", paths::toUtf8(d / "a.lila"), paths::toUtf8(d / "missing.lila")};
    std::vector<std::string> warnings;
    std::vector<std::string> got = relativeNames(gather(g, &warnings), d);
    CHECK(got == std::vector<std::string>{"a.lila", "hero_old.lila", "skip_me.lila", "sub/d.lila", "sub/deep/e.lila", "missing.lila"});
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("missing.lila") != std::string::npos);

    Spec rel;
    rel.workspace = paths::toUtf8(d);
    rel.inputs = {"sub/*.lila"};
    CHECK(relativeNames(gather(rel), d) == std::vector<std::string>{"sub/d.lila"});
}

TEST_CASE("batch output paths and the plan")
{
    fs::path d = tempDir("plan");
    touch(d / "src/a.lila");
    touch(d / "src/b.ma");
    touch(d / "src/b.lila");
    touch(d / "src/sub/c.lilaa");
    touch(d / "out/sub/c_fixed.lilaa");

    Spec s;
    s.inputs = {paths::toUtf8(d / "src")};
    s.depth = -1;
    s.extensions = {"lila", "lilaa", "ma"};
    std::vector<Item> items = gather(s);
    REQUIRE(items.size() == 4);

    plan(s, items);
    for (const Item& i : items)
        CHECK(i.output.empty());

    s.save = SaveMode::Overwrite;
    plan(s, items);
    CHECK(items[0].output == items[0].path);
    CHECK(items[0].access == Access::Replace);
    // b.lila comes first, so b.ma saving beside it as b.lila collides with it.
    CHECK(paths::toUtf8(items[1].path.filename()) == "b.lila");
    CHECK(items[2].output == d / "src/b.lila");
    CHECK(items[2].access == Access::Collision);

    s.save = SaveMode::SaveAs;
    s.outputDir = paths::toUtf8(d / "out");
    s.suffix = "_fixed";
    // Windows sets the read-only attribute only once every write bit is gone.
    fs::permissions(d / "out/sub/c_fixed.lilaa", fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                    fs::perm_options::remove);
    plan(s, items);
    CHECK(items[0].output == d / "out/a_fixed.lila");
    CHECK(items[0].access == Access::New);
    CHECK(items[3].output == d / "out/sub/c_fixed.lilaa");
    CHECK(items[3].access == Access::ReadOnly);
    s.keepTree = false;
    s.format = "lila";
    CHECK(outputFor(s, items[3]) == d / "out/c_fixed.lila");
    fs::permissions(d / "out/sub/c_fixed.lilaa", fs::perms::owner_write, fs::perm_options::add);
}

TEST_CASE("batch specs round-trip and reject bad values")
{
    Spec s;
    s.script = "print(1)";
    s.inputs = {"a", "b"};
    s.save = SaveMode::SaveAs;
    s.format = "lilaa";
    s.workers = 3;
    s.args = {{"k", 1}};
    Spec back = specFromJson(toJson(s));
    CHECK(toJson(back) == toJson(s));
    CHECK(specFromJson({{"extensions", "ma, mb ;lila"}}).extensions == std::vector<std::string>{"ma", "mb", "lila"});
    CHECK(specFromJson({{"save", true}}).save == SaveMode::Overwrite);
    CHECK_THROWS_AS(specFromJson({{"save", "sometimes"}}), std::invalid_argument);
    CHECK_THROWS_AS(specFromJson({{"format", "fbx"}}), std::invalid_argument);
    CHECK_THROWS_AS(specFromJson({{"checkout", "maybe"}}), std::invalid_argument);
}

TEST_CASE("batch CSV quotes fields and adds a column per result key")
{
    std::vector<Item> items(2);
    items[0].path = "C:/a, b.lila";
    items[1].path = "C:/c.lila";
    std::vector<FileResult> rs(2);
    rs[0].status = Status::Done;
    rs[0].results = {{"count", 3}, {"note", "say \"hi\""}};
    rs[1].status = Status::Failed;
    rs[1].message = "line1\nline2";
    rs[1].results = {{"extra", json::array({1, 2})}};
    std::string csv = toCsv(items, rs);
    std::istringstream in(csv);
    std::string header;
    std::getline(in, header);
    CHECK(header == "index,file,status,seconds,output,saved,message,count,note,extra");
    CHECK(csv.find("\"C:/a, b.lila\",done") != std::string::npos);
    CHECK(csv.find(",3,\"say \"\"hi\"\"\",") != std::string::npos);
    CHECK(csv.find("\"line1\nline2\",,,\"[1,2]\"") != std::string::npos);
}

TEST_CASE("batch runs queue files over a bounded number of workers")
{
    fs::path d = tempDir("queue");
    auto fake = std::make_shared<FakeLauncher>();
    Spec s = fakeSpec(d, {"a.lila", "b_bad.lila", "c.lila", "d_crash.lila", "e.lila", "f.lila"});
    s.workers = 2;
    std::vector<std::string> warnings;
    auto run = startRun(s, &warnings, fake);
    REQUIRE(run->wait(30));
    CHECK(fake->peak <= 2);
    CHECK(fake->started == 6);
    std::vector<FileResult> rs = run->results();
    CHECK(rs[0].status == Status::Done);
    CHECK(rs[0].results["name"] == "a.lila");
    CHECK(rs[1].status == Status::Failed);
    CHECK(rs[1].message == "boom");
    CHECK(rs[3].status == Status::Failed);
    CHECK(rs[3].message.find("0xC0000005") != std::string::npos);
    CHECK(!rs[0].log.empty());
    json sum = run->summary();
    CHECK(sum["counts"]["done"] == 4);
    CHECK(sum["counts"]["failed"] == 2);
    CHECK(sum["state"] == "finished");
    CHECK(findRun() == run);
    CHECK(findRun(run->id()) == run);

    auto retry = retryRun(*run, {}, fake);
    REQUIRE(retry->wait(30));
    REQUIRE(retry->items().size() == 2);
    CHECK(retry->items()[0].path.filename() == "b_bad.lila");
    CHECK(retry->items()[1].path.filename() == "d_crash.lila");
}

TEST_CASE("batch runs stop on error, time out, cancel and dry-run")
{
    fs::path d = tempDir("control");
    auto fake = std::make_shared<FakeLauncher>();

    Spec stop = fakeSpec(d, {"a_bad.lila", "b.lila", "c.lila"});
    stop.workers = 1;
    stop.stopOnError = true;
    auto r1 = startRun(stop, nullptr, fake);
    REQUIRE(r1->wait(30));
    CHECK(r1->result(0).status == Status::Failed);
    CHECK(r1->result(1).status == Status::Skipped);
    CHECK(r1->result(2).status == Status::Skipped);

    Spec slow = fakeSpec(d, {"x_hang.lila", "y.lila"});
    slow.workers = 1;
    slow.timeout = 0.2;
    auto r2 = startRun(slow, nullptr, fake);
    REQUIRE(r2->wait(30));
    CHECK(r2->result(0).status == Status::Failed);
    CHECK(r2->result(0).message.find("timed out") != std::string::npos);
    CHECK(r2->result(1).status == Status::Done);

    Spec hang = fakeSpec(d, {"p_hang.lila", "q.lila", "r.lila"});
    hang.workers = 1;
    auto r3 = startRun(hang, nullptr, fake);
    while (r3->result(0).status != Status::Running)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    r3->cancel();
    REQUIRE(r3->wait(30));
    CHECK(r3->wasCancelled());
    CHECK(r3->result(0).status == Status::Cancelled);
    CHECK(r3->result(2).status == Status::Cancelled);
    CHECK(r3->summary()["state"] == "cancelled");

    const int before = fake->started;
    Spec dry = fakeSpec(d, {"m.lila", "n.ma"});
    dry.dryRun = true;
    dry.save = SaveMode::SaveAs;
    dry.outputDir = paths::toUtf8(d / "out");
    auto r4 = startRun(dry, nullptr, fake);
    CHECK(r4->isFinished());
    CHECK(fake->started == before);
    CHECK(r4->result(0).status == Status::Planned);
    json files = r4->summary()["files"];
    CHECK(files[1]["output"].get<std::string>().ends_with("out/n.lila"));
    CHECK(files[1]["access"] == "new");

    Spec none;
    none.inputs = {paths::toUtf8(d / "nothing") + "/*.lila"};
    none.script = "pass";
    CHECK_THROWS_AS(startRun(none, nullptr, fake), std::invalid_argument);
    Spec noScript = fakeSpec(d, {"m.lila"});
    noScript.script = " \n";
    CHECK_THROWS_AS(startRun(noScript, nullptr, fake), std::invalid_argument);
}

TEST_CASE("batchRun presets and file lists live in prefs")
{
    batch::registerCommands();
    ops::execute("batchRunPreset", {{"args", {"fix"}}, {"spec", {{"script", "pass"}, {"save", "overwrite"}, {"workers", 2}}}});
    CHECK(ops::execute("batchRunPreset", {{"args", {"fix"}}})["workers"] == 2);
    CHECK(ops::execute("batchRunPreset", json::object()).contains("fix"));
    ops::execute("batchRunFileList", {{"args", {"props"}}, {"files", {"a.lila", "b.lila"}}});
    CHECK(ops::execute("batchRunFileList", {{"args", {"props"}}}).size() == 2);
    ops::execute("batchRunFileList", {{"args", {"props"}}, {"delete", true}});
    CHECK_THROWS_AS(ops::execute("batchRunFileList", {{"args", {"props"}}}), CommandError);
    ops::execute("batchRunPreset", {{"args", {"fix"}}, {"delete", true}});
    CHECK_FALSE(ops::execute("batchRunPreset", json::object()).contains("fix"));
}

TEST_CASE("batchRun end to end through lilabatch workers")
{
    batch::registerCommands();
    fs::path d = tempDir("e2e");
    for (const char* name : {"one", "two", "bad", "three"})
    {
        ops::execute("newScene", json::object());
        ops::execute("spaceLocator", {{"name", std::string(name) + "_loc"}});
        io::save(d / "in" / (std::string(name) + ".lila"));
    }
    ops::execute("newScene", json::object());
    touch(d / "in" / "skip_this.lila");

    const std::string script = "names = ls()\n"
                               "ctx.results['nodes'] = len(names)\n"
                               "ctx.results['tag'] = ctx.args.get('tag', '')\n"
                               "if 'bad' in ctx.file:\n"
                               "    raise ValueError('boom')\n"
                               "spaceLocator(name='batched')\n"
                               "if 'three' in ctx.file:\n"
                               "    ctx.skip('not today')\n";
    touch(d / "fix.py", script);
    const fs::path csv = d / "summary.csv";
    json sum = ops::execute("batchRun", {{"args", {paths::toUtf8(d / "in")}},
                                         {"scriptFile", paths::toUtf8(d / "fix.py")},
                                         {"excludePrefix", "skip"},
                                         {"save", "saveAs"},
                                         {"outputDir", paths::toUtf8(d / "out")},
                                         {"suffix", "_b"},
                                         {"workers", 3},
                                         {"scriptArgs", {{"tag", "t1"}}},
                                         {"csv", paths::toUtf8(csv)},
                                         {"wait", true}});
    INFO(sum.dump(1));
    REQUIRE(sum["state"] == "finished");
    CHECK(sum["total"] == 4);
    CHECK(sum["counts"]["done"] == 2);
    CHECK(sum["counts"]["failed"] == 1);
    CHECK(sum["counts"]["skipped"] == 1);

    std::map<std::string, json> byName;
    for (const json& f : sum["files"])
        byName[paths::toUtf8(paths::fromUtf8(f["file"].get<std::string>()).filename())] = f;
    const json& bad = byName["bad.lila"];
    CHECK(bad["status"] == "failed");
    CHECK(bad["message"] == "ValueError: boom");
    CHECK(paths::fromUtf8(bad["errorFile"].get<std::string>()) == d / "fix.py");
    CHECK(bad["errorLine"] == 5);
    CHECK(slurp(paths::fromUtf8(bad["log"].get<std::string>())).find("ValueError: boom") != std::string::npos);
    CHECK(byName["one.lila"]["results"]["nodes"].get<int>() >= 1);
    CHECK(byName["one.lila"]["results"]["tag"] == "t1");
    CHECK(byName["one.lila"]["saved"] == true);
    CHECK(byName["three.lila"]["status"] == "skipped");
    CHECK(byName["three.lila"]["message"] == "not today");

    CHECK(fs::exists(d / "out/one_b.lila"));
    CHECK(fs::exists(d / "out/two_b.lila"));
    CHECK_FALSE(fs::exists(d / "out/bad_b.lila"));
    CHECK_FALSE(fs::exists(d / "out/three_b.lila"));
    io::load(d / "out/one_b.lila");
    CHECK(scene::Graph::current().find("batched"));
    CHECK(scene::Graph::current().find("one_loc"));
    ops::execute("newScene", json::object());

    std::string table = slurp(csv);
    CHECK(table.starts_with("index,file,status,seconds,output,saved,message,nodes,tag"));
    CHECK(table.find("ValueError: boom") != std::string::npos);

    json retried = ops::execute("batchRunRetry", {{"wait", true}});
    CHECK(retried["total"] == 1);
    CHECK(retried["counts"]["failed"] == 1);
}

TEST_CASE("lilabatch passes the arguments after its script to the script")
{
    const char* argv[] = {"lilabatch", "--batch", "build.py", "out_dir", "--fast"};
    host::Options o = host::parse(5, const_cast<char**>(argv));
    CHECK(o.batchScript == "build.py");
    CHECK(o.scriptArgs == std::vector<std::string>{"out_dir", "--fast"});
    CHECK(o.path.empty());

    const char* scene[] = {"lilabatch", "scene.lila", "--batch", "build.py"};
    host::Options s = host::parse(4, const_cast<char**>(scene));
    CHECK(s.path == "scene.lila");
    CHECK(s.scriptArgs.empty());
}

TEST_CASE("lilabatch --export, --export-args, --quiet and --help")
{
    const char* convert[] = {"lilabatch", "-q", "--export", "Ship.lila", "out/Ship.fbx", "--export-args", "profile='unity', units='m'"};
    host::Options o = host::parse(7, const_cast<char**>(convert));
    CHECK(o.error.empty());
    CHECK(o.quiet);
    CHECK(o.batch);
    CHECK(o.exportScene == "Ship.lila");
    CHECK(o.exportOut == "out/Ship.fbx");
    CHECK(o.exportArgs == "profile='unity', units='m'");
    CHECK(o.path.empty());

    const char* alias[] = {"lilabatch", "--convert", "a.lila", "a.glb", "--quiet"};
    host::Options c = host::parse(5, const_cast<char**>(alias));
    CHECK(c.exportOut == "a.glb");
    CHECK(c.quiet);

    const char* missing[] = {"lilabatch", "--export", "a.lila"};
    CHECK_FALSE(host::parse(3, const_cast<char**>(missing)).error.empty());

    const char* help[] = {"lilabatch", "-h"};
    CHECK(host::parse(2, const_cast<char**>(help)).printHelp);
    const std::string usage = host::usage();
    for (const char* flag : {"--help", "--version", "--batch", "--command", "--export", "--convert", "--export-args", "--quiet",
                             "--batch-run", "--change-stream", "Examples"})
    {
        CAPTURE(flag);
        CHECK(usage.find(flag) != std::string::npos);
    }
}

TEST_CASE("export on save writes the scene's export file on every save and is saved with the scene")
{
    io::translators::registerBuiltins();
    io::exportOnSave::registerCommands();
    static bool hooked = false;
    if (!hooked)
    {
        io::onSaved([](const fs::path& p) { io::exportOnSave::onSceneSaved(p); });
        hooked = true;
    }
    const fs::path d = tempDir("export_on_save");
    ops::execute("newScene", json::object());
    ops::execute("polyCube", {{"name", "Box"}});
    ops::execute("select", {{"args", json::array()}});

    CHECK_THROWS_AS(ops::execute("exportOnSave", {{"path", "Exports/Box.xyz"}}), CommandError);
    CHECK_THROWS_AS(ops::execute("exportOnSave", {{"enable", true}}), CommandError);
    json r = ops::execute("exportOnSave", {{"path", "Exports/Box.obj"}});
    CHECK(r["enabled"] == true);
    CHECK(r["path"] == "Exports/Box.obj");

    const fs::path scene = d / "Box.lila";
    const fs::path out = d / "Exports" / "Box.obj";
    io::save(scene);
    REQUIRE(fs::exists(out));
    // The whole scene goes out though nothing is selected, and the selection stays as it was.
    CHECK(slurp(out).find("\nf ") != std::string::npos);
    CHECK(scene::Graph::current().selection().empty());
    CHECK_FALSE(io::modified());
    CHECK(paths::fromUtf8(ops::execute("exportOnSave", {{"query", true}})["file"].get<std::string>()) == out);

    fs::remove(out);
    ops::execute("newScene", json::object());
    CHECK(ops::execute("exportOnSave", {{"query", true}})["enabled"] == false);
    io::load(scene);
    CHECK(ops::execute("exportOnSave", {{"query", true}})["path"] == "Exports/Box.obj");
    io::save(scene);
    CHECK(fs::exists(out));

    ops::execute("exportOnSave", {{"enable", false}});
    fs::remove(out);
    io::save(scene);
    CHECK_FALSE(fs::exists(out));
    ops::execute("newScene", json::object());
}

TEST_CASE("lilabatch --export converts a scene by the output's extension and fails with a message")
{
    const fs::path d = tempDir("export_cli");
    ops::execute("newScene", json::object());
    ops::execute("polyCube", {{"name", "Box"}});
    io::save(d / "Box.lila");
    ops::execute("newScene", json::object());

    const fs::path exe = paths::executableDir() / "lilabatch.exe";
    REQUIRE(fs::exists(exe));
    auto lilabatch = [&](const std::string& args, const fs::path& log) {
        // cmd.exe strips the outer quotes, leaving the quoted exe and arguments intact.
        const std::string line = "\"\"" + paths::toUtf8(exe) + "\" " + args + " > \"" + paths::toUtf8(log) + "\" 2>&1\"";
        return std::system(line.c_str());
    };
    const fs::path glb = d / "out" / "Box.glb";
    CHECK(lilabatch("--quiet --export \"" + paths::toUtf8(d / "Box.lila") + "\" \"" + paths::toUtf8(glb) + "\"", d / "ok.log") == 0);
    REQUIRE(fs::exists(glb));
    CHECK(slurp(glb).starts_with("glTF"));
    const std::string quiet = slurp(d / "ok.log");
    INFO(quiet);
    CHECK(quiet.find("[info]") == std::string::npos);

    CHECK(lilabatch("--quiet --export \"" + paths::toUtf8(d / "Box.lila") + "\" \"" + paths::toUtf8(d / "Box.xyz") + "\"", d / "bad.log") == 1);
    const std::string bad = slurp(d / "bad.log");
    INFO(bad);
    CHECK(bad.find("lilabatch --export: no exporter writes '.xyz' files") != std::string::npos);
}
