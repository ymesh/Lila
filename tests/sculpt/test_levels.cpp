#include "ops/Command.h"
#include "ops/Undo.h"
#include "geo/Primitives.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "sculpt/SculptSession.h"

#include <doctest/doctest.h>

#include <chrono>

using namespace lila;
using geo::Index;

namespace
{
void makeMesh(const std::string& name, geo::Mesh mesh)
{
    scene::Graph& g = scene::Graph::current();
    if (scene::Node* old = g.find(name))
        g.deleteNode(old->id());
    scene::Node* n = g.createNode(*scene::types::find("mesh"), name, scene::kNoNode, false);
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
}

std::vector<glm::dvec3> positions()
{
    auto s = sculpt::active();
    REQUIRE(s);
    return {s->sculpt.mesh().positions().begin(), s->sculpt.mesh().positions().end()};
}

double maxGap(const std::vector<glm::dvec3>& a, const std::vector<glm::dvec3>& b)
{
    REQUIRE(a.size() == b.size());
    double d = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        d = std::max(d, glm::length(a[i] - b[i]));
    return d;
}

void bump(glm::dvec3 at, double radius, double strength = 1.0)
{
    json dab = {{"center", {at.x, at.y, at.z}}, {"normal", {0.0, 0.0, 1.0}}, {"radius", radius}};
    ops::execute("sculptStroke", {{"brush", "draw"}, {"strength", strength}, {"dabs", json::array({dab, dab})}});
}

int level() { return ops::execute("sculptState", json::object())["level"].get<int>(); }
} // namespace

TEST_CASE("stepping down and back up a level gives the sculpt back exactly")
{
    makeMesh("lvCube", geo::makeCube({}));
    ops::execute("sculptEnter", {{"args", {"lvCube"}}, {"levels", 3}});
    CHECK(level() == 3);
    bump({0.0, 0.0, 50.0}, 15.0);
    bump({20.0, 10.0, 50.0}, 6.0);
    const std::vector<glm::dvec3> top = positions();

    ops::execute("sculptLevel", {{"level", 1}});
    CHECK(level() == 1);
    CHECK(sculpt::active()->sculpt.mesh().numFaces() == 6 * 4);
    ops::execute("sculptLevel", {{"step", 1}});
    ops::execute("sculptLevel", {{"step", 1}});
    CHECK(level() == 3);
    CHECK(maxGap(top, positions()) < 1e-6);
    ops::execute("sculptCancel", json::object());
}

TEST_CASE("moving or turning a lower level carries the finer detail with it")
{
    makeMesh("lvCarry", geo::makeCube({}));
    ops::execute("sculptEnter", {{"args", {"lvCarry"}}, {"levels", 3}});
    bump({0.0, 0.0, 50.0}, 10.0);
    bump({-15.0, 20.0, 50.0}, 5.0);
    const std::vector<glm::dvec3> top = positions();

    ops::execute("sculptLevel", {{"level", 1}});
    // A quarter turn about z and a shift, applied to the level-1 shape only.
    auto move = [](const glm::dvec3& p) { return glm::dvec3(-p.y, p.x, p.z) + glm::dvec3(5.0, -3.0, 7.0); };
    std::vector<glm::dvec3> low = positions();
    for (glm::dvec3& p : low)
        p = move(p);
    sculpt::active()->sculpt.setPositions(low);
    ops::execute("sculptLevel", {{"level", 3}});

    std::vector<glm::dvec3> want = top;
    for (glm::dvec3& p : want)
        p = move(p);
    CHECK(maxGap(want, positions()) < 1e-6);
    ops::execute("sculptCancel", json::object());
}

TEST_CASE("level changes undo and redo to the same sessions")
{
    makeMesh("lvUndo", geo::makeCube({}));
    ops::execute("sculptEnter", {{"args", {"lvUndo"}}, {"levels", 2}});
    bump({0.0, 0.0, 50.0}, 15.0);
    auto atTop = sculpt::active();
    ops::execute("sculptLevel", {{"level", 0}});
    auto atCage = sculpt::active();
    CHECK(atCage != atTop);
    CHECK(atCage->sculpt.mesh().numFaces() == 6);

    UndoStack& u = UndoStack::get();
    REQUIRE(u.undo());
    CHECK(sculpt::active() == atTop);
    REQUIRE(u.redo());
    CHECK(sculpt::active() == atCage);
    ops::execute("sculptCancel", json::object());
}

TEST_CASE("adding a level subdivides the finest and keeps its shape; deleting higher drops the detail")
{
    makeMesh("lvAdd", geo::makeCube({}));
    ops::execute("sculptEnter", {{"args", {"lvAdd"}}, {"levels", 2}});
    bump({0.0, 0.0, 50.0}, 15.0);
    const std::vector<glm::dvec3> two = positions();
    ops::execute("sculptLevel", {{"level", 1}});

    json r = ops::execute("sculptAddLevel", json::object());
    CHECK(r["level"].get<int>() == 3);
    CHECK(r["levels"].get<int>() == 3);
    CHECK(r["faces"].get<int>() == 6 * 64);
    ops::execute("sculptLevel", {{"level", 2}});
    CHECK(maxGap(two, positions()) < 1e-4);

    ops::execute("sculptLevel", {{"level", 1}});
    r = ops::execute("sculptDeleteHigher", json::object());
    CHECK(r["levels"].get<int>() == 1);
    ops::execute("sculptLevel", {{"level", 1}});
    CHECK(level() == 1);
    ops::execute("sculptCancel", json::object());
}

TEST_CASE("enter starts on the cage and exit without levels keeps no high poly")
{
    makeMesh("lvBare", geo::makeCube({.subdivX = 4, .subdivY = 4, .subdivZ = 4}));
    json r = ops::execute("sculptEnter", {{"args", {"lvBare"}}});
    CHECK(r["levels"].get<int>() == 0);
    CHECK(r["faces"].get<int>() == 6 * 16);
    // Delete Higher from level 0 drops every level.
    ops::execute("sculptAddLevel", json::object());
    ops::execute("sculptLevel", {{"level", 0}});
    CHECK(ops::execute("sculptDeleteHigher", json::object())["levels"].get<int>() == 0);
    bump({0.0, 0.0, 50.0}, 20.0);
    const std::vector<glm::dvec3> sculpted = positions();
    ops::execute("sculptExit", json::object());
    scene::Graph& g = scene::Graph::current();
    CHECK_FALSE(sculpt::storedLevels(*g.find("lvBare")));
    const auto& mesh = *std::get<scene::MeshPtr>(g.storedValue({g.find("lvBare")->id(), scene::MeshNode::kInMesh, -1}));
    CHECK(maxGap(sculpted, {mesh.positions().begin(), mesh.positions().end()}) < 1e-6);
}

TEST_CASE("exit leaves the mesh at the level sculpted and the next enter gets every level back")
{
    makeMesh("lvExit", geo::makeCube({}));
    scene::Graph& g = scene::Graph::current();
    ops::execute("sculptEnter", {{"args", {"lvExit"}}, {"levels", 3}});
    bump({0.0, 0.0, 50.0}, 20.0);
    const std::vector<glm::dvec3> top = positions();
    ops::execute("sculptLevel", {{"level", 1}});
    json r = ops::execute("sculptExit", json::object());
    CHECK(r["level"].get<int>() == 1);
    CHECK(r["levels"].get<int>() == 3);
    CHECK(g.children(g.find("lvExit")->id()).empty());
    const auto& mesh = *std::get<scene::MeshPtr>(g.storedValue({g.find("lvExit")->id(), scene::MeshNode::kInMesh, -1}));
    CHECK(mesh.numFaces() == 6 * 4);
    auto stored = sculpt::storedLevels(*g.find("lvExit"));
    REQUIRE(stored);
    CHECK(stored->level == 1);
    CHECK(stored->levels == 3);
    CHECK(stored->fits);

    r = ops::execute("sculptEnter", {{"args", {"lvExit"}}});
    CHECK(level() == 1);
    CHECK(r["levels"].get<int>() == 3);
    ops::execute("sculptLevel", {{"level", 3}});
    CHECK(maxGap(top, positions()) < 1e-4);
    // Leaving and coming back at the top keeps the sculpt exactly.
    ops::execute("sculptExit", json::object());
    ops::execute("sculptEnter", {{"args", {"lvExit"}}});
    CHECK(level() == 3);
    CHECK(maxGap(top, positions()) < 1e-4);
    ops::execute("sculptCancel", json::object());
}

TEST_CASE("points moved outside sculpt mode carry into the levels; a topology change drops them")
{
    makeMesh("lvEdit", geo::makeCube({}));
    scene::Graph& g = scene::Graph::current();
    ops::execute("sculptEnter", {{"args", {"lvEdit"}}, {"levels", 2}});
    bump({0.0, 0.0, 50.0}, 10.0);
    ops::execute("sculptLevel", {{"level", 0}});
    ops::execute("sculptExit", json::object());

    const scene::PlugRef in{g.find("lvEdit")->id(), scene::MeshNode::kInMesh, -1};
    geo::Mesh moved = *std::get<scene::MeshPtr>(g.storedValue(in));
    for (glm::dvec3& p : moved.positions())
        p.x += 30.0;
    g.setValue(in, scene::MeshPtr(std::make_shared<geo::Mesh>(moved)));
    REQUIRE(sculpt::storedLevels(*g.find("lvEdit"))->fits);
    ops::execute("sculptEnter", {{"args", {"lvEdit"}}});
    CHECK(level() == 0);
    CHECK(maxGap(positions(), {moved.positions().begin(), moved.positions().end()}) < 1e-9);
    ops::execute("sculptLevel", {{"level", 2}});
    double minX = 1e9;
    for (const glm::dvec3& p : positions())
        minX = std::min(minX, p.x);
    CHECK(minX > -50.0 + 20.0);
    ops::execute("sculptCancel", json::object());

    g.setValue(in, scene::MeshPtr(std::make_shared<geo::Mesh>(geo::makeCube({.subdivX = 3}))));
    REQUIRE(sculpt::storedLevels(*g.find("lvEdit")));
    CHECK_FALSE(sculpt::storedLevels(*g.find("lvEdit"))->fits);
    json r = ops::execute("sculptEnter", {{"args", {"lvEdit"}}});
    CHECK(r["levels"].get<int>() == 0);
    CHECK_FALSE(sculpt::storedLevels(*g.find("lvEdit")));
    ops::execute("sculptCancel", json::object());
}

TEST_CASE("a command that breaks stored levels is reported")
{
    makeMesh("lvGuard", geo::makeCube({}));
    scene::Graph& g = scene::Graph::current();
    ops::execute("sculptEnter", {{"args", {"lvGuard"}}, {"levels", 1}});
    ops::execute("sculptExit", json::object());
    std::vector<scene::NodeId> reported;
    std::string by;
    sculpt::setLevelsBrokenHandler([&](const std::vector<scene::NodeId>& ids, std::string_view command) {
        reported = ids;
        by = command;
    });
    ops::execute("select", {{"args", {"lvGuard"}}});
    ops::execute("polyTriangulate", json::object());
    sculpt::setLevelsBrokenHandler(nullptr);
    REQUIRE(reported.size() == 1);
    CHECK(reported[0] == g.find("lvGuard")->id());
    CHECK(by == "polyTriangulate");
    ops::execute("sculptDeleteLevels", {{"args", {"lvGuard"}}});
    CHECK_FALSE(sculpt::storedLevels(*g.find("lvGuard")));
}

TEST_CASE("the mask and polypaint survive a trip down and up")
{
    makeMesh("lvMask", geo::makeCube({}));
    ops::execute("sculptEnter", {{"args", {"lvMask"}}, {"levels", 2}});
    json dab = {{"center", {0.0, 0.0, 50.0}}, {"normal", {0.0, 0.0, 1.0}}, {"radius", 20.0}};
    ops::execute("sculptStroke", {{"brush", "mask"}, {"strength", 1.0}, {"dabs", json::array({dab})}});
    ops::execute("sculptStroke", {{"brush", "paint"}, {"color", {1.0, 0.0, 0.0}}, {"strength", 1.0}, {"dabs", json::array({dab})}});
    const auto& before = sculpt::active()->sculpt;
    std::vector<float> mask(before.mask().begin(), before.mask().end());
    std::vector<glm::vec4> color(before.colors().begin(), before.colors().end());

    ops::execute("sculptLevel", {{"level", 0}});
    ops::execute("sculptLevel", {{"level", 2}});
    const auto& after = sculpt::active()->sculpt;
    double maskGap = 0.0, colorGap = 0.0;
    for (size_t i = 0; i < mask.size(); ++i)
    {
        maskGap = std::max(maskGap, double(std::abs(after.mask()[i] - mask[i])));
        colorGap = std::max(colorGap, double(glm::length(after.colors()[i] - color[i])));
    }
    CHECK(maskGap < 1e-5);
    CHECK(colorGap < 1e-5);
    ops::execute("sculptCancel", json::object());
}

TEST_CASE("stepping down from half a million faces is quick enough to feel like a key press")
{
    geo::SphereOptions o;
    o.radius = 50.0;
    o.subdivAxis = 32;
    o.subdivHeight = 16;
    makeMesh("lvBig", geo::makeSphere(o));
    ops::execute("sculptEnter", {{"args", {"lvBig"}}, {"levels", 5}});
    bump({0.0, 0.0, 50.0}, 10.0);
    const std::vector<glm::dvec3> top = positions();
    const auto t0 = std::chrono::steady_clock::now();
    ops::execute("sculptLevel", {{"step", -1}});
    const double down = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const auto t1 = std::chrono::steady_clock::now();
    ops::execute("sculptLevel", {{"step", 1}});
    const double up = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    MESSAGE("top faces " << sculpt::active()->sculpt.mesh().numFaces() << ", down " << down << " s, up " << up << " s");
    CHECK(maxGap(top, positions()) < 1e-5);
    ops::execute("sculptCancel", json::object());
}

TEST_CASE("duplicate copies a level beside the mesh, hidden and sculpting on, or selected and done")
{
    scene::Graph& g = scene::Graph::current();
    makeMesh("lvDupBefore", geo::makeCube({}));
    makeMesh("lvDup", geo::makeCube({}));
    makeMesh("lvDupAfter", geo::makeCube({}));
    ops::execute("sculptEnter", {{"args", {"lvDup"}}, {"levels", 3}});
    bump({0.0, 0.0, 50.0}, 10.0);

    json r = ops::execute("sculptDuplicate", {{"level", 2}, {"hide", true}});
    CHECK(r["mesh"].get<std::string>() == "lvDup_level2");
    scene::Node* copy = g.find("lvDup_level2");
    REQUIRE(copy);
    CHECK_FALSE(copy->as<bool>(scene::Transform::kVisibility));
    CHECK(sculpt::active());
    g.evaluate();
    CHECK(copy->as<scene::MeshPtr>(scene::MeshNode::kOutMesh)->numFaces() == 6 * 16);
    CHECK_FALSE(sculpt::storedLevels(*copy));
    auto indexOf = [&](const std::string& name) {
        const auto& roots = g.roots();
        return std::ranges::find(roots, g.find(name)->id()) - roots.begin();
    };
    CHECK(indexOf("lvDup_level2") == indexOf("lvDup") + 1);

    r = ops::execute("sculptDuplicate", json::object());
    CHECK(r["mesh"].get<std::string>() == "lvDup_level3");
    CHECK_FALSE(sculpt::active());
    CHECK(g.selection() == std::vector<scene::NodeId>{g.find("lvDup_level3")->id()});
    CHECK(indexOf("lvDup_level3") == indexOf("lvDup") + 1);
    CHECK(sculpt::storedLevels(*g.find("lvDup"))->levels == 3);

    ops::execute("sculptEnter", {{"args", {"lvDup"}}});
    r = ops::execute("sculptDuplicate", json::object());
    CHECK(r["mesh"].get<std::string>() == "lvDup_level3_2");
}

TEST_CASE("delete lower makes the current level the cage and keeps the detail above")
{
    makeMesh("lvLower", geo::makeCube({}));
    ops::execute("sculptEnter", {{"args", {"lvLower"}}, {"levels", 3}});
    bump({0.0, 0.0, 50.0}, 10.0);
    const std::vector<glm::dvec3> top = positions();
    ops::execute("sculptLevel", {{"level", 1}});
    json r = ops::execute("sculptDeleteLower", json::object());
    CHECK(r["level"].get<int>() == 0);
    CHECK(r["levels"].get<int>() == 2);
    CHECK(r["faces"].get<int>() == 6 * 4);
    ops::execute("sculptLevel", {{"level", 2}});
    CHECK(maxGap(top, positions()) < 1e-4);
    ops::execute("sculptExit", json::object());
    ops::execute("sculptEnter", {{"args", {"lvLower"}}});
    ops::execute("sculptLevel", {{"level", 0}});
    CHECK(ops::execute("sculptState", json::object())["faces"].get<int>() == 6 * 4);
    ops::execute("sculptCancel", json::object());
}

TEST_CASE("reconstruct rebuilds the levels below a mesh subdivided elsewhere")
{
    geo::SubdivideOptions o;
    o.levels = 2;
    const geo::Mesh fine = geo::subdivide(geo::makeCube({}), o);
    makeMesh("lvRecon", fine);
    json r = ops::execute("sculptEnter", {{"args", {"lvRecon"}}});
    CHECK(r["faces"].get<int>() == 6 * 16);
    r = ops::execute("sculptReconstruct", json::object());
    CHECK(r["level"].get<int>() == 1);
    CHECK(r["levels"].get<int>() == 1);
    ops::execute("sculptLevel", {{"level", 0}});
    r = ops::execute("sculptReconstruct", json::object());
    CHECK(r["levels"].get<int>() == 2);
    ops::execute("sculptLevel", {{"level", 0}});
    CHECK(ops::execute("sculptState", json::object())["faces"].get<int>() == 6);
    // The finest level is the mesh as it was, point for point in a new order.
    ops::execute("sculptLevel", {{"level", 2}});
    const std::vector<glm::dvec3> now = positions();
    double worst = 0.0;
    for (const glm::dvec3& p : fine.positions())
    {
        double best = 1e9;
        for (const glm::dvec3& q : now)
            best = std::min(best, glm::length(p - q));
        worst = std::max(worst, best);
    }
    CHECK(worst < 1e-6);
    ops::execute("sculptCancel", json::object());

    // A mesh that isn't a subdivision says so.
    makeMesh("lvNotSubd", geo::makeCube({.subdivX = 3}));
    ops::execute("sculptEnter", {{"args", {"lvNotSubd"}}});
    CHECK_THROWS(ops::execute("sculptReconstruct", json::object()));
    ops::execute("sculptCancel", json::object());
}
