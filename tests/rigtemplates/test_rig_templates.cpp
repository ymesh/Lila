#include "ops/Command.h"
#include "extensions/ExtensionManager.h"
#include "scene/AutoPose.h"
#include "scene/CenterInMesh.h"
#include "scene/Graph.h"
#include "scene/Picker.h"
#include "scene/Poses.h"
#include "scene/Rig.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/DrawOverrides.h"
#include "ui/Menus.h"
#include "ui/Panels.h"

#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>

using namespace lila;
using namespace lila::scene;

namespace
{
json run(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }

Graph& graph() { return Graph::current(); }

void ensureLoaded()
{
    auto& em = extensions::ExtensionManager::get();
    if (!em.isLoaded("RigTemplates"))
        REQUIRE(em.load(LILA_TEST_RIG_TEMPLATES_PACKAGE) == "RigTemplates");
}

glm::dmat4 world(const std::string& name)
{
    Graph& g = graph();
    g.evaluate();
    Node* n = g.find(name);
    REQUIRE_MESSAGE(n, name);
    return n->as<glm::dmat4>(Transform::kWorldMatrix);
}

double matrixDiff(const glm::dmat4& a, const glm::dmat4& b, bool rotationOnly = false)
{
    double d = 0.0;
    for (int c = 0; c < (rotationOnly ? 3 : 4); ++c)
        for (int r = 0; r < 3; ++r)
            d = std::max(d, std::abs(a[c][r] - b[c][r]));
    return d;
}

std::vector<std::string> strings(const json& j)
{
    std::vector<std::string> out;
    for (const json& e : j)
        out.push_back(e.get<std::string>());
    return out;
}

/** Joint name -> parent joint name, for every joint under (and including) top. */
std::map<std::string, std::string> hierarchy(const std::string& top, const std::string& strip = {})
{
    Graph& g = graph();
    std::map<std::string, std::string> out;
    auto bare = [&](const std::string& n) { return n.starts_with(strip) ? n.substr(strip.size()) : n; };
    NodeId root = g.find(top)->id();
    out[bare(top)] = "";
    for (NodeId d : g.descendants(root))
        if (rig::isJoint(*g.node(d)))
            out[bare(g.node(d)->name())] = bare(g.node(g.parent(d))->name());
    return out;
}

json registries()
{
    std::vector<std::string> poseNames;
    for (const poses::Entry& e : poses::list("", poses::Tier::Template))
        poseNames.push_back(e.pose.character + "/" + e.pose.name);
    std::vector<std::string> pickers;
    for (const picker::Document& d : picker::templatePickers())
        pickers.push_back(d.name);
    return {{"commands", ops::registry().ids()},       {"types", types::registry().ids()},
            {"drawOverrides", ui::drawOverrides::registry().ids()}, {"actions", ui::actions::registry().ids()},
            {"menuItems", ui::menus::itemRegistry().ids()}, {"panels", ui::panels::registry().ids()},
            {"poses", poseNames},                       {"pickers", pickers}};
}

std::filesystem::path tempDir()
{
    auto dir = std::filesystem::temp_directory_path() / "lila_rig_templates_tests" / "files";
    std::filesystem::create_directories(dir);
    return dir;
}
} // namespace

TEST_CASE("rig templates: loading registers everything, unloading restores every registry")
{
    auto& em = extensions::ExtensionManager::get();
    if (em.isLoaded("RigTemplates"))
        em.unload("RigTemplates");
    run("newScene");
    const json before = registries();
    REQUIRE(em.load(LILA_TEST_RIG_TEMPLATES_PACKAGE) == "RigTemplates");
    const json loaded = registries();
    for (auto& [key, ids] : loaded.items())
        CHECK_MESSAGE(ids != before[key], key);
    CHECK(*ops::registry().ownerOf("buildRigSkeleton") == "RigTemplates");
    CHECK(*types::registry().ownerOf("rigMarker") == "RigTemplates");
    CHECK(*ui::drawOverrides::registry().ownerOf("rigTemplates.marker") == "RigTemplates");
    CHECK(ui::actions::find("rigTemplates.place"));
    CHECK(poses::find("UE5_Manny", "T", poses::Tier::Template));
    CHECK(picker::find("UE5_Manny"));

    // The built-in template is there and shipped next to the library.
    json list = run("listRigTemplates");
    REQUIRE(list.size() >= 1);
    bool manny = false;
    for (const json& t : list)
        manny |= t["id"] == "UE5_Manny" && t["tier"] == "builtin";
    CHECK(manny);

    em.unload("RigTemplates");
    CHECK(registries() == before);
    CHECK_FALSE(poses::find("UE5_Manny", "T", poses::Tier::Template));
}

TEST_CASE("rig templates: .lilarig save and load round trip")
{
    ensureLoaded();
    const json info = run("rigTemplateInfo", {{"args", {"UE5_Manny"}}});
    CHECK(info["format"] == "lilarig");
    CHECK(info["markers"].size() == 161);

    const std::filesystem::path file = tempDir() / "manny_copy.lilarig";
    run("saveRigTemplate", {{"args", {"UE5_Manny", file.generic_string()}}});
    std::ifstream in(file);
    const json written = json::parse(in);
    CHECK(written == info);
    CHECK(run("loadRigTemplate", {{"args", {file.generic_string()}}}) == "UE5_Manny");
    CHECK(run("rigTemplateInfo", {{"args", {"UE5_Manny"}}}) == info);

    // Broken files say what is wrong.
    json broken = info;
    broken["markers"][1]["parent"] = "nobody";
    const std::filesystem::path bad = tempDir() / "broken.lilarig";
    std::ofstream(bad) << broken.dump();
    CHECK_THROWS_WITH_AS(run("loadRigTemplate", {{"args", {bad.generic_string()}}}), doctest::Contains("unknown marker nobody"), CommandError);
}

TEST_CASE("rig templates: building UE5 Manny matches the source skeleton")
{
    if (!std::filesystem::exists(LILA_TEST_MANNY_FBX))
        return;
    ensureLoaded();
    run("newScene");
    json imported = run("importFbx", {{"args", {LILA_TEST_MANNY_FBX}}, {"animation", false}, {"report", true}});
    const std::vector<std::string> source = strings(imported["joints"]);
    REQUIRE(source.size() == 161);

    json built = run("buildRigSkeleton", {{"args", {"UE5_Manny"}}, {"prefix", "t_"}});
    CHECK(built["root"] == "t_root");
    std::set<std::string> names;
    for (const std::string& n : strings(built["joints"]))
        names.insert(n.substr(2));
    CHECK(names == std::set<std::string>(source.begin(), source.end()));
    CHECK(hierarchy("t_root", "t_") == hierarchy("root"));

    double worst = 0.0;
    std::string worstName;
    for (const std::string& n : source)
        if (double d = matrixDiff(world(n), world("t_" + n)); d > worst)
            worst = d, worstName = n;
    CHECK_MESSAGE(worst < 1e-4, worstName);

    // Built joints carry their orientation in jointOrient, with rotate free for animation.
    for (const char* n : {"t_pelvis", "t_hand_l", "t_foot_r"})
        CHECK(glm::length(graph().find(n)->as<glm::dvec3>(Transform::kRotate)) == 0.0);

    // UE bones aim down their X, which stays local X in Lila once the FBX import has turned the character to face forward.
    const glm::dmat4 upper = world("t_upperarm_l"), lower = world("t_lowerarm_l");
    CHECK(glm::dot(glm::normalize(glm::dvec3(upper[0])), glm::normalize(glm::dvec3(lower[3] - upper[3]))) > 0.999);
}

TEST_CASE("rig templates: templateFromSkeleton labels Manny's main bones")
{
    if (!std::filesystem::exists(LILA_TEST_MANNY_FBX))
        return;
    ensureLoaded();
    run("newScene");
    run("importFbx", {{"args", {LILA_TEST_MANNY_FBX}}, {"animation", false}});
    json s = run("templateFromSkeleton", {{"args", {"pelvis"}}, {"id", "MannyCopy"}});
    CHECK(s["markers"] == 161);
    const json t = run("rigTemplateInfo", {{"args", {"MannyCopy"}}});
    std::map<std::string, json> byName;
    for (const json& m : t["markers"])
        byName[m["name"].get<std::string>()] = m;
    for (const char* n : {"pelvis", "spine_01", "spine_02", "spine_03", "spine_04", "spine_05", "neck_01", "head", "clavicle_l", "upperarm_l",
                          "lowerarm_l", "hand_l", "hand_r", "thumb_01_l", "index_02_r", "pinky_03_l", "thigh_l", "calf_r", "foot_l", "ball_r"})
    {
        CHECK_MESSAGE(byName[n]["label"] == n, n);
        CHECK_MESSAGE(byName[n]["placement"] == "free", n);
    }
    CHECK(byName["hand_l"]["side"] == "left");
    CHECK(byName["hand_l"]["mirror"] == "hand_r");
    CHECK(byName["ik_hand_l"]["flags"] == json::array({"ik"}));
    CHECK(byName["ik_hand_l"]["follow"] == "hand_l");
    CHECK(byName["upperarm_twist_01_l"]["placement"] == "segment");
    CHECK(t["poses"].contains("bind"));
    CHECK(t["poses"].contains("T"));
    CHECK(t["poses"].contains("A"));
    CHECK(t["aliases"]["upperarm_l"].size() > 0);
    CHECK(!t["picker"]["pages"][0]["items"].empty());
}

TEST_CASE("rig templates: orient an off-axis skeleton to the template")
{
    ensureLoaded();
    run("newScene");
    // The bare skeleton: twist drivers would hold the twist joints to their rest under the scrambled orients.
    json built = run("buildRigSkeleton", {{"args", {"UE5_Manny"}}, {"rig", false}});
    std::map<std::string, glm::dmat4> want;
    for (const std::string& n : strings(built["joints"]))
        want[n] = world(n);

    // Turn every joint's axes off the template's, keeping every joint where it is.
    auto scramble = [&] {
        Graph& g = graph();
        int i = 0;
        for (const std::string& n : strings(built["joints"]))
        {
            ++i;
            NodeId id = g.find(n)->id();
            std::vector<std::pair<NodeId, glm::dmat4>> kids;
            for (NodeId c : g.children(id))
                kids.push_back({c, world(g.node(c)->name())});
            g.setValue({id, Joint::kJointOrient, -1}, glm::dvec3((i * 37) % 50 - 25, (i * 53) % 70 - 35, (i * 17) % 40 - 20));
            const glm::dmat4 inv = glm::inverse(world(n));
            for (auto& [c, w] : kids)
                Transform::setLocalMatrix(g, c, inv * w);
        }
    };
    scramble();
    double off = 0.0;
    for (auto& [n, m] : want)
    {
        CHECK_MESSAGE(glm::length(glm::dvec3(world(n)[3] - m[3])) < 1e-6, n); // positions kept
        off = std::max(off, matrixDiff(world(n), m, true));
    }
    REQUIRE(off > 0.1);

    // One limb only: the left arm comes back, the spine stays off.
    run("orientRigSkeleton", {{"args", {"hand_l"}}, {"template", "UE5_Manny"}, {"limbs", {"arm_l"}}});
    for (const char* n : {"clavicle_l", "upperarm_l", "lowerarm_l", "hand_l", "upperarm_twist_01_l"})
        CHECK_MESSAGE(matrixDiff(world(n), want[n]) < 1e-4, n);
    CHECK(matrixDiff(world("spine_02"), want["spine_02"], true) > 0.1);

    // The whole skeleton, with the template found from the joint names.
    json changed = run("orientRigSkeleton", {{"args", {"spine_03"}}});
    CHECK(changed.size() > 100);
    double worst = 0.0;
    std::string worstName;
    for (auto& [n, m] : want)
        if (double d = matrixDiff(world(n), m); d > worst)
            worst = d, worstName = n;
    CHECK_MESSAGE(worst < 1e-4, worstName);
    for (const char* n : {"pelvis", "upperarm_r", "calf_l"})
        CHECK(glm::length(graph().find(n)->as<glm::dvec3>(Transform::kRotate)) < 1e-9);

    // A skinned skeleton goes through editSkeleton: the pose stays as it is and rotate takes up the change.
    json cyl = run("polyCylinder", {{"radius", 20}, {"height", 180}, {"caps", false}});
    run("move", {{"args", {cyl[0]}}, {"to", {0.0, 90.0, 0.0}}});
    scramble();
    run("skinCluster", {{"args", {"root", cyl[0]}}});
    std::map<std::string, glm::dmat4> posed;
    for (auto& [n, m] : want)
        posed[n] = world(n);
    const glm::dvec3 joBefore = graph().find("upperarm_l")->as<glm::dvec3>(Joint::kJointOrient);
    run("orientRigSkeleton", {{"args", {"root"}}});
    double moved = 0.0;
    for (auto& [n, m] : posed)
        moved = std::max(moved, matrixDiff(world(n), m));
    CHECK(moved < 1e-6);
    CHECK(glm::length(graph().find("upperarm_l")->as<glm::dvec3>(Joint::kJointOrient) - joBefore) > 1.0);
}

TEST_CASE("rig templates: markers centre in a bound mesh, and bind, unbind and mirror")
{
    ensureLoaded();
    run("newScene");
    json cyl = run("polyCylinder", {{"name", "Body"}, {"radius", 25}, {"height", 220}, {"subdivisionsAxis", 32}, {"caps", false}});
    const std::string mesh = cyl[0].get<std::string>();
    run("move", {{"args", {mesh}}, {"to", {8.0, 100.0, 3.0}}});

    std::string root = run("placeRigTemplate", {{"args", {"UE5_Manny"}}, {"name", "Manny"}, {"meshes", {mesh}}});
    CHECK(root == "Manny");
    Graph& g = graph();
    CHECK(rig::centerMeshes(g, g.find("spine_03_marker")->id()) == std::vector<NodeId>{g.find(mesh)->id()});

    // A marker centres across its limb (from its parent marker) like a joint across its bone.
    json moved = run("centerJoint", {{"args", {"spine_03_marker"}}, {"keepChildren", true}});
    CHECK(moved == json::array({"spine_03_marker"}));
    glm::dvec3 p(world("spine_03_marker")[3]);
    CHECK(std::abs(p.x - 8.0) < 0.05);
    CHECK(std::abs(p.z - 3.0) < 0.05);

    json centred = run("centerRigMarkers", {{"args", {root}}});
    CHECK(centred.size() > 5);
    glm::dvec3 neck(world("neck_01_marker")[3]);
    CHECK(std::abs(neck.x - 8.0) < 0.05);
    CHECK(std::abs(neck.z - 3.0) < 0.05);

    // Building from the instance uses the markers' positions.
    json built = run("buildRigSkeleton", {{"args", {"spine_03_marker"}}, {"prefix", "c_"}});
    CHECK(glm::length(glm::dvec3(world("c_spine_03")[3] - world("spine_03_marker")[3])) < 1e-6);

    // Mirror: the right hand marker becomes the left one reflected across the lateral axis (Lila X for Manny).
    run("move", {{"args", {"hand_l_marker"}}, {"by", {0.0, 4.0, 2.0}}});
    run("mirrorRigMarkers", {{"args", {root}}});
    glm::dvec3 l(world("hand_l_marker")[3]), r(world("hand_r_marker")[3]);
    CHECK(glm::length(glm::dvec3(-l.x, l.y, l.z) - r) < 1e-6);

    run("unbindRigTemplate", {{"args", {root}}});
    CHECK(rig::centerMeshes(g, g.find("spine_03_marker")->id()).empty());
    run("bindRigTemplate", {{"args", {root, mesh}}});
    CHECK(rig::centerMeshes(g, g.find("hand_l_marker")->id()).size() == 1);

}

TEST_CASE("rig templates: template poses apply to a built skeleton, and the picker is generated")
{
    ensureLoaded();
    run("newScene");
    run("buildRigSkeleton", {{"args", {"UE5_Manny"}}});
    run("select", {{"args", {"pelvis"}}});
    json report = run("applyPose", {{"args", {"T"}}, {"character", "UE5_Manny"}});
    CHECK(report["missing"].empty());
    const glm::dmat4 upper = world("upperarm_l"), lower = world("lowerarm_l"), hand = world("hand_l");
    CHECK(glm::dot(glm::normalize(glm::dvec3(lower[3] - upper[3])), glm::dvec3(1, 0, 0)) > 0.999);
    CHECK(glm::dot(glm::normalize(glm::dvec3(hand[3] - lower[3])), glm::dvec3(1, 0, 0)) > 0.999);
    run("applyPose", {{"args", {"bind"}}, {"character", "UE5_Manny"}});
    CHECK(glm::length(graph().find("upperarm_l")->as<glm::dvec3>(Transform::kRotate)) < 1e-9);

    // The pose library finds the template's character from the skeleton itself.
    // MannyCopy (made from the same skeleton above) ties with it; either names the character.
    const poses::Character ch = poses::characterOf(graph(), graph().find("hand_l")->id());
    CHECK((ch.templateId == "UE5_Manny" || ch.templateId == "MannyCopy"));
    CHECK(ch.root == graph().find("root")->id());

    CHECK(run("installRigTemplatePoses", {{"args", {"UE5_Manny"}}, {"tier", "scene"}}).size() == 3);
    CHECK(poses::find("UE5_Manny", "A", poses::Tier::Scene));

    CHECK(run("rigTemplatePicker", {{"args", {"UE5_Manny"}}}) == "UE5_Manny");
    const picker::Document* doc = picker::findScene("UE5_Manny");
    REQUIRE(doc);
    REQUIRE(!doc->pages.empty());
    std::set<std::string> targets;
    for (const picker::Item& it : doc->pages[0].items)
        targets.insert(it.targets.begin(), it.targets.end());
    for (const char* n : {"pelvis", "head", "hand_l", "hand_r", "foot_l", "index_01_r"})
        CHECK_MESSAGE(targets.contains(n), n);
}

TEST_CASE("rig templates: built skeletons get the template's default spaces")
{
    ensureLoaded();
    run("newScene");
    run("buildRigSkeleton", {{"args", {"UE5_Manny"}}, {"prefix", "s_"}});
    auto names = [](const std::string& joint) {
        std::vector<std::string> out;
        const json list = run("listSpaces", {{"args", {joint}}});
        for (const json& s : list["spaces"])
            out.push_back(s["name"].get<std::string>());
        return out;
    };
    using V = std::vector<std::string>;
    CHECK(names("s_hand_l") == V{"parent", "world", "s_spine_05", "s_head"});
    CHECK(names("s_hand_r") == V{"parent", "world", "s_spine_05", "s_head"});
    // A space that is the joint's parent is the parent space already.
    CHECK(names("s_head") == V{"parent", "world"});
    CHECK(names("s_pelvis") == V{"parent", "world"});
    CHECK(names("s_foot_l") == V{"parent", "world", "s_root"});
    CHECK(names("s_ik_foot_r") == V{"parent", "world", "s_root"});
    CHECK(names("s_spine_02") == V{"parent"});

    run("buildRigSkeleton", {{"args", {"UE5_Manny"}}, {"prefix", "n_"}, {"spaces", false}});
    CHECK(names("n_hand_l") == V{"parent"});
    CHECK(matrixDiff(world("s_hand_l") * glm::inverse(world("s_root")), world("n_hand_l") * glm::inverse(world("n_root"))) < 1e-9);

    // In world space the hand stays put while the body moves.
    const glm::dmat4 hand = world("s_hand_l");
    run("switchSpace", {{"args", {"s_hand_l"}}, {"space", "world"}});
    CHECK(matrixDiff(world("s_hand_l"), hand) < 1e-9);
    run("setAttr", {{"args", {"s_pelvis.translate", json::array({0, 10, 5})}}});
    CHECK(matrixDiff(world("s_hand_l"), hand) < 1e-9);
}

TEST_CASE("rig templates: Manny builds as a working rig in one step")
{
    ensureLoaded();
    run("newScene");
    json bare = run("buildRigSkeleton", {{"args", {"UE5_Manny"}}, {"prefix", "b_"}, {"rig", false}});
    CHECK(bare["rig"].empty());
    json built = run("buildRigSkeleton", {{"args", {"UE5_Manny"}}});
    Graph& g = graph();
    std::map<std::string, int> kinds;
    for (const json& e : built["rig"])
        ++kinds[g.find(e.get<std::string>())->type().name];
    CHECK(kinds["ikChain"] == 4);
    CHECK(kinds["reverseFoot"] == 2);
    CHECK(kinds["looker"] == 1);
    CHECK(kinds["twistDriver"] == 16);

    // Deform flags: IK helpers and correctives stay behind, twist joints deform.
    auto deform = [&](const char* n) { return g.find(n)->as<bool>(Joint::kDeform); };
    CHECK_FALSE(deform("ik_foot_l"));
    CHECK_FALSE(deform("ik_hand_root"));
    CHECK_FALSE(deform("upperarm_bicep_l"));
    CHECK_FALSE(deform("center_of_mass"));
    CHECK(deform("upperarm_twist_01_l"));
    CHECK(deform("hand_l"));
    CHECK(deform("root"));

    // The rig changes nothing at rest: every joint sits where the bare skeleton's does.
    double worst = 0.0;
    for (const std::string& n : strings(bare["joints"]))
        worst = std::max(worst, matrixDiff(world(n), world(n.substr(2))));
    CHECK(worst < 1e-6);

    // Twisting the upper arm along its bone: the shoulder-end twist joint keeps two thirds of it back.
    auto turned = [](const glm::dmat4& a, const glm::dmat4& b) {
        const glm::dquat qa = glm::quat_cast(glm::dmat3(glm::normalize(glm::dvec3(a[0])), glm::normalize(glm::dvec3(a[1])), glm::normalize(glm::dvec3(a[2]))));
        const glm::dquat qb = glm::quat_cast(glm::dmat3(glm::normalize(glm::dvec3(b[0])), glm::normalize(glm::dvec3(b[1])), glm::normalize(glm::dvec3(b[2]))));
        return glm::degrees(2.0 * std::acos(std::min(1.0, std::abs(glm::dot(qa, qb)))));
    };
    const glm::dmat4 twist0 = world("upperarm_twist_01_l");
    const glm::dvec3 bone = glm::normalize(glm::dvec3(world("lowerarm_l")[3] - world("upperarm_l")[3]));
    const glm::dmat3 spin(glm::rotate(glm::dmat4(1.0), glm::radians(60.0), bone));
    const glm::dmat4 up = world("upperarm_l");
    glm::dmat4 want = glm::dmat4(spin) * up;
    want[3] = up[3];
    Transform::setLocalMatrix(g, g.find("upperarm_l")->id(), glm::inverse(world("clavicle_l")) * want);
    CHECK(turned(world("upperarm_l"), up) == doctest::Approx(60.0).epsilon(1e-6));
    CHECK(turned(world("upperarm_twist_01_l"), twist0) == doctest::Approx(20.0).epsilon(1e-3));
    // The wrist's twist spreads up the forearm: the twist joint nearer the hand takes two thirds of it.
    const glm::dvec3 fore = glm::normalize(glm::dvec3(world("hand_l")[3] - world("lowerarm_l")[3]));
    const glm::dmat4 lower0 = world("lowerarm_twist_01_l"), hand = world("hand_l");
    glm::dmat4 handWant = glm::dmat4(glm::dmat3(glm::rotate(glm::dmat4(1.0), glm::radians(45.0), fore))) * hand;
    handWant[3] = hand[3];
    Transform::setLocalMatrix(g, g.find("hand_l")->id(), glm::inverse(world("lowerarm_l")) * handWant);
    CHECK(turned(world("lowerarm_twist_01_l"), lower0) == doctest::Approx(30.0).epsilon(1e-3));

    // The leg's reverse foot poses the leg: lifting it lifts the foot joint with it.
    const glm::dvec3 ankle0(world("foot_l")[3]);
    run("ikPose", {{"args", {"foot_l"}}, {"moveBy", json::array({0, 10, 0})}});
    CHECK(glm::length(glm::dvec3(world("foot_l")[3]) - (ankle0 + glm::dvec3(0, 10, 0))) < 1e-6);
    CHECK(g.find("calf_l")->as<double>(Joint::kIkBlend) == doctest::Approx(0.0));
}

TEST_CASE("full-body IK poses Manny, with and without the auto-poser")
{
    if (!std::filesystem::exists(LILA_TEST_MANNY_FBX))
        return;
    auto pos = [](const char* n) { return glm::dvec3(world(n)[3]); };
    auto pose = [&](bool autoPose) {
        run("newScene");
        run("importFbx", {{"args", {LILA_TEST_MANNY_FBX}}, {"animation", false}});
        const glm::dvec3 foot = pos("foot_l"), ball = pos("ball_r");
        // Reach down to the floor in front: a person bends at the hips and knees to get there.
        const glm::dvec3 target = (foot + ball) * 0.5 + glm::dvec3(0.0, 10.0, 0.0);
        const json r = run("fullBodyIk", {{"pins", {{{"joint", "hand_l"}, {"position", {target.x, target.y, target.z}}}}},
                                          {"hold", {"foot_l", "foot_r"}},
                                          {"autoPose", autoPose}});
        CHECK(r["error"].get<double>() < 0.05);
        CHECK(glm::length(pos("hand_l") - target) < 0.05);
        CHECK(glm::length(pos("foot_l") - foot) < 0.05);
        // The root stays where it is; the pelvis is what moves.
        CHECK(glm::length(pos("root")) < 1e-6);
        return pos("pelvis");
    };
    const glm::dvec3 plain = pose(false), predicted = pose(true);
    // Reaching that low, a person crouches; plain IK mostly bends the spine.
    if (autopose::shipped())
        CHECK(predicted.y < plain.y - 5.0);
}

TEST_CASE("Manny's template says how stiff its spine is for full-body posing, and bad entries are refused")
{
    ensureLoaded();
    json t = run("rigTemplateInfo", {{"args", {"UE5_Manny"}}});
    CHECK(t["rig"]["fullBody"]["stiffness"]["spine_01"].get<double>() == doctest::Approx(0.4));
    CHECK(t["rig"]["fullBody"]["stiffness"]["clavicle_l"].get<double>() == doctest::Approx(0.7));

    t["id"] = "BadStiffness";
    t["rig"]["fullBody"]["stiffness"]["no_such_marker"] = 0.5;
    const auto file = std::filesystem::temp_directory_path() / "lila_bad_stiffness.lilarig";
    std::ofstream(file) << t.dump();
    CHECK_THROWS(run("loadRigTemplate", {{"args", {file.generic_string()}}}));
}
