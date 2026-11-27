// Interiors on their own: every program on rectangles, L-shapes and courtyards gives rooms the entry reaches; the facade's
// windows open into rooms with no partition across them; stairs keep to the character profile and the check flags them
// when it tightens; layout edits (kind, split, merge, move, doors) replay.

#include "Interior.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>

using namespace buildings;

namespace
{
const std::map<std::string, Style>& styles()
{
    static const std::map<std::string, Style> s = [] {
        std::map<std::string, Style> out;
        for (const auto& e : std::filesystem::directory_iterator(LILA_TEST_BUILDINGS_STYLES))
        {
            std::ifstream in(e.path());
            Style st = Style::fromJson(json::parse(in));
            out[st.id] = st;
        }
        return out;
    }();
    return s;
}

Spec specOf(Ring fp, std::vector<int> sides, int floors, Vec2 entrance, const std::string& program, uint64_t key, std::vector<Ring> holes = {})
{
    Spec s;
    s.key = key;
    s.id = "building:" + hex64(key);
    s.footprint = fp;
    s.sides = sides;
    s.holes = holes;
    s.floors = floors;
    s.front = {0.0, -1.0};
    s.ridge = {1.0, 0.0};
    s.entrances = {entrance};
    s.program = program;
    if (!program.empty() && program != "residential" && program != "house" && program != "apartments")
        s.businesses = {{"Abarrotes Lupita", program, entrance, true}};
    return s;
}

Spec rectSpec(double w, double d, int floors, const std::string& program, uint64_t key)
{
    return specOf({{0, 0}, {w, 0}, {w, d}, {0, d}}, {0, 2, 2, 2}, floors, {w * 0.5, 0.0}, program, key);
}

Spec lSpec(int floors, const std::string& program, uint64_t key)
{
    return specOf({{0, 0}, {1800, 0}, {1800, 900}, {900, 900}, {900, 1600}, {0, 1600}}, {0, 2, 2, 2, 2, 2}, floors, {900.0, 0.0}, program, key);
}

Spec courtyardSpec(int floors, const std::string& program, uint64_t key)
{
    return specOf({{0, 0}, {2600, 0}, {2600, 2600}, {0, 2600}}, {0, 2, 2, 2}, floors, {1300.0, 0.0}, program, key,
                  {{{900, 900}, {1700, 900}, {1700, 1700}, {900, 1700}}});
}

/** A style that goes with a program. */
std::string styleFor(const std::string& program)
{
    if (program == "church")
        return "church";
    if (program == "police")
        return "policeStation";
    if (program == "mansion")
        return "mansion";
    if (program == "warehouse")
        return "warehouse";
    if (program == "hotel")
        return "resortTower";
    if (program == "office")
        return "modernCommercial";
    if (program == "shop" || program == "bar" || program == "garage")
        return "colonial";
    return "residential";
}

std::string specProgram(const std::string& program)
{
    if (program == "house" || program == "apartment" || program == "generic")
        return "residential";
    if (program == "shop")
        return "shop";
    return program;
}

std::shared_ptr<const Result> build(const Spec& s, const Style& st, const json& interior = json::object())
{
    Edits e;
    e.enterable = true;
    e.interior = interior;
    return generate(s, st, e);
}

/** First hit of a ray on a soup's polygons (fan triangles), or a huge number. */
double raySoup(const Soup& s, Vec3 o, Vec3 d)
{
    double best = 1e300;
    for (size_t f = 0, off = 0; f < s.sizes.size(); off += s.sizes[f], ++f)
        for (uint32_t k = 1; k + 1 < s.sizes[f]; ++k)
        {
            const Vec3 a = s.pts[off], b = s.pts[off + k], c = s.pts[off + k + 1];
            const Vec3 e1 = b - a, e2 = c - a, p = glm::cross(d, e2);
            const double det = glm::dot(e1, p);
            if (std::abs(det) < 1e-9)
                continue;
            const Vec3 t = o - a;
            const double u = glm::dot(t, p) / det;
            if (u < 0.0 || u > 1.0)
                continue;
            const Vec3 q = glm::cross(t, e1);
            const double v = glm::dot(d, q) / det;
            if (v < 0.0 || u + v > 1.0)
                continue;
            const double tt = glm::dot(e2, q) / det;
            if (tt > 1e-6 && tt < best)
                best = tt;
        }
    return best;
}

double rayInterior(const Interior& in, Vec3 o, Vec3 d)
{
    double best = 1e300;
    for (const InteriorPart& p : in.parts)
        best = std::min(best, raySoup(p.soup, o, d));
    return best;
}
} // namespace

TEST_CASE("interiors: every program gives rooms the entry reaches, on rectangles, L-shapes and courtyards")
{
    const CharacterProfile prof;
    uint64_t key = 0x5000;
    for (const std::string& program : interiorPrograms())
    {
        const Style& st = styles().at(styleFor(program));
        for (int shape = 0; shape < 3; ++shape)
            for (int floors : {1, 2, 4})
            {
                if (program == "church" && floors > 2)
                    continue;
                ++key;
                Spec s = shape == 0 ? rectSpec(1400, 1000, floors, specProgram(program), key)
                                    : shape == 1 ? lSpec(floors, specProgram(program), key) : courtyardSpec(floors, specProgram(program), key);
                s.style = st.id;
                CAPTURE(program);
                CAPTURE(shape);
                CAPTURE(floors);
                auto r = build(s, st, {{"program", program}});
                REQUIRE(r->enterable);
                auto in = generateInterior(s, *r, st, {{"program", program}}, prof);
                CAPTURE(in->warnings);
                CHECK(in->program == program);
                CHECK(!in->rooms.empty());
                CHECK(!in->entries.empty());
                int unreachable = 0;
                for (const Room& rm : in->rooms)
                {
                    unreachable += rm.reachable ? 0 : 1;
                    CHECK(rm.area > 1.0);
                    CHECK(!rm.rects.empty());
                }
                CHECK(unreachable == 0);
                if (unreachable)
                    for (const InteriorDoor& d : in->doors)
                        MESSAGE(d.id << " " << d.a << " " << d.b << " w " << d.width);
                const bool volume = program == "church" || (program == "warehouse" && floors <= 2);
                if (r->floors > 1 && !volume)
                    CHECK(in->stairs.size() == size_t(r->floors - 1));
                const json c = checkInterior(*in, prof);
                CAPTURE(c.dump());
                CHECK(c["ok"].get<bool>());
                // Every part has geometry and every door a slot.
                for (const InteriorPart& p : in->parts)
                    CHECK(p.soup.faces() > 0);
                size_t doorSlots = 0, leaves = 0;
                for (const Module& m : in->slots)
                    doorSlots += m.type.rfind("door.interior", 0) == 0 ? 1 : 0;
                for (const InteriorDoor& d : in->doors)
                    leaves += d.open ? 0 : 1;
                CHECK(doorSlots == leaves);
            }
    }
}

TEST_CASE("interiors: the program comes from the building, with a generic fallback that says so")
{
    const Style& res = styles().at("residential");
    std::string from;
    CHECK(interiorProgramFor(rectSpec(1000, 800, 2, "residential", 1), res, "", &from) == "house");
    CHECK(interiorProgramFor(rectSpec(1000, 800, 5, "residential", 1), res, "", &from) == "apartment");
    CHECK(interiorProgramFor(rectSpec(1000, 800, 1, "bar", 1), res, "", &from) == "bar");
    CHECK(interiorProgramFor(rectSpec(1000, 800, 1, "shop", 1), res, "", &from) == "shop");
    CHECK(interiorProgramFor(rectSpec(1000, 800, 1, "", 1), styles().at("policeStation"), "", &from) == "police");
    CHECK(interiorProgramFor(rectSpec(1000, 800, 1, "", 1), styles().at("church"), "", &from) == "church");
    CHECK(interiorProgramFor(rectSpec(1000, 800, 1, "", 1), styles().at("mansion"), "", &from) == "mansion");
    Spec odd = rectSpec(1000, 800, 2, "spaceport", 7);
    odd.businesses.clear();
    CHECK(interiorProgramFor(odd, res, "", &from) == "generic");
    CHECK(from == "fallback");
    auto r = build(odd, res);
    auto in = generateInterior(odd, *r, res, json::object(), CharacterProfile{});
    CHECK(in->generic);
    REQUIRE(!in->warnings.empty());
    CHECK(in->warnings.front().find("generic") != std::string::npos);
    CHECK(fakeRoomKind("house", 0) == "living");
    CHECK(fakeRoomKind("house", 2) == "bedroom");
}

TEST_CASE("interiors: the facade's windows open into rooms, no partition across them")
{
    for (const std::string& program : {std::string("house"), std::string("apartment"), std::string("office"), std::string("shop")})
    {
        const Style& st = styles().at(styleFor(program));
        Spec s = program == "apartment" ? courtyardSpec(4, "residential", 0x77) : lSpec(3, specProgram(program), 0x78);
        s.style = st.id;
        auto r = build(s, st, {{"program", program}});
        auto in = generateInterior(s, *r, st, {{"program", program}}, CharacterProfile{});
        CAPTURE(program);
        int windows = 0, open = 0, oneRoom = 0;
        for (const Opening& o : r->openings)
        {
            if (o.kind != "window" || o.bay.find("/l") == std::string::npos)
                continue;
            ++windows;
            const Vec3 n = glm::normalize(o.normal);
            const Vec3 centre = o.origin + Vec3(0.0, o.height * 0.5, 0.0);
            // A ray from outside through the middle of the window meets nothing of the interior within the wall.
            const double hit = rayInterior(*in, centre + n * 50.0, -n);
            // A window may light the stair core, with the flight behind it.
            const bool stairs = in->roomAt(centre - n * (st.wallThickness + 30.0)) == -2;
            if (hit > 50.0 + st.wallThickness + 20.0 || stairs)
                ++open;
            else
                MESSAGE(o.bay << " hit " << hit << " at " << centre.x << "," << centre.y << "," << centre.z);
            // Both sides of the window look into the same room.
            const Vec3 along = glm::normalize(glm::cross(Vec3(0, 1, 0), n));
            const Vec3 inside = centre - n * (st.wallThickness + 30.0);
            const int a = in->roomAt(inside + along * (o.width * 0.35)), b = in->roomAt(inside - along * (o.width * 0.35));
            if (a != -1 && a == b)
                ++oneRoom;
        }
        CHECK(windows > 0);
        CHECK(open == windows);
        CHECK(oneRoom >= windows * 9 / 10);
    }
}

TEST_CASE("interiors: stairs keep to the character profile, and the check flags a stricter one")
{
    const Style& st = styles().at("residential");
    Spec s = rectSpec(1200, 1000, 3, "residential", 0x91);
    auto r = build(s, st);
    CharacterProfile prof;
    auto in = generateInterior(s, *r, st, json::object(), prof);
    REQUIRE(in->stairs.size() == 2);
    for (const Stair& k : in->stairs)
    {
        CHECK(k.riser <= 18.0 + 1e-6);
        CHECK(k.slope <= prof.maxSlope);
        CHECK(k.headroom >= prof.height);
        CHECK(k.width >= 2.0 * prof.radius);
    }
    CHECK(checkInterior(*in, prof)["ok"].get<bool>());
    CharacterProfile strict;
    strict.maxStep = 12.0;
    const json c = checkInterior(*in, strict);
    CHECK(c["counts"]["stairSteep"].get<int>() == 2);
    CharacterProfile wide;
    wide.radius = 80.0;
    CHECK(checkInterior(*in, wide)["counts"]["doorNarrow"].get<int>() > 0);
    CharacterProfile tall;
    tall.height = 240.0;
    CHECK(checkInterior(*in, tall)["counts"]["doorLow"].get<int>() > 0);
    // Built for a gentler slope, the stair is gentler.
    CharacterProfile gentle;
    gentle.maxSlope = 28.0;
    Spec big = rectSpec(1600, 1200, 3, "residential", 0x92);
    auto r2 = build(big, st);
    auto in2 = generateInterior(big, *r2, st, json::object(), gentle);
    REQUIRE(in2->stairs.size() == 2);
    for (const Stair& k : in2->stairs)
        CHECK(k.slope <= 28.0 + 1e-6);
}

TEST_CASE("interiors: tight floors get a dog-leg or a straight flight, and every floor is reachable")
{
    const CharacterProfile prof;
    struct Case
    {
        double w, d;
        int floors;
        const char* kind;
    };
    // A wide house keeps the U; a small square one fits a dog-leg; a slot two metres wide fits only a straight flight.
    for (const Case& c : {Case{1200, 1000, 3, "u"}, Case{440, 440, 2, "dogleg"}, Case{440, 440, 3, "dogleg"}, Case{200, 1200, 2, "straight"},
                          Case{200, 1200, 4, "straight"}})
    {
        CAPTURE(c.w);
        CAPTURE(c.d);
        CAPTURE(c.floors);
        const Style& st = styles().at("residential");
        Spec s = rectSpec(c.w, c.d, c.floors, "residential", 0x700 + uint64_t(c.w) + uint64_t(c.floors));
        auto r = build(s, st);
        auto in = generateInterior(s, *r, st, json::object(), prof);
        CAPTURE(in->warnings);
        CAPTURE(in->notes);
        REQUIRE(in->stairs.size() == size_t(r->floors - 1));
        for (const Stair& k : in->stairs)
        {
            CHECK(k.kind == c.kind);
            CHECK(k.straight == (std::string(c.kind) == "straight"));
            CHECK(k.riser <= 18.0 + 1e-6);
            CHECK(k.slope <= prof.maxSlope);
            CHECK(k.headroom >= prof.height);
            CHECK(k.width >= 2.0 * prof.radius);
        }
        for (const Room& rm : in->rooms)
        {
            CAPTURE(rm.id);
            CHECK(rm.reachable);
            if (!rm.reachable)
                for (const InteriorDoor& d : in->doors)
                    MESSAGE(d.id << " " << d.a << " " << d.b << " w " << d.width << " at " << d.origin.x << "," << d.origin.z);
        }
        std::set<int> floorsWithRooms;
        for (const Room& rm : in->rooms)
            floorsWithRooms.insert(rm.floor);
        CHECK(floorsWithRooms.size() == size_t(r->floors));
        const json chk = checkInterior(*in, prof);
        CAPTURE(chk.dump());
        CHECK(chk["ok"].get<bool>());
        int open = 0, up = 0;
        for (const InteriorDoor& d : in->doors)
        {
            open += d.open ? 1 : 0;
            up += d.up ? 1 : 0;
            if (d.open)
                CHECK(d.width >= 2.0 * prof.radius);
        }
        if (std::string(c.kind) == "u")
            CHECK(open == 0);
        else
            CHECK(open >= r->floors);
        // A straight flight on a middle floor starts at a second opening, where the one below did not arrive.
        CHECK(up == (std::string(c.kind) == "straight" ? r->floors - 2 : 0));
        for (const InteriorPart& p : in->parts)
            CHECK(p.soup.faces() > 0);
    }
}

TEST_CASE("interiors: a stepped ground floor on a slope: rooms at their doors' level, steps between levels")
{
    const CharacterProfile prof;
    for (const char* program : {"shop", "residential"})
        for (int floors : {1, 2})
        {
            CAPTURE(program);
            CAPTURE(floors);
            const Style& st = styles().at(std::string(program) == "shop" ? "colonial" : "residential");
            Spec s = rectSpec(2400, 1200, floors, program, 0x880 + uint64_t(floors) + (std::string(program) == "shop" ? 10 : 0));
            s.style = st.id;
            s.cornerGround = {0.0, 260.0, 260.0, 0.0}; // rising along the street
            s.ground = 0.0;
            s.highGround = 260.0;
            auto r = build(s, st);
            auto in = generateInterior(s, *r, st, json::object(), prof);
            CAPTURE(in->warnings);
            const double floor0 = r->plates.front().y;
            std::set<int> levels;
            for (const Room& rm : in->rooms)
            {
                CAPTURE(rm.id);
                CHECK(rm.reachable);
                if (rm.floor != 0)
                    continue;
                // Lower rooms keep the ceiling where it was: the floor above doesn't move.
                CHECK(rm.y <= floor0 + 0.01);
                CHECK(rm.y >= 5.0 - 0.01);
                CHECK(rm.y + rm.height == doctest::Approx(floor0 + r->plates.front().height));
                for (double y : rm.rectY)
                {
                    CHECK(y >= rm.y);
                    CHECK(y <= floor0 + 0.01);
                    levels.insert(int(std::lround(y)));
                }
            }
            CHECK(levels.size() >= 2);
            // Every street door opens level with the room behind it, as the facade stepped it.
            int entries = 0;
            for (const Opening& op : r->openings)
            {
                if (op.kind == "window" || op.bay.find("/l0/") == std::string::npos)
                    continue;
                const Vec3 inPt = op.origin - op.normal * 60.0;
                const std::optional<double> floorY = in->floorAt({inPt.x, op.origin.y + 1.0, inPt.z});
                if (!floorY || in->roomAt({inPt.x, op.origin.y + 50.0, inPt.z}) < 0)
                    continue;
                CAPTURE(op.bay);
                CHECK(*floorY == doctest::Approx(op.origin.y));
                ++entries;
            }
            CHECK(entries > 0);
            // A door between floors at different levels has steps, each riser within the character's step.
            int stepped = 0;
            for (const InteriorDoor& d : in->doors)
            {
                if (d.floor != 0)
                    continue;
                const Vec3 at{d.origin.x, floor0 - 1.0, d.origin.z};
                const std::optional<double> fa = in->floorAt(at - d.normal * 10.0), fb = in->floorAt(at + d.normal * 10.0);
                REQUIRE(fa);
                REQUIRE(fb);
                CAPTURE(d.id);
                CHECK(d.origin.y == doctest::Approx(std::max(*fa, *fb)));
                const auto st2 = std::find_if(in->stairs.begin(), in->stairs.end(), [&](const Stair& k) { return k.id == d.id + "/steps"; });
                if (std::abs(*fa - *fb) < 0.5)
                {
                    CHECK(st2 == in->stairs.end());
                    continue;
                }
                ++stepped;
                REQUIRE(st2 != in->stairs.end());
                CHECK(st2->kind == "steps");
                CHECK(st2->riser <= 18.0 + 1e-6);
                CHECK(st2->risers * st2->riser == doctest::Approx(std::abs(*fa - *fb)));
            }
            MESSAGE("terraces " << levels.size() << ", stepped doors " << stepped);
            const json chk = checkInterior(*in, prof);
            CAPTURE(chk.dump());
            CHECK(chk["ok"].get<bool>());
        }
}

TEST_CASE("interiors: layout edits replay by room id")
{
    const Style& st = styles().at("residential");
    Spec s = rectSpec(1400, 1000, 1, "residential", 0xa1);
    auto r = build(s, st);
    auto base = generateInterior(s, *r, st, json::object(), CharacterProfile{});
    REQUIRE(base->room("l0/living"));
    REQUIRE(base->room("l0/kitchen"));
    REQUIRE(base->room("l0/bedroom"));
    const double livingArea = base->room("l0/living")->area;

    auto kind = generateInterior(s, *r, st, {{"kinds", {{"l0/kitchen", "study"}}}}, CharacterProfile{});
    CHECK(kind->room("l0/kitchen")->kind == "study");
    CHECK(kind->room("l0/kitchen")->user);

    auto split = generateInterior(s, *r, st, {{"splits", {{"l0/living", {{"axis", "auto"}, {"at", 0.5}, {"kind", "office"}}}}}}, CharacterProfile{});
    REQUIRE(split->room("l0/living-split"));
    CHECK(split->room("l0/living-split")->kind == "office");
    CHECK(split->room("l0/living-split")->reachable);
    CHECK(split->room("l0/living")->area < livingArea);

    auto merged = generateInterior(s, *r, st, {{"merges", json::array({json::array({"l0/living", "l0/kitchen"})})}}, CharacterProfile{});
    CHECK(!merged->room("l0/kitchen"));
    CHECK(merged->room("l0/living")->area > livingArea);

    // Grow the living room into a neighbour it shares a wall with.
    std::string neighbour;
    for (const InteriorDoor& d : base->doors)
        if (d.a == "l0/living" && d.b.rfind("l0/", 0) == 0)
            neighbour = d.b;
        else if (d.b == "l0/living" && d.a.rfind("l0/", 0) == 0)
            neighbour = d.a;
    REQUIRE(!neighbour.empty());
    auto moved = generateInterior(s, *r, st, {{"moves", {{{"room", "l0/living"}, {"toward", neighbour}, {"by", 80.0}}}}}, CharacterProfile{});
    CHECK(moved->room("l0/living")->area > livingArea + 0.5);

    // Remove a door: its room can't be reached; add one back elsewhere.
    std::string door, cutOff;
    for (const InteriorDoor& d : base->doors)
        if (d.b != "l0/living" && d.a != "l0/living")
            continue;
        else
        {
            door = d.id;
            cutOff = d.a == "l0/living" ? d.b : d.a;
            break;
        }
    REQUIRE(!door.empty());
    auto removed = generateInterior(s, *r, st, {{"removeDoors", {door}}}, CharacterProfile{});
    CHECK(!removed->room(cutOff)->reachable);
    CHECK(checkInterior(*removed, CharacterProfile{})["counts"]["roomUnreachable"].get<int>() >= 1);
    auto added = generateInterior(s, *r, st, {{"removeDoors", {door}}, {"addDoors", {{{"a", "l0/living"}, {"b", cutOff}}}}}, CharacterProfile{});
    CHECK(added->room(cutOff)->reachable);
    bool user = false;
    for (const InteriorDoor& d : added->doors)
        user = user || d.user;
    CHECK(user);

    // An edit naming a room that isn't there is reported, not dropped silently.
    auto orphan = generateInterior(s, *r, st, {{"kinds", {{"l0/nowhere", "bar"}}}}, CharacterProfile{});
    CHECK(orphan->orphanEdits.size() == 1);

    // Same input, same ids.
    auto again = generateInterior(s, *r, st, json::object(), CharacterProfile{});
    REQUIRE(again->rooms.size() == base->rooms.size());
    for (size_t k = 0; k < base->rooms.size(); ++k)
        CHECK(again->rooms[k].id == base->rooms[k].id);
}

// Through the package's commands ----------------------------------------------------------------------------------------------

#include "extensions/ExtensionManager.h"
#include "gn/NodeLib.h"
#include "ops/Command.h"
#include "scene/Graph.h"

namespace
{
json run(const char* cmd, json call = json::object()) { return lila::ops::execute(cmd, call); }

void loadPackages(bool settlement)
{
    if (!lila::ops::find("buildingCreate"))
        REQUIRE(lila::extensions::ExtensionManager::get().load(LILA_TEST_BUILDINGS_PACKAGE) == "Buildings");
    if (settlement && !lila::gn::findDecl("Settlement"))
        REQUIRE(lila::extensions::ExtensionManager::get().load(LILA_TEST_SETTLEMENT_PACKAGE) == "Settlement");
    if (settlement && !lila::ops::find("terrainCreate"))
        REQUIRE(lila::extensions::ExtensionManager::get().load(LILA_TEST_TERRAIN_PACKAGE) == "Terrain");
}

json env(uint64_t lot, double x0, double z0, double w, double d, int floors, const std::string& style, const std::string& program)
{
    json e = {{"lot", int(lot)},
              {"lotId", lot},
              {"footprint", {{x0, z0}, {x0 + w, z0}, {x0 + w, z0 + d}, {x0, z0 + d}}},
              {"sides", {"street", "open", "open", "open"}},
              {"floors", floors},
              {"height", floors * 320.0},
              {"ground", 0.0},
              {"base", -50.0},
              {"top", floors * 320.0 + 40.0},
              {"roof", "flat"},
              {"front", {0.0, -1.0}},
              {"entrances", json::array({{x0 + w * 0.5, z0}})},
              {"style", style},
              {"program", program},
              {"use", program == "residential" ? "residential" : "commercial"}};
    if (program != "residential")
        e["businesses"] = json::array({{{"name", "Tortilleria La Huerta"}, {"program", program}, {"at", {x0 + w * 0.5, z0}}}});
    return e;
}

json town()
{
    return json::array({env(0x4a1, 0, 0, 1400, 1000, 2, "colonial", "shop"), env(0x4a2, 3000, 0, 1200, 1000, 3, "residential", "residential")});
}

std::string partMesh(const std::string& id, const std::string& part)
{
    const json got1 = run("buildingInterior", {{"args", {id}}});
    for (const json& p : got1["parts"])
        if (p["part"] == part)
            return p["mesh"];
    return {};
}

bool partEdited(const std::string& id, const std::string& part)
{
    const json got2 = run("buildingInterior", {{"args", {id}}});
    for (const json& p : got2["parts"])
        if (p["part"] == part)
            return p["edited"];
    return false;
}

std::string pathOf(const json& r) { return r.is_array() ? r[0].get<std::string>() : r.get<std::string>(); }
} // namespace

TEST_CASE("interiors commands: make enterable and back, entry tags, pinning, undo")
{
    loadPackages(false);
    run("newScene");
    run("buildingCreate", {{"envelopes", town()}, {"name", "Town"}});
    const std::string A = "building:4a1";
    const json made = run("buildingMakeEnterable", {{"args", {A}}});
    REQUIRE(made["count"] == 1);
    const json r = made["buildings"][0];
    CAPTURE(r.dump());
    CHECK(r["rooms"].get<int>() >= 3);
    CHECK(r["unreachable"] == 0);
    CHECK(!r["entries"].empty());
    CHECK(r["pinned"] == true);
    const json info = run("buildingInterior", {{"args", {A}}, {"check", true}});
    CHECK(!info["group"].get<std::string>().empty());
    CHECK(info["program"] == "shop");
    CHECK(info["enterable"] == true);
    CHECK(info["pinned"] == true);
    CHECK(info["parts"].size() >= info["rooms"].size());
    CHECK(info["check"]["ok"] == true);
    CHECK(run("slotFind", {{"type", "door.interior"}, {"limit", 0}})["count"].get<size_t>() == info["doors"].size());
    CHECK(run("slotFind", {{"tag", "entry"}, {"limit", 0}})["count"].get<int>() >= 1);
    CHECK(run("slotFind", {{"type", "furnishing"}, {"limit", 0}})["count"].get<int>() >= 3);
    CHECK(run("slotFind", {{"type", "lamp.ceiling"}, {"limit", 0}})["count"].get<size_t>() == info["rooms"].size());
    const json check = run("buildingCheck");
    CAPTURE(check["counts"].dump());
    CHECK(check["ok"] == true);
    CHECK(check["interiors"] == 1);

    // Undo takes it all back, redo brings it again.
    run("undo");
    CHECK(run("buildingInterior", {{"args", {A}}})["group"] == "");
    CHECK(run("buildingInterior", {{"args", {A}}})["enterable"] == false);
    CHECK(run("buildingInterior", {{"args", {A}}})["pinned"] == false);
    CHECK(run("slotFind", {{"type", "door.interior"}, {"limit", 0}})["count"] == 0);
    run("redo");
    CHECK(run("buildingInterior", {{"args", {A}}})["group"] != "");
    CHECK(run("buildingInterior", {{"args", {A}}})["pinned"] == true);

    // Back to an exterior: no group, not pinned, windows show fake interiors again; undo restores.
    run("buildingMakeExterior", {{"args", {A}}});
    CHECK(run("buildingInterior", {{"args", {A}}})["group"] == "");
    CHECK(run("buildingInterior", {{"args", {A}}})["enterable"] == false);
    const json win = run("slotFind", {{"type", "window"}, {"owner", A}, {"limit", 1}, {"full", true}});
    REQUIRE(win["count"].get<int>() > 0);
    CHECK(win["slots"][0]["payload"]["data"]["interior"] == "card");
    run("undo");
    CHECK(run("buildingInterior", {{"args", {A}}})["group"] != "");
}

TEST_CASE("interiors commands: hand-edited rooms and layout edits survive regeneration")
{
    loadPackages(false);
    run("newScene");
    run("buildingCreate", {{"envelopes", town()}, {"name", "Town"}});
    const std::string B = "building:4a2";
    run("buildingMakeEnterable", {{"args", {B}}});
    const json rooms = run("buildingInterior", {{"args", {B}}})["rooms"];
    std::string kitchen, edited;
    for (const json& rm : rooms)
    {
        if (rm["kind"] == "kitchen")
            kitchen = rm["id"];
        if (rm["floor"] == 1 && edited.empty())
            edited = rm["id"];
    }
    REQUIRE(!kitchen.empty());
    REQUIRE(!edited.empty());
    const std::string mesh = partMesh(B, edited);
    CAPTURE(run("buildingInterior", {{"args", {B}}})["parts"].dump());
    CAPTURE(edited);
    REQUIRE(!mesh.empty());
    // Ordinary modelling on the room's mesh.
    run("selectComponents", {{"args", {mesh}}, {"vertices", {0, 1}}});
    run("moveComponents", {{"by", {0.0, 30.0, 0.0}}});
    CHECK(partEdited(B, edited));

    const json rk = run("buildingRoom", {{"args", {B}}, {"room", kitchen}, {"kind", "study"}});
    CHECK(std::find(rk["keptEdited"].begin(), rk["keptEdited"].end(), json(edited)) != rk["keptEdited"].end());
    // The building changes: the interior regenerates, the edited room stays as modelled.
    const json more = run("buildingEdit", {{"args", {B}}, {"floors", 4}});
    REQUIRE(more.contains("interiors"));
    const json kept = more["interiors"][0]["keptEdited"];
    CHECK(std::find(kept.begin(), kept.end(), json(edited)) != kept.end());
    CHECK(partEdited(B, edited));
    CHECK(run("buildingInterior", {{"args", {B}}})["stairs"].size() == 3);
    // Running the generator over the same envelopes keeps both the room edit and the hand edit.
    run("buildingCreate", {{"envelopes", town()}, {"name", "Town"}});
    bool study = false;
    const json got3 = run("buildingInterior", {{"args", {B}}});
    for (const json& rm : got3["rooms"])
        study = study || (rm["id"] == kitchen && rm["kind"] == "study");
    CHECK(study);
    CHECK(partEdited(B, edited));
    // reset=True takes the generated room back.
    run("buildingMakeEnterable", {{"args", {B}}, {"reset", true}});
    CHECK_FALSE(partEdited(B, edited));
}

TEST_CASE("interiors commands: buildingCheck flags unreachable rooms, closed doors, steep stairs and narrow doors")
{
    loadPackages(false);
    run("newScene");
    run("buildingCreate", {{"envelopes", town()}, {"name", "Town"}});
    const std::string B = "building:4a2";
    run("buildingMakeEnterable", {{"args", {B}}});
    CHECK(run("buildingCheck")["ok"] == true);
    // A door taken away leaves its room unreachable; one added back fixes it.
    json info = run("buildingInterior", {{"args", {B}}});
    std::string door, a, b;
    for (const json& d : info["doors"])
        if (d["a"].get<std::string>().rfind("l0/", 0) == 0 && d["b"].get<std::string>().rfind("l0/", 0) == 0)
        {
            door = d["id"];
            a = d["a"];
            b = d["b"];
            break;
        }
    REQUIRE(!door.empty());
    run("buildingDoor", {{"args", {B}}, {"remove", door}});
    json check = run("buildingCheck");
    CHECK(check["counts"]["roomUnreachable"].get<int>() >= 1);
    run("buildingDoor", {{"args", {B}}, {"add", {a, b}}});
    check = run("buildingCheck");
    CAPTURE(check["issues"].dump());
    CHECK(check["counts"]["roomUnreachable"] == 0);

    // A wall modelled across a door closes it.
    info = run("buildingInterior", {{"args", {B}}});
    const json d = info["doors"][0];
    const glm::dvec3 o(d["origin"][0].get<double>(), d["origin"][1].get<double>(), d["origin"][2].get<double>());
    const glm::dvec3 n(d["normal"][0].get<double>(), d["normal"][1].get<double>(), d["normal"][2].get<double>());
    const glm::dvec3 al = glm::normalize(glm::cross(glm::dvec3(0, 1, 0), n)) * 80.0;
    const json slab = run("createMesh", {{"points", {{o.x - al.x, o.y, o.z - al.z}, {o.x + al.x, o.y, o.z + al.z}, {o.x + al.x, o.y + 250, o.z + al.z},
                                                     {o.x - al.x, o.y + 250, o.z - al.z}}},
                                          {"faces", {{0, 1, 2, 3}}},
                                          {"name", "Patch"}});
    const std::string patched = pathOf(run("parent", {{"args", {pathOf(slab), info["group"]}}}));
    run("addAttr", {{"args", {patched}}, {"longName", "interiorPart"}, {"dataType", "string"}});
    run("setAttr", {{"args", {patched + ".interiorPart", "patch"}}});
    check = run("buildingCheck");
    CHECK(check["counts"]["doorNoOpening"].get<int>() >= 1);
    run("delete", {{"args", {patched}}});

    // The check against a stricter character than the one the stairs were built for.
    check = run("buildingCheck", {{"maxStep", 12.0}, {"radius", 80.0}});
    CHECK(check["counts"]["stairSteep"].get<int>() >= 1);
    CHECK(check["counts"]["doorNarrow"].get<int>() >= 1);
    // The scene's profile changes: stairs are built to it.
    const json prof = run("buildingCharacterProfile", {{"maxStep", 15.0}});
    CHECK(prof["maxStep"] == 15.0);
    const json got4 = run("buildingInterior", {{"args", {B}}});
    for (const json& s : got4["stairs"])
        CHECK(s["riser"].get<double>() <= 15.0 + 1e-6);
    CHECK(run("buildingCheck")["counts"]["stairSteep"] == 0);
}

TEST_CASE("interiors commands: room edits through buildingRoom")
{
    loadPackages(false);
    run("newScene");
    run("buildingCreate", {{"envelopes", town()}, {"name", "Town"}});
    const std::string A = "building:4a1";
    run("buildingMakeEnterable", {{"args", {A}}});
    json r = run("buildingRoom", {{"args", {A}}, {"room", "l0/sales"}, {"split", "auto"}, {"newKind", "office"}});
    bool split = false;
    for (const json& rm : r["roomList"])
        split = split || (rm["id"] == "l0/sales-split" && rm["kind"] == "office" && rm["reachable"] == true);
    CHECK(split);
    r = run("buildingRoom", {{"args", {A}}, {"room", "l0/sales"}, {"merge", "l0/sales-split"}});
    bool gone = true;
    for (const json& rm : r["roomList"])
        gone = gone && rm["id"] != "l0/sales-split";
    CHECK(gone);
    r = run("buildingRoom", {{"args", {A}}, {"room", "l0/sales"}, {"swap", "l0/backroom"}});
    for (const json& rm : r["roomList"])
        if (rm["id"] == "l0/sales")
            CHECK(rm["kind"] == "backroom");
    r = run("buildingRoom", {{"args", {A}}, {"reset", true}});
    for (const json& rm : r["roomList"])
        if (rm["id"] == "l0/sales")
            CHECK(rm["kind"] == "sales");
    CHECK_THROWS(run("buildingRoom", {{"args", {A}}, {"room", "l0/nowhere"}, {"kind", "bar"}}));
    run("undo");
    CHECK(run("buildingInterior", {{"args", {A}}})["edits"].contains("kinds"));
}

TEST_CASE("interiors commands: three buildings of the OSM fixture town")
{
    loadPackages(true);
    run("newScene");
    const json terrain = run("terrainCreate", {{"size", json::array({80000.0, 80000.0})}, {"resolution", 65}, {"amplitude", 200.0}, {"seed", 4}});
    const std::string townFile = (std::filesystem::path(LILA_SOURCE_DIR) / "tests" / "data" / "osm" / "w1_town.osm").string();
    const json t = run("settlementCreate", {{"profile", "usTown"}, {"size", 60000.0}, {"ground", terrain["mesh"]}, {"osmFile", townFile},
                                            {"osmOrigin", {23.2, -106.42}}, {"name", "Town"}});
    run("settlementBuildings", {{"args", {t["mesh"]}}, {"via", "buildings"}});
    // Three buildings of different kinds: a shop, a house and the one with the most modules.
    std::vector<std::pair<int, std::string>> all;
    std::string shop, house;
    const json meshes = run("ls", {{"type", "mesh"}});
    for (const json& m : meshes)
    {
        const std::string mesh = m.is_string() ? m.get<std::string>() : m.value("name", std::string());
        json list;
        try
        {
            list = run("buildingInfo", {{"args", {mesh}}, {"limit", 0}});
        }
        catch (const std::exception&)
        {
            continue;
        }
        for (const json& b : list["buildings"])
        {
            const std::string id = b["id"];
            const std::string program = b.value("program", std::string());
            if (shop.empty() && (program == "shop" || program == "commercial" || program == "retail" || program == "restaurant"))
                shop = id;
            else if (house.empty() && (program == "residential" || program == "house" || program.empty()))
                house = id;
            all.push_back({b["modules"].get<int>(), id});
        }
    }
    REQUIRE(all.size() >= 3);
    std::sort(all.rbegin(), all.rend());
    std::vector<std::string> pick;
    for (const std::string& id : {shop, house})
        if (!id.empty())
            pick.push_back(id);
    for (const auto& [n, id] : all)
        if (pick.size() < 3 && std::find(pick.begin(), pick.end(), id) == pick.end())
            pick.push_back(id);
    const json made = run("buildingMakeEnterable", {{"args", pick}});
    CAPTURE(made.dump());
    REQUIRE(made["count"] == 3);
    for (const json& b : made["buildings"])
    {
        CHECK(b["rooms"].get<int>() >= 1);
        CHECK(b["unreachable"] == 0);
        CHECK(!b["entries"].empty());
    }
    const json check = run("buildingCheck");
    CAPTURE(check["counts"].dump());
    CHECK(check["interiors"] == 3);
    for (const char* k : {"roomUnreachable", "doorNoOpening", "stairSteep", "stairLow", "doorNarrow", "doorLow", "noEntry", "interiorMissing"})
        CHECK(check["counts"][k] == 0);
}


TEST_CASE("interiors commands: clutter strewn on every room's floor through Set Dressing")
{
    loadPackages(false);
    if (!lila::ops::find("scatterProps"))
        REQUIRE(lila::extensions::ExtensionManager::get().load(LILA_TEST_STAGEHAND_PACKAGE) == "Stagehand");
    run("newScene");
    run("buildingCreate", {{"envelopes", town()}, {"name", "Town"}});
    const std::string crate = pathOf(run("polyCube", {{"name", "Crate"}, {"width", 30.0}, {"height", 30.0}, {"depth", 30.0}}));
    run("move", {{"args", {crate}}, {"by", {0, 0, -100000}}});
    const json made = run("buildingMakeEnterable", {{"args", {"building:4a1"}}, {"clutter", {crate}}, {"clutterDensity", 1.0}, {"settle", false}});
    const json c = made["buildings"][0]["clutter"];
    CAPTURE(c.dump());
    CHECK(c["rooms"].get<int>() >= 3);
    CHECK(c["placed"].get<int>() > 0);
    CHECK_FALSE(c.contains("errors"));
    // Again: the earlier clutter is replaced, not added to.
    const json again = run("buildingMakeEnterable", {{"args", {"building:4a1"}}, {"clutter", {crate}}, {"clutterDensity", 1.0}, {"settle", false}});
    CHECK(again["buildings"][0]["clutter"]["placed"] == c["placed"]);
}

TEST_CASE("interiors commands: compact stairs land in the room with room to step off, and buildingCheck stays clean")
{
    loadPackages(false);
    run("newScene");
    // Small square houses fit only a dog-leg, a narrow one a straight flight.
    run("buildingCreate", {{"envelopes", json::array({env(0x5a1, 0, 0, 480, 480, 3, "residential", "residential"),
                                                       env(0x5a2, 2000, 0, 200, 1200, 4, "residential", "residential"),
                                                       env(0x5a3, 4000, 0, 440, 440, 3, "residential", "residential")})},
                           {"name", "Compact"}});
    const json made = run("buildingMakeEnterable", {{"args", {"building:5a1", "building:5a2", "building:5a3"}}});
    REQUIRE(made["count"] == 3);
    std::set<std::string> kinds;
    for (const char* b : {"building:5a1", "building:5a2", "building:5a3"})
    {
        const json info = run("buildingInterior", {{"args", {b}}});
        for (const json& st : info["stairs"])
            kinds.insert(st["kind"].get<std::string>());
    }
    CHECK(kinds.contains("dogleg"));
    CHECK(kinds.contains("straight"));
    // Each archway onto a compact stair has floor in front of it: a door check ray started 40 cm back meets no wall.
    const json check = run("buildingCheck");
    CAPTURE(check.dump());
    CHECK(check["counts"]["doorNoOpening"] == 0);
    CHECK(check["counts"]["roomUnreachable"] == 0);
}
