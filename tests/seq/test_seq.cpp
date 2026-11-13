#include <doctest/doctest.h>

#include "core/MainQueue.h"
#include "fbx/Fbx.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "geo/ObjIO.h"
#include "geo/Primitives.h"
#include "io/FileTranslators.h"
#include "math/Unreal.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Node.h"
#include "scene/Anim.h"
#include "scene/SceneCommands.h"
#include "scene/Transform.h"
#include "seq/Alembic.h"
#include "seq/MeshSequence.h"
#include "seq/Vat.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

using namespace lila;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
void setup()
{
    static bool once = [] {
        mainq::init();
        ops::registerBuiltins();
        scene::types::registerBuiltins();
        scene::registerCommands();
        io::translators::registerCommands();
        seq::registerCommands();
        return true;
    }();
    (void)once;
}

fs::path tempDir(const char* name)
{
    fs::path d = fs::temp_directory_path() / ("lila_seq_" + std::string(name));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

/** A 100 cm cube named Box whose translateY goes 0, 100, 200 over frames 1 to 3 (linear keys at 30 fps). */
std::string animatedBox(bool colors = false)
{
    ops::execute("newScene", json::object());
    ops::execute("playbackOptions", {{"fps", 30.0}, {"minTime", 1.0 / 30.0}, {"maxTime", 3.0 / 30.0}});
    scene::Graph& g = scene::Graph::current();
    geo::Mesh cube = geo::makeCube();
    if (colors)
        geo::colors::add(cube, "colorSet1", glm::vec4(1.0f, 0.5f, 0.25f, 1.0f));
    scene::Node* n = g.createNode(*scene::types::find("mesh"), "Box");
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(cube))));
    const std::string name = n->name();
    for (auto [frame, value] : {std::pair{1.0, 0.0}, {3.0, 200.0}})
        ops::execute("setKeyframe", {{"args", {name + ".ty"}}, {"time", frame / 30.0}, {"value", value}, {"inTangentType", "linear"},
                                     {"outTangentType", "linear"}});
    return name;
}
} // namespace

TEST_CASE("frame naming: index from 0 or frame number, zero padded")
{
    CHECK(seq::frameName("{name}_{index}", "Spike", 0, 32.0, 3) == "Spike_000");
    CHECK(seq::frameName("{name}_{index}", "Spike", 26, 58.0, 3) == "Spike_026");
    CHECK(seq::frameName("{name}_{frame}", "Spike", 0, 32.0, 3) == "Spike_032");
    CHECK(seq::frameName("{name}.{frame}", "Spike", 0, 7.0, 4) == "Spike.0007");
    CHECK(seq::frameName("f{frame}", "x", 0, -3.0, 2) == "f-03");
    CHECK(seq::frameName("f{frame}", "x", 0, 2.5, 3) == "f2_500");
    CHECK(seq::frameName("{index}", "x", 1234, 0.0, 2) == "1234");

    seq::SequenceOptions o;
    o.start = 32;
    o.end = 58;
    CHECK(seq::frames(o).size() == 27);
    o.step = 2;
    std::vector<double> f = seq::frames(o);
    REQUIRE(f.size() == 14);
    CHECK(f.back() == doctest::Approx(58.0));
    o.step = 0;
    CHECK_THROWS(seq::frames(o));
    o.step = 1;
    o.end = 10;
    CHECK_THROWS(seq::frames(o));
}

TEST_CASE("exportMeshSequence writes a file per frame with the scale and the animation baked")
{
    setup();
    const std::string box = animatedBox();
    fs::path dir = tempDir("obj");

    json files = ops::execute("exportMeshSequence", {{"args", {box}}, {"format", "obj"}, {"folder", dir.generic_string()}, {"scale", 0.1},
                                                     {"profile", "generic"}, {"pattern", "spike_{index}"}});
    REQUIRE(files.size() == 3);
    CHECK(fs::path(files[0].get<std::string>()).filename() == "spike_000.obj");
    CHECK(fs::path(files[2].get<std::string>()).filename() == "spike_002.obj");
    for (int i = 0; i < 3; ++i)
    {
        geo::ObjReadResult r = geo::readObjFile(files[size_t(i)].get<std::string>());
        REQUIRE(r.ok());
        REQUIRE(r.objects.size() == 1);
        geo::Bounds b = r.objects[0].mesh.bounds();
        CHECK(b.size().x == doctest::Approx(10.0));
        CHECK(b.center().y == doctest::Approx(10.0 * i));
        // Triangulated by default.
        CHECK(r.objects[0].mesh.numFaces() == 12);
    }
    CHECK(ops::execute("currentTime", json::object()).get<double>() == doctest::Approx(0.0));

    // Frame numbers in the name, an explicit range, recentring every frame, object space untouched by scale 1.
    json more = ops::execute("exportMeshSequence", {{"args", {box}}, {"format", "obj"}, {"folder", dir.generic_string()}, {"start", 2},
                                                    {"end", 3}, {"profile", "generic"}, {"pattern", "{name}_{frame}"}, {"padding", 4},
                                                    {"recenter", "each"}, {"triangulate", false}});
    REQUIRE(more.size() == 2);
    CHECK(fs::path(more[0].get<std::string>()).filename() == box + "_0002.obj");
    geo::ObjReadResult r = geo::readObjFile(more[1].get<std::string>());
    REQUIRE(r.ok());
    CHECK(glm::length(r.objects[0].mesh.bounds().center()) == doctest::Approx(0.0));
    CHECK(r.objects[0].mesh.numFaces() == 6);

    // The legacy single path form still works: {frame} padded to 4.
    json legacy = ops::execute("exportMeshSequence", {{"args", {box}}, {"start", 1}, {"end", 1}, {"path", (dir / "bob_{frame}.obj").generic_string()}});
    REQUIRE(legacy.size() == 1);
    CHECK(legacy[0].get<std::string>().ends_with("bob_0001.obj"));

    // A dry run names the files and writes nothing.
    json dry = ops::execute("exportMeshSequence", {{"args", {box}}, {"folder", (dir / "dry").generic_string()}, {"dryRun", true}});
    CHECK(dry.size() == 3);
    CHECK_FALSE(fs::exists(dir / "dry"));

    CHECK_THROWS_AS(ops::execute("exportMeshSequence", {{"args", {box}}, {"format", "usd"}}), CommandError);
    CHECK_THROWS_AS(ops::execute("exportMeshSequence", {{"args", {"nobody"}}}), CommandError);
    fs::remove_all(dir);
}

TEST_CASE("exportMeshSequence writes FBX frames")
{
    setup();
    const std::string box = animatedBox();
    fs::path dir = tempDir("fbx");
    json files = ops::execute("exportMeshSequence", {{"args", {box}}, {"folder", dir.generic_string()}, {"normals", false}});
    REQUIRE(files.size() == 3);
    for (const json& f : files)
    {
        CHECK(f.get<std::string>().ends_with(".fbx"));
        CHECK(fs::file_size(f.get<std::string>()) > 0);
    }
    fs::remove_all(dir);
}

TEST_CASE("Alembic round trip: a sample per frame with points, faces, normals, UVs and colors")
{
    setup();
    const std::string box = animatedBox(true);
    fs::path dir = tempDir("abc");
    fs::path file = dir / "box.abc";

    json written = ops::execute("exportAlembic", {{"args", {box}}, {"path", file.generic_string()}, {"scale", 0.1}, {"profile", "generic"}});
    CHECK(fs::path(written.get<std::string>()) == fs::path(file.generic_string()));
    REQUIRE(fs::exists(file));

    std::string err;
    std::vector<seq::abc::ObjectInfo> objects = seq::abc::read(file, &err);
    INFO(err);
    REQUIRE(objects.size() == 1);
    const seq::abc::ObjectInfo& o = objects[0];
    CHECK(o.path == "/" + box + "/" + box + "Shape");
    CHECK(o.samples == 3);
    CHECK(o.points == std::vector<size_t>{8, 8, 8});
    CHECK(o.faces == std::vector<size_t>{12, 12, 12});
    REQUIRE(o.times.size() == 3);
    CHECK(o.times[0] == doctest::Approx(1.0 / 30.0));
    CHECK(o.times[2] == doctest::Approx(3.0 / 30.0));
    CHECK(o.normals);
    CHECK(o.uvs);
    CHECK(o.colors);
    REQUIRE(o.firstPositions.size() == 8);
    for (const glm::vec3& p : o.firstPositions)
        CHECK(std::abs(p.x) == doctest::Approx(5.0));

    // exportMeshSequence format=abc writes the same single file.
    json viaSeq = ops::execute("exportMeshSequence", {{"args", {box}}, {"format", "abc"}, {"folder", dir.generic_string()}, {"colors", false}});
    REQUIRE(viaSeq.size() == 1);
    CHECK(fs::path(viaSeq[0].get<std::string>()).filename() == box + ".abc");
    std::vector<seq::abc::ObjectInfo> again = seq::abc::read(viaSeq[0].get<std::string>());
    REQUIRE(again.size() == 1);
    CHECK(again[0].samples == 3);
    CHECK_FALSE(again[0].colors);

    CHECK(seq::abc::read(dir / "missing.abc").empty());
    fs::remove_all(dir);
}

TEST_CASE("importAlembic brings a mesh cache back as a mesh with a meshCache deformer, undoably")
{
    setup();
    const std::string box = animatedBox();
    std::vector<glm::dvec3> world[3];
    scene::Graph& g = scene::Graph::current();
    for (int f = 1; f <= 3; ++f)
    {
        g.setTime(f / 30.0);
        g.evaluate();
        const scene::Node* n = g.find(box);
        const glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        for (const glm::dvec3& p : n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh)->positions())
            world[f - 1].push_back(glm::dvec3(w * glm::dvec4(p, 1.0)));
    }
    fs::path dir = tempDir("abcimport");
    const fs::path file = dir / "box.abc";
    ops::execute("exportAlembic", {{"args", {box}}, {"path", file.generic_string()}, {"scale", 0.1}, {"profile", "ue5"}});

    ops::execute("newScene", json::object());
    json r = ops::execute("importAlembic", {{"args", {file.generic_string()}}, {"scale", 10.0}, {"report", true}});
    INFO(r.dump());
    REQUIRE(r["nodes"].size() == 1);
    REQUIRE(r["meshCaches"].size() == 1);
    CHECK(r["start"].get<double>() == doctest::Approx(1.0 / 30.0));
    CHECK(r["end"].get<double>() == doctest::Approx(3.0 / 30.0));
    CHECK(scene::anim::playback().maxTime == doctest::Approx(3.0 / 30.0));
    const std::string name = r["nodes"][0];
    // The exporter triangulates; the points stay as they were.
    for (int f = 1; f <= 3; ++f)
    {
        CAPTURE(f);
        g.setTime(f / 30.0);
        g.evaluate();
        const scene::MeshPtr m = g.find(name)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        REQUIRE(m);
        REQUIRE(m->pointCapacity() == world[f - 1].size());
        for (size_t p = 0; p < world[f - 1].size(); ++p)
            CHECK(glm::length(m->position(geo::Index(p)) - world[f - 1][p]) < 1e-3);
    }
    g.setTime(2.0 / 30.0);
    g.evaluate();
    CHECK(g.find(name)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh)->bounds().center().y == doctest::Approx(100.0).epsilon(1e-4));

    ops::execute("undo", json::object());
    CHECK(!g.find(name));

    // Through File > Import, by extension.
    json viaTranslator = ops::execute("importFile", {{"args", {file.generic_string()}}});
    CHECK(g.find(name));
    (void)viaTranslator;
    g.setTime(0.0);
    fs::remove_all(dir);
}

TEST_CASE("importAlembic keeps a mesh whose topology changes at its first sample")
{
    setup();
    ops::execute("newScene", json::object());
    fs::path dir = tempDir("abcgrow");
    seq::abc::Writer w;
    seq::abc::WriteOptions wo;
    wo.fps = 24.0;
    w.open(dir / "grow.abc", wo);
    geo::Mesh small = geo::makeCube();
    geo::CubeOptions co;
    co.subdivX = 2;
    geo::Mesh big = geo::makeCube(co);
    w.addFrame({"Grow"}, {&small});
    w.addFrame({"Grow"}, {&big});
    w.close();
    json r = ops::execute("importAlembic", {{"args", {(dir / "grow.abc").generic_string()}}, {"report", true}, {"profile", "generic"}});
    INFO(r.dump());
    REQUIRE(r["nodes"].size() == 1);
    CHECK(r["meshCaches"].empty());
    REQUIRE(r["warnings"].size() == 1);
    CHECK(r["warnings"][0].get<std::string>().find("topology changes") != std::string::npos);
    scene::Graph::current().evaluate();
    const scene::MeshPtr grown = scene::Graph::current().find(r["nodes"][0].get<std::string>())->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    REQUIRE(grown);
    CHECK(grown->pointCapacity() == 8);
    CHECK_THROWS_AS(ops::execute("importAlembic", {{"args", {(dir / "missing.abc").generic_string()}}}), CommandError);
    fs::remove_all(dir);
}

TEST_CASE("the Alembic writer allows topology to change between frames")
{
    fs::path dir = tempDir("topo");
    seq::abc::Writer w;
    seq::abc::WriteOptions wo;
    wo.fps = 24.0;
    w.open(dir / "grow.abc", wo);
    geo::Mesh small = geo::makeCube();
    geo::CubeOptions co;
    co.subdivX = 2;
    geo::Mesh big = geo::makeCube(co);
    w.addFrame({"Grow"}, {&small});
    w.addFrame({"Grow"}, {&big});
    // An object that turns up on the last frame starts there.
    w.addFrame({"Grow", "Late"}, {&small, &small});
    w.close();

    std::vector<seq::abc::ObjectInfo> objects = seq::abc::read(dir / "grow.abc");
    REQUIRE(objects.size() == 2);
    CHECK(objects[0].samples == 3);
    CHECK(objects[0].points[0] == 8);
    CHECK(objects[0].points[1] > 8);
    CHECK(objects[1].samples == 1);
    CHECK(objects[1].times[0] == doctest::Approx(2.0 / 24.0));
    fs::remove_all(dir);
}

namespace
{
glm::dvec3 ue(const glm::dvec3& v)
{
    math::Vector u = math::toUnreal(math::Vector(v.x, v.y, v.z));
    return {u.x, u.y, u.z};
}

glm::dvec3 texel(const seq::VatImage& img, int col, int row)
{
    const float* p = &img.rgba[(size_t(row) * size_t(img.width) + size_t(col)) * 4];
    return {p[0], p[1], p[2]};
}

/** Vertex i of a frame through the layout the metadata describes. */
glm::dvec3 vertexTexel(const seq::VatImage& img, const json& meta, int frame, int i)
{
    const int w = meta["width"], rpf = meta["rowsPerFrame"];
    return texel(img, i % w, frame * rpf + i / w);
}

/** The rest mesh as Unreal-facing data: each point's Lila position and the texture column its VAT UV addresses. */
std::vector<std::pair<glm::dvec3, int>> restColumns(const fs::path& fbxFile, const json& meta)
{
    ops::execute("newScene", json::object());
    scene::Graph& g = scene::Graph::current();
    fbx::ImportResult r = fbx::importFbx(g, fbxFile);
    REQUIRE(r.ok);
    std::vector<std::pair<glm::dvec3, int>> out;
    const int w = meta["width"], h = meta["height"];
    for (scene::NodeId id : r.nodes)
    {
        scene::Node* n = g.node(id);
        if (!n->type().isA("mesh"))
            continue;
        g.evaluate();
        const geo::Mesh& m = *n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(meta["uvChannel"].get<int>()));
        REQUIRE(uv.size() == m.cornerCapacity());
        for (geo::Index c = 0; c < m.cornerCapacity(); ++c)
        {
            if (!m.cornerValid(c))
                continue;
            // Unreal flips V on import, so its V is 1 - the file's.
            const int col = int(std::floor(uv[c].x * w));
            const int row = int(std::floor((1.0f - uv[c].y) * h));
            out.push_back({m.position(m.cornerPoint(c)), row * w + col});
        }
    }
    return out;
}
} // namespace

TEST_CASE("exportVertexAnimation bakes offsets and normals that decode back to the animation")
{
    setup();
    const std::string box = animatedBox();
    fs::path dir = tempDir("vat");

    json r = ops::execute("exportVertexAnimation", {{"args", {box}}, {"folder", dir.generic_string()}});
    const json& meta = r["meta"];
    CHECK(meta["mode"] == "constant");
    CHECK(meta["frameCount"] == 3);
    CHECK(meta["vertexCount"] == 8);
    CHECK(meta["width"] == 8);
    CHECK(meta["rowsPerFrame"] == 1);
    CHECK(meta["height"] == 3);
    CHECK(meta["uvChannel"] == 1);
    CHECK(meta["fps"].get<double>() == doctest::Approx(30.0));
    for (const char* k : {"mesh", "positions", "normals", "meta"})
        CHECK(fs::exists(r["files"][k].get<std::string>()));
    CHECK(json::parse(std::ifstream(r["files"]["meta"].get<std::string>())) == meta);
    // The material inputs, by the names a VAT material uses.
    const json& mp = r["materialParams"];
    CHECK(mp == meta["materialParams"]);
    CHECK(mp["HDR"] == 1);
    CHECK(mp["FrameCount"] == 3);
    CHECK(mp["RowsPerFrame"] == 1);
    CHECK(mp["TextureHeight"] == 3);
    CHECK(mp["PositionTexture"] == fs::path(r["files"]["positions"].get<std::string>()).filename().string());
    CHECK(mp["NormalTexture"] == fs::path(r["files"]["normals"].get<std::string>()).filename().string());
    CHECK(mp["BoundsMax"][2].get<double>() == doctest::Approx(200.0));
    CHECK(seq::vatMaterialSummary(mp).find("HDR=1 BoundsMin=(0, 0, 0) BoundsMax=(0, 0, 200) FrameCount=3") == 0);

    std::string err;
    seq::VatImage pos = seq::readVatImage(r["files"]["positions"].get<std::string>(), &err);
    INFO(err);
    REQUIRE(pos.width == 8);
    REQUIRE(pos.height == 3);
    for (int f = 0; f < 3; ++f)
    {
        const glm::dvec3 expect = ue(glm::dvec3(0.0, 100.0 * f, 0.0));
        for (int i = 0; i < 8; ++i)
        {
            const glm::dvec3 got = vertexTexel(pos, meta, f, i);
            CHECK(glm::length(got - expect) < 0.1);
            CHECK(pos.rgba[(size_t(f) * 8 + size_t(i)) * 4 + 3] == 1.0f);
        }
    }
    // Box corner normals average to the diagonals.
    seq::VatImage nrm = seq::readVatImage(r["files"]["normals"].get<std::string>());
    REQUIRE(nrm.width == 8);
    for (int i = 0; i < 8; ++i)
    {
        const glm::dvec3 n = vertexTexel(nrm, meta, 1, i) * 2.0 - 1.0;
        CHECK(glm::length(n) == doctest::Approx(1.0).epsilon(0.02));
        CHECK(std::abs(n.x) == doctest::Approx(1.0 / std::sqrt(3.0)).epsilon(0.03));
    }

    // Absolute positions read through the rest mesh's UV land on its own vertices.
    json abs = ops::execute("exportVertexAnimation", {{"args", {box}}, {"folder", dir.generic_string()}, {"name", "abs"}, {"positions", "absolute"}});
    seq::VatImage absPos = seq::readVatImage(abs["files"]["positions"].get<std::string>());
    std::vector<std::pair<glm::dvec3, int>> cols = restColumns(abs["files"]["mesh"].get<std::string>(), abs["meta"]);
    // Triangulated on import: 12 triangles.
    REQUIRE(cols.size() == 36);
    for (const auto& [p, col] : cols)
    {
        REQUIRE(col >= 0);
        REQUIRE(col < 8);
        CHECK(glm::length(texel(absPos, col, 0) - ue(p)) < 0.05);
        CHECK(glm::length(texel(absPos, col, 2) - ue(p + glm::dvec3(0, 200, 0))) < 0.15);
    }

    // PNG positions are normalised by the bounds in the metadata.
    const std::string box2 = animatedBox();
    json png = ops::execute("exportVertexAnimation", {{"args", {box2}}, {"folder", dir.generic_string()}, {"name", "png"}, {"format", "png16"},
                                                      {"normalBits", 16}, {"positions", "absolute"}});
    CHECK(fs::path(png["files"]["positions"].get<std::string>()).extension() == ".png");
    seq::VatImage p16 = seq::readVatImage(png["files"]["positions"].get<std::string>());
    REQUIRE(p16.width == 8);
    const json& pm = png["meta"];
    const json& bp = pm["materialParams"];
    CHECK(bp["HDR"] == 0);
    glm::dvec3 lo(bp["BoundsMin"][0], bp["BoundsMin"][1], bp["BoundsMin"][2]), hi(bp["BoundsMax"][0], bp["BoundsMax"][1], bp["BoundsMax"][2]);
    CHECK(glm::length(lo - glm::dvec3(pm["bounds"]["min"][0], pm["bounds"]["min"][1], pm["bounds"]["min"][2])) < 1e-9);
    // Frame 3 is the top of the animation: every corner's up coordinate decodes to the box top plus 200.
    double topUe = -1e9;
    for (int i = 0; i < 8; ++i)
        topUe = std::max(topUe, (lo + vertexTexel(p16, pm, 2, i) * (hi - lo)).z);
    CHECK(topUe == doctest::Approx(250.0).epsilon(1e-3));

    // 8-bit positions decode within one step (1/255 of the range) of the 16-bit ones.
    json png8 = ops::execute("exportVertexAnimation", {{"args", {box2}}, {"folder", dir.generic_string()}, {"name", "png8"}, {"format", "png8"},
                                                       {"positions", "absolute"}});
    CHECK(png8["meta"]["positionBits"] == 8);
    CHECK(png8["materialParams"]["HDR"] == 0);
    CHECK(png8["materialParams"]["BoundsMin"] == bp["BoundsMin"]);
    seq::VatImage p8 = seq::readVatImage(png8["files"]["positions"].get<std::string>());
    REQUIRE(p8.width == 8);
    for (int f = 0; f < 3; ++f)
        for (int i = 0; i < 8; ++i)
        {
            const glm::dvec3 a = lo + vertexTexel(p8, pm, f, i) * (hi - lo), b = lo + vertexTexel(p16, pm, f, i) * (hi - lo);
            for (int c = 0; c < 3; ++c)
                CHECK(std::abs(a[c] - b[c]) <= (hi[c] - lo[c]) / 255.0 + 1e-6);
        }
    // A png format names its bit depth.
    CHECK_THROWS(ops::execute("exportVertexAnimation", {{"args", {box2}}, {"folder", dir.generic_string()}, {"name", "bare"}, {"format", "png"}}));

    // Dry run plans the files only.
    json dry = ops::execute("exportVertexAnimation", {{"args", {box2}}, {"folder", (dir / "dry").generic_string()}, {"dryRun", true}});
    CHECK(dry["files"]["mesh"].get<std::string>().ends_with(box2 + ".fbx"));
    CHECK_FALSE(fs::exists(dir / "dry"));

    CHECK_THROWS_AS(ops::execute("exportVertexAnimation", {{"args", {box2}}, {"format", "tga"}}), CommandError);
    fs::remove_all(dir);
}

TEST_CASE("exportVertexAnimation wraps wide frames onto several rows")
{
    setup();
    const std::string box = animatedBox();
    fs::path dir = tempDir("vatwrap");
    json r = ops::execute("exportVertexAnimation", {{"args", {box}}, {"folder", dir.generic_string()}, {"maxWidth", 3}, {"positions", "absolute"}});
    const json& meta = r["meta"];
    CHECK(meta["width"] == 3);
    CHECK(meta["rowsPerFrame"] == 3);
    CHECK(meta["height"] == 9);
    seq::VatImage pos = seq::readVatImage(r["files"]["positions"].get<std::string>());
    REQUIRE(pos.height == 9);
    // The last row of each frame has one texel unused.
    CHECK(pos.rgba[(size_t(2) * 3 + 2) * 4 + 3] == 0.0f);
    std::vector<std::pair<glm::dvec3, int>> cols = restColumns(r["files"]["mesh"].get<std::string>(), meta);
    REQUIRE_FALSE(cols.empty());
    std::set<int> seen;
    for (const auto& [p, index] : cols)
    {
        // index is row * width + column within frame 0's rows.
        const int col = index % 3, row = index / 3;
        REQUIRE(row < 3);
        seen.insert(index);
        CHECK(glm::length(texel(pos, col, row) - ue(p)) < 0.05);
        CHECK(glm::length(texel(pos, col, 2 * 3 + row) - ue(p + glm::dvec3(0, 200, 0))) < 0.15);
    }
    CHECK(seen.size() == 8);
    fs::remove_all(dir);
}

TEST_CASE("exportVertexAnimation bakes changing topology as a padded triangle soup")
{
    setup();
    ops::execute("newScene", json::object());
    ops::execute("playbackOptions", {{"fps", 30.0}, {"minTime", 1.0 / 30.0}, {"maxTime", 3.0 / 30.0}});
    json made = ops::execute("polyPlane", {{"width", 100.0}, {"depth", 100.0}, {"name", "Grow"}});
    const std::string plane = made.is_array() ? made[0].get<std::string>() : made.get<std::string>();
    scene::Graph& g = scene::Graph::current();
    std::string gen = plane;
    if (g.find(plane)->type().attrIndex("subdivisionsX") < 0)
        for (const json& m : made)
            if (g.find(m.get<std::string>())->type().attrIndex("subdivisionsX") >= 0)
                gen = m.get<std::string>();
    for (auto [frame, value] : {std::pair{1.0, 1.0}, {2.0, 2.0}, {3.0, 3.0}})
        ops::execute("setKeyframe", {{"args", {gen + ".subdivisionsX"}}, {"time", frame / 30.0}, {"value", value}});
    fs::path dir = tempDir("vatsoup");

    CHECK_THROWS_WITH_AS(ops::execute("exportVertexAnimation", {{"args", {plane}}, {"folder", dir.generic_string()}, {"mode", "constant"}}),
                         doctest::Contains("frame 2"), CommandError);

    json r = ops::execute("exportVertexAnimation", {{"args", {plane}}, {"folder", dir.generic_string()}, {"positions", "absolute"}});
    const json& meta = r["meta"];
    CHECK(meta["mode"] == "soup");
    CHECK(meta["triangles"] == 6);
    CHECK(meta["vertexCount"] == 18);
    seq::VatImage pos = seq::readVatImage(r["files"]["positions"].get<std::string>());
    REQUIRE(pos.width == 18);
    auto area = [&](int frame, int tri) {
        const glm::dvec3 a = vertexTexel(pos, meta, frame, tri * 3), b = vertexTexel(pos, meta, frame, tri * 3 + 1),
                         c = vertexTexel(pos, meta, frame, tri * 3 + 2);
        return glm::length(glm::cross(b - a, c - a)) * 0.5;
    };
    // Frame 1 has 2 triangles, so 4 slots collapse; frame 3 fills all 6. The real ones cover the 100 x 100 plane.
    double total1 = 0.0, total3 = 0.0;
    for (int t = 0; t < 6; ++t)
    {
        CHECK((t < 2) == (area(0, t) > 1.0));
        CHECK(area(2, t) > 1.0);
        total1 += area(0, t);
        total3 += area(2, t);
    }
    CHECK(total1 == doctest::Approx(10000.0).epsilon(1e-3));
    CHECK(total3 == doctest::Approx(10000.0).epsilon(1e-3));
    // The rest mesh is the full soup: 6 triangles, 18 unwelded points, each addressing its own column.
    std::vector<std::pair<glm::dvec3, int>> cols = restColumns(r["files"]["mesh"].get<std::string>(), meta);
    CHECK(cols.size() == 18);
    std::set<int> seen;
    for (const auto& c : cols)
        seen.insert(c.second);
    CHECK(seen.size() == 18);
    fs::remove_all(dir);
}

namespace
{
/** Three 100 cm cubes, Rigid0..2, each translating and tumbling on all three axes over frames 1 to 6 (linear keys at 30 fps). */
std::vector<std::string> tumblingCubes()
{
    ops::execute("newScene", json::object());
    ops::execute("playbackOptions", {{"fps", 30.0}, {"minTime", 1.0 / 30.0}, {"maxTime", 6.0 / 30.0}});
    scene::Graph& g = scene::Graph::current();
    std::vector<std::string> names;
    for (int i = 0; i < 3; ++i)
    {
        scene::Node* n = g.createNode(*scene::types::find("mesh"), "Rigid" + std::to_string(i));
        g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(geo::makeCube())));
        const std::string name = n->name();
        auto key = [&](const char* attr, double frame, double value) {
            ops::execute("setKeyframe", {{"args", {name + "." + attr}}, {"time", frame / 30.0}, {"value", value}, {"inTangentType", "linear"},
                                         {"outTangentType", "linear"}});
        };
        key("tx", 1.0, 300.0 * i);
        key("tx", 6.0, 300.0 * i + 120.0 * (i + 1));
        key("ty", 1.0, 50.0 * i);
        key("ty", 6.0, -80.0);
        key("tz", 1.0, 0.0);
        key("tz", 6.0, 250.0 + 40.0 * i);
        // Up to 75 degrees a frame on rz, so the quaternion sign has to be kept continuous.
        key("rx", 1.0, 10.0 * i);
        key("rx", 6.0, 200.0 + 30.0 * i);
        key("ry", 1.0, 0.0);
        key("ry", 6.0, -140.0);
        key("rz", 1.0, 0.0);
        key("rz", 6.0, 375.0);
        names.push_back(name);
    }
    return names;
}

/** Every sampled point of every frame, in Unreal space. */
std::vector<std::vector<glm::dvec3>> sampledPoints(const std::vector<std::string>& names)
{
    scene::Graph& g = scene::Graph::current();
    std::vector<scene::NodeId> ids;
    for (const std::string& n : names)
        ids.push_back(g.find(n)->id());
    seq::SequenceOptions so;
    so.playbackRange = true;
    so.triangulate = false;
    std::vector<std::vector<glm::dvec3>> out;
    seq::sample(g, seq::meshNodes(g, ids), so, [&](seq::FrameMeshes& f) {
        std::vector<glm::dvec3> pts;
        for (const geo::Mesh& m : f.meshes)
            for (geo::Index p = 0; p < m.pointCapacity(); ++p)
                if (m.pointValid(p))
                    pts.push_back(ue(m.position(p)));
        out.push_back(std::move(pts));
    });
    return out;
}

glm::dvec4 texel4(const seq::VatImage& img, int col, int row)
{
    const float* p = &img.rgba[(size_t(row) * size_t(img.width) + size_t(col)) * 4];
    return {p[0], p[1], p[2], p[3]};
}

glm::dvec3 rotateQ(const glm::dvec4& q, const glm::dvec3& v)
{
    const glm::dvec3 u(q.x, q.y, q.z);
    const glm::dvec3 t = 2.0 * glm::cross(u, v);
    return v + q.w * t + glm::cross(u, t);
}

/** One rest mesh corner run through the rigid material's math, as Unreal sees the FBX (V flipped on import). */
struct RigidCorner
{
    glm::dvec3 rest;
    int piece = 0;
    glm::dvec3 pivot;
    std::vector<glm::dvec3> frames;
    std::vector<glm::dvec4> rotations;
};

std::vector<RigidCorner> decodeRigid(const json& files, const json& meta)
{
    std::string err;
    seq::VatImage pos = seq::readVatImage(files["positions"].get<std::string>(), &err);
    INFO(err);
    seq::VatImage rot = seq::readVatImage(files["rotations"].get<std::string>(), &err);
    INFO(err);
    const json& mp = meta["materialParams"];
    const int w = meta["width"], h = meta["height"], rpf = meta["rowsPerFrame"], frames = mp["FrameCount"];
    REQUIRE(pos.width == w);
    REQUIRE(pos.height == h);
    REQUIRE(rot.width == w);
    REQUIRE(rot.height == h);
    const bool hdr = mp["HDR"] == 1, absolute = meta["positions"] == "absolute";
    const glm::dvec3 lo(mp["BoundsMin"][0], mp["BoundsMin"][1], mp["BoundsMin"][2]), hi(mp["BoundsMax"][0], mp["BoundsMax"][1], mp["BoundsMax"][2]);

    ops::execute("newScene", json::object());
    scene::Graph& g = scene::Graph::current();
    fbx::ImportResult r = fbx::importFbx(g, files["mesh"].get<std::string>());
    REQUIRE(r.ok);
    std::vector<RigidCorner> out;
    for (scene::NodeId id : r.nodes)
    {
        scene::Node* n = g.node(id);
        if (!n->type().isA("mesh"))
            continue;
        g.evaluate();
        const geo::Mesh& m = *n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        std::span<const glm::vec2> address = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(meta["uvChannel"].get<int>()));
        std::span<const glm::vec2> pivotXY = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(meta["pivotChannels"][0].get<int>()));
        std::span<const glm::vec2> pivotZ = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(meta["pivotChannels"][1].get<int>()));
        REQUIRE(address.size() == m.cornerCapacity());
        REQUIRE(pivotXY.size() == m.cornerCapacity());
        REQUIRE(pivotZ.size() == m.cornerCapacity());
        for (geo::Index c = 0; c < m.cornerCapacity(); ++c)
        {
            if (!m.cornerValid(c))
                continue;
            RigidCorner rc;
            rc.rest = ue(m.position(m.cornerPoint(c)));
            // Unreal's importer flips V, in float like the UVs themselves.
            const glm::dvec2 uvA(address[c].x, 1.0f - address[c].y), uvXY(pivotXY[c].x, 1.0f - pivotXY[c].y), uvZ(pivotZ[c].x, 1.0f - pivotZ[c].y);
            CHECK(uvZ.y == 0.0);
            rc.pivot = glm::dvec3(uvXY.x, uvXY.y, uvZ.x);
            const int col = int(std::floor(uvA.x * w)), row = int(std::floor(uvA.y * h));
            REQUIRE(row < rpf);
            rc.piece = row * w + col;
            for (int f = 0; f < frames; ++f)
            {
                const glm::dvec4 rt = texel4(rot, col, f * rpf + row);
                const glm::dvec4 q = glm::normalize(hdr ? rt : rt * 2.0 - 1.0);
                const glm::dvec3 pt = texel(pos, col, f * rpf + row);
                const glm::dvec3 centre = (absolute ? glm::dvec3(0.0) : rc.pivot) + (hdr ? pt : lo + pt * (hi - lo));
                rc.frames.push_back(rotateQ(q, rc.rest - rc.pivot) + centre);
                rc.rotations.push_back(q);
            }
            out.push_back(std::move(rc));
        }
    }
    return out;
}

/** Largest decode error against the sampled points (each corner matched by its rest position), and largest arm angle in degrees. */
std::pair<double, double> rigidError(const std::vector<RigidCorner>& corners, const std::vector<std::vector<glm::dvec3>>& truth)
{
    double worst = 0.0, worstAngle = 0.0;
    for (const RigidCorner& c : corners)
    {
        size_t best = 0;
        double bestDist = 1e30;
        for (size_t i = 0; i < truth[0].size(); ++i)
            if (const double d = glm::length(truth[0][i] - c.rest); d < bestDist)
            {
                bestDist = d;
                best = i;
            }
        REQUIRE(bestDist < 1e-3);
        REQUIRE(c.frames.size() == truth.size());
        for (size_t f = 0; f < truth.size(); ++f)
        {
            worst = std::max(worst, glm::length(c.frames[f] - truth[f][best]));
            // The piece is the cube whose rest corners lie within 90 cm of the pivot; its centre is their mean.
            glm::dvec3 centre(0.0);
            int count = 0;
            for (size_t i = 0; i < truth[0].size(); ++i)
                if (glm::length(truth[0][i] - c.pivot) < 90.0)
                {
                    centre += truth[f][i];
                    ++count;
                }
            REQUIRE(count == 8);
            const glm::dvec3 decodedArm = rotateQ(c.rotations[f], c.rest - c.pivot);
            const glm::dvec3 trueArm = truth[f][best] - centre / double(count);
            const double cosA = glm::dot(glm::normalize(decodedArm), glm::normalize(trueArm));
            worstAngle = std::max(worstAngle, glm::degrees(std::acos(std::clamp(cosA, -1.0, 1.0))));
        }
    }
    return {worst, worstAngle};
}

std::string fileBytes(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
} // namespace

TEST_CASE("exportVertexAnimation rigid mode bakes a transform per piece that decodes back to the animation")
{
    setup();
    fs::path dir = tempDir("vatrigid");
    std::vector<std::string> cubes = tumblingCubes();
    const std::vector<std::vector<glm::dvec3>> truth = sampledPoints(cubes);
    REQUIRE(truth.size() == 6);
    REQUIRE(truth[0].size() == 24);

    json r = ops::execute("exportVertexAnimation", {{"args", cubes}, {"folder", (dir / "a").generic_string()}, {"name", "rigid"}, {"mode", "rigid"}});
    const json& meta = r["meta"];
    CHECK(meta["mode"] == "rigid");
    CHECK(meta["pieceCount"] == 3);
    CHECK(meta["pieceSource"] == "components");
    CHECK(meta["width"] == 3);
    CHECK(meta["rowsPerFrame"] == 1);
    CHECK(meta["height"] == 6);
    CHECK(meta["uvChannel"] == 1);
    CHECK(meta["pivotChannels"] == json::array({2, 3}));
    CHECK(meta["positionBits"] == 32);
    CHECK(meta["rigid"]["maxResidual"].get<double>() < 1e-6);
    CHECK_FALSE(r["files"].contains("normals"));
    for (const char* k : {"mesh", "positions", "rotations", "meta"})
        CHECK(fs::exists(r["files"][k].get<std::string>()));
    CHECK(fs::path(r["files"]["rotations"].get<std::string>()).filename() == "rigid_rot.exr");
    const json& mp = r["materialParams"];
    CHECK(mp["HDR"] == 1);
    CHECK(mp["PieceCount"] == 3);
    CHECK(mp["FrameCount"] == 6);
    CHECK(mp["PivotChannelXY"] == 2);
    CHECK(mp["PivotChannelZ"] == 3);
    CHECK(mp["RotationTexture"] == "rigid_rot.exr");
    CHECK_FALSE(mp.contains("NormalTexture"));
    CHECK(seq::vatMaterialSummary(mp).find("RotationTexture=rigid_rot.exr PieceCount=3") != std::string::npos);
    // Mesh bounds hold every frame's vertices.
    const glm::dvec3 bmin(meta["meshBounds"]["min"][0], meta["meshBounds"]["min"][1], meta["meshBounds"]["min"][2]);
    const glm::dvec3 bmax(meta["meshBounds"]["max"][0], meta["meshBounds"]["max"][1], meta["meshBounds"]["max"][2]);
    for (const auto& frame : truth)
        for (const glm::dvec3& p : frame)
            CHECK((glm::all(glm::greaterThanEqual(p, bmin - 1e-6)) && glm::all(glm::lessThanEqual(p, bmax + 1e-6))));

    // Same scene, same options: identical bytes.
    json again = ops::execute("exportVertexAnimation", {{"args", cubes}, {"folder", (dir / "b").generic_string()}, {"name", "rigid"}, {"mode", "rigid"}});
    for (const char* k : {"mesh", "positions", "rotations", "meta"})
        CHECK(fileBytes(r["files"][k].get<std::string>()) == fileBytes(again["files"][k].get<std::string>()));

    // Absolute positions and 16-bit PNGs, wrapped onto two rows a frame.
    json absPng = ops::execute("exportVertexAnimation", {{"args", cubes}, {"folder", (dir / "c").generic_string()}, {"name", "png"}, {"mode", "rigid"},
                                                         {"format", "png16"}, {"positions", "absolute"}, {"maxWidth", 2}});
    CHECK(absPng["meta"]["rowsPerFrame"] == 2);
    CHECK(absPng["meta"]["height"] == 12);
    CHECK(absPng["materialParams"]["HDR"] == 0);
    CHECK(fs::path(absPng["files"]["rotations"].get<std::string>()).extension() == ".png");

    {
        std::vector<RigidCorner> corners = decodeRigid(r["files"], meta);
        REQUIRE(corners.size() == 3 * 36);
        std::set<int> pieces;
        for (const RigidCorner& c : corners)
        {
            pieces.insert(c.piece);
            for (size_t f = 1; f < c.rotations.size(); ++f)
                CHECK(glm::dot(c.rotations[f], c.rotations[f - 1]) >= 0.0);
            CHECK(c.rotations[0].w >= 0.0);
        }
        CHECK(pieces == std::set<int>{0, 1, 2});
        const auto [err, angle] = rigidError(corners, truth);
        MESSAGE("rigid exr decode error " << err << " cm, " << angle << " deg");
        CHECK(err < 0.01);
        CHECK(angle < 0.05);
    }
    {
        std::vector<RigidCorner> corners = decodeRigid(absPng["files"], absPng["meta"]);
        REQUIRE(corners.size() == 3 * 36);
        const auto [err, angle] = rigidError(corners, truth);
        MESSAGE("rigid png16 decode error " << err << " cm, " << angle << " deg");
        CHECK(err < 0.05);
        CHECK(angle < 0.05);
    }
    fs::remove_all(dir);
}

TEST_CASE("exportVertexAnimation auto picks rigid only for a piece attribute that moves rigidly")
{
    setup();
    fs::path dir = tempDir("vatrigidauto");
    auto twoPieces = [](bool stretch) {
        const std::string box = animatedBox();
        scene::Graph& g = scene::Graph::current();
        geo::Mesh m = geo::makeCube();
        geo::Mesh other = geo::makeCube();
        for (geo::Index p = 0; p < other.pointCapacity(); ++p)
            other.setPosition(p, other.position(p) + glm::dvec3(400.0, 0.0, 0.0));
        m.append(other);
        std::span<int32_t> piece = m.pointAttrs().getOrAdd<int32_t>(seq::kVatPieceAttr);
        REQUIRE(piece.size() == 16);
        for (geo::Index p = 0; p < 16; ++p)
            piece[p] = p < 8 ? 7 : 3;
        g.setValue({g.find(box)->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
        ops::execute("setKeyframe", {{"args", {box + ".rz"}}, {"time", 1.0 / 30.0}, {"value", 0.0}});
        ops::execute("setKeyframe", {{"args", {box + ".rz"}}, {"time", 3.0 / 30.0}, {"value", 90.0}});
        if (stretch)
        {
            ops::execute("setKeyframe", {{"args", {box + ".sx"}}, {"time", 1.0 / 30.0}, {"value", 1.0}});
            ops::execute("setKeyframe", {{"args", {box + ".sx"}}, {"time", 3.0 / 30.0}, {"value", 2.0}});
        }
        return box;
    };

    const std::string box = twoPieces(false);
    json r = ops::execute("exportVertexAnimation", {{"args", {box}}, {"folder", dir.generic_string()}, {"name", "auto"}});
    CHECK(r["meta"]["mode"] == "rigid");
    CHECK(r["meta"]["modeRequested"] == "auto");
    CHECK(r["meta"]["pieceSource"] == "attribute");
    CHECK(r["meta"]["pieceCount"] == 2);
    CHECK(r["files"].contains("rotations"));
    CHECK_FALSE(r["files"].contains("normals"));
    {
        // Piece values order the columns: piece 3 (the cube at x = 400 at rest) is column 0.
        std::vector<RigidCorner> corners = decodeRigid(r["files"], r["meta"]);
        REQUIRE_FALSE(corners.empty());
        for (const RigidCorner& c : corners)
            CHECK(glm::length(c.pivot - ue(glm::dvec3(c.piece == 0 ? 400.0 : 0.0, 0.0, 0.0))) < 1e-3);
    }

    const std::string stretched = twoPieces(true);
    json c = ops::execute("exportVertexAnimation", {{"args", {stretched}}, {"folder", dir.generic_string()}, {"name", "stretch"}});
    CHECK(c["meta"]["mode"] == "constant");
    json forced = ops::execute("exportVertexAnimation", {{"args", {stretched}}, {"folder", dir.generic_string()}, {"name", "forced"}, {"mode", "rigid"}});
    CHECK(forced["meta"]["mode"] == "rigid");
    CHECK(forced["meta"]["rigid"]["maxResidual"].get<double>() > seq::kVatRigidTolerance);

    // A mesh without the attribute never turns rigid on its own.
    const std::string plain = animatedBox();
    CHECK(ops::execute("exportVertexAnimation", {{"args", {plain}}, {"folder", dir.generic_string()}, {"name", "plain"}})["meta"]["mode"] == "constant");
    fs::remove_all(dir);
}

TEST_CASE("rigidBind pieces bake to joints and to a rigid VAT without residual")
{
    setup();
    fs::path dir = tempDir("rigidbind");
    ops::execute("newScene", json::object());
    ops::execute("playbackOptions", {{"fps", 30.0}, {"minTime", 1.0 / 30.0}, {"maxTime", 6.0 / 30.0}});
    const std::string body = ops::execute("polyPlane", {{"name", "Body"}, {"width", 20}, {"depth", 20}, {"subdivisionsX", 10}, {"subdivisionsZ", 10}})[0];
    json cubes = json::array();
    for (int i = 0; i < 3; ++i)
    {
        const std::string c = ops::execute("polyCube", {{"width", 1.6}, {"height", 0.4}, {"depth", 1.6}})[0];
        ops::execute("move", {{"args", {c}}, {"to", {-2.0 + 2.0 * i, 0.3, 0.0}}});
        cubes.push_back(c);
    }
    const std::string scales = ops::execute("polyCombine", {{"args", cubes}, {"name", "Scales"}}).get<std::string>();
    ops::execute("rigidBind", {{"args", {scales, body}}});
    ops::execute("setKeyframe", {{"args", {body + ".rz"}}, {"time", 1.0 / 30.0}, {"value", 0.0}});
    ops::execute("setKeyframe", {{"args", {body + ".rz"}}, {"time", 6.0 / 30.0}, {"value", 60.0}});
    ops::execute("setKeyframe", {{"args", {body + ".ty"}}, {"time", 6.0 / 30.0}, {"value", 30.0}});

    json vat = ops::execute("exportVertexAnimation", {{"args", {scales}}, {"folder", dir.generic_string()}, {"name", "scales"}, {"mode", "rigid"}});
    CHECK(vat["meta"]["mode"] == "rigid");
    CHECK(vat["meta"]["pieceCount"] == 3);
    CHECK(vat["meta"]["rigid"]["maxResidual"].get<double>() < seq::kVatRigidTolerance);

    json joints = ops::execute("bakePiecesToJoints", {{"args", {scales}}});
    CHECK(joints["joints"].size() == 3);
    CHECK(joints["residual"].get<double>() < 1e-3);
    fs::remove_all(dir);
}
