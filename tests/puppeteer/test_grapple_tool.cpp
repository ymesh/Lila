#include "TestUtil.h"

#include "scene/AnimLayer.h"
#include "scene/Grapple.h"
#include "ui/Actions.h"
#include "ui/ViewportTool.h"

#include <QMouseEvent>

#include <map>

using namespace puptest;

namespace grappletooltest
{
/** A UE-named stick biped under its own root joint; facing -1 turns it round. Returns full joint paths by name. */
std::map<std::string, std::string> biped(const std::string& rootName, glm::dvec3 offset, double facing)
{
    scene::Graph& g = graph();
    auto at = [&](double x, double y, double z) { return v3(offset.x + x * facing, offset.y + y, offset.z + z * facing); };
    std::map<std::string, std::string> path;
    run("joint", {{"position", at(0, 0, 0)}, {"name", rootName}, {"parent", ""}, {"skipSelect", true}});
    path["root"] = "|" + rootName;
    auto j = [&](const std::string& name, double x, double y, double z, const std::string& parent) {
        const std::string made = run("joint", {{"position", at(x, y, z)}, {"name", name}, {"parent", path.at(parent)}, {"skipSelect", true}});
        path[name] = path.at(parent) + "|" + made.substr(made.rfind('|') + 1);
        REQUIRE(g.find(path[name]));
    };
    j("pelvis", 0, 100, 0, "root");
    j("spine_01", 0, 112, 0, "pelvis");
    j("spine_02", 0, 126, 0, "spine_01");
    j("neck_01", 0, 146, 0, "spine_02");
    j("head", 0, 160, 0, "neck_01");
    for (const std::string side : {"l", "r"})
    {
        const double s = side == "l" ? 1.0 : -1.0;
        j("upperarm_" + side, 18 * s, 140, 0, "spine_02");
        j("lowerarm_" + side, 46 * s, 140, -2, "upperarm_" + side);
        j("hand_" + side, 72 * s, 140, 0, "lowerarm_" + side);
        j("thigh_" + side, 10 * s, 95, 0, "pelvis");
        j("calf_" + side, 10 * s, 52, 2, "thigh_" + side);
        j("foot_" + side, 10 * s, 8, 0, "calf_" + side);
    }
    return path;
}
} // namespace grappletooltest

using namespace grappletooltest;

TEST_CASE("Grapple tool: dragging from one character's hand onto another's forearm makes a contact and bakes it")
{
    loadPuppeteer();
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("playbackOptions", {{"minTime", 0.0}, {"maxTime", 1.0}, {"fps", 30.0}});
    const auto a = biped("a_root", {0, 0, 0}, 1.0);
    const auto b = biped("b_root", {0, 0, 80}, -1.0);

    // From above: A's left hand at z 0, B's right forearm at z about 82, nothing in front of either.
    ui::ViewInfo v = view();
    v.eye = {60.0, 450.0, 40.0};
    v.view = glm::lookAt(v.eye, glm::dvec3(60.0, 140.0, 40.0), glm::dvec3(0, 0, -1));
    ui::actions::run("puppeteer.grapple.tool");
    ui::ViewportTool* tool = ui::viewportTools::active();
    REQUIRE(tool);
    CHECK(std::string(tool->name()) == "Grapple");
    auto mouse = [&](QEvent::Type type, QPointF at) {
        QMouseEvent e(type, at, at, Qt::LeftButton, type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        if (type == QEvent::MouseButtonPress)
            return tool->mousePress(&e, v);
        if (type == QEvent::MouseMove)
            return tool->mouseMove(&e, v);
        return tool->mouseRelease(&e, v);
    };
    const glm::dvec3 hand = pos(a.at("hand_l"));
    const glm::dvec3 arm = glm::mix(pos(b.at("lowerarm_r")), pos(b.at("hand_r")), 0.3);
    const auto from = v.project(hand + glm::dvec3(3.0, 0.0, 0.0)), to = v.project(arm);
    REQUIRE((from && to));
    // A press on empty space is the viewport's, not the tool's.
    CHECK_FALSE(mouse(QEvent::MouseButtonPress, QPointF(5.0, 5.0)));
    REQUIRE(mouse(QEvent::MouseButtonPress, *from));
    for (int i = 1; i <= 10; ++i)
        mouse(QEvent::MouseMove, *from + (*to - *from) * (i / 10.0));
    CHECK(mouse(QEvent::MouseButtonRelease, *to));
    ui::viewportTools::set(nullptr);

    const json info = run("grappleInfo");
    REQUIRE(info["contacts"].size() == 1);
    const json c = info["contacts"][0];
    CHECK(c["source"].get<std::string>() == a.at("hand_l"));
    CHECK(c["target"].get<std::string>() == b.at("lowerarm_r"));
    CHECK(c["orient"].get<bool>());
    // Auto Bake: the grapple is on its layer, and the hand sits on the forearm where the contact is on.
    REQUIRE(info["layer"].is_string());
    graph().setTime(1.0);
    graph().evaluate();
    const auto [s, t] = scene::grapple::contactPoints(graph(), graph().find(c["contact"].get<std::string>())->id());
    CHECK(glm::length(s - t) < 0.5);
}
