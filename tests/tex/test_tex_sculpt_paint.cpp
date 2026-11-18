#include "TexPaintTest.h"

#include "render/PaintGpu.h"
#include "sculpt/SculptSession.h"
#include "tex/PaintStrokes.h"

using namespace textest;
namespace gp = lila::render::paint;

TEST_CASE("texture paint in sculpt: a stroke made while sculpting lands on the sculpted surface and writes the set")
{
    if (!gpu())
        return;
    newScene();
    const std::string panel = addMesh("Panel", seamPanel());
    scene::Node& set = makeSet("Set", 128);
    run("select", {{"args", {panel}}});
    run("sculptEnter", {{"args", {panel}}, {"levels", 0}});
    std::shared_ptr<sculpt::Session> s = sculpt::active();
    REQUIRE(s);
    // The sculpt raised 5 units: a dab of radius 3 at that height only reaches the sculpted surface.
    std::vector<glm::dvec3> lifted;
    const geo::Mesh& m = s->sculpt.mesh();
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        lifted.push_back(m.position(p) + glm::dvec3(0.0, 5.0, 0.0));
    s->sculpt.setPositions(lifted);
    CHECK(gp::paintSurface(*sg().find(panel))->position(0).y == doctest::Approx(5.0));

    json dabs = json::array();
    for (double z : {-6.0, 0.0, 6.0})
        dabs.push_back({{"position", {30.0, 5.0, z}}, {"radius", 3.0}});
    paintStroke("Set", "P", {{"space", "surface"}, {"mesh", panel}, {"brush", brushJson()}, {"payload", {{"channels", {{"baseColor", {1.0, 0.0, 0.0}}}}}}, {"dabs", dabs}});
    REQUIRE(tex::paint::readPaint(set)->buffer("P")->strokes.size() == 1);
    CHECK(at(gp::readChannel(set.id(), "P", "baseColor"), seamUv(1, 30.0, 0.0))[3] > 0.5f);

    run("sculptExit");
    CHECK_FALSE(sculpt::active());
    CHECK(tex::paint::readPaint(set)->buffer("P")->strokes.size() == 1);
}
