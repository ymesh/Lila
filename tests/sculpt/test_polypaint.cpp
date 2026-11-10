#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/ExtensionContext.h"
#include "geo/Bvh.h"
#include "geo/ColorSets.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "geo/Symmetry.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "sculpt/Sculpt.h"
#include "sculpt/SculptSession.h"

#include <doctest/doctest.h>

using namespace lila;
using geo::Index;

namespace
{
geo::Mesh denseCube()
{
    geo::SubdivideOptions o;
    o.levels = 3;
    return geo::subdivide(geo::makeCube({}), o);
}

sculpt::Dab frontDab(const sculpt::SculptMesh& s, double x, double radius)
{
    geo::BvhHit hit;
    const glm::dvec3 from(x, 0.0, 200.0), dir(0.0, 0.0, -1.0);
    REQUIRE(s.raycast(from, dir, hit));
    sculpt::Dab d;
    d.center = hit.point;
    d.normal = s.normalAt(hit);
    d.radius = radius;
    d.viewDir = dir;
    return d;
}

size_t countColored(const sculpt::SculptMesh& s)
{
    size_t n = 0;
    for (const glm::vec4& c : s.colors())
        n += c != glm::vec4(1.0f);
    return n;
}

void makePaintNode(const std::string& name, geo::Mesh mesh)
{
    scene::Graph& g = scene::Graph::current();
    if (scene::Node* old = g.find(name))
        g.deleteNode(old->id());
    scene::Node* n = g.createNode(*scene::types::find("mesh"), name, scene::kNoNode, false);
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
}

const geo::Mesh& storedPaintMesh(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    return *std::get<scene::MeshPtr>(g.storedValue({g.find(name)->id(), scene::MeshNode::kInMesh, -1}));
}
} // namespace

TEST_CASE("blurring paint and mask softens them without moving anything")
{
    const geo::Mesh proxy = denseCube();
    sculpt::SculptMesh s(proxy);
    s.setSymmetry(geo::axisPlane(0), 1e-4);
    // A hard red/white edge and a hard mask edge, both across y = 0.
    std::vector<glm::vec4> colors(proxy.pointCapacity(), glm::vec4(1.0f));
    std::vector<float> mask(proxy.pointCapacity(), 0.0f);
    for (Index p = 0; p < proxy.pointCapacity(); ++p)
        if (proxy.position(p).y > 0.0)
        {
            colors[p] = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
            mask[p] = 1.0f;
        }
    s.setColors(colors);
    const std::vector<glm::dvec3> held(s.mesh().positions().begin(), s.mesh().positions().end());
    auto softened = [&](auto value) {
        size_t n = 0;
        for (Index p = 0; p < proxy.pointCapacity(); ++p)
            n += value(p) > 0.01f && value(p) < 0.99f;
        return n;
    };
    CHECK(softened([&](Index p) { return s.colors()[p].g; }) == 0);

    sculpt::BrushSettings b;
    b.brush = sculpt::Brush::Paint;
    b.color = glm::vec3(0.0f, 0.0f, 1.0f);
    b.strength = 1.0;
    b.blur = true;
    s.beginStroke(b);
    for (int i = 0; i < 6; ++i)
        s.dab(frontDab(s, -20.0 + i * 8.0, 15.0));
    sculpt::StrokeRecord rec = s.endStroke();
    REQUIRE(!rec.empty());
    CHECK(softened([&](Index p) { return s.colors()[p].g; }) > 0);
    for (const glm::vec4& c : s.colors())
        CHECK(c.b == doctest::Approx(c.g));
    CHECK(std::equal(held.begin(), held.end(), s.mesh().positions().begin()));
    s.apply(rec, true);
    CHECK(std::equal(colors.begin(), colors.end(), s.colors().begin()));

    s.setMask(mask);
    b.brush = sculpt::Brush::Mask;
    s.beginStroke(b);
    for (int i = 0; i < 6; ++i)
        s.dab(frontDab(s, -20.0 + i * 8.0, 15.0));
    s.endStroke();
    CHECK(softened([&](Index p) { return s.mask()[p]; }) > 0);
    CHECK(std::equal(held.begin(), held.end(), s.mesh().positions().begin()));
    // Blurring leaves untouched what was already even.
    for (Index p = 0; p < proxy.pointCapacity(); ++p)
        if (std::abs(proxy.position(p).y) > 40.0)
            CHECK(s.mask()[p] == mask[p]);
}

TEST_CASE("the paint brush colors without moving, honours the mask and symmetry, and undoes")
{
    const geo::Mesh proxy = denseCube();
    sculpt::SculptMesh s(proxy);
    s.setSymmetry(geo::axisPlane(0), 1e-4);
    const std::vector<glm::dvec3> held(s.mesh().positions().begin(), s.mesh().positions().end());
    std::vector<float> mask(proxy.pointCapacity(), 0.0f);
    for (Index p = 0; p < proxy.pointCapacity(); ++p)
        mask[p] = proxy.position(p).y > 20.0 ? 1.0f : 0.0f;
    s.setMask(mask);
    CHECK(s.colors().empty());
    CHECK_FALSE(s.painted());

    sculpt::BrushSettings b;
    b.brush = sculpt::Brush::Paint;
    b.strength = 1.0;
    b.color = glm::vec3(1.0f, 0.0f, 0.0f);
    s.beginStroke(b);
    for (int i = 0; i < 6; ++i)
        s.dab(frontDab(s, 10.0 + i * 3.0, 30.0));
    sculpt::StrokeRecord r = s.endStroke();
    REQUIRE(!r.empty());
    CHECK(s.painted());
    CHECK(r.colorBefore.size() == r.points.size());
    CHECK(r.colorAfter.size() == r.points.size());

    for (Index p = 0; p < proxy.pointCapacity(); ++p)
    {
        CHECK(s.mesh().position(p) == held[p]);
        if (mask[p] > 0.0f)
            CHECK(s.colors()[p] == glm::vec4(1.0f));
        if (Index q = s.symmetry().mirrorPoint(p); q != geo::kInvalid)
            CHECK(s.colors()[p] == s.colors()[q]);
    }
    // Painted on the stroke side and mirrored onto the other.
    bool left = false, right = false;
    for (Index p = 0; p < proxy.pointCapacity(); ++p)
        if (s.colors()[p].g < 0.5f)
            (proxy.position(p).x < 0.0 ? left : right) = true;
    CHECK(left);
    CHECK(right);

    const size_t colored = countColored(s);
    s.apply(r, true);
    CHECK_FALSE(s.painted());
    s.apply(r, false);
    CHECK(countColored(s) == colored);
}

TEST_CASE("shape brushes paint where they move when paint is on")
{
    const geo::Mesh proxy = denseCube();
    sculpt::SculptMesh s(proxy);
    sculpt::BrushSettings b;
    b.brush = sculpt::Brush::Draw;
    b.strength = 1.0;
    s.beginStroke(b);
    s.dab(frontDab(s, 0.0, 30.0));
    s.endStroke();
    CHECK(s.colors().empty());

    b.paint = true;
    b.color = glm::vec3(0.0f, 0.0f, 1.0f);
    s.beginStroke(b);
    s.dab(frontDab(s, 0.0, 30.0));
    sculpt::StrokeRecord r = s.endStroke();
    REQUIRE(s.painted());
    for (size_t i = 0; i < r.points.size(); ++i)
        if (r.after[i] != r.before[i])
            CHECK(s.colors()[r.points[i]].r < 1.0f);

    // Grab never paints.
    sculpt::SculptMesh g(proxy);
    b.brush = sculpt::Brush::Grab;
    g.beginStroke(b);
    sculpt::Dab d = frontDab(g, 0.0, 30.0);
    d.delta = glm::dvec3(0.0, 5.0, 0.0);
    g.dab(d);
    g.endStroke();
    CHECK_FALSE(g.painted());
}

TEST_CASE("fill and clear polypaint, masked points keep theirs")
{
    const geo::Mesh proxy = denseCube();
    sculpt::SculptMesh s(proxy);
    std::vector<float> mask(proxy.pointCapacity(), 0.0f);
    mask[0] = 1.0f;
    s.setMask(mask);
    CHECK(s.clearColor().empty());
    sculpt::StrokeRecord fill = s.fillColor(glm::vec3(0.2f, 0.4f, 0.6f));
    CHECK(!fill.empty());
    CHECK(s.colors()[0] == glm::vec4(1.0f));
    CHECK(s.colors()[1] == glm::vec4(0.2f, 0.4f, 0.6f, 1.0f));
    sculpt::StrokeRecord clear = s.clearColor();
    CHECK_FALSE(s.painted());
    s.apply(clear, true);
    CHECK(s.colors()[1] == glm::vec4(0.2f, 0.4f, 0.6f, 1.0f));
}

TEST_CASE("a registered brush sculpts through the shared stroke, and unloads with its extension")
{
    const geo::Mesh proxy = denseCube();
    {
        ext::Context ctx("testBrushes");
        sculpt::BrushDef def;
        def.label = "Lift";
        def.displace = [](const sculpt::BrushArea& area, const sculpt::BrushSample& p) {
            return glm::dvec3(0.0, 1.0, 0.0) * (p.weight * area.radius * 0.1 * area.sign);
        };
        REQUIRE(sculpt::registerBrush(ctx, "lift", def) != 0);
        CHECK(sculpt::registerBrush(ctx, "draw", def) == 0);
        CHECK(sculpt::registerBrush(ctx, "empty", sculpt::BrushDef{}) == 0);

        sculpt::BrushSettings b;
        REQUIRE(sculpt::setBrush(b, "lift"));
        CHECK(b.brush == sculpt::Brush::Custom);
        CHECK(sculpt::brushId(b) == "lift");
        b.strength = 1.0;
        b.paint = true;
        b.color = glm::vec3(0.0f, 1.0f, 0.0f);
        sculpt::SculptMesh s(proxy);
        std::vector<float> mask(proxy.pointCapacity(), 0.0f);
        for (Index p = 0; p < proxy.pointCapacity(); ++p)
            mask[p] = proxy.position(p).x < 0.0 ? 1.0f : 0.0f;
        s.setMask(mask);
        s.beginStroke(b);
        s.dab(frontDab(s, 0.0, 30.0));
        sculpt::StrokeRecord r = s.endStroke();
        REQUIRE(!r.empty());
        bool lifted = false;
        for (Index p = 0; p < proxy.pointCapacity(); ++p)
        {
            const glm::dvec3 moved = s.mesh().position(p) - proxy.position(p);
            if (mask[p] > 0.0f)
                CHECK(moved == glm::dvec3(0.0));
            CHECK(moved.x == 0.0);
            CHECK(moved.z == 0.0);
            lifted |= moved.y > 1e-6;
        }
        CHECK(lifted);
        CHECK(s.painted());

        // Ctrl inverts through area.sign.
        b.invert = true;
        sculpt::SculptMesh t(proxy);
        t.beginStroke(b);
        t.dab(frontDab(t, 0.0, 30.0));
        t.endStroke();
        bool lowered = false;
        for (Index p = 0; p < proxy.pointCapacity(); ++p)
            lowered |= t.mesh().position(p).y < proxy.position(p).y - 1e-6;
        CHECK(lowered);
        ctx.unloadAll();
    }
    CHECK_FALSE(sculpt::brushRegistry().contains("lift"));
    sculpt::BrushSettings b;
    CHECK_FALSE(sculpt::setBrush(b, "lift"));
}

TEST_CASE("polypaint survives exit onto the mesh and the stored levels, and resumes")
{
    makePaintNode("paintCube", geo::makeCube({}));
    ops::execute("sculptEnter", {{"args", {"paintCube"}}, {"levels", 2}});
    auto s = sculpt::active();
    REQUIRE(s);
    json dab = {{"center", {0.0, 0.0, 50.0}}, {"normal", {0.0, 0.0, 1.0}}, {"radius", 90.0}};
    json r = ops::execute("sculptStroke", {{"brush", "paint"}, {"strength", 1.0}, {"color", {1.0, 0.0, 0.0}}, {"dabs", json::array({dab, dab})}});
    CHECK(r["points"].get<int>() > 0);
    REQUIRE(s->sculpt.painted());
    CHECK(ops::execute("sculptState", json::object())["painted"].get<bool>());

    // Fill and its undo.
    UndoStack& u = UndoStack::get();
    const std::vector<glm::vec4> painted(s->sculpt.colors().begin(), s->sculpt.colors().end());
    ops::execute("sculptColor", {{"op", "fill"}, {"color", {0.0, 1.0, 0.0}}});
    CHECK(s->sculpt.colors()[0] == glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
    REQUIRE(u.undo());
    CHECK(std::vector<glm::vec4>(s->sculpt.colors().begin(), s->sculpt.colors().end()) == painted);

    CHECK_THROWS(ops::execute("sculptStroke", {{"brush", "noSuchBrush"}, {"dabs", json::array({dab})}}));

    ops::execute("sculptExit", json::object());
    const geo::Mesh& cage = storedPaintMesh("paintCube");
    REQUIRE(geo::colors::has(cage, sculpt::kColorSet));
    bool red = false;
    for (const glm::vec4& c : geo::colors::pointColors(cage, sculpt::kColorSet))
        red |= c.r > 0.5f && c.g < 0.5f;
    CHECK(red);
    REQUIRE(sculpt::storedLevels(*scene::Graph::current().find("paintCube")));

    ops::execute("sculptEnter", {{"args", {"paintCube"}}});
    REQUIRE(sculpt::active());
    std::span<const glm::vec4> resumed = sculpt::active()->sculpt.colors();
    REQUIRE(resumed.size() == painted.size());
    float worst = 0.0f;
    for (size_t p = 0; p < painted.size(); ++p)
        worst = std::max(worst, glm::length(resumed[p] - painted[p]));
    CHECK(worst < 1e-5f);
    ops::execute("sculptCancel", json::object());
}

TEST_CASE("dam standard cuts in at the centre, trim dynamic only lowers, fold raises a ridge")
{
    const geo::Mesh proxy = denseCube();
    double front = -1e9;
    for (Index p = 0; p < proxy.pointCapacity(); ++p)
        front = std::max(front, proxy.position(p).z);
    auto run = [&](sculpt::Brush brush, bool invert) {
        sculpt::SculptMesh s(proxy);
        sculpt::BrushSettings b;
        b.brush = brush;
        b.strength = 1.0;
        b.invert = invert;
        s.beginStroke(b);
        for (int i = 0; i < 6; ++i)
        {
            sculpt::Dab d = frontDab(s, -10.0 + i * 4.0, 25.0);
            d.direction = glm::dvec3(1.0, 0.0, 0.0);
            s.dab(d);
        }
        s.endStroke();
        double lowest = 0.0, highest = 0.0;
        for (Index p = 0; p < proxy.pointCapacity(); ++p)
            if (proxy.position(p).z > front - 3.0)
            {
                const double dz = s.mesh().position(p).z - proxy.position(p).z;
                lowest = std::min(lowest, dz);
                highest = std::max(highest, dz);
            }
        return std::pair{lowest, highest};
    };
    auto [damLow, damHigh] = run(sculpt::Brush::DamStandard, false);
    CHECK(damLow < -0.1);
    // A flat face has nothing above the trim plane but the plane sits under it, so trimming only cuts down.
    auto [trimLow, trimHigh] = run(sculpt::Brush::TrimDynamic, false);
    CHECK(trimLow < 0.0);
    CHECK(trimHigh <= 1e-9);
    auto [foldLow, foldHigh] = run(sculpt::Brush::Fold, false);
    CHECK(foldHigh > 0.1);
    auto [creaseLow, creaseHigh] = run(sculpt::Brush::Fold, true);
    CHECK(creaseLow < -0.1);

    // Polish flattens the rounded face under it.
    auto spread = [&](const geo::Mesh& m) {
        double lo = 1e9, hi = -1e9;
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (proxy.position(p).z > front - 10.0 && std::abs(proxy.position(p).x) < 10.0 && std::abs(proxy.position(p).y) < 10.0)
            {
                lo = std::min(lo, m.position(p).z);
                hi = std::max(hi, m.position(p).z);
            }
        return hi - lo;
    };
    sculpt::SculptMesh s(proxy);
    sculpt::BrushSettings b;
    b.brush = sculpt::Brush::Polish;
    b.strength = 1.0;
    s.beginStroke(b);
    for (int i = 0; i < 10; ++i)
        s.dab(frontDab(s, 0.0, 30.0));
    s.endStroke();
    CHECK(spread(s.mesh()) < spread(proxy) * 0.8);
}

TEST_CASE("undoing a stroke hands back the tumble point it moved")
{
    makePaintNode("pivotCube", geo::makeCube({}));
    ops::execute("sculptEnter", {{"args", {"pivotCube"}}, {"levels", 1}});
    auto s = sculpt::active();
    REQUIRE(s);
    std::vector<std::optional<glm::dvec3>> handed;
    sculpt::setPivotHandler([&](const std::optional<glm::dvec3>& p) { handed.push_back(p); });

    // What the viewport tool does: apply the stroke live, then adopt it through the command.
    sculpt::BrushSettings b;
    b.strength = 1.0;
    s->sculpt.beginStroke(b);
    sculpt::Dab d;
    d.center = glm::dvec3(0.0, 0.0, 50.0);
    d.normal = glm::dvec3(0.0, 0.0, 1.0);
    d.radius = 40.0;
    s->sculpt.dab(d);
    sculpt::StrokeRecord rec = s->sculpt.endStroke();
    REQUIRE(!rec.empty());
    rec.pivotMoved = true;
    rec.pivotBefore = std::nullopt;
    rec.pivotAfter = glm::dvec3(1.0, 2.0, 3.0);
    const uint64_t token = sculpt::nextLiveToken();
    s->liveToken = token;
    s->live = std::move(rec);
    ops::execute("sculptStroke", {{"live", int64_t(token)}});

    UndoStack& u = UndoStack::get();
    REQUIRE(u.undo());
    REQUIRE(handed.size() == 1);
    CHECK_FALSE(handed[0].has_value());
    REQUIRE(u.redo());
    REQUIRE(handed.size() == 2);
    REQUIRE(handed[1].has_value());
    CHECK(*handed[1] == glm::dvec3(1.0, 2.0, 3.0));
    sculpt::setPivotHandler(nullptr);
    ops::execute("sculptCancel", json::object());
}
