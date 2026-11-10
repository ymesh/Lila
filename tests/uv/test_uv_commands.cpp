#include "TestUtil.h"
#include "geo/Topology.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/LastOp.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "uv/Chart.h"
#include "uv/Uv.h"
#include "uv/UvCommands.h"

#include <doctest/doctest.h>

#include <algorithm>

using namespace lila;
using namespace uvtest;

TEST_CASE("uv commands on a cube")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    run("select", {{"args", {cube}}});

    json r = run("uvAuto");
    CHECK(r["ok"] == true);
    CHECK(r["islands"].get<int>() <= 6);
    json info = run("uvLayout");
    CHECK(info["overlappingFaces"] == 0);
    CHECK(info["foldedFaces"] == 0);
    CHECK(info["islands"].get<int>() == r["islands"].get<int>());
    run("undo");
    run("redo");

    run("uvProject", {{"type", "box"}});
    CHECK(run("uvLayout")["islands"] == 6);
    run("uvPack", {{"resolution", 512}, {"padding", 8}});
    CHECK(run("uvLayout")["overlappingFaces"] == 0);
    run("uvOrient");
    run("uvStraighten");

    double td = run("uvTexelDensity", {{"resolution", 1024}}).get<double>();
    CHECK(td > 0.0);
    run("uvTexelDensity", {{"resolution", 1024}, {"density", 2.0}});
    CHECK(run("uvTexelDensity", {{"resolution", 1024}}).get<double>() == doctest::Approx(2.0).epsilon(1e-3));

    json lm = run("uvLightmap", {{"resolution", 64}});
    CHECK(lm["ok"] == true);
    CHECK(run("uvLayout", {{"uvSet", "uv1"}})["overlappingFaces"] == 0);
    CHECK(run("uvSetCurrent") == "uv0");

    // Component edits: cut then sew one edge of a face, move one face's uvs.
    run("selectComponents", {{"args", {cube}}, {"edges", {0}}});
    CHECK(run("uvCutEdges")["cut"] == 1);
    CHECK(run("uvSewEdges")["sewn"].get<int>() <= 1);
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    CHECK(run("uvTransform", {{"translate", {0.1, 0.0}}, {"rotate", 15.0}})["corners"].get<int>() >= 4);
    run("uvUnwrap");
    run("uvAutoSeams");
}

TEST_CASE("polyUVSet deletes, copies and reorders sets without leaving gaps")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    run("select", {{"args", {cube}}});
    run("uvLightmap", {{"resolution", 64}});
    using Sets = std::vector<std::string>;
    CHECK(run("polyUVSet", {{"query", true}})["sets"].get<Sets>() == Sets{"uv0", "uv1"});

    CHECK(run("polyUVSet", {{"copy", true}, {"uvSet", "uv1"}})["sets"].get<Sets>() == Sets{"uv0", "uv1", "uv2"});
    CHECK(run("uvLayout", {{"uvSet", "uv2"}})["overlappingFaces"] == 0);

    run("uvSetCurrent", {{"args", {"uv2"}}});
    CHECK(run("polyUVSet", {{"delete", true}, {"uvSet", "uv0"}})["sets"].get<Sets>() == Sets{"uv0", "uv1"});
    CHECK(run("uvSetCurrent") == "uv0");
    run("undo");
    CHECK(run("polyUVSet", {{"query", true}})["sets"].get<Sets>() == Sets{"uv0", "uv1", "uv2"});

    CHECK(run("polyUVSet", {{"moveTo", 0}, {"uvSet", "uv2"}})["sets"].get<Sets>() == Sets{"uv0", "uv1", "uv2"});
    CHECK(run("uvLayout", {{"uvSet", "uv0"}})["overlappingFaces"] == 0);
    CHECK_THROWS(run("polyUVSet", {{"delete", true}, {"uvSet", "uv7"}}));

    std::string plain = run("polyCube")[0];
    run("select", {{"args", {cube, plain}}});
    run("polyUVSet", {{"delete", true}, {"uvSet", "uv2"}});
    CHECK(run("polyUVSet", {{"args", {cube}}, {"query", true}})["sets"].get<Sets>() == Sets{"uv0", "uv1"});

    json made = run("polyUVSet", {{"copy", true}, {"uvSet", "uv0"}})["created"];
    REQUIRE(made.size() == 2);
    Sets numbers;
    for (const auto& [mesh, set] : made.items())
        numbers.push_back(set.get<std::string>());
    std::ranges::sort(numbers);
    CHECK(numbers == Sets{"uv1", "uv2"});
    CHECK(run("polyUVSet", {{"args", {plain}}, {"query", true}})["sets"].get<Sets>() == Sets{"uv0", "uv1"});
}

TEST_CASE("uvPack packs every selected mesh into one layout and stacks")
{
    run("newScene");
    std::string a = run("polyCube")[0], b = run("polyCube")[0];
    run("select", {{"args", {a, b}}});
    run("uvProject", {{"type", "box"}});
    json r = run("uvPack", {{"resolution", 512}, {"padding", 4}, {"effort", 2}});
    CHECK(r["ok"] == true);
    CHECK(r["density"].get<double>() > 30.0);
    CHECK(r["stacked"] == 0);
    CHECK(run("uvStacks")[a].empty());

    json s = run("uvPack", {{"stack", true}, {"rotateStep", 0}, {"angles", {0, 90}}, {"layout", "fill"}, {"keep", "none"}});
    CHECK(s["stacked"] == 11);
    json stacks = run("uvStacks", {{"select", true}});
    CHECK(stacks[a].size() == 1);
    CHECK(stacks[a][0].size() == 6);

    run("select", {{"args", {a, b}}});
    json g = run("uvPack", {{"layout", "groupTiles"}, {"groupBy", "object"}, {"scaleMode", "keep"}, {"region", {0.0, 0.0, 0.5, 1.0}}});
    CHECK(g["tiles"] == 2);
    run("undo");
    CHECK_THROWS(run("uvPack", {{"scaleMode", "lock"}, {"tilesU", 1}, {"padding", 600}}));
}

TEST_CASE("uvPack on a material's faces leaves the rest of a partly assigned mesh alone")
{
    run("newScene");
    std::string a = run("polyCube")[0], b = run("polyCube")[0];
    run("select", {{"args", {a, b}}});
    run("uvProject", {{"type", "box"}});
    run("uvPack", {{"resolution", 512}, {"padding", 4}});
    run("createMaterial", {{"args", {"M_Blue"}}});
    run("assignMaterial", {{"args", {"M_Blue", a}}});
    run("assignMaterial", {{"args", {"M_Blue", b}}, {"faces", {1, 3}}});
    auto uvsOf = [&](const std::string& object, std::vector<geo::Index> faces) {
        scene::Graph& g = scene::Graph::current();
        g.evaluate();
        const geo::Mesh& m = *g.find(object)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        std::span<const glm::vec2> uvs = m.cornerAttrs().get<glm::vec2>("uv0");
        std::vector<glm::vec2> out;
        for (geo::Index c : uv::uvCornersOfFaces(m, "uv0", faces))
            out.push_back(uvs[c]);
        return out;
    };
    const std::vector<glm::vec2> rest = uvsOf(b, {0, 2, 4, 5}), mine = uvsOf(b, {1, 3});
    run("selectByMaterial", {{"args", {"M_Blue"}}});
    const double alone = run("uvPack", {{"resolution", 512}, {"padding", 4}, {"keep", "none"}})["texelsPerUnit"].get<double>();
    CHECK(uvsOf(b, {0, 2, 4, 5}) == rest);
    CHECK(uvsOf(b, {1, 3}) != mine);

    // Keeping the unselected islands in place: those of another material take no room in this layout.
    run("undo");
    const double kept = run("uvPack", {{"resolution", 512}, {"padding", 4}, {"keep", "unselected"}})["texelsPerUnit"].get<double>();
    CHECK(kept == doctest::Approx(alone).epsilon(0.02));
    CHECK(uvsOf(b, {0, 2, 4, 5}) == rest);
}

TEST_CASE("uvPack perObject gives each mesh a layout of its own")
{
    run("newScene");
    std::string a = run("polyCube")[0], b = run("polyCube")[0];
    run("select", {{"args", {a, b}}});
    run("uvProject", {{"type", "box"}});
    double shared = run("uvPack", {{"resolution", 512}, {"padding", 4}})["texelsPerUnit"].get<double>();
    json r = run("uvPack", {{"resolution", 512}, {"padding", 4}, {"perObject", true}});
    REQUIRE(r["meshes"].size() == 2);
    CHECK(r["meshes"][0]["mesh"] == a);
    for (const json& m : r["meshes"])
        CHECK(m["texelsPerUnit"].get<double>() > shared * 1.2);
    run("undo");
}

namespace
{
std::string made(const json& r) { return (r.is_array() ? r[0] : r).get<std::string>(); }
scene::NodeId idOf(const std::string& name) { return scene::Graph::current().find(name)->id(); }
std::vector<scene::NodeId> sorted(std::vector<scene::NodeId> v)
{
    std::sort(v.begin(), v.end());
    return v;
}
} // namespace

TEST_CASE("uv islands of seed faces")
{
    run("newScene");
    std::string cube = made(run("polyCube"));
    run("select", {{"args", {cube}}});
    scene::Graph& g = scene::Graph::current();
    auto mesh = [&] {
        g.evaluate();
        return g.find(cube)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    };
    uv::ChartOptions co;
    co.source = uv::IslandSource::Uvs;

    run("uvProject", {{"type", "box"}});
    const geo::Index seed[] = {0};
    CHECK(uv::islandsOfFaces(*mesh(), seed, co) == std::vector<geo::Index>{0});
    const geo::Index two[] = {0, 3};
    CHECK(uv::islandsOfFaces(*mesh(), two, co) == std::vector<geo::Index>{0, 3});
    CHECK(uv::islandsOfFaces(*mesh(), {}, co).empty());

    // One shared uv per point joins every face into a single island.
    run("uvProject", {{"type", "planar"}});
    CHECK(uv::islandsOfFaces(*mesh(), seed, co).size() == 6);
}

TEST_CASE("uv editor meshes follow the selection and its hierarchy")
{
    run("newScene");
    std::string a = made(run("polyCube")), b = made(run("polyCube")), c = made(run("polyCube"));
    std::string grp = made(run("group", {{"args", {a, b}}}));
    scene::Graph& g = scene::Graph::current();

    run("select", {{"args", {grp}}});
    CHECK(sorted(uv::shownMeshes()) == sorted({idOf(a), idOf(b)}));
    const scene::NodeId roots[] = {idOf(grp), idOf(a)};
    CHECK(uv::meshesUnder(g, roots).size() == 2);

    // Whole-mesh uv tools reach the meshes under a selected group.
    json r = run("uvProject", {{"type", "box"}});
    CHECK(r.contains(a));
    CHECK(r.contains(b));

    // A component selection left on a mesh does not keep it shown once something else is selected.
    run("select", {{"args", {a}}});
    run("selectMode", {{"args", {"face"}}});
    run("selectComponents", {{"args", {a}}, {"faces", {0, 1}}});
    run("selectMode", {{"args", {"object"}}});
    run("select", {{"args", {c}}});
    CHECK(uv::shownMeshes() == std::vector<scene::NodeId>{idOf(c)});
    run("selectMode", {{"args", {"vertex"}}});
    CHECK(uv::shownMeshes() == std::vector<scene::NodeId>{idOf(c)});

    // With no objects selected, meshes with components of the current kind show.
    run("select", {{"clear", true}});
    run("selectComponents", {{"args", {b}}, {"vertices", {0}}});
    CHECK(uv::shownMeshes() == std::vector<scene::NodeId>{idOf(b)});
    run("selectMode", {{"args", {"object"}}});
    CHECK(uv::shownMeshes().empty());
}

TEST_CASE("uv editor meshes leave out children when includeChildren is off")
{
    run("newScene");
    std::string a = made(run("polyCube")), b = made(run("polyCube"));
    run("parent", {{"args", {b, a}}});
    run("select", {{"args", {a}}});
    CHECK(uv::shownMeshes().size() == 2);
    uv::setIncludeChildren(false);
    CHECK(uv::shownMeshes() == std::vector<scene::NodeId>{idOf(a)});
    // One mesh projected: the result is its frame rather than results keyed by mesh.
    json r = run("uvProject", {{"type", "box"}});
    CHECK(r.contains("type"));
    CHECK(!r.contains(b));
    uv::setIncludeChildren(true);
}

TEST_CASE("uvTransform perIsland turns each island about its own centre")
{
    run("newScene");
    std::string cube = made(run("polyCube"));
    run("select", {{"args", {cube}}});
    run("uvProject", {{"type", "box"}});
    run("uvPack", {{"resolution", 512}, {"padding", 4}});
    auto centres = [&] {
        scene::Graph& g = scene::Graph::current();
        g.evaluate();
        const geo::Mesh& m = *g.find(cube)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        uv::ChartOptions co;
        co.source = uv::IslandSource::Uvs;
        std::span<const glm::vec2> uvs = m.cornerAttrs().get<glm::vec2>("uv0");
        std::vector<glm::dvec2> out;
        for (const auto& island : uv::islandFaces(m, {}, co))
        {
            glm::dvec2 lo(1e30), hi(-1e30);
            for (geo::Index c : uv::uvCornersOfFaces(m, "uv0", island))
            {
                lo = glm::min(lo, glm::dvec2(uvs[c]));
                hi = glm::max(hi, glm::dvec2(uvs[c]));
            }
            out.push_back((lo + hi) * 0.5);
        }
        return out;
    };
    std::vector<glm::dvec2> before = centres();
    REQUIRE(before.size() == 6);
    CHECK(run("uvTransform", {{"rotate", 90.0}, {"perIsland", true}})["corners"].get<int>() > 0);
    run("uvTransform", {{"scale", {-1.0, 1.0}}, {"perIsland", true}});
    std::vector<glm::dvec2> after = centres();
    REQUIRE(after.size() == 6);
    for (size_t i = 0; i < 6; ++i)
        CHECK(glm::distance(before[i], after[i]) < 1e-4);
}

TEST_CASE("uvProject adjustments undo and redo one at a time")
{
    namespace lo = scene::lastOp;
    lo::install();
    run("newScene");
    std::string cube = run("polyCube")[0];
    run("select", {{"args", {cube}}});
    auto scale = [] { return uv::lastProjections().front().scale.x; };
    auto uvs = [] { return run("uvTexelDensity"); };

    ops::execute("uvProject", {{"type", "planar"}, {"scaleU", 1.0}, {"normalize", false}}, CallSource::UI);
    REQUIRE(lo::live());
    json u1 = uvs();
    lo::rerun({{"scaleU", 2.0}});
    json u2 = uvs();
    CHECK(u1 != u2);
    lo::rerun({{"scaleU", 3.0}});
    CHECK(scale() == doctest::Approx(3.0));

    run("undo");
    CHECK(scale() == doctest::Approx(2.0));
    CHECK(uvs() == u2);
    CHECK(lo::current()->call["scaleU"].get<double>() == doctest::Approx(2.0));
    CHECK(lo::live());
    run("undo");
    CHECK(scale() == doctest::Approx(1.0));
    CHECK(uvs() == u1);
    CHECK(lo::live());
    run("redo");
    CHECK(scale() == doctest::Approx(2.0));
    CHECK(uvs() == u2);
    run("undo");
    run("undo");
    CHECK_FALSE(lo::live());
    run("redo");
    CHECK(lo::live());
    CHECK(scale() == doctest::Approx(1.0));
}

TEST_CASE("uv edits keep the modifier stack and go onto its base")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    run("addModifier", {{"args", {cube}}, {"kind", "autoBevel"}});
    scene::Graph& g = scene::Graph::current();
    scene::NodeId id = g.find(cube)->id();
    REQUIRE(scene::modifiers::stack(g, id).size() == 1);
    run("select", {{"args", {cube}}});

    run("uvProject", {{"type", "box"}});
    run("uvPack", {{"resolution", 512}, {"padding", 8}});
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    run("uvTransform", {{"translate", {0.1, 0.0}}});
    CHECK(scene::modifiers::stack(g, id).size() == 1);

    scene::MeshPtr base = scene::modifiers::cage(g, id);
    scene::MeshPtr out = g.node(id)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    CHECK(base->numFaces() == 6);
    CHECK(out->numFaces() > 6);
    CHECK(base->cornerAttrs().get<glm::vec2>("uv0").size() == base->cornerCapacity());
    CHECK(out->cornerAttrs().get<glm::vec2>("uv0").size() == out->cornerCapacity());
    run("undo");
    run("undo");
    run("undo");
    CHECK(scene::modifiers::stack(g, id).size() == 1);
}

namespace
{
scene::MeshPtr cageOf(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    return scene::modifiers::cage(g, g.find(name)->id());
}

/** Mean x of the seam edges running along the cylinder's height. */
double seamX(const std::string& name)
{
    scene::MeshPtr m = cageOf(name);
    std::span<const bool> seam = std::as_const(*m).edgeAttrs().get<bool>(geo::attr::kSeam);
    double sum = 0.0;
    int n = 0;
    for (geo::Index e = 0; e < m->edgeCapacity(); ++e)
        if (m->edgeValid(e) && !seam.empty() && seam[e])
        {
            glm::dvec3 a = m->position(m->edgePoint(e, 0)), b = m->position(m->edgePoint(e, 1));
            if (std::abs(a.y - b.y) > 1.0)
            {
                sum += 0.5 * (a.x + b.x);
                ++n;
            }
        }
    REQUIRE(n > 0);
    return sum / n;
}
} // namespace

TEST_CASE("uvUnwrap stops at closed islands unless asked to open them")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    std::vector<uint32_t> edges;
    for (geo::Index e = 0; e < cageOf(cube)->edgeCapacity(); ++e)
        edges.push_back(e);
    run("selectMode", {{"args", {"edge"}}});
    run("selectComponents", {{"args", {cube}}, {"edges", edges}});
    run("uvSewEdges");
    run("selectMode", {{"args", {"object"}}});
    run("select", {{"args", {cube}}});
    CHECK_THROWS_AS(run("uvUnwrap"), CommandError);
    json r = run("uvUnwrap", {{"openIslands", true}});
    CHECK(r["seamsAdded"].get<int>() > 0);
    CHECK(run("uvLayout")["foldedFaces"] == 0);
}

TEST_CASE("uvUnwrap gives islands without usable uvs the density of the rest")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 10}, {"height", 10}, {"depth", 10}})[0];
    run("select", {{"args", {cube}}});
    run("uvProject", {{"type", "box"}});
    run("uvPack", {{"resolution", 1024}, {"padding", 4}});
    run("selectMode", {{"args", {"face"}}});
    run("selectComponents", {{"args", {cube}}, {"faces", {1, 2}}});
    double rest = run("uvTexelDensity").get<double>();
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    run("uvDelete");
    CHECK(run("uvTexelDensity").get<double>() == doctest::Approx(0.0));
    run("uvUnwrap");
    CHECK(run("uvTexelDensity").get<double>() == doctest::Approx(rest).epsilon(0.02));

    run("selectMode", {{"args", {"object"}}});
    run("select", {{"args", {cube}}});
    CHECK(run("uvDelete")["removed"] == true);
    CHECK(run("uvLayout")["exists"] == false);
}

TEST_CASE("seam views keep auto seams out of their sight")
{
    run("newScene");
    std::string tube = run("polyCylinder", {{"radius", 5}, {"height", 40}, {"subdivisionsAxis", 24}, {"subdivisionsHeight", 4}, {"caps", false}})[0];
    run("select", {{"args", {tube}}});
    std::string view = run("uvSeamView", {{"position", {200.0, 0.0, 0.0}}}).get<std::string>();
    run("select", {{"args", {tube}}});
    run("uvAutoSeams");
    CHECK(seamX(tube) < -4.0);
    run("move", {{"args", {view}}, {"to", {-200.0, 0.0, 0.0}}});
    run("select", {{"args", {tube}}});
    run("uvAutoSeams");
    CHECK(seamX(tube) > 4.0);
    run("uvAutoSeams", {{"views", {view}}, {"useViews", false}});
}

TEST_CASE("uvHose straightens a tube and hides its rail from a camera or a plane")
{
    run("newScene");
    std::string tube = run("polyCylinder", {{"radius", 5}, {"height", 40}, {"subdivisionsAxis", 16}, {"subdivisionsHeight", 8}})[0];
    run("select", {{"args", {tube}}});
    json r = run("uvHose", {{"visibility", false}});
    CHECK(r["strips"] == 1);
    CHECK(r["caps"] == 2);
    CHECK(r["otherFaces"] == 0);
    CHECK(r["pack"]["ok"] == true);

    // Rail edges run along Y; their mean x says which side the rail is on.
    auto railX = [&] {
        scene::MeshPtr m = cageOf(tube);
        std::span<const bool> seam = m->edgeAttrs().get<bool>(geo::attr::kSeam);
        double x = 0.0;
        int n = 0;
        for (geo::Index e = 0; e < m->edgeCapacity(); ++e)
            if (m->edgeValid(e) && seam[e])
            {
                glm::dvec3 a = m->position(m->edgePoint(e, 0)), b = m->position(m->edgePoint(e, 1));
                if (std::abs(glm::normalize(b - a).y) > 0.9)
                    x += a.x, ++n;
            }
        return n ? x / n : 0.0;
    };
    std::string view = run("uvSeamView", {{"position", {200.0, 0.0, 0.0}}}).get<std::string>();
    run("select", {{"args", {tube}}});
    run("uvHose", {{"visibility", false}, {"views", {view}}});
    CHECK(railX() < -4.0);

    std::string plane = run("polyPlane", {{"width", 10}, {"depth", 10}})[0];
    run("move", {{"args", {plane}}, {"to", {-200.0, 0.0, 0.0}}});
    run("setAttr", {{"args", {plane + ".rotateZ", 90.0}}});
    run("select", {{"args", {tube}}});
    run("uvHose", {{"visibility", false}, {"views", {plane}}});
    CHECK(railX() > 4.0);
}

TEST_CASE("planar projection with symmetry fits one side, not both")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 10}, {"height", 10}, {"depth", 10}})[0];
    run("setSymmetry", {{"args", {cube}}, {"axis", "x"}});
    scene::MeshPtr m = cageOf(cube);
    std::vector<uint32_t> sides;
    for (geo::Index f = 0; f < m->faceCapacity(); ++f)
        if (m->faceValid(f) && std::abs(m->faceNormal(f).x) > 0.9)
            sides.push_back(f);
    REQUIRE(sides.size() == 2);
    run("selectMode", {{"args", {"face"}}});
    run("selectComponents", {{"args", {cube}}, {"faces", sides}});
    run("uvProject", {{"type", "planar"}});
    json info = run("uvLayout");
    CHECK(info["foldedFaces"] == 0);
    CHECK(info["areaDistortion"]["max"].get<double>() < 0.1);
}

TEST_CASE("projection under an auto bevel splits the bevelled result too")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100}, {"height", 100}, {"depth", 100}})[0];
    run("autoBevel", {{"args", {cube}}, {"segments", 2}, {"offset", 5.0}});
    run("select", {{"args", {cube}}});
    for (const char* type : {"box", "planar", "cylindrical"})
    {
        CAPTURE(type);
        run("uvProject", {{"type", type}});
        scene::Graph& g = scene::Graph::current();
        g.evaluate();
        scene::MeshPtr cage = cageOf(cube);
        scene::MeshPtr out = g.find(cube)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        REQUIRE(out->numFaces() > cage->numFaces());
        uv::ChartOptions co;
        co.source = uv::IslandSource::Seams;
        CHECK(uv::islandFaces(*out, {}, co).size() == uv::islandFaces(*cage, {}, co).size());
    }
}

TEST_CASE("cut seams survive a planar projection under an auto bevel")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100}, {"height", 100}, {"depth", 100}})[0];
    run("autoBevel", {{"args", {cube}}, {"segments", 2}, {"offset", 5.0}});
    run("select", {{"args", {cube}}});
    run("uvProject", {{"type", "box"}});
    scene::Graph& g = scene::Graph::current();
    uv::ChartOptions co;
    co.source = uv::IslandSource::Uvs;
    scene::MeshPtr cage = cageOf(cube);
    std::vector<uint32_t> all;
    for (geo::Index e = 0; e < cage->edgeCapacity(); ++e)
        if (cage->edgeValid(e))
            all.push_back(e);
    run("selectMode", {{"args", {"edge"}}});
    run("selectComponents", {{"args", {cube}}, {"edges", all}});
    run("uvSewEdges");
    // Cut the top face off, then project everything planar from the top.
    cage = cageOf(cube);
    std::vector<uint32_t> ring;
    for (geo::Index e = 0; e < cage->edgeCapacity(); ++e)
        if (cage->edgeValid(e) && cage->position(cage->edgePoint(e, 0)).y > 49 && cage->position(cage->edgePoint(e, 1)).y > 49)
            ring.push_back(e);
    REQUIRE(ring.size() == 4);
    run("selectComponents", {{"args", {cube}}, {"edges", ring}});
    run("uvCutEdges");
    run("selectMode", {{"args", {"object"}}});
    run("select", {{"args", {cube}}});
    run("uvProject", {{"type", "planar"}, {"axis", "y"}, {"keepSeams", true}});
    g.evaluate();
    cage = cageOf(cube);
    scene::MeshPtr out = g.find(cube)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    std::span<const bool> seam = std::as_const(*cage).edgeAttrs().get<bool>(geo::attr::kSeam);
    size_t seams = 0;
    for (uint32_t e : ring)
        seams += seam[e] ? 1 : 0;
    CHECK(seams == 4);
    co.source = uv::IslandSource::Seams;
    CHECK(uv::islandFaces(*out, {}, co).size() == 2);
    // Moving the top's uvs leaves the sides where they are, though they touch along the cut.
    std::vector<uint32_t> top;
    for (geo::Index f = 0; f < cage->faceCapacity(); ++f)
        if (cage->faceValid(f) && cage->faceNormal(f).y > 0.9)
            top.push_back(f);
    REQUIRE(top.size() == 1);
    CHECK(uv::uvCornersOfFaces(*cage, "uv0", top).size() == 4);
}

TEST_CASE("uv vertex selection moves just the picked uvs of a shared point")
{
    run("newScene");
    std::string cube = made(run("polyCube"));
    run("select", {{"args", {cube}}});
    run("uvProject", {{"type", "box"}});
    const geo::Mesh& m0 = *meshOf(cube);
    const std::vector<geo::Index> rows = geo::faceVertexCorners(m0);
    const geo::Index c0 = rows[0], p0 = m0.cornerPoint(c0);
    std::vector<geo::Index> shared;
    m0.forEachPointCorner(p0, [&](geo::Index c) { shared.push_back(c); });
    REQUIRE(shared.size() == 3);
    const std::vector<glm::vec2> before = uvsOf(cube);

    run("selectComponents", {{"args", {cube}}, {"uvs", {0}}});
    json sel = run("componentSelection")[cube];
    CHECK(sel["uvs"] == json::array({0}));
    CHECK(sel["vertices"] == json::array({p0}));
    CHECK(run("uvTransform", {{"translate", {0.25, 0.0}}})["corners"] == 1);
    std::vector<glm::vec2> after = uvsOf(cube);
    for (geo::Index c : shared)
        CHECK(after[c].x == doctest::Approx(before[c].x + (c == c0 ? 0.25f : 0.0f)));

    // A vertex pick from anywhere else drops the uvs and moves every corner of the point again.
    run("selectComponents", {{"args", {cube}}, {"vertices", {p0}}});
    CHECK_FALSE(run("componentSelection")[cube].contains("uvs"));
    CHECK(run("uvTransform", {{"translate", {0.25, 0.0}}})["corners"] == 3);

    // Undo brings the uv pick back with the selection.
    run("undo");
    run("undo");
    CHECK(run("componentSelection")[cube]["uvs"] == json::array({0}));

    // Adding a uv to a point selection made elsewhere keeps that point's corners.
    run("selectComponents", {{"args", {cube}}, {"vertices", {p0}}});
    run("selectComponents", {{"args", {cube}}, {"uvs", {1}}, {"add", true}});
    CHECK(run("componentSelection")[cube]["uvs"].size() == 4);
    run("selectComponents", {{"args", {cube}}, {"uvs", {1}}, {"deselect", true}});
    CHECK(run("componentSelection")[cube]["uvs"].size() == 3);
}

TEST_CASE("selecting a uv widens to its whole uv vertex")
{
    run("newScene");
    std::string cube = made(run("polyCube"));
    run("select", {{"args", {cube}}});
    run("uvProject", {{"type", "planar"}});
    geo::Mesh m = *meshOf(cube);
    const std::vector<geo::Index> rows = geo::faceVertexCorners(m);
    const geo::Index p0 = m.cornerPoint(rows[0]);
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
    const std::vector<geo::Index> group = geo::uvVertexCorners(m, "uv0", std::vector<geo::Index>{rows[0]});
    for (geo::Index c : group)
        CHECK(m.cornerPoint(c) == p0);
    for (geo::Index c : group)
        CHECK(uv[c] == uv[rows[0]]);
    run("selectComponents", {{"args", {cube}}, {"uvs", {0}}});
    CHECK(run("componentSelection")[cube]["uvs"].size() == group.size());
    const std::vector<geo::Index> back = geo::faceVertexRows(m);
    for (geo::Index r = 0; r < rows.size(); ++r)
        CHECK(back[rows[r]] == r);
}

TEST_CASE("uv tools solve and measure on the mesh after its modifiers")
{
    run("newScene");
    std::string plate = run("polyCube", {{"width", 10.0}, {"height", 1.0}, {"depth", 10.0}})[0];
    run("select", {{"args", {plate}}});
    run("uvProject", {{"type", "box"}});
    run("uvPack");
    const json cage = run("uvLayout", {{"finalMesh", false}});
    CHECK(cage["measuredOn"] == "cage");

    run("autoBevel", {{"args", {plate}}, {"segments", 3}, {"offset", 0.03}});
    const json added = run("addModifier", {{"args", {plate}}, {"kind", "subdivision"}});
    const std::string subdiv = added.is_array() ? added[0].get<std::string>() : added.get<std::string>();
    run("setAttr", {{"args", {subdiv + ".levels", 2}}});
    run("select", {{"args", {plate}}});
    const json before = run("uvLayout");
    CHECK(before["measuredOn"] == "exported");
    CHECK(before["texelDensity"].get<double>() > 0.0);

    // Script control: the base mesh alone, as without modifiers.
    json r = run("uvOptimize", {{"finalMesh", false}});
    CHECK_FALSE(r.contains("finalMesh"));
    run("undo");

    r = run("uvOptimize");
    REQUIRE(r.contains("finalMesh"));
    CHECK(r["finalMesh"]["method"] == "probed");
    const json collapsed = r["finalMesh"]["collapsed"];
    MESSAGE("collapsed " << collapsed[0] << " -> " << collapsed[1]);
    CHECK(collapsed[1].get<int>() <= collapsed[0].get<int>());

    r = run("uvLiveRelax", {{"iterations", 20}});
    CHECK_FALSE(r.contains("finalMesh"));
    r = run("uvPack");
    CHECK(r["ok"] == true);
    CHECK(run("uvLayout")["overlappingFaces"] == 0);
    const double target = 10.0;
    run("uvTexelDensity", {{"density", target}, {"resolution", 1024}, {"worldSpace", false}});
    CHECK(run("uvTexelDensity", {{"resolution", 1024}, {"worldSpace", false}}).get<double>() == doctest::Approx(target).epsilon(0.02));
}

TEST_CASE("uvUnwrapExported applies the modifiers and leaves no collapsed uvs")
{
    run("newScene");
    std::string plate = run("polyCube", {{"width", 10.0}, {"height", 1.0}, {"depth", 10.0}})[0];
    run("select", {{"args", {plate}}});
    run("uvProject", {{"type", "box"}});
    run("uvPack");
    run("autoBevel", {{"args", {plate}}, {"segments", 3}, {"offset", 0.03}});
    const json added = run("addModifier", {{"args", {plate}}, {"kind", "subdivision"}});
    const std::string subdiv = added.is_array() ? added[0].get<std::string>() : added.get<std::string>();
    run("setAttr", {{"args", {subdiv + ".levels", 2}}});
    // Blend uvs: the rim strips collapse on the export.
    run("setAttr", {{"args", {subdiv + ".uvs", 0}}});
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const size_t exportedFaces = g.find(plate)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh)->numFaces();

    run("select", {{"args", {plate}}});
    const json r = run("uvUnwrapExported");
    CHECK(r["modifiersApplied"] == true);
    CHECK(scene::modifiers::stack(g, g.find(plate)->id()).empty());
    g.evaluate();
    const geo::Mesh& m = *g.find(plate)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    CHECK(m.numFaces() == exportedFaces);

    // Per island, no triangle under 2% of the uv area the island's density gives it.
    uv::ChartOptions co;
    co.source = uv::IslandSource::Uvs;
    size_t collapsed = 0;
    for (const uv::Chart& ch : uv::buildCharts(m, {}, co))
    {
        const double density = std::abs(ch.areaUv()) / std::max(ch.area3d(), 1e-300);
        for (const auto& t : ch.tris)
        {
            const glm::dvec3 a = ch.pos[t[0]], b = ch.pos[t[1]], c = ch.pos[t[2]];
            const double a3 = 0.5 * glm::length(glm::cross(b - a, c - a));
            const glm::dvec2 u = ch.uv[t[1]] - ch.uv[t[0]], v = ch.uv[t[2]] - ch.uv[t[0]];
            collapsed += a3 > 1e-9 && 0.5 * std::abs(u.x * v.y - u.y * v.x) < 0.02 * a3 * density;
        }
    }
    CHECK(collapsed == 0);
    run("undo");
    CHECK(scene::modifiers::stack(g, g.find(plate)->id()).size() == 2);
}

TEST_CASE("uvLiveRelax repairs the export after its uv symmetry pass")
{
    // A thin symmetric plate unwrapped symmetrically: the relax leaves the rim strips collapsed on the export, and the
    // symmetry pass that mirrors the relax rewrites islands across the plane, so the repair has to come after it.
    run("newScene");
    std::string plate = run("polyCube", {{"width", 10.0}, {"height", 0.4}, {"depth", 6.0}, {"subdivisionsX", 2}, {"subdivisionsZ", 2}})[0];
    run("select", {{"args", {plate}}});
    run("setSymmetry", {{"args", {plate}}, {"axis", "x"}});
    run("uvUnwrapSymmetric");
    run("autoBevel", {{"args", {plate}}, {"segments", 3}, {"offset", 0.2}});
    const json added = run("addModifier", {{"args", {plate}}, {"kind", "subdivision"}});
    const std::string subdiv = added.is_array() ? added[0].get<std::string>() : added.get<std::string>();
    run("setAttr", {{"args", {subdiv + ".levels", 2}}});
    run("setAttr", {{"args", {subdiv + ".uvs", 1}}});
    run("select", {{"args", {plate}}});
    run("uvLiveRelax", {{"iterations", 200}});

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const geo::Mesh& m = *g.find(plate)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    // Per island, no triangle under 2% of the uv area the island's density gives it.
    uv::ChartOptions co;
    co.source = uv::IslandSource::Uvs;
    size_t collapsed = 0;
    for (const uv::Chart& ch : uv::buildCharts(m, {}, co))
    {
        const double density = std::abs(ch.areaUv()) / std::max(ch.area3d(), 1e-300);
        for (const auto& t : ch.tris)
        {
            const glm::dvec3 a = ch.pos[t[0]], b = ch.pos[t[1]], c = ch.pos[t[2]];
            const double a3 = 0.5 * glm::length(glm::cross(b - a, c - a));
            const glm::dvec2 u = ch.uv[t[1]] - ch.uv[t[0]], v = ch.uv[t[2]] - ch.uv[t[0]];
            collapsed += a3 > 1e-9 && 0.5 * std::abs(u.x * v.y - u.y * v.x) < 0.02 * a3 * density;
        }
    }
    CHECK(collapsed == 0);
}

TEST_CASE("camera projection maps what a camera sees to 0-1, image up as V")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"width", 10}, {"depth", 10}})[0];
    run("setAttr", {{"args", {plane + ".translate", {3.0, 0.0, 0.0}}}});
    std::string cam = run("createNode", {{"args", {"camera"}}}).get<std::string>();
    // Looking straight down, so image up is world -Z.
    run("setAttr", {{"args", {cam + ".translate", {0.0, 50.0, 0.0}}}});
    run("setAttr", {{"args", {cam + ".rotate", {-90.0, 0.0, 0.0}}}});
    run("select", {{"args", {plane}}});
    CHECK_THROWS_AS(run("uvProject", {{"type", "camera"}, {"camera", plane}}), CommandError);
    run("uvProject", {{"type", "camera"}, {"camera", cam}});

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const geo::Mesh& m = *g.find(plane)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    std::span<const glm::vec2> uvs = m.cornerAttrs().get<glm::vec2>("uv0");
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            m.forEachFaceCorner(f, [&](geo::Index c) {
                glm::dvec3 p = m.position(m.cornerPoint(c));
                CHECK(uvs[c].x > 0.4f);
                CHECK(uvs[c].x < 0.7f);
                CHECK(uvs[c].y > 0.0f);
                CHECK(uvs[c].y < 1.0f);
                CHECK((uvs[c].x > 0.5f) == (p.x > 0.0));
                CHECK((uvs[c].y > 0.5f) == (p.z < 0.0));
            });
}
