#include "geo/Mesh.h"
#include "deform/SoftNet.h"
#include "layout/SoftNet.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <doctest/doctest.h>

using namespace lila;

namespace softnettest
{
json run(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }

scene::MeshPtr outMesh(const std::string& path)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(path);
    REQUIRE(n);
    return n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}

/** The point of m nearest p (the plane's grid has one at every multiple of 2.5). */
glm::dvec3 near(const geo::Mesh& m, const glm::dvec3& p)
{
    glm::dvec3 best(0.0);
    double d = 1e300;
    for (geo::Index i = 0; i < m.pointCapacity(); ++i)
        if (m.pointValid(i) && glm::length(m.position(i) - p) < d)
        {
            d = glm::length(m.position(i) - p);
            best = m.position(i);
        }
    return best;
}

/** How far the point that sat at p moved between a and b. */
glm::dvec3 moved(const geo::Mesh& a, const geo::Mesh& b, const glm::dvec3& p)
{
    for (geo::Index i = 0; i < a.pointCapacity(); ++i)
        if (a.pointValid(i) && glm::length(a.position(i) - p) < 1e-6)
            return b.position(i) - a.position(i);
    FAIL("no point at the spot");
    return {};
}

json line(double x0, double x1, int n = 120)
{
    json pts = json::array();
    for (int i = 0; i <= n; ++i)
        pts.push_back({x0 + (x1 - x0) * double(i) / n, 0.0, 0.0});
    return pts;
}
} // namespace softnettest

using namespace softnettest;

TEST_CASE("softNet: a knot moved along the surface normal carries its line and the surface, fading with distance, without cutting")
{
    scene::Graph::current().clear();
    UndoStack::get().clear();
    const std::string mesh = softnettest::run("polyPlane", {{"name", "Sheet"}, {"width", 100.0}, {"depth", 100.0}, {"subdivisionsX", 40}, {"subdivisionsZ", 40}})[0];
    const std::string layout = softnettest::run("panelLayout", {{"args", {mesh}}, {"mirror", "off"}})[0];
    softnettest::run("panelLayoutLineSet", {{"args", {layout}}, {"set", "Major"}, {"output", "none"}});
    softnettest::run("panelLayoutStroke", {{"args", {layout}}, {"points", line(-30, 30)}, {"tolerance", 1.0}});
    const scene::MeshPtr before = softnettest::outMesh(mesh);
    REQUIRE(before);

    const json net = softnettest::run("softNet", {{"args", {mesh}}, {"reach", 12.0}});
    REQUIRE(net["knots"].size() == 2);
    CHECK(net["samples"].get<int>() > 10);
    // At rest the net changes nothing.
    scene::MeshPtr rest = softnettest::outMesh(mesh);
    CHECK(rest->numPoints() == before->numPoints());
    for (geo::Index i = 0; i < before->pointCapacity(); ++i)
        if (before->pointValid(i))
            CHECK(glm::length(rest->position(i) - before->position(i)) < 1e-9);

    // The knot at x = 30 lifts 5 along its frame's z, the surface normal.
    scene::Graph& g = scene::Graph::current();
    std::string knot;
    for (const json& k : net["knots"])
    {
        scene::Node* n = g.find(k.get<std::string>());
        REQUIRE(n);
        g.evaluate();
        if (glm::dvec3(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]).x > 0.0)
            knot = k.get<std::string>();
    }
    REQUIRE(!knot.empty());
    softnettest::run("setAttr", {{"args", {knot + ".translate", 0.0, 0.0, 5.0}}});
    const scene::MeshPtr bent = softnettest::outMesh(mesh);
    CHECK(bent->numPoints() == before->numPoints());
    CHECK(bent->numFaces() == before->numFaces());
    const glm::dvec3 atKnot = moved(*before, *bent, near(*before, {30, 0, 0}));
    const glm::dvec3 middle = moved(*before, *bent, near(*before, {0, 0, 0}));
    const glm::dvec3 otherEnd = moved(*before, *bent, near(*before, {-30, 0, 0}));
    const glm::dvec3 far = moved(*before, *bent, near(*before, {30, 0, 45}));
    MESSAGE("knot ", atKnot.y, " middle ", middle.y, " other end ", otherEnd.y, " far ", far.y);
    CHECK(std::abs(std::abs(atKnot.y) - 5.0) < 0.5);
    CHECK(std::abs(std::abs(middle.y) - 2.5) < 0.5);
    CHECK(std::abs(otherEnd.y) < 0.3);
    CHECK(std::abs(far.y) < 0.15 * std::abs(atKnot.y));
    // Back at rest, the surface is back where it was.
    softnettest::run("setAttr", {{"args", {knot + ".translate", 0.0, 0.0, 0.0}}});
    rest = softnettest::outMesh(mesh);
    for (geo::Index i = 0; i < before->pointCapacity(); ++i)
        if (before->pointValid(i))
            CHECK(glm::length(rest->position(i) - before->position(i)) < 1e-9);
}

namespace softnettest
{
/** A sheet with a Panel Layout (mirror as given) and the strokes drawn on it. */
std::string sheet(const char* name, const char* mirror, const std::vector<json>& strokes, int subdivisions = 40)
{
    const std::string mesh = softnettest::run("polyPlane", {{"name", name}, {"width", 100.0}, {"depth", 100.0}, {"subdivisionsX", subdivisions}, {"subdivisionsZ", subdivisions}})[0];
    const std::string layout = softnettest::run("panelLayout", {{"args", {mesh}}, {"mirror", mirror}})[0];
    softnettest::run("panelLayoutLineSet", {{"args", {layout}}, {"set", "Major"}, {"output", "none"}});
    for (const json& s : strokes)
        softnettest::run("panelLayoutStroke", {{"args", {layout}}, {"points", s}, {"tolerance", 1.0}});
    return mesh;
}

json segment(glm::dvec3 a, glm::dvec3 b, int n = 60)
{
    json pts = json::array();
    for (int i = 0; i <= n; ++i)
    {
        const glm::dvec3 p = a + (b - a) * (double(i) / n);
        pts.push_back({p.x, p.y, p.z});
    }
    return pts;
}

std::string knotNear(const json& knots, const glm::dvec3& p)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    std::string best;
    double d = 1e300;
    for (const json& k : knots)
    {
        scene::Node* n = g.find(k.get<std::string>());
        REQUIRE(n);
        const glm::dvec3 at = glm::dvec3(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
        if (glm::length(at - p) < d)
        {
            d = glm::length(at - p);
            best = k.get<std::string>();
        }
    }
    return best;
}

void sameAs(const geo::Mesh& a, const geo::Mesh& b)
{
    for (geo::Index i = 0; i < a.pointCapacity(); ++i)
        if (a.pointValid(i))
            CHECK(a.position(i) == b.position(i));
}

bool visible(const std::string& node)
{
    scene::Graph& g = scene::Graph::current();
    return std::get<bool>(g.value({g.find(node)->id(), scene::Transform::kVisibility, -1}));
}
} // namespace softnettest

TEST_CASE("softNet frames: junctions turn with their lines about the normal, other knots take the junctions' turn")
{
    namespace sn = deform::softnet;
    // A star of four triangles fanned round point 0, and a lone triangle far off; all on z = 0.
    std::vector<glm::dvec3> pts = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {-1, 0, 0}, {0, -1, 0}, {5, 0, 0}, {6, 0, 0}, {5, 1, 0}};
    auto pt = [](uint32_t a, uint32_t b, uint32_t c, glm::dvec3 w) { return sn::SurfacePoint{{a, b, c}, w}; };
    sn::KnotRig junction;
    junction.at = pt(0, 1, 2, {1, 0, 0});
    junction.along = {pt(0, 1, 2, {0, 1, 0}), pt(0, 2, 3, {0, 1, 0}), pt(0, 3, 4, {0, 0, 1})};
    junction.degree = 3;
    sn::KnotRig end;
    end.at = pt(5, 6, 7, {1, 0, 0});
    end.along = {pt(5, 6, 7, {0, 1, 0})};
    end.degree = 1;
    end.blend = {{0, 1.0}};
    for (sn::KnotRig* k : {&junction, &end})
    {
        k->rest = sn::restFrame(*k, pts);
        for (const sn::SurfacePoint& p : k->along)
        {
            const glm::dvec3 v = sn::at(p, pts) - sn::at(k->at, pts);
            k->restDirections.push_back(glm::transpose(k->rest) * glm::normalize(v - k->rest[2] * glm::dot(v, k->rest[2])));
        }
    }
    const std::vector<sn::KnotRig> rigs = {junction, end};
    const std::vector<glm::dmat4> rest = sn::knotFrames(rigs, pts);
    CHECK(glm::length(glm::dvec3(rest[0][2]) - glm::dvec3(0, 0, 1)) < 1e-12);
    CHECK(glm::length(glm::dvec3(rest[0][0]) - glm::dvec3(1, 0, 0)) < 1e-12);

    // The star turned 30 degrees about the normal: the junction's x turns 30, and the far line end, its own line unturned,
    // takes the junction's turn.
    const double a = glm::radians(30.0);
    std::vector<glm::dvec3> turned = pts;
    for (size_t i = 1; i <= 4; ++i)
        turned[i] = {pts[i].x * std::cos(a) - pts[i].y * std::sin(a), pts[i].x * std::sin(a) + pts[i].y * std::cos(a), 0.0};
    const std::vector<glm::dmat4> f = sn::knotFrames(rigs, turned);
    CHECK(std::abs(std::atan2(f[0][0].y, f[0][0].x) - a) < 1e-9);
    CHECK(std::abs(std::atan2(f[1][0].y, f[1][0].x) - a) < 1e-9);
    // The surface under the end knot tilted: z stays on its normal, x across it.
    std::vector<glm::dvec3> tilted = pts;
    tilted[7] = {5, 1, 1};
    const std::vector<glm::dmat4> t = sn::knotFrames(rigs, tilted);
    CHECK(glm::length(glm::dvec3(t[1][2]) - glm::normalize(glm::dvec3(0, -1, 1))) < 1e-9);
    CHECK(std::abs(glm::dot(glm::dvec3(t[1][0]), glm::dvec3(t[1][2]))) < 1e-12);
}

TEST_CASE("softNet: tangent handles bend a line between its knots; every control at rest leaves the mesh bitwise unchanged")
{
    scene::Graph::current().clear();
    UndoStack::get().clear();
    const std::string mesh = softnettest::sheet("Sheet", "off", {softnettest::line(-30, 30)});
    const scene::MeshPtr before = softnettest::outMesh(mesh);
    const json net = softnettest::run("softNet", {{"args", {mesh}}, {"reach", 12.0}});
    REQUIRE(net["tangents"].get<int>() == 2);
    const std::string a = softnettest::knotNear(net["knots"], {-30, 0, 0});
    std::string handle;
    for (const json& k : softnettest::run("softNetKnots", {{"args", {a}}}))
        if (k["knot"].get<std::string>() == a)
            handle = k["tangents"][0].get<std::string>();
    REQUIRE(!handle.empty());
    // The handle at the -30 end lifted 6 along the normal: the line rises near that end, not at the knots.
    softnettest::run("setAttr", {{"args", {handle + ".translate", 0.0, 0.0, 6.0}}});
    const scene::MeshPtr bent = softnettest::outMesh(mesh);
    const double quarter = softnettest::moved(*before, *bent, softnettest::near(*before, {-15, 0, 0})).y;
    const double mid = softnettest::moved(*before, *bent, softnettest::near(*before, {0, 0, 0})).y;
    const double atA = softnettest::moved(*before, *bent, softnettest::near(*before, {-30, 0, 0})).y;
    const double atB = softnettest::moved(*before, *bent, softnettest::near(*before, {30, 0, 0})).y;
    MESSAGE("handle: at a ", atA, " quarter ", quarter, " middle ", mid, " at b ", atB);
    CHECK(std::abs(quarter) > 1.5);
    CHECK(std::abs(quarter) > std::abs(mid));
    CHECK(std::abs(atA) < 0.4);
    CHECK(std::abs(atB) < 0.2);
    softnettest::run("setAttr", {{"args", {handle + ".translate", 0.0, 0.0, 0.0}}});
    softnettest::sameAs(*before, *softnettest::outMesh(mesh));
}

TEST_CASE("softNet: knots show at junctions and line ends; the rest show with showAll")
{
    scene::Graph::current().clear();
    UndoStack::get().clear();
    // A T (a junction and three ends) and an L drawn as one stroke (its corner a knot inside a line).
    json corner = softnettest::segment({-30, 0, -20}, {0, 0, -20});
    for (const json& p : softnettest::segment({0, 0, -20}, {0, 0, -40}))
        corner.push_back(p);
    const std::string mesh = softnettest::sheet("Sheet", "off", {softnettest::segment({-30, 0, 0}, {30, 0, 0}), softnettest::segment({0, 0, 0}, {0, 0, 30}), corner});
    const json net = softnettest::run("softNet", {{"args", {mesh}}, {"reach", 10.0}});
    const json knots = softnettest::run("softNetKnots", {{"args", {net["knotsGroup"]}}});
    int hidden = 0, shown = 0;
    for (const json& k : knots)
    {
        CHECK(softnettest::visible(k["knot"]) == k["exposed"].get<bool>());
        CHECK(k["exposed"].get<bool>() == (k["degree"].get<int>() != 2));
        (softnettest::visible(k["knot"]) ? shown : hidden)++;
    }
    MESSAGE("shown ", shown, " hidden ", hidden, " ", knots.dump());
    CHECK(shown >= 6);
    softnettest::run("softNetKnots", {{"args", {net["knotsGroup"]}}, {"showAll", true}});
    for (const json& k : knots)
        CHECK(softnettest::visible(k["knot"]));
}

TEST_CASE("softNet: knots follow the mesh's transform, and the shape it makes moves with the mesh")
{
    scene::Graph::current().clear();
    UndoStack::get().clear();
    const std::string mesh = softnettest::sheet("Sheet", "off", {softnettest::line(-30, 30)});
    const json net = softnettest::run("softNet", {{"args", {mesh}}, {"reach", 12.0}});
    const std::string knot = softnettest::knotNear(net["knots"], {30, 0, 0});
    softnettest::run("setAttr", {{"args", {knot + ".translate", 0.0, 0.0, 5.0}}});
    const scene::MeshPtr here = softnettest::outMesh(mesh);
    scene::Graph& g = scene::Graph::current();
    softnettest::run("setAttr", {{"args", {mesh + ".translate", 10.0, 20.0, 0.0}}});
    softnettest::run("setAttr", {{"args", {mesh + ".rotate", 0.0, 0.0, 90.0}}});
    g.evaluate();
    const glm::dmat4 world = g.find(mesh)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    const glm::dmat4 knotWorld = g.find(knot)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    // The knot sits at its lifted place carried by the mesh's transform: object (30, 5, 0).
    const glm::dvec3 expect = glm::dvec3(world * glm::dvec4(30.0, 5.0, 0.0, 1.0));
    MESSAGE("knot at ", knotWorld[3].x, " ", knotWorld[3].y, " ", knotWorld[3].z, " expected ", expect.x, " ", expect.y, " ", expect.z);
    CHECK(glm::length(glm::dvec3(knotWorld[3]) - expect) < 0.05);
    const scene::MeshPtr there = softnettest::outMesh(mesh);
    for (geo::Index i = 0; i < here->pointCapacity(); ++i)
        if (here->pointValid(i))
            CHECK(glm::length(here->position(i) - there->position(i)) < 1e-9);
}

TEST_CASE("softNetRebind keeps knot controls and their keys on the junctions that remain, and adds and removes the rest")
{
    scene::Graph::current().clear();
    UndoStack::get().clear();
    const std::string mesh = softnettest::sheet("Sheet", "off", {softnettest::line(-30, 30)});
    json net = softnettest::run("softNet", {{"args", {mesh}}, {"reach", 12.0}});
    const std::string knot = softnettest::knotNear(net["knots"], {30, 0, 0});
    softnettest::run("setAttr", {{"args", {knot + ".translate", 0.0, 0.0, 3.0}}});
    softnettest::run("setKeyframe", {{"args", {knot + ".translateZ"}}, {"time", 1.0}});
    scene::Graph& g = scene::Graph::current();
    auto keyed = [&] {
        const scene::NodeId id = g.find(knot)->id();
        return g.source({id, scene::Transform::kTranslate, 2}).has_value() || g.source({id, scene::Transform::kTranslate}).has_value();
    };
    REQUIRE(keyed());
    std::string layoutNode;
    for (const json& n : softnettest::run("ls", {{"type", "panelLayoutModifier"}}))
        layoutNode = n.get<std::string>();
    REQUIRE(!layoutNode.empty());
    softnettest::run("panelLayoutStroke", {{"args", {layoutNode}}, {"points", softnettest::segment({-10, 0, -30}, {-10, 0, 30})}, {"tolerance", 1.0}});
    net = softnettest::run("softNetRebind", {{"args", {mesh}}});
    MESSAGE("knots after the cross stroke: ", net["knots"].dump());
    CHECK(net["knots"].size() == 5);
    REQUIRE(g.find(knot));
    CHECK(keyed());
    CHECK(std::get<glm::dvec3>(g.value({g.find(knot)->id(), scene::Transform::kTranslate, -1})).z == doctest::Approx(3.0));
    // The cross stroke gone again: back to the two knots, the keyed one still there.
    UndoStack::get().undo();
    UndoStack::get().undo();
    net = softnettest::run("softNetRebind", {{"args", {mesh}}});
    CHECK(net["knots"].size() == 2);
    REQUIRE(g.find(knot));
    CHECK(keyed());
}

TEST_CASE("softNet mirrored pairs: a mirrored layout's knots pair as _l and _r, and posing with mirror moves both alike")
{
    scene::Graph::current().clear();
    UndoStack::get().clear();
    const std::string mesh = softnettest::sheet("Sheet", "x", {softnettest::segment({10, 0, -20}, {30, 0, 20})});
    const json net = softnettest::run("softNet", {{"args", {mesh}}, {"reach", 10.0}});
    REQUIRE(net["knots"].size() == 4);
    int paired = 0;
    for (const json& k : softnettest::run("softNetKnots", {{"args", {net["knotsGroup"]}}}))
        if (!k["partner"].is_null())
        {
            ++paired;
            const std::string n = k["knot"].get<std::string>();
            CHECK((n.ends_with("_l") || n.ends_with("_r")));
        }
    CHECK(paired == 4);
    const scene::MeshPtr before = softnettest::outMesh(mesh);
    const std::string left = softnettest::knotNear(net["knots"], {30, 0, 20});
    softnettest::run("softNetPose", {{"args", {left}}, {"translate", {1.0, 2.0, 4.0}}, {"mirror", true}});
    const scene::MeshPtr after = softnettest::outMesh(mesh);
    const glm::dvec3 l = softnettest::moved(*before, *after, softnettest::near(*before, {30, 0, 20}));
    const glm::dvec3 r = softnettest::moved(*before, *after, softnettest::near(*before, {-30, 0, 20}));
    MESSAGE("left ", l.x, " ", l.y, " ", l.z, " right ", r.x, " ", r.y, " ", r.z);
    CHECK(l.x == doctest::Approx(-r.x).epsilon(1e-6));
    CHECK(l.y == doctest::Approx(r.y).epsilon(1e-6));
    CHECK(l.z == doctest::Approx(r.z).epsilon(1e-6));
    CHECK(std::abs(l.y) > 2.0);
    // On the whole net, mirrorSoftNetKnots copies the + side over the - side.
    CHECK(softnettest::run("mirrorSoftNetKnots", {{"args", {net["knotsGroup"]}}}).size() == 2);
}

TEST_CASE("softNet multi-mesh: a garment over the body follows the same lines")
{
    scene::Graph::current().clear();
    UndoStack::get().clear();
    const std::string body = softnettest::sheet("Body", "off", {softnettest::line(-30, 30)});
    const std::string shirt = softnettest::run("polyPlane", {{"name", "Shirt"}, {"width", 80.0}, {"depth", 60.0}, {"subdivisionsX", 32}, {"subdivisionsZ", 24}})[0];
    softnettest::run("setAttr", {{"args", {shirt + ".translate", 0.0, 0.5, 0.0}}});
    // Something under the shirt's net to ride.
    softnettest::run("panelLayout", {{"args", {shirt}}, {"mirror", "off"}});
    const scene::MeshPtr shirtBefore = softnettest::outMesh(shirt);
    const scene::MeshPtr bodyBefore = softnettest::outMesh(body);
    const json net = softnettest::run("softNet", {{"args", {body}}, {"reach", 12.0}, {"meshes", {shirt}}});
    REQUIRE(net["meshes"].size() == 2);
    CHECK(net["meshes"][1]["samples"].get<int>() > 10);
    const std::string knot = softnettest::knotNear(net["knots"], {30, 0, 0});
    softnettest::run("setAttr", {{"args", {knot + ".translate", 0.0, 0.0, 5.0}}});
    const glm::dvec3 b = softnettest::moved(*bodyBefore, *softnettest::outMesh(body), softnettest::near(*bodyBefore, {0, 0, 0}));
    const glm::dvec3 s = softnettest::moved(*shirtBefore, *softnettest::outMesh(shirt), softnettest::near(*shirtBefore, {0, 0, 0}));
    MESSAGE("body ", b.y, " shirt ", s.y);
    CHECK(std::abs(b.y - s.y) < 0.15);
    CHECK(std::abs(s.y) > 2.0);
}

TEST_CASE("softNetLimbStations: rings round a limb and rails along it, crossing at junctions a net binds")
{
    scene::Graph::current().clear();
    UndoStack::get().clear();
    const std::string limb = softnettest::run("polyCylinder", {{"name", "Arm"}, {"radius", 5.0}, {"height", 40.0}, {"subdivisionsAxis", 32}, {"subdivisionsHeight", 40}})[0];
    const json rootMade = softnettest::run("joint", {{"position", {0.0, -18.0, 0.0}}, {"name", "upper"}, {"parent", ""}});
    const std::string root = rootMade.is_array() ? rootMade[0].get<std::string>() : rootMade.get<std::string>();
    softnettest::run("joint", {{"position", {0.0, 0.0, 0.0}}, {"name", "lower"}, {"parent", root}});
    softnettest::run("joint", {{"position", {0.0, 18.0, 0.0}}, {"name", "hand"}});
    const json made = softnettest::run("softNetLimbStations", {{"args", {limb}}, {"spine", root}, {"rings", 4}, {"rails", 4}});
    MESSAGE(made.dump());
    CHECK(made["rings"].get<int>() == 4);
    CHECK(made["rails"].get<int>() == 4);
    const json net = softnettest::run("softNet", {{"args", {limb}}, {"reach", 6.0}});
    int junctions = 0;
    for (const json& k : softnettest::run("softNetKnots", {{"args", {net["knotsGroup"]}}}))
        junctions += k["degree"].get<int>() >= 3 ? 1 : 0;
    MESSAGE("knots ", net["knots"].size(), " junctions ", junctions);
    CHECK(junctions >= 16);
}

TEST_CASE("Soft Net mouth (Mochi style): a loop with spokes gives one junction per spoke, frames on the surface, and mirrored pairs")
{
    namespace sn = deform::softnet;
    scene::Graph::current().clear();
    UndoStack::get().clear();
    const std::string head = softnettest::run("polySphere", {{"name", "Head"}, {"radius", 10.0}, {"subdivisionsAxis", 64}, {"subdivisionsHeight", 48}})[0];
    const std::string layout = softnettest::run("panelLayout", {{"args", {head}}, {"mirror", "x"}})[0];
    softnettest::run("panelLayoutLineSet", {{"args", {layout}}, {"set", "Major"}, {"output", "none"}});
    // The left eye: a loop on the +x side of the face, the mirror making the right one.
    auto onHead = [](glm::dvec3 d) { return glm::normalize(d) * 10.0; };
    json eye = json::array();
    for (int i = 0; i < 48; ++i)
    {
        const double a = 2.0 * 3.14159265358979 * i / 48;
        const glm::dvec3 p = onHead({0.35 + 0.16 * std::cos(a), 0.3 + 0.1 * std::sin(a), 1.0});
        eye.push_back({p.x, p.y, p.z});
    }
    const json spokes = softnettest::run("panelLayoutLoopSpokes", {{"args", {layout}}, {"points", eye}, {"count", 8}, {"length", 1.5}, {"cornerBias", 1.0}});
    CHECK(spokes["spokes"].get<int>() == 8);
    const json net = softnettest::run("softNet", {{"args", {head}}, {"reach", 2.0}});
    const json knots = softnettest::run("softNetKnots", {{"args", {net["knotsGroup"]}}});
    int junctions = 0, ends = 0, paired = 0;
    for (const json& k : knots)
    {
        junctions += k["degree"].get<int>() == 3 ? 1 : 0;
        ends += k["degree"].get<int>() == 1 ? 1 : 0;
        paired += k["partner"].is_null() ? 0 : 1;
    }
    MESSAGE("knots ", knots.size(), " junctions ", junctions, " ends ", ends, " paired ", paired);
    CHECK(junctions == 16);
    CHECK(ends == 16);
    CHECK(paired == int(knots.size()));
    // Every knot's frame: z on the sphere's normal, x across it.
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    for (const json& k : knots)
    {
        const glm::dmat4 w = g.find(k["knot"].get<std::string>())->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        const glm::dvec3 n = glm::normalize(glm::dvec3(w[3]));
        CHECK(glm::dot(glm::normalize(glm::dvec3(w[2])), n) > 0.99);
        CHECK(std::abs(glm::dot(glm::dvec3(w[0]), glm::dvec3(w[2]))) < 1e-9);
    }
    // Opening the left eye with mirror opens the right alike.
    const scene::MeshPtr before = softnettest::outMesh(head);
    std::string top;
    double best = -1e300;
    for (const json& k : knots)
    {
        const glm::dvec3 p = glm::dvec3(g.find(k["knot"].get<std::string>())->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
        if (p.x > 0 && k["degree"].get<int>() == 3 && p.y > best)
        {
            best = p.y;
            top = k["knot"].get<std::string>();
        }
    }
    REQUIRE(!top.empty());
    const glm::dvec3 topAt = glm::dvec3(g.find(top)->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
    softnettest::run("softNetPose", {{"args", {top}}, {"translate", {0.0, 0.0, 0.6}}, {"mirror", true}});
    const scene::MeshPtr after = softnettest::outMesh(head);
    const glm::dvec3 l = softnettest::moved(*before, *after, softnettest::near(*before, topAt));
    const glm::dvec3 r = softnettest::moved(*before, *after, softnettest::near(*before, {-topAt.x, topAt.y, topAt.z}));
    MESSAGE("left lid ", l.x, " ", l.y, " ", l.z, " right lid ", r.x, " ", r.y, " ", r.z);
    CHECK(glm::length(l) > 0.2);
    CHECK(glm::length(glm::dvec3(-l.x, l.y, l.z) - r) < 0.02 * glm::length(l) + 1e-3);
}

namespace softnettest
{
/** A skinned sheet (two joints) with an animated Soft Net over frames 0 to 20; returns the mesh. */
std::string animatedNet()
{
    scene::Graph::current().clear();
    UndoStack::get().clear();
    const std::string mesh = softnettest::run("polyPlane", {{"name", "Sheet"}, {"width", 100.0}, {"depth", 100.0}, {"subdivisionsX", 30}, {"subdivisionsZ", 30}})[0];
    const json r = softnettest::run("joint", {{"position", {-40.0, 0.0, 0.0}}, {"name", "root"}, {"parent", ""}});
    const std::string root = r.is_array() ? r[0].get<std::string>() : r.get<std::string>();
    const json t = softnettest::run("joint", {{"position", {0.0, 0.0, 0.0}}, {"name", "mid"}, {"parent", root}});
    const std::string mid = t.is_array() ? t[0].get<std::string>() : t.get<std::string>();
    softnettest::run("skinCluster", {{"args", {root, mid, mesh}}});
    const std::string layout = softnettest::run("panelLayout", {{"args", {mesh}}, {"mirror", "off"}})[0];
    softnettest::run("panelLayoutLineSet", {{"args", {layout}}, {"set", "Major"}, {"output", "none"}});
    softnettest::run("panelLayoutStroke", {{"args", {layout}}, {"points", softnettest::segment({-30, 0, -10}, {30, 0, -10})}, {"tolerance", 1.0}});
    softnettest::run("panelLayoutStroke", {{"args", {layout}}, {"points", softnettest::segment({10, 0, -30}, {10, 0, 30})}, {"tolerance", 1.0}});
    const json net = softnettest::run("softNet", {{"args", {mesh}}, {"reach", 10.0}});
    const json knots = softnettest::run("softNetKnots", {{"args", {net["knotsGroup"]}}});
    // The skin's mid joint bends a little; every knot and handle moves on its own rhythm.
    for (int f = 0; f <= 20; f += 2)
    {
        const double time = f / 30.0;
        softnettest::run("setKeyframe", {{"args", {mid + ".rotateY"}}, {"time", time}, {"value", 20.0 * std::sin(f * 0.2)}});
        int i = 0;
        for (const json& k : knots)
        {
            const std::string kn = k["knot"].get<std::string>();
            const double ph = f * 0.3 + i * 1.3;
            softnettest::run("setKeyframe", {{"args", {kn + ".translateZ"}}, {"time", time}, {"value", 4.0 * std::sin(ph)}});
            softnettest::run("setKeyframe", {{"args", {kn + ".translateX"}}, {"time", time}, {"value", 2.0 * std::cos(ph * 0.7)}});
            softnettest::run("setKeyframe", {{"args", {kn + ".rotateZ"}}, {"time", time}, {"value", 25.0 * std::sin(ph * 0.5)}});
            for (const json& h : k["tangents"])
                softnettest::run("setKeyframe", {{"args", {h.get<std::string>() + ".translateZ"}}, {"time", time}, {"value", 3.0 * std::sin(ph + 0.8)}});
            ++i;
        }
    }
    return mesh;
}
} // namespace softnettest

TEST_CASE("bakeSoftNet: knot joints as fixed bones, helpers and correctives beat a plain skin bake of the same net")
{
    std::string mesh = softnettest::animatedNet();
    const json plain = softnettest::run("bakeSkinWeights", {{"args", {mesh}}, {"source", "animation"}, {"start", 0}, {"end", 20}, {"helpers", 4}});
    MESSAGE("bakeSkinWeights helpers=4: rmse ", plain["rmse"].get<double>(), " max ", plain["maxError"].get<double>());

    mesh = softnettest::animatedNet();
    const json baked = softnettest::run("bakeSoftNet", {{"args", {mesh}}, {"start", 0}, {"end", 20}, {"helpers", 4}, {"correctives", 8}});
    MESSAGE("bakeSoftNet: joints ", baked["joints"].size(), " correctives ", baked["correctives"].size(), " skinned rmse ", baked["skinnedRmse"].get<double>(),
            " final rmse ", baked["rmse"].get<double>(), " max ", baked["maxError"].get<double>(), " unreal worst ", baked["unrealCheck"]["worst"].get<double>());
    CHECK(baked["rmse"].get<double>() < plain["rmse"].get<double>());
    CHECK(baked["rmse"].get<double>() < 0.065);
    CHECK(baked["unrealCheck"]["mismatches"].empty());
    CHECK(baked["frameError"].size() == 21);
}
