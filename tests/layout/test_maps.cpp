#include "bake/Maps.h"
#include "geo/Attribute.h"
#include "geo/Primitives.h"
#include "layout/Edit.h"
#include "layout/Lines.h"
#include "layout/Maps.h"

#include <doctest/doctest.h>

using namespace lila;
using namespace lila::layout;

namespace
{
std::vector<glm::dvec3> mapLine(glm::dvec3 a, glm::dvec3 b)
{
    std::vector<glm::dvec3> pts;
    const int n = std::max(1, int(glm::length(b - a) / 0.25));
    for (int i = 0; i <= n; ++i)
        pts.push_back(glm::mix(a, b, double(i) / n));
    return pts;
}

/** A 100 cm plane split down the middle by one line in the given output. */
std::shared_ptr<const Resolved> split(Output output)
{
    static auto mesh = std::make_shared<const geo::Mesh>(geo::makePlane({100.0, 100.0, 10, 10}));
    Surface s(*mesh);
    EditContext ctx{s, 0.5, 0.25};
    PanelLayout l = PanelLayout::withDefaultSets();
    l.sets.front().output = output;
    l.sets.front().width = 4.0;
    l.sets.front().depth = 1.0;
    addStroke(l, ctx, mapLine({-50, 0, 0}, {50, 0, 0}));
    return Resolved::build(l, mesh, {.spacing = 0.25});
}

const float* texel(const image::Image& img, double u, double v)
{
    const int x = std::clamp(int(u * img.width), 0, img.width - 1), y = std::clamp(int((1.0 - v) * img.height), 0, img.height - 1);
    return &img.rgba[(size_t(y) * img.width + x) * 4];
}
} // namespace

TEST_CASE("maps: Texture lines bake into height, panels into ids, lines into the mask")
{
    auto r = split(Output::Texture);
    const LayoutMaps maps = bakeMaps(*r, {.resolution = 128});
    CHECK(maps.warnings.empty());
    // The line runs across the middle of UV space, at v = 0.5.
    CHECK(texel(maps.height, 0.3, 0.5)[0] < 0.2f);
    CHECK(texel(maps.height, 0.3, 0.2)[0] == doctest::Approx(1.0f));
    CHECK(texel(maps.lines, 0.3, 0.5)[0] > 0.8f);
    CHECK(texel(maps.lines, 0.3, 0.2)[0] == doctest::Approx(0.0f));
    const float* a = texel(maps.panelId, 0.3, 0.2);
    const float* b = texel(maps.panelId, 0.3, 0.8);
    CHECK((a[0] != b[0] || a[1] != b[1] || a[2] != b[2]));
}

TEST_CASE("maps: a mesh without UVs says so")
{
    geo::Mesh bare;
    const geo::Index p0 = bare.addPoint({0, 0, 0}), p1 = bare.addPoint({1, 0, 0}), p2 = bare.addPoint({0, 0, 1});
    bare.addFace({p0, p1, p2});
    auto r = Resolved::build(PanelLayout::withDefaultSets(), std::make_shared<const geo::Mesh>(bare));
    CHECK_THROWS_AS(bakeMaps(*r), std::runtime_error);
}

TEST_CASE("lines: Decal lines become strips on their own material")
{
    auto r = split(Output::Decal);
    const LinesResult out = applyLines(*r);
    const auto& names = out.mesh.materialNames();
    REQUIRE(std::find(names.begin(), names.end(), std::string(kDecalMaterial)) != names.end());
    CHECK(out.mesh.numFaces() > r->mesh()->numFaces());
    CHECK(out.mesh.bounds().max.y > 0.0);
}

TEST_CASE("maps: the baker offers the layout maps")
{
    for (const char* id : {"panelLines", "panelId", "panelHeight"})
    {
        const bake::MapDef* def = bake::findMap(id);
        REQUIRE(def);
        CHECK(def->cpu);
        CHECK(def->prepare);
    }
}
