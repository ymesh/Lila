#include <doctest/doctest.h>

#include "TestUtil.h"
#include "ops/Command.h"
#include "gn/Exec.h"
#include "gn/Gn.h"
#include "geo/Mesh.h"

using namespace lila;
using namespace gntest;
using json = nlohmann::json;

namespace
{
/** A group with the given nodes and links, evaluated. */
gn::GeometryPtr evalNodes(std::vector<gn::GNode> nodes, std::vector<gn::GLink> links, const gn::ExecEnv& env = {})
{
    gn::Group g = gn::Group::makeDefault("T");
    g.links.clear();
    g.nodes.insert(g.nodes.end(), nodes.begin(), nodes.end());
    g.links = std::move(links);
    gn::PlanPtr plan = gn::compile(g);
    REQUIRE_MESSAGE(plan->error.empty(), plan->error);
    return gn::execute(*plan, env);
}

using N = gn::GNode;
using L = gn::GLink;
} // namespace

TEST_CASE("mesh nodes")
{
    // Extrude every face of a cube outward: 6 tops plus 4 sides each.
    auto ext = evalNodes({N{"C", "MeshCube", {}, {}}, N{"E", "ExtrudeMesh", {}, {}}},
                         {L{"C", "mesh", "E", "mesh"}, L{"E", "mesh", "GroupOutput", "geometry"}});
    CHECK(ext->mesh->numFaces() == 6 + 24);
    CHECK(ext->mesh->bounds().max.x == doctest::Approx(60.0));

    // Delete the extruded tops through the Top field.
    auto noTop = evalNodes({N{"C", "MeshCube", {}, {}}, N{"E", "ExtrudeMesh", {}, {}}, N{"D", "DeleteGeometry", {}, {{"domain", "face"}}}},
                           {L{"C", "mesh", "E", "mesh"}, L{"E", "mesh", "D", "geometry"}, L{"E", "top", "D", "selection"},
                            L{"D", "geometry", "GroupOutput", "geometry"}});
    CHECK(noTop->mesh->numFaces() == 24);

    auto sub = evalNodes({N{"C", "MeshCube", {}, {}}, N{"S", "SubdivisionSurface", {}, {{"level", 2}}}, N{"T", "Triangulate", {}, {}}},
                         {L{"C", "mesh", "S", "mesh"}, L{"S", "mesh", "T", "mesh"}, L{"T", "mesh", "GroupOutput", "geometry"}});
    CHECK(sub->mesh->numFaces() == 6 * 16 * 2);

    // Mesh to points, back to vertices, merged into one point.
    auto merged = evalNodes({N{"C", "MeshCube", {}, {}}, N{"P", "MeshToPoints", {}, {}}, N{"V", "PointsToVertices", {}, {}},
                             N{"M", "MergeByDistance", {}, {{"distance", 1000.0}}}},
                            {L{"C", "mesh", "P", "mesh"}, L{"P", "points", "V", "points"}, L{"V", "mesh", "M", "geometry"},
                             L{"M", "geometry", "GroupOutput", "geometry"}});
    CHECK(merged->mesh->numPoints() == 1);

    // Each face of a cube three times, as separate pieces.
    auto dup = evalNodes({N{"C", "MeshCube", {}, {}}, N{"D", "DuplicateElements", {}, {{"domain", "face"}, {"amount", 3}}}},
                         {L{"C", "mesh", "D", "geometry"}, L{"D", "geometry", "GroupOutput", "geometry"}});
    CHECK(dup->mesh->numFaces() == 18);
    CHECK(dup->mesh->numPoints() == 72);

    // Separate the face whose normal points up.
    auto sep = evalNodes({N{"C", "MeshCube", {}, {}}, N{"Nm", "Normal", {}, {}}, N{"X", "SeparateXYZ", {}, {}}, N{"Cmp", "Compare", {}, {{"b", 0.5}}},
                          N{"S", "SeparateGeometry", {}, {{"domain", "face"}}}},
                         {L{"C", "mesh", "S", "geometry"}, L{"Nm", "value", "X", "vector"}, L{"X", "y", "Cmp", "a"}, L{"Cmp", "result", "S", "selection"},
                          L{"S", "selection", "GroupOutput", "geometry"}});
    CHECK(sep->mesh->numFaces() == 1);
}

TEST_CASE("attribute nodes")
{
    // Store the index, then lift the whole grid by its maximum.
    auto stored = evalNodes({N{"G", "MeshGrid", {}, {}}, N{"I", "Index", {}, {}}, N{"St", "StoreNamedAttribute", {}, {{"name", "weight"}}},
                             N{"Stat", "AttributeStatistic", {}, {}}, N{"A", "NamedAttribute", {}, {{"name", "weight"}}}, N{"Sp", "SetPosition", {}, {}},
                             N{"Xyz", "CombineXYZ", {}, {}}},
                            {L{"G", "mesh", "St", "geometry"}, L{"I", "value", "St", "value"}, L{"St", "geometry", "Stat", "geometry"},
                             L{"A", "attribute", "Stat", "attribute"}, L{"St", "geometry", "Sp", "geometry"}, L{"Stat", "max", "Xyz", "y"},
                             L{"Xyz", "vector", "Sp", "offset"}, L{"Sp", "geometry", "GroupOutput", "geometry"}});
    REQUIRE(stored->mesh);
    CHECK(stored->mesh->bounds().max.y == doctest::Approx(8.0));
    CHECK(stored->mesh->pointAttrs().has("weight"));

    // Captured positions stay put after the geometry moves; writing them back undoes the move.
    auto captured = evalNodes({N{"C", "MeshCube", {}, {}}, N{"Pos", "Position", {}, {}}, N{"Cap", "CaptureAttribute", {}, {{"dataType", "vector"}}},
                               N{"T", "TransformGeometry", {}, {{"translation", {500, 0, 0}}}}, N{"Sp", "SetPosition", {}, {}}},
                              {L{"C", "mesh", "Cap", "geometry"}, L{"Pos", "value", "Cap", "value"}, L{"Cap", "geometry", "T", "geometry"},
                               L{"T", "geometry", "Sp", "geometry"}, L{"Cap", "attribute", "Sp", "position"}, L{"Sp", "geometry", "GroupOutput", "geometry"}});
    CHECK(captured->mesh->bounds().max.x == doctest::Approx(50.0));
}

TEST_CASE("curve nodes")
{
    auto tube = evalNodes({N{"L", "CurveLine", {}, {}}, N{"R", "ResampleCurve", {}, {{"count", 5}}}, N{"Ci", "CurveCircle", {}, {{"resolution", 6}}},
                           N{"M", "CurveToMesh", {}, {}}},
                          {L{"L", "curve", "R", "curve"}, L{"R", "curve", "M", "curve"}, L{"Ci", "curve", "M", "profileCurve"},
                           L{"M", "mesh", "GroupOutput", "geometry"}});
    CHECK(tube->mesh->numFaces() == 4 * 6);
    CHECK(tube->mesh->bounds().max.y == doctest::Approx(100.0));

    auto fill = evalNodes({N{"Mc", "MeshCircle", {}, {{"vertices", 12}}}, N{"Mtc", "MeshToCurve", {}, {}}, N{"F", "FillCurve", {}, {}}},
                          {L{"Mc", "mesh", "Mtc", "mesh"}, L{"Mtc", "curve", "F", "curve"}, L{"F", "mesh", "GroupOutput", "geometry"}});
    REQUIRE(fill->mesh);
    CHECK(fill->mesh->numFaces() == 1);
    CHECK(fill->mesh->faceSize(0) == 12);

    auto bez = evalNodes({N{"B", "BezierSegment", {}, {{"resolution", 10}}}, N{"M", "CurveToMesh", {}, {}}},
                         {L{"B", "curve", "M", "curve"}, L{"M", "mesh", "GroupOutput", "geometry"}});
    CHECK(bez->mesh->numEdges() == 10);
}

TEST_CASE("sampling nodes")
{
    // Raycast a lifted cube's points down onto a grid.
    auto hits = evalNodes({N{"G", "MeshGrid", {}, {{"sizeX", 1000.0}, {"sizeY", 1000.0}}}, N{"C", "MeshCube", {}, {{"size", {10, 10, 10}}}},
                           N{"T", "TransformGeometry", {}, {{"translation", {0, 100, 0}}}}, N{"Rc", "Raycast", {}, {}}, N{"Sp", "SetPosition", {}, {}}},
                          {L{"C", "mesh", "T", "geometry"}, L{"T", "geometry", "Sp", "geometry"}, L{"G", "mesh", "Rc", "targetGeometry"},
                           L{"Rc", "hitPosition", "Sp", "position"}, L{"Rc", "isHit", "Sp", "selection"}, L{"Sp", "geometry", "GroupOutput", "geometry"}});
    CHECK(hits->mesh->bounds().max.y == doctest::Approx(0.0));

    // Proximity pulls cube points onto a far sphere's points.
    auto prox = evalNodes({N{"C", "MeshCube", {}, {}}, N{"S", "MeshIcoSphere", {}, {{"radius", 10.0}}}, N{"T", "TransformGeometry", {}, {{"translation", {500, 0, 0}}}},
                           N{"P", "GeometryProximity", {}, {{"targetElement", "points"}}}, N{"Sp", "SetPosition", {}, {}}},
                          {L{"S", "mesh", "T", "geometry"}, L{"T", "geometry", "P", "targetGeometry"}, L{"C", "mesh", "Sp", "geometry"},
                           L{"P", "position", "Sp", "position"}, L{"Sp", "geometry", "GroupOutput", "geometry"}});
    CHECK(prox->mesh->bounds().min.x > 480.0);

    // Sample Nearest + Sample Index snap cube points onto a sphere.
    auto snap = evalNodes({N{"C", "MeshCube", {}, {}}, N{"S", "MeshUVSphere", {}, {{"radius", 20.0}}}, N{"Nr", "SampleNearest", {}, {}},
                           N{"Pos", "Position", {}, {}}, N{"Si", "SampleIndex", {}, {{"dataType", "vector"}}}, N{"Sp", "SetPosition", {}, {}}},
                          {L{"S", "mesh", "Nr", "geometry"}, L{"S", "mesh", "Si", "geometry"}, L{"Pos", "value", "Si", "value"}, L{"Nr", "index", "Si", "index"},
                           L{"C", "mesh", "Sp", "geometry"}, L{"Si", "value", "Sp", "position"}, L{"Sp", "geometry", "GroupOutput", "geometry"}});
    for (const glm::dvec3& p : snap->mesh->positions())
        CHECK(glm::length(p) == doctest::Approx(20.0).epsilon(0.01));
}

TEST_CASE("utility nodes")
{
    auto util = evalNodes({N{"C", "MeshCube", {}, {}}, N{"Mr", "MapRange", {}, {{"value", 0.5}, {"toMin", 10.0}, {"toMax", 30.0}}},
                           N{"Cl", "Clamp", {}, {{"min", 0.0}, {"max", 15.0}}}, N{"Mx", "Mix", {}, {{"factor", 0.5}, {"b", 10.0}}},
                           N{"Ri", "RandomValue", {}, {{"dataType", "int"}, {"min", 3}, {"max", 3}}}, N{"Add", "Math", {}, {{"operation", "add"}}},
                           N{"Add2", "Math", {}, {{"operation", "add"}}}, N{"Xyz", "CombineXYZ", {}, {}}, N{"T", "TransformGeometry", {}, {}}},
                          {L{"Mr", "result", "Cl", "value"}, L{"Cl", "result", "Add", "a"}, L{"Mx", "result", "Add", "b"}, L{"Add", "value", "Add2", "a"},
                           L{"Ri", "value", "Add2", "b"}, L{"Add2", "value", "Xyz", "x"}, L{"Xyz", "vector", "T", "translation"}, L{"C", "mesh", "T", "geometry"},
                           L{"T", "geometry", "GroupOutput", "geometry"}});
    // clamp(map 0.5 -> 20, 0..15) = 15; mix(0.5, 0, 10) = 5; plus 3.
    CHECK(util->mesh->bounds().center().x == doctest::Approx(15.0 + 5.0 + 3.0));

    // Align a rotation's Y to +X, then rotate (0,1,0) by it.
    auto align = evalNodes({N{"C", "MeshCube", {}, {{"size", {1, 1, 1}}}}, N{"Al", "AlignEulerToVector", {}, {{"vector", {1, 0, 0}}}},
                            N{"Vr", "VectorRotate", {}, {{"rotationType", "euler"}, {"vector", {0, 100, 0}}}}, N{"T", "TransformGeometry", {}, {}}},
                           {L{"Al", "rotation", "Vr", "rotation"}, L{"Vr", "vector", "T", "translation"}, L{"C", "mesh", "T", "geometry"},
                            L{"T", "geometry", "GroupOutput", "geometry"}});
    CHECK(align->mesh->bounds().center().x == doctest::Approx(100.0));
    CHECK(align->mesh->bounds().center().y == doctest::Approx(0.0).epsilon(1e-6));
}

TEST_CASE("object info follows another object and refuses cycles")
{
    run("newScene");
    std::string target = run("polySphere", {{"radius", 50.0}, {"name", "Source"}})[0];
    std::string mesh = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"name", "User"}})[0];
    run("gnCreateGroup", args({"Copy"}));
    std::string oi = run("gnAddNode", args({"Copy", "ObjectInfo"}));
    std::string join = run("gnAddNode", args({"Copy", "JoinGeometry"}));
    run("gnSetValue", args({"Copy", oi + ".object", target}));
    run("gnSetValue", args({"Copy", oi + ".transformSpace", "world"}));
    run("gnDisconnect", args({"Copy", "GroupOutput.geometry"}));
    run("gnConnect", args({"Copy", "GroupInput.geometry", join + ".geometry"}));
    run("gnConnect", args({"Copy", oi + ".geometry", join + ".geometry"}));
    run("gnConnect", args({"Copy", join + ".geometry", "GroupOutput.geometry"}));
    run("gnApply", args({mesh, "Copy"}));
    int sphereFaces = faces(target);
    CHECK(faces(mesh) == 6 + sphereFaces);

    run("setAttr", args({target + ".translateX", 300.0}));
    CHECK(bounds(mesh)["boundsMax"][0].get<double>() == doctest::Approx(350.0));
    run("setAttr", args({target + ".translateX", 0.0}));
    CHECK(bounds(mesh)["boundsMax"][0].get<double>() == doctest::Approx(50.0));

    // The mesh can't read itself.
    CHECK_THROWS(run("gnSetValue", args({"Copy", oi + ".object", mesh})));
    CHECK(faces(mesh) == 6 + sphereFaces);
}

TEST_CASE("along curve nodes")
{
    // A 100 cm cube laid along a 300 cm line up Y, stretched to its length.
    auto bent = evalNodes({N{"C", "MeshCube", {}, {{"verticesX", 5}}}, N{"L", "CurveLine", {}, {{"end", {0, 300, 0}}}},
                           N{"D", "DeformAlongCurve", {}, {{"stretch", true}}}},
                          {L{"C", "mesh", "D", "geometry"}, L{"L", "curve", "D", "curve"}, L{"D", "geometry", "GroupOutput", "geometry"}});
    REQUIRE(bent->mesh);
    CHECK(bent->mesh->bounds().min.y == doctest::Approx(0.0));
    CHECK(bent->mesh->bounds().max.y == doctest::Approx(300.0));

    auto inst = evalNodes({N{"C", "MeshCube", {}, {{"size", {10, 10, 10}}}}, N{"L", "CurveLine", {}, {{"end", {200, 0, 0}}}},
                           N{"I", "InstanceOnCurve", {}, {{"count", 3}}}, N{"R", "RealizeInstances", {}, {}}},
                          {L{"C", "mesh", "I", "instance"}, L{"L", "curve", "I", "curve"}, L{"I", "instances", "R", "geometry"},
                           L{"R", "geometry", "GroupOutput", "geometry"}});
    REQUIRE(inst->mesh);
    CHECK(inst->mesh->numFaces() == 18);
    CHECK(inst->mesh->bounds().max.x == doctest::Approx(205.0));
}

TEST_CASE("math step and smooth step, operations listed as search variants")
{
    // smoothStep(0.25 from 0 to 1) = 0.15625 moves the cube in x; step(0.7 against 0.5) = 1 in y.
    auto moved = evalNodes({N{"C", "MeshCube", {}, {}}, N{"Ss", "Math", {}, {{"operation", "smoothStep"}, {"a", 0.25}, {"b", 0.0}, {"c", 1.0}}},
                            N{"St", "Math", {}, {{"operation", "step"}, {"a", 0.7}, {"b", 0.5}}}, N{"Xyz", "CombineXYZ", {}, {}},
                            N{"T", "TransformGeometry", {}, {}}},
                           {L{"Ss", "value", "Xyz", "x"}, L{"St", "value", "Xyz", "y"}, L{"Xyz", "vector", "T", "translation"},
                            L{"C", "mesh", "T", "geometry"}, L{"T", "geometry", "GroupOutput", "geometry"}});
    CHECK(moved->mesh->bounds().center().x == doctest::Approx(0.15625));
    CHECK(moved->mesh->bounds().center().y == doctest::Approx(1.0));

    json types = ops::execute("gnNodeTypes", json::object());
    bool greater = false, smooth = false;
    for (const json& t : types)
        for (const json& v : t.value("variants", json::array()))
        {
            greater |= t.value("type", "") == "Compare" && v.value("label", "") == "Greater Than";
            smooth |= t.value("type", "") == "Math" && v["values"].value("operation", "") == "smoothStep";
        }
    CHECK(greater);
    CHECK(smooth);
}
