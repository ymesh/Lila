#include "TexPaintTest.h"

#include "geo/Attribute.h"
#include "render/PaintGpu.h"
#include "scene/MeshNodes.h"
#include "tex/PaintStrokes.h"

using namespace textest;
namespace gp = lila::render::paint;

namespace
{
/** The panel unwrapped the other way round: face 0 on the right of uv space, face 1 on the left. */
geo::Mesh swappedPanel()
{
    geo::Mesh m = seamPanel();
    auto uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    for (glm::vec2& c : uv)
        c.x = 1.0f - c.x;
    return m;
}

/** The panel cut into more faces: same surface, other triangles. */
geo::Mesh finerPanel()
{
    std::vector<glm::dvec3> pts;
    std::vector<geo::Index> sizes, idx;
    const int n = 4;
    for (int f = 0; f < 2; ++f)
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i <= n; ++i)
                pts.push_back({-50.0 + f * 50.0 + 50.0 * i / n, 0.0, -50.0 + 100.0 * j / n});
    const int row = n + 1, per = row * row;
    for (int f = 0; f < 2; ++f)
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
            {
                const geo::Index a = geo::Index(f * per + j * row + i);
                sizes.push_back(4);
                idx.insert(idx.end(), {a, geo::Index(a + row), geo::Index(a + row + 1), geo::Index(a + 1)});
            }
    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    auto uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    for (geo::Index f = 0; f < m.numFaces(); ++f)
        for (geo::Index c : m.faceCorners(f))
        {
            const glm::dvec3 p = m.position(m.cornerPoint(c));
            uv[c] = seamUv(f < geo::Index(n * n) ? 0 : 1, p.x, p.z);
        }
    return m;
}

void setMesh(geo::Mesh m)
{
    scene::Node* n = sg().find("PanelMesh");
    sg().setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
}

json stroke()
{
    json dabs = json::array();
    for (double z : {-20.0, -10.0, 0.0, 10.0, 20.0})
        dabs.push_back({{"position", {-25.0, 0.0, z}}, {"radius", 6.0}});
    return {{"space", "surface"}, {"brush", brushJson()}, {"payload", {{"channels", {{"baseColor", {1.0, 0.0, 1.0}}}}}}, {"dabs", dabs}, {"mesh", "PanelMesh"}};
}
} // namespace

TEST_CASE("texture paint retarget: strokes follow the surface through a new unwrap and a remesh")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    paintStroke("Panel", "P", stroke());
    const tex::paint::Stroke& s = *tex::paint::readPaint(set)->buffer("P")->strokes.front();
    REQUIRE(s.anchors.size() == s.dabs.size());
    CHECK(s.meshSignature != 0);
    CHECK(at(gp::readChannel(set.id(), "P", "baseColor"), seamUv(0, -25.0, 0.0))[3] > 0.5f);

    // New uvs: the paint moves with face 0 to the right half of uv space.
    setMesh(swappedPanel());
    gp::sync(sg(), set);
    image::Image img = gp::readChannel(set.id(), "P", "baseColor");
    glm::vec2 moved = seamUv(0, -25.0, 0.0);
    moved.x = 1.0f - moved.x;
    CHECK(at(img, moved)[3] > 0.5f);
    CHECK(at(img, seamUv(0, -25.0, 0.0))[3] == 0.0f);

    // A finer mesh with the first unwrap: the paint lands where it was on the surface, the same texels as at first.
    setMesh(finerPanel());
    gp::sync(sg(), set);
    img = gp::readChannel(set.id(), "P", "baseColor");
    CHECK(at(img, seamUv(0, -25.0, 0.0))[3] > 0.5f);
    CHECK(at(img, seamUv(0, -25.0, 10.0))[3] > 0.5f);
    CHECK(at(img, seamUv(1, 25.0, 0.0))[3] == 0.0f);
}

TEST_CASE("texture paint retarget: Transfer Texture Set moves the look onto another mesh")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    geo::Mesh other = finerPanel();
    scene::Node* target = sg().createNode(*scene::types::find("mesh"), "NewPanel");
    sg().setValue({target->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(other))));
    makeSet("Panel", 128);
    run("texAddLayer", {{"args", {"Panel", "fill"}}, {"material", {{"roughness", 0.25}}}});
    const json r = run("texTransferSet", {{"args", {"Panel"}}, {"source", "PanelMesh"}, {"onto", "NewPanel"}, {"folder", tempDir("transfer").string()}});
    CHECK(r["texels"].get<size_t>() > 1000);
    CHECK(r["missed"].get<size_t>() == 0);
    CHECK(r["channels"].contains("roughness"));
    std::string err;
    const image::Image img = image::read(r["channels"]["roughness"].get<std::string>(), &err);
    REQUIRE(img.width > 0);
    CHECK(at(img, seamUv(0, -25.0, 0.0))[0] == doctest::Approx(0.25).epsilon(0.02));
}
