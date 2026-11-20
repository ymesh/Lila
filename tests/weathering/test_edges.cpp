#include "TestUtil.h"

#include "EdgeGeometry.h"
#include "EdgeSheets.h"

#include "geo/Bvh.h"
#include "geo/Primitives.h"
#include "geo/Triangulate.h"
#include "scene/Materials.h"

#include <glm/gtc/constants.hpp>
#include "scene/Modifiers.h"
#include "tex/TextureSet.h"

#include <cmath>

using namespace weatheringtest;
namespace edges = weathering::edges;

namespace
{
void loadEdgeDeps()
{
    auto& em = extensions::ExtensionManager::get();
    if (!em.isLoaded("TrimSheets"))
        REQUIRE(em.load(LILA_TEST_TRIM_PACKAGE) == "TrimSheets");
    if (!em.isLoaded("Blockwork"))
        REQUIRE(em.load(LILA_TEST_BLOCKWORK_PACKAGE) == "Blockwork");
    // trimDecalSend asks for texture sets, which this suite's main doesn't register.
    if (!ops::registry().find("texSets"))
    {
        lila::tex::registerTypes();
        lila::tex::registerCommands();
    }
}

std::string cylinder(const char* name, double radius, double height, int sides)
{
    return run("polyCylinder", {{"name", name}, {"radius", radius}, {"height", height}, {"subdivisionsAxis", sides}, {"baseAtOrigin", true},
                                {"generator", false}})[0]
        .get<std::string>();
}

/** An L-shaped block: one concave edge along Y at x = z = 50. */
std::string lBlock()
{
    return run("blockworkBlock", {{"shape", "prism"},
                                  {"profile", json::array({json::array({0, 0}), json::array({100, 0}), json::array({100, 50}), json::array({50, 50}),
                                                           json::array({50, 100}), json::array({0, 100})})},
                                  {"size", json::array({0, 0, 60})},
                                  {"name", "ell"}})
        .get<std::string>();
}

/** Every point of the mesh within reach of one of the segments. */
bool allNear(const geo::Mesh& m, const std::vector<std::pair<glm::dvec3, glm::dvec3>>& segments, double reach)
{
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p))
            continue;
        double best = 1e300;
        for (const auto& [a, b] : segments)
        {
            const glm::dvec3 ab = b - a;
            const double t = std::clamp(glm::dot(m.position(p) - a, ab) / std::max(glm::dot(ab, ab), 1e-12), 0.0, 1.0);
            best = std::min(best, glm::length(m.position(p) - (a + ab * t)));
        }
        if (best > reach)
            return false;
    }
    return true;
}

bool closed(const geo::Mesh& m)
{
    int bad = 0, one = 0;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.edgeFaceCount(e) != 2)
        {
            ++bad;
            one += m.edgeFaceCount(e) == 1;
        }
    if (bad)
        MESSAGE("open edges " << bad << " boundary " << one << " of " << m.numEdges());
    return bad == 0 && m.numFaces() > 0;
}

/** Faces whose triangles (as the viewport draws them) don't cover the face: folded or degenerate n-gons. */
int badlyTriangulated(const geo::Mesh& m)
{
    const geo::Triangulation t = geo::triangulate(m);
    std::vector<double> area(m.faceCapacity(), 0.0);
    for (size_t k = 0; k < t.faces.size(); ++k)
    {
        const glm::dvec3 a = m.position(m.cornerPoint(t.corners[k * 3])), b = m.position(m.cornerPoint(t.corners[k * 3 + 1])),
                         c = m.position(m.cornerPoint(t.corners[k * 3 + 2]));
        area[t.faces[k]] += glm::dot(glm::cross(b - a, c - a) * 0.5, m.faceNormal(t.faces[k]));
    }
    int bad = 0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && std::abs(area[f] - m.faceArea(f)) > 1e-3 * std::max(1.0, m.faceArea(f)))
            ++bad;
    return bad;
}

double signedVolume(const geo::Mesh& m)
{
    double v = 0.0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const std::vector<geo::Index> p = m.facePoints(f);
        for (size_t k = 1; k + 1 < p.size(); ++k)
            v += glm::dot(m.position(p[0]), glm::cross(m.position(p[k]), m.position(p[k + 1]))) / 6.0;
    }
    return v;
}

double positionSum(const geo::Mesh& m)
{
    double s = 0.0;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            s += m.position(p).x * 1.0 + m.position(p).y * 3.0 + m.position(p).z * 7.0;
    return s;
}
} // namespace

TEST_CASE("weathering edges: only edges past the angle, convex and concave told apart")
{
    fresh();
    const geo::Mesh cyl = meshOf(cylinder("Drum", 50, 80, 24));
    // Side edges turn 15 degrees, the rims 90.
    const std::vector<edges::SharpEdge> rims = edges::sharpEdges(cyl, 35.0, edges::kConvex | edges::kConcave);
    CHECK(rims.size() == 48);
    for (const edges::SharpEdge& e : rims)
    {
        CHECK(e.kind == edges::kConvex);
        CHECK(e.angle > 35.0);
    }
    CHECK(edges::sharpEdges(cyl, 10.0, edges::kConvex).size() == 72);
    CHECK(edges::sharpEdges(cyl, 95.0, edges::kConvex).empty());
    CHECK(edges::sharpEdges(cyl, 35.0, edges::kConcave).empty());

    loadEdgeDeps();
    const std::string ell = lBlock();
    const json found = run("edgeFind", {{"args", {ell}}, {"angle", 35.0}});
    CHECK(found["concave"].size() == 1);
    CHECK(found["convex"].size() == 17);
}

TEST_CASE("weathering edges: tool faces, hidden faces and block contacts are skipped")
{
    geo::Mesh m = geo::makeCube({});
    const size_t all = edges::sharpEdges(m, 35.0, edges::kConvex).size();
    CHECK(all == 12);
    scene::materials::assign(m, "ToolNodraw", {0});
    CHECK(edges::sharpEdges(m, 35.0, edges::kConvex).size() == 8);
    geo::Mesh h = geo::makeCube({});
    std::span<bool> hidden = h.faceAttrs().getOrAdd<bool>(edges::kHiddenFaceAttr);
    hidden[1] = true;
    CHECK(edges::sharpEdges(h, 35.0, edges::kConvex).size() == 8);
    geo::Mesh c = geo::makeCube({});
    std::span<bool> contact = c.edgeAttrs().getOrAdd<bool>(edges::kContactEdgeAttr);
    contact[0] = contact[1] = true;
    CHECK(edges::sharpEdges(c, 35.0, edges::kConvex).size() == 10);
}

TEST_CASE("weathering edges: decal strips lie only along the sharp edges and follow a curved loop")
{
    fresh();
    loadEdgeDeps();
    const std::string drum = cylinder("Drum", 50, 80, 24);
    const json chains = run("edgeFind", {{"args", {drum}}})["chains"];
    REQUIRE(chains.size() == 2);
    for (const json& c : chains)
    {
        CHECK(c["closed"].get<bool>());
        CHECK(c["edges"].size() == 24);
    }

    const json r = run("edgeDecals", {{"args", {drum}}, {"width", 6.0}, {"coverage", 1.0}, {"jitter", 0.0}, {"offset", 0.0}});
    CHECK_FALSE(r["group"].get<std::string>().empty());
    CHECK(r["notes"].size() >= 1); // the placeholder sheet stood in
    const geo::Mesh strips = meshOf(r["meshes"][0].get<std::string>());
    REQUIRE(strips.numFaces() > 0);
    CHECK(r["stats"]["Drum"]["concavePieces"] == 0);
    // Every strip point within half the width of a rim: none spill onto the curved side's soft edges.
    for (geo::Index p = 0; p < strips.pointCapacity(); ++p)
        if (strips.pointValid(p))
        {
            const glm::dvec3 q = strips.position(p);
            const double rimDistance = std::min(std::hypot(std::hypot(q.x, q.z) - 50.0 * std::cos(glm::pi<double>() / 24), q.y),
                                                std::hypot(std::hypot(q.x, q.z) - 50.0 * std::cos(glm::pi<double>() / 24), q.y - 80.0));
            CHECK(rimDistance < 3.0 + 50.0 * (1.0 - std::cos(glm::pi<double>() / 24)) + 1e-3);
        }
    // A closed loop with full coverage goes all the way round: strip points at every angle.
    int sectors[8] = {};
    for (geo::Index p = 0; p < strips.pointCapacity(); ++p)
        if (strips.pointValid(p))
            sectors[int((std::atan2(strips.position(p).z, strips.position(p).x) + glm::pi<double>()) / glm::two_pi<double>() * 8.0) % 8] += 1;
    for (int s : sectors)
        CHECK(s > 0);
    // UVs inside the sheet.
    const std::span<const glm::vec2> uv = strips.cornerAttrs().get<glm::vec2>("uv0");
    REQUIRE(!uv.empty());
    for (const glm::vec2& t : uv)
        CHECK((t.x >= -1e-4f && t.x <= 1.0001f && t.y >= -1e-4f && t.y <= 1.0001f));
    CHECK(scene::materials::find(g(), r["material"].get<std::string>()));

    // A steeper angle than the rims leaves nothing.
    const json none = run("edgeDecals", {{"args", {drum}}, {"angle", 95.0}});
    CHECK(meshOf(none["meshes"][0].get<std::string>()).numFaces() == 0);
}

TEST_CASE("weathering edges: convex and concave selection")
{
    fresh();
    loadEdgeDeps();
    const std::string ell = lBlock();
    const json concave = run("edgeDecals", {{"args", {ell}}, {"edges", "concave"}, {"coverage", 1.0}, {"width", 6.0}, {"jitter", 0.0}});
    CHECK(concave["stats"]["ell"]["convexPieces"] == 0);
    CHECK(concave["stats"]["ell"]["concavePieces"].get<int>() > 0);
    const geo::Mesh host = meshOf(ell);
    const geo::Index e = run("edgeFind", {{"args", {ell}}})["concave"][0].get<geo::Index>();
    const glm::dvec3 a = host.position(host.edgePoint(e, 0)), b = host.position(host.edgePoint(e, 1));
    CHECK(glm::length(b - a) == doctest::Approx(60.0));
    const geo::Mesh cm = meshOf(concave["meshes"][0].get<std::string>());
    CHECK(allNear(cm, {{a, b}}, 3.2));

    const json convex = run("edgeDecals", {{"args", {ell}}, {"edges", "convex"}, {"coverage", 1.0}});
    CHECK(convex["stats"]["ell"]["concavePieces"] == 0);
    CHECK(convex["stats"]["ell"]["convexPieces"].get<int>() > 0);
}

TEST_CASE("weathering edges: chains break where they turn past the split angle")
{
    const geo::Mesh m = geo::makeCube({});
    const std::vector<edges::SharpEdge> sharp = edges::sharpEdges(m, 35.0, edges::kConvex);
    CHECK(edges::chainEdges(m, sharp, 40.0).size() == 12);
    // A square loop holds together once 90 degree turns are allowed.
    size_t loops = 0;
    for (const edges::Chain& c : edges::chainEdges(m, sharp, 95.0))
        loops += c.closed;
    CHECK(loops >= 1);
}

TEST_CASE("weathering edges: chipped mesh stays closed and within depth of the original")
{
    fresh();
    const std::string box = cube("Crate", {0, 0, 0}, {100, 100, 100});
    const geo::Mesh before = meshOf(box);
    const json r = run("edgeChip", {{"args", {box}}, {"size", 10.0}, {"depth", 3.0}, {"amount", 0.6}, {"seed", 4}});
    CHECK(r["group"] == "");
    REQUIRE(r["modifiers"].size() == 1);
    CHECK(r["chips"]["Crate"].get<int>() > 10);
    const geo::Mesh after = meshOf(box);
    CHECK(closed(after));
    CHECK(after.numFaces() > before.numFaces());
    CHECK(badlyTriangulated(after) == 0);
    CHECK(signedVolume(before) > 0.0);
    CHECK(signedVolume(after) > 0.9 * signedVolume(before));
    CHECK(signedVolume(after) < signedVolume(before));
    const geo::MeshBvh original(before);
    double worst = 0.0;
    for (geo::Index p = 0; p < after.pointCapacity(); ++p)
        if (after.pointValid(p))
        {
            geo::BvhClosest c;
            REQUIRE(original.closestPoint(after.position(p), c));
            worst = std::max(worst, c.distance);
        }
    CHECK(worst <= 3.0 + 0.05);
    CHECK(worst > 0.5); // something was cut
    // Cut faces carry world-projected uvs.
    CHECK(!after.cornerAttrs().get<glm::vec2>("uv0").empty());

    // Toon chunks are closed too, with fewer, bigger facets per chip.
    const json toon = run("edgeChip", {{"args", {box}}, {"style", "toon"}});
    CHECK(closed(meshOf(box)));
    CHECK(toon["modifiers"][0] == r["modifiers"][0]); // the same modifier, changed
}

TEST_CASE("weathering edges: chip cutters fit within depth and keep the shape control")
{
    for (double shape : {0.0, 0.5, 1.0})
    {
        const geo::Mesh c = edges::chipCutter(10.0, 2.0, shape, 3, 1);
        CHECK(closed(c));
        for (geo::Index p = 0; p < c.pointCapacity(); ++p)
            if (c.pointValid(p))
            {
                CHECK(std::hypot(c.position(p).y, c.position(p).z) <= 2.0 + 1e-9);
                CHECK(std::abs(c.position(p).x) <= 5.0 + 1e-9);
            }
    }
    CHECK(edges::chipCutter(10.0, 2.0, 1.0, 3, 1).numFaces() < edges::chipCutter(10.0, 2.0, 0.0, 3, 1).numFaces());
}

TEST_CASE("weathering edges: same seed same result")
{
    auto build = [](int64_t seed) {
        fresh();
        loadEdgeDeps();
        const std::string box = cube("Crate", {0, 0, 0}, {100, 60, 80});
        run("edgeChip", {{"args", {box}}, {"seed", seed}, {"amount", 0.5}});
        const json d = run("edgeDecals", {{"args", {box}}, {"seed", seed}});
        return std::pair{positionSum(meshOf(box)), positionSum(meshOf(d["meshes"][0].get<std::string>()))};
    };
    const auto a = build(7), b = build(7), c = build(8);
    CHECK(a.first == b.first);
    CHECK(a.second == b.second);
    CHECK((a.first != c.first || a.second != c.second));
}

TEST_CASE("weathering edges: decals and chips rebuild when a Blockwork block is extended")
{
    fresh();
    loadEdgeDeps();
    const std::string block = run("blockworkBlock", {{"origin", json::array({0, 0, 0})}, {"size", json::array({128, 128, 128})}, {"name", "wall"}})
                                  .get<std::string>();
    const json chip = run("edgeChip", {{"args", {block}}, {"size", 12.0}, {"depth", 3.0}, {"amount", 0.7}});
    const json dec = run("edgeDecals", {{"args", {block}}, {"coverage", 1.0}});
    const std::string decals = dec["meshes"][0].get<std::string>();
    const double topBefore = meshOf(decals).bounds().max.y;
    const double blockTop = meshOf(block).bounds().max.y;
    CHECK(topBefore == doctest::Approx(128.0).epsilon(0.05));

    // The top face is the one whose centroid is highest.
    const geo::Mesh base = meshOf(block);
    geo::Index top = 0;
    for (geo::Index f = 0; f < base.faceCapacity(); ++f)
        if (base.faceValid(f) && base.faceNormal(f).y > 0.99)
            top = f;
    const geo::Mesh cage = *scene::modifiers::cage(g(), g().find(block)->id());
    geo::Index cageTop = 0;
    for (geo::Index f = 0; f < cage.faceCapacity(); ++f)
        if (cage.faceValid(f) && cage.faceNormal(f).y > 0.99)
            cageTop = f;
    (void)top;
    run("blockworkExtend", {{"args", {block}}, {"faces", json::array({cageTop})}, {"distance", 64.0}});
    const geo::Mesh grown = meshOf(block);
    CHECK(grown.bounds().max.y > blockTop + 50.0);
    CHECK(closed(grown));
    CHECK(meshOf(decals).bounds().max.y > topBefore + 50.0);
    CHECK(g().find(chip["modifiers"][0].get<std::string>()));

    // Clipping rebuilds them too.
    run("blockworkClip", {{"args", {block}}, {"point", json::array({0, 100, 0})}, {"normal", json::array({0, -1, 0})}});
    CHECK(meshOf(block).bounds().max.y == doctest::Approx(100.0).epsilon(0.05));
    CHECK(meshOf(decals).bounds().max.y < 105.0);
}

TEST_CASE("weathering edges: a user mesh is swept along the edges")
{
    fresh();
    loadEdgeDeps();
    const std::string box = cube("Pillar", {0, 0, 0}, {60, 200, 60});
    // A trim running along +X, 20 cm long, 2 cm across.
    const std::string trim = cube("TrimPiece", {500, 0, 0}, {20, 2, 2});
    const json r = run("edgeDecals", {{"args", {box}}, {"mode", "mesh"}, {"pieces", json::array({trim})}, {"edges", "convex"}, {"coverage", 1.0}});
    REQUIRE(r["pieces"].size() == 1);
    const geo::Mesh swept = meshOf(r["meshes"][0].get<std::string>());
    const int placed = r["stats"]["Pillar"]["pieces"].get<int>();
    CHECK(placed > 12);
    CHECK(swept.numFaces() == geo::Index(placed * 6));
    // Every swept point stays near the pillar's edges: within the piece's own size of the edge lines.
    std::vector<std::pair<glm::dvec3, glm::dvec3>> lines;
    for (double x : {-30.0, 30.0})
        for (double z : {-30.0, 30.0})
            lines.push_back({{x, 0, z}, {x, 200, z}});
    for (double y : {0.0, 200.0})
        for (double s : {-30.0, 30.0})
        {
            lines.push_back({{-30, y, s}, {30, y, s}});
            lines.push_back({{s, y, -30}, {s, y, 30}});
        }
    CHECK(allNear(swept, lines, 2.5));

    // Without pieces= the placeholder edge trims for the style are swept.
    const json def = run("edgeDecals", {{"args", {box}}, {"mode", "mesh"}, {"style", "toon"}});
    CHECK(meshOf(def["meshes"][0].get<std::string>()).numFaces() > 0);
}

TEST_CASE("weathering edges: realize bakes strips and sheets are written once")
{
    fresh();
    loadEdgeDeps();
    const std::string box = cube("Crate", {0, 0, 0}, {100, 100, 100});
    const json r = run("edgeDecals", {{"args", {box}}, {"realize", true}, {"style", "toon"}});
    CHECK(r["nodes"].empty());
    const std::string mesh = r["meshes"][0].get<std::string>();
    const size_t faces = meshOf(mesh).numFaces();
    CHECK(faces > 0);
    run("setAttr", {{"args", {box + ".translateY", 50.0}}});
    CHECK(meshOf(mesh).numFaces() == faces);

    const json sheets = run("edgeDecalSheets");
    CHECK(sheets["built"] == false);
    const json info = run("trimSheetInfo", {{"sheet", sheets["toon"]}});
    CHECK(info["trims"].size() == 16);
    CHECK(info["textures"].contains("baseColor"));
    CHECK(info["textures"].contains("opacity"));
}

TEST_CASE("weathering edges: strips send to Unreal as mesh decals on the decal master")
{
    fresh();
    loadEdgeDeps();
    const std::string box = cube("Crate", {0, 0, 0}, {100, 100, 100});
    const json r = run("edgeDecals", {{"args", {box}}});
    const json plan = run("trimDecalSend", {{"dryRun", true}});
    const json meshes = plan["request"]["meshes"];
    REQUIRE(meshes.size() == 1);
    CHECK(meshes[0]["slot"] == r["material"]);
    CHECK(meshes[0]["mesh"].get<std::string>().find("/Meshes/SM_Crate_EdgeDecals") != std::string::npos);
    const json tex = meshes[0]["instance"]["textures"];
    CHECK(tex.contains("DecalBaseColorMap"));
    CHECK(tex.contains("DecalNormalMap"));
    CHECK(tex.contains("DecalOpacityMap"));
    CHECK(plan["request"]["decals"].empty());
    CHECK(plan["newlyMapped"].size() == 1);
}

TEST_CASE("weathering edges: decals follow the clean edges of a chipped mesh, in either order")
{
    fresh();
    loadEdgeDeps();
    const std::string first = cube("ChipFirst", {0, 0, 0}, {100, 100, 100});
    run("edgeChip", {{"args", {first}}, {"amount", 0.8}});
    const json a = run("edgeDecals", {{"args", {first}}});
    CHECK(a["stats"]["ChipFirst"]["edges"] == 12);

    const std::string later = cube("ChipLater", {300, 0, 0}, {100, 100, 100});
    const json b = run("edgeDecals", {{"args", {later}}});
    run("edgeChip", {{"args", {later}}, {"amount", 0.8}});
    g().evaluate();
    scene::Node* strip = g().find(b["nodes"][0].get<std::string>());
    REQUIRE(strip);
    const json st = json::parse(strip->as<std::string>(strip->type().attrIndex("stats")));
    CHECK(st["edges"] == 12);
    CHECK(closed(meshOf(later)));
}
