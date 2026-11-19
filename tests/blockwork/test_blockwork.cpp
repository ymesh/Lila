#include "Blockwork.h"

#include "extensions/ExtensionManager.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"
#include "ui/Tools.h"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;
using Approx = doctest::Approx;
using geo::Index;

namespace
{
json run(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }

Graph& g() { return Graph::current(); }

void fresh()
{
    auto& em = extensions::ExtensionManager::get();
    if (!em.isLoaded("Blockwork"))
        REQUIRE(em.load(LILA_TEST_BLOCKWORK_PACKAGE) == "Blockwork");
    run("newScene");
}

geo::Mesh out(const std::string& path)
{
    g().evaluate();
    Node* n = g().find(path);
    REQUIRE(n);
    const MeshPtr& m = n->as<MeshPtr>(MeshNode::kOutMesh);
    REQUIRE(m);
    return *m;
}

geo::Bounds worldBounds(const std::string& path)
{
    geo::Mesh m = out(path);
    const glm::dmat4 w = g().find(path)->as<glm::dmat4>(Transform::kWorldMatrix);
    geo::Bounds b;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            b.extend(glm::dvec3(w * glm::dvec4(m.position(p), 1.0)));
    return b;
}

bool closed(const geo::Mesh& m)
{
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isBoundaryEdge(e))
            return false;
    return m.validate().empty();
}

std::string box(glm::dvec3 origin, glm::dvec3 size, const std::string& name = "block1")
{
    return run("blockworkBlock", {{"origin", {origin.x, origin.y, origin.z}}, {"size", {size.x, size.y, size.z}}, {"name", name}}).get<std::string>();
}

/** The face whose normal is closest to n. */
Index faceFacing(const geo::Mesh& m, glm::dvec3 n)
{
    Index best = geo::kInvalid;
    double bestDot = -2.0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && glm::dot(m.faceNormal(f), n) > bestDot)
            bestDot = glm::dot(m.faceNormal(f), n), best = f;
    return best;
}

int countBlocks()
{
    int n = 0;
    for (NodeId id : g().nodes())
        if (Node* k = g().node(id); k && blockwork::isBlock(*k))
            ++n;
    return n;
}
} // namespace

TEST_CASE("blockwork: a box block stands on its footprint, pivot centred, under the level group")
{
    fresh();
    const std::string b = box({0, 0, 0}, {256, 128, 64});
    geo::Mesh m = out(b);
    CHECK(m.numFaces() == 6);
    CHECK(closed(m));
    geo::Bounds bb = worldBounds(b);
    CHECK(bb.min.x == Approx(0));
    CHECK(bb.max.x == Approx(256));
    CHECK(bb.max.z == Approx(128));
    CHECK(bb.max.y == Approx(64));
    CHECK(g().find(b)->as<glm::dvec3>(Transform::kRotatePivot).x == Approx(128));
    CHECK(g().node(g().parent(g().find(b)->id()))->name() == "Level");
    CHECK(blockwork::isBlock(*g().find(b)));
    CHECK(m.cornerAttrs().has("uv0"));
}

TEST_CASE("blockwork: negative sizes and flat blocks")
{
    fresh();
    geo::Bounds bb = worldBounds(box({0, 0, 0}, {-128, 64, -32}));
    CHECK(bb.min.x == Approx(-128));
    CHECK(bb.min.y == Approx(-32));
    CHECK(out(box({0, 0, 0}, {128, 128, 0}, "floor")).numFaces() == 1);
    geo::Mesh wall = out(box({0, 0, 0}, {128, 0, 64}, "wall"));
    CHECK(wall.numFaces() == 1);
    CHECK(std::abs(wall.faceNormal(0).y) == Approx(0).epsilon(1e-6));
    CHECK_THROWS(box({0, 0, 0}, {0, 0, 64}, "line"));
}

TEST_CASE("blockwork: a footprint drawn on a wall builds where the tool drew it")
{
    fresh();
    for (const glm::dvec3 n : {glm::dvec3(1, 0, 0), glm::dvec3(-1, 0, 0), glm::dvec3(0, 0, 1), glm::dvec3(0, 0, -1)})
    {
        glm::dvec3 u, v;
        blockwork::planeBasis(n, u, v);
        const std::string b = run("blockworkBlock", {{"origin", {0, 0, 0}}, {"size", {16, 16, 64}}, {"normal", {n.x, n.y, n.z}}, {"uAxis", {u.x, u.y, u.z}}}).get<std::string>();
        geo::Bounds want;
        for (double a : {0.0, 16.0})
            for (double c : {0.0, 16.0})
                for (double h : {0.0, 64.0})
                    want.extend(u * a + v * c + n * h);
        const geo::Bounds got = worldBounds(b);
        for (int i = 0; i < 3; ++i)
        {
            CHECK(got.min[i] == Approx(want.min[i]));
            CHECK(got.max[i] == Approx(want.max[i]));
        }
    }
}

TEST_CASE("blockwork: shapes are closed")
{
    fresh();
    for (const char* shape : {"box", "wedge", "cylinder", "arch", "stairs"})
    {
        CAPTURE(shape);
        const std::string b = run("blockworkBlock", {{"shape", shape}, {"size", {256, 192, 160}}, {"name", shape}}).get<std::string>();
        geo::Mesh m = out(b);
        CHECK(closed(m));
        CHECK(worldBounds(b).max.y == Approx(160));
    }
    CHECK(out("cylinder").numFaces() == 14);
    CHECK(out("wedge").numFaces() == 5);
    CHECK(out("stairs").numFaces() == 8 * 2 + 4);
    const std::string l = run("blockworkBlock", {{"shape", "prism"}, {"profile", {{0, 0}, {256, 0}, {256, 128}, {128, 128}, {128, 256}, {0, 256}}},
                                                 {"size", {0, 0, 64}}, {"name", "ell"}})
                              .get<std::string>();
    geo::Mesh ell = out(l);
    CHECK(closed(ell));
    CHECK(ell.numFaces() == 8);
}

TEST_CASE("blockwork: a small arch keeps an opening half its width")
{
    blockwork::Frame f;
    f.size = {48, 16, 48};
    const geo::Mesh m = blockwork::buildShape(blockwork::Shape::Arch, f, {});
    bool left = false, right = false;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p) && std::abs(m.position(p).y) < 1e-6)
        {
            left |= std::abs(m.position(p).x - 12.0) < 1e-6;
            right |= std::abs(m.position(p).x - 36.0) < 1e-6;
        }
    CHECK(left);
    CHECK(right);
    CHECK(blockwork::archFrame(16, 32, 25) == Approx(4));
    CHECK(blockwork::archFrame(400, 300, 25) == Approx(25));
}

TEST_CASE("blockwork: a selection of blocks snaps to the grid, anything else doesn't")
{
    fresh();
    const std::string b = run("blockworkBlock", {{"origin", {32, 0, 16}}, {"size", {48, 16, 48}}}).get<std::string>();
    run("select", {{"args", {b}}});
    std::optional<ui::SelectionSnap> s = ui::tools::selectionSnap();
    REQUIRE(s);
    CHECK(s->moveStep == Approx(blockwork::gridStep()));
    CHECK(s->rotateStep == Approx(15));
    CHECK(s->anchor.x == Approx(32));
    CHECK(s->anchor.z == Approx(16));
    CHECK(s->extent.x == Approx(48));
    const std::string cube = run("polyCube")[0].get<std::string>();
    run("select", {{"args", {b, cube}}});
    CHECK_FALSE(ui::tools::selectionSnap());
    ui::tools::setSnap(ui::SnapRotateHold, true);
    run("select", {{"args", {b}}});
    CHECK_FALSE(ui::tools::selectionSnap());
    ui::tools::setSnap(ui::SnapRotateHold, false);
}

TEST_CASE("blockwork: bevel rounds exposed edges and leaves the ones resting on another block square")
{
    fresh();
    run("blockworkBlock", {{"origin", {-256, -32, -256}}, {"size", {512, 512, 32}}, {"name", "slab"}});
    const std::string b = run("blockworkBlock", {{"size", {128, 128, 128}}, {"name", "box"}}).get<std::string>();
    // A shutter on the box's side: its back edges lie on the box, the box's own corners don't touch it.
    run("blockworkBlock", {{"origin", {128, 40, 40}}, {"size", {8, 48, 48}}, {"name", "shutter"}});
    run("blockworkBevel", {{"args", {b}}, {"width", 4.0}});
    const geo::Mesh m = out(b);
    bool bottomEdge = false, topCorner = false, sideCorner = false;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p))
            continue;
        const glm::dvec3 q = m.position(p);
        // The edge along the ground on the x = 0 side stays where it was; the top one is cut back.
        bottomEdge |= std::abs(q.y) < 1e-6 && std::abs(q.x) < 1e-6;
        topCorner |= std::abs(q.y - 128.0) < 1e-6 && std::abs(q.x) < 1e-6;
        sideCorner |= glm::length(q - glm::dvec3(128, 0, 0)) < 1e-6;
    }
    CHECK(bottomEdge);
    CHECK_FALSE(topCorner);
    CHECK_FALSE(sideCorner);
    CHECK(m.numFaces() > 6);

    run("blockworkBevel", {{"args", {b}}, {"remove", true}});
    CHECK(out(b).numFaces() == 6);
}

TEST_CASE("blockwork: an edge another block covers part of stays square only where it is covered")
{
    fresh();
    // A parapet butting the back of another near its end, as at a corner of a roof.
    const std::string back = box({0, 0, 0}, {800, 24, 64}, "back");
    box({776, 0, -400}, {24, 400, 64}, "side");
    run("blockworkBevel", {{"args", {back}}, {"width", 2.0}, {"segments", 2}});
    const geo::Mesh m = out(back);
    const glm::dmat4 w = g().find(back)->as<glm::dmat4>(Transform::kWorldMatrix);
    bool coveredSquare = false, exposedSquare = false;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p))
            continue;
        const glm::dvec3 q(w * glm::dvec4(m.position(p), 1.0));
        coveredSquare |= glm::length(q - glm::dvec3(776, 64, 0)) < 1e-3;
        exposedSquare |= std::abs(q.y - 64) < 1e-3 && std::abs(q.z) < 1e-3 && q.x < 770;
    }
    CHECK(coveredSquare);
    CHECK_FALSE(exposedSquare);
    CHECK(closed(m));
}

TEST_CASE("blockwork: a rounded bevel shades smooth through its corners, a chamfer stays faceted")
{
    const auto hardCreases = [](const geo::Mesh& m) {
        const auto hard = m.edgeAttrs().get<bool>(geo::attr::kHard);
        int n = 0;
        for (Index e = 0; e < m.edgeCapacity(); ++e)
        {
            if (!m.edgeValid(e) || m.edgeFaceCount(e) != 2 || !hard[e])
                continue;
            const Index c = m.edgeCorner(e);
            const double d = glm::dot(m.faceNormal(m.cornerFace(c)), m.faceNormal(m.cornerFace(m.cornerRadial(c))));
            // Hard edges other than the box's own square corners.
            n += d > 0.1 ? 1 : 0;
        }
        return n;
    };
    fresh();
    const std::string b = run("blockworkBlock", {{"size", {256, 192, 24}}, {"name", "lid"}}).get<std::string>();
    run("blockworkBevel", {{"args", {b}}, {"width", 2.0}, {"segments", 2}});
    CHECK(hardCreases(out(b)) == 0);
    run("blockworkBevel", {{"args", {b}}, {"width", 2.0}, {"segments", 1}});
    CHECK(hardCreases(out(b)) > 0);
}

TEST_CASE("blockwork: stairs climb the way direction turns them")
{
    fresh();
    const std::string s = run("blockworkBlock", {{"shape", "stairs"}, {"size", {100, 400, 200}}, {"steps", 4}}).get<std::string>();
    geo::Mesh m = out(s);
    // The highest step's tread is at the far end (+z) for direction 0.
    double topZ = -1e9;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p) && m.position(p).y > 199.0)
            topZ = std::max(topZ, m.position(p).z);
    CHECK(topZ == Approx(400));
    double lowZ = 1e9;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p) && m.position(p).y > 199.0)
            lowZ = std::min(lowZ, m.position(p).z);
    CHECK(lowZ == Approx(300));
}

TEST_CASE("blockwork: texture lock keeps world texel density through extend")
{
    fresh();
    const std::string b = box({0, 0, 0}, {200, 200, 200});
    geo::Mesh before = out(b);
    const Index top = faceFacing(before, {0, 1, 0});
    run("blockworkExtend", {{"args", {b}}, {"faces", {top}}, {"distance", 100}});
    geo::Mesh m = out(b);
    CHECK(worldBounds(b).max.y == Approx(300));
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
    REQUIRE(!uv.empty());
    // Every corner's uv is its position over the 100 cm tile, so the side faces did not stretch.
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const blockwork::FaceMapping fm = blockwork::faceMapping(m, f);
        m.forEachFaceCorner(f, [&](Index c) {
            glm::dvec2 want = blockwork::mapPoint(fm, m.faceNormal(f), m.position(m.cornerPoint(c)), 100.0);
            CHECK(double(uv[c].x) == Approx(want.x).epsilon(1e-5));
            CHECK(double(uv[c].y) == Approx(want.y).epsilon(1e-5));
        });
    }
    // A wall's v runs up the world: the 300 cm wall spans three repeats.
    const Index side = faceFacing(m, {0, 0, 1});
    float vmin = 1e9f, vmax = -1e9f;
    m.forEachFaceCorner(side, [&](Index c) { vmin = std::min(vmin, uv[c].y), vmax = std::max(vmax, uv[c].y); });
    CHECK(double(vmax - vmin) == Approx(3.0));
}

TEST_CASE("blockwork: mapping axes read like a viewer facing the face")
{
    blockwork::FaceMapping m;
    glm::dvec3 u, v;
    blockwork::mappingAxes(m, {0, 0, 1}, u, v);
    CHECK(u.x == Approx(1));
    CHECK(v.y == Approx(1));
    blockwork::mappingAxes(m, {1, 0, 0}, u, v);
    CHECK(u.z == Approx(-1));
    CHECK(v.y == Approx(1));
    blockwork::mappingAxes(m, {0, 1, 0}, u, v);
    CHECK(u.x == Approx(1));
    CHECK(v.z == Approx(-1));
    m.rotate = 90.0f;
    blockwork::mappingAxes(m, {0, 0, 1}, u, v);
    CHECK(u.y == Approx(1));
    CHECK(v.x == Approx(-1));
    // A slope maps on its own plane, so a ramp does not stretch.
    m.rotate = 0.0f;
    const glm::dvec3 n = glm::normalize(glm::dvec3(0, 1, 1));
    blockwork::mappingAxes(m, n, u, v);
    CHECK(glm::dot(u, n) == Approx(0).epsilon(1e-9));
    CHECK(glm::dot(v, n) == Approx(0).epsilon(1e-9));
}

TEST_CASE("blockwork: hollow walls a block, open faces bridged")
{
    fresh();
    const std::string b = box({0, 0, 0}, {256, 256, 256});
    run("blockworkHollow", {{"args", {b}}, {"thickness", 16}});
    geo::Mesh m = out(b);
    CHECK(m.numFaces() == 12);
    CHECK(closed(m));
    // The inner box is the outer one shrunk by the thickness.
    double innerMax = 0;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p) && m.position(p).x < 250 && m.position(p).x > 10)
            innerMax = std::max(innerMax, m.position(p).x);
    CHECK(innerMax == Approx(240));

    fresh();
    const std::string r = box({0, 0, 0}, {256, 256, 256}, "room");
    const Index top = faceFacing(out(r), {0, 1, 0});
    run("blockworkHollow", {{"args", {r}}, {"thickness", 16}, {"faces", {top}}});
    geo::Mesh open = out(r);
    CHECK(open.numFaces() == 5 + 5 + 4);
    CHECK(closed(open));

    fresh();
    const std::string o = box({0, 0, 0}, {256, 256, 256}, "shell");
    run("blockworkHollow", {{"args", {o}}, {"thickness", 16}, {"inward", false}});
    CHECK(closed(out(o)));
    CHECK(worldBounds(o).max.x == Approx(272));
}

TEST_CASE("blockwork: clip keeps a side and caps it")
{
    fresh();
    const std::string b = box({0, 0, 0}, {256, 128, 128});
    run("blockworkClip", {{"args", {b}}, {"point", {128, 0, 0}}, {"normal", {-1, 0, 0}}});
    CHECK(worldBounds(b).max.x == Approx(128));
    CHECK(closed(out(b)));

    fresh();
    const std::string w = box({0, 0, 0}, {256, 128, 128});
    json r = run("blockworkClip", {{"args", {w}}, {"point", {128, 64, 0}}, {"normal", {1, 1, 0}}, {"keep", "both"}});
    REQUIRE(r.size() == 2);
    CHECK(closed(out(r[0].get<std::string>())));
    CHECK(closed(out(r[1].get<std::string>())));
    CHECK(countBlocks() == 2);
}

TEST_CASE("blockwork: carve cuts every block the cutter overlaps")
{
    fresh();
    const std::string wall = box({0, 0, 0}, {512, 256, 32}, "wall");
    const std::string other = box({0, 0, 200}, {512, 256, 32}, "other");
    const int before = out(wall).numFaces();
    const std::string door = box({200, 0, -16}, {96, 192, 64}, "door");
    json r = run("blockworkCarve", {{"args", {door}}});
    CHECK(r.size() == 1);
    CHECK(g().find(door) == nullptr);
    geo::Mesh m = out(wall);
    CHECK(m.numFaces() > before);
    CHECK(closed(m));
    CHECK(out(other).numFaces() == 6);
}

TEST_CASE("blockwork: carving a wedge into a face takes out the wedge")
{
    auto volume = [](const geo::Mesh& m, const glm::dmat4& w) {
        double v = 0.0;
        for (Index f = 0; f < m.faceCapacity(); ++f)
        {
            if (!m.faceValid(f))
                continue;
            std::vector<glm::dvec3> p;
            m.forEachFaceCorner(f, [&](Index c) { p.push_back(glm::dvec3(w * glm::dvec4(m.position(m.cornerPoint(c)), 1.0))); });
            for (size_t i = 1; i + 1 < p.size(); ++i)
                v += glm::dot(p[0], glm::cross(p[i], p[i + 1])) / 6.0;
        }
        return v;
    };
    const glm::dvec3 centre(128, 128, 128);
    for (const glm::dvec3 n : {glm::dvec3(1, 0, 0), glm::dvec3(-1, 0, 0), glm::dvec3(0, 0, 1), glm::dvec3(0, 0, -1), glm::dvec3(0, 1, 0)})
    {
        CAPTURE(n);
        fresh();
        const std::string b = box({0, 0, 0}, {256, 256, 256});
        glm::dvec3 u, v;
        blockwork::planeBasis(n, u, v);
        const glm::dvec3 o = centre + n * 128.0 - u * 32.0 - v * 32.0;
        const std::string cutter = run("blockworkBlock", {{"shape", "wedge"}, {"origin", {o.x, o.y, o.z}}, {"size", {64, 64, -96}}, {"normal", {n.x, n.y, n.z}},
                                                          {"uAxis", {u.x, u.y, u.z}}, {"name", "cutter1"}})
                                       .get<std::string>();
        run("blockworkCarve", {{"args", {cutter}}});
        geo::Mesh m = out(b);
        CHECK(closed(m));
        CHECK(run("meshCheck", {{"args", {b}}})["shells"] == 1);
        CHECK(volume(m, g().find(b)->as<glm::dmat4>(Transform::kWorldMatrix)) == Approx(256.0 * 256.0 * 256.0 - 0.5 * 64 * 64 * 96).epsilon(1e-6));
    }
}

TEST_CASE("blockwork: a prism stays on the outline drawn, whichever way the last point lies")
{
    fresh();
    const std::string p = run("blockworkBlock", {{"shape", "prism"}, {"origin", {100, 0, 100}}, {"size", {-32, -32, 16}}, {"normal", {0, 1, 0}},
                                                 {"uAxis", {1, 0, 0}}, {"profile", {{0, 0}, {32, 0}, {32, 32}}}})
                              .get<std::string>();
    const geo::Bounds b = worldBounds(p);
    CHECK(b.min.x == Approx(100));
    CHECK(b.max.x == Approx(132));
    CHECK(b.min.z == Approx(100));
    CHECK(b.max.z == Approx(132));
}

TEST_CASE("blockwork: carving an arch into a wall cuts a doorway up the wall")
{
    fresh();
    const std::string b = box({0, 0, 0}, {256, 256, 128});
    const std::string door = run("blockworkBlock", {{"shape", "arch"}, {"origin", {80, 0, 128}}, {"size", {96, 192, -128}}, {"normal", {0, 0, 1}},
                                                    {"uAxis", {1, 0, 0}}, {"opening", true}, {"name", "cutter1"}})
                                 .get<std::string>();
    const geo::Bounds d = worldBounds(door);
    CHECK(d.min.x == Approx(80));
    CHECK(d.max.x == Approx(176));
    CHECK(d.max.y == Approx(192));
    CHECK(d.min.z == Approx(0));
    CHECK(d.max.z == Approx(128));
    run("blockworkCarve", {{"args", {door}}});
    CHECK(closed(out(b)));
    CHECK(run("meshCheck", {{"args", {b}}})["shells"] == 1);
}

TEST_CASE("blockwork: carving goes through a hollowed wall and no further")
{
    fresh();
    const std::string r = box({0, 0, 0}, {256, 256, 256}, "room");
    run("blockworkHollow", {{"args", {r}}, {"thickness", 16}});
    const geo::Mesh m = out(r);
    const geo::Bounds bb = m.bounds();
    const glm::dvec3 c = (bb.min + bb.max) * 0.5;
    // From the +X wall's outside, inward: out through its inner face, not across the room.
    CHECK(blockwork::depthThrough(m, {{bb.max.x, c.y, c.z}, {bb.max.x, c.y + 64, c.z - 64}}, {-1, 0, 0}) == Approx(16));
    // A ray starting off the block leaves nothing.
    CHECK(blockwork::depthThrough(m, {{bb.max.x, bb.max.y + 50, c.z}}, {-1, 0, 0}) == Approx(0));

    fresh();
    const geo::Mesh solid = out(box({0, 0, 0}, {256, 128, 64}));
    const geo::Bounds sb = solid.bounds();
    const glm::dvec3 sc = (sb.min + sb.max) * 0.5;
    CHECK(blockwork::depthThrough(solid, {{sb.min.x, sc.y, sc.z}}, {1, 0, 0}) == Approx(sb.max.x - sb.min.x));
}

TEST_CASE("blockwork: join merges touching blocks")
{
    fresh();
    const std::string a = box({0, 0, 0}, {128, 128, 128}, "a");
    box({128, 0, 0}, {128, 128, 128}, "b");
    run("blockworkJoin", {{"args", {a}}});
    CHECK(countBlocks() == 1);
    CHECK(worldBounds(a).max.x == Approx(256));
    CHECK(closed(out(a)));
}

TEST_CASE("blockwork: extend copy builds a new block from the faces")
{
    fresh();
    const std::string a = box({0, 0, 0}, {128, 128, 128}, "a");
    const Index side = faceFacing(out(a), {1, 0, 0});
    const std::string made = run("blockworkExtend", {{"args", {a}}, {"faces", {side}}, {"distance", 64}, {"mode", "copy"}}).get<std::string>();
    CHECK(made != a);
    geo::Bounds b = worldBounds(made);
    CHECK(b.min.x == Approx(128));
    CHECK(b.max.x == Approx(192));
    CHECK(closed(out(made)));
    CHECK(countBlocks() == 2);
}

TEST_CASE("blockwork: overlapping faces are removed from both blocks on request")
{
    fresh();
    const std::string a = box({0, 0, 0}, {128, 128, 128}, "a");
    const std::string b = run("blockworkBlock", {{"origin", {128, 0, 0}}, {"size", {128, 128, 128}}, {"name", "b"}, {"removeOverlap", true}})
                              .get<std::string>();
    CHECK(out(a).numFaces() == 5);
    CHECK(out(b).numFaces() == 5);
}

TEST_CASE("blockwork: grid steps")
{
    fresh();
    CHECK(run("blockworkGrid", {{"step", 32}})["step"].get<double>() == Approx(32));
    CHECK(run("blockworkGrid", {{"finer", true}})["step"].get<double>() == Approx(16));
    CHECK(run("blockworkGrid", {{"coarser", true}})["step"].get<double>() == Approx(32));
    CHECK(blockwork::snap(37.0, 16.0) == Approx(32));
}

namespace
{
glm::dvec2 cornerUv(const geo::Mesh& m, Index f, const glm::dvec3& at)
{
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
    glm::dvec2 out(1e9);
    double best = 1e9;
    m.forEachFaceCorner(f, [&](Index c) {
        const double d = glm::length(m.position(m.cornerPoint(c)) - at);
        if (d < best)
            best = d, out = glm::dvec2(uv[c]);
    });
    return out;
}

void faceUvBounds(const geo::Mesh& m, Index f, glm::dvec2& lo, glm::dvec2& hi)
{
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
    lo = glm::dvec2(1e9), hi = glm::dvec2(-1e9);
    m.forEachFaceCorner(f, [&](Index c) { lo = glm::min(lo, glm::dvec2(uv[c])), hi = glm::max(hi, glm::dvec2(uv[c])); });
}
} // namespace

TEST_CASE("blockwork: shift, fit and justify")
{
    fresh();
    const std::string b = box({0, 0, 0}, {300, 150, 200});
    const Index front = faceFacing(out(b), {0, 0, 1});
    glm::dvec2 lo0, hi0, lo, hi;
    faceUvBounds(out(b), front, lo0, hi0);
    run("blockworkTexture", {{"args", {b}}, {"faces", {front}}, {"shift", {0.25, 0}}});
    faceUvBounds(out(b), front, lo, hi);
    CHECK(lo.x == Approx(lo0.x + 0.25));

    run("blockworkTexture", {{"args", {b}}, {"faces", {front}}, {"fit", "both"}});
    faceUvBounds(out(b), front, lo, hi);
    CHECK(lo.x == Approx(0).epsilon(1e-5));
    CHECK(hi.x == Approx(1));
    CHECK(lo.y == Approx(0).epsilon(1e-5));
    CHECK(hi.y == Approx(1));

    run("blockworkTexture", {{"args", {b}}, {"faces", {front}}, {"reset", true}});
    run("blockworkTexture", {{"args", {b}}, {"faces", {front}}, {"justify", "right"}});
    faceUvBounds(out(b), front, lo, hi);
    CHECK(hi.x == Approx(1));
    CHECK(hi.x - lo.x == Approx(3));
}

TEST_CASE("blockwork: copying a texture round a corner keeps it continuous")
{
    fresh();
    const std::string b = box({0, 0, 0}, {200, 200, 200});
    geo::Mesh m = out(b);
    const Index front = faceFacing(m, {0, 0, 1}), side = faceFacing(m, {1, 0, 0});
    run("blockworkTexture", {{"args", {b}}, {"faces", {front}}, {"shift", {0.3, 0.1}}, {"turn", 30}});
    run("blockworkCopyTexture", {{"args", {b}}, {"source", b}, {"sourceFace", front}, {"faces", {side}}});
    m = out(b);
    // The corner they share has the same uv on both faces.
    for (const glm::dvec3 at : {glm::dvec3(200, 0, 200), glm::dvec3(200, 200, 200)})
    {
        const glm::dvec2 a = cornerUv(m, front, at), c = cornerUv(m, side, at);
        CHECK(a.x == Approx(c.x).epsilon(1e-4));
        CHECK(a.y == Approx(c.y).epsilon(1e-4));
    }
}

TEST_CASE("blockwork: align a texture's bottom to an edge")
{
    fresh();
    const std::string b = box({0, 0, 0}, {256, 128, 128});
    geo::Mesh m = out(b);
    const Index front = faceFacing(m, {0, 0, 1});
    std::vector<Index> pts = m.facePoints(front);
    run("blockworkAlignEdge", {{"args", {b}}, {"face", front}, {"edge", 0}, {"side", "bottom"}, {"fit", "height"}});
    m = out(b);
    const glm::dvec3 pa = m.position(pts[0]), pb = m.position(pts[1]);
    CHECK(cornerUv(m, front, pa).y == Approx(0).epsilon(1e-5));
    CHECK(cornerUv(m, front, pb).y == Approx(0).epsilon(1e-5));
    CHECK(cornerUv(m, front, pa).x == Approx(0).epsilon(1e-5));
    glm::dvec2 lo, hi;
    faceUvBounds(m, front, lo, hi);
    CHECK(hi.y - lo.y == Approx(1));
}

TEST_CASE("blockwork: a strip of faces maps continuously along it")
{
    fresh();
    const std::string road = run("blockworkBlock", {{"origin", {0, 0, 0}}, {"size", {400, 100, 0}}, {"name", "road"}}).get<std::string>();
    // Split the floor into four quads along x, then lay the strip.
    geo::Mesh m = blockwork::baseMesh(g(), g().find(road)->id());
    REQUIRE(m.numFaces() == 1);
    std::vector<glm::dvec3> pts;
    std::vector<Index> sizes, idx;
    for (int i = 0; i <= 4; ++i)
        pts.push_back({100.0 * i, 0, 0}), pts.push_back({100.0 * i, 0, 100});
    for (Index i = 0; i < 4; ++i)
        sizes.push_back(4), idx.insert(idx.end(), {2 * i, 2 * i + 1, 2 * i + 3, 2 * i + 2});
    geo::Mesh strip = geo::Mesh::fromPolygons(pts, sizes, idx);
    if (strip.faceNormal(0).y < 0)
        for (Index f = 0; f < 4; ++f)
            strip.flipFace(f);
    blockwork::setBaseMesh(g(), g().find(road)->id(), strip);
    run("blockworkTextureStrip", {{"args", {road}}, {"faces", {0, 1, 2, 3}}});
    m = out(road);
    for (Index f = 0; f + 1 < 4; ++f)
    {
        const glm::dvec3 at(100.0 * (f + 1), 0, 50);
        const glm::dvec2 a = cornerUv(m, f, glm::dvec3(at.x, 0, 0)), c = cornerUv(m, f + 1, glm::dvec3(at.x, 0, 0));
        CHECK(a.x == Approx(c.x).epsilon(1e-4));
    }
    glm::dvec2 lo, hi;
    faceUvBounds(m, 0, lo, hi);
    CHECK(std::abs(hi.y - lo.y) == Approx(1));
}

TEST_CASE("blockwork: texture lock off keeps the uvs, which then stretch")
{
    fresh();
    const std::string b = box({0, 0, 0}, {200, 200, 200});
    const Index top = faceFacing(out(b), {0, 1, 0});
    const Index side = faceFacing(out(b), {0, 0, 1});
    glm::dvec2 lo0, hi0, lo, hi;
    faceUvBounds(out(b), side, lo0, hi0);
    run("blockworkTextureLock", {{"args", {b}}, {"on", false}});
    faceUvBounds(out(b), side, lo, hi);
    CHECK(hi.y - lo.y == Approx(hi0.y - lo0.y));
    run("blockworkExtend", {{"args", {b}}, {"faces", {top}}, {"distance", 200}});
    faceUvBounds(out(b), side, lo, hi);
    CHECK(hi.y - lo.y == Approx(hi0.y - lo0.y));
    run("blockworkTextureLock", {{"args", {b}}, {"on", true}});
    faceUvBounds(out(b), side, lo, hi);
    CHECK(hi.y - lo.y == Approx(4));
}

TEST_CASE("blockwork: a placeholder image becomes a material mapped at the texel density")
{
    fresh();
    const std::string b = box({0, 0, 0}, {512, 512, 512});
    const std::string mat = run("blockworkPlaceholder", {{"name", "BwTestDev"}, {"size", 256}}).get<std::string>();
    run("blockworkMaterial", {{"args", {b}}, {"material", mat}});
    geo::Mesh m = out(b);
    // 256 px at 5.12 px/cm repeats every 50 cm: a 512 cm face spans 10.24 repeats.
    glm::dvec2 lo, hi;
    faceUvBounds(m, faceFacing(m, {0, 0, 1}), lo, hi);
    CHECK(hi.x - lo.x == Approx(10.24));
    CHECK(m.materialNames().size() >= 1);
}

TEST_CASE("blockwork: entity classes load and place")
{
    fresh();
    json classes = run("blockworkEntityClasses", {{"reload", true}});
    REQUIRE(classes.size() >= 10);
    const std::string ps = run("blockworkEntity", {{"class", "PlayerStart"}, {"position", {100, 0, 50}}, {"turn", 90}}).get<std::string>();
    Node* n = g().find(ps);
    REQUIRE(n);
    CHECK(n->type().isA("locator"));
    CHECK(n->as<std::string>(n->type().attrIndex("entityClass")) == "PlayerStart");
    CHECK(n->type().attrIndex("PlayerStartTag") >= 0);

    const std::string t = run("blockworkEntity", {{"class", "Trigger"}, {"origin", {0, 0, 0}}, {"size", {256, 128, 512}}}).get<std::string>();
    CHECK(blockwork::isBlock(*g().find(t)));
    geo::Bounds b = worldBounds(t);
    CHECK(b.max.x == Approx(256));
    CHECK(b.max.y == Approx(128));
    CHECK(b.max.z == Approx(512));

    g().select({});
    json sent = run("blockworkSendEntities", {{"dryRun", true}});
    REQUIRE(sent["entities"].size() == 2);
    for (const json& e : sent["entities"])
    {
        if (e["name"] == "Trigger1")
        {
            // A 256 x 128 x 512 Lila box is 512 x 256 x 128 in Unreal: forward is Lila z, right Lila x, up Lila y.
            std::vector<double> s = e["scale"];
            std::sort(s.begin(), s.end());
            CHECK(s[0] == Approx(0.64));
            CHECK(s[1] == Approx(1.28));
            CHECK(s[2] == Approx(2.56));
        }
        else
            CHECK(e["class"] == "/Script/Engine.PlayerStart");
        CHECK(!e["id"].get<std::string>().empty());
    }
    // A second send keeps the same ids.
    json again = run("blockworkSendEntities", {{"dryRun", true}});
    CHECK(again["entities"][0]["id"] == sent["entities"][0]["id"]);
}

TEST_CASE("blockwork: a block becomes a volume and back")
{
    fresh();
    const std::string b = box({0, 0, 0}, {128, 128, 128});
    run("blockworkVolume", {{"args", {b}}, {"class", "KillZone"}});
    Node* n = g().find(b);
    CHECK(n->as<std::string>(n->type().attrIndex("entityClass")) == "KillZone");
    CHECK(n->type().attrIndex("DamagePerSec") >= 0);
    CHECK(out(b).numFaces() == 6);
    run("blockworkVolume", {{"args", {b}}, {"clear", true}});
    n = g().find(b);
    CHECK(n->type().attrIndex("entityClass") < 0);
    CHECK(n->type().attrIndex("DamagePerSec") < 0);
    CHECK_THROWS(run("blockworkVolume", {{"args", {b}}, {"class", "PlayerStart"}}));
}

TEST_CASE("blockwork: merge leaves tool faces out and hulls convex blocks")
{
    fresh();
    run("blockworkToolMaterials");
    const std::string a = box({0, 0, 0}, {256, 128, 256}, "a");
    const std::string b = box({512, 0, 0}, {128, 128, 128}, "b");
    const Index bottom = faceFacing(out(a), {0, -1, 0});
    run("blockworkMaterial", {{"args", {a}}, {"material", "ToolNodraw"}, {"faces", {bottom}}});
    json r = run("blockworkMerge", {{"args", {"Level"}}});
    CHECK(r["collision"] == "hulls");
    CHECK(r["hulls"] == 2);
    geo::Mesh m = out(r["mesh"].get<std::string>());
    CHECK(m.numFaces() == 11);
    CHECK(m.cornerAttrs().has("uv0"));

    // A hollow room is concave: auto collides as the mesh itself and says so.
    run("blockworkHollow", {{"args", {a}}, {"thickness", 16}});
    r = run("blockworkMerge", {{"args", {"Level"}}});
    CHECK(r["collision"] == "mesh");
    CHECK(r["concave"].size() == 1);
    CHECK(r["hulls"] == 0);
}

TEST_CASE("blockwork: visgroups hide members while any of their groups is hidden")
{
    fresh();
    const std::string a = box({0, 0, 0}, {64, 64, 64}, "a");
    const std::string b = box({128, 0, 0}, {64, 64, 64}, "b");
    run("blockworkVisgroup", {{"args", {"Upstairs"}}, {"add", {a, b}}});
    run("blockworkVisgroup", {{"args", {"Props"}}, {"add", {b}}});
    run("blockworkVisgroup", {{"args", {"Upstairs"}}, {"hidden", true}});
    CHECK_FALSE(g().find(a)->as<bool>(Transform::kVisibility));
    CHECK_FALSE(g().find(b)->as<bool>(Transform::kVisibility));
    run("blockworkVisgroup", {{"args", {"Upstairs"}}, {"hidden", false}});
    run("blockworkVisgroup", {{"args", {"Props"}}, {"hidden", true}});
    CHECK(g().find(a)->as<bool>(Transform::kVisibility));
    CHECK_FALSE(g().find(b)->as<bool>(Transform::kVisibility));
    json all = run("blockworkVisgroup");
    CHECK(all.size() == 2);
}

TEST_CASE("blockwork: prefabs save, place on a surface and update every copy")
{
    fresh();
    const auto dir = std::filesystem::temp_directory_path() / "lila_blockwork_prefabs";
    std::filesystem::remove_all(dir);
    run("blockworkPrefabs", {{"folder", dir.generic_string()}});
    const std::string a = box({0, 0, 0}, {128, 64, 32}, "base");
    run("blockworkEntity", {{"class", "PlayerStart"}, {"position", {64, 32, 32}}});
    const std::string file = run("blockworkPrefabSave", {{"args", {a, "PlayerStart1"}}, {"name", "Spawn"}}).get<std::string>();
    CHECK(std::filesystem::exists(file));

    run("newScene");
    run("blockworkPrefabs", {{"folder", dir.generic_string()}});
    const std::string p1 = run("blockworkPrefabPlace", {{"args", {"Spawn"}}, {"position", {1000, 0, 0}}}).get<std::string>();
    const std::string p2 = run("blockworkPrefabPlace", {{"args", {"Spawn"}}, {"position", {0, 0, 0}}, {"normal", {1, 0, 0}}}).get<std::string>();
    CHECK(g().children(g().find(p1)->id()).size() == 2);
    // The first copy stands where it was put; the pivot was the base's centre.
    std::string base1;
    for (NodeId c : g().children(g().find(p1)->id()))
        if (blockwork::isBlock(*g().node(c)))
            base1 = g().fullPath(c);
    REQUIRE(!base1.empty());
    geo::Bounds bb = worldBounds(base1);
    CHECK(bb.center().x == Approx(1000));
    // The second stands on a wall facing +x: its height runs along x.
    std::string base2;
    for (NodeId c : g().children(g().find(p2)->id()))
        if (blockwork::isBlock(*g().node(c)))
            base2 = g().fullPath(c);
    CHECK(worldBounds(base2).size().x == Approx(32));
    CHECK(worldBounds(base2).size().y == Approx(128));

    // Grow the first copy's block and update: the second follows.
    const Index top = faceFacing(out(base1), {0, 1, 0});
    run("blockworkExtend", {{"args", {base1}}, {"faces", {top}}, {"distance", 64}});
    json rebuilt = run("blockworkPrefabUpdate", {{"args", {base1}}});
    CHECK(rebuilt.size() == 1);
    for (NodeId c : g().children(g().find(p2)->id()))
        if (blockwork::isBlock(*g().node(c)))
            base2 = g().fullPath(c);
    CHECK(worldBounds(base2).size().x == Approx(96));
}
