#include <doctest/doctest.h>

#include "GroomTestUtil.h"
#include "curve/CurveNodes.h"
#include "curve/NodeHelpers.h"
#include "curve/TestUtil.h"
#include "geo/ColorSets.h"
#include "groom/GroomBrush.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include "ops/Command.h"

using namespace lila;

namespace
{
constexpr double kPixels = 600.0, kScale = 20.0;

/** Looking straight down at the crown of a radius 10 head, 20 pixels a centimetre. */
groom::GroomBrushView topView()
{
    groom::GroomBrushView v;
    v.project = [](const glm::dvec3& p) { return std::optional<glm::dvec2>(glm::dvec2(kPixels / 2 + p.x * kScale, kPixels / 2 + p.z * kScale)); };
    v.ray = [](glm::dvec2 px, glm::dvec3& o, glm::dvec3& d) {
        o = glm::dvec3((px.x - kPixels / 2) / kScale, 100.0, (px.y - kPixels / 2) / kScale);
        d = glm::dvec3(0.0, -1.0, 0.0);
    };
    v.eye = glm::dvec3(0.0, 100.0, 0.0);
    v.viewDir = glm::dvec3(0.0, -1.0, 0.0);
    v.ortho = true;
    v.height = kPixels;
    return v;
}

scene::Node* groomOnHead()
{
    scene::Graph& g = scene::Graph::current();
    g.clear();
    UndoStack::get().clear();
    const std::string head = curvetest::run("polySphere", {{"name", "Head"}, {"radius", 10.0}, {"subdivisionsAxis", 40}, {"subdivisionsHeight", 30}})[0];
    scene::Node* scalp = g.find(head);
    scene::Node* gr = g.createNode(*scene::types::find("groom"), "Hair");
    g.connect({scalp->id(), scene::MeshNode::kOutMesh, -1}, {gr->id(), gr->type().attrIndex("inMesh"), -1});
    g.connect({scalp->id(), scene::Transform::kWorldMatrix, -1}, {gr->id(), gr->type().attrIndex("meshMatrix"), -1});
    UndoStack::get().clear();
    return gr;
}

std::string hierarchyOf(const scene::Node& gr)
{
    return std::get<std::string>(gr.value(gr.type().attrIndex("hierarchy")));
}

size_t guideCount(const scene::Node& gr)
{
    const std::string text = hierarchyOf(gr);
    if (text.empty())
        return 0;
    const nlohmann::json j = nlohmann::json::parse(text);
    return j.contains("guides") ? j["guides"].size() : 0;
}

void stroke(groom::GroomBrush& brush, std::vector<glm::dvec2> points, unsigned mods = 0)
{
    const groom::GroomBrushView v = topView();
    REQUIRE(brush.press(v, points[0].x, points[0].y, 1.0, mods));
    for (size_t i = 1; i < points.size(); ++i)
        brush.drag(v, points[i].x, points[i].y, 1.0);
    brush.release(v);
}
} // namespace

TEST_CASE("groom brush: Place and Comb on a groom node land as one undo step each, live while stroking")
{
    scene::Node* gr = groomOnHead();
    groom::GroomBrush brush;
    brush.setGroom(gr->id());
    groom::GroomBrushSettings s;
    s.mode = "place";
    s.placeLength = 6.0;
    s.matchNeighbors = false;
    s.placeSpacing = 0.8;
    brush.setSettings(s);

    // A drag places guides along it.
    std::vector<glm::dvec2> line;
    for (int i = 0; i <= 20; ++i)
        line.push_back({240.0 + 6.0 * i, 300.0});
    stroke(brush, line);
    const size_t placed = guideCount(*gr);
    CHECK(placed >= 3);
    CHECK(brush.takeWarnings().empty());
    CHECK(UndoStack::get().undo());
    CHECK(guideCount(*gr) == 0);
    CHECK(UndoStack::get().redo());
    CHECK(guideCount(*gr) == placed);

    // Comb drags them toward +x; while the stroke runs the node already holds the work in progress.
    const std::string before = hierarchyOf(*gr);
    s.mode = "comb";
    s.strength = 1.0;
    s.radius = 200.0;
    brush.setSettings(s);
    const groom::GroomBrushView v = topView();
    REQUIRE(brush.press(v, 300.0, 300.0, 1.0, 0));
    for (int i = 1; i <= 10; ++i)
        brush.drag(v, 300.0 + 10.0 * i, 300.0, 1.0);
    CHECK(hierarchyOf(*gr) != before);
    CHECK(brush.release(v));
    const std::string combed = hierarchyOf(*gr);
    CHECK(combed != before);
    CHECK(guideCount(*gr) == placed);
    CHECK(UndoStack::get().undo());
    CHECK(hierarchyOf(*gr) == before);
    CHECK(UndoStack::get().redo());
    CHECK(hierarchyOf(*gr) == combed);

    // The guides show for drawing, and hovering finds the ones under the brush.
    CHECK(brush.guideLines().size() == placed);
    CHECK_FALSE(brush.affectedLines(v, 300.0, 300.0).empty());
}

TEST_CASE("groom brush: Part draws a part into the node's parts, Erase removes guides")
{
    scene::Node* gr = groomOnHead();
    groom::GroomBrush brush;
    brush.setGroom(gr->id());
    groom::GroomBrushSettings s;
    s.mode = "part";
    brush.setSettings(s);
    std::vector<glm::dvec2> line;
    for (int i = 0; i <= 20; ++i)
        line.push_back({300.0, 220.0 + 8.0 * i});
    stroke(brush, line);
    const std::string parts = std::get<std::string>(gr->value(gr->type().attrIndex("parts")));
    CHECK_FALSE(parts.empty());
    CHECK(UndoStack::get().undo());
    CHECK(std::get<std::string>(gr->value(gr->type().attrIndex("parts"))).empty());

    s.mode = "place";
    s.matchNeighbors = false;
    s.placeLength = 5.0;
    brush.setSettings(s);
    stroke(brush, {{300.0, 300.0}});
    REQUIRE(guideCount(*gr) == 1);
    s.mode = "erase";
    s.radius = 80.0;
    brush.setSettings(s);
    stroke(brush, {{300.0, 300.0}, {302.0, 300.0}});
    CHECK(guideCount(*gr) == 0);
}

TEST_CASE("groom brush: the catalog lists GroomLab's tools with icons, without the host's own Select")
{
    const auto& catalog = groom::groomBrushCatalog();
    CHECK(catalog.size() >= 15);
    CHECK(groom::groomBrushEntry("select") == nullptr);
    for (const char* id : {"place", "draw", "comb", "slide", "lift", "twist", "attract", "shape", "cut", "lengths", "freeze", "paint"})
    {
        const groom::GroomBrushEntry* e = groom::groomBrushEntry(id);
        REQUIRE_MESSAGE(e, id);
        CHECK_FALSE(e->icon.empty());
        CHECK_FALSE(e->label.empty());
    }
    CHECK(groom::groomBrushCategories().size() == 5);
}

namespace
{
/** A head whose color set "density" is `top` above the equator and 1 below. */
scene::Node* headWithDensity(float top)
{
    geo::Mesh mesh = groomtest::sphereMesh(10.0);
    std::span<glm::vec4> v = geo::colors::add(mesh, "density");
    for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f))
            mesh.forEachFaceCorner(f, [&](geo::Index c) {
                const float x = mesh.position(mesh.cornerPoint(c)).y > 0.0 ? top : 1.0f;
                v[c] = glm::vec4(x, x, x, 1.0f);
            });
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.createNode(*scene::types::find("mesh"), "Head", scene::kNoNode, false);
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
    return n;
}

size_t strandsOn(scene::Node* head)
{
    scene::Graph& g = scene::Graph::current();
    scene::Node* gr = g.createNode(*scene::types::find("groom"), "Hair");
    const auto at = [&](const char* a) { return gr->type().attrIndex(a); };
    g.connect({head->id(), scene::MeshNode::kOutMesh, -1}, {gr->id(), at("inMesh"), -1});
    g.connect({head->id(), scene::Transform::kWorldMatrix, -1}, {gr->id(), at("meshMatrix"), -1});
    g.setValue({gr->id(), at("scaleWithScalp"), -1}, false);
    g.setValue({gr->id(), at("graph"), -1}, std::string(R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 4.0}, "paramInputs": ["density"]},
        {"name": "density", "type": "Map", "params": {"name": "density"}},
        {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "density", "to": "scatter", "input": "density"}, {"from": "scatter", "to": "out", "input": "strands"}]})j"));
    scene::Node* view = curve::helpers::createCurve(g, {}, "StrandsView");
    g.connect({gr->id(), at("outStrands"), -1}, {view->id(), curve::CurveShape::kCreate, -1});
    g.evaluate();
    return curve::splinesOf(gr->value(at("outStrands"))).size();
}
} // namespace

TEST_CASE("groom strands: the scalp's color sets are maps the graph reads, painted with vertex paint")
{
    scene::Graph& g = scene::Graph::current();
    g.clear();
    const size_t full = strandsOn(headWithDensity(1.0f));
    CHECK(full > 500);
    g.clear();
    const size_t half = strandsOn(headWithDensity(0.0f));
    CHECK(double(half) == doctest::Approx(full * 0.5).epsilon(0.15));
}

TEST_CASE("groom brush: Paint on a texture map paints the scalp's uv0 image and writes it; on a vertex map stores the values")
{
    scene::Node* gr = groomOnHead();
    const auto file = std::filesystem::temp_directory_path() / "lila_groom_paint" / "crown.png";
    std::filesystem::create_directories(file.parent_path());
    std::filesystem::remove(file);
    ops::execute("groomAddMap", {{"args", {gr->path()}}, {"name", "crown"}, {"kind", "texture"}, {"file", file.generic_string()}});
    ops::execute("groomAddMap", {{"args", {gr->path()}}, {"name", "dens"}});

    groom::GroomBrush brush;
    brush.setGroom(gr->id());
    groom::GroomBrushSettings s;
    s.mode = "paint";
    s.paintKind = "texture";
    s.attribute = "crown";
    s.value = 1.0;
    s.strength = 1.0;
    s.radius = 120.0;
    brush.setSettings(s);
    stroke(brush, {{300.0, 300.0}, {310.0, 300.0}, {320.0, 300.0}});
    CHECK(brush.takeWarnings().empty());
    REQUIRE(std::filesystem::exists(file));
    CHECK(std::filesystem::file_size(file) > 100);

    s.paintKind = "vertices";
    s.attribute = "dens";
    brush.setSettings(s);
    stroke(brush, {{300.0, 300.0}, {310.0, 300.0}});
    const nlohmann::json maps = nlohmann::json::parse(std::get<std::string>(gr->value(gr->type().attrIndex("scalpMaps"))));
    REQUIRE(maps.contains("dens"));
    double sum = 0.0;
    for (const auto& v : maps["dens"]["values"])
        sum += v.get<double>();
    CHECK(sum > 1.0);
    CHECK(UndoStack::get().undo());
    const nlohmann::json before = nlohmann::json::parse(std::get<std::string>(gr->value(gr->type().attrIndex("scalpMaps"))));
    double after = 0.0;
    for (const auto& v : before["dens"]["values"])
        after += v.get<double>();
    CHECK(after == doctest::Approx(0.0));
}
