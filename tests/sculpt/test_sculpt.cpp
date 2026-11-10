#include "ops/Command.h"
#include "ops/Undo.h"
#include "geo/Bvh.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "geo/Symmetry.h"
#include "retopo/Retopo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "sculpt/Reproject.h"
#include "sculpt/Sculpt.h"
#include "sculpt/SculptSession.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <map>
#include <set>

using namespace lila;
using geo::Index;

namespace
{
scene::Node* makeMeshNode(const std::string& name, geo::Mesh mesh)
{
    scene::Graph& g = scene::Graph::current();
    if (scene::Node* old = g.find(name))
        g.deleteNode(old->id());
    scene::Node* n = g.createNode(*scene::types::find("mesh"), name, scene::kNoNode, false);
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
    return n;
}

const geo::Mesh& inMesh(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    return *std::get<scene::MeshPtr>(g.storedValue({g.find(name)->id(), scene::MeshNode::kInMesh, -1}));
}

double maxDiff(const geo::Mesh& a, const geo::Mesh& b)
{
    double d = 0.0;
    for (Index p = 0; p < a.pointCapacity(); ++p)
        if (a.pointValid(p))
            d = std::max(d, glm::length(a.position(p) - b.position(p)));
    return d;
}

/** Largest gap between a point and the reflection of its partner across x = 0. */
double asymmetry(const sculpt::SculptMesh& s)
{
    const geo::SymmetryMap& map = s.symmetry();
    double worst = 0.0;
    for (Index p = 0; p < s.mesh().pointCapacity(); ++p)
    {
        Index q = map.mirrorPoint(p);
        if (q == geo::kInvalid)
            continue;
        worst = std::max(worst, glm::length(s.mesh().position(q) - map.plane().reflect(s.mesh().position(p))));
    }
    return worst;
}

geo::Mesh sphere(int subdiv, double radius = 50.0)
{
    geo::SphereOptions o;
    o.radius = radius;
    o.subdivAxis = subdiv;
    o.subdivHeight = subdiv;
    return geo::makeSphere(o);
}

sculpt::Dab dabAt(const sculpt::SculptMesh& s, glm::dvec3 from, double radius)
{
    // Aimed at the origin from outside, like a click on the surface.
    geo::BvhHit hit;
    glm::dvec3 dir = -glm::normalize(from);
    REQUIRE(s.raycast(from, dir, hit));
    sculpt::Dab d;
    d.center = hit.point;
    d.normal = s.normalAt(hit);
    d.radius = radius;
    d.viewDir = dir;
    return d;
}
} // namespace

TEST_CASE("subdivision stencils reproduce subdivide")
{
    geo::Mesh cube = geo::makeCube({});
    // A creased edge and an open border exercise the sharp rules.
    auto crease = cube.edgeAttrs().getOrAdd<float>(geo::attr::kCrease);
    crease[0] = 1.0f;
    crease[3] = 0.5f;
    geo::deleteFaces(cube, std::vector<Index>{0});
    for (bool sharp : {true, false})
    {
        geo::SubdivideOptions o;
        o.levels = 3;
        o.sharpBoundary = sharp;
        geo::Mesh sub;
        auto stencils = geo::subdivideStencils(cube, o, &sub);
        REQUIRE(stencils.size() == 3);
        std::vector<glm::dvec3> pts(cube.positions().begin(), cube.positions().end());
        for (const auto& s : stencils)
            pts = s.apply(pts);
        geo::Mesh direct = geo::subdivide(cube, o);
        REQUIRE(pts.size() == direct.pointCapacity());
        REQUIRE(sub.pointCapacity() == direct.pointCapacity());
        double err = 0.0;
        for (Index p = 0; p < direct.pointCapacity(); ++p)
            err = std::max(err, glm::length(pts[p] - direct.position(p)));
        CHECK(err < 1e-12);
    }
}

TEST_CASE("reprojection round trip leaves an untouched cage unchanged")
{
    geo::Mesh cube = geo::makeCube({});
    for (int levels : {1, 2, 3})
    {
        geo::SubdivideOptions o;
        o.levels = levels;
        geo::Mesh sub;
        auto stencils = geo::subdivideStencils(cube, o, &sub);
        std::vector<glm::dvec3> x = sculpt::fitCage(cube, stencils, sub.positions());
        double err = 0.0;
        for (Index p = 0; p < cube.pointCapacity(); ++p)
            err = std::max(err, glm::length(x[p] - cube.position(p)));
        CHECK(err < 1e-9);
    }

    // The same through the commands, with symmetry on, levels 2.
    scene::Node* n = makeMeshNode("rtCube", geo::makeCube({}));
    scene::Graph::current().setValue({n->id(), scene::MeshNode::kSymmetry, -1}, int64_t(1));
    geo::Mesh before = inMesh("rtCube");
    ops::execute("sculptEnter", {{"args", {"rtCube"}}, {"levels", 2}});
    REQUIRE(sculpt::active());
    CHECK(sculpt::active()->sculpt.mesh().numFaces() == 6 * 16);
    ops::execute("sculptLevel", {{"level", 0}});
    ops::execute("sculptExit", json::object());
    CHECK_FALSE(sculpt::active());
    CHECK(maxDiff(before, inMesh("rtCube")) < 1e-4);
}

TEST_CASE("exit fit recovers a displaced cage")
{
    // Sculpting exactly the subdivision of a moved cage must give that cage back.
    geo::Mesh cage = sphere(8);
    geo::Mesh moved = cage;
    for (Index p = 0; p < moved.pointCapacity(); ++p)
        moved.setPosition(p, moved.position(p) * glm::dvec3(1.2, 0.9, 1.0) + glm::dvec3(0.0, std::sin(double(p)) * 2.0, 0.0));
    geo::SubdivideOptions o;
    o.levels = 2;
    auto stencils = geo::subdivideStencils(cage, o);
    std::vector<glm::dvec3> target(moved.positions().begin(), moved.positions().end());
    for (const auto& s : stencils)
        target = s.apply(target);
    std::vector<glm::dvec3> x = sculpt::fitCage(cage, stencils, target);
    double err = 0.0;
    for (Index p = 0; p < cage.pointCapacity(); ++p)
        err = std::max(err, glm::length(x[p] - moved.position(p)));
    // The regularization's pull toward the starting cage is all that is left.
    CHECK(err < 1e-4);
}

TEST_CASE("relax brush keeps points on the original surface")
{
    geo::SubdivideOptions o;
    o.levels = 2;
    geo::Mesh proxy = geo::subdivide(sphere(10), o);
    // Uneven spacing to relax.
    for (Index p = 0; p < proxy.pointCapacity(); ++p)
    {
        glm::dvec3 at = proxy.position(p);
        proxy.setPosition(p, glm::normalize(at + glm::dvec3(std::sin(at.y * 0.3) * 4.0, 0.0, 0.0)) * glm::length(at));
    }
    geo::MeshBvh original(proxy);
    sculpt::SculptMesh s(proxy);
    sculpt::BrushSettings b;
    b.brush = sculpt::Brush::Relax;
    b.strength = 1.0;
    s.beginStroke(b);
    for (int i = 0; i < 20; ++i)
        s.dab(dabAt(s, glm::dvec3(200.0, 10.0 * std::sin(i * 0.3), 0.0), 30.0));
    sculpt::StrokeRecord r = s.endStroke();
    REQUIRE(r.points.size() > 50);
    double moved = 0.0, off = 0.0;
    for (size_t i = 0; i < r.points.size(); ++i)
    {
        moved = std::max(moved, glm::length(r.after[i] - r.before[i]));
        geo::BvhClosest c;
        REQUIRE(original.closestPoint(r.after[i], c));
        off = std::max(off, c.distance);
    }
    CHECK(moved > 0.1);
    CHECK(off < 1e-9);

    // The standalone command: relaxMesh keeps the form too.
    makeMeshNode("relaxed", proxy);
    ops::execute("relaxMesh", {{"args", {"relaxed"}}, {"iterations", 10}, {"pinSeams", true}});
    const geo::Mesh& out = inMesh("relaxed");
    double cmdOff = 0.0, cmdMoved = 0.0;
    for (Index p = 0; p < out.pointCapacity(); ++p)
    {
        geo::BvhClosest c;
        original.closestPoint(out.position(p), c);
        cmdOff = std::max(cmdOff, c.distance);
        cmdMoved = std::max(cmdMoved, glm::length(out.position(p) - proxy.position(p)));
    }
    CHECK(cmdMoved > 0.1);
    CHECK(cmdOff < 1e-6);
}

TEST_CASE("symmetric strokes stay symmetric")
{
    geo::SubdivideOptions o;
    o.levels = 2;
    geo::Mesh proxy = geo::subdivide(geo::makeCube({}), o);
    sculpt::SculptMesh s(proxy);
    s.setSymmetry(geo::axisPlane(0), 1e-4);
    REQUIRE(s.symmetric());
    CHECK(asymmetry(s) == 0.0);
    for (sculpt::Brush brush : {sculpt::Brush::Draw, sculpt::Brush::ClayStrips, sculpt::Brush::Inflate, sculpt::Brush::Smooth,
                                sculpt::Brush::Flatten, sculpt::Brush::Pinch, sculpt::Brush::Crease, sculpt::Brush::Scrape,
                                sculpt::Brush::Relax, sculpt::Brush::Grab, sculpt::Brush::DamStandard, sculpt::Brush::TrimDynamic,
                                sculpt::Brush::Polish, sculpt::Brush::Fold, sculpt::Brush::Fill, sculpt::Brush::Clay,
                                sculpt::Brush::Layer, sculpt::Brush::Blob, sculpt::Brush::Magnify, sculpt::Brush::Planar,
                                sculpt::Brush::SnakeHook, sculpt::Brush::ElasticGrab, sculpt::Brush::Nudge, sculpt::Brush::Thumb,
                                sculpt::Brush::Rotate, sculpt::Brush::Pose})
    {
        sculpt::BrushSettings b;
        b.brush = brush;
        b.strength = 0.8;
        s.beginStroke(b);
        // Off centre and crossing the plane.
        for (int i = 0; i < 12; ++i)
        {
            sculpt::Dab d = dabAt(s, glm::dvec3(-30.0 + i * 5.0, 20.0, 200.0), 25.0);
            d.delta = glm::dvec3(1.0, 0.5, 0.0);
            d.direction = glm::dvec3(1.0, 0.0, 0.0);
            s.dab(d);
        }
        sculpt::StrokeRecord r = s.endStroke();
        CHECK_MESSAGE(!r.empty(), sculpt::brushName(brush));
        CHECK_MESSAGE(asymmetry(s) == 0.0, sculpt::brushName(brush));
    }
}

TEST_CASE("a dab on the symmetry plane moves points once, not once per side")
{
    geo::SubdivideOptions o;
    o.levels = 3;
    const geo::Mesh proxy = geo::subdivide(geo::makeCube({}), o);
    for (sculpt::Brush brush : {sculpt::Brush::Smooth, sculpt::Brush::Draw, sculpt::Brush::Flatten})
    {
        sculpt::SculptMesh plain(proxy), mirrored(proxy);
        mirrored.setSymmetry(geo::axisPlane(0), 1e-4);
        REQUIRE(mirrored.symmetric());
        sculpt::BrushSettings b;
        b.brush = brush;
        b.strength = 1.0;
        for (sculpt::SculptMesh* s : {&plain, &mirrored})
        {
            s->beginStroke(b);
            for (int i = 0; i < 4; ++i)
                s->dab(dabAt(*s, glm::dvec3(0.0, 20.0, 200.0), 40.0));
            s->endStroke();
        }
        double off = 0.0, moved = 0.0;
        for (Index p = 0; p < proxy.pointCapacity(); ++p)
        {
            off = std::max(off, glm::length(plain.mesh().position(p) - mirrored.mesh().position(p)));
            moved = std::max(moved, glm::length(plain.mesh().position(p) - proxy.position(p)));
        }
        CHECK_MESSAGE(moved > 0.0, sculpt::brushName(brush));
        CHECK_MESSAGE(off < 1e-6 * std::max(moved, 1.0), sculpt::brushName(brush));
    }
}

namespace
{
/** A flat 100 cm square in XZ, facing +Y, 40 quads a side. */
geo::Mesh flatGrid()
{
    geo::PlaneOptions o;
    o.subdivX = o.subdivZ = 40;
    return geo::makePlane(o);
}

sculpt::Dab flatDab(glm::dvec3 at, double radius)
{
    sculpt::Dab d;
    d.center = at;
    d.normal = glm::dvec3(0.0, 1.0, 0.0);
    d.radius = radius;
    d.viewDir = glm::dvec3(0.0, -1.0, 0.0);
    return d;
}

double highest(const sculpt::SculptMesh& s)
{
    double y = -1e30;
    for (Index p = 0; p < s.mesh().pointCapacity(); ++p)
        y = std::max(y, s.mesh().position(p).y);
    return y;
}

sculpt::SculptMesh strokeOn(geo::Mesh mesh, sculpt::Brush brush, const std::vector<sculpt::Dab>& dabs, bool invert = false)
{
    sculpt::SculptMesh s(std::move(mesh));
    sculpt::BrushSettings b;
    b.brush = brush;
    b.strength = 1.0;
    b.invert = invert;
    s.beginStroke(b);
    for (const sculpt::Dab& d : dabs)
        s.dab(d);
    s.endStroke();
    return s;
}
} // namespace

TEST_CASE("Layer raises a plateau that repeated passes don't build past")
{
    std::vector<sculpt::Dab> once, many;
    for (int i = 0; i < 10; ++i)
        once.push_back(flatDab({-20.0 + i * 4.0, 0.0, 0.0}, 15.0));
    for (int pass = 0; pass < 5; ++pass)
        many.insert(many.end(), once.begin(), once.end());
    const double h1 = highest(strokeOn(flatGrid(), sculpt::Brush::Layer, once));
    const double h5 = highest(strokeOn(flatGrid(), sculpt::Brush::Layer, many));
    CHECK(h1 > 0.1);
    CHECK(h5 <= 15.0 * 0.08 * 1.02);
    CHECK(highest(strokeOn(flatGrid(), sculpt::Brush::Draw, many)) > h5);
}

TEST_CASE("Fill raises dips to the plane and leaves what stands above it")
{
    geo::Mesh dented = flatGrid();
    for (Index p = 0; p < dented.pointCapacity(); ++p)
    {
        glm::dvec3 q = dented.position(p);
        const double r = glm::length(glm::dvec2(q.x, q.z));
        q.y = r < 6.0 ? -2.0 : (r > 10.0 && r < 12.0 ? 1.0 : 0.0);
        dented.setPosition(p, q);
    }
    sculpt::SculptMesh s = strokeOn(dented, sculpt::Brush::Fill, std::vector<sculpt::Dab>(8, flatDab({0.0, 0.0, 0.0}, 16.0)));
    double deepest = 0.0, ridge = 0.0;
    for (Index p = 0; p < s.mesh().pointCapacity(); ++p)
    {
        const glm::dvec3 q = s.mesh().position(p);
        const double r = glm::length(glm::dvec2(q.x, q.z));
        if (r < 6.0)
            deepest = std::min(deepest, q.y);
        if (r > 10.5 && r < 11.5)
            ridge = std::max(ridge, q.y);
    }
    CHECK(deepest > -1.0);
    CHECK(ridge == doctest::Approx(1.0));
}

TEST_CASE("Planar holds the first dab's plane for the whole stroke")
{
    // A step up at x = 0; the stroke starts on the low side and runs onto the high one.
    geo::Mesh stepped = flatGrid();
    for (Index p = 0; p < stepped.pointCapacity(); ++p)
    {
        glm::dvec3 q = stepped.position(p);
        q.y = q.x > 0.0 ? 3.0 : 0.0;
        stepped.setPosition(p, q);
    }
    std::vector<sculpt::Dab> dabs;
    for (int i = 0; i < 30; ++i)
        dabs.push_back(flatDab({-20.0 + i * 1.5, 0.0, 0.0}, 8.0));
    auto heightAtEnd = [&](sculpt::Brush brush) {
        sculpt::SculptMesh s = strokeOn(stepped, brush, dabs);
        double y = -1e30;
        for (Index p = 0; p < stepped.pointCapacity(); ++p)
            if (glm::length(stepped.position(p) - glm::dvec3(22.5, 3.0, 0.0)) < 1e-6)
                y = std::max(y, s.mesh().position(p).y);
        return y;
    };
    // Flatten follows the high side's own plane; Planar presses it down toward where the stroke began.
    CHECK(heightAtEnd(sculpt::Brush::Flatten) == doctest::Approx(3.0).epsilon(0.01));
    CHECK(heightAtEnd(sculpt::Brush::Planar) < 1.5);
}

TEST_CASE("Rotate turns the patch rigidly about where the stroke began")
{
    std::vector<sculpt::Dab> dabs;
    sculpt::Dab first = flatDab({0.0, 0.0, 0.0}, 20.0);
    dabs.push_back(first);
    // Out along +X, then a quarter turn around to +Z.
    for (int i = 1; i <= 10; ++i)
    {
        sculpt::Dab d = first;
        d.delta = glm::dvec3(1.0, 0.0, 0.0);
        dabs.push_back(d);
    }
    for (int i = 1; i <= 20; ++i)
    {
        const double a0 = (i - 1) * 0.5 * glm::pi<double>() / 20.0, a1 = i * 0.5 * glm::pi<double>() / 20.0;
        sculpt::Dab d = first;
        d.delta = 10.0 * glm::dvec3(std::cos(a1) - std::cos(a0), 0.0, std::sin(a1) - std::sin(a0));
        dabs.push_back(d);
    }
    const geo::Mesh grid = flatGrid();
    sculpt::SculptMesh s = strokeOn(grid, sculpt::Brush::Rotate, dabs);
    double moved = 0.0, radial = 0.0;
    for (Index p = 0; p < grid.pointCapacity(); ++p)
    {
        const glm::dvec3 a = grid.position(p), b = s.mesh().position(p);
        moved = std::max(moved, glm::length(b - a));
        radial = std::max(radial, std::abs(glm::length(b) - glm::length(a)));
        if (std::abs(b.y) > 1e-9)
            FAIL_CHECK("rotated off the plane");
    }
    CHECK(moved > 1.0);
    CHECK(radial < 1e-6);
}

TEST_CASE("Pose bends the part under the brush about the ring where it meets the rest")
{
    geo::CylinderOptions co;
    co.radius = 25.0;
    co.subdivHeight = 20;
    co.subdivCaps = 4;
    const geo::Mesh rod = geo::makeCylinder(co);
    geo::Bounds b = rod.bounds();
    // Grab the top end and pull sideways.
    sculpt::Dab first = flatDab({0.0, b.max.y, 0.0}, b.size().y * 0.4);
    std::vector<sculpt::Dab> dabs = {first};
    for (int i = 0; i < 10; ++i)
    {
        sculpt::Dab d = first;
        d.delta = glm::dvec3(b.size().y * 0.03, 0.0, 0.0);
        dabs.push_back(d);
    }
    sculpt::SculptMesh s = strokeOn(rod, sculpt::Brush::Pose, dabs);
    double tip = 0.0, base = 0.0;
    for (Index p = 0; p < rod.pointCapacity(); ++p)
    {
        const double move = glm::length(s.mesh().position(p) - rod.position(p));
        if (rod.position(p).y > b.max.y - 1e-6)
            tip = std::max(tip, move);
        if (rod.position(p).y < b.max.y - first.radius - 1e-6)
            base = std::max(base, move);
    }
    CHECK(tip > b.size().y * 0.05);
    CHECK(base == 0.0);
}

TEST_CASE("Boundary moves open borders and leaves a closed surface alone")
{
    sculpt::Dab first = flatDab({50.0, 0.0, 0.0}, 15.0);
    std::vector<sculpt::Dab> dabs = {first};
    sculpt::Dab pull = first;
    pull.delta = glm::dvec3(0.0, 2.0, 0.0);
    dabs.push_back(pull);
    const geo::Mesh grid = flatGrid();
    sculpt::SculptMesh s = strokeOn(grid, sculpt::Brush::Boundary, dabs);
    double border = 0.0, inner = 0.0;
    for (Index p = 0; p < grid.pointCapacity(); ++p)
    {
        const double move = glm::length(s.mesh().position(p) - grid.position(p));
        if (grid.position(p).x > 50.0 - 1e-6)
            border = std::max(border, move);
        if (grid.position(p).x < 50.0 - 2.5 * 4)
            inner = std::max(inner, move);
    }
    CHECK(border > 1.0);
    CHECK(inner == 0.0);

    geo::Mesh cube = geo::makeCube({});
    sculpt::SculptMesh closed(cube);
    std::vector<sculpt::Dab> onCube = {dabAt(closed, glm::dvec3(0.0, 0.0, 200.0), 30.0)};
    sculpt::Dab cubePull = onCube[0];
    cubePull.delta = glm::dvec3(5.0, 0.0, 0.0);
    onCube.push_back(cubePull);
    CHECK(maxDiff(strokeOn(cube, sculpt::Brush::Boundary, onCube).mesh(), cube) == 0.0);
}

TEST_CASE("Snake Hook, Elastic Grab, Nudge, Thumb, Clay, Blob and Magnify move the surface the way they say")
{
    const geo::Mesh grid = flatGrid();
    auto moveOf = [&](sculpt::Brush brush, const std::vector<sculpt::Dab>& dabs, bool invert = false) {
        sculpt::SculptMesh s = strokeOn(grid, brush, dabs, invert);
        glm::dvec3 sum(0.0);
        double far = 0.0;
        for (Index p = 0; p < grid.pointCapacity(); ++p)
        {
            const glm::dvec3 d = s.mesh().position(p) - grid.position(p);
            sum += d;
            if (glm::length(glm::dvec2(grid.position(p).x, grid.position(p).z)) > 12.0)
                far = std::max(far, glm::length(d));
        }
        return std::pair(sum, far);
    };
    sculpt::Dab at = flatDab({0.0, 0.0, 0.0}, 10.0);
    sculpt::Dab up = at;
    up.delta = glm::dvec3(0.0, 1.0, 0.0);
    // Snake Hook pulls up along the drag.
    CHECK(moveOf(sculpt::Brush::SnakeHook, {at, up, up, up}).first.y > 1.0);
    // Elastic Grab reaches past the brush radius; Grab does not.
    CHECK(moveOf(sculpt::Brush::ElasticGrab, {at, up, up}).second > 0.01);
    CHECK(moveOf(sculpt::Brush::Grab, {at, up, up}).second == 0.0);
    // Nudge and Thumb push along the stroke and stay on the surface.
    sculpt::Dab along = at;
    along.direction = glm::dvec3(1.0, 0.0, 0.0);
    for (sculpt::Brush brush : {sculpt::Brush::Nudge, sculpt::Brush::Thumb})
    {
        const glm::dvec3 sum = moveOf(brush, {along, along, along}).first;
        CHECK_MESSAGE(sum.x > 1.0, sculpt::brushName(brush));
        CHECK_MESSAGE(std::abs(sum.y) < 1e-9, sculpt::brushName(brush));
    }
    // Clay and Blob build up; Ctrl digs.
    for (sculpt::Brush brush : {sculpt::Brush::Clay, sculpt::Brush::Blob})
    {
        CHECK_MESSAGE(moveOf(brush, {at, at, at}).first.y > 0.1, sculpt::brushName(brush));
        CHECK_MESSAGE(moveOf(brush, {at, at, at}, true).first.y < -0.1, sculpt::brushName(brush));
    }
    // Magnify spreads points out from the centre: the ring just inside the radius moves outward.
    sculpt::SculptMesh m = strokeOn(grid, sculpt::Brush::Magnify, {at, at});
    double outward = 0.0;
    for (Index p = 0; p < grid.pointCapacity(); ++p)
    {
        const glm::dvec2 a(grid.position(p).x, grid.position(p).z), b(m.mesh().position(p).x, m.mesh().position(p).z);
        outward += glm::length(b) - glm::length(a);
    }
    CHECK(outward > 0.1);
}

TEST_CASE("Smooth removes noise and keeps the form's volume")
{
    geo::SubdivideOptions o;
    o.levels = 2;
    geo::Mesh noisy = geo::subdivide(sphere(16), o);
    for (Index p = 0; p < noisy.pointCapacity(); ++p)
    {
        const double h = std::sin(double(p) * 12.9898) * 43758.5453;
        const glm::dvec3 at = noisy.position(p);
        noisy.setPosition(p, at * ((50.0 + (h - std::floor(h) - 0.5) * 1.2) / glm::length(at)));
    }
    auto radii = [](const geo::Mesh& m) {
        double sum = 0.0, sq = 0.0;
        int n = 0;
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (glm::length(m.position(p) - glm::dvec3(50.0, 0.0, 0.0)) < 12.0)
            {
                const double r = glm::length(m.position(p));
                sum += r;
                sq += r * r;
                ++n;
            }
        const double mean = sum / n;
        return std::pair(mean, std::sqrt(std::max(0.0, sq / n - mean * mean)));
    };
    auto smoothed = [&](bool keepVolume) {
        sculpt::SculptMesh s(noisy);
        sculpt::BrushSettings b;
        b.brush = sculpt::Brush::Smooth;
        b.strength = 1.0;
        b.keepVolume = keepVolume;
        s.beginStroke(b);
        for (int i = 0; i < 20; ++i)
            s.dab(dabAt(s, glm::dvec3(200.0, 0.0, 0.0), 30.0));
        s.endStroke();
        return radii(s.mesh());
    };
    const auto [meanBefore, noiseBefore] = radii(noisy);
    const auto [meanAfter, noiseAfter] = smoothed(true);
    CHECK(noiseBefore > 0.25);
    CHECK(noiseAfter < noiseBefore * 0.3);
    CHECK(std::abs(meanAfter - meanBefore) < 0.15);
    // Keep Volume off averages points, so the form shrinks toward flat as well.
    const auto [meanFlat, noiseFlat] = smoothed(false);
    CHECK(noiseFlat < noiseBefore);
    CHECK(meanBefore - meanFlat > 2.0 * std::abs(meanAfter - meanBefore));
}

TEST_CASE("Pinch draws points in and presses them down; Ctrl spreads and lifts")
{
    const geo::Mesh grid = flatGrid();
    auto moveOf = [&](bool invert) {
        sculpt::SculptMesh s = strokeOn(grid, sculpt::Brush::Pinch, std::vector<sculpt::Dab>(4, flatDab({0.0, 0.0, 0.0}, 10.0)), invert);
        double down = 0.0, inward = 0.0;
        for (Index p = 0; p < grid.pointCapacity(); ++p)
        {
            const glm::dvec3 a = grid.position(p), b = s.mesh().position(p);
            down += a.y - b.y;
            inward += glm::length(glm::dvec2(a.x, a.z)) - glm::length(glm::dvec2(b.x, b.z));
        }
        return std::pair(down, inward);
    };
    const auto [down, inward] = moveOf(false);
    CHECK(down > 0.1);
    CHECK(inward > 0.1);
    const auto [downInv, inwardInv] = moveOf(true);
    CHECK(downInv < -0.1);
    CHECK(inwardInv < -0.1);
}

TEST_CASE("Pinch at depth 0 draws points in without pressing them down")
{
    const geo::Mesh grid = flatGrid();
    sculpt::SculptMesh s(grid);
    sculpt::BrushSettings b;
    b.brush = sculpt::Brush::Pinch;
    b.strength = 1.0;
    b.pinchDepth = 0.0;
    s.beginStroke(b);
    for (int i = 0; i < 4; ++i)
        s.dab(flatDab({0.0, 0.0, 0.0}, 10.0));
    s.endStroke();
    double down = 0.0, inward = 0.0;
    for (Index p = 0; p < grid.pointCapacity(); ++p)
    {
        const glm::dvec3 a = grid.position(p), c = s.mesh().position(p);
        down = std::max(down, std::abs(a.y - c.y));
        inward += glm::length(glm::dvec2(a.x, a.z)) - glm::length(glm::dvec2(c.x, c.z));
    }
    CHECK(down < 1e-9);
    CHECK(inward > 0.1);
}

TEST_CASE("Ctrl-Grab moves along the stroke's normal, not the drag")
{
    const geo::Mesh grid = flatGrid();
    sculpt::Dab at = flatDab({0.0, 0.0, 0.0}, 10.0);
    auto moveOf = [&](glm::dvec3 delta, bool invert) {
        sculpt::Dab drag = at;
        drag.delta = delta;
        sculpt::SculptMesh s = strokeOn(grid, sculpt::Brush::Grab, {at, drag, drag}, invert);
        glm::dvec3 sum(0.0);
        double sideways = 0.0;
        for (Index p = 0; p < grid.pointCapacity(); ++p)
        {
            const glm::dvec3 d = s.mesh().position(p) - grid.position(p);
            sum += d;
            sideways = std::max(sideways, glm::length(glm::dvec2(d.x, d.z)));
        }
        return std::pair(sum, sideways);
    };
    const auto [plain, plainSideways] = moveOf({1.0, 0.5, 0.0}, false);
    CHECK(plain.x > 1.0);
    CHECK(plainSideways > 0.1);
    const auto [out, outSideways] = moveOf({1.0, 0.5, 0.0}, true);
    CHECK(out.y > 1.0);
    CHECK(outSideways < 1e-9);
    CHECK(moveOf({0.3, -1.0, 0.0}, true).first.y < -1.0);
}

TEST_CASE("mask and freeze are respected")
{
    geo::SubdivideOptions o;
    o.levels = 2;
    geo::Mesh proxy = geo::subdivide(geo::makeCube({}), o);
    sculpt::SculptMesh s(proxy);
    std::vector<float> mask(proxy.pointCapacity(), 0.0f);
    for (Index p = 0; p < proxy.pointCapacity(); ++p)
        mask[p] = proxy.position(p).x < 0.0 ? 1.0f : 0.0f;
    s.setMask(mask);
    sculpt::BrushSettings b;
    b.brush = sculpt::Brush::Draw;
    b.strength = 1.0;
    s.beginStroke(b);
    for (int i = 0; i < 10; ++i)
        s.dab(dabAt(s, glm::dvec3(-20.0 + i * 4.0, 0.0, 200.0), 40.0));
    sculpt::StrokeRecord r = s.endStroke();
    REQUIRE(!r.empty());
    bool movedUnmasked = false;
    for (Index p = 0; p < proxy.pointCapacity(); ++p)
    {
        double d = glm::length(s.mesh().position(p) - proxy.position(p));
        if (mask[p] > 0.0f)
            CHECK(d == 0.0);
        else
            movedUnmasked |= d > 1e-6;
    }
    CHECK(movedUnmasked);

    // Freeze brush, then a stroke over it: frozen points stay.
    sculpt::BrushSettings f;
    f.brush = sculpt::Brush::Freeze;
    s.beginStroke(f);
    s.dab(dabAt(s, glm::dvec3(30.0, 0.0, 200.0), 15.0));
    s.endStroke();
    std::vector<glm::dvec3> held(s.mesh().positions().begin(), s.mesh().positions().end());
    std::vector<uint8_t> frozen(s.frozen().begin(), s.frozen().end());
    REQUIRE(std::count(frozen.begin(), frozen.end(), 1) > 0);
    b.brush = sculpt::Brush::Inflate;
    s.beginStroke(b);
    s.dab(dabAt(s, glm::dvec3(30.0, 0.0, 200.0), 40.0));
    s.endStroke();
    for (Index p = 0; p < proxy.pointCapacity(); ++p)
        if (frozen[p])
            CHECK(s.mesh().position(p) == held[p]);

    // Mask ops and undo of them.
    sculpt::StrokeRecord inv = s.maskOp(sculpt::MaskOp::Invert);
    CHECK(!inv.empty());
    CHECK(s.mask()[0] == 1.0f - mask[0]);
    s.apply(inv, true);
    CHECK(s.mask()[0] == mask[0]);
}

namespace
{
struct FitResult
{
    double sculptMove = 0.0; // the proxy's largest move
    double cageMove = 0.0;   // the fitted cage's largest move
    double rms = 0.0;        // fitted subdivision to the sculpt
};

/** Sculpts a mesh with these strokes and fits its cage, with and without the move limit. */
std::pair<FitResult, FitResult> sculptAndFit(geo::Mesh cage, const std::vector<json>& strokes)
{
    makeMeshNode("fitProbe", std::move(cage));
    ops::execute("sculptEnter", {{"args", {"fitProbe"}}, {"levels", 2}});
    auto s = sculpt::active();
    REQUIRE(s);
    const std::vector<glm::dvec3> start(s->sculpt.mesh().positions().begin(), s->sculpt.mesh().positions().end());
    for (const json& st : strokes)
        ops::execute("sculptStroke", st);
    std::span<const glm::dvec3> now = s->sculpt.mesh().positions();
    auto fit = [&](double ratio) {
        FitResult r;
        for (size_t i = 0; i < now.size(); ++i)
            r.sculptMove = std::max(r.sculptMove, glm::length(now[i] - start[i]));
        sculpt::FitOptions fo;
        fo.maxMoveRatio = ratio;
        const geo::Mesh& cage = s->stack->levels.front()->mesh;
        std::vector<glm::dvec3> x = sculpt::fitCage(cage, s->stack->stencils, now, fo);
        for (Index p = 0; p < cage.pointCapacity(); ++p)
            r.cageMove = std::max(r.cageMove, glm::length(x[p] - cage.position(p)));
        for (const auto& st : s->stack->stencils)
            x = st.apply(x);
        for (size_t i = 0; i < now.size(); ++i)
            r.rms += glm::dot(x[i] - now[i], x[i] - now[i]);
        r.rms = std::sqrt(r.rms / double(now.size()));
        return r;
    };
    auto out = std::pair{fit(sculpt::FitOptions{}.maxMoveRatio), fit(0.0)};
    ops::execute("sculptCancel", json::object());
    return out;
}

std::vector<json> strokes(glm::dvec3 c, glm::dvec3 n, double radius, int count)
{
    json d = {{"center", {c.x, c.y, c.z}}, {"normal", {n.x, n.y, n.z}}, {"radius", radius}};
    return std::vector<json>(size_t(count), json{{"brush", "draw"}, {"strength", 1.0}, {"dabs", std::vector<json>(6, d)}});
}
} // namespace

TEST_CASE("exit fit keeps a coarse cage from overshooting the sculpt")
{
    const geo::CubeOptions co{.subdivX = 2, .subdivY = 2, .subdivZ = 2};
    const double ratio = sculpt::FitOptions{}.maxMoveRatio;

    SUBCASE("a small pull on a face")
    {
        auto [limited, plain] = sculptAndFit(geo::makeCube(co), strokes({0.0, 0.0, 40.0}, {0.0, 0.0, 1.0}, 45.0, 5));
        CHECK(plain.cageMove > ratio * limited.sculptMove);
        CHECK(limited.cageMove <= ratio * limited.sculptMove * 1.01);
        CHECK(limited.rms < plain.rms * 1.25);
    }
    SUBCASE("a stroke on a corner")
    {
        auto [limited, plain] = sculptAndFit(geo::makeCube(co), strokes({50.0, 50.0, 50.0}, glm::normalize(glm::dvec3(1.0)), 60.0, 6));
        // Unlimited, the corner lands three and a half times as far off as the sculpt moved.
        CHECK(plain.cageMove > 3.0 * limited.sculptMove);
        CHECK(limited.cageMove <= ratio * limited.sculptMove * 1.01);
    }
}

TEST_CASE("exit fit still follows a broad sculpt")
{
    // Big strokes on a cage dense enough to hold them: the limit never bites and the fit stays close.
    std::vector<json> all = strokes({0.0, 0.0, 50.0}, {0.0, 0.0, 1.0}, 70.0, 6);
    for (json& s : strokes({50.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, 40.0, 6))
        all.push_back(std::move(s));
    auto [limited, plain] = sculptAndFit(sphere(8), all);
    CHECK(limited.sculptMove > 30.0);
    CHECK(limited.rms < 0.05 * limited.sculptMove);
    CHECK(limited.rms <= plain.rms * 1.01);
}

TEST_CASE("CageFitter matches fitCage from any start")
{
    geo::Mesh cage = sphere(6);
    geo::SubdivideOptions o;
    o.levels = 2;
    auto stencils = geo::subdivideStencils(cage, o);
    std::vector<glm::dvec3> target(cage.positions().begin(), cage.positions().end());
    for (glm::dvec3& p : target)
        p *= 1.1;
    for (const auto& s : stencils)
        target = s.apply(target);
    const sculpt::CageFitter fitter(cage, stencils);
    std::vector<glm::dvec3> a = sculpt::fitCage(cage, stencils, target);
    std::vector<glm::dvec3> b = fitter.solve(cage.positions(), target);
    double err = 0.0;
    for (Index p = 0; p < cage.pointCapacity(); ++p)
        err = std::max(err, glm::length(a[p] - b[p]));
    CHECK(err < 1e-12);
    // Starting from the answer leaves it there.
    std::vector<glm::dvec3> c = fitter.solve(b, target);
    err = 0.0;
    for (Index p = 0; p < cage.pointCapacity(); ++p)
        err = std::max(err, glm::length(c[p] - b[p]));
    CHECK(err < 1e-3);
}

TEST_CASE("sculpt commands undo as single steps")
{
    scene::Node* n = makeMeshNode("undoCube", geo::makeCube({}));
    (void)n;
    geo::Mesh before = inMesh("undoCube");
    UndoStack& u = UndoStack::get();
    ops::execute("sculptEnter", {{"args", {"undoCube"}}, {"levels", 2}});
    auto s = sculpt::active();
    REQUIRE(s);
    std::vector<glm::dvec3> start(s->sculpt.mesh().positions().begin(), s->sculpt.mesh().positions().end());
    json dab = {{"center", {0.0, 0.0, 50.0}}, {"normal", {0.0, 0.0, 1.0}}, {"radius", 40.0}};
    json r = ops::execute("sculptStroke", {{"brush", "draw"}, {"strength", 1.0}, {"dabs", json::array({dab, dab, dab})}});
    CHECK(r["points"].get<int>() > 0);
    std::vector<glm::dvec3> sculpted(s->sculpt.mesh().positions().begin(), s->sculpt.mesh().positions().end());
    CHECK(sculpted != start);
    ops::execute("sculptExit", json::object());
    CHECK_FALSE(sculpt::active());
    CHECK(inMesh("undoCube").numFaces() == 6 * 16);
    CHECK(sculpt::storedLevels(*scene::Graph::current().find("undoCube")));

    // Undo exit: back in sculpt mode with the sculpt intact, mesh and levels as before.
    REQUIRE(u.undo());
    REQUIRE(sculpt::active() == s);
    CHECK(maxDiff(before, inMesh("undoCube")) == 0.0);
    CHECK(std::vector<glm::dvec3>(s->sculpt.mesh().positions().begin(), s->sculpt.mesh().positions().end()) == sculpted);
    CHECK_FALSE(sculpt::storedLevels(*scene::Graph::current().find("undoCube")));
    // Undo the stroke, then the enter.
    REQUIRE(u.undo());
    CHECK(std::vector<glm::dvec3>(s->sculpt.mesh().positions().begin(), s->sculpt.mesh().positions().end()) == start);
    REQUIRE(u.undo());
    CHECK_FALSE(sculpt::active());
    // Redo all three.
    REQUIRE(u.redo());
    CHECK(sculpt::active() == s);
    REQUIRE(u.redo());
    CHECK(std::vector<glm::dvec3>(s->sculpt.mesh().positions().begin(), s->sculpt.mesh().positions().end()) == sculpted);
    REQUIRE(u.redo());
    CHECK_FALSE(sculpt::active());
    CHECK(inMesh("undoCube").numFaces() == 6 * 16);

    // Re-entering picks up the stored levels.
    json r2 = ops::execute("sculptEnter", {{"args", {"undoCube"}}});
    REQUIRE(sculpt::active());
    CHECK(r2["level"].get<int>() == 2);
    CHECK(r2["levels"].get<int>() == 2);
    ops::execute("sculptCancel", json::object());
    CHECK_FALSE(sculpt::active());
}

TEST_CASE("transferShape and shrinkwrap move a cage onto a high poly")
{
    geo::Mesh high = sphere(40, 60.0);
    makeMeshNode("high", high);
    makeMeshNode("low", geo::makeCube({}));
    geo::MeshBvh highBvh(high);
    ops::execute("transferShape", {{"args", {"high", "low"}}, {"levels", 2}});
    // The fitted cage's subdivision lies near the sphere.
    geo::SubdivideOptions o;
    o.levels = 2;
    geo::Mesh sub = geo::subdivide(inMesh("low"), o);
    double worst = 0.0;
    for (Index p = 0; p < sub.pointCapacity(); ++p)
        worst = std::max(worst, std::abs(glm::length(sub.position(p)) - 60.0));
    CHECK(worst < 6.0);

    makeMeshNode("wrapped", geo::makeCube({}));
    ops::execute("shrinkwrap", {{"args", {"wrapped"}}, {"target", "high"}, {"method", "normal"}, {"offset", 2.0}, {"iterations", 0}});
    const geo::Mesh& w = inMesh("wrapped");
    for (Index p = 0; p < w.pointCapacity(); ++p)
        CHECK(std::abs(glm::length(w.position(p)) - 62.0) < 0.5);
}

TEST_CASE("brush benchmark on a million-triangle proxy" * doctest::skip(std::getenv("LILA_SCULPT_BENCH") == nullptr))
{
    geo::SubdivideOptions o;
    o.levels = 3;
    auto t0 = std::chrono::steady_clock::now();
    geo::Mesh proxy = geo::subdivide(sphere(90), o);
    auto t1 = std::chrono::steady_clock::now();
    sculpt::SculptMesh s(std::move(proxy));
    s.setSymmetry(geo::axisPlane(0), 1e-4);
    auto render = s.renderMesh();
    auto t2 = std::chrono::steady_clock::now();
    const size_t tris = render->indices.size() / 3;
    MESSAGE("proxy: " << s.mesh().numPoints() << " points, " << tris << " triangles; subdivide "
                      << std::chrono::duration<double, std::milli>(t1 - t0).count() << " ms, setup "
                      << std::chrono::duration<double, std::milli>(t2 - t1).count() << " ms");
    CHECK(tris >= 1000000);
    {
        std::scoped_lock lock(render->dynamic->mutex);
        render->dynamic->allDirty = false;
        render->dynamic->dirty.clear();
    }

    for (auto [brush, radius] : {std::pair{sculpt::Brush::Draw, 8.0}, std::pair{sculpt::Brush::Draw, 25.0}, std::pair{sculpt::Brush::ClayStrips, 8.0},
                                 std::pair{sculpt::Brush::Smooth, 8.0}, std::pair{sculpt::Brush::Relax, 8.0}, std::pair{sculpt::Brush::Grab, 25.0}})
    {
        sculpt::BrushSettings b;
        b.brush = brush;
        b.strength = 0.5;
        s.beginStroke(b);
        const int dabs = 100;
        double worst = 0.0;
        size_t dirtyMax = 0;
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < dabs; ++i)
        {
            auto d0 = std::chrono::steady_clock::now();
            sculpt::Dab d = dabAt(s, glm::dvec3(-30.0 + i * 0.6, 10.0, 200.0), radius);
            d.delta = glm::dvec3(0.05, 0.0, 0.0);
            s.dab(d);
            worst = std::max(worst, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - d0).count());
            // What the renderer would copy for this frame.
            std::scoped_lock lock(render->dynamic->mutex);
            dirtyMax = std::max(dirtyMax, render->dynamic->dirty.size());
            render->dynamic->dirty.clear();
        }
        double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        sculpt::StrokeRecord r = s.endStroke();
        MESSAGE(sculpt::brushName(brush) << " r" << radius << ": " << total / dabs << " ms per dab (worst " << worst << "), " << r.points.size()
                                         << " points changed, at most " << dirtyMax << " of " << s.mesh().numPoints()
                                         << " vertices re-uploaded per dab");
        CHECK(total / dabs < 16.0);
        CHECK(dirtyMax < s.mesh().numPoints() / 20);
    }
}


TEST_CASE("partial refit matches a rebuilt bvh")
{
    geo::SubdivideOptions o;
    o.levels = 2;
    sculpt::SculptMesh s(geo::subdivide(sphere(12), o));
    sculpt::BrushSettings b;
    b.brush = sculpt::Brush::Draw;
    b.strength = 1.0;
    s.beginStroke(b);
    for (int i = 0; i < 30; ++i)
        s.dab(dabAt(s, glm::dvec3(-20.0 + i, 5.0, 200.0), 20.0));
    s.endStroke();
    // Same triangles: a fresh triangulation of the deformed quads could pick other diagonals.
    geo::MeshBvh fresh;
    fresh.build(s.mesh(), s.bvh().triangles());
    for (int i = 0; i < 200; ++i)
    {
        glm::dvec3 from(std::sin(i * 0.37) * 80.0, std::cos(i * 0.21) * 80.0, 200.0);
        glm::dvec3 dir = glm::normalize(glm::dvec3(0.0, 0.0, 0.0) - from + glm::dvec3(std::sin(i * 1.3) * 30.0, 0.0, 0.0));
        geo::BvhHit a, c;
        bool ha = s.bvh().raycast(from, dir, a), hc = fresh.raycast(from, dir, c);
        REQUIRE(ha == hc);
        if (ha)
            CHECK(std::abs(a.t - c.t) < 1e-9);
    }
}

TEST_CASE("strokes on a radially symmetric mesh repeat exactly through its orbits")
{
    for (bool mirror : {false, true})
    {
        CAPTURE(mirror);
        sculpt::SculptMesh s(sphere(24, 50.0));
        geo::SymmetrySpec spec;
        spec.radial = geo::axisRadial(1, glm::dvec3(0.0), 6, mirror);
        s.setSymmetry(spec, 1e-4);
        REQUIRE(s.symmetric());
        const geo::SymmetryMap& map = s.symmetry();
        REQUIRE(map.isRadial());
        REQUIRE(map.unmatchedCount() == 0);
        auto radialAsymmetry = [&] {
            double worst = 0.0;
            for (Index p = 0; p < s.mesh().pointCapacity(); ++p)
                for (int g = 1; g < map.order(); ++g)
                    if (const Index q = map.image(g, p); q != geo::kInvalid)
                        worst = std::max(worst, glm::length(s.mesh().position(q) - map.transform(g, s.mesh().position(p))));
            return worst;
        };
        CHECK(radialAsymmetry() < 1e-9);
        for (sculpt::Brush brush : {sculpt::Brush::Draw, sculpt::Brush::Inflate, sculpt::Brush::Smooth, sculpt::Brush::Pinch, sculpt::Brush::Grab})
        {
            sculpt::BrushSettings b;
            b.brush = brush;
            b.strength = 0.8;
            // The brush's own radial copies are ignored on a radially symmetric mesh.
            b.radial.count = 5;
            s.beginStroke(b);
            for (int i = 0; i < 8; ++i)
            {
                sculpt::Dab d = dabAt(s, glm::dvec3(30.0 + i * 3.0, 20.0, 200.0), 20.0);
                d.delta = glm::dvec3(1.0, 0.5, 0.0);
                d.direction = glm::dvec3(1.0, 0.0, 0.0);
                s.dab(d);
            }
            sculpt::StrokeRecord r = s.endStroke();
            CHECK_MESSAGE(!r.empty(), sculpt::brushName(brush));
            CHECK_MESSAGE(radialAsymmetry() < 1e-9, sculpt::brushName(brush));
        }
    }
}

TEST_CASE("the sculpt draws with the mesh's material slots and uv seams")
{
    geo::Mesh cube = geo::makeCube({});
    cube.materialNames() = {"Red", "Blue"};
    std::span<int32_t> mat = cube.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
    mat[0] = 1;
    mat[1] = 1;
    geo::SubdivideOptions o;
    o.levels = 2;
    sculpt::SculptMesh sm(geo::subdivide(cube, o));
    const auto rm = sm.renderMesh();
    REQUIRE(rm->sections.size() == 2);
    std::set<uint32_t> slots;
    for (const render::MeshSection& s : rm->sections)
        slots.insert(s.material);
    CHECK(slots == std::set<uint32_t>{0, 1});
    // A cube's uvs are cut, so seam points get a vertex per side.
    CHECK(rm->dynamic->vertices.size() > size_t(sm.mesh().numPoints()));

    // A stroke moves every vertex of a seam point together.
    sculpt::BrushSettings b;
    sm.beginStroke(b);
    sculpt::Dab d;
    d.center = glm::dvec3(50.0);
    d.normal = glm::normalize(glm::dvec3(1.0));
    d.radius = 30.0;
    sm.dab(d);
    sm.endStroke();
    std::map<glm::vec3, int, decltype([](const glm::vec3& a, const glm::vec3& c) { return std::tie(a.x, a.y, a.z) < std::tie(c.x, c.y, c.z); })> seen;
    for (Index p = 0; p < sm.mesh().pointCapacity(); ++p)
        if (sm.mesh().pointValid(p))
            seen[glm::vec3(sm.mesh().position(p))] = 0;
    for (const render::MeshVertex& v : rm->dynamic->vertices)
        CHECK(seen.contains(v.pos));
}

namespace
{
/** A flat 40 cm square in XZ, 0.5 cm apart, with a column of points on x = 0. */
struct FineGrid
{
    static constexpr int kSide = 81;
    static constexpr double kStep = 0.5;
    geo::Mesh mesh;

    FineGrid()
    {
        geo::PlaneOptions o;
        o.width = o.depth = kStep * (kSide - 1);
        o.subdivX = o.subdivZ = kSide - 1;
        mesh = geo::makePlane(o);
    }

    /** Point at column i (x) and row k (z). */
    Index at(const std::vector<std::vector<Index>>& lookup, int i, int k) const { return lookup[size_t(i)][size_t(k)]; }

    std::vector<std::vector<Index>> lookup() const
    {
        std::vector<std::vector<Index>> out(kSide, std::vector<Index>(kSide, geo::kInvalid));
        const double half = kStep * (kSide - 1) * 0.5;
        for (Index p = 0; p < mesh.pointCapacity(); ++p)
        {
            const glm::dvec3 q = mesh.position(p);
            out[size_t(std::lround((q.x + half) / kStep))][size_t(std::lround((q.z + half) / kStep))] = p;
        }
        return out;
    }
};

struct PlaneReport
{
    double asymmetry = 0.0;
    /** Largest distance of a point on the plane from it. */
    double offPlane = 0.0;
    double moved = 0.0;
    /** Largest change in move from a plane point to its neighbour across x, and from there to the next two out. */
    double planeStep = 0.0, nextStep = 0.0;
};

PlaneReport planeReport(const sculpt::SculptMesh& s, const FineGrid& grid, std::span<const glm::dvec3> before)
{
    PlaneReport r;
    r.asymmetry = asymmetry(s);
    const auto lookup = grid.lookup();
    const int mid = FineGrid::kSide / 2;
    auto disp = [&](int i, int k) {
        const Index p = grid.at(lookup, i, k);
        return s.mesh().position(p) - before[p];
    };
    for (int k = 0; k < FineGrid::kSide; ++k)
    {
        r.offPlane = std::max(r.offPlane, std::abs(s.mesh().position(grid.at(lookup, mid, k)).x));
        for (int i = 0; i < FineGrid::kSide; ++i)
            r.moved = std::max(r.moved, glm::length(disp(i, k)));
        r.planeStep = std::max(r.planeStep, glm::length(disp(mid + 1, k) - disp(mid, k)));
        for (int i = mid + 2; i <= mid + 3; ++i)
            r.nextStep = std::max(r.nextStep, glm::length(disp(i, k) - disp(i - 1, k)));
    }
    return r;
}

/** Every built-in brush that moves points, plus Mask. */
std::vector<sculpt::Brush> planeBrushes()
{
    std::vector<sculpt::Brush> out;
    for (int b = 0; b < int(sculpt::Brush::Count); ++b)
        if (sculpt::Brush(b) != sculpt::Brush::Freeze && sculpt::Brush(b) != sculpt::Brush::Paint && sculpt::Brush(b) != sculpt::Brush::Boundary)
            out.push_back(sculpt::Brush(b));
    return out;
}
} // namespace

TEST_CASE("strokes near and across the symmetry plane leave no seam at the centre")
{
    const FineGrid grid;
    const std::vector<glm::dvec3> before(grid.mesh.positions().begin(), grid.mesh.positions().end());
    // Along the plane just off it, so dab and mirror overlap without matching; and straight across it.
    struct Path
    {
        const char* name;
        glm::dvec3 from, step, delta;
    };
    const Path paths[] = {
        {"along", {1.5, 0.0, -4.0}, {0.0, 0.0, 1.0}, {0.4, 0.3, 0.6}},
        {"across", {-6.0, 0.0, 0.0}, {1.5, 0.0, 0.0}, {0.6, 0.3, 0.2}},
    };
    for (sculpt::Brush brush : planeBrushes())
        for (const Path& path : paths)
        {
            CAPTURE(sculpt::brushName(brush));
            CAPTURE(std::string(path.name));
            sculpt::SculptMesh s(grid.mesh);
            s.setSymmetry(geo::axisPlane(0), 1e-4);
            REQUIRE(s.symmetric());
            sculpt::BrushSettings b;
            b.brush = brush;
            b.strength = 0.6;
            s.beginStroke(b);
            for (int i = 0; i < 9; ++i)
            {
                sculpt::Dab d = flatDab(path.from + path.step * double(i), 5.0);
                d.delta = path.delta;
                d.direction = glm::normalize(path.step);
                s.dab(d);
            }
            s.endStroke();
            if (brush == sculpt::Brush::Mask)
            {
                // The mask's own seam: the plane column against its neighbours.
                const auto lookup = grid.lookup();
                const int mid = FineGrid::kSide / 2;
                double step = 0.0, planeStep = 0.0, most = 0.0;
                for (int k = 0; k < FineGrid::kSide; ++k)
                {
                    for (int i = 1; i < FineGrid::kSide; ++i)
                        step = std::max(step, double(std::abs(s.mask()[grid.at(lookup, i, k)] - s.mask()[grid.at(lookup, i - 1, k)])));
                    planeStep = std::max(planeStep, double(std::abs(s.mask()[grid.at(lookup, mid + 1, k)] - s.mask()[grid.at(lookup, mid, k)])));
                    for (int i = 0; i < FineGrid::kSide; ++i)
                        most = std::max(most, double(s.mask()[grid.at(lookup, i, k)]));
                }
                CHECK(most > 0.0);
                CHECK(planeStep <= step);
                continue;
            }
            const PlaneReport r = planeReport(s, grid, before);
            if (r.moved < 1e-9)
                continue;
            CHECK(r.asymmetry == 0.0);
            CHECK(r.offPlane == 0.0);
            // A smooth move changes about as much from the plane to its neighbour as between the next points out; a seam jumps.
            CHECK(r.planeStep <= 2.0 * r.nextStep + 0.02 * r.moved);
        }
}

TEST_CASE("a held or dragged brush on the symmetry plane moves points once, not once per side")
{
    const FineGrid grid;
    for (sculpt::Brush brush : {sculpt::Brush::Grab, sculpt::Brush::ElasticGrab, sculpt::Brush::SnakeHook, sculpt::Brush::Nudge,
                                sculpt::Brush::Pinch, sculpt::Brush::Inflate, sculpt::Brush::Mask})
    {
        CAPTURE(sculpt::brushName(brush));
        sculpt::SculptMesh plain(grid.mesh), mirrored(grid.mesh);
        mirrored.setSymmetry(geo::axisPlane(0), 1e-4);
        REQUIRE(mirrored.symmetric());
        sculpt::BrushSettings b;
        b.brush = brush;
        b.strength = 0.6;
        for (sculpt::SculptMesh* s : {&plain, &mirrored})
        {
            s->beginStroke(b);
            for (int i = 0; i < 4; ++i)
            {
                sculpt::Dab d = flatDab({0.0, 0.0, double(i) * 0.5}, 5.0);
                // Along the plane, so the mirrored drag is the same drag.
                d.delta = glm::dvec3(0.0, 0.4, 0.5);
                d.direction = glm::dvec3(0.0, 0.0, 1.0);
                s->dab(d);
            }
            s->endStroke();
        }
        double off = 0.0, moved = 0.0, maskOff = 0.0, mask = 0.0;
        for (Index p = 0; p < grid.mesh.pointCapacity(); ++p)
        {
            off = std::max(off, glm::length(plain.mesh().position(p) - mirrored.mesh().position(p)));
            moved = std::max(moved, glm::length(plain.mesh().position(p) - grid.mesh.position(p)));
            maskOff = std::max(maskOff, double(std::abs(plain.mask()[p] - mirrored.mask()[p])));
            mask = std::max(mask, double(plain.mask()[p]));
        }
        CHECK(moved + mask > 0.0);
        CHECK(off <= 1e-6 * std::max(moved, 1.0));
        CHECK(maskOff <= 1e-5);
    }
}
