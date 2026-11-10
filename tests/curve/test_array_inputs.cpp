#include <doctest/doctest.h>

#include "TestUtil.h"

#include "geo/Mesh.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace lila;
using json = nlohmann::json;
using namespace curvetest;

namespace
{
size_t facesOf(const std::string& mesh)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(mesh);
    REQUIRE(n);
    const scene::MeshPtr& m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    return m ? m->numFaces() : 0;
}

std::string circleAt(double y) { return run("circle", {{"radius", 10.0}, {"center", {0, y, 0}}}); }

std::string slurp(const std::filesystem::path& p)
{
    std::ifstream f(p);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}
} // namespace

TEST_CASE("array inputs: loft takes any number of sections, appended by connectAttr")
{
    run("newScene");
    std::vector<std::string> rings;
    for (int k = 0; k < 20; ++k)
        rings.push_back(circleAt(10.0 * k));
    json args = json::array();
    for (const std::string& r : rings)
        args.push_back(r);
    json lo = run("loft", {{"args", args}, {"samples", 8}});
    const std::string mesh = lo[0], op = lo[1];
    CHECK(facesOf(mesh) == 8 * 19);
    CHECK(run("getAttr", {{"args", {op + ".sections"}}}).size() == 20);

    // A whole array plug appends; the result names the element made.
    std::string extra = circleAt(200.0);
    CHECK(run("connectAttr", {{"args", {extra + ".worldSpace", op + ".sections"}}}) == op + ".sections[20]");
    CHECK(facesOf(mesh) == 8 * 20);

    // Elements are addressed by index; a gap left by a disconnect keeps the others' order.
    run("disconnectAttr", {{"args", {op + ".sections[5]"}}});
    CHECK(facesOf(mesh) == 8 * 19);
    run("undo");
    CHECK(facesOf(mesh) == 8 * 20);

    // Deleting a section curve drops its element.
    run("delete", {{"args", {rings[0]}}});
    CHECK(facesOf(mesh) == 8 * 19);
    run("undo");
    CHECK(facesOf(mesh) == 8 * 20);

    CHECK_THROWS(run("setAttr", {{"args", {op + ".sections", 1}}}));
    CHECK_THROWS(run("connectAttr", {{"args", {extra + ".worldSpace", op + ".samples[0]"}}}));
}

TEST_CASE("loft joins a half curve and its live symmetry mirror into one closed section")
{
    run("newScene");
    json args = json::array();
    for (double z : {0.0, 10.0, 25.0})
    {
        std::string half = run("curve", {{"type", "bezier"}, {"handleType", "auto"}, {"points", {{0, 10, z}, {10, 0, z}, {0, -10, z}}}});
        run("curveSymmetry", {{"args", {half}}, {"axis", "x"}, {"space", "world"}});
        args.push_back(half);
    }
    json lo = run("loft", {{"args", args}, {"samples", 16}, {"smooth", false}});
    json check = run("meshCheck", {{"args", {lo[0]}}});
    CHECK(facesOf(lo[0]) == 16 * 2);
    CHECK(check["shells"] == 1);
    CHECK(check["nonManifoldEdges"] == 0);
    CHECK(check["borderEdges"] == 16 * 2);
}

TEST_CASE("option boxes built from node attrs show the attrs' defaults without passing them")
{
    json v = ops::options::values("loft");
    CHECK(v["smooth"] == true);
    CHECK(v["autoAlign"] == true);
    CHECK(v["spansBetween"] == 1);
    CHECK(ops::options::callWith("loft").empty());
}

TEST_CASE("array inputs survive save and load, and read the numbered attrs older files used")
{
    run("newScene");
    json lo = run("loft", {{"args", {circleAt(0), circleAt(10), circleAt(25)}}, {"samples", 8}, {"name", "Tube"}});
    const std::string op = lo[1];
    CHECK(facesOf("Tube") == 16);

    auto dir = std::filesystem::temp_directory_path() / "lila_curve_tests";
    std::filesystem::create_directories(dir);
    const auto path = dir / "arrays.lilaa";
    run("saveScene", {{"args", {path.string()}}});
    run("newScene");
    run("openScene", {{"args", {path.string()}}});
    CHECK(facesOf("Tube") == 16);

    // The same scene as an older file wrote it: section0, section1 ... instead of sections[0] ...
    std::string text = slurp(path);
    for (int k = 0; k < 3; ++k)
    {
        const std::string now = ".sections[" + std::to_string(k) + "]", old = ".section" + std::to_string(k);
        size_t at = text.find(now);
        REQUIRE(at != std::string::npos);
        text.replace(at, now.size(), old);
    }
    const auto legacy = dir / "arrays_legacy.lilaa";
    std::ofstream(legacy) << text;
    run("newScene");
    run("openScene", {{"args", {legacy.string()}}});
    CHECK(facesOf("Tube") == 16);
    CHECK(run("getAttr", {{"args", {op + ".sections"}}}).size() == 3);
}
