#include <doctest/doctest.h>

#include "blend/Blend.h"
#include "blend/BlendFile.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "io/References.h"
#include "scene/Camera.h"
#include "scene/Diagnostics.h"
#include "scene/Graph.h"
#include "scene/Light.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Morphs.h"
#include "scene/Skin.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>

using namespace lila;
using namespace lila::scene;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
/** The fixture folders: Blender 5.2 at the top, 5.0 and 4.2 (the older file format) below. */
const char* kVersions[] = {"", "v50", "v42"};

fs::path fixture(const std::string& version, const std::string& name) { return fs::path(LILA_BLEND_TEST_DATA) / version / name; }

json oracle(const std::string& version, const std::string& name)
{
    std::ifstream f(fixture(version, name + ".json"));
    REQUIRE(f);
    return json::parse(f);
}

Graph& fresh()
{
    io::refs::clear();
    Graph& g = Graph::current();
    g.clear();
    g.setTime(0.0);
    return g;
}

blend::ImportResult load(const std::string& version, const std::string& name, blend::ImportOptions opts = {})
{
    opts.textureDir = (fs::temp_directory_path() / "lila_blend_tests" / "textures" / (version.empty() ? "v52" : version)).string();
    blend::ImportResult r = blend::importBlend(Graph::current(), fixture(version, name + ".blend"), opts);
    if (!r.ok)
        MESSAGE(r.error);
    REQUIRE(r.ok);
    return r;
}

Node* node(Graph& g, const std::string& name)
{
    Node* n = g.find(name);
    if (!n)
        MESSAGE("missing node " << name);
    REQUIRE(n);
    return n;
}

MeshPtr outMesh(Graph& g, const std::string& name)
{
    Node* n = node(g, name);
    MeshPtr m = std::get<MeshPtr>(g.value({n->id(), MeshNode::kOutMesh, -1}));
    REQUIRE(m);
    return m;
}

glm::dmat4 worldOf(Graph& g, const std::string& name)
{
    g.evaluate();
    return node(g, name)->as<glm::dmat4>(Transform::kWorldMatrix);
}

/** Blender Z up metres to Lila's default Y up centimetres. */
const glm::dmat3 F(glm::dvec3(1, 0, 0), glm::dvec3(0, 0, -1), glm::dvec3(0, 1, 0));

glm::dvec3 point(const json& p) { return F * glm::dvec3(p[0].get<double>(), p[1].get<double>(), p[2].get<double>()) * 100.0; }

glm::dmat4 blenderWorld(const json& rows, bool turned)
{
    glm::dmat4 b(1.0);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            b[c][r] = rows[r][c].get<double>();
    glm::dmat4 out(F * glm::dmat3(b) * glm::transpose(F));
    out[3] = glm::dvec4(F * glm::dvec3(b[3]) * 100.0, 1.0);
    if (turned)
        out = out * glm::dmat4(F);
    return out;
}

void checkMatrix(const glm::dmat4& a, const glm::dmat4& b, double eps = 1e-3)
{
    double worst = 0.0;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            worst = std::max(worst, std::abs(a[c][r] - b[c][r]) / (c == 3 ? 100.0 : 1.0));
    INFO("translation got " << a[3].x << " " << a[3].y << " " << a[3].z << ", want " << b[3].x << " " << b[3].y << " " << b[3].z);
    CHECK(worst < eps);
}

void checkVec(const glm::dvec3& a, const glm::dvec3& b, double eps)
{
    INFO("got " << a.x << " " << a.y << " " << a.z << ", want " << b.x << " " << b.y << " " << b.z);
    CHECK(glm::length(a - b) < eps);
}

std::pair<glm::dvec3, glm::dvec3> bounds(const geo::Mesh& m)
{
    glm::dvec3 lo(1e300), hi(-1e300);
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            lo = glm::min(lo, m.position(p));
            hi = glm::max(hi, m.position(p));
        }
    return {lo, hi};
}

bool contains(const std::vector<std::string>& lines, std::string_view what)
{
    for (const std::string& s : lines)
        if (s.find(what) != std::string::npos)
            return true;
    return false;
}
} // namespace

TEST_CASE("blend file: header, struct descriptions and datablocks")
{
    for (const char* v : kVersions)
    {
        CAPTURE(v);
        const blend::File f(fixture(v, "basic.blend"));
        const json o = oracle(v, "basic");
        CHECK(f.versionString().substr(0, 3) == o["blender"].get<std::string>().substr(0, 3));
        CHECK(f.ids("OB").size() == o["objects"].size());
        const blend::Struct* object = f.type("Object");
        REQUIRE(object);
        CHECK(object->field("loc"));
        CHECK(object->field("loc")->count == 3);
        CHECK(object->isId);
        bool box = false;
        for (const blend::View& ob : f.ids("OB"))
            box = box || ob.idName() == "Box";
        CHECK(box);
        const json i = blend::info(f);
        CHECK(i["objectTypes"]["mesh"].get<int>() == 5);
        CHECK(i["packedImages"].get<int>() == 1);
    }
    CHECK(blend::isBlendFile("a/b.blend"));
    CHECK(blend::isBlendFile("a/b.BLEND1"));
    CHECK_FALSE(blend::isBlendFile("a/b.blender"));
}

TEST_CASE("blend import: objects, transforms, cameras and lights match Blender")
{
    for (const char* v : kVersions)
    {
        CAPTURE(v);
        Graph& g = fresh();
        const blend::ImportResult r = load(v, "basic");
        const json o = oracle(v, "basic");
        for (const auto& [name, ob] : o["objects"].items())
        {
            CAPTURE(name);
            const std::string type = ob["type"];
            // Blender leaves a viewport-hidden object's world matrix unevaluated.
            if (name == "Hidden")
                continue;
            REQUIRE(r.objectNames.count(name));
            checkMatrix(worldOf(g, r.objectNames.at(name)), blenderWorld(ob["world"], type == "CAMERA" || type == "LIGHT"));
        }
        // Hierarchy, collections and visibility.
        CHECK(g.parent(node(g, "Ball")->id()) == node(g, "Box")->id());
        CHECK(g.find("Props"));
        CHECK(g.find("Inner"));
        CHECK(g.parent(node(g, "Box")->id()) == node(g, "Props")->id());
        CHECK_FALSE(node(g, "Hidden")->as<bool>(Transform::kVisibility));

        // Meshes: topology, UVs, colors, sharp edges, creases, attributes, materials.
        MeshPtr box = outMesh(g, "Box");
        CHECK(box->numPoints() == 8);
        CHECK(box->numFaces() == 6);
        CHECK_FALSE(box->cornerAttrs().get<glm::vec2>(geo::attr::uvName(0)).empty());
        CHECK_FALSE(box->cornerAttrs().get<glm::vec2>(geo::attr::uvName(1)).empty());
        CHECK(geo::colors::has(*box, "Col"));
        CHECK(geo::colors::has(*box, "PointCol"));
        CHECK_FALSE(box->pointAttrs().get<float>("wear").empty());
        CHECK(box->pointAttrs().get<float>("wear")[3] == doctest::Approx(1.5));
        std::span<const float> crease = box->edgeAttrs().get<float>(geo::attr::kCrease);
        REQUIRE_FALSE(crease.empty());
        CHECK(std::count(crease.begin(), crease.end(), 1.0f) == 1);
        std::span<const bool> hard = box->edgeAttrs().get<bool>(geo::attr::kHard);
        REQUIRE_FALSE(hard.empty());
        CHECK(std::count(hard.begin(), hard.end(), true) >= 4);
        REQUIRE(box->materialNames().size() == 2);
        CHECK(box->materialNames()[0] == "Red");
        CHECK(box->materialNames()[1] == "Blue");
        Node* red = materials::find(g, "Red");
        REQUIRE(red);
        CHECK(red->as<double>(material::kMetallic) == doctest::Approx(1.0));
        CHECK(red->as<double>(material::kRoughness) == doctest::Approx(0.3));
        Node* blue = materials::find(g, "Blue");
        REQUIRE(blue);
        const std::string map = blue->as<std::string>(material::kBaseColorMap);
        CHECK(fs::exists(fs::path(map)));
        CHECK(blue->as<double>(material::kEmissiveIntensity) == doctest::Approx(2.0));

        // Box and Twin share their mesh.
        CHECK(outMesh(g, "Twin")->numPoints() == 8);

        // The camera's lens and the lights' kinds.
        CHECK(node(g, "Cam")->as<double>(Camera::kFocalLength) == doctest::Approx(35.0));
        CHECK(node(g, "Cam")->as<double>(Camera::kNearClipPlane) == doctest::Approx(20.0));
        CHECK(node(g, "Ortho")->as<bool>(Camera::kOrthographic));
        CHECK(lights::settings(*node(g, "Sun")).kind == lights::Kind::Directional);
        CHECK(lights::settings(*node(g, "Spot")).kind == lights::Kind::Spot);
        CHECK(lights::settings(*node(g, "Spot")).outerConeAngle == doctest::Approx(30.0));
        CHECK(lights::settings(*node(g, "Area")).kind == lights::Kind::Area);
        CHECK(lights::settings(*node(g, "Area")).width == doctest::Approx(200.0));
        CHECK(lights::settings(*node(g, "Point")).intensity == doctest::Approx(100.0));
    }
}

TEST_CASE("blend import: modifiers come in live and match Blender's result")
{
    for (const char* v : kVersions)
    {
        CAPTURE(v);
        Graph& g = fresh();
        const blend::ImportResult r = load(v, "modifiers");
        const json o = oracle(v, "modifiers");
        g.evaluate();
        // Same topology as Blender's evaluated mesh.
        for (const char* name : {"Mirror", "Subsurf", "Array", "Triangulate", "Solidify"})
        {
            CAPTURE(name);
            MeshPtr m = outMesh(g, name);
            CHECK(m->numPoints() == o["objects"][name]["verts"].get<size_t>());
            CHECK(m->numFaces() == o["objects"][name]["faces"].get<size_t>());
        }
        // Same extent.
        for (const char* name : {"Mirror", "Subsurf", "Bevel", "Array", "Solidify", "Triangulate"})
        {
            CAPTURE(name);
            const auto [lo, hi] = bounds(*outMesh(g, name));
            const json& b = o["objects"][name]["bounds"];
            const glm::dvec3 a = point(b[0]), c = point(b[1]);
            // Blender places subdivided points on the limit surface; Lila's stay on the last level's cage.
            const double eps = std::string(name) == "Subsurf" ? 4.0 : 0.5;
            checkVec(lo, glm::min(a, c), eps);
            checkVec(hi, glm::max(a, c), eps);
        }
        // The decimate keeps fewer faces; the wave is dropped and flagged.
        CHECK(outMesh(g, "Decimate")->numFaces() < 32);
        CHECK(contains(r.skipped, "Wave"));
        bool flagged = false;
        for (const diagnostics::Issue& i : diagnostics::issues())
            flagged = flagged || (i.source == "blendImport" && i.target.find("Wave") != std::string::npos);
        CHECK(flagged);
    }
}

TEST_CASE("blend import: armature, skin, shape keys and animation follow Blender over time")
{
    for (const char* v : kVersions)
    {
        CAPTURE(v);
        Graph& g = fresh();
        const blend::ImportResult r = load(v, "rig");
        const json o = oracle(v, "rig");
        CHECK(r.joints.size() == 3);
        REQUIRE(r.skins.size() == 1);
        REQUIRE(r.morphs.size() == 1);
        const int fps = 24;
        for (const auto& [frame, at] : o["frames"].items())
        {
            CAPTURE(frame);
            g.setTime(std::stod(frame) / fps);
            g.evaluate();
            checkVec(glm::dvec3(worldOf(g, "Rig")[3]), point(at["rig"]), 0.01);
            for (const auto& [bone, head] : at["heads"].items())
            {
                CAPTURE(bone);
                checkVec(glm::dvec3(worldOf(g, bone)[3]), point(head), 0.05);
            }
            // The deformed mesh: skin and shape key together.
            MeshPtr m = outMesh(g, "Body");
            const glm::dmat4 w = worldOf(g, "Body");
            const glm::dvec3 top(w * glm::dvec4(m->position(geo::Index(o["topIndex"].get<int>())), 1.0));
            checkVec(top, point(at["top"]), 0.5);
        }
    }
}

TEST_CASE("blend import: collection instances")
{
    for (const char* v : kVersions)
    {
        CAPTURE(v);
        Graph& g = fresh();
        const blend::ImportResult r = load(v, "instances");
        const json o = oracle(v, "instances");
        for (int i = 0; i < 3; ++i)
        {
            const std::string name = "KitInstance" + std::to_string(i);
            CAPTURE(name);
            checkMatrix(worldOf(g, r.objectNames.at(name)), blenderWorld(o["objects"][name]["world"], false));
            // Each instance holds the kit's two meshes.
            size_t meshes = 0;
            std::function<void(NodeId)> walk = [&](NodeId id) {
                for (NodeId c : g.children(id))
                {
                    meshes += g.node(c)->type().isA("mesh");
                    walk(c);
                }
            };
            walk(node(g, r.objectNames.at(name))->id());
            CHECK(meshes == 2);
        }
        // The kit's collection is excluded from the view layer, so only its instances show.
        CHECK_FALSE(g.find("|Kit"));
    }
}

TEST_CASE("blend import: curves")
{
    for (const char* v : kVersions)
    {
        CAPTURE(v);
        fresh();
        const blend::ImportResult r = load(v, "curves");
        REQUIRE(r.curves.size() == 1);
    }
}

TEST_CASE("blend import: the splash scenes, when downloaded")
{
    const char* dir = std::getenv("LILA_BLEND_CORPUS");
    if (!dir || !*dir || !fs::is_directory(dir))
        return;
    for (const auto& e : fs::directory_iterator(dir))
    {
        if (!blend::isBlendFile(e.path()))
            continue;
        CAPTURE(e.path().string());
        fresh();
        blend::ImportOptions opts;
        if (std::getenv("LILA_BLEND_EVALUATE"))
            opts.evaluate = "unsupported";
        const blend::ImportResult r = blend::importBlend(Graph::current(), e.path(), opts);
        CHECK(r.ok);
        MESSAGE(e.path().filename().string() << ": " << r.nodes.size() << " nodes, " << r.meshes.size() << " meshes, " << r.materials.size()
                                              << " materials, " << r.evaluated.size() << " evaluated, " << r.skipped.size() << " skipped, " << r.warnings.size()
                                              << " warnings");
        if (std::getenv("LILA_BLEND_VERBOSE"))
        {
            for (const std::string& line : r.skipped)
                MESSAGE("skipped: " << line);
            for (const std::string& line : r.warnings)
                MESSAGE("warning: " << line);
        }
    }
}

TEST_CASE("blend import: evaluate with Blender, when asked")
{
    // Runs an installed Blender, so only on request.
    if (!std::getenv("LILA_BLEND_EVALUATE"))
        return;
    Graph& g = fresh();
    blend::ImportOptions opts;
    opts.evaluate = "unsupported";
    const blend::ImportResult r = load("", "modifiers", opts);
    const json o = oracle("", "modifiers");
    // Wave and Weld have no Lila modifier.
    std::vector<std::string> evaluated = r.evaluated;
    std::sort(evaluated.begin(), evaluated.end());
    CHECK(evaluated == std::vector<std::string>{"Wave", "Weld"});
    g.evaluate();
    const auto [lo, hi] = bounds(*outMesh(g, "Wave"));
    const json& b = o["objects"]["Wave"]["bounds"];
    checkVec(lo, glm::min(point(b[0]), point(b[1])), 0.01);
    checkVec(hi, glm::max(point(b[0]), point(b[1])), 0.01);
}

TEST_CASE("blend import: a deform bone posed by a constraint keeps Blender's pose")
{
    for (const char* v : kVersions)
    {
        CAPTURE(v);
        Graph& g = fresh();
        load(v, "constrained");
        const json o = oracle(v, "constrained");
        g.evaluate();
        MeshPtr m = outMesh(g, "Stick");
        const glm::dmat4 w = worldOf(g, "Stick");
        const json& pts = o["points"];
        REQUIRE(m->numPoints() == pts.size());
        for (size_t p = 0; p < pts.size(); ++p)
        {
            CAPTURE(p);
            checkVec(glm::dvec3(w * glm::dvec4(m->position(geo::Index(p)), 1.0)), point(pts[p]), 0.05);
        }
    }
}

TEST_CASE("blend import: emission cards cut out by alpha; transparent objects hidden; volume-only objects become volumes")
{
    for (const char* v : kVersions)
    {
        CAPTURE(v);
        Graph& g = fresh();
        load(v, "cards");
        MeshPtr mesh = outMesh(g, "Card");
        REQUIRE(mesh->materialNames().size() == 1);
        Node* card = materials::find(g, mesh->materialNames()[0]);
        REQUIRE(card);
        CHECK(card->as<int64_t>(material::kShadingModel) == int64_t(material::ShadingModel::Unlit));
        CHECK(card->as<int64_t>(material::kBlendMode) == int64_t(material::BlendMode::Masked));
        CHECK(fs::exists(fs::path(card->as<std::string>(material::kBaseColorMap))));
        CHECK(fs::path(card->as<std::string>(material::kBaseColorMap)).extension() == ".png");
        CHECK(node(g, "Card")->as<bool>(Transform::kVisibility));
        CHECK_FALSE(node(g, "Clear")->as<bool>(Transform::kVisibility));
        CHECK_FALSE(node(g, "EyeHidden")->as<bool>(Transform::kVisibility));
        // A default Principled Volume: grey, density 1, black absorption colour.
        CHECK(node(g, "Fog")->as<bool>(Transform::kVisibility));
        MeshPtr fogMesh = outMesh(g, "Fog");
        REQUIRE(fogMesh->materialNames().size() == 1);
        Node* fog = materials::find(g, fogMesh->materialNames()[0]);
        REQUIRE(fog);
        CHECK(fog->as<int64_t>(material::kShadingModel) == int64_t(material::ShadingModel::Volume));
        CHECK(fog->as<double>(material::kVolumeDensity) == doctest::Approx(1.0));
        checkVec(fog->as<glm::dvec3>(material::kVolumeScatterColor), glm::dvec3(0.5), 1e-3);
        checkVec(fog->as<glm::dvec3>(material::kVolumeAbsorptionColor), glm::dvec3(0.5), 1e-3);
    }
}

TEST_CASE("blend probe" * doctest::skip())
{
    const char* file = std::getenv("LILA_BLEND_PROBE");
    if (!file)
        return;
    Graph& g = fresh();
    blend::ImportOptions opts;
    if (std::getenv("LILA_BLEND_EVALUATE"))
        opts.evaluate = "unsupported";
    const blend::ImportResult r = blend::importBlend(g, fs::path(file), opts);
    g.evaluate();
    for (NodeId id : r.meshes)
    {
        Node* n = g.node(id);
        MeshPtr m = std::get<MeshPtr>(g.value({id, MeshNode::kOutMesh, -1}));
        if (!m)
            continue;
        const auto [lo, hi] = bounds(*m);
        std::string mats;
        for (const std::string& s : m->materialNames())
        {
            Node* mat = materials::find(g, s);
            mats += s + (mat ? std::format("[sm{} bm{} op{} map:{}]", mat->as<int64_t>(material::kShadingModel), mat->as<int64_t>(material::kBlendMode),
                                           mat->as<double>(material::kOpacity), fs::path(mat->as<std::string>(material::kBaseColorMap)).filename().string())
                               : "[none]") + " ";
        }
        std::string uv;
        if (std::span<const glm::vec2> u = m->cornerAttrs().get<glm::vec2>(geo::attr::uvName(0)); !u.empty())
            uv = std::format(" uv0[{:.3f},{:.3f}]", u[m->faceCorner(0)].x, u[m->faceCorner(0)].y);
        MESSAGE(n->path() << " vis=" << n->as<bool>(Transform::kVisibility) << " size=" << glm::length(hi - lo) << uv << " " << mats);
    }
}

TEST_CASE("blend probe: world bounds against Blender's" * doctest::skip())
{
    // LILA_BLEND_PROBE: a .blend; LILA_BLEND_BOUNDS: {object: [[min], [max]]} Blender's world bounds, metres, Z up.
    const char* file = std::getenv("LILA_BLEND_PROBE");
    const char* boundsFile = std::getenv("LILA_BLEND_BOUNDS");
    if (!file || !boundsFile)
        return;
    Graph& g = fresh();
    const blend::ImportResult r = blend::importBlend(g, fs::path(file));
    g.evaluate();
    std::ifstream in(boundsFile);
    const json want = json::parse(in);
    for (const auto& [name, b] : want.items())
    {
        if (!r.objectNames.count(name))
            continue;
        const std::string path = r.objectNames.at(name);
        MeshPtr m = std::get<MeshPtr>(g.value({node(g, path)->id(), MeshNode::kOutMesh, -1}));
        const glm::dmat4 w = worldOf(g, path);
        glm::dvec3 lo(1e300), hi(-1e300);
        for (geo::Index p = 0; p < m->pointCapacity(); ++p)
            if (m->pointValid(p))
            {
                const glm::dvec3 q(w * glm::dvec4(m->position(p), 1.0));
                lo = glm::min(lo, q);
                hi = glm::max(hi, q);
            }
        const glm::dvec3 a = point(b[0]), c = point(b[1]);
        const double err = std::max(glm::length(lo - glm::min(a, c)), glm::length(hi - glm::max(a, c)));
        MESSAGE(name << " off by " << err << " cm");
    }
}

TEST_CASE("blend probe: joint positions against Blender's" * doctest::skip())
{
    // LILA_BLEND_PROBE: a .blend; LILA_BLEND_HEADS: {bone: [x, y, z]} Blender's world bone heads; LILA_BLEND_RIG: the armature object.
    const char* file = std::getenv("LILA_BLEND_PROBE");
    const char* headsFile = std::getenv("LILA_BLEND_HEADS");
    const char* rig = std::getenv("LILA_BLEND_RIG");
    if (!file || !headsFile || !rig)
        return;
    Graph& g = fresh();
    const blend::ImportResult r = blend::importBlend(g, fs::path(file));
    g.evaluate();
    std::ifstream in(headsFile);
    const json want = json::parse(in);
    const std::string root = r.objectNames.at(rig);
    for (const auto& [bone, head] : want.items())
    {
        std::string safe = bone;
        for (char& c : safe)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
                c = '_';
        Node* j = g.find(root + "|" + safe) ? g.find(root + "|" + safe) : nullptr;
        if (!j)
            for (NodeId id : r.joints)
                if (g.node(id)->name() == safe && g.node(id)->path().starts_with(root))
                    j = g.node(id);
        if (!j)
            continue;
        const double err = glm::length(glm::dvec3(worldOf(g, j->path())[3]) - point(head));
        if (err > 0.5)
            MESSAGE(bone << " off by " << err << " cm");
    }
}
