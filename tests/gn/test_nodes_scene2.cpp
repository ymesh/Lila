#include "Nodes2Util.h"

#include "TestUtil.h"

#include "core/ImageIO.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <filesystem>

using namespace nodes2test;
using gn::SocketType;

TEST_CASE("image info")
{
    auto dir = std::filesystem::temp_directory_path() / "lila_gn_image_info";
    std::filesystem::create_directories(dir);
    std::vector<float> rgba(4 * 2 * 4, 0.5f);
    rgba[3] = 0.25f;
    image::writePng(dir / "seq.0001.png", 4, 2, rgba, 8);
    image::writePng(dir / "seq.0002.png", 4, 2, rgba, 8);
    Run r = run({N{"I", "ImageInfo", {}, {{"file", (dir / "seq.0001.png").generic_string()}}}},
                {L{"I", "width", "GroupOutput", "w"}, L{"I", "height", "GroupOutput", "h"}, L{"I", "channels", "GroupOutput", "c"}, L{"I", "hasAlpha", "GroupOutput", "a"},
                 L{"I", "frameCount", "GroupOutput", "f"}},
                {{"w", SocketType::Int}, {"h", SocketType::Int}, {"c", SocketType::Int}, {"a", SocketType::Bool}, {"f", SocketType::Int}});
    CHECK(single<int64_t>(r.values[0]) == 4);
    CHECK(single<int64_t>(r.values[1]) == 2);
    CHECK(single<int64_t>(r.values[2]) == 4);
    CHECK(single<uint8_t>(r.values[3]) == 1);
    CHECK(single<int64_t>(r.values[4]) == 2);
}

namespace
{
/** The User mesh's bounds centre after evaluating the scene, in its own space. */
D3 userCentre()
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    return g.find("User")->as<scene::MeshPtr>(scene::MeshNode::kOutMesh)->bounds().center();
}
} // namespace

TEST_CASE("bone info follows a posed joint")
{
    using namespace gntest;
    gntest::run("newScene");
    gntest::run("joint", {{"name", "root"}, {"position", {0, 0, 0}}, {"parent", ""}});
    gntest::run("joint", {{"name", "tip"}, {"position", {0, 50, 0}}, {"parent", "root"}});
    gntest::run("polyCube", {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}, {"name", "User"}});
    gntest::run("gnCreateGroup", args({"Pose"}));
    std::string bone = gntest::run("gnAddNode", args({"Pose", "BoneInfo"}));
    std::string xf = gntest::run("gnAddNode", args({"Pose", "TransformGeometry"}));
    gntest::run("gnSetValue", args({"Pose", bone + ".joint", "tip"}));
    gntest::run("gnDisconnect", args({"Pose", "GroupOutput.geometry"}));
    gntest::run("gnConnect", args({"Pose", "GroupInput.geometry", xf + ".geometry"}));
    gntest::run("gnConnect", args({"Pose", bone + ".location", xf + ".translation"}));
    gntest::run("gnConnect", args({"Pose", xf + ".geometry", "GroupOutput.geometry"}));
    gntest::run("gnApply", args({"User", "Pose"}));
    CHECK(near(userCentre(), D3(0, 50, 0), 1e-6));
    gntest::run("setAttr", args({"root.translateX", 20.0}));
    CHECK(near(userCentre(), D3(20, 50, 0), 1e-6));

    gntest::run("gnSetValue", args({"Pose", bone + ".space", "local"}));
    CHECK(near(userCentre(), D3(0, 50, 0), 1e-6));
}

TEST_CASE("collection info reads the objects under a transform")
{
    using namespace gntest;
    gntest::run("newScene");
    std::string a = gntest::run("polyCube", {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}, {"name", "PartA"}})[0];
    std::string b = gntest::run("polySphere", {{"radius", 5.0}, {"name", "PartB"}})[0];
    gntest::run("setAttr", args({"PartB.translateX", 100.0}));
    gntest::run("group", {{"args", {a, b}}, {"name", "Kit"}});
    gntest::run("polyCube", {{"width", 1.0}, {"height", 1.0}, {"depth", 1.0}, {"name", "User"}});
    gntest::run("gnCreateGroup", args({"Use"}));
    std::string col = gntest::run("gnAddNode", args({"Use", "CollectionInfo"}));
    std::string real = gntest::run("gnAddNode", args({"Use", "RealizeInstances"}));
    gntest::run("gnSetValue", args({"Use", col + ".parent", "Kit"}));
    gntest::run("gnSetValue", args({"Use", col + ".separateChildren", true}));
    gntest::run("gnDisconnect", args({"Use", "GroupOutput.geometry"}));
    gntest::run("gnConnect", args({"Use", col + ".instances", real + ".geometry"}));
    gntest::run("gnConnect", args({"Use", real + ".geometry", "GroupOutput.geometry"}));
    gntest::run("gnApply", args({"User", "Use"}));
    CHECK(faces("User") == 6 + faces(b));
    CHECK(bounds("User")["boundsMax"][0].get<double>() == doctest::Approx(105.0));
    gntest::run("setAttr", args({"PartB.translateX", 200.0}));
    CHECK(bounds("User")["boundsMax"][0].get<double>() == doctest::Approx(205.0));
}
