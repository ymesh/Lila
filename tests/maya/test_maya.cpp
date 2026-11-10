#include <doctest/doctest.h>

#include "TestUtil.h"
#include "ops/Command.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "io/References.h"
#include "io/SceneFile.h"
#include "maya/Maya.h"
#include "maya/MayaFile.h"
#include "math/MathCore.h"
#include "scene/Anim.h"
#include "scene/AnimCurve.h"
#include "scene/Camera.h"
#include "scene/DisplayLayers.h"
#include "scene/Graph.h"
#include "scene/ImagePlane.h"
#include "scene/Light.h"
#include "scene/LightingChannels.h"
#include "scene/LodGroup.h"
#include "scene/Instancing.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Rig.h"
#include "scene/Skin.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>

using namespace lila;
using namespace lila::scene;
using namespace mayatest;
namespace fs = std::filesystem;

namespace
{
Graph& fresh()
{
    io::refs::clear();
    Graph& g = Graph::current();
    g.clear();
    return g;
}

maya::ImportResult load(const std::string& name, maya::ImportOptions opts = {})
{
    maya::ImportResult r = maya::importMaya(Graph::current(), fixture(name), opts);
    if (!r.ok)
        MESSAGE(r.error);
    REQUIRE(r.ok);
    return r;
}

MeshPtr outMesh(Graph& g, const std::string& name)
{
    scene::Node* n = node(g, name);
    MeshPtr m = std::get<MeshPtr>(g.value({n->id(), MeshNode::kOutMesh, -1}));
    REQUIRE(m);
    return m;
}

glm::dvec3 world(Graph& g, const std::string& mesh, geo::Index p)
{
    MeshPtr m = outMesh(g, mesh);
    glm::dmat4 w = std::get<glm::dmat4>(g.value({node(g, mesh)->id(), Transform::kWorldMatrix, -1}));
    return glm::dvec3(w * glm::dvec4(m->position(p), 1.0));
}

void checkVec(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-4)
{
    INFO("got " << a.x << " " << a.y << " " << a.z << ", want " << b.x << " " << b.y << " " << b.z);
    CHECK(glm::length(a - b) < eps);
}

glm::dvec3 vec(Graph& g, const std::string& n, int attr) { return node(g, n)->as<glm::dvec3>(attr); }

std::string slotOfFace(const geo::Mesh& m, geo::Index f)
{
    std::span<const int32_t> mat = m.faceAttrs().get<int32_t>(geo::attr::kMaterial);
    return mat.empty() ? std::string() : materials::slotName(m, mat[f]);
}
} // namespace

TEST_CASE("maya ascii: statements, values and polyFaces")
{
    maya::File f = maya::readAscii(R"(//Maya ASCII 2026 scene
requires maya "2026";
currentUnit -l meter -a radian -t ntsc;
fileInfo "application" "maya";
file -r -ns "chars" -dr 1 -rfn "charsRN" -typ "mayaAscii" "C:/hero.ma";
createNode transform -n "a";
	setAttr ".t" -type "double3" 1 2 3 ;
	setAttr -k off ".v" no;
	setAttr ".s" -type "string" "semi;colon";
createNode mesh -n "aShape" -p "a";
	setAttr -s 2 ".uvst[0].uvsp[0:1]" -type "float2" 0 0 1 1;
	setAttr ".iog[0].og[0].gcl" -type "componentList" 2 "f[0:2]" "f[5]";
	setAttr -s 1 -ch 6 ".fc[0]" -type "polyFaces"
		f 3 0 1 -3
		mu 0 3 0 1 2
		h 3 3 4 5
		mc 1 3 0 -1 2;
select -ne :lambert1;
	setAttr ".c" -type "float3" 0.1 0.2 0.3 ;
connectAttr "a.t" "aShape.t" -na;
parent -s -nc -r -add "|a|aShape" "b";
)");
    CHECK(f.linearUnit == "meter");
    CHECK(f.angleUnit == "radian");
    CHECK(f.timeUnit == "ntsc");
    CHECK(f.fileInfo["application"] == "maya");
    REQUIRE(f.references.size() == 1);
    CHECK(f.references[0].path == "C:/hero.ma");
    CHECK(f.references[0].ns == "chars");
    REQUIRE(f.nodes.size() == 3);
    const maya::Node& a = f.nodes[0];
    CHECK(a.type == "transform");
    REQUIRE(a.attrs.size() == 3);
    CHECK(a.attrs[0].nums == std::vector<double>{1, 2, 3});
    CHECK(a.attrs[1].nums == std::vector<double>{0});
    CHECK(a.attrs[2].strs[0] == "semi;colon");
    const maya::Node& shape = f.nodes[1];
    CHECK(shape.parent == "a");
    CHECK(shape.attrs[0].size == 2);
    CHECK(shape.attrs[1].strs == std::vector<std::string>{"f[0:2]", "f[5]"});
    REQUIRE(shape.attrs[2].mesh);
    const maya::MeshData& faces = *shape.attrs[2].mesh;
    REQUIRE(faces.faces.size() == 1);
    REQUIRE(faces.faces[0].loops.size() == 2);
    CHECK(faces.faces[0].loops[0].edges == std::vector<int32_t>{0, 1, -3});
    CHECK(faces.faces[0].loops[0].uv[0] == std::vector<int32_t>{0, 1, 2});
    CHECK(faces.faces[0].loops[1].color[1] == std::vector<int32_t>{0, -1, 2});
    CHECK(f.nodes[2].existing);
    CHECK(f.nodes[2].name == "lambert1");
    REQUIRE(f.connections.size() == 1);
    CHECK(f.connections[0].nextAvailable);
    REQUIRE(f.parents.size() == 1);
    CHECK(f.parents[0].add);

    std::vector<maya::AttrSeg> segs = maya::splitAttr("uvst[0].uvsp[3:7]");
    REQUIRE(segs.size() == 2);
    CHECK(segs[0].name == "uvst");
    CHECK(segs[1].lo == 3);
    CHECK(segs[1].hi == 7);
    CHECK(maya::fpsOf("film") == 24.0);
    CHECK(maya::fpsOf("23.976fps") == doctest::Approx(23.976));
    CHECK(maya::cmPerUnit("in") == doctest::Approx(2.54));
}

TEST_CASE("maya import: handmade group, cube, locator, camera")
{
    Graph& g = fresh();
    maya::ImportResult r = load("cube_hier.ma");
    CHECK(r.upAxis == "y");
    CHECK(g.find("top") == nullptr);

    scene::Node* grp = node(g, "grp");
    scene::Node* box = node(g, "box");
    CHECK(box->type().name == "mesh");
    CHECK(g.parent(box->id()) == grp->id());
    CHECK(grp->as<int64_t>(Transform::kRotateOrder) == int64_t(RotateOrder::ZXY));
    checkVec(vec(g, "box", Transform::kTranslate), {5, 1, 0});
    checkVec(vec(g, "box", Transform::kScale), {1, 2, 1});
    checkVec(vec(g, "box", Transform::kRotatePivot), {0.5, 0, 0});
    checkVec(vec(g, "box", Transform::kScalePivot), {0, 0.25, 0});

    MeshPtr m = outMesh(g, "box");
    CHECK(m->numPoints() == 8);
    CHECK(m->numFaces() == 6);
    checkVec(m->position(1), {1, -0.5, 1});
    std::span<const glm::vec2> uv = m->cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    REQUIRE(!uv.empty());
    CHECK(uv[m->faceCorner(0) + 2].x == doctest::Approx(0.625f));
    CHECK(uv[m->faceCorner(0) + 2].y == doctest::Approx(0.25f));
    std::span<const bool> hard = m->edgeAttrs().get<bool>(geo::attr::kHard);
    REQUIRE(!hard.empty());
    CHECK(std::count(hard.begin(), hard.end(), true) == 11);

    // Maya's pivot model, written out: T * Rp * R * Rp^-1 * Sp * S * Sp^-1 under the group's T * R.
    glm::dmat4 grpWorld = glm::translate(glm::dmat4(1.0), {1, 2, 3}) * glm::rotate(glm::dmat4(1.0), glm::radians(90.0), {0, 1, 0});
    glm::dmat4 boxLocal = glm::translate(glm::dmat4(1.0), {5, 1, 0}) * glm::translate(glm::dmat4(1.0), {0, 0.25, 0}) *
                          glm::scale(glm::dmat4(1.0), {1, 2, 1}) * glm::translate(glm::dmat4(1.0), {0, -0.25, 0});
    checkVec(world(g, "box", 0), glm::dvec3(grpWorld * boxLocal * glm::dvec4(-1, -1, 1, 1)));

    CHECK_FALSE(node(g, "empty")->as<bool>(Transform::kVisibility));
    scene::Node* marker = node(g, "marker");
    CHECK(marker->type().name == "locator");
    checkVec(marker->as<glm::dvec3>(Locator::kLocalScale), {2, 2, 2});
    CHECK(node(g, "shotCam")->type().name == "camera");
    CHECK_FALSE(contains(r.skipped, "camera"));
    CHECK(contains(r.skipped, "nurbsCurve 'curveShape1'"));
    CHECK(contains(r.skipped, "polyBevel3"));
}

TEST_CASE("maya import: world points match Maya, from .ma and .mb")
{
    for (const char* file : {"generated/cube_hier.ma", "generated/cube_hier.mb"})
    {
        CAPTURE(file);
        Graph& g = fresh();
        maya::ImportResult r = load(file);
        CHECK(r.format == (std::string_view(file).ends_with(".mb") ? "binary" : "ascii"));
        // Maya: MFnMesh world points of |grp|box|boxShape; point 1 carries a pnts tweak.
        checkVec(world(g, "box", 0), {1.77158, -1.25865, 4.36271});
        checkVec(world(g, "box", 1), {3.4766, 0.56445, 4.31459});
        checkVec(world(g, "box", 7), {2.11127, 0.92075, 0.23183});
        CHECK(outMesh(g, "box")->numFaces() == 6);
    }
}

TEST_CASE("maya import: binary and ascii files decode to the same scene")
{
    struct Snap
    {
        std::string type;
        glm::dmat4 world;
        size_t points = 0, faces = 0, uvSets = 0;
    };
    auto snapshot = [](Graph& g) {
        g.evaluate();
        std::map<std::string, Snap> out;
        for (NodeId id : g.nodes())
        {
            scene::Node* n = g.node(id);
            Snap s{n->type().name};
            if (n->type().isA("transform"))
                s.world = n->as<glm::dmat4>(Transform::kWorldMatrix);
            if (n->type().isA("mesh"))
                if (MeshPtr m = std::get<MeshPtr>(g.value({id, MeshNode::kOutMesh, -1})))
                {
                    s.points = m->numPoints();
                    s.faces = m->numFaces();
                    for (int k = 0; k < 4; ++k)
                        s.uvSets += m->cornerAttrs().has(geo::attr::uvName(k)) ? 1 : 0;
                }
            out[n->name()] = s;
        }
        return out;
    };
    for (const char* base : {"cube_hier", "skin", "materials", "anim", "prims", "rich", "variants", "misc"})
    {
        CAPTURE(base);
        Graph& g = fresh();
        load(std::string("generated/") + base + ".ma");
        auto ascii = snapshot(g);
        fresh();
        load(std::string("generated/") + base + ".mb");
        auto binary = snapshot(g);
        CHECK(ascii.size() == binary.size());
        for (const auto& [name, a] : ascii)
        {
            CAPTURE(name);
            auto it = binary.find(name);
            REQUIRE(it != binary.end());
            const Snap& b = it->second;
            CHECK(a.type == b.type);
            CHECK(a.points == b.points);
            CHECK(a.faces == b.faces);
            CHECK(a.uvSets == b.uvSets);
            for (int c = 0; c < 4; ++c)
                CHECK(glm::length(a.world[c] - b.world[c]) < 1e-6);
        }
    }
}

TEST_CASE("maya import: rich mesh data from .ma and .mb")
{
    for (const char* file : {"generated/rich.ma", "generated/rich.mb"})
    {
        CAPTURE(file);
        Graph& g = fresh();
        load(file);
        MeshPtr m = outMesh(g, "rich");
        CHECK(m->numFaces() == 4);
        CHECK(m->cornerAttrs().has(geo::attr::uvName(1)));
        std::vector<std::string> sets = geo::colors::sets(*m);
        REQUIRE(sets.size() == 2);
        std::span<const glm::vec4> col = geo::colors::values(*m, sets[0]);
        REQUIRE(!col.empty());
        CHECK(col[m->faceCorner(0)].a == doctest::Approx(0.5f));
        CHECK(geo::colors::current(*m) == sets[0]);
        // The locked normal on vertex 4 is shared by every face around it.
        std::span<const glm::vec3> nrm = m->cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
        REQUIRE(!nrm.empty());
        int locked = 0;
        for (geo::Index c = 0; c < m->cornerCapacity(); ++c)
            if (m->cornerValid(c) && m->cornerPoint(c) == 4)
            {
                CHECK(nrm[c].x == doctest::Approx(0.6f));
                ++locked;
            }
        CHECK(locked == 4);
        std::span<const float> crease = m->edgeAttrs().get<float>(geo::attr::kCrease);
        REQUIRE(!crease.empty());
        CHECK(crease[m->findEdge(0, 3)] == doctest::Approx(2.5f));
    }
}

TEST_CASE("maya import: handmade skin with both weight forms and an animated joint")
{
    Graph& g = fresh();
    maya::ImportResult r = load("skin.ma");
    REQUIRE(r.skins.size() == 1);
    CHECK(r.joints.size() == 2);
    checkVec(vec(g, "root", Joint::kJointOrient), {0, 0, 90});
    scene::Node* elbow = node(g, "elbow");
    CHECK(elbow->as<int64_t>(Joint::kSide) == 1);
    CHECK(elbow->as<int64_t>(Joint::kJointType) == 11);
    CHECK(elbow->as<bool>(Joint::kSegmentScaleCompensate));
    CHECK(g.source({elbow->id(), Joint::kInverseScale, -1}).has_value());
    CHECK(node(g, "root")->as<double>(Joint::kRadius) == doctest::Approx(0.5));
    CHECK(r.curves == 1);
    CHECK_FALSE(contains(r.warnings, "tweak"));

    WeightTable t = skin::table(g, r.skins[0]);
    REQUIRE(t.points.size() == 6);
    std::vector<skin::Influence> infs = skin::influences(g, r.skins[0]);
    REQUIRE(infs.size() == 2);
    auto inf = [&](const char* joint) { return uint32_t(skin::indexOf(infs, joint)); };
    CHECK(t.get(3, inf("elbow")) == doctest::Approx(0.75f));
    CHECK(t.get(4, inf("elbow")) == doctest::Approx(1.0f));
    CHECK(t.get(2, inf("root")) == doctest::Approx(0.5f));
    CHECK(t.get(5, inf("elbow")) == doctest::Approx(1.0f));

    g.setTime(0.0);
    checkVec(world(g, "strip", 4), {0, 2, 0});
    g.setTime(1.0); // frame 30 at ntsc: the elbow is at 90
    checkVec(world(g, "strip", 0), {0, 0, 0});
    checkVec(world(g, "strip", 5), {-1, 2, 0});
    checkVec(world(g, "strip", 3), {0.25, 1.75, 0});
}

TEST_CASE("maya import: skinned mesh deforms like Maya at frame 10, from .ma and .mb")
{
    for (const char* file : {"generated/skin.ma", "generated/skin.mb"})
    {
        CAPTURE(file);
        Graph& g = fresh();
        maya::ImportResult r = load(file);
        REQUIRE(r.skins.size() == 1);
        CHECK(r.curves == 2);
        g.setTime(0.0);
        g.setTime(10.0 / 24.0);
        checkVec(world(g, "skinbox", 0), {1.52043, -1.99696, 0.5}, 2e-3);
        checkVec(world(g, "skinbox", 1), {2.5166, -1.9877, 0.5}, 2e-3);
        checkVec(world(g, "skinbox", 19), {2.5166, -1.9877, -0.5}, 2e-3);
        g.setTime(1.0 / 24.0);
        checkVec(world(g, "skinbox", 0), {-0.5, -2.0, 0.5}, 1e-4);
    }
}

TEST_CASE("maya import: handmade materials, textures and per-face assignment")
{
    Graph& g = fresh();
    maya::ImportResult r = load("material.ma");
    scene::Node* paint = materials::find(g, "Paint");
    REQUIRE(paint);
    CHECK(paint->as<std::string>(material::kBaseColorMap) == "textures/paint_bc.png");
    CHECK(paint->as<std::string>(material::kNormalMap) == "C:/maps/paint_n.png");
    CHECK(paint->as<double>(material::kMetallic) == doctest::Approx(0.7));
    CHECK(paint->as<double>(material::kRoughness) == doctest::Approx(0.3));
    scene::Node* glass = materials::find(g, "Glass");
    REQUIRE(glass);
    CHECK(glass->as<int64_t>(material::kBlendMode) == int64_t(material::BlendMode::Translucent));
    CHECK(glass->as<double>(material::kOpacity) == doctest::Approx(0.5));
    checkVec(glass->as<glm::dvec3>(material::kBaseColor), glm::dvec3(0.2, 0.4, 0.6) * 0.8, 1e-6);
    REQUIRE(materials::find(g, "Shiny"));
    CHECK(materials::find(g, "Shiny")->as<double>(material::kRoughness) == doctest::Approx(0.1));

    MeshPtr crate = outMesh(g, "crate");
    for (geo::Index f = 0; f < 4; ++f)
        CHECK(slotOfFace(*crate, f) == "Paint");
    CHECK(slotOfFace(*crate, 4) == "Glass");
    CHECK(slotOfFace(*crate, 5) == "Glass");
    CHECK(slotOfFace(*outMesh(g, "floor"), 0) == "Shiny");
    CHECK_FALSE(contains(r.skipped, "place2dTexture"));

    // The base color file's wrap and mirror come from its place2dTexture, filter type Off is nearest.
    const material::TextureSampling s = materials::sampling(*paint, material::kBaseColorMap);
    CHECK(s.wrapU == material::TextureWrap::Black);
    CHECK(s.wrapV == material::TextureWrap::Mirror);
    CHECK(s.filter == material::TextureFilter::Nearest);
    CHECK(materials::sampling(*paint, material::kNormalMap).isDefault());
    // A surface shader is unlit.
    scene::Node* flat = materials::find(g, "Flat");
    REQUIRE(flat);
    CHECK(flat->as<int64_t>(material::kShadingModel) == int64_t(material::ShadingModel::Unlit));
    checkVec(flat->as<glm::dvec3>(material::kBaseColor), glm::dvec3(0.9, 0.5, 0.1), 1e-6);
    CHECK(slotOfFace(*outMesh(g, "card"), 0) == "Flat");
    CHECK_FALSE(contains(r.skipped, "groupId"));
}

TEST_CASE("maya import: Maya-written materials from .ma and .mb")
{
    for (const char* file : {"generated/materials.ma", "generated/materials.mb"})
    {
        CAPTURE(file);
        Graph& g = fresh();
        load(file);
        MeshPtr box = outMesh(g, "matbox");
        for (geo::Index f = 0; f < 4; ++f)
            CHECK(slotOfFace(*box, f) == "Paint");
        CHECK(slotOfFace(*box, 4) == "Rubber");
        scene::Node* paint = materials::find(g, "Paint");
        REQUIRE(paint);
        CHECK(paint->as<std::string>(material::kBaseColorMap) == "textures/paint_bc.png");
        CHECK(paint->as<std::string>(material::kNormalMap) == "textures/paint_n.png");
        CHECK(paint->as<double>(material::kMetallic) == doctest::Approx(0.7).epsilon(1e-4));
        CHECK(slotOfFace(*outMesh(g, "blinnbox"), 0) == "Shiny");
    }
}

TEST_CASE("maya import: units, DAG paths, namespaces and custom attributes")
{
    Graph& g = fresh();
    maya::ImportResult r = load("units_paths.ma");
    checkVec(vec(g, "a", Transform::kTranslate), {100, 0, 0});
    checkVec(vec(g, "b", Transform::kRotate), {0, 90, 0}, 1e-9);
    // Both leafs keep their name; paths tell them apart.
    CHECK(g.parent(node(g, "a|leaf")->id()) == node(g, "a")->id());
    CHECK(g.parent(node(g, "b|leaf")->id()) == node(g, "b")->id());
    checkVec(vec(g, "b|leaf", Transform::kTranslate), {0, 0, 300});
    CHECK(g.parent(node(g, "kid")->id()) == node(g, "b|leaf")->id());
    CHECK(node(g, "props:barrel")->nameSpace() == "props");
    // Numeric custom attributes become Lila user attrs, with their values.
    CHECK_FALSE(contains(r.skipped, "barrel.health"));
    CHECK(std::get<double>(g.value(g.plug("props:barrel.health"))) == doctest::Approx(7.0));
    CHECK(std::get<double>(g.value(g.plug("props:barrel.hp"))) == doctest::Approx(7.0));
    // Not centimetres, so the cube is baked at its size rather than kept as a live generator.
    MeshPtr box = outMesh(g, "props:box");
    CHECK(box->bounds().size().x == doctest::Approx(200.0));
    CHECK(box->bounds().size().z == doctest::Approx(600.0));
    // 25 fps: frame 25 is one second, 2 m up.
    g.setTime(1.0);
    CHECK(std::get<glm::dvec3>(g.value({node(g, "a")->id(), Transform::kTranslate, -1})).y == doctest::Approx(200.0));
}

TEST_CASE("maya import: primitive history stays live, smoothing is evaluated")
{
    for (const char* file : {"generated/prims.ma", "generated/prims.mb"})
    {
        CAPTURE(file);
        Graph& g = fresh();
        load(file);
        scene::Node* cube = node(g, "hcube");
        auto src = g.source({cube->id(), MeshNode::kInMesh, -1});
        REQUIRE(src.has_value());
        scene::Node* gen = g.node(src->node);
        CHECK(gen->type().name == "polyCube");
        CHECK(gen->as<double>(gen->type().attrIndex("width")) == doctest::Approx(5.0));
        CHECK(gen->as<int64_t>(gen->type().attrIndex("subdivisionsY")) == 3);
        CHECK(outMesh(g, "hcube")->numFaces() == 22);
        CHECK(outMesh(g, "hsphere")->bounds().size().y == doctest::Approx(6.0).epsilon(1e-3));
        MeshPtr smoothed = outMesh(g, "smoothed");
        CHECK(smoothed->numFaces() == 24);
        CHECK(smoothed->numPoints() == 26);
    }
}

TEST_CASE("maya import: metres and a Z-up file")
{
    for (const char* file : {"generated/meter_zup.ma", "generated/meter_zup.mb"})
    {
        CAPTURE(file);
        Graph& g = fresh();
        load(file, {.upAxis = "y"});
        checkVec(world(g, "mcube", 0), {200.0, -70.71068, 50.0}, 1e-3);

        fresh();
        maya::ImportResult r = load(file, {.upAxis = "z"});
        CHECK(r.upAxis == "z");
        glm::dvec3 p = world(g, "mcube", 0);
        // Maya's Z (up) lands on Lila's up.
        CHECK(p[int(math::upAxis() == math::UpAxis::Z ? 2 : 1)] == doctest::Approx(50.0));
        CHECK(glm::length(p) == doctest::Approx(glm::length(glm::dvec3(200.0, -70.71068, 50.0))).epsilon(1e-5));
    }
}

TEST_CASE("maya import: animation curves follow Maya's samples")
{
    // Maya getAttr at these frames (film, 24 fps).
    const std::pair<double, double> tx[] = {{1.0, 0.0}, {3.5, 0.5}, {6.0, 1.0}, {13.5, 1.25}, {23.5, 1.625}};
    const std::pair<double, double> ry[] = {{1.0, 0.0}, {5.5, 12.754045779268113}, {10.0, 41.450648782621364}, {20.0, 90.0}, {30.0, 90.0}};
    for (const char* file : {"generated/anim.ma", "generated/anim.mb"})
    {
        CAPTURE(file);
        Graph& g = fresh();
        maya::ImportResult r = load(file);
        CHECK(r.curves == 2);
        NodeId tan = node(g, "tan")->id();
        auto txCurve = g.source({tan, Transform::kTranslate, 0});
        auto ryCurve = g.source({tan, Transform::kRotate, 1});
        REQUIRE(txCurve.has_value());
        REQUIRE(ryCurve.has_value());
        anim::Curve cx = anim::curveOf(*g.node(txCurve->node));
        anim::Curve cy = anim::curveOf(*g.node(ryCurve->node));
        CHECK(cx.keys.size() == 10);
        CHECK(cx.pre == anim::Infinity::Cycle);
        CHECK(cx.post == anim::Infinity::Oscillate);
        CHECK(cx.keys[1].in == anim::Tangent::Linear);
        CHECK(cx.keys[3].out == anim::Tangent::Step);
        for (const auto& [frame, value] : tx)
        {
            CAPTURE(frame);
            CHECK(cx.evaluate(frame / 24.0) == doctest::Approx(value).epsilon(0.02));
        }
        for (const auto& [frame, value] : ry)
        {
            CAPTURE(frame);
            CHECK(cy.evaluate(frame / 24.0) == doctest::Approx(value).epsilon(0.05));
        }
    }
}

TEST_CASE("maya import: cameras and image planes from .ma and .mb")
{
    // Maya 2026 at frame 1: shotCam's eye, view and up directions.
    const glm::dvec3 eye(10, 5, 20), view(-0.4161977407267834, -0.17364817766693036, -0.89253893528903);
    const glm::dvec3 up(-0.07338689100003824, 0.984807753012208, -0.15737869562426265);
    for (const char* file : {"generated/cameras.ma", "generated/cameras.mb"})
    {
        CAPTURE(file);
        Graph& g = fresh();
        maya::ImportResult r = load(file);
        CHECK(r.cameras.size() == 3);
        CHECK(r.imagePlanes.size() == 6);
        CHECK_FALSE(contains(r.skipped, "camera"));
        CHECK_FALSE(contains(r.skipped, "imagePlane"));
        CHECK(g.find("persp") == nullptr);
        CHECK(g.find("frontShape") == nullptr);

        scene::Node* shot = node(g, "shotCam");
        CHECK(shot->type().name == "camera");
        CHECK(shot->as<double>(Camera::kHorizontalFilmAperture) == doctest::Approx(1.5));
        CHECK(shot->as<double>(Camera::kVerticalFilmAperture) == doctest::Approx(1.0));
        CHECK(shot->as<int64_t>(Camera::kFilmFit) == 2);
        CHECK(shot->as<double>(Camera::kNearClipPlane) == doctest::Approx(1.0));
        CHECK(shot->as<double>(Camera::kFarClipPlane) == doctest::Approx(5000.0));
        CHECK_FALSE(shot->as<bool>(Camera::kOrthographic));
        CHECK_FALSE(shot->as<bool>(Camera::kLocked));
        glm::dmat4 w = std::get<glm::dmat4>(g.value({shot->id(), Transform::kWorldMatrix, -1}));
        checkVec(glm::dvec3(w[3]), eye);
        imagePlane::CameraFrame cf = camera::frame(*shot, w);
        checkVec(cf.forward, view);
        checkVec(cf.up, up);
        CHECK(cf.film.x == doctest::Approx(1.5 * 12.7 / 50.0));

        // Focal length and translate X are keyed at frames 1 and 24.
        auto fl = g.source({shot->id(), Camera::kFocalLength, -1});
        REQUIRE(fl.has_value());
        CHECK(anim::curveOf(*g.node(fl->node)).evaluate(1.0) == doctest::Approx(85.0));
        CHECK(anim::curveOf(*g.node(fl->node)).evaluate(1.0 / 24.0) == doctest::Approx(50.0));
        CHECK(g.source({shot->id(), Transform::kTranslate, 0}).has_value());

        scene::Node* ortho = node(g, "orthoCam");
        CHECK(ortho->as<bool>(Camera::kOrthographic));
        CHECK(ortho->as<double>(Camera::kOrthographicWidth) == doctest::Approx(30.0));
        CHECK(node(g, "lockedCam")->as<bool>(Camera::kLocked));

        // A plane on a user perspective camera rides it at its depth, sized from the lens.
        scene::Node* shotPlate = node(g, "shotPlate");
        CHECK(shotPlate->type().name == "imagePlane");
        CHECK(g.parent(shotPlate->id()) == shot->id());
        CHECK(shotPlate->as<std::string>(ImagePlane::kCamera).empty());
        checkVec(vec(g, "shotPlate", Transform::kTranslate), {0, 0, -50});
        CHECK(shotPlate->as<double>(ImagePlane::kWidth) == doctest::Approx(1.5 * 25.4));
        CHECK(shotPlate->as<double>(ImagePlane::kHeight) == doctest::Approx(25.4));
        CHECK(shotPlate->as<double>(ImagePlane::kOffsetX) == doctest::Approx(2.54));
        CHECK(shotPlate->as<double>(ImagePlane::kOffsetY) == doctest::Approx(-1.27));
        CHECK_FALSE(shotPlate->as<bool>(ImagePlane::kMaintainRatio));
        CHECK(shotPlate->as<int64_t>(ImagePlane::kFit) == int64_t(imagePlane::Fit::Fill));
        CHECK(shotPlate->as<double>(ImagePlane::kAlphaGain) == doctest::Approx(0.5));
        CHECK(shotPlate->as<int64_t>(ImagePlane::kPlacement) == int64_t(imagePlane::Placement::InScene));
        std::string img = shotPlate->as<std::string>(ImagePlane::kImageName);
        CHECK(img.ends_with("maya/sourceimages/ref.png"));
        CHECK(fs::exists(fs::path(std::u8string(img.begin(), img.end()))));
        imagePlane::Quad q = imagePlane::layoutFree(imagePlane::settings(*shotPlate),
                                                    std::get<glm::dmat4>(g.value({shotPlate->id(), Transform::kWorldMatrix, -1})), 2.0);
        checkVec(q.center, eye + 50.0 * view + cf.right * 2.54 - up * 1.27);
        checkVec(q.normal(), -view);

        // Planes on the default cameras attach to Lila's views.
        scene::Node* front = node(g, "frontPlate");
        CHECK(front->as<std::string>(ImagePlane::kCamera) == "front");
        CHECK(front->as<double>(ImagePlane::kOffsetX) == doctest::Approx(1.0));
        CHECK(front->as<double>(ImagePlane::kOffsetY) == doctest::Approx(2.0));
        CHECK(front->as<double>(ImagePlane::kDepth) == doctest::Approx(5.0));
        CHECK(front->as<double>(ImagePlane::kWidth) == doctest::Approx(20.0));
        CHECK(front->as<double>(ImagePlane::kHeight) == doctest::Approx(15.0));
        CHECK(front->as<int64_t>(ImagePlane::kFit) == int64_t(imagePlane::Fit::ToSize));
        CHECK(front->as<int64_t>(ImagePlane::kDisplayMode) == int64_t(imagePlane::DisplayMode::AllViews));
        CHECK(front->as<std::string>(ImagePlane::kImageName).ends_with("maya/sourceimages/front.png"));
        CHECK(contains(r.warnings, "frontPlate: image 'D:/oldproject/sourceimages/front.png' not found, using"));
        scene::Node* persp = node(g, "perspPlate");
        CHECK(persp->as<std::string>(ImagePlane::kCamera) == "persp");
        CHECK(persp->as<double>(ImagePlane::kDepth) == doctest::Approx(200.0));
        CHECK(persp->as<double>(ImagePlane::kWidth) == doctest::Approx(1.41732 * 25.4 / 35.0 * 200.0).epsilon(1e-4));

        // On a user orthographic camera: fixed in the world at its center, facing along the view.
        scene::Node* orthoPlate = node(g, "orthoPlate");
        CHECK(g.parent(orthoPlate->id()) == kNoNode);
        CHECK(orthoPlate->as<double>(ImagePlane::kWidth) == doctest::Approx(40.0));
        CHECK(orthoPlate->as<bool>(ImagePlane::kMaintainRatio));
        checkVec(vec(g, "orthoPlate", Transform::kTranslate), {0, 0, 0});

        scene::Node* free = node(g, "refFree");
        CHECK(free->type().name == "imagePlane");
        checkVec(vec(g, "refFree", Transform::kTranslate), {0, 10, -30});
        checkVec(vec(g, "refFree", Transform::kRotate), {0, 45, 0});
        CHECK(free->as<double>(ImagePlane::kWidth) == doctest::Approx(12.0));
        CHECK(free->as<double>(ImagePlane::kHeight) == doctest::Approx(8.0));
        CHECK_FALSE(free->as<bool>(ImagePlane::kMaintainRatio));
        CHECK(free->as<bool>(ImagePlane::kUseFrameExtension));
        CHECK(free->as<int64_t>(ImagePlane::kFrameOffset) == 3);
        CHECK(free->as<double>(ImagePlane::kAlphaGain) == doctest::Approx(0.75));
        CHECK(free->as<std::string>(ImagePlane::kImageName).ends_with("sourceimages/seq.0001.png"));
        CHECK_FALSE(node(g, "hiddenPlane")->as<bool>(Transform::kVisibility));
        CHECK(r.mayaNames.at("|shotCam|shotCamShape") == "shotCam");
    }
}

TEST_CASE("maya import: cameras and image planes from a Z-up file look the same way")
{
    // Maya: shotCam at (10, 5, 20) looking along view, with Z up in this file.
    const glm::dvec3 view(-0.4161977407267834, -0.17364817766693036, -0.89253893528903);
    const int upIndex = math::upAxis() == math::UpAxis::Z ? 2 : 1;
    for (const char* file : {"generated/cameras_zup.ma", "generated/cameras_zup.mb"})
    {
        CAPTURE(file);
        Graph& g = fresh();
        // mayapy's upAxis leaves the default cameras as they were, so the axis is given.
        maya::ImportResult r = load(file, {.upAxis = "z"});
        REQUIRE(r.upAxis == "z");
        REQUIRE(r.cameras.size() == 3);
        scene::Node* cam = g.node(r.cameras[0]);
        CHECK(cam->type().name == "camera");
        glm::dmat4 w = std::get<glm::dmat4>(g.value({cam->id(), Transform::kWorldMatrix, -1}));
        imagePlane::CameraFrame cf = camera::frame(*cam, w);
        CHECK(cf.eye[upIndex] == doctest::Approx(20.0));
        CHECK(cf.forward[upIndex] == doctest::Approx(view.z));
        CHECK(glm::length(cf.eye) == doctest::Approx(glm::length(glm::dvec3(10, 5, 20))));

        scene::Node* plate = node(g, "shotPlate");
        CHECK(g.parent(plate->id()) == cam->id());
        imagePlane::Quad q = imagePlane::layoutFree(imagePlane::settings(*plate),
                                                    std::get<glm::dmat4>(g.value({plate->id(), Transform::kWorldMatrix, -1})), 2.0);
        checkVec(q.normal(), -cf.forward);
        checkVec(q.center, cf.eye + 50.0 * cf.forward + cf.right * 2.54 - cf.up * 1.27);

        // With the axes turned the shape is a child of the transform.
        scene::Node* free = node(g, g.find("refFreeShape") ? "refFreeShape" : "refFree");
        CHECK(free->type().name == "imagePlane");
        glm::dmat4 fw = std::get<glm::dmat4>(g.value({free->id(), Transform::kWorldMatrix, -1}));
        CHECK(fw[3][upIndex] == doctest::Approx(-30.0));
        // Maya's free plane faces its local +Z, turned 45 degrees about Maya's Y.
        imagePlane::Quad fq = imagePlane::layoutFree(imagePlane::settings(*free), fw, 1.0);
        CHECK(fq.normal()[upIndex] == doctest::Approx(0.7071067811865476));
    }
}

TEST_CASE("maya convert: a file and a folder to .lila")
{
    fs::path out = fs::temp_directory_path() / "lila_maya_tests" / "convert";
    std::error_code ec;
    fs::remove_all(out, ec);
    fs::create_directories(out);
    fresh();
    io::markClean();

    json one = ops::execute("mayaConvert", {{"source", fixture("skin.ma").generic_string()}, {"destination", (out / "skin.lila").generic_string()}});
    REQUIRE(one.size() == 1);
    CHECK_FALSE(one[0].contains("error"));
    CHECK(fs::exists(out / "skin.lila"));

    json many = ops::execute("mayaConvert", {{"source", fixture("generated").generic_string()},
                                              {"destination", (out / "all").generic_string()},
                                              {"format", "ascii"}});
    CHECK(many.size() == 24);
    for (const json& e : many)
        CHECK_FALSE(e.contains("error"));
    CHECK(fs::exists(out / "all" / "rich_ma.lilaa"));
    CHECK(fs::exists(out / "all" / "rich_mb.lilaa"));

    io::load(out / "skin.lila");
    CHECK(Graph::current().find("elbow") != nullptr);
    CHECK(Graph::current().find("skinCluster1") != nullptr);
    fresh();
    io::setCurrentPath({});
    io::markClean();
}

TEST_CASE("maya import: unknown files fail cleanly, mayaInfo reads without importing")
{
    fresh();
    maya::ImportResult r = maya::importMaya(Graph::current(), fixture("does_not_exist.ma"));
    CHECK_FALSE(r.ok);
    json info = ops::execute("mayaInfo", {{"args", {fixture("generated/skin.mb").generic_string()}}});
    CHECK(info["format"] == "binary");
    CHECK(info["nodeTypes"]["skinCluster"] == 1);
    CHECK(info["nodeTypes"]["joint"] == 3);
    CHECK(Graph::current().nodes().empty());
}


TEST_CASE("maya import: referenced Maya files land in their namespace")
{
    Graph& g = fresh();
    maya::ImportResult r = load("with_reference.ma", {.references = "import"});
    CHECK(g.find("stage") != nullptr);
    scene::Node* box = node(g, "set:box");
    CHECK(box->nameSpace() == "set");
    CHECK(g.parent(box->id()) == node(g, "set:grp")->id());
    CHECK(contains(r.warnings, "reference 'missing.ma' not found"));
    CHECK(box->referenceId() == 0);
    CHECK(r.references.empty());

    fresh();
    maya::ImportResult flat = load("with_reference.ma", {.references = "none"});
    CHECK(g.find("set:box") == nullptr);
}

namespace
{
// Maya: xform -q -ws -m of every node of the two references in refs/refscene.ma, after the scene's edits.
const std::pair<const char*, std::array<double, 16>> kRefTruth[] = {
    {"rig:root", {-0.469846310393, 0.813797681349, -0.342020143326, 0.0, -0.882564119259, -0.44096961053, 0.163175911167, 0.0, -0.018028311236, 0.37852230637, 0.925416578398, 0.0, 1.0, 12.0, 3.0, 1.0}},
    {"rig:spine", {-0.701567896029, 1.181623607978, -0.601305360304, 0.0, 0.072344881655, -0.645169516403, -1.352228720743, 0.0, -1.323846178889, -0.661454415795, 0.24476386675, 0.0, -3.69846310393, 20.137976813494, -0.420201433257, 1.0}},
    {"rig:arm", {0.098244206935, 0.984363007584, -0.146210618983, 0.0, -0.114645600606, -0.134747971975, -0.984225264007, 0.0, -0.988536525408, 0.113456834736, 0.099614680577, 0.0, 6.17121907024, 31.913266590729, -7.141951535948, 1.0}},
    {"rig:hand", {0.098244206935, 0.984363007584, -0.146210618983, 0.0, -0.114645600606, -0.134747971975, -0.984225264007, 0.0, -0.988536525408, 0.113456834736, 0.099614680577, 0.0, 6.858928518784, 38.803807643819, -8.165425868827, 1.0}},
    {"rig:neck", {0.882564119259, 0.44096961053, -0.163175911167, 0.0, -0.449170615296, 0.688099415905, -0.569881524692, 0.0, -0.139019184828, 0.576250810343, 0.80536244625, 0.0, -10.853079887858, 32.188220483095, -6.552336964895, 1.0}},
    {"rigb:root", {-0.469846310393, 0.813797681349, -0.342020143326, 0.0, -0.882564119259, -0.44096961053, 0.163175911167, 0.0, -0.018028311236, 0.37852230637, 0.925416578398, 0.0, 1.0, 12.0, 3.0, 1.0}},
    {"rigb:spine", {-0.701567896029, 1.181623607978, -0.601305360304, 0.0, 0.072344881655, -0.645169516403, -1.352228720743, 0.0, -1.323846178889, -0.661454415795, 0.24476386675, 0.0, -3.69846310393, 20.137976813494, -0.420201433257, 1.0}},
    {"rigb:arm", {0.098244206935, 0.984363007584, -0.146210618983, 0.0, -0.114645600606, -0.134747971975, -0.984225264007, 0.0, -0.988536525408, 0.113456834736, 0.099614680577, 0.0, 6.17121907024, 31.913266590729, -7.141951535948, 1.0}},
    {"rigb:hand", {0.098244206935, 0.984363007584, -0.146210618983, 0.0, -0.114645600606, -0.134747971975, -0.984225264007, 0.0, -0.988536525408, 0.113456834736, 0.099614680577, 0.0, 6.858928518784, 38.803807643819, -8.165425868827, 1.0}},
    {"rigb:neck", {0.882564119259, 0.44096961053, -0.163175911167, 0.0, -0.449170615296, 0.688099415905, -0.569881524692, 0.0, -0.139019184828, 0.576250810343, 0.80536244625, 0.0, -10.853079887858, 32.188220483095, -6.552336964895, 1.0}},
    {"rig:props", {1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0}},
    {"rig:marker", {1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, -4.0, 2.0, 3.0, 1.0}},
    {"rigb:props", {1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0}},
    {"rigb:marker", {1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, -4.0, 2.0, 3.0, 1.0}},
};

/** The refs fixtures copied somewhere writable, since referencing writes .lila files beside the Maya ones. */
fs::path refsCopy(const std::string& name)
{
    fs::path dir = fs::temp_directory_path() / "lila_maya_tests" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    for (const auto& e : fs::directory_iterator(fixture("refs")))
        fs::copy_file(e.path(), dir / e.path().filename(), fs::copy_options::overwrite_existing);
    return dir;
}

void checkRefWorlds(Graph& g)
{
    g.evaluate();
    for (const auto& [name, m] : kRefTruth)
    {
        const std::string node = name;
        CAPTURE(node);
        scene::Node* n = g.find(name);
        REQUIRE(n);
        glm::dmat4 w = n->as<glm::dmat4>(Transform::kWorldMatrix);
        double err = 0.0;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                err = std::max(err, std::abs(w[c][r] - m[size_t(c * 4 + r)]));
        CHECK(err < 1e-6);
    }
}

std::string bytesOf(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
} // namespace

TEST_CASE("maya references: referenced skeletons become Lila references with Maya's edits")
{
    for (std::string scene : {"refscene.ma", "refscene.mb"})
    {
        CAPTURE(scene);
        fs::path dir = refsCopy(std::string("ref_") + scene);
        Graph& g = fresh();
        maya::ImportResult r = maya::importMaya(g, dir / scene);
        REQUIRE(r.ok);
        CHECK(r.references == std::vector<std::string>{"rig", "rigb"});
        REQUIRE(io::refs::find("rig"));
        REQUIRE(io::refs::find("rigb"));
        CHECK(fs::exists(dir / "skeleton.lila"));
        CHECK(fs::exists(dir / "skeleton_mb.lila"));
        CHECK(g.find("rig:root")->referenceId() != 0);
        CHECK(g.parent(g.find("rig:props")->id()) == g.find("stage")->id());
        CHECK_FALSE(g.find("rig:neck")->as<bool>(Transform::kVisibility));
        CHECK(contains(r.skipped, "addAttr"));
        checkRefWorlds(g);

        // The edits are Lila reference overrides now: they survive a save and reopen.
        fs::path saved = dir / "converted_scene.lila";
        io::save(saved);
        fresh();
        io::load(saved);
        checkRefWorlds(Graph::current());
        CHECK(Graph::current().find("rig:root")->referenceId() != 0);

        // A second import reuses the converted skeletons.
        fresh();
        maya::ImportResult again = maya::importMaya(Graph::current(), dir / scene);
        if (!again.ok)
            MESSAGE(again.error);
        REQUIRE(again.ok);
        CHECK(again.converted.empty());
        checkRefWorlds(Graph::current());
    }
    fresh();
    io::setCurrentPath({});
    io::markClean();
}

TEST_CASE("maya references: import mode, and a .lila the converter did not write is left alone")
{
    fs::path dir = refsCopy("ref_import");
    Graph& g = fresh();
    maya::ImportResult r = maya::importMaya(g, dir / "refscene.ma", {.references = "import"});
    REQUIRE(r.ok);
    CHECK(r.references.empty());
    CHECK(g.find("rig:root")->referenceId() == 0);
    checkRefWorlds(g);

    // Someone else's skeleton.lila, older than the Maya file: kept, and the conversion goes beside it.
    fresh();
    fs::path theirs = dir / "skeleton.lila";
    io::save(theirs);
    const std::string before = bytesOf(theirs);
    fs::last_write_time(theirs, fs::last_write_time(dir / "skeleton.ma") - std::chrono::hours(1));
    fresh();
    maya::ImportResult ref = maya::importMaya(Graph::current(), dir / "refscene.ma");
    REQUIRE(ref.ok);
    CHECK(bytesOf(theirs) == before);
    CHECK(fs::exists(dir / "skeleton_ma.lila"));
    checkRefWorlds(Graph::current());
    fresh();
    io::setCurrentPath({});
    io::markClean();
}

TEST_CASE("maya references: a reference Maya saved unloaded comes over unloaded, its edits kept")
{
    for (std::string scene : {"unloaded.ma", "unloaded.mb"})
    {
        CAPTURE(scene);
        fs::path dir = refsCopy(std::string("ref_unloaded_") + scene);
        maya::File f = maya::readFile(dir / scene);
        REQUIRE(f.references.size() == 2);
        CHECK_FALSE(f.references[0].loaded);
        CHECK(f.references[1].loaded);

        Graph& g = fresh();
        maya::ImportResult r = maya::importMaya(g, dir / scene);
        REQUIRE(r.ok);
        CHECK(r.references == std::vector<std::string>{"rig", "rigb"});
        io::Reference* rig = io::refs::find("rig");
        REQUIRE(rig);
        CHECK_FALSE(rig->loaded);
        CHECK(g.find("rig:root") == nullptr);
        CHECK(g.find("rigb:root") != nullptr);
        for (NodeId id : r.nodes)
            CHECK(g.node(id) != nullptr);

        std::vector<std::string> warnings;
        io::refs::load(*rig, warnings);
        CHECK(g.find("rig:spine")->as<glm::dvec3>(Transform::kTranslate).x == doctest::Approx(5.0));

        fresh();
        maya::ImportResult flat = maya::importMaya(Graph::current(), dir / scene, {.references = "import"});
        REQUIRE(flat.ok);
        CHECK(Graph::current().find("rig:root") == nullptr);
        CHECK(Graph::current().find("rigb:root") != nullptr);
        CHECK(contains(flat.skipped, "unloaded"));
    }
    fresh();
    io::setCurrentPath({});
    io::markClean();
}

TEST_CASE("maya convert: an unloaded reference is written unloaded")
{
    fs::path dir = refsCopy("ref_convert_unloaded");
    fresh();
    io::markClean();
    json res = ops::execute("mayaConvert", {{"source", (dir / "unloaded.mb").generic_string()},
                                             {"destination", (dir / "out" / "scene.lila").generic_string()}});
    REQUIRE(res.size() == 1);
    CHECK_FALSE(res[0].contains("error"));
    io::load(dir / "out" / "scene.lila");
    REQUIRE(io::refs::find("rig"));
    CHECK_FALSE(io::refs::find("rig")->loaded);
    CHECK(io::refs::find("rigb")->loaded);
    CHECK(Graph::current().find("rig:root") == nullptr);
    fresh();
    io::setCurrentPath({});
    io::markClean();
}

TEST_CASE("maya references: animation and connections through reference placeholders")
{
    for (const char* scene : {"refedits2.ma", "refedits2.mb"})
    {
        CAPTURE(scene);
        fs::path dir = refsCopy(std::string("ref_ph_") + scene);
        Graph& g = fresh();
        maya::ImportResult r = maya::importMaya(g, dir / scene);
        REQUIRE(r.ok);
        NodeId hand = node(g, "rig:hand")->id(), spine = node(g, "rig:spine")->id(), driver = node(g, "driver")->id();
        auto curve = g.source({hand, Transform::kRotate, 2});
        REQUIRE(curve.has_value());
        CHECK(anim::isCurve(*g.node(curve->node)));
        auto drive = g.source({spine, Transform::kTranslate, 0});
        REQUIRE(drive.has_value());
        CHECK(drive->node == driver);
        auto back = g.source({driver, Transform::kTranslate, 1});
        REQUIRE(back.has_value());
        CHECK(back->node == node(g, "rig:root")->id());
        CHECK(contains(r.skipped, "lock"));
    }
    fresh();
}

TEST_CASE("maya convert: a scene with references converts to a .lila that references the skeletons")
{
    fs::path dir = refsCopy("ref_convert");
    fresh();
    io::markClean();
    json res = ops::execute("mayaConvert", {{"source", (dir / "refscene.mb").generic_string()},
                                             {"destination", (dir / "out" / "scene.lila").generic_string()}});
    REQUIRE(res.size() == 1);
    CHECK_FALSE(res[0].contains("error"));
    io::load(dir / "out" / "scene.lila");
    checkRefWorlds(Graph::current());
    CHECK(io::refs::find("rig") != nullptr);
    fresh();
    io::setCurrentPath({});
    io::markClean();
}

TEST_CASE("maya references: an OBJ reference becomes a Lila reference through a converted .lila")
{
    fs::path dir = fs::temp_directory_path() / "lila_maya_tests" / "ref_obj";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    std::ofstream(dir / "crate.obj") << "o Crate\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\n";
    std::ofstream(dir / "scene.ma") << "//Maya ASCII 2026 scene\n"
                                       "file -r -ns \"prop\" -dr 1 -rfn \"propRN\" -typ \"OBJ\" \"crate.obj\";\n"
                                       "currentUnit -l centimeter -a degree -t film;\n"
                                       "createNode reference -n \"propRN\";\n"
                                       "\tsetAttr \".ed\" -type \"dataReferenceEdits\" \"propRN\" \"propRN\" 0 \"propRN\" 1\n"
                                       "\t\t2 \"|prop:Crate\" \"translate\" \" -type \\\"double3\\\" 0 5 0\";\n";
    Graph& g = fresh();
    maya::ImportResult r = maya::importMaya(g, dir / "scene.ma");
    REQUIRE(r.ok);
    REQUIRE(r.references.size() == 1);
    CHECK(fs::exists(dir / "crate.lila"));
    scene::Node* crate = node(g, "prop:Crate");
    CHECK(crate->referenceId() != 0);
    checkVec(crate->as<glm::dvec3>(Transform::kTranslate), {0, 5, 0});
    fresh();
}

TEST_CASE("maya references: edits on an FBX reference's joints pose them as in Maya")
{
    fs::path dir = fs::temp_directory_path() / "lila_maya_tests" / "ref_fbx_pose";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    // Z-up like an Unreal export. Maya turns only Rig onto its Y-up axes, so the edit on a is in the file's axes.
    std::ofstream(dir / "rig.fbx") << "; FBX 7.4.0 project file\n"
                                      "FBXHeaderExtension:  {\n\tFBXHeaderVersion: 1003\n\tFBXVersion: 7400\n}\n"
                                      "GlobalSettings:  {\n\tVersion: 1000\n\tProperties70:  {\n"
                                      "\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",2\n"
                                      "\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
                                      "\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",1\n"
                                      "\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",-1\n"
                                      "\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
                                      "\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
                                      "\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1\n\t}\n}\n"
                                      "Objects:  {\n"
                                      "\tNodeAttribute: 1001, \"NodeAttribute::a\", \"LimbNode\" {\n\t\tTypeFlags: \"Skeleton\"\n\t}\n"
                                      "\tNodeAttribute: 1002, \"NodeAttribute::b\", \"LimbNode\" {\n\t\tTypeFlags: \"Skeleton\"\n\t}\n"
                                      "\tModel: 2000, \"Model::Rig\", \"Null\" {\n\t\tVersion: 232\n\t}\n"
                                      "\tModel: 2001, \"Model::a\", \"LimbNode\" {\n\t\tVersion: 232\n\t\tProperties70:  {\n"
                                      "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,0,10\n\t\t}\n\t}\n"
                                      "\tModel: 2002, \"Model::b\", \"LimbNode\" {\n\t\tVersion: 232\n\t\tProperties70:  {\n"
                                      "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",5,0,0\n\t\t}\n\t}\n"
                                      "}\n"
                                      "Connections:  {\n\tC: \"OO\",2000,0\n\tC: \"OO\",2001,2000\n\tC: \"OO\",2002,2001\n"
                                      "\tC: \"OO\",1001,2001\n\tC: \"OO\",1002,2002\n}\n";
    std::ofstream(dir / "scene.ma") << "//Maya ASCII 2026 scene\n"
                                       "file -r -ns \"rig\" -dr 1 -rfn \"rigRN\" -op \"fbx\" -typ \"FBX\" \"rig.fbx\";\n"
                                       "currentUnit -l centimeter -a degree -t film;\n"
                                       "createNode transform -n \"wantA\";\n"
                                       "\tsetAttr \".t\" -type \"double3\" 0 10 0 ;\n"
                                       "createNode transform -n \"wantB\";\n"
                                       "\tsetAttr \".t\" -type \"double3\" 0 10 -5 ;\n"
                                       "createNode reference -n \"rigRN\";\n"
                                       "\tsetAttr \".ed\" -type \"dataReferenceEdits\" \"rigRN\" \"rigRN\" 0 \"rigRN\" 2\n"
                                       "\t\t2 \"|rig:Rig\" \"translate\" \" -type \\\"double3\\\" 0 0 0\"\n"
                                       "\t\t2 \"|rig:Rig|rig:a\" \"rotate\" \" -type \\\"double3\\\" 0 0 90\";\n";
    const math::UpAxis was = math::upAxis();
    for (math::UpAxis up : {math::UpAxis::Z, math::UpAxis::Y})
    {
        CAPTURE(int(up));
        math::setUpAxis(up);
        fs::remove(dir / "rig.lila", ec);
        Graph& g = fresh();
        maya::ImportResult r = maya::importMaya(g, dir / "scene.ma");
        REQUIRE(r.ok);
        REQUIRE(r.references.size() == 1);
        // a at 10 up, b 5 along a's X after a quarter turn about up: (0, 5, 10) in the file, where Maya has wantB.
        auto pos = [&](const char* n) { return glm::dvec3(std::get<glm::dmat4>(g.value({node(g, n)->id(), Transform::kWorldMatrix, -1}))[3]); };
        checkVec(pos("rig:a"), pos("wantA"));
        checkVec(pos("rig:b"), pos("wantB"));
    }
    math::setUpAxis(was);
    fresh();
}

TEST_CASE("maya import: display layers from .ma and .mb")
{
    namespace dl = scene::displayLayers;
    auto lin = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    for (const char* file : {"generated/layers.ma", "generated/layers.mb"})
    {
        CAPTURE(file);
        Graph& g = fresh();
        maya::ImportResult r = load(file);
        CHECK(r.displayLayers == std::vector<std::string>{"hiddenLayer", "templateLayer", "refLayer", "groupLayer", "emptyLayer"});
        CHECK(g.find("defaultLayer") == nullptr);
        CHECK_FALSE(contains(r.skipped, "displayLayer"));
        auto layerOf = [&](const char* n) {
            const scene::Node* l = dl::layerOf(g, node(g, n)->id());
            return l ? l->name() : std::string();
        };

        CHECK(layerOf("boxA") == "hiddenLayer");
        CHECK_FALSE(effectivelyVisible(g, node(g, "boxA")->id()));
        CHECK(node(g, "boxA")->as<bool>(Transform::kVisibility));

        CHECK(layerOf("boxB") == "templateLayer");
        CHECK(layerOf("marker") == "templateLayer");
        dl::Display t = dl::display(g, node(g, "boxB")->id());
        CHECK(t.type == dl::DisplayType::Template);
        REQUIRE(t.color);
        checkVec(*t.color, {1, 0, 0});
        CHECK_FALSE(dl::pickable(g, node(g, "marker")->id()));

        CHECK(layerOf("ball") == "refLayer");
        scene::Node* ref = node(g, "refLayer");
        CHECK(ref->as<int64_t>(DisplayLayer::kDisplayType) == 2);
        CHECK(ref->as<bool>(DisplayLayer::kHideOnPlayback));
        CHECK(ref->as<bool>(DisplayLayer::kUseColor));
        checkVec(ref->as<glm::dvec3>(DisplayLayer::kColor), {lin(0.2), lin(0.4), lin(0.8)});

        // A shape in a layer puts Lila's merged mesh in it; children of a member follow the layer.
        CHECK(layerOf("grp") == "groupLayer");
        CHECK(layerOf("boxD") == "groupLayer");
        CHECK(layerOf("inner").empty());
        REQUIRE(dl::display(g, node(g, "inner")->id()).color);
        checkVec(*dl::display(g, node(g, "inner")->id()).color, {0, 0, 1});

        CHECK(dl::members(g, node(g, "emptyLayer")->id()).empty());
        CHECK_FALSE(node(g, "emptyLayer")->as<bool>(DisplayLayer::kUseColor));
        std::vector<std::string> order;
        for (const scene::Node* l : dl::all(g))
            order.push_back(l->name());
        CHECK(order == r.displayLayers);
    }
}

TEST_CASE("maya convert: file paths are stored relative to the converted scene")
{
    // Everything under one temp folder, so the paths share a drive and a relative form exists.
    fs::path proj = fs::temp_directory_path() / "lila_maya_tests" / "relpaths";
    std::error_code ec;
    fs::remove_all(proj, ec);
    for (const char* d : {"scenes", "sourceimages", "textures", "out"})
        fs::create_directories(proj / d);
    for (const char* f : {"ref.png", "front.png", "seq.0001.png"})
        fs::copy_file(fixture("sourceimages") / f, proj / "sourceimages" / f);
    fs::copy_file(fixture("sourceimages/ref.png"), proj / "textures" / "paint_bc.png");
    fs::copy_file(fixture("generated/cameras.ma"), proj / "scenes" / "cameras.ma");
    fs::copy_file(fixture("generated/materials.ma"), proj / "scenes" / "materials.ma");
    fs::copy_file(fixture("with_reference.ma"), proj / "scenes" / "with_reference.ma");
    fs::copy_file(fixture("cube_hier.ma"), proj / "scenes" / "cube_hier.ma");
    fresh();
    io::markClean();

    json res = ops::execute("mayaConvert", {{"source", (proj / "scenes").generic_string()}, {"destination", (proj / "out").generic_string()},
                                             {"format", "ascii"}});
    REQUIRE(res.size() == 4);
    for (const json& e : res)
        CHECK_FALSE(e.contains("error"));

    auto attrsOf = [](const json& doc, const std::string& attr) {
        std::vector<std::string> out;
        for (const json& n : doc["nodes"])
            if (n.contains("attrs") && n["attrs"].contains(attr))
                out.push_back(n["attrs"][attr].get<std::string>());
        return out;
    };
    json cams = io::readSceneFile(proj / "out" / "cameras.lilaa", nullptr);
    std::vector<std::string> images = attrsOf(cams, "imageName");
    CHECK(images.size() == 6);
    for (const std::string& p : images)
    {
        CAPTURE(p);
        CHECK(p.starts_with("../sourceimages/"));
    }
    json mats = io::readSceneFile(proj / "out" / "materials.lilaa", nullptr);
    CHECK(attrsOf(mats, "baseColorMap") == std::vector<std::string>{"../textures/paint_bc.png"});

    json withRef = io::readSceneFile(proj / "out" / "with_reference.lilaa", nullptr);
    REQUIRE(withRef["references"].size() == 1);
    CHECK(withRef["references"][0]["path"] == "../scenes/cube_hier.lila");
    // The reference's own conversion records its source beside it.
    CHECK(io::readSceneHeader(proj / "scenes" / "cube_hier.lila")["mayaSource"]["path"] == "cube_hier.ma");

    fresh();
    io::setCurrentPath({});
    io::markClean();
}

TEST_CASE("maya import: lodGroup keeps its level order and screen height percentages")
{
    Graph& g = fresh();
    maya::ImportResult r = load("lod.ma");
    scene::Node* rock = node(g, "rockLod");
    CHECK(rock->type().name == "lodGroup");
    CHECK(rock->as<glm::dvec3>(Transform::kTranslate).x == doctest::Approx(10.0));
    CHECK(lod::parseSizes(rock->as<std::string>(LodGroup::kScreenSizes)) == std::vector<double>{1.0, 0.4, 0.15});
    const std::vector<NodeId> levels = lod::levels(g, rock->id());
    REQUIRE(levels.size() == 3);
    CHECK(g.node(levels[0])->name() == "z_high");
    CHECK(g.node(levels[1])->name() == "a_mid");
    CHECK(g.node(levels[2])->name() == "m_low");

    scene::Node* tree = node(g, "treeLod");
    CHECK(tree->type().name == "lodGroup");
    CHECK(tree->as<std::string>(LodGroup::kScreenSizes).empty());
    CHECK(std::ranges::any_of(r.warnings, [](const std::string& w) { return w.find("treeLod") != std::string::npos; }));
}

TEST_CASE("maya import: a mesh shape parented under a second transform becomes a Lila instance")
{
    Graph& g = fresh();
    std::ifstream in(fixture("cube_hier.ma"), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    text += "createNode transform -n \"box2\";\n"
            "\tsetAttr \".t\" -type \"double3\" 10 0 0 ;\n"
            "createNode transform -n \"marker2\";\n"
            "parent -s -nc -r -add \"|grp|box|boxShape\" \"box2\";\n"
            "parent -s -nc -r -add \"|marker|markerShape\" \"marker2\";\n";
    fs::path path = fs::temp_directory_path() / "lila_maya_instances.ma";
    std::ofstream(path, std::ios::binary) << text;

    maya::ImportResult r = maya::importMaya(g, path, maya::ImportOptions{});
    REQUIRE(r.ok);
    scene::Node* box = node(g, "box");
    scene::Node* box2 = node(g, "box2");
    CHECK(box2->type().name == "mesh");
    CHECK(instancing::kind(g, box2->id()) == instancing::Kind::Instance);
    CHECK(instancing::source(g, box2->id()) == box->id());
    CHECK(outMesh(g, "box2") == outMesh(g, "box"));
    checkVec(vec(g, "box2", Transform::kTranslate), {10, 0, 0});
    // Only mesh shapes convert; a shared locator shape is reported.
    CHECK(contains(r.skipped, "markerShape"));
    CHECK_FALSE(contains(r.skipped, "boxShape"));
    fs::remove(path);
}

TEST_CASE("maya import: lights")
{
    for (const char* file : {"lights.ma", "lights_zup.ma"})
    {
        CAPTURE(file);
        Graph& g = fresh();
        maya::ImportResult r = load(file);
        CHECK(r.lights.size() == 5);
        CHECK(contains(r.skipped, "aiMeshLight"));
        // The light looks down its local -Z in both apps, so this is where Maya's key light points.
        auto aim = [&](const std::string& name) {
            scene::Node* n = node(g, name);
            for (NodeId c : g.children(n->id()))
                if (g.node(c)->type().isA("light"))
                    n = g.node(c);
            REQUIRE(n->type().isA("light"));
            const glm::dmat4 w = n->as<glm::dmat4>(Transform::kWorldMatrix);
            return std::pair(n, glm::normalize(glm::dvec3(w * glm::dvec4(0, 0, -1, 0))));
        };
        auto [key, dir] = aim("key");
        checkVec(dir, math::upAxis() == math::UpAxis::Z ? glm::dvec3(0, 0, -1) : glm::dvec3(0, -1, 0), 1e-9);
        lights::Settings s = lights::settings(*key);
        CHECK(s.kind == lights::Kind::Spot);
        checkVec(s.color, {1, 0.5, 0.25});
        CHECK(s.intensity == doctest::Approx(4.0)); // exposure 1 doubles it
        CHECK(s.useTemperature);
        CHECK(s.temperature == doctest::Approx(3200.0));
        CHECK(s.outerConeAngle == doctest::Approx(35.0));
        CHECK(s.innerConeAngle == doctest::Approx(30.0));
        CHECK(s.sourceFormat == "maya:spotLight");
        CHECK(s.extra.find("\"decayRate\":2") != std::string::npos);

        lights::Settings bulb = lights::settings(*aim("bulb").first);
        CHECK(bulb.kind == lights::Kind::Point);
        CHECK(bulb.sourceRadius == doctest::Approx(4.0));
        CHECK(bulb.castShadows);
        CHECK(lights::settings(*aim("sun").first).kind == lights::Kind::Directional);
        lights::Settings panel = lights::settings(*aim("panel").first);
        CHECK(panel.kind == lights::Kind::Area);
        CHECK(panel.width == doctest::Approx(2.0));
        CHECK(panel.extra.find("\"emitDiffuse\":false") != std::string::npos);
        lights::Settings sky = lights::settings(*aim("sky").first);
        CHECK(sky.kind == lights::Kind::Dome);
        CHECK(sky.intensity == doctest::Approx(0.5));
    }
}

TEST_CASE("maya import: light linking bakes into lighting channels")
{
    // keyShape is broken from boxB; fill lights both through the default links, so only key gets a channel.
    const std::string text = "//Maya ASCII 2026 scene\n"
                             "requires maya \"2026\";\n"
                             "currentUnit -l centimeter -a degree -t film;\n"
                             "createNode transform -s -n \"top\";\n"
                             "\tsetAttr \".t\" -type \"double3\" 0 1000.1 0 ;\n"
                             "\tsetAttr \".r\" -type \"double3\" -90 0 0 ;\n"
                             "createNode camera -s -n \"topShape\" -p \"top\";\n"
                             "createNode polyCube -n \"polyCube1\";\n"
                             "createNode polyCube -n \"polyCube2\";\n"
                             "createNode transform -n \"boxA\";\n"
                             "createNode mesh -n \"boxAShape\" -p \"boxA\";\n"
                             "createNode transform -n \"boxB\";\n"
                             "createNode mesh -n \"boxBShape\" -p \"boxB\";\n"
                             "createNode transform -n \"key\";\n"
                             "createNode pointLight -n \"keyShape\" -p \"key\";\n"
                             "createNode transform -n \"fill\";\n"
                             "createNode pointLight -n \"fillShape\" -p \"fill\";\n"
                             "createNode lightLinker -s -n \"lightLinker1\";\n"
                             "connectAttr \"polyCube1.out\" \"boxAShape.i\";\n"
                             "connectAttr \"polyCube2.out\" \"boxBShape.i\";\n"
                             "connectAttr \"boxAShape.iog\" \":initialShadingGroup.dsm\" -na;\n"
                             "connectAttr \"boxBShape.iog\" \":initialShadingGroup.dsm\" -na;\n"
                             "connectAttr \"key.iog\" \":defaultLightSet.dsm\" -na;\n"
                             "connectAttr \"fill.iog\" \":defaultLightSet.dsm\" -na;\n"
                             "relationship \"link\" \":lightLinker1\" \":initialShadingGroup.message\" \":defaultLightSet.message\";\n"
                             "relationship \"ignore\" \":lightLinker1\" \"|boxB|boxBShape.message\" \"|key|keyShape.message\";\n";
    const maya::File file = maya::readAscii(text);
    REQUIRE(file.relationships.size() == 2);
    CHECK(file.relationships[1].type == "ignore");
    CHECK(file.relationships[1].plugs == std::vector<std::string>{"|boxB|boxBShape.message", "|key|keyShape.message"});

    Graph& g = fresh();
    maya::ImportResult r = maya::importMaya(g, "linked.ma", file);
    REQUIRE(r.ok);
    REQUIRE(r.lightingChannels.size() == 1);
    namespace lc = scene::lightingChannels;
    auto lightNode = [&](const char* name) -> const scene::Node& {
        for (scene::NodeId id : r.lights)
            if (const scene::Node* n = g.node(id); n && n->path().find(name) != std::string::npos)
                return *n;
        FAIL("no light ", name);
        return *g.node(r.lights.front());
    };
    CHECK(lc::lights(lightNode("key"), *node(g, "boxA")));
    CHECK_FALSE(lc::lights(lightNode("key"), *node(g, "boxB")));
    CHECK(lc::lights(lightNode("fill"), *node(g, "boxA")));
    CHECK(lc::lights(lightNode("fill"), *node(g, "boxB")));

    Graph& again = fresh();
    maya::ImportResult plain = maya::importMaya(again, "linked.ma", file, {.lightingChannels = false});
    REQUIRE(plain.ok);
    CHECK(plain.lightingChannels.empty());
    CHECK(lc::nodesWithChannels(again).empty());
}
