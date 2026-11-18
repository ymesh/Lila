#include "TexPaintTest.h"

#include "render/PaintGpu.h"
#include "scene/MeshNodes.h"
#include "tex/PaintStrokes.h"

using namespace textest;
namespace gp = lila::render::paint;

TEST_CASE("texture paint multi: one stroke over two meshes with their own sets writes both, and one undo removes both")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelA", seamPanel());
    addMesh("PanelB", seamPanel());
    // Each panel's material binds its own set.
    for (const char* m : {"PanelA", "PanelB"})
    {
        run("select", {{"args", {m}}});
        run("createMaterial", {{"args", {std::string("M_") + m}}});
        run("assignMaterial", {{"args", {std::string("M_") + m}}});
    }
    run("texCreateSet", {{"args", {"SetA"}}, {"resolution", 128}, {"materials", {"M_PanelA"}}});
    run("texCreateSet", {{"args", {"SetB"}}, {"resolution", 128}, {"materials", {"M_PanelB"}}});
    scene::Node& a = *tex::findSet(sg(), "SetA");
    scene::Node& b = *tex::findSet(sg(), "SetB");

    auto part = [](const char* mesh, double x) {
        json dabs = json::array();
        for (double z : {-10.0, 0.0, 10.0})
            dabs.push_back({{"position", {x, 0.0, z}}, {"radius", 6.0}});
        return json{{"space", "surface"}, {"mesh", mesh}, {"brush", brushJson()}, {"payload", {{"channels", {{"baseColor", {0.0, 1.0, 0.0}}}}}}, {"dabs", dabs}};
    };
    run("texPaintStrokes", {{"strokes", {{{"set", "SetA"}, {"layer", "P"}, {"stroke", part("PanelA", 30.0)}},
                                         {{"set", "SetB"}, {"layer", "P"}, {"stroke", part("PanelB", -30.0)}}}}});
    CHECK(tex::paint::readPaint(a)->buffer("P")->strokes.size() == 1);
    CHECK(tex::paint::readPaint(b)->buffer("P")->strokes.size() == 1);
    CHECK(at(gp::readChannel(a.id(), "P", "baseColor"), seamUv(1, 30.0, 0.0))[3] > 0.5f);
    CHECK(at(gp::readChannel(b.id(), "P", "baseColor"), seamUv(0, -30.0, 0.0))[3] > 0.5f);

    run("undo");
    const tex::paint::Buffer* ba = tex::paint::readPaint(a)->buffer("P");
    const tex::paint::Buffer* bb = tex::paint::readPaint(b)->buffer("P");
    CHECK((!ba || ba->strokes.empty()));
    CHECK((!bb || bb->strokes.empty()));
}
