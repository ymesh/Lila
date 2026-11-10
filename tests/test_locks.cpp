#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "io/SceneFile.h"
#include "io/SceneHistory.h"
#include "scene/Graph.h"
#include "scene/Transform.h"
#include "scene/TransformOps.h"

#include <chrono>
#include <filesystem>
#include <thread>

using namespace lila;
using namespace lila::scene;
using namespace testutil;
namespace fs = std::filesystem;

namespace
{
fs::path lockDir(const char* name)
{
    fs::path dir = fs::temp_directory_path() / "lila_tests" / "locks" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}
} // namespace

TEST_CASE("a locked plug refuses setAttr until unlocked, and locking undoes")
{
    run("newScene");
    run("spaceLocator", {{"name", "A"}});
    run("setAttr", {{"args", {"A.tx"}}, {"lock", true}});
    CHECK(run("getAttr", {{"args", {"A.tx"}}, {"lock", true}}).get<bool>());
    CHECK_FALSE(run("getAttr", {{"args", {"A.translate"}}, {"lock", true}}).get<bool>());
    CHECK_THROWS_AS(run("setAttr", {{"args", {"A.tx", 5}}}), CommandError);
    // A whole Vec3 set fails only when the locked component would change.
    CHECK_THROWS_AS(run("setAttr", {{"args", {"A.translate", json::array({1, 2, 3})}}}), CommandError);
    run("setAttr", {{"args", {"A.translate", json::array({0, 2, 3})}}});
    CHECK(vec(run("getAttr", {{"args", {"A.translate"}}})) == glm::dvec3(0, 2, 3));
    CHECK(run("listAttr", {{"args", {"A"}}, {"locked", true}}) == json::array({"translateX"}));

    run("undo"); // the translate set
    run("undo"); // the lock
    CHECK_FALSE(run("getAttr", {{"args", {"A.tx"}}, {"lock", true}}).get<bool>());
    run("setAttr", {{"args", {"A.tx", 5}}});
    run("undo");
    run("redo");
    CHECK(Graph::current().isLocked(Graph::current().plug("A.tx")) == false);

    run("setAttr", {{"args", {"A.translate"}}, {"lock", true}});
    CHECK(run("getAttr", {{"args", {"A.ty"}}, {"lock", true}}).get<bool>());
    run("setAttr", {{"args", {"A.ty", 7}}, {"lock", false}});
    CHECK(run("getAttr", {{"args", {"A.ty"}}}).get<double>() == 7.0);
    CHECK(run("getAttr", {{"args", {"A.tz"}}, {"lock", true}}).get<bool>());

    run("setAttr", {{"args", {"A.visibility"}}, {"lock", true}});
    CHECK_THROWS_AS(run("setAttr", {{"args", {"A.visibility", false}}}), CommandError);
}

TEST_CASE("move, rotate and scale leave locked channels where they are")
{
    run("newScene");
    run("spaceLocator", {{"name", "A"}});
    run("setAttr", {{"args", {"A.ty"}}, {"lock", true}});
    run("move", {{"args", {"A"}}, {"by", {1, 2, 3}}});
    CHECK(vec(run("getAttr", {{"args", {"A.translate"}}})) == glm::dvec3(1, 0, 3));
    run("setAttr", {{"args", {"A.scale"}}, {"lock", true}});
    run("scale", {{"args", {"A"}}, {"by", {2, 2, 2}}});
    CHECK(vec(run("getAttr", {{"args", {"A.scale"}}})) == glm::dvec3(1, 1, 1));
    run("setAttr", {{"args", {"A.rx"}}, {"lock", true}});
    run("rotate", {{"args", {"A"}}, {"to", {30, 40, 50}}});
    CHECK(vec(run("getAttr", {{"args", {"A.rotate"}}})) == glm::dvec3(0, 40, 50));
}

TEST_CASE("rotatedEuler: an object-space turn about X changes only rotateX in XYZ order")
{
    run("newScene");
    run("spaceLocator", {{"name", "A"}});
    run("rotate", {{"args", {"A"}}, {"to", {10, 20, 30}}});
    Graph& g = Graph::current();
    NodeId a = g.find("A")->id();
    glm::dvec3 before = g.node(a)->as<glm::dvec3>(Transform::kRotate);
    glm::dvec3 after = xformops::rotatedEuler(g, a, glm::angleAxis(glm::radians(5.0), glm::dvec3(1, 0, 0)), Space::Object);
    CHECK(after.x == doctest::Approx(before.x + 5.0));
    CHECK(after.y == doctest::Approx(before.y));
    CHECK(after.z == doctest::Approx(before.z));
    glm::dvec3 world = xformops::rotatedEuler(g, a, glm::angleAxis(glm::radians(5.0), glm::dvec3(0, 0, 1)));
    CHECK(world.z == doctest::Approx(before.z + 5.0));
    CHECK(world.y == doctest::Approx(before.y));
}

TEST_CASE("locks survive save and load, binary and ASCII, and duplicate")
{
    fs::path dir = lockDir("roundtrip");
    for (const char* file : {"locked.lila", "locked.lilaa"})
    {
        run("newScene");
        run("spaceLocator", {{"name", "A"}});
        run("setAttr", {{"args", {"A.tx", 4}}, {"lock", true}});
        run("setAttr", {{"args", {"A.visibility"}}, {"lock", true}});
        fs::path path = dir / file;
        run("saveScene", {{"args", {path.generic_string()}}});
        run("openScene", {{"args", {path.generic_string()}}, {"force", true}});
        CHECK(run("getAttr", {{"args", {"A.tx"}}}).get<double>() == 4.0);
        CHECK(run("listAttr", {{"args", {"A"}}, {"locked", true}}) == json::array({"translateX", "visibility"}));
    }
    json copy = run("duplicate", {{"args", {"A"}}});
    CHECK(run("getAttr", {{"args", {copy[0].get<std::string>() + ".tx"}}, {"lock", true}}).get<bool>());
    CHECK_FALSE(run("getAttr", {{"args", {copy[0].get<std::string>() + ".ty"}}, {"lock", true}}).get<bool>());
}

TEST_CASE("flushUndo drops the past, the future or both")
{
    UndoStack& u = UndoStack::get();
    auto three = [] {
        run("newScene");
        for (const char* name : {"A", "B", "C"})
            run("spaceLocator", {{"name", name}});
        run("undo");
    };

    three();
    run("flushUndo");
    CHECK(u.undoCount() == 0);
    CHECK(u.redoCount() == 1);
    run("redo");
    CHECK(Graph::current().find("C"));
    run("undo");
    CHECK_FALSE(Graph::current().find("C"));
    CHECK_THROWS(run("undo"));

    three();
    run("flushUndo", {{"which", "future"}});
    CHECK(u.undoCount() == 2);
    CHECK(u.redoCount() == 0);

    three();
    run("flushUndo", {{"which", "all"}});
    CHECK(u.undoCount() == 0);
    CHECK(u.redoCount() == 0);

    CHECK_THROWS_AS(run("flushUndo", {{"which", "sideways"}}), CommandError);
}

TEST_CASE("flushing the past keeps a saved scene unmodified and undo back to it clean")
{
    fs::path scene = lockDir("flushpast") / "shot.lila";
    run("newScene");
    run("spaceLocator", {{"name", "A"}});
    run("saveScene", {{"args", {scene.generic_string()}}});
    run("spaceLocator", {{"name", "B"}});
    run("undo");
    CHECK_FALSE(io::modified());
    run("flushUndo");
    CHECK_FALSE(io::modified());
    run("redo");
    CHECK(io::modified());
    run("undo");
    CHECK_FALSE(io::modified());
}

TEST_CASE("saveScene ifModified skips an unchanged file unless it is missing")
{
    fs::path scene = lockDir("noop") / "shot.lila";
    run("newScene");
    run("spaceLocator", {{"name", "A"}});
    run("saveScene", {{"args", {scene.generic_string()}}});
    REQUIRE(fs::exists(scene));
    CHECK(io::upToDate(scene));

    auto stamp = fs::last_write_time(scene) - std::chrono::hours(1);
    fs::last_write_time(scene, stamp);
    run("saveScene", {{"ifModified", true}});
    CHECK(fs::last_write_time(scene) == stamp);

    run("move", {{"args", {"A"}}, {"by", {1, 0, 0}}});
    CHECK_FALSE(io::upToDate(scene));
    run("saveScene", {{"ifModified", true}});
    CHECK(fs::last_write_time(scene) != stamp);

    fs::remove(scene);
    CHECK_FALSE(io::upToDate(scene));
    run("saveScene", {{"ifModified", true}});
    CHECK(fs::exists(scene));

    // A different path always writes.
    fs::path other = scene.parent_path() / "other.lila";
    run("saveScene", {{"args", {other.generic_string()}}, {"ifModified", true}});
    CHECK(fs::exists(other));
}

TEST_CASE("autosave skips a scene that matches its file or the last autosave")
{
    fs::path scene = lockDir("autosave") / "auto.lila";
    run("newScene");
    run("spaceLocator", {{"name", "A"}});
    run("saveScene", {{"args", {scene.generic_string()}}});
    fs::path folder = io::history::autosaveFolder(scene);
    std::error_code ec;
    fs::remove_all(folder, ec);
    auto count = [&] { return io::history::entries(folder, io::history::stemFor(scene), "autosave").size(); };
    auto settle = [&](size_t want) {
        for (int i = 0; i < 200 && count() < want; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return count();
    };

    io::autosave();
    CHECK(settle(1) == 0);

    run("move", {{"args", {"A"}}, {"by", {1, 0, 0}}});
    io::autosave();
    CHECK(settle(1) == 1);

    // Back to the saved state: nothing new to keep.
    run("undo");
    io::autosave();
    CHECK(settle(2) == 1);

    // Forward again to exactly what the last autosave holds.
    run("redo");
    io::autosave();
    CHECK(settle(2) == 1);

    run("move", {{"args", {"A"}}, {"by", {1, 0, 0}}});
    io::autosave();
    CHECK(settle(2) == 2);
}

TEST_CASE("move, rotate and scale take one axis with x=, y=, z=")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 10}, {"height", 10}, {"depth", 10}})[0];
    run("move", {{"args", {cube}}, {"to", {5, 6, 7}}});
    run("move", {{"args", {cube}}, {"x", 0}});
    CHECK(vec(run("getAttr", {{"args", {cube + ".translate"}}})) == glm::dvec3(0, 6, 7));
    run("rotate", {{"args", {cube}}, {"to", {10, 20, 30}}});
    run("rotate", {{"args", {cube}}, {"y", 90}});
    CHECK(vec(run("getAttr", {{"args", {cube + ".rotate"}}})) == glm::dvec3(10, 90, 30));
    run("scale", {{"args", {cube}}, {"z", 2}});
    CHECK(vec(run("getAttr", {{"args", {cube + ".scale"}}})) == glm::dvec3(1, 1, 2));
    run("rotate", {{"args", {cube}}, {"to", {0, 0, 0}}});
    run("scale", {{"args", {cube}}, {"to", {1, 1, 1}}});

    // Components: every selected point lands on the plane.
    run("selectComponents", {{"args", {cube}}, {"vertices", {0, 1, 2, 3, 4, 5, 6, 7}}});
    run("move", {{"x", 3}});
    json pts = run("meshData", {{"args", {cube}}, {"worldSpace", true}})["points"];
    for (const json& p : pts)
        CHECK(p[0].get<double>() == doctest::Approx(3.0));
    run("scale", {{"y", 0}});
    pts = run("meshData", {{"args", {cube}}, {"worldSpace", true}})["points"];
    for (const json& p : pts)
        CHECK(p[1].get<double>() == doctest::Approx(6.0));
}

TEST_CASE("gimbalFrame: turning about each gimbal axis changes only that channel, and Y at 90 lines X up with Z")
{
    run("newScene");
    run("spaceLocator", {{"name", "A"}});
    Graph& g = Graph::current();
    NodeId a = g.find("A")->id();
    for (int order : {0, 4})
    {
        run("setAttr", {{"args", {"A.rotateOrder", order}}});
        run("rotate", {{"args", {"A"}}, {"to", {10, 20, 30}}});
        const glm::dmat3 frame = xformops::gimbalFrame(g, a);
        for (int i = 0; i < 3; ++i)
        {
            glm::dvec3 after = xformops::rotatedEuler(g, a, glm::angleAxis(glm::radians(5.0), frame[i]));
            glm::dvec3 want(10, 20, 30);
            want[i] += 5.0;
            for (int c = 0; c < 3; ++c)
                CHECK(after[c] == doctest::Approx(want[c]));
        }
    }
    run("setAttr", {{"args", {"A.rotateOrder", 0}}});
    run("rotate", {{"args", {"A"}}, {"to", {0, 90, 0}}});
    const glm::dmat3 locked = xformops::gimbalFrame(g, a);
    CHECK(std::abs(glm::dot(locked[0], locked[2])) == doctest::Approx(1.0));

    run("rotate", {{"args", {"A"}}, {"by", {15, 0, 0}}, {"space", "gimbal"}});
    CHECK(vec(run("getAttr", {{"args", {"A.rotate"}}})) == glm::dvec3(15, 90, 0));
}
