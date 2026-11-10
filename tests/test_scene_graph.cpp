#include <doctest/doctest.h>

#include "io/SceneFile.h"
#include "ops/Command.h"
#include "rigmath/MathNodes.h"
#include "scene/Graph.h"

#include <algorithm>

using namespace lila;
using namespace lila::scene;

namespace sceneGraphTests
{
json sgCmd(const char* name, json call = json::object()) { return ops::execute(name, call); }

json viewOf(const char* view, json flags = json::object())
{
    flags["args"] = json::array({view});
    return sgCmd("sceneGraphInfo", flags);
}

std::vector<std::string> nodeNames(const json& info)
{
    std::vector<std::string> out;
    for (const json& n : info["nodes"])
        out.push_back(n["name"].get<std::string>());
    std::sort(out.begin(), out.end());
    return out;
}

const json* shownNode(const json& info, const std::string& name)
{
    for (const json& n : info["nodes"])
        if (n["name"] == name)
            return &n;
    return nullptr;
}

const json* defNode(const json& info, const std::string& name)
{
    for (const json& n : info["definition"]["nodes"])
        if (n["name"] == name)
            return &n;
    return nullptr;
}

bool hasLink(const json& info, const std::string& from, const std::string& fromSocket, const std::string& to, const std::string& toSocket)
{
    json want = json::array({from, fromSocket, to, toSocket});
    for (const json& l : info["definition"]["links"])
        if (l == want)
            return true;
    return false;
}

/** A -> B -> C -> D: A.translateX drives B.translateY, B.translate drives C.rotate, C.rotateX drives D.translateZ. */
void buildChain()
{
    sgCmd("newScene");
    for (const char* n : {"A", "B", "C", "D"})
        sgCmd("spaceLocator", {{"name", n}});
    sgCmd("connectAttr", {{"args", {"A.translateX", "B.translateY"}}});
    sgCmd("connectAttr", {{"args", {"B.translate", "C.rotate"}}});
    sgCmd("connectAttr", {{"args", {"C.rotateX", "D.translateZ"}}});
}

void selectOnly(const std::string& name) { sgCmd("select", {{"args", {name}}}); }

const rigmath::Overload& addFloat()
{
    rigmath::registerTypes();
    for (const rigmath::Function& f : rigmath::functions())
        if (f.name == "add")
            for (const rigmath::Overload& o : f.overloads)
                if (!o.inputs.empty() && o.inputs[0].kind == rigmath::Kind::Float)
                    return o;
    FAIL("no float add in the rig math library");
    return rigmath::functions().front().overloads.front();
}
} // namespace sceneGraphTests
using namespace sceneGraphTests;

TEST_CASE("sceneGraphInfo: Selection shows direct inputs and outputs, wired by plug and component")
{
    buildChain();
    selectOnly("B");
    json info = viewOf("Selection");
    CHECK(nodeNames(info) == std::vector<std::string>{"A", "B", "C"});
    CHECK(hasLink(info, "A", "translateX", "B", "translateY"));
    CHECK(hasLink(info, "B", "translate", "C", "rotate"));
    CHECK(info["definition"]["links"].size() == 2);
    const json* b = shownNode(info, "B");
    REQUIRE(b);
    CHECK((*b)["position"].is_array());
    bool tyLinked = false;
    for (const json& s : (*b)["inputs"])
        if (s["id"] == "translateY")
            tyLinked = s["linked"].get<bool>();
    CHECK(tyLinked);
    // Nodes show only their connected plugs until expanded.
    CHECK((*defNode(info, "B"))["hideUnused"] == true);
    // Auto layout puts each node right of what feeds it.
    CHECK((*shownNode(info, "A"))["position"][0].get<double>() < (*b)["position"][0].get<double>());
    CHECK((*b)["position"][0].get<double>() < (*shownNode(info, "C"))["position"][0].get<double>());

    sgCmd("select", {{"clear", true}});
    info = viewOf("Selection");
    CHECK(info["nodes"].empty());
    CHECK(info.contains("error"));
    CHECK_THROWS(viewOf("Sideways"));
}

TEST_CASE("sceneGraphInfo: Upstream and Downstream follow the selection to a depth")
{
    buildChain();
    selectOnly("D");
    CHECK(nodeNames(viewOf("Upstream")) == std::vector<std::string>{"B", "C", "D"});
    CHECK(nodeNames(viewOf("Upstream", {{"depth", 3}})) == std::vector<std::string>{"A", "B", "C", "D"});
    CHECK(nodeNames(viewOf("Upstream", {{"depth", 1}})) == std::vector<std::string>{"C", "D"});
    CHECK(nodeNames(viewOf("Downstream")) == std::vector<std::string>{"D"});

    selectOnly("A");
    json down = viewOf("Downstream");
    CHECK(nodeNames(down) == std::vector<std::string>{"A", "B", "C"});
    CHECK(hasLink(down, "B", "translate", "C", "rotate"));
    CHECK(nodeNames(viewOf("Downstream", {{"depth", 5}})) == std::vector<std::string>{"A", "B", "C", "D"});
}

TEST_CASE("sceneGraphInfo: History shows the whole modifier stack from the mesh or any modifier")
{
    sgCmd("newScene");
    sgCmd("polyCube");
    for (const char* kind : {"bevel", "array", "bevel"})
        sgCmd("addModifier", {{"args", {"Cube"}}, {"kind", kind}});
    const std::vector<std::string> stack{"ArrayModifier", "BevelModifier", "BevelModifier1", "Cube", "PolyCube"};
    selectOnly("Cube");
    CHECK(nodeNames(viewOf("History")) == stack);
    selectOnly("BevelModifier");
    CHECK(nodeNames(viewOf("History")) == stack);
}

TEST_CASE("sceneGraphInfo: anim curves are looked through unless all is on")
{
    buildChain();
    sgCmd("setKeyframe", {{"args", {"B.translateZ"}}, {"time", 0.0}, {"value", 1.0}});
    sgCmd("setKeyframe", {{"args", {"B.translateZ"}}, {"time", 1.0}, {"value", 2.0}});
    selectOnly("B");
    json info = viewOf("Selection");
    CHECK(info["nodes"].size() == 3);
    CHECK(viewOf("Selection", {{"all", true}})["nodes"].size() == 4);
}

TEST_CASE("sceneGraphInfo: Hierarchy shows parents and children; Children into Parent parents")
{
    buildChain();
    sgCmd("spaceLocator", {{"name", "P"}});
    sgCmd("spaceLocator", {{"name", "K"}});
    Graph& g = Graph::current();
    sgCmd("sceneGraphConnect", {{"args", {"Hierarchy", "P.@children", "K.@parent"}}});
    CHECK(g.parent(g.find("K")->id()) == g.find("P")->id());
    selectOnly("K");
    json info = viewOf("Hierarchy");
    CHECK(nodeNames(info) == std::vector<std::string>{"K", "P"});
    CHECK(hasLink(info, "P", "@children", "K", "@parent"));
    CHECK(info["nodes"][0]["name"] == "P"); // Outliner order: the parent first

    CHECK(sgCmd("sceneGraphDisconnect", {{"args", {"Hierarchy", "K.@parent"}}}) == 1);
    CHECK(g.parent(g.find("K")->id()) == kNoNode);
    sgCmd("undo");
    CHECK(g.parent(g.find("K")->id()) == g.find("P")->id());
    CHECK_THROWS(sgCmd("sceneGraphConnect", {{"args", {"Hierarchy", "P.translate", "K.@parent"}}}));
}

TEST_CASE("sceneGraph edits: add, connect, disconnect and set value, each undoable")
{
    buildChain();
    Graph& g = Graph::current();
    const rigmath::Overload& add = addFloat();
    const std::string in = add.inputs[0].name, out = add.outputs[0].name;
    selectOnly("B");

    std::string node = sgCmd("sceneGraphAddNode", {{"args", {"Selection", add.nodeType}}, {"position", {10, 20}}}).get<std::string>();
    REQUIRE(g.find(node));
    CHECK(g.isSelected(g.find(node)->id()));
    json info = viewOf("Selection");
    REQUIRE(shownNode(info, node));
    CHECK((*shownNode(info, node))["position"] == json::array({10.0, 20.0}));
    sgCmd("undo");
    CHECK_FALSE(g.find(node));

    node = sgCmd("sceneGraphAddNode", {{"args", {"Selection", add.nodeType}}}).get<std::string>();
    sgCmd("sceneGraphConnect", {{"args", {"Selection", "A.translateZ", node + "." + in}}});
    sgCmd("sceneGraphConnect", {{"args", {"Selection", node + "." + out, "B.translateX"}}});
    CHECK(g.plugName(*g.source(g.plug("B.translateX"))) == node + "." + out);
    info = viewOf("Selection");
    CHECK(hasLink(info, node, out, "B", "translateX"));
    CHECK(hasLink(info, "A", "translateZ", node, in));
    CHECK_THROWS(sgCmd("sceneGraphConnect", {{"args", {"Selection", "B.worldMatrix", "C.translateX"}}}));

    CHECK(sgCmd("sceneGraphDisconnect", {{"args", {"Selection", "B.translateX"}}, {"source", node + "." + out}}) == 1);
    CHECK_FALSE(g.source(g.plug("B.translateX")));
    sgCmd("undo");
    CHECK(g.source(g.plug("B.translateX")));

    sgCmd("sceneGraphSetValue", {{"args", {"Selection", "B.translateZ", 4.0}}});
    CHECK(sgCmd("getAttr", {{"args", {"B.translateZ"}}}).get<double>() == doctest::Approx(4.0));
    sgCmd("undo");
    CHECK(sgCmd("getAttr", {{"args", {"B.translateZ"}}}).get<double>() == doctest::Approx(0.0));

    sgCmd("sceneGraphDeleteNode", {{"args", {"Selection", node}}});
    CHECK_FALSE(g.find(node));
    sgCmd("undo");
    CHECK(g.find(node));
}

TEST_CASE("sceneGraph view state: positions per view and node flags undo and survive a save")
{
    buildChain();
    selectOnly("C");
    json before = (*shownNode(viewOf("Selection"), "C"))["position"];
    sgCmd("sceneGraphMoveNodes", {{"args", {"Upstream"}}, {"positions", {{"C", {300, 40}}}}});
    CHECK((*shownNode(viewOf("Upstream"), "C"))["position"] == json::array({300.0, 40.0}));
    CHECK((*shownNode(viewOf("Selection"), "C"))["position"] == before);

    sgCmd("sceneGraphSetNodeFlags", {{"args", {"Upstream", "C"}}, {"hideUnused", false}});
    CHECK((*defNode(viewOf("Upstream"), "C"))["hideUnused"] == false);
    sgCmd("undo");
    CHECK((*defNode(viewOf("Upstream"), "C"))["hideUnused"] == true);

    json doc = io::serialize();
    sgCmd("newScene");
    io::deserialize(doc);
    selectOnly("C");
    CHECK((*shownNode(viewOf("Upstream"), "C"))["position"] == json::array({300.0, 40.0}));

    sgCmd("sceneGraphMoveNodes", {{"args", {"Upstream"}}, {"positions", {{"C", {0, 0}}}}});
    sgCmd("undo");
    CHECK((*shownNode(viewOf("Upstream"), "C"))["position"] == json::array({300.0, 40.0}));
}

TEST_CASE("sceneGraphNodeTypes: rig math first; a wire's type filters and names the socket")
{
    addFloat();
    json all = sgCmd("sceneGraphNodeTypes");
    REQUIRE_FALSE(all.empty());
    CHECK(all[0]["category"].get<std::string>().starts_with("Math"));
    json vectors = sgCmd("sceneGraphNodeTypes", {{"linkType", "vector"}});
    REQUIRE_FALSE(vectors.empty());
    CHECK(vectors.size() < all.size());
    for (const json& t : vectors)
        CHECK_FALSE(t.value("socket", "").empty());
}
