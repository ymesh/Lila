#include "ops/Command.h"
#include "ops/Undo.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "geo/ObjIO.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "scene/Graph.h"
#include "scene/MeshData.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "vcolor/Paint.h"
#include "vcolor/VertexColors.h"

#include <doctest/doctest.h>

#include <filesystem>

using namespace lila;
using geo::Index;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

scene::MeshPtr outMesh(const std::string& name)
{
    auto& g = scene::Graph::current();
    g.evaluate();
    return g.find(name)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}

/** Every valid corner of a set equals c. */
bool allCorners(const geo::Mesh& m, const std::string& set, const glm::vec4& c, float tol = 1e-4f)
{
    auto v = geo::colors::values(m, set);
    if (v.empty())
        return false;
    for (Index k = 0; k < m.cornerCapacity(); ++k)
        if (m.cornerValid(k) && glm::length(v[k] - c) > tol)
            return false;
    return true;
}

geo::Mesh paintedCube()
{
    geo::Mesh m = geo::makeCube({.width = 100, .height = 100, .depth = 100, .subdivX = 2, .subdivY = 2, .subdivZ = 2});
    auto a = geo::colors::add(m, "Base", glm::vec4(0.25f, 0.5f, 0.75f, 1.0f));
    for (Index c = 0; c < m.cornerCapacity(); ++c)
        a[c].a = float(m.cornerPoint(c) % 5) / 4.0f;
    geo::colors::add(m, "Mask", glm::vec4(0.0f));
    geo::colors::setCurrent(m, "Mask");
    geo::colors::setHalf(m, "Mask", true);
    return m;
}

/** A baked primitive at 1 m (radius 50), the size these expectations are written for. */
std::string mesh(const char* kind, const char* name, json extra = json::object())
{
    const std::string_view k = kind;
    const json sizes = k == "polyCube"       ? json{{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}}
                       : k == "polyPlane"    ? json{{"width", 100.0}, {"depth", 100.0}}
                       : k == "polyCylinder" ? json{{"radius", 50.0}, {"height", 100.0}}
                                             : json{{"radius", 50.0}};
    for (auto& [size, value] : sizes.items())
        if (!extra.contains(size))
            extra[size] = value;
    extra["name"] = name;
    extra["generator"] = false;
    return run(kind, extra)[0].get<std::string>();
}
} // namespace

TEST_CASE("color sets: add, rename, copy, move, current, half")
{
    geo::Mesh m = geo::makePlane({.width = 10, .depth = 10, .subdivX = 2, .subdivZ = 2});
    CHECK(geo::colors::current(m).empty());
    geo::colors::add(m, "A", glm::vec4(1, 0, 0, 1));
    CHECK(geo::colors::current(m) == "A");
    geo::colors::add(m, "B");
    CHECK(geo::colors::sets(m) == std::vector<std::string>{"A", "B"});
    CHECK(geo::colors::rename(m, "A", "Red"));
    CHECK(geo::colors::current(m) == "Red");
    CHECK(geo::colors::copy(m, "Red", "Red2"));
    CHECK(allCorners(m, "Red2", {1, 0, 0, 1}));
    CHECK(geo::colors::move(m, "Red2", 0));
    CHECK(geo::colors::sets(m).front() == "Red2");
    geo::colors::values(m, "B")[0] = glm::vec4(0.1f);
    geo::colors::setHalf(m, "B", true);
    CHECK(geo::colors::values(m, "B")[0].x == geo::colors::roundToHalf(0.1f));
    CHECK(geo::colors::values(m, "B")[0].x != 0.1f);
    CHECK(geo::colors::remove(m, "Red"));
    CHECK(geo::colors::current(m) == "Red2");
    CHECK(geo::colors::uniqueName(m, "B") == "B1");
}

TEST_CASE("color sets survive mesh ops, compaction and saving")
{
    geo::Mesh base = paintedCube();
    std::vector<Index> faces = {0, 1, 2};

    geo::Mesh ex = base;
    geo::extrudeFaces(ex, faces, {.offset = 10.0});
    CHECK(geo::colors::sets(ex) == geo::colors::sets(base));
    // New side faces copy their colors from the faces they grew from: Base keeps its blue, Mask its black.
    CHECK(allCorners(ex, "Mask", {0, 0, 0, 0}));

    geo::Mesh bev = base;
    std::vector<Index> edges;
    for (Index e = 0; e < bev.edgeCapacity() && edges.size() < 4; ++e)
        if (bev.edgeValid(e))
            edges.push_back(e);
    geo::bevelEdges(bev, edges, {.offset = 2.0});
    CHECK(allCorners(bev, "Mask", {0, 0, 0, 0}));

    geo::Mesh sub = geo::subdivide(base, {.levels = 1});
    CHECK(geo::colors::sets(sub) == geo::colors::sets(base));
    CHECK(geo::colors::current(sub) == "Mask");
    CHECK(geo::colors::isHalf(sub, "Mask"));

    geo::Mesh mir = base;
    geo::mirrorMesh(mir, {});
    CHECK(geo::colors::sets(mir) == geo::colors::sets(base));

    geo::Mesh tri = base;
    std::vector<Index> all;
    for (Index f = 0; f < tri.faceCapacity(); ++f)
        all.push_back(f);
    geo::triangulateFaces(tri, all);
    tri.compact();
    for (Index c = 0; c < tri.cornerCapacity(); ++c)
        CHECK(geo::colors::values(tri, "Base")[c].b == doctest::Approx(0.75f));

    geo::Mesh ext = geo::extractFaces(base, faces);
    CHECK(geo::colors::current(ext) == "Mask");

    // Scene file form: values, current set, precision and the white default for new faces.
    scene::MeshPtr back = scene::meshFromJson(scene::meshToJson(base));
    CHECK(geo::colors::sets(*back) == geo::colors::sets(base));
    CHECK(geo::colors::current(*back) == "Mask");
    CHECK(geo::colors::isHalf(*back, "Mask"));
    auto bv = geo::colors::values(*back, "Base");
    auto av = geo::colors::values(base, "Base");
    for (Index c = 0; c < base.cornerCapacity(); ++c)
        CHECK(glm::length(av[c] - bv[c]) < 1e-6f);
}

TEST_CASE("legacy color layers become color sets")
{
    geo::Mesh m = geo::makePlane({});
    m.pointAttrs().getOrAdd<glm::vec4>(geo::attr::kColor)[0] = glm::vec4(0.5f);
    m.cornerAttrs().getOrAdd<glm::vec4>("color1");
    geo::colors::adopt(m);
    auto sets = geo::colors::sets(m);
    CHECK(sets.size() == 2);
    CHECK(!m.pointAttrs().has(geo::attr::kColor));
    CHECK(geo::colors::has(m, "colorSet"));
}

TEST_CASE("blend modes")
{
    using vcolor::Blend;
    const glm::vec4 base(0.2f, 0.4f, 0.6f, 1.0f), brush(0.5f, 0.5f, 0.5f, 0.5f);
    auto approx = [](const glm::vec4& a, const glm::vec4& b) { return glm::length(a - b) < 1e-5f; };
    CHECK(approx(vcolor::blend(Blend::Replace, base, brush, 1.0f, vcolor::kRgba), brush));
    CHECK(approx(vcolor::blend(Blend::Replace, base, brush, 0.5f, vcolor::kRgba), (base + brush) * 0.5f));
    CHECK(approx(vcolor::blend(Blend::Add, base, brush, 1.0f, vcolor::kRgb), {0.7f, 0.9f, 1.1f, 1.0f}));
    CHECK(approx(vcolor::blend(Blend::Subtract, base, brush, 1.0f, vcolor::kRgb), {0.0f, 0.0f, 0.1f, 1.0f}));
    CHECK(approx(vcolor::blend(Blend::Multiply, base, brush, 1.0f, vcolor::kRgba), base * brush));
    CHECK(approx(vcolor::blend(Blend::Lighten, base, brush, 1.0f, vcolor::kRgba), {0.5f, 0.5f, 0.6f, 1.0f}));
    CHECK(approx(vcolor::blend(Blend::Darken, base, brush, 1.0f, vcolor::kRgba), {0.2f, 0.4f, 0.5f, 0.5f}));
    CHECK(approx(vcolor::blend(Blend::Blur, base, brush, 1.0f, vcolor::kRgba), brush));
    CHECK(approx(vcolor::blend(Blend::Sharpen, base, brush, 1.0f, vcolor::kRgb), {0.0f, 0.3f, 0.7f, 1.0f}));
    // Channel masks leave the others alone.
    CHECK(approx(vcolor::blend(Blend::Replace, base, brush, 1.0f, vcolor::kAlpha), {0.2f, 0.4f, 0.6f, 0.5f}));
    CHECK(vcolor::channelsFromString("ga") == (vcolor::kGreen | vcolor::kAlpha));
    CHECK(vcolor::channelsToString(vcolor::kRgb) == "rgb");
    for (int i = 0; i < int(Blend::Count); ++i)
        CHECK(vcolor::blendFromName(vcolor::blendName(Blend(i))) == Blend(i));
    CHECK(vcolor::applyLevels(0.5f, {.inBlack = 0.25f, .inWhite = 0.75f}) == doctest::Approx(0.5f));
    CHECK(vcolor::applyLevels(0.1f, {.inBlack = 0.25f}) == doctest::Approx(0.0f));
    glm::vec4 c(0.3f, 0.6f, 0.9f, 0.5f);
    CHECK(approx(vcolor::srgbToLinear(vcolor::linearToSrgb(c)), c));
}

TEST_CASE("brush strokes: falloff, no build-up, channel mask, symmetry")
{
    geo::Mesh plane = geo::makePlane({.width = 100, .depth = 100, .subdivX = 20, .subdivZ = 20});
    geo::colors::add(plane, "Paint", glm::vec4(0, 0, 0, 1));
    auto shared = std::make_shared<const geo::Mesh>(plane);
    vcolor::PaintMesh pm(shared, "Paint");
    pm.setSymmetry(geo::SymmetryMap::build(plane, geo::axisPlane(0), 1e-3));

    vcolor::BrushSettings s;
    s.color = glm::vec4(1, 0, 0, 1);
    s.channels = vcolor::kRed;
    s.mirror = true;
    s.strength = 0.8f;
    vcolor::Dab d;
    d.center = glm::dvec3(25, 0, 0);
    d.radius = 15;
    d.viewDir = glm::dvec3(0, -1, 0);
    d.eye = glm::dvec3(25, 500, 0);
    pm.beginStroke(s);
    for (int i = 0; i < 5; ++i)
        pm.dab(d);
    vcolor::ColorDelta delta = pm.endStroke();
    REQUIRE(!delta.empty());

    auto pointColor = [&](const glm::dvec3& at) {
        Index best = 0;
        for (Index p = 0; p < plane.pointCapacity(); ++p)
            if (glm::length(plane.position(p) - at) < glm::length(plane.position(best) - at))
                best = p;
        return pm.colors()[pm.pointCorners(best)[0]];
    };
    // Five dabs on the same spot stay at the stroke's strength.
    CHECK(pointColor({25, 0, 0}).r == doctest::Approx(0.8f));
    CHECK(pointColor({25, 0, 0}).g == 0.0f);
    CHECK(pointColor({-25, 0, 0}).r == doctest::Approx(0.8f));
    CHECK(pointColor({0, 0, 0}).r == 0.0f);
    float mid = pointColor({35, 0, 0}).r;
    CHECK(mid > 0.0f);
    CHECK(mid < 0.8f);
    CHECK(pointColor({-35, 0, 0}).r == doctest::Approx(mid));
    // Every corner the delta holds mirrors exactly.
    for (Index p = 0; p < plane.pointCapacity(); ++p)
        if (Index q = pm.symmetry().mirrorPoint(p); q != geo::kInvalid)
            CHECK(pm.colors()[pm.pointCorners(p)[0]].r == pm.colors()[pm.pointCorners(q)[0]].r);

    // Undoing the delta restores the start.
    pm.apply(delta, true);
    for (Index c = 0; c < plane.cornerCapacity(); ++c)
        CHECK(pm.colors()[c] == glm::vec4(0, 0, 0, 1));

    // Viewed from below, front faces only leaves the plane alone.
    d.viewDir = glm::dvec3(0, 1, 0);
    d.eye = glm::dvec3(25, -500, 0);
    pm.beginStroke(s);
    pm.dab(d);
    CHECK(pm.endStroke().empty());
}

TEST_CASE("vertexColorStroke is one small undo step")
{
    run("newScene");
    std::string p = mesh("polyPlane", "Canvas", {{"subdivisionsX", 40}, {"subdivisionsZ", 40}});
    run("setSymmetry", {{"args", {p}}, {"axis", "x"}});
    json dab = {{"center", {25, 0, 0}}, {"radius", 10}, {"view", {0, -1, 0}}, {"eye", {25, 500, 0}}};
    run("vertexColorFill", {{"args", {p}}, {"color", {0, 0, 0, 1}}});
    CHECK(geo::colors::current(*outMesh(p)) == "colorSet");
    size_t before = UndoStack::get().memoryBytes();
    json r = run("vertexColorStroke", {{"args", {p}}, {"color", {1, 1, 1, 1}}, {"dabs", {dab}}});
    CHECK(r["corners"].get<size_t>() > 0);
    // Only the changed corners are kept, not another copy of the mesh.
    CHECK(UndoStack::get().memoryBytes() - before < r["corners"].get<size_t>() * 64 + 4096);
    auto m = outMesh(p);
    CHECK(!allCorners(*m, "colorSet", {0, 0, 0, 1}));
    run("undo");
    CHECK(allCorners(*outMesh(p), "colorSet", {0, 0, 0, 1}));
    run("redo");
    CHECK(!allCorners(*outMesh(p), "colorSet", {0, 0, 0, 1}));
    run("undo");
    run("undo");
    // Undoing the fill that made the set takes the set away again.
    CHECK(geo::colors::sets(*outMesh(p)).empty());
}

TEST_CASE("vertexColorMasks packs a 0/1 mask per object and per material")
{
    run("newScene");
    std::vector<std::string> names;
    for (const char* n : {"E", "D", "C", "B", "A"})
        names.push_back(mesh("polyCube", n));
    json legend = run("vertexColorMasks", {{"args", names}});
    CHECK(legend["A"] == "mask0.r");
    CHECK(legend["D"] == "mask0.a");
    CHECK(legend["E"] == "mask1.r");
    for (const std::string& n : names)
    {
        auto m = outMesh(n);
        REQUIRE(geo::colors::has(*m, "mask0"));
        REQUIRE(geo::colors::has(*m, "mask1"));
        CHECK(geo::colors::isData(*m, "mask0"));
    }
    CHECK(allCorners(*outMesh("C"), "mask0", {0, 0, 1, 0}));
    CHECK(allCorners(*outMesh("C"), "mask1", {0, 0, 0, 0}));
    CHECK(allCorners(*outMesh("E"), "mask1", {1, 0, 0, 0}));
    run("undo");
    CHECK_FALSE(geo::colors::has(*outMesh("A"), "mask0"));

    run("createMaterial", {{"args", {"Red"}}});
    run("assignMaterial", {{"args", {"Red", "A"}}, {"faces", {0}}});
    legend = run("vertexColorMasks", {{"args", {"A"}}, {"source", "material"}, {"channels", 3}, {"prefix", "mat"}});
    CHECK(legend.size() == 2);
    CHECK(legend["Default"] == "mat0.r");
    CHECK(legend["Red"] == "mat0.g");
    auto m = outMesh("A");
    auto v = geo::colors::values(*m, "mat0");
    for (Index c = 0; c < m->cornerCapacity(); ++c)
        if (m->cornerValid(c))
            CHECK(v[c] == (m->cornerFace(c) == 0 ? glm::vec4(0, 1, 0, 1) : glm::vec4(1, 0, 0, 1)));
}

TEST_CASE("fill, gradient, adjust, blur and bake commands")
{
    run("newScene");
    std::string c = mesh("polyCube", "Box", {{"subdivisionsX", 4}, {"subdivisionsY", 4}, {"subdivisionsZ", 4}});
    run("polyColorSet", {{"args", {c}}, {"create", true}, {"colorSet", "Masks"}, {"fill", {0, 0, 0, 1}}});
    run("vertexColorGradient", {{"args", {c}}, {"axis", "y"}, {"channels", "g"}});
    auto m = outMesh(c);
    auto v = geo::colors::values(*m, "Masks");
    for (Index k = 0; k < m->cornerCapacity(); ++k)
        if (m->cornerValid(k))
            CHECK(v[k].g == doctest::Approx(float((m->position(m->cornerPoint(k)).y + 50.0) / 100.0)).epsilon(1e-4));
    run("vertexColorAdjust", {{"args", {c}}, {"channels", "g"}});
    m = outMesh(c);
    v = geo::colors::values(*m, "Masks");
    for (Index k = 0; k < m->cornerCapacity(); ++k)
        if (m->cornerValid(k))
            CHECK(v[k].g == doctest::Approx(float((50.0 - m->position(m->cornerPoint(k)).y) / 100.0)).epsilon(1e-4));
    run("vertexColorBake", {{"args", {c}}, {"type", "height"}, {"channels", "b"}});
    run("vertexColorBake", {{"args", {c}}, {"type", "ao"}, {"channels", "r"}, {"rays", 16}});
    m = outMesh(c);
    v = geo::colors::values(*m, "Masks");
    // A lone convex box occludes nothing of itself.
    for (Index k = 0; k < m->cornerCapacity(); ++k)
        if (m->cornerValid(k))
            CHECK(v[k].r == doctest::Approx(1.0f));
    run("vertexColorBake", {{"args", {c}}, {"type", "cavity"}, {"channels", "a"}});
    m = outMesh(c);
    v = geo::colors::values(*m, "Masks");
    // Nothing on a box is concave.
    for (Index k = 0; k < m->cornerCapacity(); ++k)
        if (m->cornerValid(k))
            CHECK(v[k].a == doctest::Approx(1.0f));
    run("vertexColorBlur", {{"args", {c}}, {"channels", "b"}, {"iterations", 3}});
    json q = run("polyColorSet", {{"args", {c}}, {"query", true}});
    CHECK(q["current"] == "Masks");

    // Concave: a box with a pushed-in face has occlusion and cavity inside.
    std::string bowl = mesh("polyCube", "Bowl", {{"subdivisionsX", 3}, {"subdivisionsY", 3}, {"subdivisionsZ", 3}});
    geo::Mesh b = *outMesh(bowl);
    std::vector<float> ao = vcolor::bake(b, {.type = vcolor::BakeType::AmbientOcclusion, .rays = 32});
    float lo = 1.0f;
    for (float x : ao)
        lo = std::min(lo, x);
    CHECK(lo == doctest::Approx(1.0f));
    std::vector<Index> top;
    for (Index f = 0; f < b.faceCapacity(); ++f)
        if (b.faceValid(f) && b.faceNormal(f).y > 0.9 && glm::length(glm::dvec2(b.faceCentroid(f).x, b.faceCentroid(f).z)) < 1.0)
            top.push_back(f);
    REQUIRE(top.size() == 1);
    geo::extrudeFaces(b, top, {.offset = -40.0});
    ao = vcolor::bake(b, {.type = vcolor::BakeType::AmbientOcclusion, .rays = 64});
    std::vector<float> cav = vcolor::bake(b, {.type = vcolor::BakeType::Cavity});
    float aoMin = 1.0f, cavMin = 1.0f;
    for (Index p = 0; p < b.pointCapacity(); ++p)
        if (b.pointValid(p))
        {
            aoMin = std::min(aoMin, ao[p]);
            cavMin = std::min(cavMin, cav[p]);
        }
    CHECK(aoMin < 0.8f);
    CHECK(cavMin < 0.9f);
}

TEST_CASE("live transfer modifier follows its source and caches the source tree")
{
    run("newScene");
    std::string src = mesh("polySphere", "Src");
    std::string dst = mesh("polySphere", "Dst", {{"subdivisionsAxis", 30}, {"subdivisionsHeight", 24}, {"radius", 52}});
    run("vertexColorFill", {{"args", {src}}, {"color", {1, 0, 0, 1}}});
    json mods = run("transferAttributes", {{"args", {src, dst}}, {"uvs", false}, {"colors", true}, {"live", true}});
    REQUIRE(mods.size() == 1);
    std::string mod = mods[0];
    CHECK(allCorners(*outMesh(dst), "colorSet", {1, 0, 0, 1}));
    auto& g = scene::Graph::current();
    auto builds = [&] {
        g.evaluate();
        scene::Node* n = g.find(mod);
        return n->as<int64_t>(n->type().attrIndex("cacheBuilds"));
    };
    CHECK(builds() == 1);

    // Editing the source updates the target live.
    run("vertexColorFill", {{"args", {src}}, {"color", {0, 0, 1, 1}}});
    CHECK(allCorners(*outMesh(dst), "colorSet", {0, 0, 1, 1}));
    CHECK(builds() == 2);
    // Moving the source reuses the tree; the colors follow it in world space.
    run("move", {{"args", {src}}, {"by", {0, 1, 0}}});
    outMesh(dst);
    CHECK(builds() == 2);
    // The base mesh under the modifier never got the colors.
    CHECK(geo::colors::sets(*scene::modifiers::cage(g, g.find(dst)->id())).empty());

    run("applyModifier", {{"args", {mod}}});
    CHECK(!g.find(mod));
    CHECK(allCorners(*scene::modifiers::cage(g, g.find(dst)->id()), "colorSet", {0, 0, 1, 1}));
    run("undo");
    CHECK(g.find(mod));

    // Maps one named set onto another.
    run("polyColorSet", {{"args", {src}}, {"create", true}, {"colorSet", "Dirt"}, {"fill", {0.5, 0.5, 0.5, 1}}});
    run("setAttr", {{"args", {mod + ".sourceColorSet", "Dirt"}}});
    run("setAttr", {{"args", {mod + ".targetColorSet", "Grime"}}});
    CHECK(allCorners(*outMesh(dst), "Grime", {0.5f, 0.5f, 0.5f, 1}));
}

TEST_CASE("OBJ and FBX keep vertex colors")
{
    geo::Mesh m = paintedCube();
    geo::Mesh obj = geo::readObj(geo::writeObj(m)).objects.at(0).mesh;
    // OBJ holds one color per point: the current set.
    REQUIRE(geo::colors::sets(obj).size() == 1);
    auto pts = geo::colors::pointColors(m, "Mask");
    auto back = geo::colors::pointColors(obj, geo::colors::current(obj));
    CHECK(glm::length(glm::vec3(back[0]) - glm::vec3(pts[0])) < 1e-4f);

    run("newScene");
    auto& g = scene::Graph::current();
    scene::Node* n = g.createNode(*scene::types::find("mesh"), "Painted", scene::kNoNode, false);
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(m)));
    auto path = (std::filesystem::temp_directory_path() / "lila_vcolor_test.fbx").string();
    run("exportFbx", {{"args", {path, "Painted"}}, {"profile", "generic"}});
    run("newScene");
    json names = run("importFbx", {{"args", {path}}});
    scene::MeshPtr in;
    for (const json& j : names)
        if (scene::Node* x = g.find(j.get<std::string>()); x && x->type().isA("mesh"))
            in = outMesh(x->name());
    REQUIRE(in);
    // Both sets come back by name and in order (the first is what Unreal reads).
    CHECK(geo::colors::sets(*in) == geo::colors::sets(m));
    float maxA = 0.0f;
    for (const glm::vec4& c : geo::colors::values(*in, "Base"))
    {
        CHECK(c.b == doctest::Approx(0.75f));
        maxA = std::max(maxA, c.a);
    }
    CHECK(maxA == doctest::Approx(1.0f));
}

TEST_CASE("booleans keep the base's color sets")
{
    run("newScene");
    std::string box = mesh("polyCube", "Block");
    std::string drill = mesh("polyCylinder", "Drill", {{"radius", 20}, {"height", 300}});
    run("vertexColorFill", {{"args", {box}}, {"color", {1, 0, 0, 1}}});
    run("polyBoolean", {{"args", {box, drill}}, {"operation", "difference"}});
    auto m = outMesh(box);
    REQUIRE(geo::colors::sets(*m) == std::vector<std::string>{"colorSet"});
    auto v = geo::colors::values(*m, "colorSet");
    int red = 0, white = 0;
    for (Index c = 0; c < m->cornerCapacity(); ++c)
        if (m->cornerValid(c))
            (v[c] == glm::vec4(1, 0, 0, 1) ? red : v[c] == glm::vec4(1.0f) ? white : red) += 1;
    // The block's own faces stay red; the drilled hole's walls come from the cutter, which had no colors.
    CHECK(red > 0);
    CHECK(white > 0);
}
