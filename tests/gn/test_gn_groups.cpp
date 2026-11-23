#include <doctest/doctest.h>

#include "TestUtil.h"
#include "ops/Command.h"
#include "fbx/Fbx.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "gn/Exec.h"
#include "gn/Gn.h"
#include "gn/Viewer.h"
#include "scene/Graph.h"
#include "scene/Modifiers.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace lila;
using namespace gntest;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
scene::MeshPtr outMesh(const std::string& mesh)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(mesh);
    return n->as<scene::MeshPtr>(n->type().attrIndex("outMesh"));
}

std::string plane(const char* name, int subdivisions = 10)
{
    return run("polyPlane", {{"width", 100.0}, {"depth", 100.0}, {"name", name}, {"subdivisionsX", subdivisions}, {"subdivisionsZ", subdivisions}})[0].get<std::string>();
}

json boxSelectionSpec()
{
    return json::parse(R"({
      "name": "BoxSelection",
      "description": "True for points inside an axis-aligned box.",
      "inputs": [
        {"name": "center", "type": "vector", "default": [0, 0, 0], "description": "Middle of the box.", "subtype": "distance"},
        {"name": "size", "type": "vector", "default": [100, 100, 100], "description": "Full extent along each axis.", "subtype": "distance"}
      ],
      "outputs": [{"name": "selection", "type": "bool"}, {"name": "inverted", "type": "bool"}],
      "nodes": [
        {"name": "Position", "type": "Position"},
        {"name": "Offset", "type": "VectorMath", "props": {"operation": "subtract"}},
        {"name": "Abs", "type": "VectorMath", "props": {"operation": "absolute"}},
        {"name": "Half", "type": "VectorMath", "props": {"operation": "scale"}, "values": {"scale": 0.5}},
        {"name": "Inside", "type": "VectorMath", "props": {"operation": "subtract"}},
        {"name": "Split", "type": "SeparateXYZ"},
        {"name": "X", "type": "Compare", "props": {"operation": "greaterEqual"}},
        {"name": "Y", "type": "Compare", "props": {"operation": "greaterEqual"}},
        {"name": "Z", "type": "Compare", "props": {"operation": "greaterEqual"}},
        {"name": "XY", "type": "BooleanMath"},
        {"name": "XYZ", "type": "BooleanMath"},
        {"name": "Not", "type": "BooleanMath", "props": {"operation": "not"}}
      ],
      "links": [
        "Position.value -> Offset.a", "GroupInput.center -> Offset.b", "Offset.vector -> Abs.a", "GroupInput.size -> Half.a",
        "Half.vector -> Inside.a", "Abs.vector -> Inside.b", "Inside.vector -> Split.vector",
        "Split.x -> X.a", "Split.y -> Y.a", "Split.z -> Z.a", "X.result -> XY.a", "Y.result -> XY.b", "XY.result -> XYZ.a",
        "Z.result -> XYZ.b", "XYZ.result -> GroupOutput.selection", "XYZ.result -> Not.a", "Not.result -> GroupOutput.inverted"
      ]
    })");
}

/** Keeps the part of a mesh inside a box given to a nested BoxSelection group. */
json cropSpec(json center, json size)
{
    return {{"name", "Crop"},
            {"inputs", json::array({{{"name", "size"}, {"type", "vector"}, {"default", size}}})},
            {"nodes", json::array({{{"name", "Box"}, {"type", "Group"}, {"group", "BoxSelection"}, {"values", {{"center", center}}}},
                                   {{"name", "Delete"}, {"type", "DeleteGeometry"}, {"props", {{"domain", "point"}}}}})},
            {"links", json::array({"GroupInput.geometry -> Delete.geometry", "GroupInput.size -> Box.size", "Box.inverted -> Delete.selection",
                                   "Delete.geometry -> GroupOutput.geometry"})}};
}

fs::path tempDir()
{
    fs::path d = fs::temp_directory_path() / "lila_gn_tests" / "groups";
    fs::create_directories(d);
    return d;
}
} // namespace

TEST_CASE("colour attributes become vertex colour sets that FBX exports")
{
    run("newScene");
    std::string mesh = plane("Tinted", 4);
    run("gnBuildGroup", args({json::parse(R"({
      "name": "Tint",
      "nodes": [
        {"name": "Pos", "type": "Position"},
        {"name": "Split", "type": "SeparateXYZ"},
        {"name": "Range", "type": "MapRange", "values": {"fromMin": -50, "fromMax": 50}},
        {"name": "Color", "type": "CombineColor", "values": {"green": 0.25, "blue": 0, "alpha": 1}},
        {"name": "Store", "type": "StoreNamedAttribute", "props": {"dataType": "color", "domain": "point"}, "values": {"name": "Col"}},
        {"name": "Normal", "type": "SetMeshNormal", "props": {"mode": "free", "domain": "point"}},
        {"name": "Up", "type": "Vector", "values": {"value": [0, 1, 0]}}
      ],
      "links": ["Pos.value -> Split.vector", "Split.x -> Range.value", "Range.result -> Color.red", "GroupInput.geometry -> Store.geometry",
                "Color.color -> Store.value", "Store.geometry -> Normal.mesh", "Up.value -> Normal.customNormal", "Normal.mesh -> GroupOutput.geometry"]
    })")}));
    run("gnApply", args({mesh, "Tint"}));
    scene::MeshPtr m = outMesh(mesh);
    REQUIRE(m);
    REQUIRE(geo::colors::has(*m, "Col"));
    std::span<const glm::vec4> col = geo::colors::values(*m, "Col");
    std::span<const glm::vec3> nrm = m->cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    REQUIRE(nrm.size() == m->cornerCapacity());
    for (geo::Index c = 0; c < m->cornerCapacity(); ++c)
    {
        if (!m->cornerValid(c))
            continue;
        double x = m->position(m->cornerPoint(c)).x;
        CHECK(col[c].r == doctest::Approx(std::clamp((x + 50.0) / 100.0, 0.0, 1.0)).epsilon(1e-4));
        CHECK(col[c].g == doctest::Approx(0.25));
        CHECK(col[c].a == doctest::Approx(1.0));
        CHECK(nrm[c].y == doctest::Approx(1.0));
    }
    fs::path file = tempDir() / "tinted.fbx";
    scene::Graph& g = scene::Graph::current();
    fbx::ExportOptions o;
    o.tangents = false;
    fbx::ExportScene es = fbx::gatherExport(g, {g.find(mesh)->id()}, o);
    std::string err;
    REQUIRE_MESSAGE(fbx::writeFbx(es, file, o, &err), err);

    run("newScene");
    fbx::ImportResult r = fbx::importFbx(scene::Graph::current(), file);
    std::string imported;
    for (scene::NodeId id : scene::Graph::current().nodes())
        if (scene::Node* n = scene::Graph::current().node(id); n && n->type().isA("mesh"))
            imported = n->name();
    REQUIRE(!imported.empty());
    scene::MeshPtr back = outMesh(imported);
    REQUIRE(back);
    std::vector<std::string> sets = geo::colors::sets(*back);
    REQUIRE(sets.size() == 1);
    std::span<const glm::vec4> bc = geo::colors::values(*back, sets[0]);
    float maxRed = 0.0f;
    for (geo::Index c = 0; c < back->cornerCapacity(); ++c)
        if (back->cornerValid(c))
            maxRed = std::max(maxRed, bc[c].r);
    CHECK(maxRed == doctest::Approx(1.0).epsilon(1e-3));
    std::span<const glm::vec3> bn = back->cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    REQUIRE(bn.size() == back->cornerCapacity());
    for (geo::Index c = 0; c < back->cornerCapacity(); ++c)
        if (back->cornerValid(c))
            CHECK(glm::length(bn[c] - glm::vec3(0, 1, 0)) < 1e-3f);
}

TEST_CASE("vectors stored under a colour name still make a colour set")
{
    run("newScene");
    std::string mesh = plane("Vec", 2);
    run("gnBuildGroup", args({json{{"name", "VecCol"},
                                   {"nodes", json::array({{{"name", "S"}, {"type", "StoreNamedAttribute"}, {"props", {{"dataType", "vector"}}},
                                                           {"values", {{"name", "Cd"}, {"value", json::array({0.5, 0.5, 1.0})}}}}})},
                                   {"links", json::array({"GroupInput.geometry -> S.geometry", "S.geometry -> GroupOutput.geometry"})}}}));
    run("gnApply", args({mesh, "VecCol"}));
    scene::MeshPtr m = outMesh(mesh);
    REQUIRE(geo::colors::has(*m, "Cd"));
    CHECK(geo::colors::values(*m, "Cd")[0].b == doctest::Approx(1.0));
}

TEST_CASE("set mesh normal and shade smooth")
{
    gn::Group g = gn::Group::makeDefault("N");
    g.links.clear();
    g.nodes.push_back({"Cube", "MeshCube", {}, json::object()});
    g.nodes.push_back({"Pos", "Position", {}, json::object()});
    g.nodes.push_back({"Norm", "VectorMath", {}, {{"operation", "normalize"}}});
    g.nodes.push_back({"Set", "SetMeshNormal", {}, {{"mode", "free"}}});
    g.links = {{"Cube", "mesh", "Set", "mesh"}, {"Pos", "value", "Norm", "a"}, {"Norm", "vector", "Set", "customNormal"}, {"Set", "mesh", "GroupOutput", "geometry"}};
    gn::GeometryPtr out = gn::execute(*gn::compile(g), {});
    const geo::Mesh& m = *out->mesh;
    std::span<const glm::vec3> n = m.cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    REQUIRE(n.size() == m.cornerCapacity());
    for (geo::Index c = 0; c < m.cornerCapacity(); ++c)
        if (m.cornerValid(c))
            CHECK(glm::length(glm::dvec3(n[c]) - glm::normalize(m.position(m.cornerPoint(c)))) < 1e-5);
    // The Normal input reads custom normals on corners.
    g.nodes.push_back({"Smooth", "SetShadeSmooth", {}, json::object()});
    std::erase_if(g.links, [](const gn::GLink& l) { return l.toNode == "GroupOutput"; });
    g.links.push_back({"Set", "mesh", "Smooth", "mesh"});
    g.links.push_back({"Smooth", "mesh", "GroupOutput", "geometry"});
    gn::GeometryPtr smooth = gn::execute(*gn::compile(g), {});
    CHECK_FALSE(smooth->mesh->cornerAttrs().has(geo::attr::kNormal));

    gn::Group angle = gn::Group::makeDefault("A");
    angle.links.clear();
    angle.nodes.push_back({"Cube", "MeshCube", {}, json::object()});
    angle.nodes.push_back({"Set", "SetMeshNormal", {}, {{"mode", "smoothByAngle"}, {"angle", 30.0}}});
    angle.links = {{"Cube", "mesh", "Set", "mesh"}, {"Set", "mesh", "GroupOutput", "geometry"}};
    gn::GeometryPtr a = gn::execute(*gn::compile(angle), {});
    std::span<const bool> hard = a->mesh->edgeAttrs().get<bool>(geo::attr::kHard);
    REQUIRE(!hard.empty());
    CHECK(std::all_of(hard.begin(), hard.end(), [](bool h) { return h; }));
}

TEST_CASE("combine and separate colour")
{
    gn::Group g = gn::Group::makeDefault("C");
    g.links.clear();
    g.nodes.push_back({"Make", "CombineColor", {}, {{"mode", "hsv"}, {"hue", 0.6}, {"saturation", 0.5}, {"value", 0.8}, {"alpha", 0.4}}});
    g.nodes.push_back({"Split", "SeparateColor", {}, {{"mode", "hsl"}}});
    g.nodes.push_back({"Back", "CombineColor", {}, {{"mode", "hsl"}}});
    g.nodes.push_back({"Out", "SeparateColor", {}, {{"mode", "hsv"}}});
    g.links = {{"Make", "color", "Split", "color"}, {"Split", "hue", "Back", "hue"}, {"Split", "saturation", "Back", "saturation"},
               {"Split", "lightness", "Back", "lightness"}, {"Split", "alpha", "Back", "alpha"}, {"Back", "color", "Out", "color"}};
    gn::PlanPtr plan = gn::compile(g);
    REQUIRE(plan->error.empty());
    // Evaluate the Out node's fields as constants through a probe group output.
    g.outputs = {{"h", gn::SocketType::Float, {}}, {"s", gn::SocketType::Float, {}}, {"v", gn::SocketType::Float, {}}, {"a", gn::SocketType::Float, {}}};
    g.links.push_back({"Out", "hue", "GroupOutput", "h"});
    g.links.push_back({"Out", "saturation", "GroupOutput", "s"});
    g.links.push_back({"Out", "value", "GroupOutput", "v"});
    g.links.push_back({"Out", "alpha", "GroupOutput", "a"});
    std::vector<gn::Value> out = gn::executeOutputs(*gn::compile(g), {});
    REQUIRE(out.size() == 5);
    CHECK(gn::evaluateSingle<double>(out[1].field) == doctest::Approx(0.6));
    CHECK(gn::evaluateSingle<double>(out[2].field) == doctest::Approx(0.5));
    CHECK(gn::evaluateSingle<double>(out[3].field) == doctest::Approx(0.8));
    CHECK(gn::evaluateSingle<double>(out[4].field) == doctest::Approx(0.4));
}

TEST_CASE("nested groups, collapse and ungroup")
{
    run("newScene");
    std::string mesh = plane("Field", 10);
    run("gnBuildGroup", args({boxSelectionSpec()}));
    run("gnBuildGroup", args({cropSpec(json::array({0, 0, 0}), json::array({50, 50, 50}))}));
    std::string mod = run("gnApply", args({mesh, "Crop"})).get<std::string>();
    // A 100 cm plane with 10 divisions: points every 10 cm; the 50 cm box keeps x and z in -25..25.
    scene::MeshPtr m = outMesh(mesh);
    REQUIRE(m);
    CHECK(m->numPoints() == 25);

    // The exposed input of the outer group drives the inner one.
    scene::Graph& g = scene::Graph::current();
    scene::Node* modNode = g.find(mod);
    g.setValue({modNode->id(), modNode->type().attrIndex("size"), -1}, glm::dvec3(70.0, 70.0, 70.0));
    CHECK(outMesh(mesh)->numPoints() == 49);

    // Editing the inner group updates the outer group's modifier.
    run("gnSetValue", args({"BoxSelection", "Half.scale", 0.25}));
    CHECK(outMesh(mesh)->numPoints() == 9);
    run("undo");
    CHECK(outMesh(mesh)->numPoints() == 49);

    // A group can't contain itself, directly or through another group.
    std::string self = run("gnAddNode", args({"BoxSelection", "Group"})).get<std::string>();
    CHECK_THROWS_AS(run("gnSetValue", args({"BoxSelection", self + ".group", "Crop"})), CommandError);
    CHECK_THROWS_AS(run("gnSetValue", args({"BoxSelection", self + ".group", "BoxSelection"})), CommandError);
    run("gnDeleteNode", args({"BoxSelection", self}));

    // Ungrouping inlines the inner nodes and keeps the result; grouping them again does too.
    json added = run("gnUngroup", args({"Crop", "Box"}));
    CHECK(added.size() == 12);
    CHECK(outMesh(mesh)->numPoints() == 49);
    std::vector<json> names(added.begin(), added.end());
    json call = {{"args", json::array({"Crop"})}, {"name", "Inner"}};
    for (const json& n : names)
        call["args"].push_back(n);
    json made = run("gnGroupNodes", call);
    CHECK(made["group"] == "Inner");
    CHECK(gn::findGroupNode(g, "Inner"));
    CHECK(outMesh(mesh)->numPoints() == 49);
    gn::Group inner = gn::readGroup(*gn::findGroupNode(g, "Inner"));
    CHECK(inner.inputs.size() == 1); // center was a value, inlined by the ungroup
    CHECK(inner.outputs.size() == 1);
}

TEST_CASE("group interface")
{
    run("newScene");
    std::string mesh = plane("Iface", 2);
    run("gnCreateGroup", args({"Lift"}));
    run("gnAddInput", {{"args", json::array({"Lift", "height", "float"})}, {"label", "Lift Height"}, {"description", "How far up."},
                       {"min", 0}, {"max", 500}, {"softMax", 100}, {"step", 5}, {"panel", "Shape"}, {"default", 20}});
    run("gnAddInput", {{"args", json::array({"Lift", "tint", "color"})}, {"default", json::array({1, 0, 0, 1})}, {"panel", "Look"}});
    run("gnAddInput", {{"args", json::array({"Lift", "twist", "float"})}, {"subtype", "angle"}, {"default", 90}});
    std::string sp = run("gnAddNode", args({"Lift", "SetPosition"})).get<std::string>();
    std::string xyz = run("gnAddNode", args({"Lift", "CombineXYZ"})).get<std::string>();
    run("gnDisconnect", args({"Lift", "GroupOutput.geometry"}));
    run("gnConnect", args({"Lift", "GroupInput.geometry", sp + ".geometry"}));
    run("gnConnect", args({"Lift", sp + ".geometry", "GroupOutput.geometry"}));
    run("gnConnect", args({"Lift", "GroupInput.height", xyz + ".y"}));
    run("gnConnect", args({"Lift", xyz + ".vector", sp + ".offset"}));
    std::string mod = run("gnApply", args({mesh, "Lift"})).get<std::string>();

    scene::Graph& g = scene::Graph::current();
    const scene::NodeType& t = g.find(mod)->type();
    const scene::AttrDef& h = t.attrs[t.attrIndex("height")];
    CHECK(h.niceName == "Lift Height");
    CHECK(h.doc == "How far up.");
    CHECK(h.minValue == 0.0);
    CHECK(h.maxValue == 500.0);
    CHECK(h.softMax == 100.0);
    CHECK(h.step == 5.0);
    CHECK(h.category == "Shape");
    CHECK((t.attrs[t.attrIndex("tint")].flags & scene::AttrColor) != 0);
    CHECK(outMesh(mesh)->bounds().max.y == doctest::Approx(20.0));

    // Presentation edits update the type in place; values stay.
    g.setValue({g.find(mod)->id(), t.attrIndex("height"), -1}, 35.0);
    run("gnSetInputInfo", {{"args", json::array({"Lift", "height"})}, {"tooltip", "Distance to lift."}, {"max", ""}});
    const scene::NodeType& t2 = g.find(mod)->type();
    CHECK(t2.attrs[t2.attrIndex("height")].doc == "Distance to lift.");
    CHECK_FALSE(t2.attrs[t2.attrIndex("height")].maxValue.has_value());
    CHECK(outMesh(mesh)->bounds().max.y == doctest::Approx(35.0));

    // Renaming keeps the modifier's value.
    run("gnRenameInput", args({"Lift", "height", "lift"}));
    scene::Node* renamed = g.find(mod);
    CHECK(renamed->type().attrIndex("height") < 0);
    CHECK(renamed->as<double>(renamed->type().attrIndex("lift")) == doctest::Approx(35.0));
    CHECK(outMesh(mesh)->bounds().max.y == doctest::Approx(35.0));

    json order = run("gnMoveInput", args({"Lift", "twist", 0}));
    CHECK(order == json::array({"twist", "lift", "tint"}));
    gn::Group grp = gn::readGroup(*gn::findGroupNode(g, "Lift"));
    CHECK(grp.inputs[0].subtype == "angle");
    CHECK(grp.panels() == std::vector<std::string>{"Shape", "Look"});
    CHECK(grp.input("lift")->description == "Distance to lift.");
    CHECK(grp.input("lift")->displayName() == "Lift Height");
    CHECK(grp.input("tint")->displayName() == "Tint");
    CHECK_THROWS_AS(run("gnSetInputInfo", {{"args", json::array({"Lift", "lift"})}, {"subtype", "bogus"}}), CommandError);
}

TEST_CASE("export and import node groups")
{
    run("newScene");
    run("gnBuildGroup", args({boxSelectionSpec()}));
    run("gnBuildGroup", args({cropSpec(json::array({0, 0, 0}), json::array({50, 50, 50}))}));
    fs::path file = tempDir() / "crop.lilanodes";
    std::string written = run("gnExportGroup", args({"Crop", file.generic_string()})).get<std::string>();
    std::ifstream in(written);
    json doc = json::parse(in);
    CHECK(doc["format"] == "lila.nodes");
    CHECK(doc["version"] == 1);
    REQUIRE(doc["dependencies"].size() == 1);
    CHECK(doc["dependencies"][0]["name"] == "BoxSelection");

    run("newScene");
    json names = run("gnImportGroup", args({file.generic_string()}));
    CHECK(names == json::array({"BoxSelection", "Crop"}));
    std::string mesh = plane("Imported", 10);
    run("gnApply", args({mesh, "Crop"}));
    CHECK(outMesh(mesh)->numPoints() == 25);
    // Importing again replaces in place.
    CHECK(run("gnImportGroup", args({file.generic_string()})) == names);
    CHECK(outMesh(mesh)->numPoints() == 25);
}

TEST_CASE("build group validation lists the valid sockets")
{
    run("newScene");
    json bad = {{"name", "Bad"},
                {"nodes", json::array({{{"name", "V"}, {"type", "VoronoiTexture"}, {"values", {{"scael", 2}}}}, {{"name", "X"}, {"type", "Voronoi"}}})},
                {"links", json::array({"V.dist -> GroupOutput.geometry"})}};
    try
    {
        run("gnBuildGroup", args({bad}));
        FAIL("expected an error");
    }
    catch (const CommandError& e)
    {
        std::string msg = e.what();
        CHECK(msg.find("scael") != std::string::npos);
        CHECK(msg.find("scale (float)") != std::string::npos);
        CHECK(msg.find("did you mean VoronoiTexture") != std::string::npos);
        CHECK(msg.find("outputs: distance (float), color (color)") != std::string::npos);
    }
    CHECK(gn::findGroupNode(scene::Graph::current(), "Bad") == nullptr);

    json types = run("gnNodeTypes", {{"detail", true}});
    bool found = false;
    for (const json& t : types)
        if (t["type"] == "VoronoiTexture")
        {
            found = true;
            CHECK(t["props"].size() == 4);
            CHECK(t["inputs"].size() >= 6);
        }
    CHECK(found);
}

TEST_CASE("viewer previews a field without changing the result")
{
    run("newScene");
    std::string mesh = plane("Viewed", 4);
    run("gnBuildGroup", args({json::parse(R"({
      "name": "Look",
      "nodes": [
        {"name": "Noise", "type": "VoronoiTexture", "values": {"scale": 0.02}},
        {"name": "View", "type": "Viewer", "props": {"dataType": "float"}, "values": {"active": true}}
      ],
      "links": ["GroupInput.geometry -> View.geometry", "Noise.distance -> View.value", "GroupInput.geometry -> GroupOutput.geometry"]
    })")}));
    std::string mod = run("gnApply", args({mesh, "Look"})).get<std::string>();
    scene::MeshPtr m = outMesh(mesh);
    scene::NodeId id = scene::Graph::current().find(mod)->id();
    gn::ViewerPtr v = gn::viewerData(id);
    REQUIRE(v);
    CHECK(v->hasValues);
    CHECK(v->values.size() == v->mesh->pointCapacity());
    CHECK(v->maxValue > v->minValue);
    CHECK(m->numPoints() == 25);
    run("gnSetActiveViewer", args({"Look"}));
    outMesh(mesh);
    CHECK_FALSE(gn::viewerData(id));
}

TEST_CASE("the user's Blender VFX graph, rebuilt")
{
    run("newScene");
    std::string mesh = run("polySphere", {{"name", "Blob"}, {"radius", 50}, {"subdivisionsAxis", 24}, {"subdivisionsHeight", 16}})[0].get<std::string>();
    run("gnBuildGroup", args({boxSelectionSpec()}));
    run("gnBuildGroup", args({json::parse(R"({
      "name": "Vfx",
      "inputs": [
        {"name": "w", "type": "float", "default": 0, "label": "Evolution", "description": "Voronoi W; key it to animate.", "panel": "Noise"},
        {"name": "strength", "type": "float", "default": 1, "softMin": 0, "softMax": 10, "panel": "Noise"}
      ],
      "nodes": [
        {"name": "Keep", "type": "Group", "group": "BoxSelection", "values": {"center": [0, 50, 45], "size": [70, 70, 105]}},
        {"name": "Delete", "type": "DeleteGeometry", "props": {"domain": "point"}},
        {"name": "Move", "type": "Group", "group": "BoxSelection", "values": {"center": [0, 50, 20], "size": [50, 100, 90]}},
        {"name": "Voronoi", "type": "VoronoiTexture", "props": {"dimensions": "4D", "distance": "minkowski"},
         "values": {"scale": 0.025, "exponent": 0.3, "randomness": 0.7, "detail": 0}},
        {"name": "Mul", "type": "Math", "props": {"operation": "multiply"}},
        {"name": "Normal", "type": "Normal"},
        {"name": "Scale", "type": "VectorMath", "props": {"operation": "scale"}},
        {"name": "SetPos", "type": "SetPosition"},
        {"name": "Pos", "type": "Position"},
        {"name": "Shift", "type": "VectorMath", "props": {"operation": "add"}, "values": {"b": [0, -1480, -300]}},
        {"name": "Len", "type": "VectorMath", "props": {"operation": "length"}},
        {"name": "Range", "type": "MapRange", "values": {"fromMin": 0, "fromMax": 4610, "toMin": 0, "toMax": 1}},
        {"name": "Color", "type": "CombineColor", "values": {"green": 0, "blue": 0, "alpha": 1}},
        {"name": "Store", "type": "StoreNamedAttribute", "props": {"dataType": "color", "domain": "point"}, "values": {"name": "Col"}},
        {"name": "Pos2", "type": "Position"},
        {"name": "Unit", "type": "VectorMath", "props": {"operation": "normalize"}},
        {"name": "SetNormal", "type": "SetMeshNormal", "props": {"mode": "free", "domain": "point"}}
      ],
      "links": [
        "GroupInput.geometry -> Delete.geometry", "Keep.inverted -> Delete.selection",
        "Delete.geometry -> SetPos.geometry", "Move.selection -> SetPos.selection",
        "GroupInput.w -> Voronoi.w", "Voronoi.distance -> Mul.a", "GroupInput.strength -> Mul.b",
        "Normal.value -> Scale.a", "Mul.value -> Scale.scale", "Scale.vector -> SetPos.offset",
        "SetPos.geometry -> Store.geometry", "Pos.value -> Shift.a", "Shift.vector -> Len.a", "Len.value -> Range.value",
        "Range.result -> Color.red", "Color.color -> Store.value",
        "Store.geometry -> SetNormal.mesh", "Pos2.value -> Unit.a", "Unit.vector -> SetNormal.customNormal",
        "SetNormal.mesh -> GroupOutput.geometry"
      ]
    })")}));
    std::string mod = run("gnApply", args({mesh, "Vfx"})).get<std::string>();
    scene::Graph& g = scene::Graph::current();
    scene::Node* modNode = g.find(mod);
    int wAttr = modNode->type().attrIndex("w");
    REQUIRE(wAttr >= 0);
    CHECK(modNode->type().attrs[wAttr].niceName == "Evolution");

    scene::MeshPtr a = outMesh(mesh);
    REQUIRE(a);
    CHECK(a->numPoints() > 0);
    CHECK(geo::colors::has(*a, "Col"));
    CHECK(a->cornerAttrs().get<glm::vec3>(geo::attr::kNormal).size() == a->cornerCapacity());

    // Keyed W: different times, different shapes.
    run("setKeyframe", {{"args", json::array({mod + ".w"})}, {"time", 0.0}, {"value", 0.0}});
    run("setKeyframe", {{"args", json::array({mod + ".w"})}, {"time", 1.0}, {"value", 2.0}});
    g.setTime(0.0);
    std::vector<glm::dvec3> p0(outMesh(mesh)->positions().begin(), outMesh(mesh)->positions().end());
    g.setTime(0.5);
    std::vector<glm::dvec3> p1(outMesh(mesh)->positions().begin(), outMesh(mesh)->positions().end());
    REQUIRE(p0.size() == p1.size());
    CHECK(p0 != p1);
    g.setTime(0.0);
    std::vector<glm::dvec3> again(outMesh(mesh)->positions().begin(), outMesh(mesh)->positions().end());
    CHECK(again == p0);
}

TEST_CASE("a group rebuilt under the same name in a new scene gives new modifiers its new defaults")
{
    auto build = [](double size) {
        run("gnBuildGroup", args({json{{"name", "Sized"},
                                       {"inputs", json::array({{{"name", "size"}, {"type", "float"}, {"default", size}}})},
                                       {"links", json::array({"GroupInput.geometry -> GroupOutput.geometry"})}}}));
    };
    run("newScene");
    build(100.0);
    std::string mod = run("gnApply", args({plane("Sized", 2), "Sized"})).get<std::string>();
    CHECK(run("getAttr", args({mod + ".size"})).get<double>() == doctest::Approx(100.0));
    run("newScene");
    build(25.0);
    mod = run("gnApply", args({plane("Sized", 2), "Sized"})).get<std::string>();
    CHECK(run("getAttr", args({mod + ".size"})).get<double>() == doctest::Approx(25.0));
}

namespace
{
/** Moves the mesh up by height; with extra, a second input changes the group's interface. */
json liftSpec(bool extra)
{
    json inputs = json::array({{{"name", "height"}, {"type", "float"}, {"default", 5.0}}});
    if (extra)
        inputs.push_back({{"name", "spare"}, {"type", "float"}, {"default", 1.0}});
    return {{"name", "Lift"},
            {"inputs", inputs},
            {"nodes", json::array({{{"name", "Up"}, {"type", "CombineXYZ"}}, {{"name", "Move"}, {"type", "SetPosition"}}})},
            {"links", json::array({"GroupInput.height -> Up.z", "Up.vector -> Move.offset", "GroupInput.geometry -> Move.geometry",
                                   "Move.geometry -> GroupOutput.geometry"})}};
}
} // namespace

TEST_CASE("replacing a group's interface migrates every modifier of it, wired however they are")
{
    run("newScene");
    const std::string a = plane("LiftA", 2);
    run("gnBuildGroup", args({liftSpec(false)}));
    SUBCASE("stacked twice on one mesh")
    {
        run("gnApply", args({a, "Lift"}));
        run("gnApply", args({a, "Lift"}));
    }
    SUBCASE("on two meshes")
    {
        const std::string b = plane("LiftB", 2);
        run("gnApply", args({a, "Lift"}));
        run("gnApply", args({b, "Lift"}));
    }
    SUBCASE("a modifier with a user attr")
    {
        const std::string mod = run("gnApply", args({a, "Lift"})).get<std::string>();
        run("addAttr", {{"args", {mod}}, {"longName", "note"}, {"defaultValue", 2.0}});
        run("spaceLocator", {{"name", "Reader"}});
        run("connectAttr", args({mod + ".height", "Reader.translateY"}));
    }
    SUBCASE("a parameter driven by another node")
    {
        const std::string mod = run("gnApply", args({a, "Lift"})).get<std::string>();
        run("spaceLocator", {{"name", "Driver"}});
        run("connectAttr", args({"Driver.translateX", mod + ".height"}));
    }
    run("gnBuildGroup", args({liftSpec(true)}));
    CHECK(outMesh(a));
    run("undo");
    CHECK(outMesh(a));
    run("redo");
    CHECK(outMesh(a));
}
