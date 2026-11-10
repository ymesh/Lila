#include <doctest/doctest.h>

#include "TestUtil.h"
#include "bake/Arrange.h"
#include "bake/Bake.h"
#include "bake/BakeInputs.h"
#include "bake/BakeNodes.h"
#include "bake/BakePipeline.h"
#include "bake/Cage.h"
#include "bake/HighFiles.h"
#include "bake/Lint.h"
#include "bake/Targets.h"
#include "bake/TexelRaster.h"
#include "fbx/Fbx.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "geo/ObjIO.h"
#include "geo/Primitives.h"
#include "io/SceneFile.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "seq/Alembic.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>

using namespace lila;
using namespace baketest;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
std::string smallProject(const fs::path& dir, int res = 32, const std::string& name = "{group}_{map}")
{
    const std::string p = ops::execute("createBakeProject", {{"name", "P"}}).get<std::string>();
    ops::execute("setBakeOutput", {{"project", p}, {"folder", dir.generic_string()}, {"resolution", res}, {"supersampling", 1}, {"format", "exr"},
                                   {"name", name}, {"padding", 2}});
    return p;
}

std::vector<json> issues(const json& report, const std::string& check)
{
    std::vector<json> out;
    for (const json& i : report["issues"])
        if (i["check"] == check)
            out.push_back(i);
    return out;
}

json lintOnly(const std::string& p, const std::string& check) { return ops::execute("bakeLint", {{"project", p}, {"checks", {check}}}); }
} // namespace

TEST_CASE("groups hold lows, highs, cages and projects by node: renames follow, deletes undo, files round trip")
{
    newScene();
    scene::Graph& g = graph();
    const std::string low = addMesh("crate_low", plane());
    const std::string high = addMesh("crate_high", lifted(plane(4), 1.0));
    const std::string cage = addMesh("crate_cage", lifted(plane(), 2.0));
    const std::string p = ops::execute("createBakeProject", {{"name", "P"}}).get<std::string>();
    ops::execute("bakeGroup", {{"args", {"crate"}}, {"project", p}, {"lows", {low}}, {"highs", {high}}, {"cage", cage}});

    g.rename(g.find(low)->id(), "box_low");
    g.rename(g.find(high)->id(), "box_high");
    g.rename(g.find(cage)->id(), "box_cage");
    g.rename(g.find(p)->id(), "Q");
    scene::Node* grp = g.find("crate");
    bake::GroupSettings s = bake::groupSettings(*grp);
    CHECK(s.lows == std::vector<std::string>{"box_low"});
    REQUIRE(s.highs.size() == 1);
    CHECK(s.highs[0].node == "box_high");
    CHECK(s.cage == "box_cage");
    REQUIRE(bake::groupProject(g, *grp));
    CHECK(bake::groupProject(g, *grp)->name() == "Q");
    CHECK(bake::groupsOf(g, g.find("Q")->id()).size() == 1);
    CHECK(ops::execute("bake", {{"project", "Q"}, {"dryRun", true}})["outputs"].size() > 0);

    // A deleted low drops out; undo brings it back.
    {
        UndoChunk chunk("delete");
        g.deleteNode(g.find("box_low")->id());
    }
    CHECK(bake::groupSettings(*g.find("crate")).lows.empty());
    UndoStack::get().undo();
    CHECK(bake::groupSettings(*g.find("crate")).lows == std::vector<std::string>{"box_low"});

    for (const char* ext : {".lila", ".lilaa"})
    {
        const fs::path file = tempDir("lila_bakegrp_refs") / (std::string("scene") + ext);
        ops::execute("saveScene", {{"args", {file.generic_string()}}});
        newScene();
        ops::execute("openScene", {{"args", {file.generic_string()}}, {"force", true}});
        scene::Graph& g2 = graph();
        g2.rename(g2.find("box_low")->id(), "renamed_after_load");
        const bake::GroupSettings t = bake::groupSettings(*g2.find("crate"));
        CHECK(t.lows == std::vector<std::string>{"renamed_after_load"});
        CHECK(t.highs[0].node == "box_high");
        CHECK(bake::groupProject(g2, *g2.find("crate"))->name() == "Q");
        g2.rename(g2.find("renamed_after_load")->id(), "box_low");
    }
}

TEST_CASE("version 1 group settings, which named the project only in its attr, still load")
{
    newScene();
    scene::Graph& g = graph();
    const std::string low = addMesh("old_low", plane());
    ops::execute("createBakeProject", {{"name", "OldProject"}});
    scene::Node* grp = g.createNode(*scene::types::find(bake::kGroupType), "oldGroup");
    g.setValue({grp->id(), grp->type().attrIndex("project"), -1}, std::string("OldProject"));
    const json v1 = {{"kind", "bakeGroupSettings"},
                     {"value", {{"version", 1}, {"lows", {"old_low"}}, {"highs", {{{"self", true}}}}, {"cage", ""}, {"frontal", 2.0}}}};
    const auto* loader = scene::data::kinds().find("bakeGroupSettings");
    REQUIRE(loader);
    g.setValue({grp->id(), grp->type().attrIndex("settings"), -1}, (*loader)(v1));
    REQUIRE(bake::groupProject(g, *grp));
    CHECK(bake::groupProject(g, *grp)->name() == "OldProject");
    CHECK(bake::groupSettings(*grp).lows == std::vector<std::string>{"old_low"});
    CHECK(bake::groupSettings(*grp).frontal == 2.0);
    g.rename(g.find("old_low")->id(), "new_low");
    CHECK(bake::groupSettings(*grp).lows == std::vector<std::string>{"new_low"});
    CHECK(bake::groupSettings(*grp).toJson()["version"] == bake::kSettingsVersion);
}

TEST_CASE("an island bakes into the UDIM tile holding its centre, even where it spills past the edge")
{
    bake::RasterSettings rs;
    rs.width = rs.height = 64;
    rs.supersampling = 1;
    auto udims = [&](const geo::Mesh& m) { return bake::TexelRaster(bake::lowFromMesh(m, glm::dmat4(1.0), "m"), rs).udims(); };
    CHECK(udims(plane(16, {0.0f, 0.0f}, {1.05f, 1.0f})) == std::vector<int>{1001});
    CHECK(udims(plane(16, {-0.05f, -0.05f}, {0.9f, 0.9f})) == std::vector<int>{1001});
    CHECK(udims(plane(2, {1.2f, 0.0f}, {1.8f, 1.0f})) == std::vector<int>{1002});
}

TEST_CASE("skew blends ray directions from the smooth normals to the face normal")
{
    geo::SphereOptions so;
    so.subdivAxis = so.subdivHeight = 8;
    const bake::LowInput low = bake::lowFromMesh(geo::makeSphere(so), glm::dmat4(1.0), "ball");
    bake::RasterSettings rs;
    rs.width = rs.height = 64;
    rs.supersampling = 1;
    const bake::TexelRaster raster(low, rs);
    REQUIRE(!raster.udims().empty());
    auto samples = [&](float skew) {
        std::vector<bake::TexelSample> out;
        for (size_t b = 0; b < raster.bandCount(); ++b)
            raster.band(raster.udims().front(), b, bake::CageParams{2.0f, 2.0f, 1.0f, skew}, out);
        return out;
    };
    const auto smooth = samples(0.0f), flat = samples(1.0f);
    REQUIRE(smooth.size() == flat.size());
    REQUIRE(!flat.empty());
    double worstFlat = 0.0, differs = 0.0;
    for (size_t i = 0; i < flat.size(); i += 7)
    {
        const bake::TexelSample& s = flat[i];
        const glm::vec3* tri = &low.soup.positions[size_t(s.lowTriangle) * 3];
        const glm::vec3 gn = glm::normalize(glm::cross(tri[1] - tri[0], tri[2] - tri[0]));
        const glm::vec3 d(s.direction[0], s.direction[1], s.direction[2]);
        worstFlat = std::max(worstFlat, double(glm::length(d + gn)));
        const glm::vec3 ds(smooth[i].direction[0], smooth[i].direction[1], smooth[i].direction[2]);
        differs = std::max(differs, double(glm::length(ds - d)));
        // The ray still passes through the low point.
        const glm::vec3 o(s.origin[0], s.origin[1], s.origin[2]), P(s.position[0], s.position[1], s.position[2]);
        CHECK(glm::length(o + d * 2.0f - P) < 1e-3f);
    }
    CHECK(worstFlat < 1e-4);
    CHECK(differs > 0.01);
}

TEST_CASE("editable cages: create from the painted auto cage, validate, paint into, reset")
{
    newScene();
    scene::Graph& g = graph();
    const std::string low = addMesh("tile_low", plane(2));
    const std::string p = ops::execute("createBakeProject", {{"name", "P"}}).get<std::string>();
    ops::execute("bakeGroup", {{"args", {"tile"}}, {"project", p}, {"lows", {low}}, {"self", true}, {"frontal", 2.0}});

    const json made = ops::execute("bakeCage", {{"args", {"tile"}}, {"create", true}});
    const std::string cage = made["cage"].get<std::string>();
    CHECK(bake::groupSettings(*g.find("tile")).cage == cage);
    const scene::MeshPtr lm = baseMesh(low), cm = baseMesh(cage);
    CHECK(bake::cageMismatch(*lm, *cm).empty());
    for (geo::Index i = 0; i < lm->pointCapacity(); ++i)
        CHECK(glm::length(cm->position(i) - (lm->position(i) + glm::dvec3(0.0, 2.0, 0.0))) < 1e-6);
    CHECK(ops::execute("bakeCage", {{"args", {"tile"}}})["matches"] == true);
    CHECK(!bake::cageMismatch(*lm, geo::makeCube()).empty());

    // Paint the low's offset to 2 and bake it into the cage: the cage moves twice as far out.
    ops::execute("bakePaint", {{"args", {low}}, {"op", "set"}, {"value", 2.0}, {"flood", true}});
    ops::execute("bakeCage", {{"args", {"tile"}}, {"fromPaint", true}});
    const scene::MeshPtr cm2 = baseMesh(cage);
    for (geo::Index i = 0; i < lm->pointCapacity(); ++i)
        CHECK(cm2->position(i).y == doctest::Approx(4.0));
    UndoStack::get().undo();
    CHECK(baseMesh(cage)->position(0).y == doctest::Approx(2.0));

    // The bake projects from the cage: rays start on it.
    const bake::BakeInputs in = bake::buildInputs(g, g.find(p)->id(), {}, false);
    REQUIRE(in.groups.size() == 1);
    CHECK(in.groups[0].lows[0].cagePositions.size() == in.groups[0].lows[0].soup.positions.size());

    ops::execute("bakeCage", {{"args", {"tile"}}, {"reset", true}});
    CHECK(bake::groupSettings(*g.find("tile")).cage.empty());
    CHECK(g.find(cage) != nullptr);
    UndoStack::get().undo();
    CHECK(bake::groupSettings(*g.find("tile")).cage == cage);
    ops::execute("bakeCage", {{"args", {"tile"}}, {"reset", true}, {"delete", true}});
    CHECK(g.find(cage) == nullptr);

    // Two lows can't share one cage.
    ops::execute("bakeGroup", {{"args", {"tile"}}, {"addLow", {addMesh("other_low", plane())}}});
    CHECK_THROWS_AS(ops::execute("bakeCage", {{"args", {"tile"}}, {"create", true}}), CommandError);
}

TEST_CASE("bakePaint: add, subtract, set, smooth, flood, symmetry, one undo step each")
{
    newScene();
    const std::string low = addMesh("paint_low", plane(4));
    auto values = [&](const char* attr = "offset") { return bake::pointValues(*baseMesh(low), *bake::paintAttr(attr)); };
    CHECK(values()[0] == 1.0f);

    ops::execute("bakePaint", {{"args", {low}}, {"op", "set"}, {"value", 0.5}, {"flood", true}});
    for (float v : values())
        CHECK(v == doctest::Approx(0.5f));
    ops::execute("bakePaint", {{"args", {low}}, {"op", "add"}, {"value", 1.0}, {"points", {0}}});
    CHECK(values()[0] == doctest::Approx(1.5f));
    ops::execute("bakePaint", {{"args", {low}}, {"op", "subtract"}, {"value", 2.0}, {"points", {0}}});
    CHECK(values()[0] == doctest::Approx(0.0f)); // offsets stop at 0

    // A world-space dab with symmetry across X paints the mirror side the same.
    const scene::MeshPtr m = baseMesh(low);
    geo::Index right = geo::kInvalid, left = geo::kInvalid;
    for (geo::Index i = 0; i < m->pointCapacity(); ++i)
    {
        const glm::dvec3 q = m->position(i);
        if (std::abs(q.x - 25.0) < 1e-6 && std::abs(q.z) < 1e-6)
            right = i;
        if (std::abs(q.x + 25.0) < 1e-6 && std::abs(q.z) < 1e-6)
            left = i;
    }
    REQUIRE(right != geo::kInvalid);
    REQUIRE(left != geo::kInvalid);
    ops::execute("bakePaint", {{"args", {low}}, {"op", "set"}, {"value", 3.0}, {"center", {25.0, 0.0, 0.0}}, {"radius", 5.0}, {"symmetry", "x"}});
    CHECK(values()[right] == doctest::Approx(3.0f));
    CHECK(values()[left] == doctest::Approx(3.0f));

    // Smooth pulls the spike toward its neighbours.
    const float before = values()[right];
    ops::execute("bakePaint", {{"args", {low}}, {"op", "smooth"}, {"points", {right}}});
    CHECK(values()[right] < before);

    // Skew is its own attribute, 0 unpainted, -1 to 1.
    ops::execute("bakePaint", {{"args", {low}}, {"attr", "skew"}, {"op", "set"}, {"value", 5.0}, {"flood", true}});
    CHECK(values("skew")[3] == doctest::Approx(1.0f));
    UndoStack::get().undo();
    CHECK(values("skew")[3] == doctest::Approx(0.0f));
    CHECK(values()[right] < before);
    UndoStack::get().undo();
    CHECK(values()[right] == doctest::Approx(3.0f));

    // The painted offset reaches the bake's cage scale.
    const bake::LowInput li = bake::lowFromMesh(*baseMesh(low), glm::dmat4(1.0), "l");
    CHECK(*std::ranges::max_element(li.cageScale) == doctest::Approx(3.0f));
}

TEST_CASE("bakeLint finds each problem and bake lists its warnings first")
{
    const fs::path dir = tempDir("lila_bakegrp_lint");

    SUBCASE("hard edges off seams")
    {
        newScene();
        geo::Mesh roof = plane(2);
        for (geo::Index i = 0; i < roof.pointCapacity(); ++i)
        {
            glm::dvec3 q = roof.position(i);
            q.y = 50.0 - std::abs(q.x);
            roof.setPosition(i, q);
        }
        geo::setHardEdgesByAngle(roof, 10.0);
        geo::updateCornerNormals(roof);
        const std::string p = smallProject(dir);
        ops::execute("bakeGroup", {{"args", {"g"}}, {"lows", {addMesh("roof", roof)}}, {"self", true}});
        const auto found = issues(lintOnly(p, "hardEdges"), "hardEdges");
        REQUIRE(found.size() == 1);
        CHECK(found[0]["data"]["count"] == 2);
        newScene();
        const std::string p2 = smallProject(dir);
        ops::execute("bakeGroup", {{"args", {"g"}}, {"lows", {addMesh("flat", plane(2))}}, {"self", true}});
        CHECK(issues(lintOnly(p2, "hardEdges"), "hardEdges").empty());
    }

    SUBCASE("hard edges a weighted normals modifier keeps")
    {
        newScene();
        geo::Mesh roof = plane(2);
        for (geo::Index i = 0; i < roof.pointCapacity(); ++i)
        {
            glm::dvec3 q = roof.position(i);
            q.y = 50.0 - std::abs(q.x);
            roof.setPosition(i, q);
        }
        geo::setHardEdgesByAngle(roof, 10.0);
        geo::updateCornerNormals(roof);
        const std::string p = smallProject(dir);
        const std::string mesh = addMesh("roof", roof);
        ops::execute("bakeGroup", {{"args", {"g"}}, {"lows", {mesh}}, {"self", true}});
        // Off by default, the modifier smooths across the hard edges.
        const std::string mod = ops::execute("addModifier", {{"args", {mesh}}, {"kind", "weightedNormals"}}).get<std::string>();
        CHECK(issues(lintOnly(p, "hardEdges"), "hardEdges").empty());
        ops::execute("setAttr", {{"args", {mod + ".keepHardEdges", true}}});
        const auto found = issues(lintOnly(p, "hardEdges"), "hardEdges");
        REQUIRE(found.size() == 1);
        CHECK(found[0]["message"].get<std::string>().find("Keep Hard Edges") != std::string::npos);
    }

    SUBCASE("missing and degenerate UVs")
    {
        newScene();
        geo::Mesh m = plane(2);
        m.cornerAttrs().remove("uv0");
        geo::Mesh squashed = plane(1);
        for (glm::vec2& uv : squashed.cornerAttrs().get<glm::vec2>("uv0"))
            uv.y = 0.5f;
        const std::string p = smallProject(dir);
        ops::execute("bakeGroup", {{"args", {"g"}}, {"lows", {addMesh("bare", m), addMesh("squashed", squashed)}}, {"self", true}});
        const auto found = issues(lintOnly(p, "uvMissing"), "uvMissing");
        CHECK(found.size() == 2);
    }

    SUBCASE("overlapping UVs across groups writing one texture, not across separate textures")
    {
        newScene();
        const std::string a = addMesh("a_low", plane()), b = addMesh("b_low", plane());
        const std::string p = smallProject(dir, 32, "{project}_{map}");
        ops::execute("bakeGroup", {{"args", {"ga"}}, {"lows", {a}}, {"self", true}});
        ops::execute("bakeGroup", {{"args", {"gb"}}, {"lows", {b}}, {"self", true}});
        const auto found = issues(lintOnly(p, "uvOverlap"), "uvOverlap");
        REQUIRE(found.size() == 1);
        CHECK(found[0]["data"]["otherGroup"].get<std::string>() != found[0]["group"].get<std::string>());
        // Each overlapping island pair: island on the mesh, island on the other, cells.
        const json& pairs = found[0]["data"]["islands"];
        REQUIRE(pairs.size() == 1);
        CHECK(pairs[0][0] == 0);
        CHECK(pairs[0][1] == 0);
        CHECK(pairs[0][2] == found[0]["data"]["cells"]);
        const json r = ops::execute("bake", {{"project", p}, {"maps", {"height"}}});
        CHECK(std::ranges::any_of(r["warnings"], [](const json& w) { return w.get<std::string>().starts_with("lint uvOverlap"); }));
        CHECK(r["lint"]["warnings"].get<int>() >= 1);

        ops::execute("setBakeOutput", {{"project", p}, {"name", "{group}_{map}"}});
        CHECK(issues(lintOnly(p, "uvOverlap"), "uvOverlap").empty());
    }

    SUBCASE("texel density outliers within a mesh, not between meshes")
    {
        newScene();
        // One face of "shrunk" is its own island at a quarter of the scale of the rest of the mesh.
        geo::Mesh shrunk = plane(2);
        const geo::Index f0 = shrunk.cornerFace(0);
        for (geo::Index c : shrunk.faceCorners(f0))
            shrunk.cornerAttrs().get<glm::vec2>("uv0")[c] *= 0.25f;
        const std::string p = smallProject(dir, 64, "{mesh}_{map}");
        ops::execute("bakeGroup", {{"args", {"g"}},
                                   {"lows", {addMesh("d1", plane()), addMesh("small", plane(1, {0.0f, 0.0f}, {0.1f, 0.1f})), addMesh("shrunk", shrunk)}},
                                   {"self", true}});
        const auto found = issues(lintOnly(p, "texelDensity"), "texelDensity");
        REQUIRE(found.size() == 1);
        CHECK(found[0]["mesh"] == "shrunk");
    }

    SUBCASE("folded and mirrored islands")
    {
        newScene();
        geo::Mesh folded = plane(2);
        // Drag the centre point's UV past the top edge: the faces above it turn over inside the island.
        for (geo::Index c = 0; c < folded.cornerCapacity(); ++c)
            if (glm::length(folded.position(folded.cornerPoint(c))) < 1e-6)
                folded.cornerAttrs().get<glm::vec2>("uv0")[c] = {0.5f, 1.3f};
        geo::Mesh mirrored = plane(2);
        for (glm::vec2& uv : mirrored.cornerAttrs().get<glm::vec2>("uv0"))
            uv.x = 1.0f - uv.x;
        const std::string p = smallProject(dir, 32, "{mesh}_{map}");
        ops::execute("bakeGroup", {{"args", {"g"}}, {"lows", {addMesh("folded", folded), addMesh("mirrored", mirrored), addMesh("plain", plane(2))}}, {"self", true}});
        const auto found = issues(lintOnly(p, "mirroredTangents"), "mirroredTangents");
        CHECK(std::ranges::any_of(found, [](const json& i) { return i["mesh"] == "folded" && i["message"].get<std::string>().find("fold") != std::string::npos; }));
        CHECK(std::ranges::none_of(found, [](const json& i) { return i["mesh"] == "mirrored" || i["mesh"] == "plain"; }));
    }

    SUBCASE("problems too small to show in the map are left out")
    {
        newScene();
        // Islands packed edge to edge, and overlapping by less than a texel at 32 x 32, all in one texture.
        const std::string p = smallProject(dir, 32, "{project}_{map}");
        ops::execute("bakeGroup", {{"args", {"g"}},
                                   {"lows", {addMesh("left", plane(1, {0.0f, 0.0f}, {0.5f, 1.0f})), addMesh("right", plane(1, {0.5f, 0.0f}, {1.0f, 1.0f})),
                                             addMesh("sliver", plane(1, {0.99f, 0.0f}, {1.49f, 1.0f}))}},
                                   {"self", true}});
        CHECK(issues(lintOnly(p, "uvOverlap"), "uvOverlap").empty());

        newScene();
        // The centre point's UV pushed a hair past the top edge: the slivers above it turn over by under a texel.
        geo::Mesh nudged = plane(2);
        for (geo::Index c = 0; c < nudged.cornerCapacity(); ++c)
            if (glm::length(nudged.position(nudged.cornerPoint(c))) < 1e-6)
                nudged.cornerAttrs().get<glm::vec2>("uv0")[c] = {0.5f, 1.002f};
        geo::Mesh bent = plane(1);
        bent.setPosition(0, bent.position(0) + glm::dvec3(0.0, 20.0, 0.0));
        const std::string p2 = smallProject(dir, 32, "{mesh}_{map}");
        ops::execute("bakeGroup", {{"args", {"g"}}, {"lows", {addMesh("nudged", nudged), addMesh("bent", bent)}}, {"self", true}});
        const json r = ops::execute("bakeLint", {{"project", p2}, {"highs", false}});
        CHECK(std::ranges::none_of(issues(r, "mirroredTangents"), [](const json& i) { return i["message"].get<std::string>().find("fold") != std::string::npos; }));
        // Lila's exports triangulate as the bake does, so non-planar faces are only checked when asked for.
        CHECK(issues(r, "triangulation").empty());
    }

    SUBCASE("lows with no high in range, and highs outside the cage")
    {
        newScene();
        const std::string low = addMesh("r_low", plane(2));
        const std::string far = addMesh("r_far", lifted(plane(2), 50.0));
        const std::string p = smallProject(dir);
        ops::execute("bakeGroup", {{"args", {"g"}}, {"lows", {low}}, {"highs", {far}}, {"frontal", 1.0}, {"rear", 1.0}});
        const json r = ops::execute("bakeLint", {{"project", p}, {"checks", {"highRange", "cageContainment"}}});
        CHECK(issues(r, "highRange").size() == 1);
        CHECK(issues(r, "cageContainment").size() == 1);

        ops::execute("bakeGroup", {{"args", {"g"}}, {"highs", {addMesh("r_near", lifted(plane(2), 3.0))}}});
        const json near = ops::execute("bakeLint", {{"project", p}, {"checks", {"highRange", "cageContainment"}}});
        CHECK(issues(near, "highRange").size() == 1); // frontal 1 doesn't reach 3 cm up
        const auto out = issues(near, "cageContainment");
        REQUIRE(out.size() == 1);
        CHECK(out[0]["data"]["maxBeyond"].get<double>() == doctest::Approx(2.0).epsilon(0.01));
        ops::execute("bakeGroup", {{"args", {"g"}}, {"frontal", 5.0}});
        const json ok = ops::execute("bakeLint", {{"project", p}, {"checks", {"highRange", "cageContainment"}}});
        CHECK(ok["issues"].empty());
        // Painting the offset down pulls the cage under the high again.
        ops::execute("bakePaint", {{"args", {low}}, {"op", "set"}, {"value", 0.2}, {"flood", true}});
        CHECK(issues(ops::execute("bakeLint", {{"project", p}, {"checks", {"cageContainment"}}}), "cageContainment").size() == 1);
    }

    SUBCASE("non-planar faces and cage topology")
    {
        newScene();
        geo::Mesh bent = plane(1);
        bent.setPosition(0, bent.position(0) + glm::dvec3(0.0, 20.0, 0.0));
        const std::string p = smallProject(dir);
        ops::execute("bakeGroup", {{"args", {"g"}}, {"lows", {addMesh("bent", bent)}}, {"self", true}, {"cage", addMesh("wrong_cage", geo::makeCube())}});
        const json r = ops::execute("bakeLint", {{"project", p}, {"checks", {"triangulation", "cageTopology"}}, {"highs", false}});
        CHECK(issues(r, "triangulation").size() == 1);
        CHECK(issues(r, "cageTopology").size() == 1);
    }

    CHECK_THROWS_AS(ops::execute("bakeLint", {{"project", "P"}, {"checks", {"nope"}}}), CommandError);
}

TEST_CASE("high files: OBJ, Alembic and FBX highs stream in at bake time and are cached")
{
    newScene();
    const fs::path dir = tempDir("lila_bakegrp_files");
    auto bump = [](geo::Mesh m) {
        for (geo::Index i = 0; i < m.pointCapacity(); ++i)
            m.setPosition(i, m.position(i) + glm::dvec3(0.0, 1.0 + 0.002 * m.position(i).x, 0.0));
        return m;
    };
    const geo::Mesh high = bump(plane(8));
    const fs::path obj = dir / "high.obj";
    REQUIRE(geo::writeObjFile(obj.string(), high));

    // Alembic round trip: positions, winding and uv0 come back as written.
    const fs::path abc = dir / "high.abc";
    {
        seq::abc::Writer w;
        w.open(abc, {});
        w.addFrame({"high"}, {&high});
        w.close();
    }
    std::string err;
    const auto samples = seq::abc::readMeshes(abc, 0.0, {}, &err);
    INFO(err);
    REQUIRE(samples.size() == 1);
    const geo::Mesh& back = samples[0].mesh;
    CHECK(back.numPoints() == high.numPoints());
    CHECK(glm::length(back.bounds().max - high.bounds().max) < 1e-4);
    CHECK(geo::faceNormals(back)[0].y > 0.99);
    const auto uv = back.cornerAttrs().get<glm::vec2>("uv0");
    REQUIRE(uv.size() == back.cornerCapacity());
    for (geo::Index c = 0; c < back.cornerCapacity(); c += 5)
    {
        const glm::dvec3 q = back.position(back.cornerPoint(c));
        CHECK(uv[c].x == doctest::Approx((q.x + 50.0) / 100.0).epsilon(1e-4));
    }

    const std::string low = addMesh("f_low", plane(2));
    const std::string p = smallProject(dir);
    auto heightRange = [&](const json& r) {
        for (const json& o : r["outputs"])
            if (o["map"] == "height")
                return o["meta"]["heightRange"].get<double>();
        return -1.0;
    };
    ops::execute("bakeGroup", {{"args", {"g"}}, {"lows", {low}}, {"addHighFile", {obj.generic_string()}}, {"frontal", 3.0}, {"rear", 3.0}});
    CHECK(bake::groupSettings(*graph().find("g")).highs.at(0).file == obj.generic_string());
    bake::clearHighFileCache();
    const size_t nodes = graph().nodes().size();
    const json r1 = ops::execute("bake", {{"project", p}, {"maps", {"height"}}});
    CHECK(r1["stats"]["misses"] == 0);
    CHECK(graph().nodes().size() == nodes);
    const double expected = heightRange(r1);
    CHECK(expected == doctest::Approx(1.1).epsilon(0.02));
    ops::execute("bake", {{"project", p}, {"maps", {"height"}}});
    CHECK(bake::highFileStats().reads == 1);
    CHECK(bake::highFileStats().hits == 1);

    ops::execute("bakeGroup", {{"args", {"g"}}, {"removeHighFile", {obj.generic_string()}}, {"addHighFile", {abc.generic_string()}}});
    CHECK(heightRange(ops::execute("bake", {{"project", p}, {"maps", {"height"}}})) == doctest::Approx(expected).epsilon(0.01));

    // FBX: written by Lila's exporter from a scene mesh that is then removed.
    const std::string tmp = addMesh("fbx_high", high);
    const fs::path fbxFile = dir / "high.fbx";
    {
        fbx::ExportOptions opts;
        std::string ferr;
        const fbx::ExportScene es = fbx::gatherExport(graph(), {graph().find(tmp)->id()}, opts);
        REQUIRE(fbx::writeFbx(es, fbxFile, opts, &ferr));
        graph().deleteNode(graph().find(tmp)->id());
    }
    ops::execute("bakeGroup", {{"args", {"g"}}, {"removeHighFile", {abc.generic_string()}}, {"addHighFile", {fbxFile.generic_string()}}});
    const size_t before = graph().nodes().size();
    const json rf = ops::execute("bake", {{"project", p}, {"maps", {"height"}}});
    CHECK(rf["stats"]["misses"] == 0);
    CHECK(heightRange(rf) == doctest::Approx(expected).epsilon(0.02));
    CHECK(graph().nodes().size() == before);

    CHECK_THROWS_AS(ops::execute("bakeGroup", {{"args", {"g"}}, {"addHighFile", {(dir / "missing.obj").generic_string()}}}), CommandError);
}

TEST_CASE("one group per mesh and Arrange")
{
    newScene();
    scene::Graph& g = graph();
    const std::string rock = addMesh("rock", geo::makeCube());
    const std::string crateLow = addMesh("crate_low", geo::makeCube());
    const std::string crateHigh = addMesh("crate_high", geo::makeCube());
    ops::execute("createBakeProject", {{"name", "P"}});
    const json groups = ops::execute("bakeGroupPerMesh", {{"args", {rock, crateLow, crateHigh}}, {"preset", "Smooth x2"}});
    REQUIRE(groups.size() == 2);
    std::map<std::string, json> byLow;
    for (const json& j : groups)
        byLow[j["lows"][0].get<std::string>()] = j;
    CHECK(byLow["rock"]["highs"][0]["self"] == true);
    CHECK(byLow["rock"]["highs"][0]["modifiers"][0]["kind"] == "subdivision");
    CHECK(byLow["crate_low"]["highs"][0]["node"] == "crate_high");
    // Running again keeps the groups.
    CHECK(ops::execute("bakeGroupPerMesh", {{"args", {rock}}}).size() == 1);
    CHECK(bake::groupsOf(g, g.find("P")->id()).size() == 2);

    const glm::dvec3 t = std::get<glm::dvec3>(g.value({g.find(rock)->id(), scene::Transform::kTranslate, -1}));
    const scene::NodeId pid = g.find("P")->id();
    const std::string hash = bake::inputHash(g, pid);
    const json arranged = ops::execute("bakeArrange", {{"spacing", 10.0}});
    CHECK(arranged["arranged"] == true);
    const json offsets = arranged["offsets"];
    REQUIRE(offsets.size() == 2);
    std::vector<double> xs;
    for (const auto& [name, o] : offsets.items())
        xs.push_back(o[0].get<double>());
    std::ranges::sort(xs);
    CHECK(xs[1] - xs[0] >= 100.0 + 10.0 - 1e-6);
    // View only: the meshes stay put, nothing goes on the undo stack and the bake's inputs don't change.
    CHECK(std::get<glm::dvec3>(g.value({g.find(rock)->id(), scene::Transform::kTranslate, -1})) == t);
    CHECK(bake::inputHash(g, pid) == hash);
    CHECK(bake::arrange::active(g, pid));

    // Each group's lows and highs draw at their group's offset; meshes in no group don't move.
    const std::string loner = addMesh("loner", geo::makeCube());
    glm::dvec3 rockOffset(0.0), crateOffset(0.0);
    for (const auto& [name, o] : offsets.items())
        (name.find("rock") != std::string::npos ? rockOffset : crateOffset) = {o[0].get<double>(), o[1].get<double>(), o[2].get<double>()};
    CHECK(bake::arrange::displayOffset(g, g.find(rock)->id()) == rockOffset);
    CHECK(bake::arrange::displayOffset(g, g.find(crateLow)->id()) == crateOffset);
    CHECK(bake::arrange::displayOffset(g, g.find(crateHigh)->id()) == crateOffset);
    CHECK(bake::arrange::displayOffset(g, g.find(loner)->id()) == glm::dvec3(0.0));
    const glm::dmat4 shown = bake::arrange::displayWorld(g, g.find(crateLow)->id(), glm::dmat4(1.0));
    CHECK(glm::dvec3(shown[3]) == crateOffset);

    // A toggle: again turns it off, and reset always does.
    CHECK(ops::execute("bakeArrange", json::object())["arranged"] == false);
    CHECK(bake::arrange::displayOffset(g, g.find(crateLow)->id()) == glm::dvec3(0.0));
    CHECK(ops::execute("bakeArrange", json::object())["arranged"] == true);
    CHECK(ops::execute("bakeArrange", {{"on", true}})["arranged"] == true);
    ops::execute("bakeArrange", {{"reset", true}});
    CHECK_FALSE(bake::arrange::active(g, pid));
    CHECK(bake::arrange::displayOffset(g, g.find(rock)->id()) == glm::dvec3(0.0));

    // A new scene drops it.
    ops::execute("bakeArrange", {{"on", true}});
    newScene();
    CHECK_FALSE(bake::arrange::active(graph(), pid));
}

TEST_CASE("createBakeFromGenerator wires a ground tile hand-off into a Painter bake project")
{
    newScene();
    const fs::path dir = tempDir("lila_bakegrp_generator");
    const fs::path high = dir / "tile_Hi.obj";
    REQUIRE(geo::writeObjFile(high.string(), lifted(plane(6), 4.0)));
    const std::string bakePlane = addMesh("tile_bakePlane", plane(2));
    const bool had = ops::find("groundTileBakeData") != nullptr;
    if (!had)
        ops::registerCommand({.name = "groundTileBakeData",
                              .doc = "test stand-in",
                              .positionalDoc = "group",
                              .undoable = false,
                              .run = [=](const Args&) -> json {
                                  return {{"group", "tile"},
                                          {"high", high.generic_string()},
                                          {"low", ""},
                                          {"bakePlane", bakePlane},
                                          {"minHeight", -0.5},
                                          {"maxHeight", 6.0},
                                          {"bakePlaneHeight", 0.0},
                                          {"heightAbovePlane", 6.0},
                                          {"measured", true},
                                          {"heightP99", 4.5},
                                          {"legend", {{"1", {{"colour", {255, 0, 0}}, {"sources", {"pebble"}}}}}}};
                              }});
    const json r = ops::execute("createBakeFromGenerator", {{"args", {"tile"}}, {"kind", "groundTile"}});
    if (!had)
        ops::unregisterCommand("groundTileBakeData");
    CHECK(r["kind"] == "groundTile");
    CHECK(r["target"] == "substancePainter");
    const json& grp = r["group"];
    CHECK(grp["lows"][0] == bakePlane);
    CHECK(grp["highs"][0]["file"] == high.generic_string());
    CHECK(grp["frontal"].get<double>() == doctest::Approx(6.3));
    const bake::ProjectSettings ps = bake::projectSettings(*graph().find(r["project"].get<std::string>()));
    CHECK(ps.target == "substancePainter");
    bool height = false, id = false;
    for (const bake::MapSettings& m : ps.maps)
    {
        if (m.map == "height")
            height = m.options.value("range", 0.0) == 4.5;
        if (m.map == "id")
            id = m.options["palette"]["1"][0].get<double>() == doctest::Approx(1.0) && m.options["source"] == "vertexColor";
    }
    CHECK(height);
    CHECK(id);
    CHECK(ops::execute("bake", {{"project", r["project"]}, {"dryRun", true}})["outputs"].size() > 0);
}

TEST_CASE("bake presets: save a project's settings as a target, apply them in another scene with other meshes")
{
    newScene();
    const std::string rockLow = addMesh("rock_low", plane());
    addMesh("rock_high", lifted(plane(), 1.0));
    const std::string p = ops::execute("createBakeProject", {{"name", "P"}}).get<std::string>();
    ops::execute("setBakeOutput", {{"project", p}, {"name", "X_{mesh}_{map}"}, {"format", "tif"}, {"bits", 16}, {"resolution", 512},
                                   {"supersampling", 16}, {"padding", 8}, {"dither", true}, {"compression", "none"}});
    ops::execute("setBakeMap", {{"args", {"ao"}}, {"project", p}, {"options", {{"rays", 32}}}});
    ops::execute("setBakeMap", {{"args", {"pack"}}, {"project", p}});
    ops::execute("autoGroupBake", {{"project", p}});
    const std::string rockGroup = graph().node(bake::groupsOf(graph(), graph().find(p)->id()).front())->path();
    ops::execute("bakeGroup", {{"args", {rockGroup}}, {"frontal", 2.5}, {"rear", 1.5}, {"cageOffset", 1.3}, {"aoScope", "self"},
                               {"ignoreBackfaces", false}, {"preset", "Smooth x2"}});
    const bake::ProjectSettings saved = bake::projectSettings(*graph().find(p));

    const json r = ops::execute("saveBakeTarget", {{"args", {"Studio"}}, {"project", p}, {"backend", "cpu"}, {"overwrite", true}});
    const fs::path file = fs::path(r["file"].get<std::string>());
    REQUIRE(fs::exists(file));
    {
        std::ifstream in(file);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(text.find("rock") == std::string::npos);
    }
    CHECK_THROWS_AS(ops::execute("saveBakeTarget", {{"args", {"Studio"}}, {"project", p}}), CommandError);
    CHECK_THROWS_AS(ops::execute("saveBakeTarget", {{"args", {"substancePainter"}}, {"project", p}}), CommandError);
    const auto studio = bake::findTarget("Studio");
    REQUIRE(studio);
    CHECK(studio->preset);
    CHECK_FALSE(bake::isBuiltinTarget(*studio));
    CHECK(studio->backend == "cpu");

    // A fresh scene: other meshes, and a group with a distance set by hand before the preset arrives.
    newScene();
    const std::string crateLow = addMesh("crate_low", plane());
    const std::string crateHigh = addMesh("crate_high", lifted(plane(), 1.0));
    addMesh("bolt_low", plane());
    addMesh("bolt_high", lifted(plane(), 1.0));
    const std::string q = ops::execute("createBakeProject", {{"name", "Q"}}).get<std::string>();
    ops::execute("bakeGroup", {{"args", {"hand"}}, {"project", q}, {"lows", {crateLow}}, {"addHigh", {crateHigh}}, {"frontal", 7.0}});

    const json applied = ops::execute("applyBakeTarget", {{"args", {"Studio"}}, {"project", q}});
    CHECK(applied["backend"] == "cpu");
    const bake::ProjectSettings got = bake::projectSettings(*graph().find(q));
    CHECK(got.output.toJson() == saved.output.toJson());
    REQUIRE(got.maps.size() == saved.maps.size());
    for (size_t i = 0; i < got.maps.size(); ++i)
        CHECK(got.maps[i].toJson() == saved.maps[i].toJson());
    CHECK(got.target == "Studio");

    const bake::GroupSettings hand = bake::groupSettings(*graph().find("hand"));
    CHECK(hand.frontal == doctest::Approx(7.0));
    CHECK(hand.rear == doctest::Approx(1.5));
    CHECK(hand.cageOffset == doctest::Approx(1.3));
    CHECK(hand.aoScope == "self");
    CHECK_FALSE(hand.ignoreBackfaces);
    REQUIRE(hand.highs.size() == 1);
    CHECK(hand.highs[0].modifiers == bake::modifierPreset("Smooth x2"));
    CHECK(hand.lows == std::vector<std::string>{crateLow});

    // Groups made later start from the defaults; no mesh of the first scene ever appears.
    ops::execute("autoGroupBake", {{"project", q}});
    const auto groups = bake::groupsOf(graph(), graph().find(q)->id());
    REQUIRE(groups.size() == 2);
    for (scene::NodeId id : groups)
    {
        const bake::GroupSettings s = bake::groupSettings(*graph().node(id));
        for (const std::string& l : s.lows)
            CHECK(l.find("rock") == std::string::npos);
        if (graph().node(id)->name() == "hand")
            continue;
        CHECK(s.frontal == doctest::Approx(2.5));
        CHECK(s.aoScope == "self");
        REQUIRE(s.highs.size() == 1);
        CHECK_FALSE(s.highs[0].self);
        CHECK(s.highs[0].modifiers == bake::modifierPreset("Smooth x2"));
    }

    // overrideGroups replaces hand-made settings too; undo puts them back.
    ops::execute("applyBakeTarget", {{"args", {"Studio"}}, {"project", q}, {"overrideGroups", true}});
    CHECK(bake::groupSettings(*graph().find("hand")).frontal == doctest::Approx(2.5));
    REQUIRE(UndoStack::get().undo());
    CHECK(bake::groupSettings(*graph().find("hand")).frontal == doctest::Approx(7.0));

    CHECK_THROWS_AS(ops::execute("deleteBakeTarget", {{"args", {"substancePainter"}}}), CommandError);
    ops::execute("deleteBakeTarget", {{"args", {"Studio"}}});
    CHECK_FALSE(fs::exists(file));
    CHECK_FALSE(bake::findTarget("Studio"));
}
