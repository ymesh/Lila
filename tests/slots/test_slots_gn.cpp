#include <doctest/doctest.h>

#include "gn/Gn.h"
#include "gn/InstanceView.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "slots/Slots.h"
#include "slots/SlotsGn.h"

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>

using namespace lila;
using json = nlohmann::json;

namespace
{
json gnRun(const char* name, json call = json::object()) { return ops::execute(name, call); }
json gnArgs(std::initializer_list<json> a) { return {{"args", json(a)}}; }
std::string gnAdd(const std::string& g, const char* type) { return gnRun("gnAddNode", gnArgs({g, type})).get<std::string>(); }
void gnLink(const std::string& g, const std::string& from, const std::string& to) { gnRun("gnConnect", gnArgs({g, from, to})); }
void gnSet(const std::string& g, const std::string& plug, json v) { gnRun("gnSetValue", gnArgs({g, plug, v})); }

/** Evaluates until the slots' deferred visuals are built (the hook re-runs the modifier once). */
gn::InstanceViewPtr settle(const std::string& mesh)
{
    scene::Graph& g = scene::Graph::current();
    for (int k = 0; k < 3; ++k)
        g.evaluate();
    return gn::instanceViewFor(g, g.find(mesh)->id());
}

size_t instanceCount(const gn::InstanceViewPtr& v)
{
    size_t n = 0;
    if (v)
        for (const auto& p : v->parts)
            n += p.transforms->size();
    return n;
}

struct Scatter
{
    std::string mesh, cube, slotsNode, instancer;
};

Scatter scatter()
{
    gnRun("newScene");
    slots::refresh();
    Scatter s;
    s.mesh = gnRun("polyPlane", {{"width", 100.0}, {"depth", 100.0}, {"name", "Field"}})[0];
    gnRun("gnCreateGroup", gnArgs({"Props"}));
    const std::string grid = gnAdd("Props", "MeshGrid"), dist = gnAdd("Props", "DistributePointsOnFaces"), iop = gnAdd("Props", "InstanceOnPoints");
    s.cube = gnAdd("Props", "MeshCube");
    s.slotsNode = gnAdd("Props", "Slots");
    s.instancer = iop;
    gnSet("Props", grid + ".sizeX", 400);
    gnSet("Props", grid + ".sizeY", 400);
    gnSet("Props", dist + ".density", 2);
    gnSet("Props", dist + ".seed", 5);
    gnSet("Props", s.slotsNode + ".type", "container.20ft");
    gnSet("Props", s.slotsNode + ".owner", "yard:Port");
    gnLink("Props", grid + ".mesh", dist + ".mesh");
    gnLink("Props", dist + ".points", iop + ".points");
    gnLink("Props", s.cube + ".mesh", iop + ".instance");
    gnLink("Props", iop + ".instances", s.slotsNode + ".instances");
    gnLink("Props", s.slotsNode + ".instances", "GroupOutput.geometry");
    gnRun("gnApply", gnArgs({s.mesh, "Props"}));
    return s;
}

std::vector<std::string> slotIds()
{
    std::vector<std::string> out;
    for (const slots::Slot& s : slots::collect(scene::Graph::current()))
        out.push_back(s.id);
    std::sort(out.begin(), out.end());
    return out;
}
} // namespace

TEST_CASE("the Slots node publishes stable ids and an unrelated edit keeps them")
{
    const Scatter s = scatter();
    const gn::InstanceViewPtr v = settle(s.mesh);
    const size_t n = instanceCount(v);
    REQUIRE(n > 10);
    const std::vector<std::string> before = slotIds();
    CHECK(before.size() == n);
    CHECK(before.front().starts_with("yard:Port/i"));
    for (const slots::Slot& sl : slots::collect(scene::Graph::current()))
        CHECK(sl.type == "container.20ft");

    // The cube's size is nothing the slots depend on.
    gnSet("Props", s.cube + ".size", json::array({30, 30, 30}));
    settle(s.mesh);
    CHECK(slotIds() == before);
}

TEST_CASE("rules swap and hide geometry-node instances")
{
    const Scatter s = scatter();
    const size_t n = instanceCount(settle(s.mesh));
    REQUIRE(n > 10);
    gnRun("polyCylinder", {{"name", "Crate"}});
    gnRun("slotRule", {{"args", {"add"}}, {"type", "container"}, {"mesh", "Crate"}});
    gn::InstanceViewPtr v = settle(s.mesh);
    REQUIRE(v);
    REQUIRE(v->parts.size() == 1);
    CHECK(v->parts[0].mesh->numFaces() != 6);
    CHECK(instanceCount(v) == n);

    const std::string first = slotIds().front();
    gnRun("slotSet", {{"id", first}, {"hidden", true}});
    CHECK(instanceCount(settle(s.mesh)) == n - 1);
    gnRun("undo");
    CHECK(instanceCount(settle(s.mesh)) == n);
    gnRun("undo");
    v = settle(s.mesh);
    REQUIRE(v->parts.size() == 1);
    CHECK(v->parts[0].mesh->numFaces() == 6);
}

TEST_CASE("int attributes make the path when given")
{
    const Scatter s = scatter();
    gnSet("Props", s.slotsNode + ".pathAttributes", "missing");
    settle(s.mesh);
    for (const std::string& id : slotIds())
        CHECK(id.starts_with("yard:Port/missing?"));
    const json rep = gnRun("slotReport", {{"validate", false}});
    CHECK_FALSE(rep["duplicates"].empty());
}

TEST_CASE("instance colours: a stored colour, a rule's data colour and an override's reach the instanced draw")
{
    const Scatter s = scatter();
    gn::InstanceViewPtr v = settle(s.mesh);
    const size_t n = instanceCount(v);
    REQUIRE(n > 10);
    REQUIRE(v->parts.size() == 1);
    CHECK_FALSE(v->parts[0].colors);

    // A colour the generator stores on its instances.
    const std::string store = gnAdd("Props", "StoreNamedAttribute");
    gnSet("Props", store + ".dataType", "color");
    gnSet("Props", store + ".domain", "instance");
    gnSet("Props", store + ".name", "color");
    gnSet("Props", store + ".value", json::array({0.0, 1.0, 0.0, 1.0}));
    gnLink("Props", s.instancer + ".instances", store + ".geometry");
    gnLink("Props", store + ".geometry", s.slotsNode + ".instances");
    v = settle(s.mesh);
    REQUIRE(v->parts.size() == 1);
    REQUIRE(v->parts[0].colors);
    REQUIRE(v->parts[0].colors->size() == n);
    for (const glm::vec4& c : *v->parts[0].colors)
        CHECK(c == glm::vec4(0, 1, 0, 1));

    // A rule swapping in another mesh shows that mesh's own colour: the stored one was for the generator's cube.
    gnRun("polyCylinder", {{"name", "Crate"}});
    gnRun("slotRule", {{"args", {"add"}}, {"type", "container"}, {"mesh", "Crate"}});
    v = settle(s.mesh);
    REQUIRE(v->parts.size() == 1);
    CHECK_FALSE(v->parts[0].colors);

    // A rule's data colour (sRGB hex) wins over it, and an override's over the rule's.
    gnRun("slotRule", {{"args", {"add"}}, {"type", "container"}, {"data", {{"color", "#ff0000"}}}});
    v = settle(s.mesh);
    REQUIRE(v->parts[0].colors);
    for (const glm::vec4& c : *v->parts[0].colors)
        CHECK(c == glm::vec4(1, 0, 0, 1));
    const std::string first = slotIds().front();
    gnRun("slotSet", {{"id", first}, {"data", {{"color", json::array({0.0, 0.0, 1.0, 0.5})}}}});
    v = settle(s.mesh);
    int blue = 0, red = 0, other = 0;
    for (const glm::vec4& c : *v->parts[0].colors)
    {
        if (c == glm::vec4(0, 0, 1, 0.5f))
            ++blue;
        else if (c == glm::vec4(1, 0, 0, 1))
            ++red;
        else
            ++other;
    }
    CHECK(blue == 1);
    CHECK(red == int(n) - 1);
    CHECK(other == 0);

    // A second tone from data "color2" rides beside the first.
    CHECK_FALSE(v->parts[0].colors2);
    gnRun("slotRule", {{"args", {"add"}}, {"type", "container"}, {"data", {{"color2", json::array({0.0, 1.0, 0.0})}}}});
    v = settle(s.mesh);
    REQUIRE(v->parts[0].colors2);
    REQUIRE(v->parts[0].colors2->size() == n);
    for (const glm::vec4& c : *v->parts[0].colors2)
        CHECK(c == glm::vec4(0, 1, 0, 1));
    CHECK(slots::dataColor({{"color2", "#ffffff"}}, "color2")->g == doctest::Approx(1.0f));

    CHECK(slots::dataColor({{"color", "#808080"}})->r == doctest::Approx(0.2158f).epsilon(0.01));
    CHECK_FALSE(slots::dataColor({{"color", "red"}}));
    CHECK_FALSE(slots::dataColor({{"livery", "x"}}));
}

TEST_CASE("slots on a generator's own instances: a cell of 400 is quick, and an override reruns only its publisher")
{
    gnRun("newScene");
    slots::refresh();
    // A building cell's worth: 400 instances on 8 shared references, slots with regions, tags and a little data.
    auto inst = std::make_shared<gn::Instances>();
    for (int r = 0; r < 8; ++r)
        inst->references.push_back(gn::meshGeometry(geo::Mesh()));
    std::vector<slots::Slot> made;
    for (int k = 0; k < 400; ++k)
    {
        inst->add(glm::translate(glm::dmat4(1.0), glm::dvec3(k * 10.0, 0.0, 0.0)), k % 8);
        slots::Slot s;
        s.id = slots::makeId("building:00ff", "t0/f" + std::to_string(k % 4) + "/l" + std::to_string(k % 3) + "/b" + std::to_string(k) + "/window");
        s.type = k % 5 == 0 ? "door" : "window";
        s.owner = "building:00ff";
        s.regions = {"settlement:Puerto", "district:centro", "style:colonial", "cell:3_4"};
        s.tags = {"building", "shop"};
        if (k % 20 == 0)
            s.payload.data = {{"text", "Abarrotes Lupita"}};
        made.push_back(std::move(s));
    }
    gn::Geometry g;
    g.instances = inst;
    const gn::GeometryPtr in = gn::makeGeometry(std::move(g));
    const auto t0 = std::chrono::steady_clock::now();
    size_t rows = 0;
    for (int run = 0; run < 50; ++run)
    {
        gn::GeometryPtr out = slots::applySlots(in, scene::NodeId(0), made);
        rows += out->instances->size();
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 50.0;
    MESSAGE("applySlots, 400 slots: " << ms << " ms");
    CHECK(rows == 50 * 400);
    CHECK(ms < 4.0);

    // Two publishers: an override on a slot of one reruns that one only.
    const Scatter a = scatter();
    settle(a.mesh);
    scene::Graph& graph = scene::Graph::current();
    gnRun("polyPlane", {{"width", 100.0}, {"depth", 100.0}, {"name", "Field2"}});
    gnRun("gnCreateGroup", gnArgs({"Props2"}));
    {
        const std::string grid = gnAdd("Props2", "MeshGrid"), iop = gnAdd("Props2", "InstanceOnPoints"), cube = gnAdd("Props2", "MeshCube"),
                          sl = gnAdd("Props2", "Slots");
        gnSet("Props2", sl + ".type", "container.20ft");
        gnSet("Props2", sl + ".owner", "yard:Other");
        gnLink("Props2", grid + ".mesh", iop + ".points");
        gnLink("Props2", cube + ".mesh", iop + ".instance");
        gnLink("Props2", iop + ".instances", sl + ".instances");
        gnLink("Props2", sl + ".instances", "GroupOutput.geometry");
    }
    gnRun("gnApply", gnArgs({"Field2", "Props2"}));
    settle("Field2");
    const auto pass = [&](const std::string& mesh) { return graph.find(mesh)->evaluatedPass(); };
    const auto p1 = pass(a.mesh), p2 = pass("Field2");
    std::string onFirst;
    for (const slots::Slot& s : slots::collect(graph))
        if (s.owner == "yard:Port")
            onFirst = s.id;
    REQUIRE_FALSE(onFirst.empty());
    gnRun("slotSet", {{"id", onFirst}, {"hidden", true}});
    settle(a.mesh);
    settle("Field2");
    CHECK(pass(a.mesh) != p1);
    CHECK(pass("Field2") == p2);
}
