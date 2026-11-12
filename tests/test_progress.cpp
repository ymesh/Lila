#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/MainQueue.h"
#include "core/Progress.h"
#include "io/SceneFile.h"
#include "scene/Graph.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

using namespace lila;
using scene::Graph;
using json = nlohmann::json;
using namespace testutil;

namespace
{
const progress::Snapshot* find(const std::vector<progress::Snapshot>& roots, const std::string& title)
{
    for (const progress::Snapshot& s : roots)
        if (s.title == title)
            return &s;
    return nullptr;
}

/** Installs a pump for the test's duration, as the UI does; it counts its calls. */
struct TestPump
{
    std::atomic<int> calls{0};
    TestPump() { progress::setPump([this] { ++calls; }, 1); }
    ~TestPump() { progress::setPump(nullptr); }
};
} // namespace

TEST_CASE("progress: nested scopes map onto their share of the parent")
{
    progress::Scope root("root", 10.0);
    CHECK(progress::fraction(root) == doctest::Approx(0.0));
    {
        // A child standing for 4 of the root's 10 units, half done: 2 of 10.
        progress::Scope child("child", 2.0, progress::None, 4.0);
        child.step();
        CHECK(progress::fraction(root) == doctest::Approx(0.2));
        {
            // A grandchild for the child's second unit, a quarter done: (1 + 0.25) / 2 of the child.
            progress::Scope grand("grand", 4.0);
            grand.step();
            CHECK(progress::fraction(root) == doctest::Approx((1.25 / 2.0) * 4.0 / 10.0));
        }
        // Closing the grandchild completes its unit of the child.
        CHECK(child.done() == doctest::Approx(2.0));
        CHECK(progress::fraction(root) == doctest::Approx(0.4));
    }
    // And closing the child adds its whole span to the root.
    CHECK(root.done() == doctest::Approx(4.0));
    root.set(9.0);
    CHECK(progress::fraction(root) == doctest::Approx(0.9));
    root.step(5.0);
    CHECK(progress::fraction(root) == doctest::Approx(1.0)); // clamped
}

TEST_CASE("progress: unknown totals, status from the deepest scope, several roots")
{
    progress::Scope root("Unfurling...");
    root.setStatus("scene.lila");
    {
        auto roots = progress::snapshot();
        const progress::Snapshot* s = find(roots, "Unfurling...");
        REQUIRE(s);
        CHECK_FALSE(s->determinate);
        CHECK(s->status == "scene.lila");
        CHECK(s->mainThread);
    }
    {
        progress::Scope reading("Reading scene.lila");
        auto roots = progress::snapshot();
        CHECK(find(roots, "Unfurling...")->status == "Reading scene.lila");
        reading.setStatus("Building meshes 3/9");
        roots = progress::snapshot();
        CHECK(find(roots, "Unfurling...")->status == "Building meshes 3/9");
    }

    // A scope opened on another thread is a root of its own, not modal.
    std::atomic<bool> seen{false};
    std::thread([&] {
        progress::Scope bg("Autosaving", 0.0, progress::Silent);
        auto roots = progress::snapshot();
        const progress::Snapshot* s = find(roots, "Autosaving");
        seen = s && s->silent && !s->mainThread;
    }).join();
    CHECK(seen);
    CHECK(progress::modal());
}

TEST_CASE("progress: cancelling a root reaches every nested scope")
{
    progress::Scope root("Converting x.mb...", 2.0, progress::Cancellable);
    progress::Scope inner("Parsing", 100.0);
    CHECK_FALSE(inner.cancelled());
    CHECK_NOTHROW(inner.checkCancel());

    auto roots = progress::snapshot();
    const progress::Snapshot* s = find(roots, "Converting x.mb...");
    REQUIRE(s);
    CHECK(s->cancellable);
    progress::cancel(s->id);
    CHECK(inner.cancelled());
    CHECK_THROWS_AS(inner.checkCancel(), progress::Cancelled);
    CHECK(find(progress::snapshot(), "Converting x.mb...")->cancelled);

    root.setCancellable(false);
    CHECK_FALSE(find(progress::snapshot(), "Converting x.mb...")->cancellable);
}

TEST_CASE("progress: showNow from a nested scope marks the root and pumps at once")
{
    TestPump pump;
    progress::Scope root("Preparing bake inputs", 1.0);
    progress::Scope inner("Smoothing", 1.0);
    CHECK_FALSE(find(progress::snapshot(), "Preparing bake inputs")->immediate);
    const int before = pump.calls;
    inner.showNow();
    CHECK(find(progress::snapshot(), "Preparing bake inputs")->immediate);
    CHECK(pump.calls > before);
}

TEST_CASE("progress: the reporter is throttled and told when the last root closes")
{
    std::atomic<int> calls{0};
    std::atomic<bool> sawEmpty{false};
    progress::setReporter(
        [&](const std::vector<progress::Snapshot>& roots) {
            ++calls;
            if (roots.empty())
                sawEmpty = true;
        },
        40);
    const auto start = std::chrono::steady_clock::now();
    {
        progress::Scope root("busy", 100000.0);
        for (int i = 0; i < 100000; ++i)
            root.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        root.step();
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    progress::setReporter(nullptr);
    // At most one call per interval, plus the forced ones when the root opens and closes.
    CHECK(calls >= 2);
    CHECK(calls <= int(ms / 40.0) + 3);
    CHECK(sawEmpty);
}

TEST_CASE("progress: nobody listening costs nothing but the count")
{
    progress::Scope root("quiet", 3.0);
    root.step();
    root.step(2.0);
    CHECK(root.done() == doctest::Approx(3.0));
    CHECK(progress::fraction(root) == doctest::Approx(1.0));
}

TEST_CASE("progress: runOnWorker runs inline without a pump, on a worker with one")
{
    std::thread::id where;
    progress::runOnWorker([&] { where = std::this_thread::get_id(); });
    CHECK(where == std::this_thread::get_id());

    TestPump pump;
    progress::Scope root("root", 2.0);
    double midway = -1.0;
    progress::runOnWorker([&] {
        where = std::this_thread::get_id();
        // Nests under the caller's scope although it runs on another thread.
        progress::Scope worker("worker", 2.0);
        worker.step();
        midway = progress::fraction(root);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    });
    CHECK(where != std::this_thread::get_id());
    CHECK(midway == doctest::Approx(0.25));
    CHECK(pump.calls > 0); // the caller kept pumping while it waited
    CHECK(root.done() == doctest::Approx(1.0));

    CHECK_THROWS_WITH(progress::runOnWorker([] { throw std::runtime_error("bad file"); }), "bad file");
}

TEST_CASE("progress: console line")
{
    progress::Snapshot s;
    s.title = "Converting Interceptor_01.mb...";
    s.status = "Building meshes 120/263";
    s.determinate = true;
    s.fraction = 0.425;
    CHECK(progress::consoleLine({s}) == "Converting Interceptor_01.mb...   42%  Building meshes 120/263");
    s.determinate = false;
    CHECK(progress::consoleLine({s}) == "Converting Interceptor_01.mb...  Building meshes 120/263");
    CHECK(progress::consoleLine({s}, 20).size() == 20);
    CHECK(progress::consoleLine({}).empty());
    CHECK(progress::formatElapsed(1.24) == "1.2 s");
    CHECK(progress::formatElapsed(125.0) == "2 min 05 s");
}

TEST_CASE("graph batch: batched listeners hear one Reset, immediate ones every change")
{
    run("newScene");
    scene::Graph& g = scene::Graph::current();
    int batched = 0, resets = 0, immediate = 0;
    int a = g.addListener([&](scene::Change c, scene::NodeId) {
        ++batched;
        resets += c == scene::Change::Reset;
    });
    int b = g.addListener([&](scene::Change, scene::NodeId) { ++immediate; }, true);
    {
        scene::Graph::Batch batch;
        {
            scene::Graph::Batch nested;
            run("spaceLocator", {{"name", "A"}});
        }
        CHECK(batched == 0); // the outer batch is still open
        run("spaceLocator", {{"name", "B"}});
        run("setAttr", {{"args", {"B.ty", 2}}});
    }
    CHECK(batched == 1);
    CHECK(resets == 1);
    CHECK(immediate >= 3);
    {
        scene::Graph::Batch quiet; // no changes, no Reset
    }
    CHECK(batched == 1);
    run("setAttr", {{"args", {"B.ty", 3}}});
    CHECK(batched == 2);
    CHECK(resets == 1);
    g.removeListener(a);
    g.removeListener(b);
}

TEST_CASE("main queue: work posted during a hold waits for it")
{
    int ran = 0;
    {
        mainq::Hold hold;
        mainq::post([&] { ++ran; });
        mainq::drain();
        CHECK(ran == 0);
    }
    mainq::drain();
    CHECK(ran == 1);
}

TEST_CASE("scene load: reading on a worker builds the same scene")
{
    // Meshes, a reference with edits and a scene child under a referenced node.
    run("newScene");
    run("polyCube", {{"name", "Box"}, {"width", 3}});
    run("spaceLocator", {{"name", "Pin"}});
    run("parent", {{"args", {"Pin", "Box"}}});
    std::string asset = tempPath("progress", "asset.lila");
    run("saveScene", {{"args", {asset}}});

    run("newScene");
    run("polySphere", {{"name", "Ball"}});
    run("polyTorus", {{"name", "Ring"}, {"generator", false}});
    run("createReference", {{"args", {asset}}, {"namespace", "hero"}});
    run("setAttr", {{"args", {"hero:Box.tx", 4}}});
    run("parent", {{"args", {"Ring", "hero:Pin"}}});
    for (const char* name : {"shot.lila", "shot.lilaa"})
    {
        std::string shot = tempPath("progress", name);
        run("saveScene", {{"args", {shot}}});

        run("openScene", {{"args", {shot}}, {"force", true}});
        const json inline_ = io::serialize();
        {
            TestPump pump;
            run("openScene", {{"args", {shot}}, {"force", true}});
            CHECK(pump.calls > 0);
        }
        const json worker = io::serialize();
        CHECK(inline_ == worker);
        CHECK(inline_["nodes"].size() >= 3);
        CHECK(inline_["references"].size() == 1);
    }
}

TEST_CASE("scene load: cancelling while the file is read leaves the scene alone")
{
    run("newScene");
    run("polyCube", {{"name", "Keep"}});
    std::string path = tempPath("progress", "cancel.lila");
    run("polySphere", {{"name", "Other"}});
    run("saveScene", {{"args", {path}}});
    run("newScene");
    run("polyCube", {{"name", "Keep"}});
    const json before = io::serialize();

    // The pump stands in for the Cancel button: it cancels whatever is running.
    progress::setPump([] {
        for (const progress::Snapshot& s : progress::snapshot())
            if (s.cancellable)
                progress::cancel(s.id);
    }, 1);
    CHECK_THROWS(run("openScene", {{"args", {path}}, {"force", true}}));
    progress::setPump(nullptr);
    CHECK(io::serialize()["nodes"] == before["nodes"]);
}

TEST_CASE("progress: the cancel token follows this thread's scopes")
{
    CHECK_FALSE(progress::cancelRequested());
    const uint64_t before = progress::cancelledCount();
    {
        progress::Scope root("Importing token.fbx", 2.0, progress::Cancellable);
        progress::Scope inner("Building", 1.0);
        CHECK_FALSE(progress::cancelRequested());
        CHECK_NOTHROW(progress::checkCancel());
        progress::cancel(find(progress::snapshot(), "Importing token.fbx")->id);
        CHECK(progress::cancelRequested());
        CHECK_THROWS_AS(progress::checkCancel(), progress::Cancelled);
        bool seenOnWorker = false;
        std::thread([&, parent = progress::current()] {
            progress::Adopt adopt(parent);
            seenOnWorker = progress::cancelRequested();
        }).join();
        CHECK(seenOnWorker);
    }
    CHECK_FALSE(progress::cancelRequested());
    CHECK(progress::cancelledCount() == before + 1);
}

TEST_CASE("commands: an undoable command aborted midway leaves nothing behind")
{
    ops::registerCommand({
        .name = "testAbortedImport",
        .doc = "Builds a cube, then is cancelled as an import would be.",
        .run = [](const Args&) -> json {
            progress::Scope root("Importing half.fbx", 2.0, progress::Cancellable);
            ops::execute("polyCube", {{"name", "Half"}});
            progress::cancel(find(progress::snapshot(), "Importing half.fbx")->id);
            progress::checkCancel();
            return nullptr;
        },
    });
    run("newScene");
    run("polyCube", {{"name", "Kept"}});
    const size_t undos = UndoStack::get().undoCount();
    CHECK_THROWS_AS(run("testAbortedImport"), CommandCancelled);
    CHECK(Graph::current().find("Kept"));
    CHECK_FALSE(Graph::current().find("Half"));
    CHECK(UndoStack::get().undoCount() == undos);
}

TEST_CASE("scene load: aborting at any point of the build puts the previous scene back")
{
    run("newScene");
    for (int i = 0; i < 20; ++i)
        run("polyCube", {{"name", "Box" + std::to_string(i)}});
    const std::string path = tempPath("progress", "abort_build.lila");
    run("saveScene", {{"args", {path}}});

    // Once right after the file is read (the scene just cleared), once with the file's nodes half built.
    for (bool midBuild : {false, true})
    {
        CAPTURE(midBuild);
        run("newScene");
        run("polyCube", {{"name", "Keep"}});
        const std::string keep = tempPath("progress", "keep.lila");
        run("saveScene", {{"args", {keep}}});
        run("spaceLocator", {{"name", "Unsaved"}});
        run("select", {{"args", {"Unsaved"}}});
        const json before = io::serialize();
        REQUIRE(io::modified());

        progress::setPump([midBuild] {
            const bool cleared = !Graph::current().find("Keep") && !Graph::current().find("Unsaved");
            if (!(midBuild ? Graph::current().find("Box0") != nullptr : cleared))
                return;
            for (const progress::Snapshot& s : progress::snapshot())
                if (s.cancellable)
                    progress::cancel(s.id);
        }, 0);
        CHECK_THROWS_AS(run("openScene", {{"args", {path}}, {"force", true}}), CommandCancelled);
        progress::setPump(nullptr);
        const json after = io::serialize();
        CHECK(after["nodes"] == before["nodes"]);
        CHECK(after["selection"] == before["selection"]);
        CHECK(std::filesystem::equivalent(io::currentPath(), keep));
        CHECK(io::modified());
        CHECK_FALSE(Graph::current().find("Box0"));
    }
}
