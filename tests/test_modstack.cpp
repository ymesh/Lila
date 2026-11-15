#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <algorithm>
#include <stdexcept>

using namespace lila;
using json = nlohmann::json;
using namespace testutil;

namespace
{
int validFaces(const scene::MeshPtr& m)
{
    int n = 0;
    for (geo::Index f = 0; f < m->faceCapacity(); ++f)
        n += m->faceValid(f);
    return n;
}

/** An extension-style modifier kind that always fails, registered once. */
void registerFailingKind()
{
    if (scene::types::find("failTestModifier"))
        return;
    scene::NodeType t;
    t.name = "failTestModifier";
    t.owner = "tests";
    t.doc = "Always throws, to test how the stack reports a failing modifier.";
    t.ownAttrs = {
        {.name = "inMesh", .type = scene::PlugType::Mesh, .defaultValue = scene::MeshPtr(), .flags = scene::AttrInput | scene::AttrHidden},
        {.name = "enabled", .type = scene::PlugType::Bool, .defaultValue = true, .flags = scene::AttrInput | scene::AttrStorable,
         .doc = "Turn the modifier off without removing it."},
        {.name = "outMesh", .type = scene::PlugType::Mesh, .defaultValue = scene::MeshPtr(), .flags = scene::AttrOutput | scene::AttrHidden},
    };
    t.create = [] {
        auto n = std::make_unique<scene::ModifierNode>();
        n->outAttr = 2;
        n->apply = [](const geo::Mesh&, const scene::Node&) -> geo::Mesh { throw std::runtime_error("no luck"); };
        return n;
    };
    scene::types::registerType(std::move(t));
}
} // namespace

TEST_CASE("modifier kinds come from the registry")
{
    registerFailingKind();
    json kinds = run("modifierKinds");
    for (const char* k : {"mirror", "subdivision", "bevel", "array", "triangulate", "weightedNormals", "solidify", "decimate", "failTest"})
        CHECK_MESSAGE(std::find(kinds.begin(), kinds.end(), k) != kinds.end(), k);
    CHECK(std::find(kinds.begin(), kinds.end(), "skinCluster") == kinds.end());
}

TEST_CASE("modifierStack reports the source, each modifier and evaluation errors")
{
    registerFailingKind();
    run("newScene");
    std::string cube = run("polyCube")[0];
    json s = run("modifierStack", {{"args", {cube}}});
    CHECK(s["source"]["type"] == "polyCube");
    CHECK(s["modifiers"].empty());

    std::string bad = run("addModifier", {{"args", {cube}}, {"kind", "failTest"}});
    CHECK(faces(cube) == 6); // the failing modifier passes its input through
    s = run("modifierStack", {{"args", {cube}}});
    REQUIRE(s["modifiers"].size() == 1);
    CHECK(s["modifiers"][0]["name"] == bad);
    CHECK(s["modifiers"][0]["error"] == "no luck");
    CHECK(s["modifiers"][0]["exportEnabled"] == true); // the type has no exportEnabled attr

    run("setAttr", {{"args", {bad + ".enabled", false}}});
    s = run("modifierStack", {{"args", {cube}}});
    CHECK(s["modifiers"][0]["error"] == "");
}

TEST_CASE("duplicateModifier copies settings and inserts after the original")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    std::string arr = run("addModifier", {{"args", {cube}}, {"kind", "array"}, {"params", {{"count", 2}}}});
    std::string tri = run("addModifier", {{"args", {cube}}, {"kind", "triangulate"}});
    std::string copy = run("duplicateModifier", {{"args", {arr}}});
    CHECK(run("modifiers", {{"args", {cube}}}) == json::array({arr, copy, tri}));
    CHECK(run("getAttr", {{"args", {copy + ".count"}}}) == 2);
    CHECK(faces(cube) == 6 * 2 * 2 * 2);
    run("undo");
    CHECK(run("modifiers", {{"args", {cube}}}) == json::array({arr, tri}));
}

TEST_CASE("applyAllModifiers bakes the stack over an editable source")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    run("addModifier", {{"args", {cube}}, {"kind", "subdivision"}});
    run("addModifier", {{"args", {cube}}, {"kind", "array"}, {"params", {{"count", 2}}}});
    CHECK(faces(cube) == 48);
    run("applyAllModifiers", {{"args", {cube}}});
    CHECK(run("modifiers", {{"args", {cube}}}).empty());
    CHECK(faces(cube) == 48);
    json s = run("modifierStack", {{"args", {cube}}});
    CHECK(s["source"]["type"] == "meshSource");
    CHECK(run("ls", {{"type", "polyCube"}}).empty());

    // New modifiers stack on the baked base.
    run("addModifier", {{"args", {cube}}, {"kind", "triangulate"}});
    CHECK(faces(cube) == 96);
    run("undo");
    run("undo");
    CHECK(run("modifiers", {{"args", {cube}}}).size() == 2);
    CHECK(run("modifierStack", {{"args", {cube}}})["source"]["type"] == "polyCube");
}

TEST_CASE("bakeModifierStack leaves plain geometry")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    std::string sub = run("addModifier", {{"args", {cube}}, {"kind", "subdivision"}});
    run("bakeModifierStack", {{"args", {cube}}});
    CHECK(faces(cube) == 24);
    json s = run("modifierStack", {{"args", {cube}}});
    CHECK(s["source"].is_null());
    CHECK(s["modifiers"].empty());
    CHECK(run("ls", {{"type", "polyCube"}}).empty());
    CHECK(run("ls", {{"type", "subdivisionModifier"}}).empty());
    run("undo");
    CHECK(run("modifiers", {{"args", {cube}}}) == json::array({sub}));
    CHECK(faces(cube) == 24);
}

TEST_CASE("modifiers turned off for export are skipped by exporters only")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    std::string sub = run("addModifier", {{"args", {cube}}, {"kind", "subdivision"}});
    std::string arr = run("addModifier", {{"args", {cube}}, {"kind", "array"}, {"params", {{"count", 2}}}});
    scene::Graph& g = scene::Graph::current();
    scene::NodeId mesh = g.find(cube)->id();
    CHECK(validFaces(scene::modifiers::exportMesh(g, mesh)) == 48);

    run("setAttr", {{"args", {sub + ".exportEnabled", false}}});
    CHECK(faces(cube) == 48);
    CHECK(validFaces(scene::modifiers::exportMesh(g, mesh)) == 12);

    // Off in the viewport but on for export: the export runs it.
    run("setAttr", {{"args", {sub + ".exportEnabled", true}}});
    run("setAttr", {{"args", {sub + ".enabled", false}}});
    CHECK(faces(cube) == 12);
    CHECK(validFaces(scene::modifiers::exportMesh(g, mesh)) == 48);
    (void)arr;
}

TEST_CASE("a stack re-run for export hands back the same mesh until something it is made from changes")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    std::string sub = run("addModifier", {{"args", {cube}}, {"kind", "subdivision"}});
    scene::Graph& g = scene::Graph::current();
    scene::NodeId mesh = g.find(cube)->id();
    run("setAttr", {{"args", {sub + ".enabled", false}}});

    // The same object, so what is kept per mesh downstream (bake snapshots, subdivided highs) is found again.
    const scene::MeshPtr first = scene::modifiers::exportMesh(g, mesh);
    REQUIRE(validFaces(first) == 24);
    CHECK(scene::modifiers::exportMesh(g, mesh) == first);
    run("select", {{"args", {cube}}});
    CHECK(scene::modifiers::exportMesh(g, mesh) == first);

    // A setting of the re-run modifier.
    run("setAttr", {{"args", {sub + ".levels", 2}}});
    const scene::MeshPtr finer = scene::modifiers::exportMesh(g, mesh);
    CHECK(finer != first);
    CHECK(validFaces(finer) == 96);
    run("undo");
    CHECK(validFaces(scene::modifiers::exportMesh(g, mesh)) == 24);

    // The mesh under the stack.
    const scene::MeshPtr before = scene::modifiers::exportMesh(g, mesh);
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    run("moveComponents", {{"by", {0.0, 5.0, 0.0}}});
    const scene::MeshPtr moved = scene::modifiers::exportMesh(g, mesh);
    CHECK(moved != before);
    CHECK(moved->bounds().max.y != doctest::Approx(before->bounds().max.y));
    run("selectMode", {{"args", {"object"}}});

    // Another modifier joining the stack.
    run("addModifier", {{"args", {cube}}, {"kind", "array"}, {"params", {{"count", 2}}}});
    CHECK(validFaces(scene::modifiers::exportMesh(g, mesh)) == 48);
}

TEST_CASE("meshOf and stackSource walk the chain")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    std::string a = run("addModifier", {{"args", {cube}}, {"kind", "mirror"}});
    std::string b = run("addModifier", {{"args", {cube}}, {"kind", "bevel"}});
    scene::Graph& g = scene::Graph::current();
    scene::NodeId mesh = g.find(cube)->id();
    CHECK(scene::modifiers::meshOf(g, g.find(a)->id()) == mesh);
    CHECK(scene::modifiers::meshOf(g, g.find(b)->id()) == mesh);
    scene::NodeId src = scene::modifiers::stackSource(g, mesh);
    REQUIRE(src);
    CHECK(g.node(src)->type().name == "polyCube");
    CHECK(scene::modifiers::meshOf(g, src) == mesh);
}

TEST_CASE("bakeModifierStack bakes generator and every modifier")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    run("addModifier", {{"args", {cube}}, {"kind", "subdivision"}});
    run("addModifier", {{"args", {cube}}, {"kind", "array"}, {"params", {{"count", 2}}}});
    std::string locator = run("spaceLocator");
    run("bakeModifierStack", {{"args", {cube, locator}}});
    CHECK(faces(cube) == 48);
    CHECK(run("modifiers", {{"args", {cube}}}).empty());
    CHECK(run("ls", {{"type", "polyCube"}}).empty());
    CHECK(run("ls", {{"type", "subdivisionModifier"}}).empty());
    run("undo");
    CHECK(run("modifiers", {{"args", {cube}}}).size() == 2);
}
