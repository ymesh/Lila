#include "Generator.h"
#include "TrimBake.h"
#include "Strips.h"
#include "TrimSheet.h"

#include "extensions/ExtensionManager.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "math/Unreal.h"
#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "uv/Hotspot.h"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <set>

using namespace lila;
using namespace trims;
using json = nlohmann::json;

namespace
{
std::filesystem::path scratch(const std::string& name)
{
    static const std::filesystem::path root = std::filesystem::temp_directory_path() / ("lila_trim_" + std::to_string(std::random_device{}()));
    const std::filesystem::path p = root / name;
    std::filesystem::create_directories(p);
    return p;
}

/** A vertical quad w wide (along X) and h tall (along Y), facing +Z. */
geo::Mesh wall(double w, double h)
{
    const glm::dvec3 pts[] = {{0, 0, 0}, {w, 0, 0}, {w, h, 0}, {0, h, 0}};
    const geo::Index sizes[] = {4};
    const geo::Index idx[] = {0, 1, 2, 3};
    return geo::Mesh::fromPolygons(pts, sizes, idx);
}

glm::dvec2 uvAt(const geo::Mesh& m, glm::dvec3 p)
{
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
    for (geo::Index c = 0; c < m.cornerCapacity(); ++c)
        if (m.cornerValid(c) && glm::length(m.position(m.cornerPoint(c)) - p) < 1e-6)
            return glm::dvec2(uv[c]);
    FAIL("no corner at the point");
    return {};
}

void loadTrims()
{
    if (!ops::find("trimSheetCreate"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_TRIM_PACKAGE) == "TrimSheets");
}
} // namespace

TEST_CASE("trim: every preset fills its sheet without overlaps")
{
    for (const PresetInfo& p : presets())
    {
        const Sheet s = preset(p.id);
        CAPTURE(p.id);
        REQUIRE(!s.trims.empty());
        for (const std::string& w : validate(s))
            CHECK_MESSAGE(w.find("overlaps") == std::string::npos, w);
        long long area = 0;
        for (const Trim& t : s.trims)
            area += (long long)(t.sizePx().x) * t.sizePx().y;
        CHECK(area == (long long)(s.width) * s.height);
    }
}

TEST_CASE("trim: a sheet survives JSON, .rect, SVG and ID map round trips")
{
    const Sheet s = preset("sciFiHull", 1024);
    const Sheet j = Sheet::fromJson(s.toJson());
    REQUIRE(j.trims.size() == s.trims.size());
    CHECK(j.trims[5].profile.style == s.trims[5].profile.style);
    CHECK(j.trims[5].kind == s.trims[5].kind);

    const Sheet r = fromRect(toRect(s), s.width, s.height);
    REQUIRE(r.trims.size() == s.trims.size());
    for (size_t i = 0; i < s.trims.size(); ++i)
    {
        CHECK(r.trims[i].min == s.trims[i].min);
        CHECK(r.trims[i].max == s.trims[i].max);
    }
    CHECK(r.trims[0].kind == uv::TrimKind::StripU);

    const Sheet v = fromSvg(toSvg(s));
    CHECK(v.width == s.width);
    REQUIRE(v.trims.size() == s.trims.size());
    CHECK(v.trims[3].name == s.trims[3].name);

    const Sheet id = fromIdMap(idMap(s));
    CHECK(id.trims.size() == s.trims.size());
}

TEST_CASE("trim: ZenUV trims import from their JSON and from a trim preset script")
{
    const std::string jsonText = R"({"trimsheet": {
        "Plank_Long": {"rect": [0.0, 1.0, 1.0, 0.75], "hide": false},
        "Bolt": {"rect": [0.0, 0.75, 0.25, 0.5]},
        "Hidden": {"rect": [0.5, 0.5, 1.0, 0.0], "hide": true}}})";
    const Sheet j = fromZenUv(jsonText, 1024, 512);
    REQUIRE(j.trims.size() == 2);
    CHECK(j.trims[0].name == "Plank_Long");
    CHECK(j.trims[0].kind == uv::TrimKind::StripU);
    CHECK(j.trims[0].min == glm::ivec2(0, 0));
    CHECK(j.trims[0].max == glm::ivec2(1024, 128));
    CHECK(j.trims[1].name == "Bolt");
    CHECK(j.trims[1].kind == uv::TrimKind::Rect);
    CHECK(j.trims[1].min == glm::ivec2(0, 128));
    CHECK(j.trims[1].max == glm::ivec2(256, 256));

    const std::string script = "import bpy\n"
                               "item = prefs.trimsheet.add()\n"
                               "item.name = 'Bolt'\n"
                               "item.rect = (0.0, 0.75, 0.25, 0.5)\n"
                               "item.color = (0.8, 0.2, 0.2)\n"
                               "item = prefs.trimsheet.add()\n"
                               "item.name = \"Rim\"\n"
                               "item.rect = (0.5, 0.25, 1.0, 0.0)\n";
    const Sheet p = fromZenUv(script, 1024, 512);
    REQUIRE(p.trims.size() == 2);
    CHECK(p.trims[0].name == "Bolt");
    CHECK(p.trims[0].min == glm::ivec2(0, 128));
    CHECK(p.trims[1].name == "Rim");
    CHECK(p.trims[1].min == glm::ivec2(512, 384));
    CHECK(p.trims[1].max == glm::ivec2(1024, 512));
    CHECK(fromZenUv("not trim data", 1024, 512).trims.empty());
}

TEST_CASE("trim: uv rectangles and world sizes follow the pixel layout")
{
    Sheet s = stripLadder("T_Test", 1024, 1024, std::vector<int>{512, 256, 256});
    s.pxPerM = 512.0;
    const Trim& t = s.trims[1];
    CHECK(s.uvMin(t).y == doctest::Approx(0.25));
    CHECK(s.uvMax(t).y == doctest::Approx(0.5));
    CHECK(s.worldSize(t).y == doctest::Approx(50.0));
    CHECK(s.trimAt({0.3, 0.3}) == 1);
    // Tiling strips repeat along U.
    CHECK(s.trimAt({3.3, 0.3}) == 1);
}

TEST_CASE("trim: the generator is deterministic, bevels at the profile width and tiles strips")
{
    Sheet s = stripLadder("T_Gen", 512, 512, std::vector<int>{256, 128, 128});
    s.pxPerM = 512.0;
    s.trims[0].profile.bevelPx = 16.0;
    const Generated a = generate(s), b = generate(s);
    REQUIRE(a.high.numPoints() == b.high.numPoints());
    for (geo::Index p = 0; p < a.high.pointCapacity(); ++p)
        CHECK(a.high.position(p) == b.high.position(p));
    CHECK(a.above == doctest::Approx(16.0 * 100.0 / 512.0));
    // Strips run a sheet width past each side.
    const geo::Bounds bounds = a.high.bounds();
    CHECK(bounds.min.x == doctest::Approx(-100.0));
    CHECK(bounds.max.x == doctest::Approx(200.0));
    CHECK(a.low.numFaces() == 1);
    CHECK(uvAt(a.low, {0, 0, 0}) == glm::dvec2(0, 1));
    CHECK(glm::length(a.low.faceNormal(0) - glm::dvec3(0, 1, 0)) < 1e-9);

    // Every style builds.
    for (const std::string& name : styleNames())
    {
        Sheet one = stripLadder("T_Style", 256, 256, std::vector<int>{128, 128});
        one.trims[0].profile.style = *styleFromName(name);
        one.trims[1] = Trim::fromJson({{"name", "R"}, {"min", {0, 128}}, {"max", {128, 256}}, {"kind", "rect"}, {"profile", {{"style", name}}}});
        CAPTURE(name);
        CHECK(generate(one).high.numFaces() > 0);
    }
}

TEST_CASE("trim: the sheet bakes on the cpu, tiles across U and rebakes one trim")
{
    Sheet s = stripLadder("T_Bake", 128, 128, std::vector<int>{64, 32, 32});
    s.pxPerM = 128.0;
    s.supersampling = 1;
    s.maps = {"normal", "ao"};
    const std::filesystem::path file = scratch("bake") / "T_Bake.lilatrim";
    save(s, file);
    SheetBakeOptions o;
    o.backend = "cpu";
    const json r = bakeSheet(s, file, o);
    REQUIRE(r["outputs"].size() == 4);
    CHECK(r["outputs"][3]["map"] == "trimRect");
    CHECK(std::filesystem::exists(r["idMap"].get<std::string>()));
    CHECK(r["seamError"].get<double>() < 0.02);

    o.only = {1};
    const json again = bakeSheet(s, file, o);
    CHECK(again["composited"].size() == 2);
    CHECK(!std::filesystem::exists(textureFolder(s, file) / ".rebake"));
}

TEST_CASE("hotspot: a wall goes to the strip whose world height matches, up the strip")
{
    const Sheet s = preset("industrialMetal");
    const std::vector<uv::HotspotTrim> trims = hotspotTrims(s);
    // 256 px strips are 50 cm tall at 512 px per metre.
    geo::Mesh m = wall(400.0, 50.0);
    const uv::HotspotResult r = uv::hotspot(m, {}, trims, "uv0");
    REQUIRE(r.islands.size() == 1);
    REQUIRE(r.islands[0].trim >= 0);
    const Trim& t = s.trims[size_t(r.islands[0].trim)];
    CHECK(t.sizePx().y == 256);
    CHECK(t.kind == uv::TrimKind::StripU);
    const glm::dvec2 bottom = uvAt(m, {0, 0, 0}), top = uvAt(m, {0, 50, 0});
    CHECK(top.y > bottom.y);
    CHECK(top.y - bottom.y == doctest::Approx(s.uvMax(t).y - s.uvMin(t).y - 2.0 * t.insetPx / s.width).epsilon(0.01));
    CHECK(r.islands[0].sizeError < 0.1);

    // Same seed, same uvs; the next cycle picks another trim.
    geo::Mesh again = wall(400.0, 50.0);
    uv::hotspot(again, {}, trims, "uv0");
    CHECK(uvAt(again, {400, 50, 0}) == uvAt(m, {400, 50, 0}));
    std::set<int> cycled;
    for (int c = 1; c <= 3; ++c)
    {
        uv::HotspotOptions o;
        o.cycle = c;
        geo::Mesh other = wall(400.0, 50.0);
        cycled.insert(uv::hotspot(other, {}, trims, "uv0", o).islands[0].trim);
    }
    CHECK(cycled.size() == 3);
}

TEST_CASE("hotspot: a square panel takes a square hotspot of its size")
{
    const Sheet s = preset("hotspotGrid");
    const std::vector<uv::HotspotTrim> trims = hotspotTrims(s);
    // 256 px at 256 px per metre is a metre.
    geo::Mesh m = wall(100.0, 100.0);
    const uv::HotspotResult r = uv::hotspot(m, {}, trims, "uv0");
    REQUIRE(r.islands[0].trim >= 0);
    const Trim& t = s.trims[size_t(r.islands[0].trim)];
    CHECK(t.sizePx() == glm::ivec2(256, 256));
    const glm::dvec2 lo = uvAt(m, {0, 0, 0}), hi = uvAt(m, {100, 100, 0});
    CHECK(lo.x >= s.uvMin(t).x - 1e-9);
    CHECK(hi.x <= s.uvMax(t).x + 1e-9);
    CHECK(lo.y >= s.uvMin(t).y - 1e-9);
    CHECK(hi.y <= s.uvMax(t).y + 1e-9);
}

TEST_CASE("weighted normals: big faces stay flat beside a chamfer")
{
    // An L of two big faces joined by a thin 45 degree chamfer, all edges soft.
    const glm::dvec3 pts[] = {{0, 0, 0}, {100, 0, 0}, {100, 0, 99}, {0, 0, 99}, {100, 1, 100}, {0, 1, 100}, {100, 100, 100}, {0, 100, 100}};
    const geo::Index sizes[] = {4, 4, 4};
    const geo::Index idx[] = {3, 2, 1, 0, 5, 4, 2, 3, 7, 6, 4, 5};
    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    REQUIRE(m.numFaces() == 3);
    geo::applyWeightedNormals(m);
    std::span<const glm::vec3> n = std::as_const(m).cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    REQUIRE(!n.empty());
    for (geo::Index f : {geo::Index(0), geo::Index(2)})
    {
        const glm::dvec3 fn = m.faceNormal(f);
        m.forEachFaceCorner(f, [&](geo::Index c) { CHECK(glm::dot(glm::dvec3(n[c]), fn) == doctest::Approx(1.0).epsilon(1e-5)); });
    }
}

TEST_CASE("trim commands: create, inspect, edit and export through the package")
{
    loadTrims();
    const std::filesystem::path file = scratch("commands") / "T_Ladder.lilatrim";
    const json made = ops::execute("trimSheetCreate", {{"preset", "ladder"}, {"width", 256}, {"file", file.generic_string()}, {"bake", false}});
    CHECK(made["created"].get<bool>());
    CHECK(made["trims"].size() == 7);
    CHECK(ops::execute("trimSheetCurrent", json::object()).get<std::string>() == file.generic_string());

    ops::execute("trimSheetEdit", {{"trim", 0}, {"values", {{"profile", {{"style", "rivets"}}}}}});
    const json info = ops::execute("trimSheetInfo", json::object());
    CHECK(info["trims"][0]["profile"]["style"] == "rivets");

    const std::string rect = ops::execute("trimSheetExport", {{"format", "rect"}}).get<std::string>();
    CHECK(std::filesystem::exists(rect));
    const json imported = ops::execute("trimSheetImport", {{"args", {rect}}, {"width", 256}, {"height", 256}, {"file", (scratch("imported") / "T_In.lilatrim").generic_string()}});
    CHECK(imported["trims"].size() == 7);
}

TEST_CASE("trim: index and rectangle maps carry each trim's number and uv rectangle")
{
    const Sheet s = stripLadder("T_Maps", 64, 64, std::vector<int>{32, 16, 16});
    const lila::image::Image index = trimIndexMap(s), rect = trimRectMap(s);
    const auto texel = [&](const lila::image::Image& img, int x, int y) { return &img.rgba[(size_t(y) * size_t(img.width) + size_t(x)) * 4]; };
    CHECK(texel(index, 10, 5)[0] == 1.0f);
    CHECK(texel(index, 10, 40)[0] == 2.0f);
    CHECK(texel(index, 63, 63)[0] == 3.0f);
    const float* r = texel(rect, 3, 40);
    CHECK(r[0] == doctest::Approx(s.uvMin(s.trims[1]).x));
    CHECK(r[1] == doctest::Approx(s.uvMin(s.trims[1]).y));
    CHECK(r[3] == doctest::Approx(s.uvMax(s.trims[1]).y));
    CHECK(trimsMatching(s, json::array({2, s.trims[0].name})) == std::vector<size_t>{0, 2});
    CHECK_THROWS(trimsMatching(s, "nothing"));
}

TEST_CASE("trim layout: borders move for the whole strip, clamp, and trims split")
{
    Sheet s = stripLadder("T_Edit", 256, 256, std::vector<int>{128, 64, 64});
    s.mipLevel = 3;
    const std::vector<size_t> moved = moveBorder(s, 1, 128, 40, 144);
    CHECK(moved == std::vector<size_t>{0, 1});
    CHECK(s.trims[0].max.y == 144);
    CHECK(s.trims[1].min.y == 144);
    // Never thinner than a pixel: trim 1 now spans 144 to 192.
    moveBorder(s, 1, 144, 0, 400);
    CHECK(s.trims[1].min.y == 191);
    CHECK(moveBorder(s, 1, 77, 0, 80).empty());
    CHECK(snapToGrid(s, 101.0) == 104);

    REQUIRE(splitTrim(s, 0, 0, 96));
    REQUIRE(s.trims.size() == 4);
    CHECK(s.trims[0].max.x == 96);
    CHECK(s.trims[1].min.x == 96);
    CHECK(s.trims[1].name == s.trims[0].name + "_b");
    CHECK_FALSE(splitTrim(s, 0, 0, 200));
    // The two halves now share a vertical border that moves on its own.
    CHECK(moveBorder(s, 0, 96, 10, 128) == std::vector<size_t>{0, 1});
}

TEST_CASE("trim strips: along a line, tiling in U, capped, and over a mesh edge")
{
    Sheet s = stripLadder("T_Strip", 512, 512, std::vector<int>{256, 128, 64, 64});
    s.pxPerM = 512.0;
    s.trims[3].kind = uv::TrimKind::Unique;
    s.trims[3].min = {0, 448};
    s.trims[3].max = {64, 512};
    s.trims[3].tags = {"cap"};
    const double sheetCm = s.worldExtent().x;
    const std::vector<glm::dvec3> line = {{0, 0, 0}, {sheetCm, 0, 0}, {2.0 * sheetCm, 0, 0}};
    const std::vector<Station> st = stationsAlong(line, false, 25.0, StripOrient::Flat, 0.0);
    REQUIRE(st.size() == 3);
    CHECK(glm::length(st[0].normal - glm::dvec3(0, 1, 0)) < 1e-9);

    StripOptions o;
    o.trim = 1;
    geo::Mesh m = stripMesh(s, st, o);
    CHECK(m.numFaces() == 2);
    // U runs a sheet width per sheet width of length; V stays inside the trim.
    const glm::dvec2 end = uvAt(m, st[2].across[1]), start = uvAt(m, st[0].across[0]);
    CHECK(end.x - start.x == doctest::Approx(2.0));
    CHECK(start.y >= s.uvMin(s.trims[1]).y);
    CHECK(end.y <= s.uvMax(s.trims[1]).y);
    CHECK(m.faceAttrs().get<int32_t>(kFaceTrim)[0] == 2);

    REQUIRE(bestStripTrim(s, 25.0, "") == 1);
    o.cap = capTrim(s);
    REQUIRE(o.cap == 3);
    geo::Mesh capped = stripMesh(s, st, o);
    CHECK(capped.numFaces() == 4);
    const glm::dvec2 head = uvAt(capped, st[0].across[0]);
    CHECK(head.x == doctest::Approx(s.uvMin(s.trims[3]).x));

    // A box's top front edge: the decal wraps over it, its middle on the edge.
    const glm::dvec3 pts[] = {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}, {0, 10, -10}, {10, 10, -10}};
    const geo::Index sizes[] = {4, 4};
    const geo::Index idx[] = {0, 1, 2, 3, 3, 2, 5, 4};
    const geo::Mesh box = geo::Mesh::fromPolygons(pts, sizes, idx);
    const geo::Index edge = box.findEdge(2, 3);
    REQUIRE(edge != geo::kInvalid);
    const std::vector<EdgeChain> chains = edgeChains(box, std::vector<geo::Index>{edge});
    REQUIRE(chains.size() == 1);
    const std::vector<Station> over = stationsOverEdges(box, chains[0], 2.0, 0.0);
    REQUIRE(over.size() == 2);
    for (const Station& x : over)
    {
        CHECK(x.across[1].y == doctest::Approx(10.0));
        CHECK(x.across[1].z == doctest::Approx(0.0));
        // One side down the front, the other back along the top.
        const bool frontFirst = x.across[0].y < 10.0 - 1e-6;
        CHECK((frontFirst ? x.across[2].z : x.across[0].z) == doctest::Approx(-1.0));
        CHECK((frontFirst ? x.across[0].y : x.across[2].y) == doctest::Approx(9.0));
    }

    // Trim Cut's slab is closed.
    StripOptions cut;
    cut.trim = 2;
    cut.cutDepth = 0.5;
    const geo::Mesh slab = stripMesh(s, over, cut);
    for (geo::Index e = 0; e < slab.edgeCapacity(); ++e)
        if (slab.edgeValid(e))
            CHECK(slab.edgeFaceCount(e) == 2);
}

TEST_CASE("trim commands: a sheet textured with a smart material on some trims")
{
    loadTrims();
    const std::filesystem::path file = scratch("texture") / "T_Tex.lilatrim";
    ops::execute("trimSheetCreate", {{"preset", "ladder"}, {"width", 64}, {"file", file.generic_string()}, {"bake", false}});
    ops::execute("trimSheetEdit", {{"values", {{"supersampling", 1}}}});
    const json r = ops::execute("trimSheetTexture", {{"material", "Trim Hull Paint"}, {"trims", json::array({0})}});
    CHECK(r["set"] == "TS_TrimLadder");
    CHECK(r["project"] == "T_TrimLadder");
    const json stack = ops::execute("texLayerStack", {{"args", {"TS_TrimLadder"}}});
    REQUIRE(stack["layers"].size() == 2);
    CHECK(stack["layers"][0]["name"] == "Sheet Bake");
    REQUIRE(stack["layers"][1]["masks"].size() == 1);
    CHECK(stack["layers"][1]["masks"][0]["type"] == "trimMask");

    const json made = ops::execute("texEvaluate", {{"args", {"TS_TrimLadder"}},
                                                   {"channels", json::array({"baseColor"})},
                                                   {"folder", (scratch("texture") / "eval").generic_string()},
                                                   {"backend", "cpu"}});
    REQUIRE(made["files"].size() == 1);
    const lila::image::Image img = lila::image::read(made["files"][0].get<std::string>());
    REQUIRE(img.width == 64);
    // Strip 0 is the top half (32 px of 64): hull paint there, the set's plain grey below.
    const auto at = [&](int x, int y) { return glm::vec3(img.rgba[(size_t(y) * 64 + size_t(x)) * 4], img.rgba[(size_t(y) * 64 + size_t(x)) * 4 + 1], 0.0f); };
    CHECK(glm::length(at(32, 10) - at(32, 50)) > 0.02f);
}

TEST_CASE("trim commands: along a curve, a decal over edges and a trim cut")
{
    loadTrims();
    const std::filesystem::path file = scratch("strips") / "T_Strips.lilatrim";
    ops::execute("trimSheetCreate", {{"preset", "industrialMetal"}, {"width", 256}, {"file", file.generic_string()}, {"bake", false}});
    scene::Graph& g = scene::Graph::current();
    auto meshOf = [&](const std::string& name) {
        scene::Node* n = g.find(name);
        REQUIRE(n);
        return std::get<scene::MeshPtr>(g.value({n->id(), scene::MeshNode::kOutMesh, -1}));
    };

    const std::string c = ops::execute("curve", {{"points", {{0, 0, 0}, {300, 0, 0}, {300, 0, 300}}}, {"type", "poly"}}).get<std::string>();
    const json along = ops::execute("trimAlongCurve", {{"args", {c}}, {"width", 12.5}});
    REQUIRE(along.size() == 1);
    CHECK(along[0]["length"].get<double>() == doctest::Approx(600.0));
    CHECK(along[0]["cap"].get<int>() >= 0);
    CHECK(meshOf(along[0]["mesh"].get<std::string>())->numFaces() >= 4);

    const json box = ops::execute("createMesh", {{"points", {{0, 0, 0}, {100, 0, 0}, {100, 100, 0}, {0, 100, 0}, {0, 0, -100}, {100, 0, -100}, {100, 100, -100}, {0, 100, -100}}},
                                                 {"faces", {{0, 1, 2, 3}, {5, 4, 7, 6}, {3, 2, 6, 7}, {1, 0, 4, 5}, {1, 5, 6, 2}, {4, 0, 3, 7}}},
                                                 {"name", "Block"}});
    const std::string block = box.is_string() ? box.get<std::string>() : box.at(0).get<std::string>();
    const geo::Index edge = meshOf(block)->findEdge(2, 3);
    REQUIRE(edge != geo::kInvalid);
    scene::components::setMode(scene::ComponentMode::Edge);
    scene::components::select(g.find(block)->id(), scene::ComponentMode::Edge, {edge});
    const json decal = ops::execute("trimDecal", {{"width", 4.0}});
    REQUIRE(decal.size() == 1);
    CHECK(meshOf(decal[0]["decal"].get<std::string>())->numFaces() == 2);

    const size_t before = meshOf(block)->numFaces();
    const json cut = ops::execute("trimDecal", {{"width", 4.0}, {"cut", true}, {"depth", 1.0}});
    REQUIRE(cut.size() == 1);
    CHECK(cut[0]["cut"].get<bool>());
    CHECK(meshOf(block)->numFaces() > before);
}

TEST_CASE("trim commands: the test blockout is auto trimmed onto the sheet")
{
    loadTrims();
    const std::filesystem::path file = scratch("test") / "T_Try.lilatrim";
    ops::execute("trimSheetCreate", {{"preset", "woodTrims"}, {"width", 256}, {"file", file.generic_string()}, {"bake", false}});
    const json r = ops::execute("trimSheetTest", json::object());
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.find(r["mesh"].get<std::string>());
    REQUIRE(n);
    const scene::MeshPtr m = std::get<scene::MeshPtr>(g.value({n->id(), scene::MeshNode::kOutMesh, -1}));
    CHECK(m->numFaces() > 48);
    std::span<const int32_t> trim = m->faceAttrs().get<int32_t>(kFaceTrim);
    REQUIRE(!trim.empty());
    for (geo::Index f = 0; f < m->faceCapacity(); ++f)
        if (m->faceValid(f))
            CHECK(trim[f] > 0);
}

TEST_CASE("hotspot: a wall taller than every strip is cut into courses at true scale")
{
    const Sheet s = preset("woodTrims");
    const std::vector<uv::HotspotTrim> trims = hotspotTrims(s);
    double tallest = 0.0;
    for (const uv::HotspotTrim& t : trims)
        tallest = std::max(tallest, t.worldSize.y);
    REQUIRE(tallest == doctest::Approx(50.0));

    geo::Mesh m = wall(400.0, 250.0);
    const uv::HotspotResult r = uv::hotspot(m, {}, trims, "uv0");
    CHECK(r.sliced == 1);
    CHECK(m.numFaces() == 5);
    REQUIRE(r.islands.size() == 5);
    for (const uv::HotspotIsland& is : r.islands)
    {
        CHECK_FALSE(is.poor);
        CHECK(is.measure.height == doctest::Approx(50.0).epsilon(0.01));
        CHECK(is.sizeError < 0.1);
    }

    uv::HotspotOptions keep;
    keep.sliceOversized = false;
    geo::Mesh whole = wall(400.0, 250.0);
    const uv::HotspotResult w = uv::hotspot(whole, {}, trims, "uv0", keep);
    CHECK(w.sliced == 0);
    CHECK(whole.numFaces() == 1);
    CHECK(w.islands[0].poor);
}

TEST_CASE("hotspot: alt groups only swap trims of the same size")
{
    const Sheet s = preset("hotspotGrid");
    const std::vector<uv::HotspotTrim> trims = hotspotTrims(s);
    // A 40 x 300 cm pillar face: an 8:1 panel fits; the 50 cm squares in its row's group must never be picked.
    for (uint32_t seed = 0; seed < 12; ++seed)
    {
        uv::HotspotOptions o;
        o.seed = seed;
        geo::Mesh m = wall(40.0, 300.0);
        const uv::HotspotResult r = uv::hotspot(m, {}, trims, "uv0", o);
        REQUIRE(r.islands.size() == 1);
        const Trim& t = s.trims[size_t(r.islands[0].trim)];
        CAPTURE(seed);
        CHECK(std::max(t.sizePx().x, t.sizePx().y) >= 4 * std::min(t.sizePx().x, t.sizePx().y));
        CHECK_FALSE(r.islands[0].poor);
    }
}

TEST_CASE("trim: the sheet's own high meshes bake on top of the generated relief")
{
    Sheet s = stripLadder("T_Motif", 128, 128, std::vector<int>{64, 64});
    s.pxPerM = 128.0;
    s.supersampling = 1;
    s.maps = {"normal"};
    const std::filesystem::path file = scratch("motif") / "T_Motif.lilatrim";
    save(s, file);
    SheetBakeOptions o;
    o.backend = "cpu";
    bakeSheet(s, file, o);
    const lila::image::Image plain = lila::image::read(mapFile(s, file, "normal"));

    // A 20 cm stud on the first strip, in sheet space: x right, z down the texture, y up off it.
    geo::Mesh stud;
    {
        const glm::dvec3 lo{30.0, 0.0, 10.0}, hi{50.0, 20.0, 30.0};
        const glm::dvec3 pts[] = {{lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z},
                                  {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z}};
        const geo::Index sizes[] = {4, 4, 4, 4, 4, 4};
        const geo::Index idx[] = {0, 1, 2, 3, 5, 4, 7, 6, 3, 2, 6, 7, 1, 0, 4, 5, 1, 5, 6, 2, 4, 0, 3, 7};
        stud = geo::Mesh::fromPolygons(pts, sizes, idx);
    }
    o.high = {stud};
    bakeSheet(s, file, o);
    const lila::image::Image withStud = lila::image::read(mapFile(s, file, "normal"));
    REQUIRE(withStud.width == plain.width);
    // The stud's sides tilt the normals around its outline (texels 38 to 64 at 128 px per metre); the rest stays.
    double inside = 0.0, outside = 0.0;
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x)
        {
            const size_t i = (size_t(y) * 128 + size_t(x)) * 4;
            const double d = std::abs(withStud.rgba[i] - plain.rgba[i]) + std::abs(withStud.rgba[i + 1] - plain.rgba[i + 1]);
            ((x >= 36 && x <= 66 && y >= 10 && y <= 42) ? inside : outside) += d;
        }
    CHECK(inside > 5.0);
    CHECK(outside < inside * 0.2);
}

TEST_CASE("trim commands: a face as tall as the strip stays upright on it")
{
    loadTrims();
    const std::filesystem::path file = scratch("upright") / "T_Up.lilatrim";
    ops::execute("trimSheetCreate", {{"preset", "ladder"}, {"width", 1024}, {"file", file.generic_string()}, {"bake", false}});
    // Strip256 is 50 cm tall at 512 px per metre; the face is 42 cm wide and 50 cm tall, like a wall band's end.
    const json made = ops::execute("createMesh", {{"points", {{0, 0, 0}, {42, 0, 0}, {42, 50, 0}, {0, 50, 0}}}, {"faces", {{0, 1, 2, 3}}}, {"name", "End"}});
    const std::string end = made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
    scene::components::setMode(scene::ComponentMode::Object);
    ops::execute("select", {{"args", {end}}});
    ops::execute("trimApply", {{"trim", "Strip256"}});
    scene::Graph& g = scene::Graph::current();
    const geo::Mesh m = *std::get<scene::MeshPtr>(g.value({g.find(end)->id(), scene::MeshNode::kOutMesh, -1}));
    const glm::dvec2 bottom = uvAt(m, {0, 0, 0}), top = uvAt(m, {0, 50, 0}), right = uvAt(m, {42, 0, 0});
    CHECK(std::abs(top.x - bottom.x) < 1e-6);
    CHECK(top.y > bottom.y);
    // Too short to stretch to a whole sheet: it keeps its scale and starts on the sheet's edge.
    CHECK(bottom.x == doctest::Approx(0.0));
    CHECK(right.x == doctest::Approx(42.0 / 200.0).epsilon(0.05));
}

TEST_CASE("trim commands: a band's sides run on round its corners, with one seam at the back")
{
    loadTrims();
    const std::filesystem::path file = scratch("band") / "T_Band.lilatrim";
    ops::execute("trimSheetCreate", {{"preset", "ladder"}, {"width", 1024}, {"file", file.generic_string()}, {"bake", false}});
    // The four sides of a 400 x 50 x 40 band, no caps: front, right, back, left.
    const json made = ops::execute("createMesh", {{"points", {{-200, 0, 20}, {200, 0, 20}, {200, 0, -20}, {-200, 0, -20},
                                                              {-200, 50, 20}, {200, 50, 20}, {200, 50, -20}, {-200, 50, -20}}},
                                                   {"faces", {{0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}}},
                                                   {"name", "Band"}});
    const std::string band = made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
    scene::components::setMode(scene::ComponentMode::Object);
    ops::execute("select", {{"args", {band}}});
    auto uvOf = [&](geo::Index face, geo::Index point) {
        scene::Graph& g = scene::Graph::current();
        const geo::Mesh m = *std::get<scene::MeshPtr>(g.value({g.find(band)->id(), scene::MeshNode::kOutMesh, -1}));
        std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
        glm::vec2 out(0.0f);
        m.forEachFaceCorner(face, [&](geo::Index c) {
            if (m.cornerPoint(c) == point)
                out = uv[c];
        });
        return out;
    };
    // Vertical edge k (points k and k + 4) is shared by face k - 1 and face k; true where both faces show the same texels.
    auto joined = [&](geo::Index k) {
        auto same = [](glm::vec2 a, glm::vec2 b) {
            const float du = a.x - b.x;
            return std::abs(du - std::round(du)) < 1e-4f && std::abs(a.y - b.y) < 1e-5f;
        };
        const geo::Index before = (k + 3) % 4;
        return same(uvOf(before, k), uvOf(k, k)) && same(uvOf(before, k + 4), uvOf(k, k + 4));
    };
    auto continuous = [&] {
        int n = 0;
        for (geo::Index k = 0; k < 4; ++k)
            n += joined(k) ? 1 : 0;
        return n;
    };

    ops::execute("trimApply", {{"trim", "Strip256"}});
    // The front spans whole sheets from the sheet's edge, so copies tile; the loop round the back keeps one seam there.
    CHECK(continuous() == 3);
    CHECK(joined(0));
    CHECK(joined(1));
    CHECK(uvOf(0, 0).x == doctest::Approx(0.0));
    CHECK(uvOf(0, 1).x == doctest::Approx(2.0));

    // trimAuto picks the strip per island, then chains the ones on the same strip the same way.
    ops::execute("trimAuto", {{"seed", 3}});
    CHECK(continuous() == 3);
}

TEST_CASE("trim commands: copies of a piece laid end to end meet on the same texels")
{
    loadTrims();
    const std::filesystem::path file = scratch("pieces") / "T_Pieces.lilatrim";
    ops::execute("trimSheetCreate", {{"preset", "ladder"}, {"width", 1024}, {"file", file.generic_string()}, {"bake", false}});
    // A 380 cm wall on the 200 cm sheet: about 1.9 sheets, stretched to 2 so a copy's start continues its end.
    const json r = ops::execute("createMesh", {{"points", {{0, 0, 0}, {380, 0, 0}, {380, 50, 0}, {0, 50, 0}}}, {"faces", {{0, 1, 2, 3}}}, {"name", "Piece"}});
    const std::string n = r.is_string() ? r.get<std::string>() : r.at(0).get<std::string>();
    scene::components::setMode(scene::ComponentMode::Object);
    ops::execute("select", {{"args", {n}}});
    ops::execute("trimApply", {{"trim", "Strip256"}});
    scene::Graph& g = scene::Graph::current();
    const geo::Mesh m = *std::get<scene::MeshPtr>(g.value({g.find(n)->id(), scene::MeshNode::kOutMesh, -1}));
    const glm::dvec2 start = uvAt(m, {0, 0, 0}), end = uvAt(m, {380, 0, 0});
    CHECK(start.x == doctest::Approx(0.0));
    CHECK(end.x == doctest::Approx(2.0));
    CHECK(start.y == doctest::Approx(end.y));
}

TEST_CASE("trim commands: a blank sheet, empty or laid out as strips")
{
    loadTrims();
    const std::filesystem::path empty = scratch("blank") / "T_Empty.lilatrim";
    const json made = ops::execute("trimSheetCreate", {{"file", empty.generic_string()}, {"width", 512}, {"height", 256}, {"pxPerM", 64.0}});
    CHECK(made["trims"].empty());
    CHECK(made["size"] == json::array({512, 256}));
    CHECK_FALSE(made["baked"].get<bool>());
    CHECK(made["preset"] == "");
    ops::execute("trimSheetEdit", {{"add", {{"name", "Band"}, {"min", {0, 0}}, {"max", {512, 128}}, {"profile", {{"style", "glyphs"}}}}}});
    CHECK(ops::execute("trimSheetInfo", json::object())["trims"][0]["worldSizeCm"][1].get<double>() == doctest::Approx(200.0));

    const std::filesystem::path strips = scratch("blank") / "T_Strips.lilatrim";
    const json laid = ops::execute("trimSheetCreate", {{"file", strips.generic_string()}, {"width", 256}, {"strips", {128, 64, 64}}, {"bake", false}});
    REQUIRE(laid["trims"].size() == 3);
    CHECK(laid["trims"][2]["min"] == json::array({0, 192}));
    CHECK_THROWS(ops::execute("trimSheetCreate", {{"file", (scratch("blank") / "T_Bad.lilatrim").generic_string()}, {"strips", {"wide"}}}));
}

TEST_CASE("trim styles: carved and patterned strips tile across U")
{
    for (const char* style : {"glyphs", "meander", "fluting", "chipped", "hazard", "conduit"})
    {
        CAPTURE(style);
        Sheet s = stripLadder(std::string("T_") + style, 128, 128, std::vector<int>{64, 64});
        s.pxPerM = 128.0;
        s.supersampling = 1;
        s.maps = {"normal"};
        s.trims[0].profile.style = *styleFromName(style);
        s.trims[0].profile.bevelPx = 4.0;
        s.trims[0].profile.detailPx = 6.0;
        s.trims[0].profile.spacingPx = 32.0;
        const Generated g = generate(s);
        CHECK(g.high.numFaces() > 200);
        const std::filesystem::path file = scratch(std::string("style_") + style) / (s.name + ".lilatrim");
        save(s, file);
        SheetBakeOptions o;
        o.backend = "cpu";
        const json r = bakeSheet(s, file, o);
        CHECK(r["seamError"].get<double>() < 0.03);
    }
    // Glyph bands are seeded: another seed carves other glyphs.
    Sheet a = stripLadder("T_Glyph", 256, 64, std::vector<int>{64});
    a.trims[0].profile.style = Style::Glyphs;
    Sheet b = a;
    b.trims[0].profile.seed = 7;
    const geo::Mesh ha = generate(a).high, hb = generate(b).high;
    REQUIRE(ha.numPoints() == hb.numPoints());
    bool differs = false;
    for (geo::Index p = 0; p < ha.pointCapacity() && !differs; ++p)
        differs = ha.position(p) != hb.position(p);
    CHECK(differs);
}

namespace
{
std::string made(const json& r) { return r.is_string() ? r.get<std::string>() : r.at(0).get<std::string>(); }

geo::Mesh meshOf(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.find(name);
    REQUIRE(n);
    return *std::get<scene::MeshPtr>(g.value({n->id(), scene::MeshNode::kOutMesh, -1}));
}

/** Faces of the mesh per material slot name. */
std::map<std::string, uint32_t> slotsOf(const std::string& name)
{
    std::map<std::string, uint32_t> out;
    for (const auto& [slot, count] : scene::materials::slotUsage(meshOf(name)))
        if (count)
            out[slot] = count;
    return out;
}
} // namespace

TEST_CASE("trim palettes: one layout, several material families, each its own set and Unreal instance")
{
    loadTrims();
    const std::filesystem::path file = scratch("palettes") / "T_Pal.lilatrim";
    ops::execute("trimSheetCreate", {{"file", file.generic_string()}, {"name", "T_Pal"}, {"width", 64}, {"strips", {32, 16, 16}}, {"pxPerM", 64.0}, {"bake", false}});
    ops::execute("trimSheetEdit", {{"values", {{"supersampling", 1}}}});

    json list = ops::execute("trimPaletteAdd", {{"args", {"Rust"}}, {"material", "Trim Rusty Iron"}, {"vectors", {{"Tint", {0.4, 0.2, 0.1}}}}, {"scalars", {{"EdgeWear", 2.0}}}});
    REQUIRE(list.size() == 2);
    CHECK(list[0]["name"] == "Default");
    CHECK(list[1]["slot"] == "M_Pal_Rust");
    CHECK(list[1]["instance"] == "MI_Pal_Rust");
    CHECK(scene::materials::find(scene::Graph::current(), "M_Pal_Rust"));
    CHECK_THROWS(ops::execute("trimPaletteAdd", {{"args", {"Rust"}}}));
    CHECK_THROWS(ops::execute("trimPaletteAdd", {{"args", {"bad name"}}}));
    ops::execute("trimPaletteAdd", {{"args", {"Steel"}}, {"copyFrom", "Rust"}});
    list = ops::execute("trimPaletteEdit", {{"args", {"Steel"}}, {"scalars", {{"EdgeWear", nullptr}, {"Metallic", 1.0}}}});
    CHECK_FALSE(list[2]["scalars"].contains("EdgeWear"));
    CHECK(list[2]["material"] == "Trim Rusty Iron");
    CHECK_THROWS(ops::execute("trimPaletteEdit", {{"args", {"Steel"}}, {"vectors", {{"Tint", 3}}}}));

    // Faces mapped onto a palette keep it through a shift, move to another palette, and undo.
    const std::string wallNode = made(ops::execute("createMesh", {{"points", {{0, 0, 0}, {300, 0, 0}, {300, 25, 0}, {0, 25, 0}}}, {"faces", {{0, 1, 2, 3}}}, {"name", "PalWall"}}));
    scene::components::setMode(scene::ComponentMode::Object);
    ops::execute("select", {{"args", {wallNode}}});
    CHECK_THROWS(ops::execute("trimAuto", {{"palette", "Copper"}}));
    ops::execute("trimAuto", {{"palette", "Rust"}});
    CHECK(slotsOf(wallNode) == std::map<std::string, uint32_t>{{"M_Pal_Rust", 1}});
    ops::execute("trimShift", {{"steps", 1}});
    CHECK(slotsOf(wallNode) == std::map<std::string, uint32_t>{{"M_Pal_Rust", 1}});
    ops::execute("trimPaletteAssign", {{"palette", "Default"}});
    CHECK(slotsOf(wallNode) == std::map<std::string, uint32_t>{{"M_Pal", 1}});
    ops::execute("undo", json::object());
    CHECK(slotsOf(wallNode) == std::map<std::string, uint32_t>{{"M_Pal_Rust", 1}});
    CHECK(ops::execute("trimPaletteAssign", {{"palette", "Steel"}, {"fromPalette", "Default"}})["faces"] == 0);

    // Each palette textures its own set over the shared bake.
    const json tex = ops::execute("trimSheetTexture", {{"palette", "Rust"}});
    CHECK(tex["set"] == "TS_Pal_Rust");
    CHECK(tex["material"] == "Trim Rusty Iron");
    list = ops::execute("trimPaletteList", json::object());
    CHECK(list[1]["textured"].get<bool>());
    CHECK_FALSE(list[0]["textured"].get<bool>());

    // Unreal gets one instance per palette on the shared master, each on its own slot.
    const json plan = ops::execute("trimSheetSend", {{"dryRun", true}});
    const json& instances = plan["request"]["instances"];
    REQUIRE(instances.size() == 3);
    CHECK(instances[0]["name"] == "MI_Pal");
    CHECK(instances[1]["name"] == "MI_Pal_Rust");
    CHECK(instances[1]["slot"] == "M_Pal_Rust");
    CHECK(instances[1]["vectors"]["Tint"][0].get<double>() == doctest::Approx(0.4));
    CHECK(instances[1]["switches"]["UsePaintedMaps"].get<bool>());
    CHECK_FALSE(instances[0]["switches"]["UsePaintedMaps"].get<bool>());
    CHECK(instances[1]["textures"]["SheetNormalMap"] != instances[0]["textures"]["SheetNormalMap"]);
    CHECK(instances[0]["textures"]["SheetAOMap"] == instances[2]["textures"]["SheetAOMap"]);
    CHECK(instances[2]["scalars"]["Metallic"].get<double>() == doctest::Approx(1.0));

    const json removed = ops::execute("trimPaletteRemove", {{"args", {"Rust"}}});
    CHECK(removed["palettes"].size() == 2);
    REQUIRE(removed["stillUsedBy"].size() == 1);
    CHECK(removed["stillUsedBy"][0]["mesh"] == wallNode);
}

TEST_CASE("trim send: meshes not yet in Unreal are sent first or listed, whatever the order")
{
    loadTrims();
    const std::filesystem::path file = scratch("send") / "T_Send.lilatrim";
    ops::execute("trimSheetCreate", {{"file", file.generic_string()}, {"name", "T_Send"}, {"width", 64}, {"strips", {32, 32}}, {"bake", false}});
    ops::execute("trimSheetEdit", {{"values", {{"supersampling", 1}, {"maps", {"normal"}}}}});
    CHECK_THROWS_WITH(ops::execute("trimSheetSend", {{"dryRun", true}}), doctest::Contains("not baked"));
    ops::execute("trimSheetBake", {{"backend", "cpu"}});

    const std::string a = made(ops::execute("createMesh", {{"points", {{0, 0, 0}, {100, 0, 0}, {100, 6, 0}, {0, 6, 0}}}, {"faces", {{0, 1, 2, 3}}}, {"name", "SendA"}}));
    const std::string b = made(ops::execute("createMesh", {{"points", {{0, 0, 0}, {100, 0, 0}, {100, 6, 0}, {0, 6, 0}}}, {"faces", {{0, 1, 2, 3}}}, {"name", "SendB"}}));
    scene::components::setMode(scene::ComponentMode::Object);
    ops::execute("select", {{"args", {a, b}}});
    ops::execute("trimAuto", json::object());
    ops::execute("unrealMap", {{"args", {a}}, {"contentPath", "/Game/Props/SM_SendA"}});

    const json plan = ops::execute("trimSheetSend", {{"dryRun", true}});
    CHECK(plan["mapped"].size() == 1);
    REQUIRE(plan["meshes"]["newlyMapped"].size() == 1);
    CHECK(plan["meshes"]["newlyMapped"][0]["node"] == b);
    CHECK(plan["meshes"]["newlyMapped"][0]["contentPath"] == "/Game/Lila/Trims/T_Send/Meshes/SM_SendB");
    CHECK(plan["request"]["meshes"] == json::array({"/Game/Props/SM_SendA", "/Game/Lila/Trims/T_Send/Meshes/SM_SendB"}));
    CHECK(plan["meshes"]["sent"].size() == 2);
    CHECK_THROWS_WITH(ops::execute("trimSheetSend", {{"meshes", "mapped"}}), doctest::Contains("SendB"));
}

namespace
{
json checkOf(const std::string& node, json flags = json::object())
{
    ops::execute("select", {{"args", {node}}});
    return ops::execute("trimCheck", flags)["meshes"][node];
}

bool hasIssue(const json& mesh, const std::string& issue) { return mesh["issues"].contains(issue); }
} // namespace

TEST_CASE("trim check: density, alignment, orientation, stretch, readability and repetition")
{
    loadTrims();
    const std::filesystem::path file = scratch("check") / "T_Check.lilatrim";
    ops::execute("trimSheetCreate", {{"file", file.generic_string()}, {"name", "T_Check"}, {"width", 1024}, {"strips", {256, 128, 128, 512}}, {"pxPerM", 512.0}, {"bake", false}});
    scene::components::setMode(scene::ComponentMode::Object);

    // Strip0 is 50 cm tall: a 50 cm band maps onto it at the sheet's density, upright, inside it.
    const std::string band = made(ops::execute("createMesh", {{"points", {{0, 0, 0}, {400, 0, 0}, {400, 50, 0}, {0, 50, 0}}}, {"faces", {{0, 1, 2, 3}}}, {"name", "CheckBand"}}));
    ops::execute("select", {{"args", {band}}});
    ops::execute("trimApply", {{"trim", 0}});
    json c = checkOf(band, {{"distance", 500.0}, {"all", true}});
    REQUIRE(c["islands"] == 1);
    const json& is = c["details"][0];
    CHECK(c["ok"].get<bool>());
    CHECK(is["trim"] == "Strip0");
    CHECK(is["densityError"].get<double>() == doctest::Approx(0.0).epsilon(0.05));
    CHECK(is["quarterTurns"] == 0);
    CHECK(is["trimsCrossed"] == 1);
    // Stretched along U by at most a few percent to span whole sheets.
    CHECK(is["stretch"].get<double>() < 0.03);
    CHECK_FALSE(is["mirrored"].get<bool>());
    CHECK(is["cleanToMip"].get<int>() >= 3);
    // An 8 px bevel at 512 px per metre is 1.6 cm: about 3 px at 5 m on a 1080p screen, under 1 px at 30 m.
    CHECK(is["featurePx"].get<double>() == doctest::Approx(2.9).epsilon(0.1));
    CHECK(hasIssue(checkOf(band, {{"distance", 3000.0}}), "unreadable"));
    CHECK(hasIssue(checkOf(band, {{"distance", 500.0}, {"targetPxPerM", 256.0}}), "density"));

    // Pulled across the strip: stretched, off its rectangle and over the next strip.
    ops::execute("uvTransform", {{"scale", {1.0, 1.6}}});
    c = checkOf(band, {{"distance", 500.0}});
    CHECK(hasIssue(c, "stretched"));
    CHECK(hasIssue(c, "outside"));
    CHECK(hasIssue(c, "crossesTrims"));
    ops::execute("undo", json::object());
    ops::execute("uvTransform", {{"scale", {-1.0, 1.0}}});
    CHECK(hasIssue(checkOf(band, {{"distance", 500.0}}), "mirrored"));
    ops::execute("undo", json::object());
    ops::execute("uvTransform", {{"rotate", 7.0}});
    CHECK(hasIssue(checkOf(band, {{"distance", 500.0}}), "skewed"));
    ops::execute("undo", json::object());
    CHECK(checkOf(band, {{"distance", 500.0}})["ok"].get<bool>());

    // Two faces round a corner run on along the strip, so neither repeats the other.
    const std::string corner = made(ops::execute("createMesh", {{"points", {{0, 0, 0}, {100, 0, 0}, {100, 50, 0}, {0, 50, 0}, {100, 0, -100}, {100, 50, -100}}},
                                                                {"faces", {{0, 1, 2, 3}, {1, 4, 5, 2}}},
                                                                {"name", "CheckCorner"}}));
    ops::execute("select", {{"args", {corner}}});
    ops::execute("trimApply", {{"trim", 0}});
    CHECK_FALSE(hasIssue(checkOf(corner, {{"distance", 500.0}}), "repeats"));

    // The same corner mapped a face at a time: both centred on the strip, so the same texels meet at the corner.
    auto eachFace = [&](json args) {
        scene::Graph& g = scene::Graph::current();
        for (geo::Index f : {0u, 1u})
        {
            scene::components::setMode(scene::ComponentMode::Face);
            scene::components::select(g.find(corner)->id(), scene::ComponentMode::Face, {f});
            ops::execute("trimApply", args);
        }
        scene::components::setMode(scene::ComponentMode::Object);
        ops::execute("select", {{"args", {corner}}});
    };
    eachFace({{"trim", 0}});
    c = checkOf(corner, {{"distance", 500.0}});
    CHECK(c["issues"]["repeats"] == 2);
    eachFace({{"trim", 0}, {"randomOffset", true}, {"seed", 4}});
    CHECK_FALSE(hasIssue(checkOf(corner, {{"distance", 500.0}}), "repeats"));

    // uvLayout counts trim overlap apart from real overlap; texel density leaves trims alone unless told.
    eachFace({{"trim", 0}});
    const json layout = ops::execute("uvLayout", json::object());
    CHECK(layout["overlappingFaces"] == 0);
    CHECK(layout["trimOverlapFaces"] == 2);
    CHECK_THROWS_WITH(ops::execute("uvTexelDensity", {{"density", 2.0}}), doctest::Contains("trim sheet"));
    ops::execute("uvTexelDensity", {{"density", 2.0}, {"trims", "rescale"}});
    const geo::Mesh rescaled = meshOf(corner);
    CHECK(rescaled.faceAttrs().get<int32_t>(kFaceTrim)[0] == 0);

    ops::execute("trimSheetEdit", {{"trim", 0}, {"values", {{"allowRotate", false}}}});
    ops::execute("select", {{"args", {band}}});
    ops::execute("uvTransform", {{"rotate", 90.0}});
    CHECK(hasIssue(checkOf(band, {{"distance", 500.0}}), "rotated"));
}

TEST_CASE("trim scale: a monumental and a human-scale sheet in one scene each map their own meshes at true size")
{
    loadTrims();
    const std::filesystem::path stone = scratch("scale") / "T_Whale.lilatrim", metal = scratch("scale") / "T_Human.lilatrim";
    ops::execute("trimSheetCreate", {{"preset", "monumentalStone"}, {"name", "T_Whale"}, {"width", 512}, {"file", stone.generic_string()}, {"bake", false}});
    ops::execute("trimSheetCreate", {{"preset", "industrialFrame"}, {"name", "T_Human"}, {"width", 512}, {"file", metal.generic_string()}, {"bake", false}});
    // Same world sizes at any resolution: the stone sheet is 64 m across, the metal one 4 m.
    CHECK(load(stone).worldExtent().x == doctest::Approx(6400.0));
    CHECK(load(metal).worldExtent().x == doctest::Approx(400.0));
    scene::components::setMode(scene::ComponentMode::Object);

    auto mapped = [&](const char* name, double w, double h, const std::filesystem::path& sheet, bool rotate = true) {
        const std::string n = made(ops::execute("createMesh", {{"points", {{0, 0, 0}, {w, 0, 0}, {w, h, 0}, {0, h, 0}}}, {"faces", {{0, 1, 2, 3}}}, {"name", name}}));
        ops::execute("select", {{"args", {n}}});
        const json r = ops::execute("trimAuto", {{"sheet", sheet.generic_string()}, {"randomOffset", false}, {"allowRotate", rotate}});
        REQUIRE(r["details"].size() == 1);
        return std::pair{n, r["details"][0]};
    };
    auto worldHeight = [](const std::filesystem::path& sheet, const json& detail) {
        const Sheet s = load(sheet);
        for (const Trim& t : s.trims)
            if (t.name == detail["trim"].get<std::string>())
                return s.worldSize(t).y;
        return 0.0;
    };

    // A 4 m course along a kilometre of wall: a 4 m band of the stone sheet, repeating, not stretched.
    const auto [course, courseTrim] = mapped("WhaleCourse", 100000.0, 400.0, stone);
    CHECK(worldHeight(stone, courseTrim) == doctest::Approx(400.0));
    CHECK(courseTrim["sizeError"].get<double>() < 0.05);
    CHECK_FALSE(courseTrim["poor"].get<bool>());
    // A 16 m tall pylon face goes to the 16 m fluting.
    const auto [pylon, pylonTrim] = mapped("WhalePylon", 3000.0, 1600.0, stone);
    CHECK(courseTrim["trim"] != pylonTrim["trim"]);
    CHECK(worldHeight(stone, pylonTrim) == doctest::Approx(1600.0));
    // A 25 cm rail at human scale on the metal sheet, beside them.
    const auto [rail, railTrim] = mapped("HumanRail", 300.0, 25.0, metal);
    CHECK(worldHeight(metal, railTrim) == doctest::Approx(25.0));
    CHECK(railTrim["sizeError"].get<double>() < 0.05);

    // Each keeps its own sheet's density: 8 px per metre for the stone at this width, 128 for the metal.
    for (const auto& [node, density] : {std::pair{course, 8.0}, std::pair{pylon, 8.0}, std::pair{rail, 128.0}})
    {
        CAPTURE(node);
        const json c = checkOf(node, {{"all", true}, {"distance", 2000.0}});
        CHECK(c["targetPxPerM"].get<double>() == doctest::Approx(density));
        CHECK_FALSE(hasIssue(c, "density"));
        CHECK_FALSE(hasIssue(c, "crossesTrims"));
        CHECK_FALSE(hasIssue(c, "outside"));
    }
    // The wrong scale is caught: kept upright, the human rail on the monumental sheet is a poor match.
    const auto [lost, lostTrim] = mapped("HumanOnStone", 300.0, 25.0, stone, false);
    CHECK(lostTrim["poor"].get<bool>());
}

TEST_CASE("trim live: the hotspot modifier re-maps when the mesh under it or its sheet changes")
{
    loadTrims();
    const std::filesystem::path file = scratch("live") / "T_Live.lilatrim";
    // Strip0 is 50 cm, Strip1 and Strip2 25 cm, Strip3 100 cm at 512 px per metre.
    ops::execute("trimSheetCreate", {{"file", file.generic_string()}, {"name", "T_Live"}, {"width", 1024}, {"strips", {256, 128, 128, 512}}, {"pxPerM", 512.0}, {"bake", false}});
    const std::string node = made(ops::execute("createMesh", {{"points", {{0, 0, 0}, {400, 0, 0}, {400, 25, 0}, {0, 25, 0}}}, {"faces", {{0, 1, 2, 3}}}, {"name", "LiveWall"}}));
    scene::components::setMode(scene::ComponentMode::Object);
    ops::execute("select", {{"args", {node}}});
    const json mods = ops::execute("trimLive", {{"randomOffset", false}});
    REQUIRE(mods.contains(node));
    auto trimOf = [&] { return meshOf(node).faceAttrs().get<int32_t>(kFaceTrim)[0]; };
    const int first = trimOf();
    CHECK((first == 2 || first == 3));
    CHECK(slotsOf(node) == std::map<std::string, uint32_t>{{"M_Live", 1}});

    // The blockout under the stack grows to a metre: the modifier moves it to the metre strip by itself.
    scene::Graph& g = scene::Graph::current();
    const scene::PlugRef base = scene::modifiers::editPlug(g, g.find(node)->id());
    g.setValue(base, scene::MeshPtr(std::make_shared<geo::Mesh>(wall(400.0, 100.0))));
    CHECK(trimOf() == 4);
    // Halving the sheet's density doubles every trim's world size: Strip0 is now the metre one.
    ops::execute("trimSheetEdit", {{"values", {{"pxPerM", 256.0}}}});
    CHECK(trimOf() == 1);
}

TEST_CASE("trim decals: a unique inscription baked with an opacity mask, placed, and planned as an Unreal deferred decal")
{
    loadTrims();
    const std::filesystem::path file = scratch("decals") / "Words.lilatrim";
    const json r = ops::execute("trimDecalCreate", {{"args", {"Words"}}, {"kind", "inscription"}, {"width", 200.0}, {"height", 50.0}, {"resolution", 128},
                                                   {"file", file.generic_string()}, {"backend", "cpu"}});
    CHECK(r["baked"].get<bool>());
    const Sheet s = load(file);
    CHECK(s.decal);
    CHECK(s.height == 32);
    CHECK(s.pxPerM == doctest::Approx(64.0));
    const lila::image::Image opacity = lila::image::read(mapFile(s, file, "opacity"));
    REQUIRE(opacity.width == 128);
    double edge = 0.0, inside = 0.0;
    for (int y = 0; y < opacity.height; ++y)
        for (int x = 0; x < opacity.width; ++x)
        {
            const float v = opacity.rgba[(size_t(y) * 128 + size_t(x)) * 4];
            (x == 0 || y == 0 || x == 127 || y == 31 ? edge : inside) = std::max(x == 0 || y == 0 || x == 127 || y == 31 ? edge : inside, double(v));
        }
    // The glyph strokes show, the box around them doesn't.
    CHECK(inside > 0.5);
    CHECK(edge < 0.05);
    CHECK(std::filesystem::exists(mapFile(s, file, "baseColor")));
    CHECK_THROWS(ops::execute("trimDecalCreate", {{"args", {"Words"}}, {"file", file.generic_string()}, {"bake", false}}));

    const std::string node = ops::execute("trimDecalPlace", {{"decal", file.generic_string()}, {"position", {100, 200, 300}}, {"normal", {0, 0, 1}}, {"depth", 15.0}}).get<std::string>();
    CHECK(slotsOf(node) == std::map<std::string, uint32_t>{{"M_WordsDecal", 1}});
    const json plan = ops::execute("trimDecalSend", {{"dryRun", true}});
    REQUIRE(plan["request"]["decals"].size() == 1);
    const json& d = plan["request"]["decals"][0];
    CHECK(d["size"][0].get<double>() == doctest::Approx(15.0));
    CHECK(d["size"][1].get<double>() == doctest::Approx(200.0));
    CHECK(d["size"][2].get<double>() == doctest::Approx(50.0));
    const math::Vector at = math::toUnreal(math::Vector(glm::dvec3(100.0, 200.0, 300.05)));
    const math::Vector into = math::toUnreal(math::Vector(glm::dvec3(0.0, 0.0, -1.0))), up = math::toUnreal(math::Vector(glm::dvec3(0.0, 1.0, 0.0)));
    CHECK(d["location"][0].get<double>() == doctest::Approx(at.x));
    CHECK(d["location"][1].get<double>() == doctest::Approx(at.y));
    CHECK(d["location"][2].get<double>() == doctest::Approx(at.z));
    CHECK(d["forward"][2].get<double>() == doctest::Approx(into.z));
    CHECK(d["up"][2].get<double>() == doctest::Approx(up.z));
    CHECK(d["instance"]["name"] == "MI_WordsDecal");
    for (const char* param : {"DecalBaseColorMap", "DecalNormalMap", "DecalOpacityMap"})
        CHECK(d["instance"]["textures"].contains(param));
    CHECK(plan["textures"].size() == 3);

    // Relief from a mesh of your own, laid on the decal plane: a 20 cm boss in the middle of a metre square.
    const json boss = ops::execute("createMesh", {{"points", {{40, 0, 40}, {60, 0, 40}, {60, 0, 60}, {40, 0, 60}, {40, 5, 40}, {60, 5, 40}, {60, 5, 60}, {40, 5, 60}}},
                                                  {"faces", {{0, 1, 2, 3}, {4, 7, 6, 5}, {0, 4, 5, 1}, {1, 5, 6, 2}, {2, 6, 7, 3}, {3, 7, 4, 0}}},
                                                  {"name", "CrestBoss"}});
    const std::filesystem::path crestFile = scratch("decals") / "Crest.lilatrim";
    ops::execute("trimDecalCreate", {{"args", {"Crest"}}, {"kind", "relief"}, {"high", {made(boss)}}, {"width", 100.0}, {"height", 100.0}, {"resolution", 64},
                                     {"file", crestFile.generic_string()}, {"backend", "cpu"}});
    const lila::image::Image crest = lila::image::read(mapFile(load(crestFile), crestFile, "opacity"));
    REQUIRE(crest.width == 64);
    CHECK(crest.rgba[(size_t(32) * 64 + 32) * 4] > 0.5f);
    CHECK(crest.rgba[(size_t(5) * 64 + 5) * 4] < 0.05f);
    CHECK_THROWS(ops::execute("trimDecalCreate", {{"args", {"Bare"}}, {"kind", "relief"}, {"bake", false}}));
}

TEST_CASE("trim reopen: a scene of trimmed meshes, palettes, a live modifier and a decal saves, reopens and re-exports")
{
    loadTrims();
    ops::execute("newScene", json::object());
    const std::filesystem::path dir = scratch("reopen");
    const std::filesystem::path file = dir / "T_Reo.lilatrim", decal = dir / "Mark.lilatrim", scenePath = dir / "level.lila";
    ops::execute("trimSheetCreate", {{"file", file.generic_string()}, {"name", "T_Reo"}, {"width", 64}, {"strips", {32, 16, 16}}, {"pxPerM", 64.0}, {"bake", false}});
    ops::execute("trimSheetEdit", {{"values", {{"supersampling", 1}}}});
    ops::execute("trimPaletteAdd", {{"args", {"Rust"}}, {"material", "Trim Rusty Iron"}, {"vectors", {{"Tint", {0.4, 0.2, 0.1}}}}});
    ops::execute("trimSheetTexture", json::object());
    ops::execute("trimSheetTexture", {{"palette", "Rust"}});
    const json before = load(file).toJson();

    scene::components::setMode(scene::ComponentMode::Object);
    auto quad = [&](const char* name, double w, double h) {
        return made(ops::execute("createMesh", {{"points", {{0, 0, 0}, {w, 0, 0}, {w, h, 0}, {0, h, 0}}}, {"faces", {{0, 1, 2, 3}}}, {"name", name}}));
    };
    const std::string plain = quad("ReoPlain", 300.0, 25.0), rusty = quad("ReoRust", 300.0, 50.0), live = quad("ReoLive", 200.0, 25.0);
    ops::execute("select", {{"args", {plain}}});
    ops::execute("trimAuto", {{"seed", 2}});
    ops::execute("select", {{"args", {rusty}}});
    ops::execute("trimAuto", {{"palette", "Rust"}});
    ops::execute("select", {{"args", {live}}});
    ops::execute("trimLive", {{"palette", "Rust"}});
    ops::execute("trimDecalCreate", {{"args", {"Mark"}}, {"width", 100.0}, {"height", 25.0}, {"resolution", 64}, {"file", decal.generic_string()}, {"backend", "cpu"}});
    const std::string placed = ops::execute("trimDecalPlace", {{"decal", decal.generic_string()}, {"position", {0, 100, 0}}, {"normal", {0, 0, 1}}}).get<std::string>();

    const geo::Mesh plainBefore = meshOf(plain), liveBefore = meshOf(live);
    const json planBefore = ops::execute("trimSheetSend", {{"dryRun", true}, {"sheet", file.generic_string()}});
    ops::execute("saveScene", {{"args", {scenePath.generic_string()}}});
    ops::execute("trimSheetCurrent", {{"args", {(dir / "T_Reo.lilatrim").generic_string()}}});
    ops::execute("openScene", {{"args", {scenePath.generic_string()}}});

    // Meshes keep their uvs, trims and palette slots; the live modifier comes back and maps the same.
    const geo::Mesh plainAfter = meshOf(plain);
    CHECK(plainAfter.faceAttrs().get<int32_t>(kFaceTrim)[0] == plainBefore.faceAttrs().get<int32_t>(kFaceTrim)[0]);
    CHECK(uvAt(plainAfter, {300, 25, 0}) == uvAt(plainBefore, {300, 25, 0}));
    CHECK(slotsOf(plain) == std::map<std::string, uint32_t>{{"M_Reo", 1}});
    CHECK(slotsOf(rusty) == std::map<std::string, uint32_t>{{"M_Reo_Rust", 1}});
    CHECK(slotsOf(live) == std::map<std::string, uint32_t>{{"M_Reo_Rust", 1}});
    CHECK(uvAt(meshOf(live), {200, 25, 0}) == uvAt(liveBefore, {200, 25, 0}));
    scene::Graph& g = scene::Graph::current();
    const std::vector<scene::NodeId> stack = scene::modifiers::stack(g, g.find(live)->id());
    REQUIRE(stack.size() == 1);
    CHECK(g.node(stack[0])->type().name == "trimHotspotModifier");
    CHECK(ops::execute("trimSheetCurrent", json::object()).get<std::string>() == file.generic_string());

    // The sheet file reopens unchanged, its palettes' texture sets are back, and it plans and exports the same.
    CHECK(load(file).toJson() == before);
    const json palettes = ops::execute("trimPaletteList", {{"sheet", file.generic_string()}});
    CHECK(palettes[0]["textured"].get<bool>());
    CHECK(palettes[1]["textured"].get<bool>());
    const json planAfter = ops::execute("trimSheetSend", {{"dryRun", true}, {"sheet", file.generic_string()}});
    CHECK(planAfter["request"]["instances"] == planBefore["request"]["instances"]);
    CHECK(planAfter["meshes"]["sent"].size() == 3);
    for (const char* format : {"rect", "svg", "json", "idmap"})
    {
        CAPTURE(format);
        const std::string out = ops::execute("trimSheetExport", {{"sheet", file.generic_string()}, {"format", format}}).get<std::string>();
        CHECK(std::filesystem::exists(out));
    }
    std::ifstream rect(ops::execute("trimSheetExport", {{"sheet", file.generic_string()}, {"format", "rect"}}).get<std::string>());
    CHECK(fromRect(std::string(std::istreambuf_iterator<char>(rect), {}), 64, 64).trims.size() == 3);
    const json rebaked = ops::execute("trimSheetBake", {{"sheet", file.generic_string()}, {"backend", "cpu"}});
    CHECK(rebaked["seamError"].get<double>() < 0.05);
    // With nothing selected trimCheck takes every mapped mesh: the plain, rusty and live ones.
    ops::execute("select", {{"clear", true}});
    CHECK(ops::execute("trimCheck", {{"distance", 300.0}})["meshes"].size() == 3);

    // The decal placement comes back too and plans the same decal actor.
    const json decals = ops::execute("trimDecalSend", {{"dryRun", true}});
    REQUIRE(decals["request"]["decals"].size() == 1);
    CHECK(decals["request"]["decals"][0]["name"] == g.find(placed)->name());
}

TEST_CASE("trim presets: stone, industrial and starfighter families build at their scale")
{
    const Sheet stone = preset("monumentalStone"), frame = preset("industrialFrame"), hangar = preset("starfighterFacility");
    CHECK(stone.worldExtent().x == doctest::Approx(6400.0));
    CHECK(hangar.worldExtent().x == doctest::Approx(800.0));
    CHECK(frame.worldExtent().x == doctest::Approx(400.0));
    auto hasStyle = [](const Sheet& s, Style st) { return std::ranges::any_of(s.trims, [&](const Trim& t) { return t.profile.style == st; }); };
    for (Style st : {Style::Glyphs, Style::Meander, Style::Channel, Style::Torus, Style::Ogee, Style::Cavetto, Style::Fluting, Style::Dentils,
                     Style::Steps, Style::Chipped})
        CHECK(hasStyle(stone, st));
    for (Style st : {Style::Rail, Style::Conduit, Style::Grating, Style::Hazard})
    {
        CHECK(hasStyle(frame, st));
        CHECK(hasStyle(hangar, st));
    }
    // Generated on first use, not shipped: a small copy builds its relief.
    CHECK(generate(preset("monumentalStone", 256)).high.numFaces() > 0);
}
