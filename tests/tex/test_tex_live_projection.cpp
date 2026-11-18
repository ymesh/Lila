#include "TexPaintTest.h"

#include "render/Baker.h"
#include "render/MeshMaps.h"
#include "tex/Generators.h"
#include "tex/Layers.h"

#include <filesystem>

using namespace textest;
namespace fs = std::filesystem;

namespace
{
json live(const json& flags) { return ops::execute("bakeLiveMaps", flags); }

image::Image baseColor(const fs::path& dir, const std::string& name)
{
    const fs::path out = dir / name;
    run("texEvaluate", {{"args", {"Panel"}}, {"folder", out.generic_string()}, {"channels", {"baseColor"}}, {"format", "exr"}});
    return image::read(out / "Panel_baseColor.exr");
}
} // namespace

TEST_CASE("tex live projection: with live maps, a projected fill stays on its spot of the mesh as the mesh moves, with no file rebake")
{
    render::registerGpuBakeBackend();
    if (!gpu())
        return;
    newScene();
    const std::string plane = run("polyPlane", {{"width", 100.0}, {"depth", 100.0}})[0];
    run("select", {{"args", {plane}}});
    run("createMaterial", {{"args", {"M_Panel"}}});
    run("assignMaterial", {{"args", {"M_Panel"}}});
    run("texCreateSet", {{"args", {"Panel"}}, {"resolution", 32}, {"materials", {"M_Panel"}}});
    const fs::path dir = tempDir("live_projection");
    run("createBakeProject", {{"name", "PB"}, {"folder", (dir / "bake").generic_string()}});
    run("setBakeOutput", {{"project", "PB"}, {"resolution", 32}, {"supersampling", 1}, {"format", "exr"}, {"bits", 32}});
    run("bakeGroup", {{"args", {"G"}}, {"project", "PB"}, {"lows", {plane}}, {"self", true}});
    run("texSetSettings", {{"args", {"Panel", {{"bakeProject", "PB"}}}}});
    // A file bake first: its frame is the one that goes stale once the mesh moves.
    run("bake", {{"project", "PB"}, {"maps", {"position", "objectNormal"}}});
    const json status = live({{"project", "PB"}, {"enable", true}, {"maps", {"position", "objectNormal"}}, {"resolution", 32}, {"supersampling", 1}});
    REQUIRE_MESSAGE(status.value("error", std::string()).empty(), status.dump());
    REQUIRE(tex::liveMeshMapMeta("PB", "position").contains("lows"));

    std::vector<float> px(16 * 16 * 4);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
        {
            float* p = &px[(size_t(y) * 16 + x) * 4];
            p[0] = float(x) / 15.0f, p[1] = float(y) / 15.0f, p[2] = 0.0f, p[3] = 1.0f;
        }
    image::writeExr(dir / "coord.exr", 16, 16, px, 32);
    const json projection = {{"mode", "planar"}, {"rotationX", -90.0}, {"positionX", 10.0}, {"sizeX", 60.0}, {"sizeY", 60.0}, {"sizeZ", 60.0}};
    run("texAddLayer", {{"args", {"Panel", "fill"}}, {"material", {{"baseColor", (dir / "coord.exr").generic_string()}}}, {"projection", projection}});
    const image::Image before = baseColor(dir, "before");
    REQUIRE(before.width == 32);
    double spread = 0.0;
    for (size_t i = 0; i < before.rgba.size(); i += 4)
        spread = std::max(spread, double(before.rgba[i]));
    CHECK(spread > 0.5);

    run("setAttr", {{"args", {plane + ".translate", 35.0, 12.0, -50.0}}});
    run("setAttr", {{"args", {plane + ".rotate", 25.0, 60.0, -10.0}}});
    const json update = live({{"project", "PB"}, {"update", true}});
    REQUIRE_MESSAGE(!update.contains("error"), update.dump());
    // The stored graph still holds the file bake's frame; the live one moved with the mesh.
    const json file = tex::bakedPositionMeta(sg(), "PB"), now = tex::liveMeshMapMeta("PB", "position");
    CHECK(file["boundsMin"] != now["boundsMin"]);
    CHECK(file["lows"][0]["world"] != now["lows"][0]["world"]);

    const image::Image after = baseColor(dir, "after");
    REQUIRE(after.width == 32);
    double worst = 0.0;
    for (size_t i = 0; i < before.rgba.size(); ++i)
        if (i % 4 != 3)
            worst = std::max(worst, std::abs(double(before.rgba[i]) - double(after.rgba[i])));
    MESSAGE("largest change on the mesh after the move: " << worst);
    // Live positions are half floats over the moved mesh's bounds.
    CHECK(worst < 0.02);

    // Position-reading nodes: bounds as the bake filled them, or unset, follow the live frame; typed ones stay.
    scene::Node* set = tex::findSet(sg(), "Panel");
    REQUIRE(set);
    tex::Graph g = tex::readGraph(*set);
    g.nodes.push_back({"FromBake", "triplanar", {0.0, 0.0}, {{"boundsMin", file["boundsMin"]}, {"boundsMax", file["boundsMax"]}}});
    g.nodes.push_back({"Unset", "position", {0.0, 0.0}, json::object()});
    const json typed = {{"boundsMin", {-1.0, -2.0, -3.0}}, {"boundsMax", {4.0, 5.0, 6.0}}};
    g.nodes.push_back({"Typed", "position", {0.0, 0.0}, typed});
    CHECK(tex::followLiveFrame(g, tex::readSettings(*set)));
    CHECK(g.node("FromBake")->values["boundsMin"] == now["boundsMin"]);
    CHECK(g.node("FromBake")->values["boundsMax"] == now["boundsMax"]);
    CHECK(g.node("Unset")->values["boundsMax"] == now["boundsMax"]);
    CHECK(g.node("Typed")->values == typed);

    live({{"project", "PB"}, {"disable", true}});
    CHECK(tex::liveMeshMapMeta("PB", "position").is_null());
}
