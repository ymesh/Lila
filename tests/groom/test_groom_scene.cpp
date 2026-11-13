#include <doctest/doctest.h>

#include "curve/TestUtil.h"

#include "curve/CurveNodes.h"
#include "groom/Attrs.h"
#include "groom/GroomNodes.h"
#include "io/SceneFile.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <glm/gtc/constants.hpp>

#include <filesystem>

using namespace lila;
using json = nlohmann::json;
using D3 = glm::dvec3;
using curvetest::run;

namespace
{
void reset()
{
    scene::Graph::current().clear();
    UndoStack::get().clear();
}

scene::MeshPtr outMesh(const std::string& path)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(path);
    REQUIRE(n);
    return n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}

const curve::Splines& guides(const std::string& path) { return curve::shapeSplines(scene::Graph::current(), curvetest::idOf(path)); }

json strokePoints(double azimuth, int n = 12, double len = 14.0)
{
    json p = json::array();
    for (int i = 0; i < n; ++i)
    {
        const double polar = 0.2 + (len / 10.0) * i / (n - 1);
        p.push_back({10.3 * std::sin(polar) * std::cos(azimuth), 10.3 * std::cos(polar), 10.3 * std::sin(polar) * std::sin(azimuth)});
    }
    return p;
}

/** One brush dab at p, as the Sculpt Curves tool records it. */
json strokeDab(const D3& p, double radius)
{
    json d = {{"center", json::array({p.x, p.y, p.z})}, {"radius", radius}, {"strength", 1.0}};
    return json::array({d});
}

/** A fresh groom on a radius 10 sphere. */
json makeGroom()
{
    reset();
    const std::string head = run("polySphere", {{"name", "Head"}, {"radius", 10}, {"subdivisionsAxis", 40}, {"subdivisionsHeight", 30}})[0];
    return run("createGroom", {{"args", {head}}, {"symmetry", "off"}});
}
} // namespace

TEST_CASE("createGroom: guides, groom node and locks mesh wired up; empty until drawn")
{
    const json made = makeGroom();
    const std::string groom = made["groom"], guide = made["guides"], locks = made["locks"];
    scene::Graph& g = scene::Graph::current();
    REQUIRE(g.find(groom));
    CHECK(groom::groomSurfaceOf(*g.find(guide)) != "");
    CHECK(groom::findGroom(g, *g.find(locks)) == g.find(groom));
    CHECK(groom::findGroom(g, *g.find(guide)) == g.find(groom));
    scene::MeshPtr m = outMesh(locks);
    CHECK((!m || m->numFaces() == 0));

    const json ids = run("groomDraw", {{"args", {guide}}, {"points", strokePoints(0.0)}});
    REQUIRE(ids.size() == 1);
    m = outMesh(locks);
    REQUIRE(m);
    CHECK(m->numFaces() > 50);
    const curve::Splines& s = guides(guide);
    REQUIRE(s.size() == 1);
    // The root snapped onto the scalp.
    CHECK(glm::length(s[0].points.front()) == doctest::Approx(10.0).epsilon(0.01));
    CHECK(groom::attr::get(s[0], groom::attr::kId, 0.0) == doctest::Approx(1.0));

    UndoStack::get().undo();
    CHECK(guides(guide).empty());
    UndoStack::get().redo();
    CHECK(guides(guide).size() == 1);
}

TEST_CASE("groomDraw shapes, styles in the stack, reorder, toggle and remove")
{
    const json made = makeGroom();
    const std::string groom = made["groom"], guide = made["guides"], locks = made["locks"];
    run("groomDraw", {{"args", {groom}}, {"points", strokePoints(0.0)}, {"shape", "braid"}});
    CHECK(guides(guide).size() == 3);
    run("groomDraw", {{"args", {groom}}, {"points", strokePoints(1.0)}});
    const size_t facesBefore = outMesh(locks)->numFaces();

    const std::string split = run("groomAddStyle", {{"args", {groom}}, {"style", "split"}, {"count", 3}});
    const std::string curl = run("groomAddStyle", {{"args", {groom}}, {"style", "curl"}, {"radius", 0.8}});
    json stack = run("groomStack", {{"args", {groom}}});
    REQUIRE(stack.size() == 2);
    CHECK(stack[0]["style"] == "split");
    CHECK(stack[1]["style"] == "curl");
    // Split makes three locks of each.
    CHECK(outMesh(locks)->numFaces() > facesBefore * 2);
    stack = run("groomStack", {{"args", {groom}}, {"edit", "move"}, {"node", curl}, {"to", 0}});
    CHECK(stack[0]["style"] == "curl");
    stack = run("groomStack", {{"args", {groom}}, {"edit", "toggle"}, {"node", split}});
    CHECK(stack[1]["enabled"] == false);
    CHECK(outMesh(locks)->numFaces() < facesBefore * 2);
    stack = run("groomStack", {{"args", {groom}}, {"edit", "remove"}, {"node", curl}});
    CHECK(stack.size() == 1);
    CHECK(outMesh(locks)->numFaces() == facesBefore);
}

TEST_CASE("groomSettle, groomPart, groomPreset and lock brushes through curveSculptStroke")
{
    const json made = makeGroom();
    const std::string groom = made["groom"], guide = made["guides"];
    // A lock standing straight up off the crown.
    json up = json::array();
    for (int i = 0; i < 10; ++i)
        up.push_back({0.5, 10.0 + i * 1.2, 0.0});
    run("groomDraw", {{"args", {groom}}, {"points", up}});
    const double tipBefore = guides(guide)[0].points.back().y;
    const json st = run("groomSettle", {{"args", {groom}}, {"holdShape", 0.0}, {"rootStiffness", 0.1}, {"bend", 0.1}, {"steps", 200}});
    CHECK(st["curves"] == 1);
    CHECK(guides(guide)[0].points.back().y < tipBefore - 3.0);
    CHECK(st["penetration"].get<double>() < 0.05);

    const json ids = run("groomPreset", {{"args", {groom}}, {"preset", "ponytail"}, {"at", {0, 2, -10}}, {"count", 8}});
    CHECK(ids.size() == 8);
    CHECK(guides(guide).size() == 9);

    const json n = run("groomPart", {{"args", {groom}}, {"points", {{-5, 9, 0}, {5, 9, 0}}}, {"radius", 20.0}});
    CHECK(n.get<int>() >= 1);

    const size_t before = guides(guide).size();
    const D3 tip = guides(guide).back().points.back();
    run("curveSculptStroke", {{"args", {guide}}, {"brush", "split"}, {"dabs", strokeDab(tip, 1.0)}});
    CHECK(guides(guide).size() == before + 1);
    run("curveSculptStroke", {{"args", {guide}}, {"brush", "curl"}, {"dabs", strokeDab(tip, 3.0)}});
    bool painted = false;
    for (const curve::Spline& s : guides(guide))
        painted = painted || groom::attr::point(s, groom::attr::kCurl, s.size() - 1, 0.0) > 0.0;
    CHECK(painted);
}

TEST_CASE("groomShell: one live mesh over the locks; scene round trip keeps the groom attrs")
{
    const json made = makeGroom();
    const std::string groom = made["groom"], guide = made["guides"];
    for (int k = 0; k < 5; ++k)
        run("groomDraw", {{"args", {groom}}, {"points", strokePoints(k * 0.35)}, {"width", 1.4}});
    const json shell = run("groomShell", {{"args", {groom}}, {"voxelSize", 0.35}, {"blend", 0.8}});
    scene::MeshPtr m = outMesh(shell[0]);
    REQUIRE(m);
    CHECK(m->numFaces() > 100);
    for (geo::Index e = 0; e < m->edgeCapacity(); ++e)
        if (m->edgeValid(e))
            REQUIRE(m->edgeFaceCount(e) == 2);

    const auto path = std::filesystem::temp_directory_path() / "lila_groom_roundtrip.lila";
    run("saveScene", {{"args", {path.string()}}});
    reset();
    run("openScene", {{"args", {path.string()}}});
    const curve::Splines& s = guides(guide);
    REQUIRE(s.size() == 5);
    CHECK(groom::attr::get(s[4], groom::attr::kId, 0.0) == doctest::Approx(5.0));
    // A lock's width is its width scale times each point's radius: drawn at 1.4, the guide holds 1.4 at its root.
    CHECK(groom::attr::get(s[4], groom::attr::kWidth, 0.0) * s[4].radius.front() == doctest::Approx(1.4).epsilon(0.02));
    CHECK(outMesh(shell[0])->numFaces() > 100);
}

TEST_CASE("groomShell quads: the shell quad remeshed for retopology in the same step")
{
    const json made = makeGroom();
    const std::string groom = made["groom"];
    for (int k = 0; k < 5; ++k)
        run("groomDraw", {{"args", {groom}}, {"points", strokePoints(k * 0.35)}, {"width", 1.4}});
    const json shell = run("groomShell", {{"args", {groom}}, {"voxelSize", 0.35}, {"blend", 0.8}, {"quads", 1500}});
    REQUIRE(shell.size() == 3);
    scene::MeshPtr q = outMesh(shell[2]["name"]);
    REQUIRE(q);
    size_t quads = 0, faces = 0;
    for (geo::Index f = 0; f < q->faceCapacity(); ++f)
        if (q->faceValid(f))
        {
            ++faces;
            quads += q->facePoints(f).size() == 4;
        }
    CHECK(faces > 500);
    CHECK(quads > faces * 9 / 10);
}

TEST_CASE("presets: Lila's are listed; a saved preset brings back the stack, lock settings and guides on another head")
{
    const json made = makeGroom();
    const std::string groom = made["groom"], guide = made["guides"];
    bool chunky = false;
    for (const json& p : run("groomPresets"))
        chunky = chunky || p["id"] == "lila/chunky";
    CHECK(chunky);
    run("groomLoadPreset", {{"args", {groom}}, {"preset", "lila/chunky"}});
    json stack = run("groomStack", {{"args", {groom}}});
    REQUIRE(stack.size() == 4);
    CHECK(stack[0]["style"] == "split");

    for (int k = 0; k < 3; ++k)
        run("groomDraw", {{"args", {groom}}, {"points", strokePoints(k * 0.4)}});
    run("setAttr", {{"args", {groom + ".width", 3.5}}});
    const std::string id = run("groomSavePreset", {{"args", {groom}}, {"name", "Test Bob"}, {"guides", true}});
    CHECK(id == "user/Test_Bob");

    // A head twice the size: the guides stretch to it and root on it.
    reset();
    const std::string big = run("polySphere", {{"name", "Big"}, {"radius", 20}, {"subdivisionsAxis", 40}, {"subdivisionsHeight", 30}})[0];
    const json other = run("createGroom", {{"args", {big}}, {"symmetry", "off"}});
    CHECK(run("groomLoadPreset", {{"args", {other["groom"]}}, {"preset", id}}) == 3);
    const curve::Splines& s = guides(other["guides"]);
    REQUIRE(s.size() == 3);
    for (const curve::Spline& sp : s)
        CHECK(glm::length(sp.points.front()) == doctest::Approx(20.0).epsilon(0.02));
    scene::Graph& g = scene::Graph::current();
    scene::Node* gr = g.find(other["groom"].get<std::string>());
    CHECK(gr->as<double>(gr->type().attrIndex("width")) == doctest::Approx(3.5));
    CHECK(run("groomStack", {{"args", {other["groom"]}}}).size() == 4);
}

TEST_CASE("groomEditLock sets a guide's width and twist; grab with settle moves the neighbours out of the way")
{
    const json made = makeGroom();
    const std::string groom = made["groom"], guide = made["guides"];
    run("groomDraw", {{"args", {groom}}, {"points", strokePoints(0.0)}});
    run("groomDraw", {{"args", {groom}}, {"points", strokePoints(0.15)}});
    run("groomEditLock", {{"args", {guide}}, {"index", 1}, {"width", 2.0}, {"twist", 90.0}});
    const curve::Splines& s = guides(guide);
    // Its width scale goes into its points' radius: twice the other lock's, drawn the same.
    CHECK(s[1].radius.front() * groom::attr::get(s[1], groom::attr::kWidth, 1.0) ==
          doctest::Approx(2.0 * s[0].radius.front() * groom::attr::get(s[0], groom::attr::kWidth, 1.0)).epsilon(0.02));
    REQUIRE(s[1].tilt.size() == s[1].size());
    CHECK(s[1].tilt.back() == doctest::Approx(glm::radians(90.0)));

    const curve::Splines before = guides(guide);
    const D3 mid = before[0].points[before[0].size() / 2];
    json dabs = json::array();
    for (int k = 0; k < 6; ++k)
        dabs.push_back({{"center", {mid.x, mid.y, mid.z}}, {"radius", 1.5}, {"strength", 1.0}, {"delta", {0.0, 0.0, 0.6}}});
    run("curveSculptStroke", {{"args", {guide}}, {"brush", "grab"}, {"dabs", dabs}, {"settleWhileGrabbing", true}, {"frontOnly", false}});
    const curve::Splines& after = guides(guide);
    double grabbed = 0.0, neighbour = 0.0;
    for (size_t i = 0; i < after[0].size(); ++i)
        grabbed = std::max(grabbed, glm::distance(after[0].points[i], before[0].points[i]));
    for (size_t i = 0; i < after[1].size(); ++i)
        neighbour = std::max(neighbour, glm::distance(after[1].points[i], before[1].points[i]));
    CHECK(grabbed > 1.0);
    CHECK(neighbour > 0.01);
}

TEST_CASE("symmetry: brushing the mirrored side edits the guides; a lock on the mirror plane makes one lock, not a pair")
{
    reset();
    const std::string head = run("polySphere", {{"name", "Head"}, {"radius", 10}, {"subdivisionsAxis", 40}, {"subdivisionsHeight", 30}})[0];
    const json made = run("createGroom", {{"args", {head}}, {"symmetry", "x"}});
    const std::string groom = made["groom"], guide = made["guides"];
    // A lock on the +X side; its mirror shows on -X.
    run("groomDraw", {{"args", {groom}}, {"points", strokePoints(0.0)}});
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* gr = g.find(groom);
    CHECK(gr->as<int64_t>(gr->type().attrIndex("lockCount")) == 2);
    const D3 tip = guides(guide)[0].points.back();
    CHECK(tip.x > 1.0);
    run("curveSculptStroke", {{"args", {guide}}, {"brush", "curl"}, {"dabs", strokeDab(D3(-tip.x, tip.y, tip.z), 3.0)}});
    CHECK(groom::attr::point(guides(guide)[0], groom::attr::kCurl, guides(guide)[0].size() - 1, 0.0) > 0.0);

    // Along the plane: its mirror is itself, and the groom keeps one.
    json mid = json::array();
    for (int i = 0; i < 10; ++i)
        mid.push_back({0.0, 10.3 * std::cos(0.2 + 1.2 * i / 9.0), 10.3 * std::sin(0.2 + 1.2 * i / 9.0)});
    run("groomDraw", {{"args", {groom}}, {"points", mid}});
    g.evaluate();
    CHECK(gr->as<int64_t>(gr->type().attrIndex("lockCount")) == 3);
}
