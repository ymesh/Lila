#include <doctest/doctest.h>

#include "ops/Command.h"
#include "fbx/Fbx.h"
#include "geo/Mesh.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Rig.h"
#include "scene/Skin.h"

#include <ufbx.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

using namespace lila;
using namespace lila::scene;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
json v3(double x, double y, double z) { return json::array({x, y, z}); }

Graph& graph() { return Graph::current(); }

Node& node(const std::string& name)
{
    Node* n = graph().find(name);
    REQUIRE_MESSAGE(n, name);
    return *n;
}

fs::path tempFile(const std::string& name)
{
    fs::path dir = fs::temp_directory_path() / "lila_fbxskin_tests";
    fs::create_directories(dir);
    return dir / name;
}

const std::vector<std::string> kJoints = {"bone_a", "bone_b", "bone_c"};
constexpr int kFrames = 24;

/**
 * A 100 cm cylinder offset to x = 3, bound to a three-joint chain with jointOrients and a non-default rotate order,
 * animated over one second at 24 fps: the root slides (root motion), the middle bends, the tip twists.
 */
void buildScene()
{
    run("newScene");
    run("playbackOptions", {{"fps", 24.0}, {"minTime", 0.0}, {"maxTime", 1.0}, {"animationStartTime", 0.0}, {"animationEndTime", 1.0}});
    std::string mesh = run("polyCylinder", {{"name", "Tube"}, {"radius", 10}, {"height", 100}, {"subdivisionsHeight", 12}, {"caps", false}})[0];
    REQUIRE(mesh == "Tube");
    run("setAttr", {{"args", {"Tube.translate", v3(3, 0, 0)}}});
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(3, -50, 0)}, {"name", "bone_a"}, {"orientation", v3(0, 0, 90)}});
    run("joint", {{"position", v3(3, 0, 0)}, {"name", "bone_b"}, {"orientation", v3(10, 0, -20)}});
    run("joint", {{"position", v3(3, 50, 0)}, {"name", "bone_c"}, {"orientation", v3(0, 15, 0)}});
    run("setAttr", {{"args", {"bone_b.rotateOrder", 1}}});
    // Euler curves, so the written keys are the scene's own; rotation curves are covered in the fbx tests.
    for (const std::string& j : kJoints)
        run("setAttr", {{"args", {j + ".rotationInterpolation", 0}}});
    run("skinCluster", {{"args", {"bone_a", "Tube"}}});

    Graph& g = graph();
    auto key = [&](const char* joint, int attr, int axis, double frame, double value) {
        anim::setKey(g, {node(joint).id(), attr, axis}, frame / 24.0, value);
    };
    key("bone_a", Transform::kTranslate, 0, 0, node("bone_a").as<glm::dvec3>(Transform::kTranslate).x);
    key("bone_a", Transform::kTranslate, 0, 24, 40);
    key("bone_b", Transform::kRotate, 2, 0, 0);
    key("bone_b", Transform::kRotate, 2, 10, 70);
    key("bone_b", Transform::kRotate, 2, 24, -35);
    key("bone_b", Transform::kRotate, 0, 0, 0);
    key("bone_b", Transform::kRotate, 0, 24, 25);
    key("bone_c", Transform::kRotate, 1, 0, 0);
    key("bone_c", Transform::kRotate, 1, 24, 120);
    key("bone_c", Transform::kScale, 1, 0, 1);
    key("bone_c", Transform::kScale, 1, 24, 1.5);
}

struct Frame
{
    std::map<std::string, glm::dmat4> joints;
    std::vector<glm::dvec3> points;
};

std::vector<Frame> sample()
{
    Graph& g = graph();
    std::vector<Frame> out;
    for (int f = 0; f <= kFrames; ++f)
    {
        g.setTime(f / 24.0);
        g.evaluate();
        Frame fr;
        for (const std::string& j : kJoints)
            fr.joints[j] = node(j).as<glm::dmat4>(Transform::kWorldMatrix);
        MeshPtr m = node("Tube").as<MeshPtr>(MeshNode::kOutMesh);
        REQUIRE(m);
        const glm::dmat4 meshWorld = node("Tube").as<glm::dmat4>(Transform::kWorldMatrix);
        for (const glm::dvec3& p : m->positions())
            fr.points.push_back(glm::dvec3(meshWorld * glm::dvec4(p, 1.0)));
        out.push_back(std::move(fr));
    }
    g.setTime(0.0);
    g.evaluate();
    return out;
}

void compare(const std::vector<Frame>& a, const std::vector<Frame>& b, double eps)
{
    REQUIRE(a.size() == b.size());
    double worstJoint = 0.0, worstPoint = 0.0;
    for (size_t f = 0; f < a.size(); ++f)
    {
        for (const auto& [name, m] : a[f].joints)
            for (int c = 0; c < 4; ++c)
                worstJoint = std::max(worstJoint, glm::length(m[c] - b[f].joints.at(name)[c]));
        REQUIRE(a[f].points.size() == b[f].points.size());
        for (size_t p = 0; p < a[f].points.size(); ++p)
            worstPoint = std::max(worstPoint, glm::length(a[f].points[p] - b[f].points[p]));
    }
    MESSAGE("worst joint matrix column error " << worstJoint << ", worst skinned point error " << worstPoint << " cm");
    CHECK(worstJoint < eps);
    CHECK(worstPoint < eps);
}

struct UfbxScene
{
    ufbx_scene* scene = nullptr;
    explicit UfbxScene(const fs::path& p)
    {
        ufbx_load_opts opts{};
        ufbx_error err{};
        scene = ufbx_load_file(p.string().c_str(), &opts, &err);
        if (!scene)
            MESSAGE(std::string(err.description.data, err.description.length));
    }
    ~UfbxScene() { ufbx_free_scene(scene); }
};

void roundTrip(const std::string& profile, bool bake)
{
    buildScene();
    std::vector<Frame> before = sample();
    fs::path file = tempFile("skin_" + profile + (bake ? "_baked" : "_keys") + ".fbx");
    run("exportFbx", {{"args", {file.string(), "Tube", "bone_a"}}, {"kind", "skeletal"}, {"profile", profile}, {"bakeAnimation", bake}});

    {
        UfbxScene s(file);
        REQUIRE(s.scene);
        CHECK(s.scene->bones.count == 3);
        REQUIRE(s.scene->skin_deformers.count == 1);
        CHECK(s.scene->skin_deformers.data[0]->clusters.count == 3);
        CHECK(s.scene->poses.count == 1);
        REQUIRE(s.scene->anim_stacks.count == 1);
        CHECK(s.scene->settings.frames_per_second == doctest::Approx(24.0));
        CHECK(s.scene->anim_stacks.data[0]->time_end == doctest::Approx(1.0));
        size_t most = 0;
        for (size_t i = 0; i < s.scene->anim_curves.count; ++i)
            most = std::max(most, s.scene->anim_curves.data[i]->keyframes.count);
        // Baked curves carry a key per frame; written keys are the scene's own (three at most here).
        CHECK(most == (bake ? size_t(kFrames + 1) : size_t(3)));
    }

    run("newScene");
    run("playbackOptions", {{"fps", 30.0}, {"minTime", 0.0}, {"maxTime", 4.0}});
    json r = run("importFbx", {{"args", {file.string()}}, {"report", true}});
    CHECK(r["joints"].size() == 3);
    CHECK(r["skinClusters"].size() == 1);
    CHECK(r["fps"].get<double>() == doctest::Approx(24.0));
    CHECK(anim::playback().fps == doctest::Approx(24.0));
    CHECK(anim::playback().maxTime == doctest::Approx(1.0));
    CHECK(node("bone_b").as<int64_t>(Transform::kRotateOrder) == 1);
    // jointOrient survives as jointOrient (PreRotation), not folded into rotate.
    glm::dvec3 jo = node("bone_b").as<glm::dvec3>(Joint::kJointOrient);
    CHECK(jo.x == doctest::Approx(10.0));
    CHECK(jo.z == doctest::Approx(-20.0));
    compare(before, sample(), 2e-3);
}
} // namespace

TEST_CASE("skinned animated cylinder round-trips through FBX, baked, every profile")
{
    for (std::string profile : {"ue5", "ue6", "generic"})
    {
        CAPTURE(profile);
        roundTrip(profile, true);
    }
}

TEST_CASE("curves written as FBX keys evaluate the same as Lila's")
{
    for (std::string profile : {"ue5", "ue6"})
    {
        CAPTURE(profile);
        roundTrip(profile, false);
    }
}

TEST_CASE("max influences are pruned and renormalised")
{
    buildScene();
    // Spread every point over all three joints so the limit has something to cut.
    Graph& g = graph();
    NodeId skin = skin::find(g, node("Tube").id());
    WeightTable t = skin::table(g, skin);
    for (size_t p = 0; p < t.points.size(); ++p)
        t.points[p] = {{0, 0.5f}, {1, 0.3f}, {2, 0.2f}};
    skin::setWeights(g, skin, t);

    fs::path file = tempFile("skin_mi2.fbx");
    run("exportFbx", {{"args", {file.string(), "Tube"}}, {"kind", "skeletal"}, {"maxInfluences", 2}, {"animation", false}});
    UfbxScene s(file);
    REQUIRE(s.scene);
    REQUIRE(s.scene->skin_deformers.count == 1);
    const ufbx_skin_deformer* d = s.scene->skin_deformers.data[0];
    CHECK(d->max_weights_per_vertex == 2);
    for (size_t v = 0; v < d->vertices.count; ++v)
    {
        double sum = 0.0;
        for (uint32_t k = 0; k < d->vertices.data[v].num_weights; ++k)
            sum += d->weights.data[d->vertices.data[v].weight_begin + k].weight;
        CHECK(sum == doctest::Approx(1.0).epsilon(1e-5));
    }
    // The joints came along although only the mesh was given; no take without animation.
    CHECK(s.scene->bones.count == 3);
    CHECK(s.scene->anim_stacks.count == 0);
}

TEST_CASE("animation kind writes the skeleton and its curves, no mesh")
{
    buildScene();
    fs::path file = tempFile("anim_only.fbx");
    run("exportFbx", {{"args", {file.string(), "Tube"}}, {"kind", "animation"}});
    UfbxScene s(file);
    REQUIRE(s.scene);
    CHECK(s.scene->meshes.count == 0);
    CHECK(s.scene->bones.count == 3);
    REQUIRE(s.scene->anim_stacks.count == 1);
    CHECK(s.scene->anim_curves.count > 0);
}

TEST_CASE("exportForUnreal honours the mapping's kind")
{
    buildScene();
    fs::path file = tempFile("unreal_skeletal.fbx");
    std::string err;
    REQUIRE(fbx::exportForUnreal({node("Tube").id()}, file, "ue5", &err, "skeletal"));
    UfbxScene s(file);
    REQUIRE(s.scene);
    CHECK(s.scene->skin_deformers.count == 1);
    CHECK(s.scene->bones.count == 3);
    CHECK(!fbx::exportForUnreal({node("Tube").id()}, file, "ue5", &err, "bogus"));
}

/**
 * Writes the FBX plus what Lila computes for it (file-space joint heads and skinned points per frame) for the
 * Blender check in this folder, run by hand: see validate_blender.py.
 */
TEST_CASE("reference data for external DCC validation")
{
    for (fbx::AxisProfile profile : {fbx::AxisProfile::Ue5, fbx::AxisProfile::Generic})
    {
        buildScene();
        std::vector<Frame> frames = sample();
        const std::string stem = std::string("dcc_skin_") + fbx::profileName(profile);
        fs::path file = tempFile(stem + ".fbx");
        run("exportFbx", {{"args", {file.string(), "Tube", "bone_a"}}, {"kind", "skeletal"}, {"profile", fbx::profileName(profile)}});

        const glm::dmat3 basis = fbx::profileBasis(profile, math::upAxis());
        json ref = {{"fbx", file.generic_string()}, {"fps", 24}, {"frames", kFrames}, {"bones", kJoints}};
        json perFrame = json::array();
        for (const Frame& fr : frames)
        {
            json heads = json::object();
            for (const auto& [name, m] : fr.joints)
            {
                glm::dvec3 h = basis * glm::dvec3(m[3]);
                heads[name] = v3(h.x, h.y, h.z);
            }
            json pts = json::array();
            for (const glm::dvec3& p : fr.points)
            {
                glm::dvec3 q = basis * p;
                pts.push_back(v3(q.x, q.y, q.z));
            }
            perFrame.push_back({{"heads", heads}, {"points", pts}});
        }
        ref["perFrame"] = perFrame;
        std::ofstream(tempFile(stem + ".json")) << ref.dump();
        CHECK(fs::exists(file));
    }
}
