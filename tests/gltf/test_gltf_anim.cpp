#include "Helpers.h"

#include "geo/Primitives.h"
#include "math/MathCore.h"
#include "scene/Anim.h"
#include "scene/Deformers.h"
#include "scene/Morphs.h"
#include "scene/Rig.h"
#include "scene/Skin.h"
#include "scene/Take.h"

#include <format>
#include <map>

using namespace gltftest;

namespace
{
json v3(double x, double y, double z) { return json::array({x, y, z}); }

const std::vector<std::string> kJoints = {"bone_a", "bone_b", "bone_c"};
constexpr int kFrames = 24;

/**
 * A 100 cm cylinder bound to a three-joint chain with jointOrients and a non-default rotate order, animated over one
 * second at 24 fps, with a morph target whose weight is keyed. eulerCurves keys the joints with Euler curves instead
 * of rotation curves.
 */
void buildSkinned(bool eulerCurves)
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
    if (eulerCurves)
        for (const std::string& j : kJoints)
            run("setAttr", {{"args", {j + ".rotationInterpolation", 0}}});
    run("skinCluster", {{"args", {"bone_a", "Tube"}}});

    Graph& g = graph();
    // A bulge on the top ring as a morph target.
    const NodeId tube = node("Tube").id();
    MeshPtr base = skin::inputMesh(g, skin::find(g, tube));
    REQUIRE(base);
    auto deltas = std::make_shared<MorphDeltas>();
    for (geo::Index p = 0; p < base->pointCapacity(); ++p)
        if (base->pointValid(p) && base->position(p).y > 40.0)
        {
            deltas->indices.push_back(p);
            deltas->deltas.push_back(glm::dvec3(base->position(p).x - 3.0, 0.0, base->position(p).z) * 0.5);
        }
    const NodeId morph = morphs::create(g, tube, "Morph");
    const NodeId target = morphs::addTarget(g, morph, "Bulge", deltas, 0.0);

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
    anim::setKey(g, {target, morphs::kWeight, -1}, 0.0, 0.0);
    anim::setKey(g, {target, morphs::kWeight, -1}, 1.0, 1.0);
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
            fr.joints[j] = world(j);
        MeshPtr m = node("Tube").as<MeshPtr>(MeshNode::kOutMesh);
        REQUIRE(m);
        const glm::dmat4 meshWorld = world("Tube");
        for (const glm::dvec3& p : m->positions())
            fr.points.push_back(glm::dvec3(meshWorld * glm::dvec4(p, 1.0)));
        std::sort(fr.points.begin(), fr.points.end(), [](const glm::dvec3& a, const glm::dvec3& b) {
            return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
        });
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
        // Points are matched as nearest neighbours, so a reordering of points doesn't matter.
        for (const glm::dvec3& p : a[f].points)
        {
            double best = 1e30;
            for (const glm::dvec3& q2 : b[f].points)
                best = std::min(best, glm::length(p - q2));
            worstPoint = std::max(worstPoint, best);
        }
    }
    MESSAGE("worst joint matrix column error " << worstJoint << ", worst skinned point error " << worstPoint << " cm");
    CHECK(worstJoint < eps);
    CHECK(worstPoint < eps);
}

void roundTrip(bool eulerCurves, bool bake, bool draco = false)
{
    buildSkinned(eulerCurves);
    const std::vector<Frame> before = sample();
    const fs::path file = tempDir("skin") / std::format("skin_{}_{}{}.glb", eulerCurves ? "euler" : "quat", bake ? "baked" : "keys", draco ? "_draco" : "");
    run("exportGltf", {{"args", {file.string(), "Tube", "bone_a"}}, {"bakeAnimation", bake}, {"draco", draco}});
    {
        Parsed p(file);
        REQUIRE(p.data);
        REQUIRE(p.data->skins_count == 1);
        CHECK(p.data->skins[0].joints_count == 3);
        REQUIRE(p.data->skins[0].inverse_bind_matrices);
        REQUIRE(p.data->animations_count == 1);
        const cgltf_node* tube = p.find("Tube");
        REQUIRE(tube);
        REQUIRE(tube->mesh);
        CHECK(tube->skin == &p.data->skins[0]);
        const cgltf_primitive& prim = tube->mesh->primitives[0];
        CHECK(prim.has_draco_mesh_compression == draco);
        CHECK(attribute(prim, cgltf_attribute_type_joints, 0));
        CHECK(attribute(prim, cgltf_attribute_type_weights, 0));
        REQUIRE(prim.targets_count == 1);
        REQUIRE(tube->mesh->target_names_count == 1);
        CHECK(std::string(tube->mesh->target_names[0]) == "Bulge");
        bool weights = false, keyedTranslation = false;
        const cgltf_animation& a = p.data->animations[0];
        for (size_t c = 0; c < a.channels_count; ++c)
        {
            weights = weights || a.channels[c].target_path == cgltf_animation_path_type_weights;
            if (a.channels[c].target_path == cgltf_animation_path_type_translation && a.channels[c].target_node == p.find("bone_a"))
                keyedTranslation = a.channels[c].sampler->input->count == 2;
        }
        CHECK(weights);
        // With bakeAnimation=False the root's translate curve goes out as its two keys.
        CHECK(keyedTranslation == !bake);
    }

    run("newScene");
    run("playbackOptions", {{"fps", 30.0}, {"minTime", 0.0}, {"maxTime", 4.0}});
    json r = run("importGltf", {{"args", {file.string()}}, {"report", true}});
    CHECK(r["errors"].empty());
    CHECK(r["joints"].size() == 3);
    CHECK(r["skinClusters"].size() == 1);
    CHECK(r["morphs"].size() == 1);
    CHECK(r["fps"].get<double>() == doctest::Approx(24.0));
    CHECK(anim::playback().fps == doctest::Approx(24.0));
    CHECK(anim::playback().maxTime == doctest::Approx(1.0));
    // Draco quantizes positions (14 bits over the cylinder) and skin weights (12 bits).
    compare(before, sample(), draco ? 0.05 : 2e-3);
}
} // namespace

TEST_CASE("skinned, morphed, animated cylinder round-trips through glTF")
{
    roundTrip(false, true);
    roundTrip(true, true);
    roundTrip(true, false);
    math::setUpAxis(math::UpAxis::Z);
    roundTrip(false, true);
    math::setUpAxis(math::UpAxis::Y);
}

TEST_CASE("draco: a skinned, morphed, animated cylinder round-trips compressed")
{
    roundTrip(false, true, true);
}

TEST_CASE("more than four influences go out as extra JOINTS/WEIGHTS sets and come back")
{
    run("newScene");
    Graph& g = graph();
    Node* plane = addMesh(g, "Sheet", geo::makePlane({.width = 100, .depth = 100, .subdivX = 3, .subdivZ = 3}));
    std::vector<std::string> names;
    run("select", {{"args", json::array()}});
    for (int j = 0; j < 6; ++j)
    {
        names.push_back("j" + std::to_string(j));
        run("joint", {{"position", v3(j * 10.0, 0, 0)}, {"name", names.back()}});
    }
    run("skinCluster", {{"args", {"j0", "Sheet"}}});
    const NodeId sc = skin::find(g, plane->id());
    REQUIRE(sc != kNoNode);
    const std::vector<skin::Influence> infs = skin::influences(g, sc);
    REQUIRE(infs.size() == 6);
    MeshPtr bind = skin::inputMesh(g, sc);
    WeightTable t;
    t.points.resize(bind->pointCapacity());
    for (geo::Index p = 0; p < bind->pointCapacity(); ++p)
    {
        float total = 0.0f;
        for (size_t k = 0; k < infs.size(); ++k)
            total += float(k + 1 + p % 3);
        for (size_t k = 0; k < infs.size(); ++k)
            t.points[p].push_back({infs[k].index, float(k + 1 + p % 3) / total});
    }
    skin::setWeights(g, sc, t);
    auto weightsByName = [&](NodeId skinId) {
        std::map<std::pair<size_t, std::string>, float> out;
        const auto list = skin::influences(g, skinId);
        WeightTable table = skin::table(g, skinId);
        MeshPtr m = skin::inputMesh(g, skinId);
        // Keyed by point position so a renumbering doesn't matter.
        for (geo::Index p = 0; p < table.points.size(); ++p)
            for (const auto& [id, w] : table.points[p])
                for (const skin::Influence& inf : list)
                    if (inf.index == id)
                    {
                        const glm::dvec3 pos = m->position(p);
                        out[{size_t(std::llround(pos.x * 10) * 100000 + std::llround(pos.z * 10)), g.node(inf.joint)->name()}] = w;
                    }
        return out;
    };
    const auto before = weightsByName(sc);

    const fs::path file = tempDir("influences") / "six.gltf";
    run("exportGltf", {{"args", {file.string(), "Sheet", "j0"}}, {"animation", false}});
    {
        Parsed p(file);
        REQUIRE(p.data);
        const cgltf_primitive& prim = p.find("Sheet")->mesh->primitives[0];
        CHECK(attribute(prim, cgltf_attribute_type_joints, 1));
        CHECK(attribute(prim, cgltf_attribute_type_weights, 1));
    }
    run("newScene");
    json r = run("importGltf", {{"args", {file.string()}}, {"report", true}});
    REQUIRE(r["skinClusters"].size() == 1);
    const NodeId sc2 = skin::find(graph(), node("Sheet").id());
    CHECK(graph().node(sc2)->as<int64_t>(SkinClusterNode::kMaxInfluences) == 6);
    const auto after = weightsByName(sc2);
    REQUIRE(after.size() == before.size());
    for (const auto& [k, w] : before)
    {
        REQUIRE(after.count(k));
        CHECK(after.at(k) == doctest::Approx(w).epsilon(1e-5));
    }
}

TEST_CASE("scene takes go out as one animation each and come back as takes")
{
    run("newScene");
    run("playbackOptions", {{"fps", 24.0}, {"minTime", 0.0}, {"maxTime", 1.0}});
    Graph& g = graph();
    Node* mover = addNode(g, "locator", "Mover");
    const NodeId id = mover->id();
    anim::setKey(g, {id, Transform::kTranslate, 0}, 0.0, 0.0);
    anim::setKey(g, {id, Transform::kTranslate, 0}, 1.0, 100.0);
    anim::setKey(g, {id, Transform::kTranslate, 1}, 0.0, 0.0);
    anim::TakeSettings s;
    s.start = 0.0;
    s.end = 1.0;
    s.fps = 24.0;
    const NodeId walk = anim::createTake(g, "Walk", s);
    s.end = 2.0;
    const NodeId jump = anim::createTake(g, "Jump", s);
    anim::setActiveTake(g, jump);
    anim::setKey(g, {id, Transform::kTranslate, 1}, 0.0, 0.0);
    anim::setKey(g, {id, Transform::kTranslate, 1}, 2.0, 50.0);
    anim::setKey(g, {id, Transform::kTranslate, 0}, 0.0, 0.0);
    anim::setActiveTake(g, walk);
    // What each take poses, sampled before export; every channel is keyed in both, so switching leaves nothing behind.
    auto pose = [&](Graph& graph, NodeId take, double t) {
        anim::setActiveTake(graph, take);
        graph.setTime(t);
        graph.evaluate();
        return graph.node(graph.find("Mover")->id())->as<glm::dvec3>(Transform::kTranslate);
    };
    std::vector<glm::dvec3> before;
    for (double t : {0.0, 0.5, 1.0})
        before.push_back(pose(g, walk, t));
    for (double t : {0.0, 1.0, 2.0})
        before.push_back(pose(g, jump, t));
    anim::setActiveTake(g, walk);
    g.setTime(0.0);
    g.evaluate();

    const fs::path file = tempDir("takes") / "takes.glb";
    run("exportGltf", {{"args", {file.string(), "Mover"}}, {"takes", "all"}});
    {
        Parsed p(file);
        REQUIRE(p.data);
        REQUIRE(p.data->animations_count == 2);
        CHECK(std::string(p.data->animations[0].name) == "Walk");
        CHECK(std::string(p.data->animations[1].name) == "Jump");
    }
    run("newScene");
    json r = run("importGltf", {{"args", {file.string()}}, {"report", true}});
    REQUIRE(r["takes"].size() == 2);
    Graph& g2 = graph();
    REQUIRE(anim::takes(g2).size() == 2);
    CHECK(g2.node(g2.find("Walk")->id())->as<bool>(anim::TakeNode::kActive));
    std::vector<glm::dvec3> after;
    for (double t : {0.0, 0.5, 1.0})
        after.push_back(pose(g2, g2.find("Walk")->id(), t));
    for (double t : {0.0, 1.0, 2.0})
        after.push_back(pose(g2, g2.find("Jump")->id(), t));
    REQUIRE(after.size() == before.size());
    for (size_t i = 0; i < before.size(); ++i)
    {
        CAPTURE(i);
        CHECK(glm::length(after[i] - before[i]) < 1e-4);
    }
    CHECK(after[2].x == doctest::Approx(100.0));
    CHECK(after[5].y == doctest::Approx(50.0));
}

TEST_CASE("the exported clip spans the playback range in frames at the scene fps")
{
    // Frames 1 to 120 at 30 fps: Lila's times are seconds, so the clip is 120 samples over 119/30 s.
    run("newScene");
    run("playbackOptions", {{"fps", 30.0}, {"minTime", 1.0 / 30.0}, {"maxTime", 120.0 / 30.0}, {"animationStartTime", 1.0 / 30.0},
                            {"animationEndTime", 120.0 / 30.0}});
    Graph& g = graph();
    const NodeId id = addNode(g, "locator", "Mover")->id();
    anim::setKey(g, {id, Transform::kTranslate, 0}, 1.0 / 30.0, 0.0);
    anim::setKey(g, {id, Transform::kTranslate, 0}, 120.0 / 30.0, 100.0);
    const fs::path file = tempDir("range") / "range.glb";
    run("exportGltf", {{"args", {file.string(), "Mover"}}, {"bakeAnimation", true}});
    Parsed p(file);
    REQUIRE(p.data);
    REQUIRE(p.data->animations_count == 1);
    const cgltf_animation& a = p.data->animations[0];
    REQUIRE(a.samplers_count > 0);
    for (size_t i = 0; i < a.samplers_count; ++i)
    {
        const cgltf_accessor* in = a.samplers[i].input;
        REQUIRE(in->has_min);
        REQUIRE(in->has_max);
        CHECK(in->count == 120);
        CHECK(in->max[0] - in->min[0] == doctest::Approx(119.0 / 30.0).epsilon(1e-5));
    }
}
