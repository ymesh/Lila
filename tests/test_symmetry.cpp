#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "geo/Mesh.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "geo/Symmetry.h"
#include "geo/Topology.h"
#include "io/SceneFormats.h"
#include "scene/Graph.h"
#include "scene/MeshData.h"
#include "scene/MeshNodes.h"

#include <cstring>
#include <filesystem>
#include <random>

using namespace lila;
using geo::Index;
using json = nlohmann::json;
using namespace testutil;

namespace
{
bool sameBits(const geo::Mesh& a, const geo::Mesh& b)
{
    if (a.pointCapacity() != b.pointCapacity())
        return false;
    return std::memcmp(a.positions().data(), b.positions().data(), a.positions().size_bytes()) == 0;
}

/** Points scattered with full 53-bit mantissas, so any trip through float or short decimals shows. */
void scramble(geo::Mesh& m, uint32_t seed)
{
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> d(-1.0, 1.0);
    for (glm::dvec3& p : m.positions())
        p += glm::dvec3(d(rng), d(rng), d(rng)) * 3.14159265358979;
}
} // namespace

TEST_CASE("persistent symmetry survives drift and compaction")
{
    geo::CubeOptions o;
    o.subdivX = 4;
    o.subdivY = 3;
    geo::Mesh m = geo::makeCube(o);
    const double tol = 1e-3;
    geo::SymmetrySync sync;
    geo::SymmetryMap first = geo::SymmetryMap::persistent(m, 0, tol, &sync);
    CHECK(sync.unmatched == 0);
    CHECK(sync.byPosition == m.numPoints());
    CHECK(m.pointAttrs().has(geo::symmetryLayerName(0)));

    // Drift the negative side a thousand times past the matching tolerance.
    std::vector<std::pair<glm::dvec3, glm::dvec3>> pairs;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (first.side(p) < 0)
            m.setPosition(p, m.position(p) + glm::dvec3(0.7, -1.3, 2.1));
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (first.side(p) > 0)
            pairs.push_back({m.position(p), m.position(first.mirrorPoint(p))});
    CHECK(geo::SymmetryMap::build(m, geo::axisPlane(0), tol).unmatchedCount() > 0);

    // Renumber the points: delete the first one and compact. Its partner is left without one.
    geo::deletePoints(m, std::vector<Index>{0});
    m.compact();
    geo::SymmetryMap again = geo::SymmetryMap::persistent(m, 0, tol, &sync);
    CHECK(sync.byPosition == 0);
    CHECK(sync.topological == 0);
    CHECK(sync.unmatched == 1);
    size_t found = 0;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (again.side(p) > 0)
            for (auto& [a, b] : pairs)
                if (a == m.position(p))
                {
                    CHECK(m.position(again.mirrorPoint(p)) == b);
                    ++found;
                }
    CHECK(found > 0);

    again.symmetrize(m, 1);
    geo::SymmetryCheck c = again.check(m);
    // Only the deleted point's partner is off: its reflection falls in the hole.
    CHECK(c.maxAsymmetry > 0.0);
    CHECK(again.mirrorPoint(c.worstPoint) == geo::kInvalid);
    CHECK(c.unmatched == 1);
}

TEST_CASE("symmetric ops pair the points they create, however far the old ones drifted")
{
    geo::Mesh m = geo::makeCube({});
    geo::SymmetryMap map = geo::SymmetryMap::persistent(m, 0, 1e-3);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (map.side(p) < 0)
            m.setPosition(p, m.position(p) + glm::dvec3(-0.5, 0.25, 0.0));
    map = geo::SymmetryMap::persistent(m, 0, 1e-3);

    std::vector<Index> side;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && m.faceNormal(f).x > 0.5)
            side.push_back(f);
    REQUIRE(side.size() == 1);
    const Index before = m.pointCapacity();
    geo::ExtrudeOptions eo;
    eo.offset = 10.0;
    eo.symmetry = &map;
    geo::extrudeFaces(m, side, eo);
    CHECK(m.pointCapacity() == before + 8);
    for (Index p = before; p < m.pointCapacity(); ++p)
        m.pointAttrs().get<int32_t>(geo::symmetryLayerName(0))[p] = 0;
    geo::SymmetrySync sync;
    geo::SymmetryMap after = geo::SymmetryMap::persistent(m, 0, 1e-3, &sync);
    CHECK(sync.topological == 8);
    CHECK(sync.unmatched == 0);
}

TEST_CASE("mesh storage round-trips doubles bit-exactly")
{
    geo::Mesh m = geo::makeCube({.subdivX = 6, .subdivY = 6, .subdivZ = 6});
    scramble(m, 7);
    m.positions()[0] = glm::dvec3(1e-300, -1.7976931348623157e308, 4.9406564584124654e-324);
    m.positions()[1] = glm::dvec3(0.1, 1.0 / 3.0, -2.0 / 7.0);
    json doc = {{"lila", 2}, {"nodes", json::array({{{"name", "M"}, {"type", "mesh"}, {"attrs", {{"inMesh", scene::meshToJson(m)}}}}})}};

    std::string bin = io::encodeBinary(doc);
    geo::Mesh fromBin = *scene::meshFromJson(io::decodeBinary(bin, nullptr)["nodes"][0]["attrs"]["inMesh"]);
    CHECK(sameBits(m, fromBin));

    std::string text = io::encodeAscii(doc);
    geo::Mesh fromText = *scene::meshFromJson(io::decodeAscii(text, nullptr)["nodes"][0]["attrs"]["inMesh"]);
    CHECK(sameBits(m, fromText));

    geo::Mesh fromJson = *scene::meshFromJson(json::parse(scene::meshToJson(m).dump()));
    CHECK(sameBits(m, fromJson));
}

TEST_CASE("scene save, load and undo keep positions bit-exact")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"subdivisionsX", 3}})[0];
    std::mt19937_64 rng(11);
    std::uniform_real_distribution<double> d(-1.0, 1.0);
    for (int v = 0; v < 16; ++v)
    {
        run("selectComponents", {{"args", {cube}}, {"vertices", {v}}});
        run("moveComponents", {{"by", {d(rng), d(rng), d(rng)}}});
    }
    geo::Mesh edited = meshOf(cube);
    run("selectComponents", {{"args", {cube}}, {"vertices", {3}}});
    run("moveComponents", {{"by", {1.0 / 3.0, 0, 0}}});
    run("undo");
    CHECK(sameBits(edited, meshOf(cube)));

    auto dir = std::filesystem::temp_directory_path() / "lila_tests";
    std::filesystem::create_directories(dir);
    for (const char* ext : {".lila", ".lilaa"})
    {
        std::string path = (dir / (std::string("symmetry_bits") + ext)).string();
        run("saveScene", {{"args", {path}}});
        run("newScene");
        run("openScene", {{"args", {path}}});
        CHECK(sameBits(edited, meshOf(cube)));
    }
}

TEST_CASE("1000 random symmetric edits stay exactly symmetric")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"subdivisionsX", 4}, {"subdivisionsY", 2}})[0];
    run("setSymmetry", {{"args", {cube}}, {"axis", "x"}});
    std::mt19937 rng(1234);
    auto pick = [&](int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); };
    std::uniform_real_distribution<double> d(-1.0, 1.0);
    for (int i = 0; i < 1000; ++i)
    {
        json info = run("meshInfo", {{"args", {cube}}});
        const int points = info["points"].get<int>(), faceCount = info["faces"].get<int>();
        const int op = pick(20);
        if (op == 0 && faceCount < 400)
        {
            run("selectComponents", {{"args", {cube}}, {"faces", {pick(faceCount)}}});
            run("polyExtrude", {{"thickness", 2.0 + d(rng)}});
        }
        else if (op == 1 && faceCount < 400)
        {
            run("selectComponents", {{"args", {cube}}, {"faces", {pick(faceCount)}}});
            run("polyInset", {{"thickness", 1.0}});
        }
        else
        {
            json verts = json::array();
            for (int k = 0, n = 1 + pick(4); k < n; ++k)
                verts.push_back(pick(points));
            run("selectComponents", {{"args", {cube}}, {"vertices", verts}});
            if (op % 3 == 0)
                run("moveComponents", {{"by", {d(rng) * 3.0, d(rng) * 3.0, d(rng) * 3.0}}});
            else if (op % 3 == 1)
                run("rotateComponents", {{"by", {d(rng) * 20.0, d(rng) * 20.0, d(rng) * 20.0}}});
            else
                run("scaleComponents", {{"by", {1.0 + d(rng) * 0.1, 1.0 + d(rng) * 0.1, 1.0 + d(rng) * 0.1}}});
        }
    }
    json c = run("symmetryCheck", {{"args", {cube}}})[cube];
    CHECK(c["maxAsymmetry"].get<double>() == 0.0);
    CHECK(c["unmatched"].get<int>() == 0);
    CHECK(c["withoutStoredPartner"].get<int>() == 0);
}

TEST_CASE("drift with symmetry off keeps the pairs; symmetrize repairs it")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"subdivisionsX", 2}})[0];
    run("setSymmetry", {{"args", {cube}}, {"axis", "x"}});
    run("selectComponents", {{"args", {cube}}, {"vertices", {0}}});
    run("moveComponents", {{"by", {0, 1, 0}}}); // stores the pairs
    run("setSymmetry", {{"args", {cube}}, {"axis", "off"}});
    for (int v = 0; v < 6; ++v)
    {
        run("selectComponents", {{"args", {cube}}, {"vertices", {v}}});
        run("moveComponents", {{"by", {0.5 + v, -0.25, 0.75}}});
    }
    // A topology edit with symmetry off renumbers points; the pairs ride along.
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    run("polyExtrude", {{"thickness", 3}});
    run("setSymmetry", {{"args", {cube}}, {"axis", "x"}});

    json c = run("symmetryCheck", {{"args", {cube}}})[cube];
    CHECK(c["maxAsymmetry"].get<double>() > 0.1);
    run("symmetrize", {{"args", {cube}}, {"side", "negative"}});
    c = run("symmetryCheck", {{"args", {cube}}})[cube];
    // The extrusion made with symmetry off has no mirror; symmetryRebuild makes one.
    CHECK(c["maxAsymmetry"].get<double>() == doctest::Approx(3.0));
    CHECK(c["unmatched"].get<int>() == 4);
    run("symmetryRebuild", {{"args", {cube}}});
    c = run("symmetryCheck", {{"args", {cube}}})[cube];
    CHECK(c["maxAsymmetry"].get<double>() == 0.0);
    CHECK(c["unmatched"].get<int>() == 0);
}

TEST_CASE("polyBevel with symmetry on keeps the pairs at corners")
{
    for (int segments = 1; segments <= 3; ++segments)
    {
        CAPTURE(segments);
        run("newScene");
        std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"subdivisionsX", 2}})[0];
        run("setSymmetry", {{"args", {cube}}, {"axis", "x"}});
        const int before = run("meshInfo", {{"args", {cube}}})["faces"].get<int>();
        // Every face's border in turn: corners of three bevelled edges, terminal edges and edges across the plane.
        for (int f : {0, 3})
        {
            run("selectComponents", {{"args", {cube}}, {"faces", {f}}});
            run("polyBevel", {{"offset", 5.0}, {"segments", segments}});
            json c = run("symmetryCheck", {{"args", {cube}}})[cube];
            CHECK(c["maxAsymmetry"].get<double>() < 1e-9);
            CHECK(c["unmatched"].get<int>() == 0);
            run("undo");
            CHECK(run("meshInfo", {{"args", {cube}}})["faces"].get<int>() == before);
        }
    }
}

TEST_CASE("toggleSymmetry turns a mixed selection on, then all off, undoably")
{
    run("newScene");
    std::string a = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0], b = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    std::string group = run("createNode", {{"args", {"transform"}}});
    scene::Graph& g = scene::Graph::current();
    auto axisOf = [&](const std::string& n) { return g.find(n)->as<int64_t>(scene::MeshNode::kSymmetry); };

    run("setSymmetry", {{"args", {a}}, {"axis", "y"}});
    run("select", {{"args", {a, b, group}}});
    scene::SymmetryState st = scene::symmetryState(g, g.selection());
    CHECK(st.meshes == 2);
    CHECK(st.on == 1);
    CHECK(st.axis == 1);

    // Partial: everything goes on, on the axis already in use.
    CHECK(run("toggleSymmetry") == "y");
    CHECK(axisOf(a) == 2);
    CHECK(axisOf(b) == 2);
    CHECK(run("toggleSymmetry") == "off");
    CHECK(axisOf(a) == 0);
    CHECK(axisOf(b) == 0);
    run("undo");
    CHECK(axisOf(b) == 2);

    // An explicit axis different from the current one switches instead of turning off.
    CHECK(run("toggleSymmetry", {{"axis", "z"}}) == "z");
    CHECK(scene::symmetryState(g, g.selection()).axis == 2);
    CHECK(run("toggleSymmetry", {{"axis", "z"}}) == "off");

    run("setSymmetry", {{"args", {a}}, {"axis", "x"}});
    run("setSymmetry", {{"args", {b}}, {"axis", "z"}});
    st = scene::symmetryState(g, g.selection());
    CHECK(st.on == 2);
    CHECK(st.axis == -1);

    run("select", {{"args", {group}}});
    CHECK_THROWS(run("toggleSymmetry"));
}

namespace
{
/** The first face whose centre satisfies pred. */
template <class Pred> int findFace(const geo::Mesh& m, Pred pred)
{
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        glm::dvec3 c(0.0);
        m.forEachFaceCorner(f, [&](Index k) { c += m.position(m.cornerPoint(k)) / double(m.faceSize(f)); });
        if (pred(c))
            return int(f);
    }
    return -1;
}

/** Every point has a point at its exact reflection across x = 0. */
bool exactlyMirrored(const geo::Mesh& m)
{
    geo::SymmetryMap map = geo::SymmetryMap::build(m, geo::axisPlane(0), 1e-12);
    if (map.unmatchedCount() != 0)
        return false;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p) && (map.onPlane(p) ? m.position(p).x != 0.0 : m.position(map.mirrorPoint(p)) != m.position(p) * glm::dvec3(-1, 1, 1)))
            return false;
    return true;
}
} // namespace

TEST_CASE("symmetryCheck counts unmatched points as asymmetry")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("setSymmetry", {{"args", {cube}}, {"axis", "x"}});
    run("symmetryCheck", {{"args", {cube}}}); // stores the cube's pairs
    run("setSymmetry", {{"args", {cube}}, {"axis", "off"}});
    run("selectComponents", {{"args", {cube}}, {"faces", {findFace(meshOf(cube), [](glm::dvec3 c) { return c.x > 40.0; })}}});
    run("polyExtrude", {{"thickness", 30}});
    run("setSymmetry", {{"args", {cube}}, {"axis", "x"}});
    json c = run("symmetryCheck", {{"args", {cube}}})[cube];
    CHECK(c["unmatched"].get<int>() == 4);
    CHECK(c["maxAsymmetry"].get<double>() == doctest::Approx(30.0));
}

TEST_CASE("symmetryRebuild mirrors the kept side of an asymmetric mesh")
{
    struct Case
    {
        int subdivisionsX;
        const char* side;
        int faces, points;
    };
    // An extruded +X side face: cut at the plane, 9 faces kept, mirrored to 18. Keeping -X leaves a split cube.
    // With a centre loop the extruded face is a top quad on +X whose inner wall lies in the plane and goes, so 8 faces are
    // kept and the block is mirrored into one twice as wide.
    for (const Case& k : {Case{1, "auto", 18, 20}, Case{1, "negative", 10, 12}, Case{2, "auto", 16, 18}, Case{2, "positive", 16, 18},
                          Case{2, "negative", 10, 12}})
    {
        CAPTURE(k.subdivisionsX);
        CAPTURE(k.side);
        run("newScene");
        std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"subdivisionsX", k.subdivisionsX}})[0];
        const geo::Mesh before = meshOf(cube);
        const int face = k.subdivisionsX == 1 ? findFace(before, [](glm::dvec3 c) { return c.x > 40.0; })
                                               : findFace(before, [](glm::dvec3 c) { return c.x > 0.0 && c.y > 40.0; });
        REQUIRE(face >= 0);
        run("selectComponents", {{"args", {cube}}, {"faces", {face}}});
        run("polyExtrude", {{"thickness", 30}});
        const int extruded = run("meshInfo", {{"args", {cube}}})["faces"].get<int>();

        json out = run("symmetryRebuild", {{"args", {cube}}, {"side", k.side}});
        CHECK(out[cube].get<int>() == 0);
        geo::Mesh m = meshOf(cube);
        CHECK(m.numFaces() == Index(k.faces));
        CHECK(m.numPoints() == Index(k.points));
        CHECK(geo::boundaryLoops(m).empty());
        CHECK(exactlyMirrored(m));

        run("setSymmetry", {{"args", {cube}}, {"axis", "x"}});
        json c = run("symmetryCheck", {{"args", {cube}}})[cube];
        CHECK(c["maxAsymmetry"].get<double>() == 0.0);
        CHECK(c["unmatched"].get<int>() == 0);
        CHECK(c["withoutStoredPartner"].get<int>() == 0);

        run("undo");
        run("undo");
        CHECK(run("meshInfo", {{"args", {cube}}})["faces"].get<int>() == extruded);
    }
}

TEST_CASE("symmetryRebuild moves only points when the halves already pair, so the other half keeps its own UVs")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"subdivisionsX", 2}})[0];
    run("setSymmetry", {{"args", {cube}}, {"axis", "x"}});
    run("symmetryCheck", {{"args", {cube}}}); // stores the cube's pairs
    run("setSymmetry", {{"args", {cube}}, {"axis", "off"}});
    run("selectComponents", {{"args", {cube}}, {"faces", {findFace(meshOf(cube), [](glm::dvec3 c) { return c.x > 40.0; })}}});
    run("moveComponents", {{"by", {20.0, 5.0, 0.0}}});
    run("select", {{"args", {cube}}});
    run("selectMode", {{"args", {"object"}}});

    const geo::Mesh before = meshOf(cube);
    const std::span<const glm::vec2> uvBefore = before.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    REQUIRE_FALSE(uvBefore.empty());
    REQUIRE_FALSE(exactlyMirrored(before));

    json out = run("symmetryRebuild", {{"args", {cube}}, {"side", "negative"}});
    CHECK(out[cube].get<int>() == 0);
    const geo::Mesh after = meshOf(cube);
    CHECK(exactlyMirrored(after));
    REQUIRE(after.numFaces() == before.numFaces());
    REQUIRE(after.numPoints() == before.numPoints());
    for (Index p = 0; p < before.pointCapacity(); ++p)
        if (before.pointValid(p) && before.position(p).x <= 0.0)
            CHECK(after.position(p) == before.position(p));
    const std::span<const glm::vec2> uvAfter = after.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    REQUIRE(uvAfter.size() == uvBefore.size());
    CHECK(std::equal(uvAfter.begin(), uvAfter.end(), uvBefore.begin()));
}

TEST_CASE("symmetryCheck reports a mirror plane the object's rotation tilts off the world axes")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"subdivisionsX", 2}})[0];
    run("setSymmetry", {{"args", {cube}}, {"axis", "x"}});
    CHECK(run("symmetryCheck", {{"args", {cube}}})[cube]["planeTilt"].get<double>() == doctest::Approx(0.0));

    // Turned a little about Y, the halves still mirror each other, across a plane no world-aligned part shares.
    scene::Graph& g = scene::Graph::current();
    g.setValue({g.find(cube)->id(), scene::Transform::kRotate, -1}, glm::dvec3(0.0, 0.75, 0.0));
    const json c = run("symmetryCheck", {{"args", {cube}}})[cube];
    CHECK(c["maxAsymmetry"].get<double>() < 1e-9);
    CHECK(c["planeTilt"].get<double>() == doctest::Approx(0.75).epsilon(0.001));

    // A quarter turn puts the plane on another world axis: square again.
    g.setValue({g.find(cube)->id(), scene::Transform::kRotate, -1}, glm::dvec3(0.0, 90.0, 0.0));
    CHECK(run("symmetryCheck", {{"args", {cube}}})[cube]["planeTilt"].get<double>() == doctest::Approx(0.0).epsilon(1e-6));
}

TEST_CASE("symmetryRebuild and symmetryCheck use the plane through the pivot")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"subdivisionsX", 2}})[0];
    // The geometry sits 30 cm along +X of the origin with the pivot at its centre, as on a part modelled in place.
    run("selectComponents", {{"args", {cube}}, {"faces", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}}});
    run("moveComponents", {{"by", {30.0, 0.0, 0.0}}});
    scene::Graph& g = scene::Graph::current();
    g.setValue({g.find(cube)->id(), scene::Transform::kRotatePivot, -1}, glm::dvec3(30.0, 0.0, 0.0));
    run("select", {{"args", {cube}}});
    run("selectMode", {{"args", {"object"}}});

    json out = run("symmetryRebuild", {{"args", {cube}}});
    CHECK(out[cube].get<int>() == 0);
    const geo::Mesh m = meshOf(cube);
    CHECK(m.numFaces() == 10);
    CHECK(m.bounds().min.x == doctest::Approx(-20.0));
    CHECK(m.bounds().max.x == doctest::Approx(80.0));

    run("setSymmetry", {{"args", {cube}}, {"axis", "x"}});
    json c = run("symmetryCheck", {{"args", {cube}}})[cube];
    CHECK(c["unmatched"].get<int>() == 0);
    CHECK(c["maxAsymmetry"].get<double>() < 1e-9);
}
