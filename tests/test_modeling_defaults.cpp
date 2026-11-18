#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "geo/Attribute.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/Isolation.h"
#include "scene/MeshNodes.h"

#include <cmath>

using namespace lila;
using json = nlohmann::json;
using namespace testutil;

namespace
{
size_t hardEdges(const std::string& m)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const geo::Mesh& mesh = *g.find(m)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    std::span<const bool> hard = mesh.edgeAttrs().get<bool>(geo::attr::kHard);
    size_t n = 0;
    for (geo::Index e = 0; e < hard.size(); ++e)
        n += mesh.edgeValid(e) && hard[e] ? 1 : 0;
    return n;
}
} // namespace

TEST_CASE("polyInsertLoop follows symmetry")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"width", 100.0}, {"depth", 100.0}, {"subdivisionsX", 4}, {"subdivisionsZ", 1}})[0];
    run("setSymmetry", {{"args", {plane}}, {"axis", "x"}});
    run("selectComponents", {{"args", {plane}}, {"edges", {0}}});
    run("polyInsertLoop");
    CHECK(faces(plane) == 6);
    CHECK(run("symmetryCheck", {{"args", {plane}}})[plane]["unmatched"].get<int>() == 0);
    run("undo");
    run("selectComponents", {{"args", {plane}}, {"edges", {0}}});
    run("polyInsertLoop", {{"count", 2}});
    CHECK(faces(plane) == 8);
}

TEST_CASE("polyInsertLoop spreads several loops round fraction under symmetry")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"width", 100.0}, {"depth", 100.0}, {"subdivisionsX", 4}, {"subdivisionsZ", 1}})[0];
    run("setSymmetry", {{"args", {plane}}, {"axis", "x"}});
    run("selectComponents", {{"args", {plane}}, {"edges", {0}}});
    run("polyInsertLoop", {{"count", 2}, {"fraction", 0.3}, {"spacing", 0.5}});
    CHECK(faces(plane) == 8);
    CHECK(run("symmetryCheck", {{"args", {plane}}})[plane]["unmatched"].get<int>() == 0);
}

TEST_CASE("polyOffsetLoop and polyEditEdgeFlow are undoable edits")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"width", 100.0}, {"depth", 100.0}, {"subdivisionsX", 4}, {"subdivisionsZ", 4}})[0];
    run("selectComponents", {{"args", {plane}}, {"edges", {0}}});
    run("selectLoop");
    run("polyOffsetLoop");
    // The border loop has faces on one side only: each of the 12 border faces is cut once.
    CHECK(faces(plane) == 28);
    run("undo");
    CHECK(faces(plane) == 16);

    int centre = -1;
    const json before = run("meshData", {{"args", {plane}}, {"base", true}});
    for (size_t i = 0; i < before["points"].size(); ++i)
        if (std::abs(before["points"][i][0].get<double>()) < 1e-6 && std::abs(before["points"][i][2].get<double>()) < 1e-6)
            centre = int(i);
    REQUIRE(centre >= 0);
    run("selectComponents", {{"args", {plane}}, {"vertices", {centre}}});
    run("moveComponents", {{"by", {0.0, 10.0, 0.0}}});
    run("polyEditEdgeFlow", {{"adjustEdgeFlow", 0.0}});
    const json after = run("meshData", {{"args", {plane}}, {"base", true}});
    CHECK(after["points"][size_t(centre)][1].get<double>() == doctest::Approx(0.0));
    run("undo");
    const json undone = run("meshData", {{"args", {plane}}, {"base", true}});
    CHECK(undone["points"][size_t(centre)][1].get<double>() == doctest::Approx(10.0));
}

TEST_CASE("polyDuplicateFaces copies faces into the same mesh")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    run("polyDuplicateFaces");
    CHECK(faces(cube) == 7);
    CHECK(run("componentSelection")[cube]["faces"].size() == 1);
    run("undo");
    CHECK(faces(cube) == 6);
}

TEST_CASE("component edits in object mode take the whole mesh")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"width", 100.0}, {"depth", 100.0}})[0];
    const int before = faces(plane);
    run("select", {{"args", {plane}}});
    run("selectMode", {{"args", {"object"}}});
    run("polyExtrude", {{"thickness", 1.0}});
    // An open plane keeps a back-facing base: the top, four sides and the base.
    CHECK(faces(plane) == before + 5);
    CHECK(run("selectMode") == "face");
    run("undo");
    CHECK(faces(plane) == before);
    CHECK(run("selectMode") == "object");

    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("select", {{"args", {cube}}});

    run("polySoftEdge", {{"angle", 0.0}});
    CHECK(hardEdges(cube) == 12);
    CHECK(run("selectMode") == "object");
    run("polySoftEdge", {{"angle", 180.0}});
    CHECK(hardEdges(cube) == 0);
    run("polySoftEdge", {{"angle", 30.0}});
    CHECK(hardEdges(cube) == 12);
}

TEST_CASE("polyMirror flip keeps nothing of the original")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("setAttr", {{"args", {cube + ".translate", {20.0, 0.0, 0.0}}}});
    run("polyMirror", {{"args", {cube}}, {"space", "world"}, {"flip", true}});
    CHECK(faces(cube) == 6);
    double least = 1e9, most = -1e9;
    const json data = run("meshData", {{"args", {cube}}});
    for (const json& p : data["points"])
    {
        least = std::min(least, p[0].get<double>());
        most = std::max(most, p[0].get<double>());
    }
    CHECK(least == doctest::Approx(-90.0));
    CHECK(most == doctest::Approx(10.0));
}

TEST_CASE("showLastHidden shows what the last hide hid")
{
    run("newScene");
    std::string a = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    std::string b = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("hide", {{"args", {a}}});
    run("hide", {{"args", {b}}});
    json shown = run("showLastHidden");
    REQUIRE(shown.size() == 1);
    CHECK(run("getAttr", {{"args", {b + ".visibility"}}}) == true);
    CHECK(run("getAttr", {{"args", {a + ".visibility"}}}) == false);
}

TEST_CASE("isolation sets take in objects made from isolated ones")
{
    using scene::isolation::Set;
    Set set;
    CHECK_FALSE(scene::isolation::inheritInto(set, {7}, {1}));
    set.nodes = {1, 2};
    CHECK_FALSE(scene::isolation::inheritInto(set, {7}, {3}));
    CHECK(scene::isolation::inheritInto(set, {7, 8}, {3, 2}));
    CHECK(set.nodes.contains(7));
    CHECK(set.nodes.contains(8));
}

TEST_CASE("merging exactly two vertices ignores the distance and meets at their centre")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"width", 100.0}, {"depth", 100.0}, {"subdivisionsX", 1}, {"subdivisionsZ", 1}})[0];
    scene::Graph& g = scene::Graph::current();
    auto mesh = [&]() -> const geo::Mesh& {
        g.evaluate();
        return *g.find(plane)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    };
    const glm::dvec3 mid = (mesh().position(0) + mesh().position(1)) * 0.5;
    run("selectComponents", {{"args", {plane}}, {"vertices", {0, 1}}});
    run("polyMerge");
    CHECK(run("meshInfo", {{"args", {plane}}})["points"].get<int>() == 3);
    bool atMid = false;
    for (geo::Index p = 0; p < mesh().pointCapacity(); ++p)
        atMid |= mesh().pointValid(p) && glm::distance(mesh().position(p), mid) < 1e-4;
    CHECK(atMid);
    run("undo");
    run("selectComponents", {{"args", {plane}}, {"vertices", {0, 1, 2}}});
    run("polyMerge");
    CHECK(run("meshInfo", {{"args", {plane}}})["points"].get<int>() == 4);
}

TEST_CASE("symmetric selection picks the mirror images")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"width", 100.0}, {"depth", 100.0}, {"subdivisionsX", 4}, {"subdivisionsZ", 1}})[0];
    auto selectedFaces = [&] {
        const json sel = run("componentSelection");
        return sel.empty() ? size_t(0) : sel.begin()->value("faces", json::array()).size();
    };
    run("selectComponents", {{"args", {plane}}, {"faces", {0}}, {"symmetric", true}});
    CHECK(selectedFaces() == 1);
    run("setSymmetry", {{"args", {plane}}, {"axis", "x"}});
    run("selectComponents", {{"args", {plane}}, {"faces", {0}}, {"symmetric", true}});
    CHECK(selectedFaces() == 2);
    run("selectComponents", {{"args", {plane}}, {"faces", {1}}});
    run("symmetrizeSelection");
    CHECK(selectedFaces() == 2);
    run("polySplitRings", {{"count", 1}});
    CHECK(run("symmetryCheck", {{"args", {plane}}})[plane]["unmatched"].get<int>() == 0);
}

TEST_CASE("polySplitRings in object mode splits the whole mesh")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"width", 400.0}, {"depth", 100.0}, {"subdivisionsX", 4}, {"subdivisionsZ", 1}})[0];
    run("select", {{"args", {plane}}});
    run("selectMode", {{"args", {"object"}}});
    auto points = [&] { return run("meshInfo", {{"args", {plane}}})["points"].get<int>(); };
    // Along the strip: one loop down its length. Across: one per column.
    run("polySplitRings");
    CHECK(faces(plane) == 8);
    CHECK(points() == 15);
    run("undo");
    run("select", {{"args", {plane}}});
    run("selectMode", {{"args", {"object"}}});
    run("polySplitRings", {{"direction", "across"}});
    CHECK(points() == 18);
}

TEST_CASE("splitRings modifier splits live, on the selection or the whole mesh")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"width", 400.0}, {"depth", 100.0}, {"subdivisionsX", 4}, {"subdivisionsZ", 1}})[0];
    run("select", {{"args", {plane}}});
    run("selectMode", {{"args", {"object"}}});
    std::string whole = run("addModifier", {{"args", {plane}}, {"kind", "splitRings"}}).get<std::string>();
    CHECK(faces(plane) == 8);
    run("setAttr", {{"args", {whole + ".count", 3}}});
    CHECK(faces(plane) == 16);
    run("undo");
    run("undo");

    run("selectComponents", {{"args", {plane}}, {"edges", {0}}});
    run("addModifier", {{"args", {plane}}, {"kind", "splitRings"}});
    CHECK(faces(plane) == 5);
}

TEST_CASE("splitRings modifier selection can be edited, applied and cancelled")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"width", 400.0}, {"depth", 100.0}, {"subdivisionsX", 4}, {"subdivisionsZ", 1}})[0];
    run("select", {{"args", {plane}}});
    run("selectMode", {{"args", {"object"}}});
    std::string mod = run("addModifier", {{"args", {plane}}, {"kind", "splitRings"}}).get<std::string>();
    CHECK(faces(plane) == 8);

    run("splitRingsEditSelection", {{"args", {mod}}});
    CHECK(faces(plane) == 4);
    CHECK(run("selectMode") == "face");
    CHECK(run("componentSelection")[plane]["faces"].size() == 4);
    run("selectComponents", {{"args", {plane}}, {"edges", {0}}});
    run("splitRingsApplySelection", {{"args", {plane}}});
    CHECK(faces(plane) == 5);
    CHECK(run("selectMode") == "object");

    run("splitRingsEditSelection", {{"args", {plane}}});
    CHECK(run("componentSelection")[plane]["edges"] == json::array({0}));
    run("selectComponents", {{"args", {plane}}, {"edges", {1, 2}}});
    run("splitRingsCancelSelection", {{"args", {plane}}});
    CHECK(faces(plane) == 5);

    run("splitRingsEditSelection", {{"args", {plane}}});
    run("selectMode", {{"args", {"object"}}});
    CHECK_THROWS(run("splitRingsApplySelection", {{"args", {plane}}}));
    run("splitRingsCancelSelection", {{"args", {plane}}});
    CHECK(faces(plane) == 5);
}
