#include <doctest/doctest.h>

#include "TestUtil.h"

#include "core/PrefStore.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "scene/Components.h"
#include "scene/Duplicate.h"
#include "scene/Graph.h"
#include "scene/LastOp.h"
#include "scene/Repeat.h"
#include "scene/Transform.h"
#include "ui/Manipulator.h"
#include "ui/Tools.h"
#include "ui/ViewInfo.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

using namespace lila;
using json = nlohmann::json;
using namespace testutil;

namespace
{
json fromUi(const char* name, json call = json::object()) { return ops::execute(name, call, CallSource::UI); }

/** Full paths, which later copies with the same short names can't make ambiguous. */
std::vector<std::string> full(const json& made)
{
    std::vector<std::string> out;
    for (const json& m : made)
        out.push_back(scene::Graph::current().find(m.get<std::string>())->fullPath());
    return out;
}
} // namespace

TEST_CASE("repeat maths: steps about a pivot compose into runs")
{
    using namespace scene::repeat;
    Step s;
    s.rotate = {0.0, 90.0, 0.0};
    glm::dmat4 step = stepMatrix(s, glm::dvec3(0.0));
    std::vector<glm::dmat4> m = copyMatrices(glm::translate(glm::dmat4(1.0), glm::dvec3(100, 0, 0)), step, 4);
    REQUIRE(m.size() == 4);
    checkNear(glm::dvec3(m[0][3]), {0, 0, -100});
    checkNear(glm::dvec3(m[1][3]), {-100, 0, 0});
    checkNear(glm::dvec3(m[3][3]), {100, 0, 0});

    // About a pivot: a rotation in place around (10, 0, 0) leaves that point where it is.
    glm::dmat4 about = stepMatrix(s, {10, 0, 0});
    checkNear(glm::dvec3(about * glm::dvec4(10, 0, 0, 1)), {10, 0, 0});

    // In an object's frame the translate runs along its axes.
    Step t;
    t.translate = {5, 0, 0};
    glm::dmat3 frame(glm::rotate(glm::dmat4(1.0), glm::radians(90.0), glm::dvec3(0, 0, 1)));
    checkNear(glm::dvec3(stepMatrix(t, glm::dvec3(0.0), frame)[3]), {0, 5, 0});

    const glm::dmat4 a = glm::translate(glm::dmat4(1.0), glm::dvec3(1, 2, 3));
    const glm::dmat4 b = glm::rotate(glm::translate(glm::dmat4(1.0), glm::dvec3(4, 2, 3)), 0.3, glm::dvec3(0, 1, 0));
    const glm::dmat4 d = delta(a, b);
    checkNear(glm::dvec3((d * a)[3]), glm::dvec3(b[3]));
}

TEST_CASE("duplicate count makes rows, rings and growing runs")
{
    run("newScene");
    std::string p = run("polyCube", {{"name", "Pillar"}})[0];
    json made = run("duplicate", {{"args", {p}}, {"count", 3}, {"translate", {100, 0, 0}}});
    REQUIRE(made.size() == 3);
    CHECK(get(made[0].get<std::string>() + ".tx") == doctest::Approx(100));
    CHECK(get(made[2].get<std::string>() + ".tx") == doctest::Approx(300));
    CHECK(run("ls", {{"selection", true}}).size() == 3);

    // A ring about the world origin; the rotate channels keep counting past 180.
    run("newScene");
    std::string s = run("polyCube", {{"name", "Spoke"}, {"position", {100, 0, 0}}})[0];
    json ring = run("duplicate", {{"args", {s}}, {"count", 3}, {"rotate", {0, 90, 0}}, {"pivot", "world"}});
    REQUIRE(ring.size() == 3);
    checkNear(worldPos(ring[0]), {0, 0, -100});
    checkNear(worldPos(ring[1]), {-100, 0, 0});
    checkNear(worldPos(ring[2]), {0, 0, 100});
    CHECK(get(ring[2].get<std::string>() + ".ry") == doctest::Approx(270));

    // A point pivot, and scale compounding per copy.
    json grow = run("duplicate", {{"args", {s}}, {"count", 2}, {"scale", {2, 2, 2}}, {"pivot", "point"}, {"pivotPoint", {100, 0, 0}}});
    CHECK(get(grow[0].get<std::string>() + ".sx") == doctest::Approx(2));
    CHECK(get(grow[1].get<std::string>() + ".sx") == doctest::Approx(4));
    checkNear(worldPos(grow[1]), {100, 0, 0});

    // Steps along the object's own axes.
    run("newScene");
    std::string r = run("polyCube", {{"name", "Rail"}})[0];
    run("rotate", {{"args", {r}}, {"by", {0, 0, 90}}});
    json along = run("duplicate", {{"args", {r}}, {"translate", {10, 0, 0}}, {"space", "object"}});
    checkNear(worldPos(along[0]), {0, 10, 0});

    CHECK_THROWS_AS(run("duplicate", {{"args", {r}}, {"mode", "clone"}}), CommandError);
    CHECK_THROWS_AS(run("duplicate", {{"args", {r}}, {"pivot", "centre"}}), CommandError);
}

TEST_CASE("duplicate modes and group")
{
    run("newScene");
    std::string c = run("polyCube", {{"name", "Bolt"}})[0];
    run("addModifier", {{"args", {c}}, {"kind", "subdivision"}});
    std::vector<std::string> inst = full(run("duplicate", {{"args", {c}}, {"count", 2}, {"mode", "instance"}, {"translate", {10, 0, 0}}, {"group", true}}));
    REQUIRE(inst.size() == 2);
    CHECK(kind(inst[0]) == "instance");
    CHECK(kind(inst[1]) == "instance");
    CHECK(inst[0] == "|Group|Bolt1");
    checkNear(worldPos(inst[1]), {20, 0, 0});

    std::vector<std::string> ref = full(run("duplicate", {{"args", {c}}, {"mode", "reference"}}));
    CHECK(kind(ref[0]) == "reference");

    // A plain copy of an instance is a mesh of its own with a live stack.
    std::vector<std::string> copy = full(run("duplicate", {{"args", {inst[0]}}}));
    CHECK(kind(copy[0]) == "none");
    CHECK(run("modifiers", {{"args", {copy[0]}}}).size() == 1);

    // History shared within one call is copied once, so a source and its reference duplicated together stay related.
    const size_t gens = count("polyCube");
    std::vector<std::string> both = full(run("duplicate", {{"args", {c, ref[0]}}}));
    CHECK(count("polyCube") == gens + 1);
    CHECK(kind(both[0]) == "source");
    CHECK(kind(both[1]) == "reference");
}

TEST_CASE("duplicate is adjustable: count and step re-run live")
{
    scene::lastOp::install();
    run("newScene");
    std::string c = run("polyCube", {{"name", "Post"}})[0];
    run("select", {{"args", {c}}});
    fromUi("duplicate", json::object());
    REQUIRE(scene::lastOp::live());
    REQUIRE(scene::lastOp::current()->command == "duplicate");
    std::vector<std::string> flags;
    for (const FlagDef* f : scene::lastOp::editableFlags("duplicate"))
        flags.push_back(f->name);
    for (const char* want : {"count", "translate", "rotate", "scale", "mode", "pivot", "group"})
        CHECK_MESSAGE(std::find(flags.begin(), flags.end(), want) != flags.end(), want);
    CHECK(std::find(flags.begin(), flags.end(), "name") == flags.end());

    scene::lastOp::rerun({{"count", 4}, {"translate", {0, 0, 50}}});
    CHECK(count("mesh") == 5);
    json sel = run("ls", {{"selection", true}});
    REQUIRE(sel.size() == 4);
    CHECK(get(sel[3].get<std::string>() + ".tz") == doctest::Approx(200));
    scene::lastOp::rerun({{"count", 2}});
    CHECK(count("mesh") == 3);
    run("undo");
    CHECK(count("mesh") == 5);

    // The option box offers the defaults only, not the per-call step.
    std::vector<std::string> fields;
    for (const ops::options::Field& f : ops::options::fields("duplicate"))
        fields.push_back(f.flag);
    CHECK(fields == std::vector<std::string>{"mode", "group"});
}

TEST_CASE("duplicateWithTransform repeats the last pair's live delta")
{
    run("newScene");
    std::string c = run("polyCube", {{"name", "Fence"}})[0];
    CHECK_THROWS_AS(run("duplicateWithTransform"), CommandError);
    std::string a = run("duplicate", {{"args", {c}}})[0];
    run("move", {{"args", {a}}, {"by", {120, 0, 0}}});
    run("rotate", {{"args", {a}}, {"by", {0, 10, 0}}});
    // The step is the copy's whole change of world matrix, turn included, so a turned step bends the run into an arc.
    const glm::dmat4 step = glm::translate(glm::dmat4(1.0), glm::dvec3(120, 0, 0)) * glm::rotate(glm::dmat4(1.0), glm::radians(10.0), glm::dvec3(0, 1, 0));
    // Selection changes don't lose the record.
    run("select", {{"args", {c}}});
    json next = run("duplicateWithTransform");
    REQUIRE(next.size() == 1);
    checkNear(worldPos(next[0]), glm::dvec3((step * step)[3]));
    CHECK(get(next[0].get<std::string>() + ".ry") == doctest::Approx(20));
    json more = run("duplicateWithTransform", {{"count", 2}});
    REQUIRE(more.size() == 2);
    checkNear(worldPos(more[1]), glm::dvec3((step * step * step * step)[3]));
    CHECK(get(more[1].get<std::string>() + ".ry") == doctest::Approx(40));

    // Editing the newest copy changes the step the next repeat uses.
    run("newScene");
    c = run("polyCube", {{"name", "Rail"}})[0];
    a = run("duplicate", {{"args", {c}}})[0];
    run("move", {{"args", {a}}, {"by", {100, 0, 0}}});
    json third = run("duplicateWithTransform");
    checkNear(worldPos(third[0]), {200, 0, 0});
    run("move", {{"args", {third[0]}}, {"by", {0, 10, 0}}});
    json bent = run("duplicateWithTransform");
    checkNear(worldPos(bent[0]), {300, 20, 0});

    // The mode of the last duplicate carries over.
    std::string i = run("duplicate", {{"args", {c}}, {"mode", "instance"}})[0];
    run("move", {{"args", {i}}, {"by", {0, 0, 30}}});
    json inst = run("duplicateWithTransform");
    CHECK(kind(inst[0]) == "instance");

    // Deleting either object of the pair forgets it.
    run("delete", {{"args", {inst[0]}}});
    CHECK(scene::duplicate::lastPairs().empty());
    CHECK_THROWS_AS(run("duplicateWithTransform"), CommandError);
}

TEST_CASE("a clone drag on faces extrudes them, and the swap pref moves cloning to Shift and fine control to Y")
{
    auto topFace = [](const std::string& mesh) {
        const geo::Mesh m = meshOf(mesh);
        geo::Index best = geo::kInvalid;
        for (geo::Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f) && (best == geo::kInvalid || m.faceCentroid(f).y > m.faceCentroid(best).y))
                best = f;
        return best;
    };
    // Looking down -Z, 1 px per cm, the world origin at pixel (50, 50).
    ui::ViewInfo view;
    view.proj = glm::ortho(-50.0, 50.0, -50.0, 50.0, -100.0, 100.0);
    view.width = view.height = 100.0;
    view.ortho = true;
    view.eye = {0.0, 0.0, 50.0};
    ui::tools::setCurrent(ui::Tool::Move);

    for (const bool swapped : {false, true})
    {
        CAPTURE(swapped);
        PrefStore::get().set("general", "/manipulator/swapCloneShift", swapped);
        const Qt::KeyboardModifiers cloneMods = swapped ? Qt::ShiftModifier : Qt::NoModifier;
        const Qt::KeyboardModifiers fineMods = swapped ? Qt::NoModifier : Qt::ShiftModifier;
        // This view draws world +Y down the screen; the first drag event only starts the drag.
        auto dragTop = [&](double top, Qt::KeyboardModifiers mods, bool yHeld) {
            ui::tools::setCloneDrag(yHeld);
            ui::Manipulator m;
            m.setView(view);
            REQUIRE(m.visible());
            const int pivotY = int(std::lround(50.0 + top));
            REQUIRE(m.press({50, double(pivotY)}, Qt::LeftButton, mods));
            m.drag({50, double(pivotY + 10)}, mods);
            m.drag({50, double(pivotY + 20)}, mods);
            m.release();
            ui::tools::setCloneDrag(false);
        };

        run("newScene");
        const std::string c = run("polyCube", {{"name", "Block"}})[0];
        const double top = meshOf(c).bounds().max.y;
        scene::components::setMode(scene::ComponentMode::Face);
        run("selectComponents", {{"args", {c}}, {"faces", {topFace(c)}}});
        dragTop(top, cloneMods, !swapped);
        CHECK(faces(c) == 10);
        CHECK(meshOf(c).bounds().max.y == doctest::Approx(top + 20.0).epsilon(1e-6));
        // The extrude and its move are one step.
        run("undo");
        CHECK(faces(c) == 6);
        CHECK(meshOf(c).bounds().max.y == doctest::Approx(top));

        // Fine control: the face moves a tenth as far and nothing extrudes.
        run("selectComponents", {{"args", {c}}, {"faces", {topFace(c)}}});
        dragTop(top, fineMods, swapped);
        CHECK(faces(c) == 6);
        CHECK(meshOf(c).bounds().max.y == doctest::Approx(top + 2.0).epsilon(1e-6));
    }
    PrefStore::get().set("general", "/manipulator/swapCloneShift", false);
    scene::components::setMode(scene::ComponentMode::Object);
}

TEST_CASE("a clone drag on the manipulator moves copies and leaves the originals")
{
    run("newScene");
    std::string c = run("polyCube", {{"name", "Crate"}})[0];
    run("select", {{"args", {c}}});
    ui::tools::setCurrent(ui::Tool::Move);
    ui::tools::setCloneDrag(true);
    // Looking down -Z, 1 px per cm, the world origin at pixel (50, 50).
    ui::ViewInfo view;
    view.proj = glm::ortho(-50.0, 50.0, -50.0, 50.0, -100.0, 100.0);
    view.width = view.height = 100.0;
    view.ortho = true;
    view.eye = {0.0, 0.0, 50.0};
    ui::Manipulator m;
    m.setView(view);
    REQUIRE(m.visible());
    REQUIRE(m.press({50, 50}, Qt::LeftButton, Qt::NoModifier));
    m.drag({80, 50}, Qt::NoModifier);
    m.release();
    ui::tools::setCloneDrag(false);

    CHECK(count("mesh") == 2);
    CHECK(get(c + ".tx") == doctest::Approx(0));
    json sel = run("ls", {{"selection", true}});
    REQUIRE(sel.size() == 1);
    CHECK(sel[0] != c);
    CHECK(get(sel[0].get<std::string>() + ".tx") == doctest::Approx(30).epsilon(1e-6));
    // The pair is recorded, so Duplicate with Transform carries on from the drag.
    json next = run("duplicateWithTransform");
    CHECK(get(next[0].get<std::string>() + ".tx") == doctest::Approx(60).epsilon(1e-6));
    run("undo");
    // The clone and its move are one step.
    run("undo");
    CHECK(count("mesh") == 1);

    // A clone drag that goes nowhere leaves nothing behind.
    ui::tools::setCloneDrag(true);
    run("select", {{"args", {c}}});
    REQUIRE(m.press({50, 50}, Qt::LeftButton, Qt::NoModifier));
    m.release();
    ui::tools::setCloneDrag(false);
    CHECK(count("mesh") == 1);
}
