#include "TexPaintTest.h"

#include "geo/Attribute.h"
#include "render/PaintGpu.h"

using namespace textest;
namespace gp = lila::render::paint;

namespace
{
/** A 100 x 100 plane in x and z whose uvs fill the square: u = (x + 50) / 100, v = (z + 50) / 100. */
geo::Mesh tilePlane()
{
    const std::vector<glm::dvec3> pts = {{-50, 0, -50}, {-50, 0, 50}, {50, 0, 50}, {50, 0, -50}};
    const std::vector<geo::Index> sizes = {4}, idx = {0, 1, 2, 3};
    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    auto uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    for (geo::Index c : m.faceCorners(0))
    {
        const glm::dvec3 p = m.position(m.cornerPoint(c));
        uv[c] = {float((p.x + 50.0) / 100.0), float((p.z + 50.0) / 100.0)};
    }
    return m;
}

json edgeStroke()
{
    json dabs = json::array();
    for (double z : {-10.0, 0.0, 10.0})
        dabs.push_back({{"position", {47.0, 0.0, z}}, {"radius", 6.0}});
    return {{"space", "surface"}, {"mesh", "Tile"}, {"brush", brushJson()}, {"payload", {{"channels", {{"baseColor", {1.0, 1.0, 0.0}}}}}}, {"dabs", dabs}};
}
} // namespace

TEST_CASE("texture paint tileable: a stroke across the right edge comes in on the left")
{
    if (!gpu())
        return;
    newScene();
    addMesh("Tile", tilePlane());
    scene::Node& set = makeSet("Trim", 128);

    paintStroke("Trim", "P", edgeStroke());
    image::Image img = gp::readChannel(set.id(), "P", "baseColor");
    CHECK(at(img, {0.97f, 0.5f})[3] > 0.5f);
    CHECK(at(img, {0.01f, 0.5f})[3] == 0.0f);

    run("texSetSettings", {{"args", {"Trim", {{"tileable", true}}}}});
    paintStroke("Trim", "Q", edgeStroke());
    img = gp::readChannel(set.id(), "Q", "baseColor");
    CHECK(at(img, {0.97f, 0.5f})[3] > 0.5f);
    // The dabs at x = 47 (u 0.97) reach 3 past the edge: u 0.01 on the other side is as far from them as u 0.93 is.
    CHECK(at(img, {0.01f, 0.5f})[3] > 0.1f);
    CHECK(at(img, {0.01f, 0.5f})[3] == doctest::Approx(at(img, {0.93f, 0.5f})[3]).epsilon(0.08));
    CHECK(at(img, {0.2f, 0.5f})[3] == 0.0f);
}
