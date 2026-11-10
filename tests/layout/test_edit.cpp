#include "geo/Primitives.h"
#include "layout/Edit.h"
#include "layout/Resolve.h"

#include <doctest/doctest.h>

#include <memory>

using namespace lila;
using namespace lila::layout;

namespace
{
/** A 100 cm plane in XZ, 40 x 40 quads. */
std::shared_ptr<const geo::Mesh> plane()
{
    static auto m = std::make_shared<const geo::Mesh>(geo::makePlane({100.0, 100.0, 40, 40}));
    return m;
}

std::vector<glm::dvec3> stroke(std::initializer_list<glm::dvec3> corners, double step = 0.25)
{
    std::vector<glm::dvec3> pts;
    const std::vector<glm::dvec3> c(corners);
    for (size_t k = 0; k + 1 < c.size(); ++k)
    {
        const int n = std::max(1, int(glm::length(c[k + 1] - c[k]) / step));
        for (int i = (k == 0 ? 0 : 1); i <= n; ++i)
            pts.push_back(glm::mix(c[k], c[k + 1], double(i) / n));
    }
    return pts;
}

struct Fixture
{
    Surface surface{*plane()};
    EditContext ctx{surface, 0.5, 0.25};
    PanelLayout layout = PanelLayout::withDefaultSets();

    int degreeNear(const glm::dvec3& p) const
    {
        for (const Junction& j : layout.junctions)
            if (glm::length(surface.resolve(j.at) - p) < 1.0)
                return layout.degree(j.id);
        return -1;
    }

    std::shared_ptr<const Resolved> resolve() const { return Resolved::build(layout, plane(), {.spacing = 0.25}); }
};
} // namespace

TEST_CASE("edit: a stroke makes one run between two free ends")
{
    Fixture f;
    const EditResult r = addStroke(f.layout, f.ctx, stroke({{-40, 0, 0}, {40, 0, 0}}));
    CHECK(r.changed);
    REQUIRE(f.layout.runs.size() == 1);
    CHECK(f.layout.junctions.size() == 2);
    CHECK(f.layout.runs[0].controls.empty());
    CHECK(f.layout.runs[0].set == f.layout.activeSet);
}

TEST_CASE("edit: a stroke ending on a line makes a T, one overshooting it is trimmed back")
{
    Fixture f;
    addStroke(f.layout, f.ctx, stroke({{-40, 0, 0}, {40, 0, 0}}));
    addStroke(f.layout, f.ctx, stroke({{0, 0, 30}, {0, 0, 0.2}}));
    CHECK(f.layout.runs.size() == 3);
    CHECK(f.degreeNear({0, 0, 0}) == 3);

    // Ends 1.2 past the line: within the overshoot, so it stops on the line.
    addStroke(f.layout, f.ctx, stroke({{20, 0, 30}, {20, 0, -1.2}}));
    CHECK(f.degreeNear({20, 0, 0}) == 3);
    for (const Junction& j : f.layout.junctions)
        CHECK(f.surface.resolve(j.at).z > -0.6);
}

TEST_CASE("edit: crossing lines split at a crossing junction")
{
    Fixture f;
    addStroke(f.layout, f.ctx, stroke({{-40, 0, 0}, {40, 0, 0}}));
    addStroke(f.layout, f.ctx, stroke({{10, 0, -30}, {10, 0, 30}}));
    CHECK(f.layout.runs.size() == 4);
    CHECK(f.degreeNear({10, 0, 0}) == 4);
}

TEST_CASE("edit: a closed stroke bounds a panel")
{
    Fixture f;
    const EditResult r = addStroke(f.layout, f.ctx, stroke({{-20, 0, -20}, {20, 0, -20}, {20, 0, 20}, {-20, 0, 20}, {-20, 0, -20.3}}));
    REQUIRE(r.changed);
    CHECK(f.layout.runs.size() == 4);
    for (const Junction& j : f.layout.junctions)
        CHECK(f.layout.degree(j.id) == 2);
    auto res = f.resolve();
    CHECK(res->panels().size() == 2);
    const PanelSample in = res->panelAt({0, 0, 0}), out = res->panelAt({40, 0, 40});
    CHECK(in.panel >= 0);
    CHECK(out.panel >= 0);
    CHECK(in.panel != out.panel);
    // Just beside the line, each side reads its own panel.
    CHECK(res->panelAt({19.9, 0, 0}).panel == in.panel);
    CHECK(res->panelAt({20.1, 0, 0}).panel == out.panel);
}

TEST_CASE("edit: panel keys stay put when unrelated lines change")
{
    Fixture f;
    addStroke(f.layout, f.ctx, stroke({{-50, 0, 0}, {50, 0, 0}}));
    const uint64_t before = f.resolve()->panelAt({0, 0, 20}).key;
    addStroke(f.layout, f.ctx, stroke({{-30, 0, -40}, {30, 0, -40}}));
    CHECK(f.resolve()->panelAt({0, 0, 20}).key == before);
}

TEST_CASE("edit: drawing along a line redraws it instead of adding one")
{
    Fixture f;
    addStroke(f.layout, f.ctx, stroke({{-40, 0, 0}, {40, 0, 0}}));
    // Leaves the line heading along it, bows out, rejoins it.
    const EditResult r = addStroke(f.layout, f.ctx, stroke({{-10, 0, 0}, {-5, 0, 0}, {0, 0, 6}, {5, 0, 0}, {10, 0, 0}}));
    CHECK(r.changed);
    REQUIRE(f.layout.runs.size() == 1);
    CHECK(f.layout.junctions.size() == 2);
    CHECK_FALSE(f.layout.runs[0].controls.empty());
    auto res = f.resolve();
    CHECK(res->lineAt({0, 0, 6}, 1.0).run >= 0);
    CHECK(res->lineAt({0, 0, 0}, 1.0).run < 0);

    // Starting on the free end heading on extends the same run.
    addStroke(f.layout, f.ctx, stroke({{40, 0, 0}, {48, 0, 0}}));
    CHECK(f.layout.runs.size() == 1);
    CHECK(f.resolve()->lineAt({47, 0, 0}, 0.5).run >= 0);
}

TEST_CASE("edit: erasing the stem of a T joins the line back into one run")
{
    Fixture f;
    addStroke(f.layout, f.ctx, stroke({{-40, 0, 0}, {40, 0, 0}}));
    addStroke(f.layout, f.ctx, stroke({{0, 0, 30}, {0, 0, 0}}));
    REQUIRE(f.layout.runs.size() == 3);
    eraseStroke(f.layout, f.ctx, stroke({{-5, 0, 20}, {5, 0, 20}}));
    CHECK(f.layout.runs.size() == 1);
    CHECK(f.layout.junctions.size() == 2);
}

TEST_CASE("edit: smoothing pulls controls toward their neighbours, junctions stay")
{
    Fixture f;
    addStroke(f.layout, f.ctx, stroke({{-30, 0, 0}, {-10, 0, 0}, {0, 0, 4}, {10, 0, 0}, {30, 0, 0}}), {.cleanup = 0.0});
    REQUIRE(f.layout.runs.size() == 1);
    REQUIRE_FALSE(f.layout.runs[0].controls.empty());
    auto peak = [&] {
        double z = 0.0;
        for (const Anchor& a : f.layout.runs[0].controls)
            z = std::max(z, f.surface.resolve(a).z);
        return z;
    };
    const double before = peak();
    const glm::dvec3 a0 = f.surface.resolve(f.layout.junction(f.layout.runs[0].a)->at);
    for (int i = 0; i < 5; ++i)
        smoothStroke(f.layout, f.ctx, stroke({{-15, 0, 2}, {15, 0, 2}}), 10.0, 1.0);
    CHECK(peak() < before - 0.5);
    CHECK(glm::length(f.surface.resolve(f.layout.junction(f.layout.runs[0].a)->at) - a0) < 1e-9);
}

TEST_CASE("edit: bend, split, corner, offset, loosen and refit")
{
    Fixture f;
    addStroke(f.layout, f.ctx, stroke({{-40, 0, 0}, {40, 0, 0}}));
    const uint32_t run = f.layout.runs[0].id;

    bendRun(f.layout, f.ctx, run, {0, 0, 0}, {0, 0, 5});
    CHECK(f.layout.run(run)->controls.size() == 1);
    CHECK(f.resolve()->lineAt({0, 0, 5}, 0.5).run >= 0);

    const EditResult s = splitRun(f.layout, f.ctx, run, {-20, 0, 2.5});
    CHECK(f.layout.runs.size() == 2);
    REQUIRE(s.junctions.size() == 1);
    toggleCorner(f.layout, s.junctions[0]);
    CHECK(f.layout.junction(s.junctions[0])->corner);
    // A smooth two-run junction joins back up; a corner one stays.
    deleteJunctions(f.layout, f.ctx, s.junctions);
    CHECK(f.layout.runs.size() == 1);

    const EditResult o = offsetRuns(f.layout, f.ctx, std::vector<uint32_t>{f.layout.runs[0].id}, 3.0);
    CHECK(o.runs.size() == 1);
    CHECK(f.layout.runs.size() == 2);

    const uint32_t first = f.layout.runs[0].id;
    loosenRuns(f.layout, f.ctx, std::vector<uint32_t>{first});
    CHECK(f.layout.run(first)->loose);
    CHECK(f.layout.run(first)->controls.size() > 10);
    refitRuns(f.layout, f.ctx, 1.0);
    CHECK_FALSE(f.layout.run(first)->loose);
    CHECK(f.layout.run(first)->controls.size() < 6);
}

TEST_CASE("edit: chainOf follows two-run junctions")
{
    Fixture f;
    addStroke(f.layout, f.ctx, stroke({{-30, 0, 0}, {0, 0, 0}, {0, 0, 30}}));
    REQUIRE(f.layout.runs.size() == 2);
    CHECK(chainOf(f.layout, f.layout.runs[0].id).size() == 2);
}

TEST_CASE("resolve: a mirror makes the other side, sharing junctions on the plane")
{
    Fixture f;
    f.layout.mirrorAxis = 0;
    addStroke(f.layout, f.ctx, stroke({{0.2, 0, 10}, {30, 0, 10}}));
    REQUIRE(f.layout.runs.size() == 1);
    auto res = f.resolve();
    REQUIRE(res->runs().size() == 2);
    CHECK(res->runs()[1].mirrored());
    CHECK(res->lineAt({-20, 0, 10}, 0.5).run == 1);
    // The end on the plane is shared, so it is no free end.
    CHECK_FALSE(res->runs()[0].freeA);

    // A stroke drawn on the mirrored side is kept on the drawn side.
    addStroke(f.layout, f.ctx, stroke({{-10, 0, -20}, {-30, 0, -20}}));
    for (const Junction& j : f.layout.junctions)
        CHECK(f.surface.resolve(j.at).x >= 0.0);
}
