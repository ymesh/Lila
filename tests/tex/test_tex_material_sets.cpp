#include "TexTest.h"

#include "geo/Attribute.h"
#include "geo/Mesh.h"
#include "link/TextureSetSend.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "tex/PaintSurface.h"
#include "tex/TextureSet.h"

using namespace textest;

namespace
{
/** Three quads, each on its own material slot: A, B and C. */
geo::Mesh threeSlots()
{
    const std::vector<glm::dvec3> pts = {{0, 0, 0}, {10, 0, 0}, {10, 0, 10}, {0, 0, 10}, {20, 0, 0}, {20, 0, 10}, {30, 0, 0}, {30, 0, 10}};
    const std::vector<geo::Index> sizes = {4, 4, 4}, idx = {0, 3, 2, 1, 1, 2, 5, 4, 4, 5, 7, 6};
    geo::Mesh mesh = geo::Mesh::fromPolygons(pts, sizes, idx);
    auto slot = mesh.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
    for (geo::Index f = 0; f < 3; ++f)
        slot[f] = int32_t(f);
    mesh.materialNames() = {"A", "B", "C"};
    return mesh;
}

size_t paintable(const tex::paint::PaintMesh& m)
{
    size_t n = 0;
    for (uint32_t t = 0; t < m.triangleCount(); ++t)
        n += m.paintable[t] ? 1 : 0;
    return n;
}

const scene::Node& material(const char* name) { return *scene::materials::find(scene::Graph::current(), name); }
} // namespace

TEST_CASE("tex material sets: materials pick a set, whole or one map at a time")
{
    run("newScene");
    scene::Graph& g = scene::Graph::current();
    run("texCreateSet", {{"args", {"Hull"}}, {"materials", {"A", "B"}}});
    CHECK(tex::setForMaterial(g, "A") == "Hull");
    CHECK(tex::setForMaterial(g, "B") == "Hull");
    CHECK(tex::materialsUsing(g, "Hull") == std::vector<std::string>{"A", "B"});
    // The set knows no material.
    CHECK_THROWS_AS(run("texSetSettings", {{"args", {"Hull", {{"material", "A"}}}}}), CommandError);

    run("texCreateSet", {{"args", {"Trim"}}});
    run("createMaterial", {{"args", {"C"}}});
    CHECK(tex::setForMaterial(g, "C").empty());
    run("setMaterialTexture", {{"args", {"C"}}, {"channel", "roughness"}, {"textureSet", "Trim"}});
    CHECK(scene::materials::textureSetFor(material("C"), "roughness") == "Trim");
    CHECK(scene::materials::textureSetFor(material("C"), "baseColor").empty());
    CHECK(tex::setForMaterial(g, "C") == "Trim");
    CHECK(tex::materialsUsing(g, "Trim") == std::vector<std::string>{"C"});
    // A per-map pick wins over the whole set for that map.
    run("setAttr", {{"args", {"C.textureSet", "Hull"}}});
    CHECK(scene::materials::textureSetFor(material("C"), "roughness") == "Trim");
    CHECK(scene::materials::textureSetFor(material("C"), "baseColor") == "Hull");
    CHECK_THROWS_AS(run("setMaterialTexture", {{"args", {"C"}}, {"channel", "normal"}, {"textureSet", "Nothing"}}), CommandError);

    // Renaming a set takes its materials along, and undo takes them back.
    run("rename", {{"args", {"Trim", "Edge"}}});
    CHECK(scene::materials::textureSetFor(material("C"), "roughness") == "Edge");
    run("rename", {{"args", {"Hull", "Body"}}});
    CHECK(scene::materials::textureSet(material("A")) == "Body");
    run("undo");
    CHECK(scene::materials::textureSet(material("A")) == "Hull");

    run("setMaterialTexture", {{"args", {"C"}}, {"channel", "roughness"}, {"textureSet", ""}});
    CHECK(scene::materials::textureSetMaps(material("C")).empty());
}

TEST_CASE("tex material sets: the paint mesh takes the faces of every material using the set")
{
    const geo::Mesh mesh = threeSlots();
    auto bvh = std::make_shared<const geo::MeshBvh>(mesh);
    CHECK(paintable(*tex::paint::PaintMesh::build(mesh, bvh, "B")) == 2);
    const std::string key = std::string("A") + tex::paint::kMaterialSeparator + "C";
    CHECK(paintable(*tex::paint::PaintMesh::build(mesh, bvh, key)) == 4);
    // A key naming no slot of the mesh paints all of it, as one material does.
    CHECK(paintable(*tex::paint::PaintMesh::build(mesh, bvh, "D;E")) == 6);
}

TEST_CASE("tex material sets: each material using the set gets its Unreal instance with the maps it takes")
{
    const json result = {{"outputs",
                          {{{"file", "C:/t/T_Hull_BaseColor.png"}, {"udim", 1001}, {"map", "baseColor"}},
                           {{"file", "C:/t/T_Hull_ORM.png"}, {"udim", 1001}, {"map", "orm"}}}}};
    const json options = {{"folder", "/Game/T"}, {"materialParent", "/Game/M_Base"}, {"materials", {{"A", nullptr}, {"C", {"roughness", "orm", "mro"}}}}};
    const json params = link::textureSetParams(result, {{"name", "A"}}, options, true);
    REQUIRE(params["materials"].size() == 2);
    for (const json& m : params["materials"])
        if (m["slot"] == "A")
            CHECK(m["parameters"].size() == 2);
        else
        {
            CHECK(m["slot"] == "C");
            CHECK(m["parameters"].size() == 1);
            CHECK(m["parameters"].contains("ORMMap"));
        }
}
