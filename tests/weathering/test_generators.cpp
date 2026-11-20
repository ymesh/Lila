#include "TestUtil.h"

#include "GenCommon.h"
#include "GenPlaceholders.h"
#include "RoofLayout.h"
#include "RooftopLayout.h"
#include "Truss.h"
#include "WindowLayout.h"
#include "WireLayout.h"

#include "curve/CurveNodes.h"
#include "curve/Spline.h"
#include "curve/Tessellate.h"
#include "gn/Gn.h"
#include "gn/Geometry.h"
#include "scene/MeshBoolean.h"
#include "scene/Transform.h"
#include "ops/CommandOptions.h"
#include "ui/Actions.h"
#include "ui/Menus.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

using namespace weatheringtest;
namespace wg = weathering::gen;

namespace
{
/** A box mesh from lo to hi, closed. */
geo::Mesh boxBetween(const glm::dvec3& lo, const glm::dvec3& hi)
{
    geo::Mesh m;
    wg::addBox(m, lo, hi);
    return m;
}

/** A plane tilted about X: width along X, run along Z rising by `rise`. */
geo::Mesh slope(double width, double run, double rise)
{
    geo::Mesh m;
    const auto a = m.addPoint({0, 0, run}), b = m.addPoint({width, 0, run}), c = m.addPoint({width, rise, 0}), d = m.addPoint({0, rise, 0});
    m.addFace({a, b, c, d});
    return m;
}

/** A wall from x 0 to 400, y 0 to 300, z 0 to 30, with a hole through it. */
geo::Mesh wallWithHole(const glm::dvec3& lo, const glm::dvec3& hi)
{
    const geo::Mesh wall = boxBetween({0, 0, 0}, {400, 300, 30});
    const geo::Mesh hole = boxBetween(lo, hi);
    const scene::BooleanOperand ops[] = {{&wall, glm::dmat4(1.0)}, {&hole, glm::dmat4(1.0)}};
    scene::BooleanOptions bo;
    bo.op = scene::BooleanOp::Difference;
    scene::BooleanResult r = scene::meshBoolean(ops, glm::dmat4(1.0), bo);
    REQUIRE(r.ok());
    return r.mesh;
}

/** Share of sample points on the region not under any placed piece's footprint. */
double uncovered(const weathering::roof::Region& R, const std::vector<weathering::roof::Placed>& placed, double inset)
{
    int total = 0, open = 0;
    for (double a = R.a0 + inset; a < R.a1 - inset; a += 3.0)
        for (double v = R.v0 + inset; v < R.v1 - inset; v += 3.0)
        {
            if (!R.inside({a, v}))
                continue;
            ++total;
            bool hit = false;
            for (const auto& p : placed)
                if (p.slot == 0 && wg::insidePolygon(std::span<const glm::dvec2>(p.footprint.data(), 4), {a, v}))
                {
                    hit = true;
                    break;
                }
            open += hit ? 0 : 1;
        }
    REQUIRE(total > 0);
    return double(open) / total;
}

std::string meshFrom(const char* name, const geo::Mesh& m)
{
    const json made = run("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", name}});
    const std::string path = made.is_array() ? made[0].get<std::string>() : made.get<std::string>();
    g().setValue({g().find(path)->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(m)));
    return path;
}

/** A live result's instances: each one's reference bounds through its transform, in the mesh's (world) space. */
std::vector<geo::Bounds> instanceBounds(const json& result)
{
    g().evaluate();
    const gn::GeometryPtr geo = gn::modifierGeometry(g().find(result["modifier"].get<std::string>())->id());
    REQUIRE(geo);
    REQUIRE(geo->instances);
    std::vector<geo::Bounds> out;
    const gn::Instances& in = *geo->instances;
    for (size_t k = 0; k < in.size(); ++k)
    {
        const gn::Geometry& ref = *in.references[size_t(in.reference[k])];
        geo::Bounds b;
        if (ref.mesh)
            for (geo::Index p = 0; p < ref.mesh->pointCapacity(); ++p)
                if (ref.mesh->pointValid(p))
                    b.extend(glm::dvec3(in.transforms[k] * glm::dvec4(ref.mesh->position(p), 1.0)));
        out.push_back(b);
    }
    return out;
}

std::vector<glm::dvec3> cablePoints(const std::string& curve)
{
    const json data = run("curveData", {{"args", {curve}}, {"worldSpace", true}});
    std::vector<glm::dvec3> out;
    for (const json& sp : data)
        for (const glm::dvec3& p : curve::tessellate(curve::splineFromJson(sp)).points)
            out.push_back(p);
    return out;
}
} // namespace

TEST_CASE("weathering generators: roof pieces overlap without gaps beyond the missing pieces")
{
    const geo::Mesh roofMesh = slope(400, 300, 170);
    weathering::roof::Input in;
    in.host = &roofMesh;
    geo::Bounds sheet;
    sheet.extend({-45, 0, -100});
    sheet.extend({45, 3, 100});
    in.cover = {sheet};
    weathering::roof::Options o;
    o.neatness = 0.0;
    o.missing = 0.0;
    o.lifted = 0.0;
    o.seed = 3;
    const weathering::roof::Result full = weathering::roof::layout(in, o);
    REQUIRE(full.regions.size() == 1);
    CHECK(full.rows >= 2);
    CHECK(full.placed.size() > 4);
    CHECK(uncovered(full.regions[0], full.placed, 1.0) == doctest::Approx(0.0));

    o.missing = 0.12;
    const weathering::roof::Result some = weathering::roof::layout(in, o);
    CHECK(some.missing > 0);
    const double rate = double(some.missing) / some.candidates;
    CHECK(uncovered(some.regions[0], some.placed, 1.0) <= rate + 0.03);

    // Same seed, same roof; another seed, another one.
    const weathering::roof::Result again = weathering::roof::layout(in, o);
    REQUIRE(again.placed.size() == some.placed.size());
    for (size_t k = 0; k < again.placed.size(); ++k)
        CHECK(again.placed[k].xf == some.placed[k].xf);
    o.seed = 4;
    const weathering::roof::Result other = weathering::roof::layout(in, o);
    bool differs = other.placed.size() != some.placed.size();
    for (size_t k = 0; !differs && k < other.placed.size(); ++k)
        differs = other.placed[k].xf != some.placed[k].xf;
    CHECK(differs);
}

TEST_CASE("weathering generators: neat roofs lay regular rows, ridges get caps")
{
    // A gable: two slopes meeting at a ridge along X at y 150.
    geo::Mesh gable;
    const auto a = gable.addPoint({0, 0, 300}), b = gable.addPoint({400, 0, 300}), c = gable.addPoint({400, 150, 150}), d = gable.addPoint({0, 150, 150});
    const auto e = gable.addPoint({400, 0, 0}), f = gable.addPoint({0, 0, 0});
    gable.addFace({a, b, c, d});
    gable.addFace({d, c, e, f});
    weathering::roof::Input in;
    in.host = &gable;
    geo::Bounds tile;
    tile.extend({-15, 0, -15});
    tile.extend({15, 4, 15});
    geo::Bounds tileB;
    tileB.extend({-15, 0, -17});
    tileB.extend({15, 5, 17});
    in.cover = {tile, tileB};
    geo::Bounds cap;
    cap.extend({-15, 0, -50});
    cap.extend({15, 8, 50});
    in.caps = {cap};
    weathering::roof::Options o;
    o.neatness = 1.0;
    o.damage = 0.0;
    o.stagger = 0.5;
    const weathering::roof::Result r = weathering::roof::layout(in, o);
    REQUIRE(r.regions.size() == 2);
    CHECK(r.caps >= 4);
    for (const auto& p : r.placed)
        if (p.slot == 1)
            CHECK(glm::dvec3(p.xf[3]).y > 150.0);
    // Within a row: every footprint starts at the same height, and the steps along the row are equal.
    std::map<std::pair<int, int>, std::vector<const weathering::roof::Placed*>> rows;
    for (const auto& p : r.placed)
        if (p.slot == 0)
            rows[{p.region, p.row}].push_back(&p);
    REQUIRE(rows.size() >= 4);
    for (const auto& [key, list] : rows)
    {
        REQUIRE(!list.empty());
        for (const auto* p : list)
            CHECK(p->footprint[0].y == doctest::Approx(list.front()->footprint[0].y));
        for (size_t k = 2; k < list.size(); ++k)
        {
            const double s1 = list[k]->footprint[0].x - list[k - 1]->footprint[0].x;
            const double s0 = list[k - 1]->footprint[0].x - list[k - 2]->footprint[0].x;
            CHECK(s1 == doctest::Approx(s0).epsilon(1e-6));
        }
    }
}

TEST_CASE("weathering generators: windows fit their opening")
{
    const geo::Mesh wall = wallWithHole({140, 100, -10}, {240, 220, 40});
    const std::vector<weathering::window::Opening> found = weathering::window::findOpenings(wall, {200, 150, -500}, {});
    REQUIRE(found.size() == 1);
    const auto& op = found[0];
    CHECK_FALSE(op.door);
    CHECK(op.rectangular);
    CHECK(op.u1 - op.u0 == doctest::Approx(100.0).epsilon(0.001));
    CHECK(op.v1 - op.v0 == doctest::Approx(120.0).epsilon(0.001));
    CHECK(op.v0 == doctest::Approx(100.0).epsilon(0.001));
    CHECK(op.depth == doctest::Approx(30.0));
    // Outside is away from the given centre: -Z here.
    CHECK(op.N.z == doctest::Approx(1.0));

    weathering::window::Pieces pieces;
    pieces.slot[weathering::window::Frame] = {wg::boundsOf(wg::placeholderMesh(wg::role::kFrame, false))};
    pieces.slot[weathering::window::Sill] = {wg::boundsOf(wg::placeholderMesh(wg::role::kSill, false))};
    pieces.slot[weathering::window::Pane] = {wg::boundsOf(wg::placeholderMesh(wg::role::kGlass, false))};
    weathering::window::Options o;
    o.columns = 2;
    const weathering::window::Result r = weathering::window::layout(found, pieces, o);
    REQUIRE(r.openings.size() == 1);
    CHECK(r.panes == 2);
    int frames = 0;
    for (const auto& p : r.placed)
    {
        if (p.slot != weathering::window::Frame && p.slot != weathering::window::Pane && p.slot != weathering::window::Sill)
            continue;
        const geo::Bounds pb = pieces.slot[p.slot][0];
        for (int k = 0; k < 8; ++k)
        {
            const glm::dvec3 c(k & 1 ? pb.max.x : pb.min.x, k & 2 ? pb.max.y : pb.min.y, k & 4 ? pb.max.z : pb.min.z);
            const glm::dvec3 w(p.xf * glm::dvec4(c, 1.0));
            const double u = glm::dot(w - op.base, op.U), v = glm::dot(w - op.base, op.V);
            CHECK(u >= op.u0 - 0.01);
            CHECK(u <= op.u1 + 0.01);
            CHECK(v >= op.v0 - 0.01);
            CHECK(v <= op.v1 + 0.01);
        }
        frames += p.slot == weathering::window::Frame;
    }
    CHECK(frames == 4 + 1);

    // A hole reaching the ground is a door, left alone unless asked.
    const geo::Mesh withDoor = wallWithHole({140, -10, -10}, {240, 220, 40});
    const auto doors = weathering::window::findOpenings(withDoor, {200, 150, -500}, {});
    REQUIRE(doors.size() == 1);
    CHECK(doors[0].door);
    CHECK(weathering::window::layout(doors, pieces, {}).openings.empty());
}

TEST_CASE("weathering generators: curve to truss is closed and connected")
{
    weathering::truss::Options o;
    o.width = 80;
    o.endWidth = 30;
    o.member = 4;
    o.segment = 60;
    const std::vector<std::vector<glm::dvec3>> lines = {{{0, 0, 0}, {0, 400, 0}}};
    const weathering::truss::Result r = weathering::truss::build(lines, o);
    CHECK(r.warning.empty());
    CHECK(r.members > 20);
    CHECK(wg::closedAndConnected(r.mesh));
    const geo::Bounds b = wg::boundsOf(r.mesh);
    CHECK(b.size().y == doctest::Approx(404.0).epsilon(0.01));

    o.chords = 3;
    o.bracing = weathering::truss::Bracing::Zigzag;
    const std::vector<std::vector<glm::dvec3>> bent = {{{0, 0, 0}, {200, 50, 0}, {400, 0, 100}}};
    CHECK(wg::closedAndConnected(weathering::truss::build(bent, o).mesh));
}

TEST_CASE("weathering generators: wire anchors read poles, brackets and buildings")
{
    geo::Bounds pole, shop, bracket;
    pole.extend({-10, 0, -10});
    pole.extend({10, 800, 10});
    shop.extend({1000, 0, -300});
    shop.extend({1600, 600, 300});
    bracket.extend({2000, 400, 0});
    bracket.extend({2020, 420, 15});
    const geo::Bounds hosts[] = {pole, shop, bracket};
    weathering::wires::Options o;
    o.insulator = 15;
    const auto anchors = weathering::wires::anchorsOn(hosts, o);
    REQUIRE(anchors.size() == 4);
    CHECK(anchors[0].kind == weathering::wires::Kind::Pole);
    CHECK(anchors[0].at.y == doctest::Approx(815));
    CHECK(anchors[1].kind == weathering::wires::Kind::Corner);
    CHECK(anchors[1].at.y == doctest::Approx(450));
    CHECK(anchors[3].kind == weathering::wires::Kind::Bracket);
    const auto runs = weathering::wires::chain(anchors, 3000);
    REQUIRE(runs.size() == 1);
    CHECK(runs[0].size() == 4);
    CHECK(weathering::wires::chain(anchors, 100).empty());
}

TEST_CASE("weathering generators: placeholders exist for every role in both styles")
{
    for (const std::string& r : wg::generatorRoles())
        for (bool toon : {false, true})
        {
            const geo::Mesh m = wg::placeholderMesh(r, toon);
            INFO(r << (toon ? " toon" : ""));
            CHECK(m.numFaces() > 0);
            CHECK(m.validate().empty());
        }
}

TEST_CASE("weathering generators: roofing uses the user's pieces, is deterministic and follows its host")
{
    fresh();
    const std::string shed = cube("Shed", {0, 0, 0}, {400, 250, 300});
    const std::string tile = cube("MyTile", {900, 0, 0}, {30, 4, 30});
    json r = run("roofing", {{"args", {shed}}, {"pieces", json::array({tile})}, {"ridge", false}, {"seed", 2}});
    CHECK(r["used"] == json::array({tile}));
    CHECK(r["regions"] == 1);
    CHECK(r["count"].get<int64_t>() > 50);
    CHECK(r["group"].is_string());
    const geo::Mesh first = meshOf(r["mesh"].get<std::string>());
    CHECK(first.numPoints() == geo::Index(8 * r["count"].get<int64_t>()));
    for (const geo::Bounds& b : instanceBounds(r))
        CHECK(b.min.y > 249.0);

    json again = run("roofing", {{"args", {shed}}, {"pieces", json::array({tile})}, {"ridge", false}, {"seed", 2}});
    const geo::Mesh second = meshOf(again["mesh"].get<std::string>());
    REQUIRE(second.numPoints() == first.numPoints());
    bool same = true;
    for (geo::Index p = 0; p < first.pointCapacity(); ++p)
        same = same && glm::length(first.position(p) - second.position(p)) < 1e-9;
    CHECK(same);

    // Making the shed twice as long lays twice the pieces.
    run("setAttr", {{"args", {shed + ".scaleX", 2.0}}});
    const geo::Mesh grown = meshOf(r["mesh"].get<std::string>());
    CHECK(grown.numPoints() > first.numPoints() * 3 / 2);

    // Placeholders when no pieces are given; toon roofs are neat.
    json toon = run("roofing", {{"args", {shed}}, {"style", "toon"}, {"kind", "tiles"}});
    CHECK(toon["count"].get<int64_t>() > 0);
    CHECK(toon["style"] == "toon");

    // realize: scene objects in the result's group, the live mesh gone.
    const json real = run("roofing", {{"args", {shed}}, {"pieces", json::array({tile})}, {"ridge", false}, {"realize", true}, {"name", "ShedReal"}});
    CHECK(real["objects"].size() == size_t(real["count"].get<int64_t>()));
    CHECK(g().find(real["group"].get<std::string>()));
    CHECK(real["group"].get<std::string>().find("ShedReal") != std::string::npos);
}

TEST_CASE("weathering generators: fitWindows fills a carved wall")
{
    fresh();
    const std::string wall = meshFrom("Facade", wallWithHole({140, 100, -10}, {240, 220, 40}));
    const json r = run("fitWindows", {{"args", {wall}}, {"glass", "mixed"}, {"damage", 1.0}, {"seed", 5}});
    CHECK(r["windows"] == 1);
    CHECK(r["count"].get<int64_t>() >= 5);
    CHECK(r["openings"][0]["width"].get<double>() == doctest::Approx(100.0).epsilon(0.001));
    const json boarded = run("fitWindows", {{"args", {wall}}, {"glass", "boarded"}, {"interior", "none"}, {"style", "toon"}});
    CHECK(boarded["boarded"] == 1);
    CHECK_THROWS(run("fitWindows", {{"args", {cube("Solid", {900, 0, 0}, {100, 100, 100})}}}));

    // A card interior with the user's image: its material slot is the Unreal instance windowInteriorSend makes.
    const json card = run("fitWindows", {{"args", {wall}}, {"interior", "card"}, {"interiorImage", "D:/refs/shop_room.png"}});
    CHECK(card["cardMaterial"] == "MI_shop_roomInterior");
    const json plan = run("windowInteriorSend", {{"args", {card["mesh"]}}, {"dryRun", true}});
    REQUIRE(plan["request"]["interiors"].size() == 1);
    CHECK(plan["request"]["interiors"][0]["name"] == "MI_shop_roomInterior");
    CHECK(plan["request"]["interiors"][0]["textures"].contains("InteriorRoomMap"));
}

TEST_CASE("weathering generators: wires sag within limits and stay out of colliders")
{
    fresh();
    const std::string p1 = cube("PoleA", {0, 0, 0}, {20, 800, 20});
    const std::string p2 = cube("PoleB", {1500, 0, 0}, {20, 800, 20});
    json r = run("wires", {{"args", {p1, p2}}, {"sag", 60.0}, {"name", "Line"}});
    CHECK(r["spans"] == 1);
    CHECK(r["fittings"] == 2);
    REQUIRE(!r["cables"].empty());
    std::vector<glm::dvec3> pts = cablePoints(r["cables"][0]["curve"].get<std::string>());
    REQUIRE(pts.size() > 4);
    double lowest = 1e300, top = -1e300;
    for (const glm::dvec3& p : pts)
    {
        lowest = std::min(lowest, p.y);
        top = std::max(top, p.y);
    }
    CHECK(top - lowest == doctest::Approx(60.0).epsilon(0.1));

    // A shed in the way: the wire drapes over it instead of passing through.
    const std::string shed = cube("Shed", {600, 0, 0}, {300, 780, 300});
    r = run("wires", {{"args", {p1, p2}}, {"colliders", json::array({shed})}, {"sag", 60.0}, {"name", "Over"}});
    CHECK(r["colliders"] == 1);
    // Where a wire wraps over an edge its centreline may shave the corner, never by more than its diameter (1.6 cm).
    double deepest = 0.0;
    for (const json& c : r["cables"])
        for (const glm::dvec3& p : cablePoints(c["curve"].get<std::string>()))
            if (p.x > 450.0 && p.x < 750.0 && p.z > -150.0 && p.z < 150.0 && p.y < 780.0)
                deepest = std::max(deepest, std::min({p.x - 450.0, 750.0 - p.x, 780.0 - p.y, p.z + 150.0, 150.0 - p.z}));
    CHECK(deepest < 1.6);
}

TEST_CASE("weathering generators: rooftop clutter only on flat tops, piles from placeholders")
{
    fresh();
    const std::string tall = cube("Tower", {0, 0, 0}, {800, 1200, 800});
    const std::string low = cube("Annex", {1100, 0, 0}, {600, 500, 800});
    const json r = run("rooftopClutter", {{"args", {tall, low}}, {"density", 1.0}, {"towerChance", 1.0}, {"seed", 3}});
    CHECK(r["tops"] == 2);
    CHECK(r["towers"] == 2);
    CHECK(r["count"].get<int64_t>() > 4);
    for (const geo::Bounds& b : instanceBounds(r))
    {
        const glm::dvec3 c = b.center();
        const bool onTall = std::abs(b.min.y - 1200.0) < 2.0 && c.x > -400 && c.x < 400 && c.z > -400 && c.z < 400;
        const bool onLow = std::abs(b.min.y - 500.0) < 2.0 && c.x > 800 && c.x < 1400 && c.z > -400 && c.z < 400;
        CHECK((onTall || onLow));
    }

    const json pile = run("themedPile", {{"theme", "bricks"}, {"settle", false}, {"count", 12}, {"at", json::array({0, 0, 2000})}});
    CHECK(pile["count"] == 12);
    CHECK(pile["group"].is_string());
}

TEST_CASE("weathering generators: curveToTruss builds a live closed truss on a curve")
{
    fresh();
    const json c = run("curve", {{"points", json::array({json::array({0, 0, 0}), json::array({0, 300, 0})})}, {"degree", 1}, {"name", "Mast"}});
    const std::string curve = c.is_array() ? c[0].get<std::string>() : c.get<std::string>();
    const json r = run("curveToTruss", {{"args", {curve}}, {"chords", 3}, {"width", 60}, {"segment", 60}});
    const geo::Mesh m = meshOf(r["mesh"].get<std::string>());
    CHECK(wg::closedAndConnected(m));
    CHECK(wg::boundsOf(m).size().y > 290.0);
}

TEST_CASE("weathering generators: menu entries under Level > Weathering with option boxes")
{
    fresh();
    std::vector<std::string> actionsIn;
    for (const ui::menus::Item* i : ui::menus::itemsIn("Level/Weathering"))
        actionsIn.push_back(i->action);
    for (const char* id : {"weathering.roofing", "weathering.windows", "weathering.wires", "weathering.pile", "weathering.rooftop", "weathering.truss"})
    {
        INFO(id);
        const ui::Action* a = ui::actions::find(id);
        REQUIRE(a);
        CHECK(!a->command.empty());
        CHECK(!a->keywords.empty());
        CHECK(ops::options::findOverride(a->command));
        CHECK(std::ranges::find(actionsIn, std::string(id)) != actionsIn.end());
    }
}
