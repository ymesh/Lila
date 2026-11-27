// The building grammar on its own: every style and several seeds, stable module paths, party walls, courtyards, slopes,
// bay paint and toggles, and the polygon helpers it stands on.

#include "Buildings.h"
#include <lila/Mesh.h>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>

using namespace buildings;

namespace
{
std::map<std::string, Style> loadStyles()
{
    std::map<std::string, Style> out;
    for (const auto& e : std::filesystem::directory_iterator(LILA_TEST_BUILDINGS_STYLES))
    {
        std::ifstream in(e.path());
        Style s = Style::fromJson(json::parse(in));
        out[s.id] = s;
    }
    return out;
}

const std::map<std::string, Style>& styles()
{
    static const std::map<std::string, Style> s = loadStyles();
    return s;
}

Spec rect(double x0, double z0, double w, double d, int floors, std::vector<int> sides = {0, 2, 2, 2}, uint64_t key = 0x1a2b)
{
    Spec s;
    s.key = key;
    s.id = "building:" + hex64(key);
    s.footprint = {{x0, z0}, {x0 + w, z0}, {x0 + w, z0 + d}, {x0, z0 + d}};
    s.sides = sides;
    s.floors = floors;
    s.front = {0.0, -1.0};
    s.ridge = {1.0, 0.0};
    s.entrances = {{x0 + w * 0.5, z0}};
    return s;
}

std::vector<std::string> paths(const Result& r)
{
    std::vector<std::string> out;
    for (const Module& m : r.modules)
        out.push_back(m.path);
    return out;
}

bool pointInTri(Vec2 p, Vec2 a, Vec2 b, Vec2 c)
{
    auto cr = [](Vec2 o, Vec2 u, Vec2 v) { return (u.x - o.x) * (v.y - o.y) - (u.y - o.y) * (v.x - o.x); };
    const double d1 = cr(a, b, p), d2 = cr(b, c, p), d3 = cr(c, a, p);
    return (d1 >= 0 && d2 >= 0 && d3 >= 0) || (d1 <= 0 && d2 <= 0 && d3 <= 0);
}
} // namespace

TEST_CASE("buildings: polygon helpers")
{
    const Ring sq = {{0, 0}, {1000, 0}, {1000, 1000}, {0, 1000}};
    CHECK(signedArea(sq) == doctest::Approx(1e6));
    CHECK(signedArea(cleanRing({{0, 0}, {0, 1000}, {1000, 1000}, {1000, 0}})) > 0.0);
    CHECK(overlapArea(sq, {{500, 500}, {1500, 500}, {1500, 1500}, {500, 1500}}) == doctest::Approx(250000.0).epsilon(0.01));
    CHECK(overlapArea(sq, {{1000, 0}, {2000, 0}, {2000, 1000}, {1000, 1000}}) == doctest::Approx(0.0).epsilon(0.01));
    std::vector<Vec2> pts;
    std::vector<uint32_t> tris;
    triangulateWithHoles(sq, {{{300, 300}, {700, 300}, {700, 700}, {300, 700}}}, pts, tris);
    double area = 0.0;
    for (size_t t = 0; t + 2 < tris.size(); t += 3)
        area += std::abs(signedArea({pts[tris[t]], pts[tris[t + 1]], pts[tris[t + 2]]}));
    CHECK(area == doctest::Approx(1e6 - 160000.0).epsilon(0.001));
    CHECK(hexColour("#ffffff", glm::vec3(0)).r == doctest::Approx(1.0f));
    CHECK(colourHex(hexColour("#c8553d", glm::vec3(0))) == "#c8553d");
}

TEST_CASE("buildings: palette colour and dirt go in the first colour set, which exporters write as the vertex colour")
{
    Soup s;
    const Vec3 quad[4] = {{0, 0, 0}, {100, 0, 0}, {100, 100, 0}, {0, 100, 0}};
    const glm::vec2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    s.poly(quad, uv, 4, 0, 0, 0, PartWall, -1, glm::vec4(0.5f, 0.2f, 0.1f, 0.3f));
    const lila::geo::Mesh m = toMesh(s, {"Bld_residential_render0"});
    REQUIRE(lila::geo::colors::sets(m).size() == 1);
    const auto col = lila::geo::colors::values(m, lila::geo::colors::sets(m).front());
    for (lila::geo::Index c : m.faceCorners(0))
        CHECK(col[c] == glm::vec4(0.5f, 0.2f, 0.1f, 0.3f));
}

TEST_CASE("buildings: the shipped styles read and every style generates with several seeds")
{
    REQUIRE(styles().size() >= 16);
    for (const std::string& id : shippedStyleIds())
        CHECK_MESSAGE(styles().count(id) == 1, id);
    for (const auto& [id, st] : styles())
        for (int seed = 0; seed < 4; ++seed)
        {
            CAPTURE(id);
            CAPTURE(seed);
            Spec s = rect(0, 0, 2600, 2000, std::clamp(4, st.minFloors, st.maxFloors), {0, 1, 2, 0}, 0x100 + uint64_t(seed));
            s.businesses = {{"Abarrotes La Calandria", "shop", {900, 0}, true}};
            Edits e;
            e.seed = seed;
            const auto r = generate(s, st, e);
            CHECK(r->warnings.empty());
            CHECK(r->shell.faces() > 20);
            CHECK(r->modules.size() > 5);
            CHECK(r->floors >= st.minFloors);
            std::set<std::string> unique;
            for (const Module& m : r->modules)
            {
                CHECK_MESSAGE(unique.insert(m.path).second, m.path);
                CHECK_FALSE(m.type.empty());
            }
            CHECK(!r->plates.empty());
            CHECK(r->hlod.faces() > 0);
            CHECK(r->far.faces() > 0);
            // A business gets its fictional name on a sign wherever the style has shopfronts.
            if (st.shopfronts && id != "warehouse")
            {
                bool sign = false;
                for (const Module& m : r->modules)
                    sign = sign || (m.kind == "sign" && m.data.value("text", std::string()) == "Abarrotes La Calandria");
                CHECK(sign);
            }
        }
}

TEST_CASE("buildings: module paths are stable and follow tier, facade, floor, bay, kind")
{
    const Style& st = styles().at("colonial");
    const Spec a = rect(0, 0, 1600, 1200, 2);
    const auto r1 = generate(a, st, {});
    const auto r2 = generate(a, st, {});
    CHECK(paths(*r1) == paths(*r2));
    bool sawBay = false;
    for (const Module& m : r1->modules)
        if (m.path.find("/l0/b") != std::string::npos)
        {
            sawBay = true;
            CHECK(m.path.rfind("t0/f", 0) == 0);
        }
    CHECK(sawBay);
    // An edit to another floor's bay leaves the first floor's ids alone.
    Edits e;
    e.bays["t0/f0/l1/b1"] = "wall";
    const auto r3 = generate(a, st, e);
    std::set<std::string> before, after;
    for (const std::string& p : paths(*r1))
        if (p.find("/l0/") != std::string::npos)
            before.insert(p);
    for (const std::string& p : paths(*r3))
        if (p.find("/l0/") != std::string::npos)
            after.insert(p);
    CHECK(before == after);
}

TEST_CASE("buildings: party walls are blank, courtyards open, roofs over the rest")
{
    const Style& st = styles().at("residential");
    const Spec s = rect(0, 0, 1200, 1000, 2, {0, 1, 2, 1});
    const auto r = generate(s, st, {});
    int party = 0;
    for (const Facade& f : r->facades)
        if (f.side == 1)
        {
            ++party;
            CHECK(f.bays == 0);
            for (const Module& m : r->modules)
                if (m.path.rfind(f.path + "/", 0) == 0)
                    CHECK(m.type == "wall.party");
        }
    CHECK(party == 2);

    Spec c = rect(0, 0, 3000, 3000, 2, {0, 0, 0, 0});
    c.holes = {{{1000, 1000}, {1000, 2000}, {2000, 2000}, {2000, 1000}}};
    const auto rc = generate(c, styles().at("colonial"), {});
    int courtyard = 0;
    for (const Facade& f : rc->facades)
        courtyard += f.courtyard ? 1 : 0;
    CHECK(courtyard == 4);
    // No rooftop face covers the courtyard's middle.
    for (size_t f = 0, off = 0; f < rc->shell.sizes.size(); off += rc->shell.sizes[f], ++f)
    {
        if (rc->shell.part[f] != PartRooftop || rc->shell.sizes[f] != 3)
            continue;
        const Vec3* p = rc->shell.pts.data() + off;
        CHECK_FALSE(pointInTri({1500, 1500}, {p[0].x, p[0].z}, {p[1].x, p[1].z}, {p[2].x, p[2].z}));
    }
    CHECK(!rc->plates.empty());
    CHECK(rc->plates.front().holes.size() == 1);
}

TEST_CASE("buildings: slopes step the ground floor, doors reach the ground or get steps")
{
    const Style& st = styles().at("colonial");
    Spec s = rect(0, 0, 2400, 1200, 2);
    s.cornerGround = {0.0, 260.0, 260.0, 0.0}; // rising along the street
    s.ground = 0.0;
    s.highGround = 260.0;
    const auto r = generate(s, st, {});
    std::set<int> floorLevels;
    for (const Bay& b : r->bays)
        if (b.floor == 0 && b.path.rfind("t0/f0/", 0) == 0)
            floorLevels.insert(int(std::lround(b.corners[0].y)));
    CHECK(floorLevels.size() >= 2);
    for (const Module& m : r->modules)
    {
        if (m.kind != "door" || m.type == "door.balcony" || m.path.find("/l0/") == std::string::npos)
            continue;
        const Vec3 front = m.origin + m.z * 80.0;
        const double gap = m.origin.y - s.groundAt({front.x, front.z});
        const std::string bay = m.path.substr(0, m.path.find_last_of('/'));
        bool steps = false;
        for (const Module& o : r->modules)
            steps = steps || (o.kind == "steps" && o.path.rfind(bay, 0) == 0);
        CHECK((gap <= 45.0 || steps));
        CHECK(gap > -30.0);
    }
}

TEST_CASE("buildings: bay paint, toggles and bay counts")
{
    const Style& st = styles().at("residential");
    const Spec s = rect(0, 0, 1500, 1000, 2);
    Edits e;
    e.bays["t0/f0/l0/b0"] = "door";
    e.toggles["t0/f0/l1/b1"]["balcony"] = true;
    e.facadeBays["t0/f2"] = 2;
    const auto r = generate(s, st, e);
    bool door = false, balcony = false;
    for (const Module& m : r->modules)
    {
        door = door || m.path == "t0/f0/l0/b0/door";
        balcony = balcony || m.path == "t0/f0/l1/b1/balcony";
    }
    CHECK(door);
    CHECK(balcony);
    for (const Facade& f : r->facades)
        if (f.path == "t0/f2")
            CHECK(f.bays == 2);
    Edits w;
    w.bays["t0/f0/l1/b1"] = "wall";
    const auto rw = generate(s, st, w);
    for (const Module& m : rw->modules)
        CHECK(m.path.rfind("t0/f0/l1/b1/window", 0) != 0);
}

TEST_CASE("buildings: podium and tower, bell towers, chamfered corners, mansion extras")
{
    const auto tower = generate(rect(0, 0, 3000, 2400, 12), styles().at("resortTower"), {});
    std::set<int> tiers;
    for (const Facade& f : tower->facades)
        tiers.insert(f.tier);
    CHECK(tiers.size() >= 2);
    int balconies = 0;
    for (const Module& m : tower->modules)
        balconies += m.kind == "balcony" ? 1 : 0;
    CHECK(balconies > 20);

    const auto church = generate(rect(0, 0, 2400, 4000, 1), styles().at("church"), {});
    int domes = 0;
    for (const Module& m : church->modules)
        domes += m.type == "dome" ? 1 : 0;
    CHECK(domes == 2);

    Spec corner = rect(0, 0, 1500, 1500, 2, {0, 0, 2, 2});
    corner.corner = "chamfer";
    const auto rc = generate(corner, styles().at("colonial"), {});
    bool chamfer = false;
    for (const Facade& f : rc->facades)
        chamfer = chamfer || f.path.find("x") != std::string::npos;
    CHECK(chamfer);

    Spec villa = rect(0, 0, 2000, 1400, 2);
    villa.setbackFront = 800;
    villa.setbackSide = 600;
    const auto rv = generate(villa, styles().at("mansion"), {});
    std::set<std::string> kinds;
    for (const Module& m : rv->modules)
        kinds.insert(m.kind);
    for (const char* k : {"column", "portico", "perimeter", "gate", "pool", "dome"})
        CHECK_MESSAGE(kinds.count(k) == 1, k);
}

TEST_CASE("buildings: placeholders have uvs and faces for every kind")
{
    for (const std::string& k : placeholderKinds())
    {
        const Soup& s = placeholderSoup(k, {200, 250, 30}, 0);
        CAPTURE(k);
        CHECK(s.faces() > 0);
        CHECK(s.uvs.size() == s.pts.size());
    }
    // The sign's face is its own material with uvs over 0..1 for text.
    const Soup& sign = placeholderSoup("sign", {300, 80, 8}, 0);
    bool face = false;
    for (int32_t m : sign.mat)
        face = face || m == MatSignFace;
    CHECK(face);
}

TEST_CASE("buildings: a portico's floor stands on the ground in front of the door, on slopes too")
{
    for (const char* style : {"mansion", "mausoleum"})
        for (const std::vector<double>& corners : {std::vector<double>{0.0, 0.0, 0.0, 0.0}, std::vector<double>{400.0, 400.0, 0.0, 0.0},
                                                   std::vector<double>{0.0, 0.0, 400.0, 400.0}, std::vector<double>{0.0, 350.0, 350.0, 0.0}})
        {
            Spec s = rect(0, 0, 2000, 1400, 2);
            s.cornerGround = corners;
            s.ground = *std::min_element(corners.begin(), corners.end());
            s.highGround = *std::max_element(corners.begin(), corners.end());
            const auto r = generate(s, styles().at(style), {});
            int floors = 0;
            for (const Module& m : r->modules)
                if (m.path.find("portico/floor") != std::string::npos)
                {
                    ++floors;
                    const double ground = s.groundAt({m.origin.x, m.origin.z});
                    CHECK_MESSAGE(std::abs(m.origin.y - ground) <= 120.0, style << " ground " << ground << " floor " << m.origin.y);
                    CHECK(m.size.y >= 15.0);
                }
            CHECK_MESSAGE(floors == 1, style);
        }
}

TEST_CASE("buildings: two-tone modules take the tones their style names, framed pieces keep their frames by default")
{
    const Style& centro = styles().at("colonial");
    const Style& plain = styles().at("residential");
    Palette p;
    p.render = {0.8f, 0.2f, 0.1f};
    p.trim = {0.9f, 0.9f, 0.8f};
    p.roof = {0.4f, 0.3f, 0.2f};
    const glm::vec4 render(p.render, 1.0f), trim(p.trim, 1.0f), roof(p.roof, 1.0f), none(0.0f);
    // Defaults: one colour on both tones, framed pieces without a second.
    CHECK(moduleTones(plain, p, "wall") == std::array<glm::vec4, 2>{render, render});
    CHECK(moduleTones(plain, p, "roof") == std::array<glm::vec4, 2>{roof, roof});
    CHECK(moduleTones(plain, p, "arcade") == std::array<glm::vec4, 2>{trim, trim});
    CHECK(moduleTones(plain, p, "window") == std::array<glm::vec4, 2>{trim, none});
    CHECK(moduleTones(plain, p, "door") == std::array<glm::vec4, 2>{trim, none});
    // colonial: arcade beams in the render, columns in the trim; window and door frames in the trim.
    CHECK(moduleTones(centro, p, "arcade") == std::array<glm::vec4, 2>{render, trim});
    CHECK(moduleTones(centro, p, "window") == std::array<glm::vec4, 2>{trim, trim});
    CHECK(moduleTones(centro, p, "door") == std::array<glm::vec4, 2>{trim, trim});
    // The style keeps them through its JSON, and "none" drops a tone.
    Style s = Style::fromJson(centro.toJson());
    CHECK(s.moduleTones == centro.moduleTones);
    json j = plain.toJson();
    j["moduleTones"] = {{"arcade", {"none", "render"}}};
    s = Style::fromJson(j);
    CHECK(moduleTones(s, p, "arcade") == std::array<glm::vec4, 2>{none, render});
}

TEST_CASE("buildings: a pitched style roof spans a house, a footprint wider than 40 m takes a flat one")
{
    const Style& st = styles().at("church");
    REQUIRE(st.roof != "flat");
    auto roofOf = [&](double w, double d) {
        const auto r = generate(rect(0, 0, w, d, 1, {0, 2, 2, 2}), st, {});
        for (const Module& m : r->modules)
            if (m.path == "t0/roof")
                return m.type;
        return std::string();
    };
    CHECK(roofOf(1400, 2400) != "roof.flat");
    CHECK(roofOf(6000, 6000) == "roof.flat");
}

TEST_CASE("buildings: a sheet roof over an irregular lot stays over its footprint")
{
    // A warehouse from the real-data port city: its gabled roof ran ridge points hundreds of metres out over the hills.
    const json env = json::parse(R"({"base": 2768.25, "corner": "wrap", "floors": 1, "ground": 2818.25, "groundUse": "industrial", "height": 462.0,
        "footprint": [[262963.1256625021, 635720.4352906728], [264155.073679906, 635104.3110016737], [264694.7368789806, 634825.3562150773],
                      [264699.35019978386, 635605.5644239584], [264221.1996084282, 635608.3916975568], [264026.8302063728, 635929.7985814123],
                      [263306.3990770806, 635925.7517459665], [263150.4765349767, 636227.9381590784], [262556.71762608667, 635930.5097728012]],
        "sides": ["street", "street", "street", "party", "party", "party", "open", "street", "street"],
        "front": [-0.4591873899126458, -0.8883394289038464], "ridge": [0.8883394289038464, -0.4591873899126458],
        "entrances": [[263559.09967120405, 635412.3731461733], [264697.0435393822, 635215.4603195179]],
        "lot": 34342, "lotId": 531092620978046, "roof": "flat", "roofHeight": 0.0, "style": "warehouse", "top": 3390.351312716969,
        "use": "industrial", "wealth": 1})");
    const Spec s = Spec::fromEnvelope(env, "Puerto");
    const auto r = generate(s, styles().at("warehouse"), {});
    double lo = 1e300, hi = -1e300, x0 = 1e300, x1 = -1e300, z0 = 1e300, z1 = -1e300;
    for (const Vec3& p : r->shell.pts)
    {
        lo = std::min(lo, p.y);
        hi = std::max(hi, p.y);
        x0 = std::min(x0, p.x);
        x1 = std::max(x1, p.x);
        z0 = std::min(z0, p.z);
        z1 = std::max(z1, p.z);
    }
    CHECK(r->warnings.empty());
    CHECK(hi < 3390.35 + 400.0);
    CHECK(x0 > 262556.7 - 200.0);
    CHECK(x1 < 264699.4 + 200.0);
    CHECK(z0 > 634825.4 - 200.0);
    CHECK(z1 < 636227.9 + 200.0);
}

namespace
{
struct FlatFace
{
    std::vector<Vec3> pts;
    Vec3 n{0.0};
    double d = 0.0;
    std::string from; // module path or shell part
    int building = 0, module = -1;
};

/** Every face a building draws: its shell and its modules' placeholders where they stand. */
void facesOf(const Result& r, int building, std::vector<FlatFace>& out)
{
    auto add = [&](const Soup& s, const glm::dmat4& m, const std::string& from, int module) {
        for (size_t f = 0, off = 0; f < s.sizes.size(); off += s.sizes[f], ++f)
        {
            FlatFace x;
            for (uint32_t k = 0; k < s.sizes[f]; ++k)
                x.pts.push_back(Vec3(m * glm::dvec4(s.pts[off + k], 1.0)));
            Vec3 n{0.0};
            for (size_t k = 0; k < x.pts.size(); ++k)
                n += glm::cross(x.pts[k], x.pts[(k + 1) % x.pts.size()]);
            if (glm::length(n) < 1e-6)
                continue;
            x.n = glm::normalize(n);
            x.d = glm::dot(x.n, x.pts[0]);
            x.from = module >= 0 ? from : from + std::to_string(s.part[f]);
            x.building = building;
            x.module = module;
            out.push_back(std::move(x));
        }
    };
    add(r.shell, glm::dmat4(1.0), "shell part ", -1);
    for (size_t k = 0; k < r.modules.size(); ++k)
        if (!r.modules[k].shell)
            add(placeholderSoup(r.modules[k].kind, r.modules[k].size, r.modules[k].variant), r.modules[k].frame(), r.modules[k].path, int(k));
}

/** Pairs of faces lying in one plane over each other by more than a few square centimetres: what flickers in a view. */
std::vector<std::string> coplanarOverlaps(const std::vector<FlatFace>& faces)
{
    std::map<std::tuple<long, long, long, long>, std::vector<size_t>> buckets;
    auto key = [](const FlatFace& f, long dd) {
        const double s = (f.n.x < -1e-9 || (std::abs(f.n.x) <= 1e-9 && (f.n.y < -1e-9 || (std::abs(f.n.y) <= 1e-9 && f.n.z < 0)))) ? -1.0 : 1.0;
        return std::tuple<long, long, long, long>{std::lround(f.n.x * s * 200), std::lround(f.n.y * s * 200), std::lround(f.n.z * s * 200),
                                                  long(std::floor(f.d * s / 2.0)) + dd};
    };
    for (size_t i = 0; i < faces.size(); ++i)
        buckets[key(faces[i], 0)].push_back(i);
    std::vector<std::string> out;
    for (size_t i = 0; i < faces.size(); ++i)
        for (long dd : {-1L, 0L, 1L})
        {
            auto it = buckets.find(key(faces[i], dd));
            if (it == buckets.end())
                continue;
            for (size_t j : it->second)
            {
                if (j <= i)
                    continue;
                const FlatFace &a = faces[i], &b = faces[j];
                if (a.module >= 0 && a.module == b.module && a.building == b.building)
                    continue;
                if (std::abs(glm::dot(a.n, b.n)) < 0.99999 || std::abs(a.d - glm::dot(a.n, b.pts[0])) > 0.3)
                    continue;
                // A module's bottom is only ever seen from under it: standing on a roof or the ground it hides itself.
                if ((a.module >= 0 && a.n.y < -0.99) || (b.module >= 0 && b.n.y < -0.99))
                    continue;
                // Both into the plane's own 2D frame.
                const Vec3 u = glm::normalize(std::abs(a.n.y) < 0.9 ? glm::cross(Vec3(0, 1, 0), a.n) : glm::cross(Vec3(1, 0, 0), a.n));
                const Vec3 v = glm::cross(a.n, u);
                auto flat = [&](const FlatFace& f) {
                    Ring r;
                    for (const Vec3& p : f.pts)
                        r.push_back({glm::dot(p, u), glm::dot(p, v)});
                    return cleanRing(r);
                };
                const Ring ra = flat(a), rb = flat(b);
                if (ra.size() < 3 || rb.size() < 3)
                    continue;
                if (const double area = overlapArea(ra, rb); area > 4.0)
                    {
                    std::string w = "b" + std::to_string(a.building) + "/b" + std::to_string(b.building) + " " + a.from + " / " + b.from + " (" + std::to_string(int(area)) + " cm2) n " + std::to_string(a.n.x) + "," + std::to_string(a.n.y) + "," + std::to_string(a.n.z) + " at";
                    for (const Vec3& p : a.pts) w += " (" + std::to_string(int(p.x)) + "," + std::to_string(int(p.y)) + "," + std::to_string(int(p.z)) + ")";
                    w += " vs";
                    for (const Vec3& p : b.pts) w += " (" + std::to_string(int(p.x)) + "," + std::to_string(int(p.y)) + "," + std::to_string(int(p.z)) + ")";
                    out.push_back(w);
                }
            }
        }
    return out;
}
} // namespace

TEST_CASE("buildings: no two faces of a building, or of neighbours across a party wall, lie in one plane over each other")
{
    for (const auto& [id, st] : styles())
    {
        CAPTURE(id);
        // Two neighbours of different heights sharing a party wall, and a third against the first's open side.
        Spec a = rect(0, 0, 1400, 1000, 3, {0, 1, 2, 2}, 0x51);
        a.businesses = {{"Abarrotes La Calandria", "shop", {700, 0}, true}};
        Spec b = rect(1400, 0, 900, 1000, 2, {0, 2, 2, 1}, 0x52);
        Spec c = rect(-800, 0, 800, 1000, 2, {0, 2, 2, 2}, 0x53);
        // The third's right side lies on the first's left: built as a party wall from the measured clearance.
        std::vector<Spec> specs = {a, b, c};
        measureClearance(specs);
        CHECK(specs[0].clearance[3] < 1.0);
        CHECK(specs[2].clearance[1] < 1.0);
        std::vector<FlatFace> faces;
        for (size_t k = 0; k < specs.size(); ++k)
        {
            const auto r = generate(specs[k], st, {});
            facesOf(*r, int(k), faces);
        }
        const std::vector<std::string> bad = coplanarOverlaps(faces);
        for (size_t k = 0; k < std::min<size_t>(bad.size(), 8); ++k)
            MESSAGE(bad[k]);
        CHECK(bad.empty());

        Spec yard = rect(0, 0, 3000, 3000, 2, {0, 0, 0, 0}, 0x54);
        yard.holes = {{{1000, 1000}, {1000, 2000}, {2000, 2000}, {2000, 1000}}};
        faces.clear();
        facesOf(*generate(yard, st, {}), 0, faces);
        const std::vector<std::string> badYard = coplanarOverlaps(faces);
        for (size_t k = 0; k < std::min<size_t>(badYard.size(), 8); ++k)
            MESSAGE(badYard[k]);
        CHECK(badYard.empty());
    }
}

TEST_CASE("buildings: bay rhythm: blank piers on street fronts, sparser side walls, none facing a neighbour close by")
{
    const Style& st = styles().at("residential");
    // A neighbour 2 m off the left wall; the right wall looks over open ground.
    std::vector<Spec> specs = {rect(0, 0, 2400, 1200, 3, {0, 2, 2, 2}, 0x61), rect(-1200, 0, 1000, 1200, 2, {0, 2, 2, 2}, 0x62)};
    measureClearance(specs);
    CHECK(specs[0].clearance[3] == doctest::Approx(200.0).epsilon(0.01));
    CHECK(specs[0].clearance[1] == doctest::Approx(2000.0));
    CHECK(Spec::fromJson(specs[0].toJson()).clearance == specs[0].clearance);
    const auto r = generate(specs[0], st, {});
    std::map<std::string, int> openings, bays;
    for (const Bay& b : r->bays)
    {
        const std::string f = b.path.substr(0, b.path.find("/l"));
        ++bays[f];
        openings[f] += b.kind != "wall";
    }
    CHECK(bays["t0/f3"] > 0);
    CHECK(openings["t0/f3"] == 0);
    CHECK(openings["t0/f1"] < bays["t0/f1"]);
    CHECK(openings["t0/f0"] * 3 >= bays["t0/f0"] * 2);
    // Without a measured neighbour a side wall still opens a column.
    CHECK(openings["t0/f1"] > 0);

    // A rhythm in the style lays blank piers mirrored from both ends.
    Style piers = st;
    piers.rhythm = "wb";
    const auto rp = generate(rect(0, 0, 2400, 1200, 2, {0, 2, 2, 2}, 0x63), piers, {});
    std::vector<std::string> row;
    for (const Bay& b : rp->bays)
        if (b.path.rfind("t0/f0/l1/", 0) == 0)
            row.push_back(b.kind);
    REQUIRE(row.size() >= 4);
    CHECK(row[0] == "window");
    CHECK(row[1] == "wall");
    CHECK(row[row.size() - 1] == "window");
    CHECK(row[row.size() - 2] == "wall");
}

TEST_CASE("buildings: parapets and cornices stop short of a party wall and mitre at corners")
{
    for (const char* id : {"colonial", "residential"})
    {
        CAPTURE(id);
        const auto r = generate(rect(0, 0, 1400, 1000, 2, {0, 1, 2, 2}, 0x71), styles().at(id), {});
        int parapets = 0;
        for (size_t f = 0, off = 0; f < r->shell.sizes.size(); off += r->shell.sizes[f], ++f)
        {
            const int part = r->shell.part[f];
            if (part != PartParapet && part != PartCornice && part != PartBand)
                continue;
            parapets += part == PartParapet;
            for (uint32_t k = 0; k < r->shell.sizes[f]; ++k)
                CHECK(r->shell.pts[off + k].x <= 1400.0 - 1.0);
        }
        CHECK(parapets > 0);
    }
}
