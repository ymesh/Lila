#include <doctest/doctest.h>

#include "extensions/ExtensionManager.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopLoc_Location.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Trsf.hxx>

#include <filesystem>
#include <set>

using namespace lila;
namespace fs = std::filesystem;

namespace
{
void loadExtension()
{
    if (!ops::find("importCad"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_CAD_PACKAGE) == "CadImport");
}

/** "Gearbox": a red 10 mm box placed twice (at 0 and 100 mm along X) and a blue shaft, as a STEP assembly. */
fs::path writeGearbox(double boxSize)
{
    Handle(TDocStd_Document) doc;
    XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", doc);
    Handle(XCAFDoc_ShapeTool) shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    Handle(XCAFDoc_ColorTool) colors = XCAFDoc_DocumentTool::ColorTool(doc->Main());
    const TDF_Label box = shapes->AddShape(BRepPrimAPI_MakeBox(boxSize, boxSize, boxSize).Shape(), false);
    TDataStd_Name::Set(box, "Box");
    colors->SetColor(box, Quantity_Color(1.0, 0.0, 0.0, Quantity_TOC_RGB), XCAFDoc_ColorSurf);
    const TDF_Label shaft = shapes->AddShape(BRepPrimAPI_MakeCylinder(5.0, 50.0).Shape(), false);
    TDataStd_Name::Set(shaft, "Shaft");
    colors->SetColor(shaft, Quantity_Color(0.0, 0.0, 1.0, Quantity_TOC_RGB), XCAFDoc_ColorSurf);
    const TDF_Label gearbox = shapes->NewShape();
    TDataStd_Name::Set(gearbox, "Gearbox");
    gp_Trsf right, up;
    right.SetTranslation(gp_Vec(100.0, 0.0, 0.0));
    up.SetTranslation(gp_Vec(0.0, 50.0, 0.0));
    shapes->AddComponent(gearbox, box, TopLoc_Location());
    shapes->AddComponent(gearbox, box, TopLoc_Location(right));
    shapes->AddComponent(gearbox, shaft, TopLoc_Location(up));
    shapes->UpdateAssemblies();
    const fs::path path = fs::temp_directory_path() / "lila_cad" / "gearbox.step";
    fs::create_directories(path.parent_path());
    STEPCAFControl_Writer w;
    w.SetNameMode(true);
    w.SetColorMode(true);
    REQUIRE(w.Transfer(doc, STEPControl_AsIs));
    REQUIRE(w.Write(path.string().c_str()) == IFSelect_RetDone);
    return path;
}

std::vector<scene::Node*> meshesUnder(scene::Graph& g, scene::NodeId root)
{
    std::vector<scene::Node*> out;
    for (scene::NodeId id : g.descendants(root))
        if (scene::Node* n = g.node(id); n && n->type().isA("mesh"))
            out.push_back(n);
    return out;
}
} // namespace

TEST_CASE("a STEP assembly imports with its tree, names, colours and millimetres, and re-imports in place")
{
    loadExtension();
    const fs::path path = writeGearbox(10.0);
    const json r = ops::execute("importCad", {{"args", {path.string()}}, {"chordal", 0.01}});
    CHECK(r["parts"].get<int>() == 3);
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const scene::Node* root = g.find(r["root"].get<std::string>());
    REQUIRE(root);
    std::vector<scene::Node*> parts = meshesUnder(g, root->id());
    REQUIRE(parts.size() == 3);
    std::set<std::string> names;
    for (const scene::Node* n : parts)
        names.insert(n->name().substr(0, 5));
    CHECK(names.contains("Box"));
    CHECK(names.contains("Shaft"));
    CHECK(scene::materials::find(g, "CAD_FF0000"));
    CHECK(scene::materials::find(g, "CAD_0000FF"));

    // The box: 10 mm is 1 cm on a side, its eight corners welded, each corner keeping three face normals.
    const scene::Node* box = nullptr;
    for (const scene::Node* n : parts)
        if (n->name().rfind("Box", 0) == 0 && !box)
            box = n;
    REQUIRE(box);
    const scene::MeshPtr m = box->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    REQUIRE(m);
    CHECK(m->numPoints() == 8);
    CHECK(m->numFaces() == 12);
    glm::dvec3 lo(1e300), hi(-1e300);
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
        if (m->pointValid(p))
            lo = glm::min(lo, m->position(p)), hi = glm::max(hi, m->position(p));
    CHECK(glm::length((hi - lo) - glm::dvec3(1.0)) < 1e-9);
    const std::span<const glm::vec3> normals = m->cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    REQUIRE(!normals.empty());
    std::set<std::tuple<int, int, int>> atCorner;
    for (geo::Index f = 0; f < m->faceCapacity(); ++f)
        if (m->faceValid(f))
        {
            geo::Index c = m->faceCorner(f);
            for (int k = 0; k < 3; ++k, c = m->cornerNext(c))
                if (m->cornerPoint(c) == 0)
                {
                    const glm::vec3 n = normals[size_t(c)];
                    atCorner.insert({int(std::round(n.x)), int(std::round(n.y)), int(std::round(n.z))});
                }
        }
    CHECK(atCorner.size() == 3);

    // The second box sits 10 cm along X from the first, in CAD's Z-up frame under the root's turn.
    std::vector<glm::dvec3> boxes;
    for (const scene::Node* n : parts)
        if (n->name().rfind("Box", 0) == 0)
            boxes.push_back(glm::dvec3(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]));
    REQUIRE(boxes.size() == 2);
    CHECK(glm::length(boxes[1] - boxes[0]) == doctest::Approx(10.0));

    // Re-import a changed file: the same nodes, new geometry (a 20 mm box), nothing added.
    std::set<scene::NodeId> before;
    for (const scene::Node* n : parts)
        before.insert(n->id());
    writeGearbox(20.0);
    const json again = ops::execute("importCad", {{"args", {path.string()}}});
    CHECK(again["new"].get<int>() == 0);
    CHECK(again["updated"].get<int>() == 4); // the Gearbox group and its three parts
    CHECK(again["root"] == r["root"]);
    g.evaluate();
    std::vector<scene::Node*> after = meshesUnder(g, root->id());
    REQUIRE(after.size() == 3);
    for (const scene::Node* n : after)
        CHECK(before.contains(n->id()));
    const scene::MeshPtr bigger = box->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    glm::dvec3 lo2(1e300), hi2(-1e300);
    for (geo::Index p = 0; p < bigger->pointCapacity(); ++p)
        if (bigger->pointValid(p))
            lo2 = glm::min(lo2, bigger->position(p)), hi2 = glm::max(hi2, bigger->position(p));
    CHECK(glm::length((hi2 - lo2) - glm::dvec3(2.0)) < 1e-9);
}
