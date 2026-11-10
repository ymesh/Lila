#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "geo/FoldEdges.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "scene/AutoBevel.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Modifiers.h"
#include "scene/MeshNodes.h"
#include "uv/FinalMesh.h"
#include "uv/UvEdit.h"

#include <bit>
#include <filesystem>

using namespace lila;
using geo::Index;
using json = nlohmann::json;
using namespace testutil;

namespace
{
/** An L-shaped profile 1 cm thick with 10 cm legs, extruded 10 cm along z: a bent sheet with thickness. */
geo::Mesh lBracket()
{
    const glm::dvec2 profile[6] = {{0, 0}, {10, 0}, {10, 1}, {1, 1}, {1, 10}, {0, 10}};
    std::vector<glm::dvec3> pts;
    for (double z : {0.0, 10.0})
        for (const glm::dvec2& p : profile)
            pts.push_back({p.x, p.y, z});
    std::vector<Index> sizes, idx;
    // Caps: the back one is wound so its normal faces -z.
    sizes.push_back(6);
    for (Index i = 6; i-- > 0;)
        idx.push_back(i);
    sizes.push_back(6);
    for (Index i = 0; i < 6; ++i)
        idx.push_back(6 + i);
    for (Index i = 0; i < 6; ++i)
    {
        Index j = (i + 1) % 6;
        sizes.push_back(4);
        for (Index k : {i, j, 6 + j, 6 + i})
            idx.push_back(k);
    }
    return geo::Mesh::fromPolygons(pts, sizes, idx);
}

Index edgeAt(const geo::Mesh& m, glm::dvec3 a, glm::dvec3 b)
{
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e))
            continue;
        glm::dvec3 p = m.position(m.edgePoint(e, 0)), q = m.position(m.edgePoint(e, 1));
        if ((glm::length(p - a) < 1e-6 && glm::length(q - b) < 1e-6) || (glm::length(p - b) < 1e-6 && glm::length(q - a) < 1e-6))
            return e;
    }
    return geo::kInvalid;
}

/** The bracket with every face its own uv island in its own cell, laid flat along its dominant axis, so every edge is a seam. */
geo::Mesh islandBracket(std::vector<std::pair<glm::vec2, glm::vec2>>& islands)
{
    geo::Mesh m = lBracket();
    std::span<glm::vec2> uv = m.cornerAttrs().add("uv0", geo::AttrType::Float2).values<glm::vec2>();
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const glm::dvec3 n = glm::abs(m.faceNormal(f));
        const int drop = n.x >= n.y && n.x >= n.z ? 0 : n.y >= n.z ? 1 : 2;
        const glm::vec2 cell(float(f % 3) / 3.0f, float(f / 3) / 3.0f);
        glm::vec2 lo(1e9f), hi(-1e9f);
        for (Index c : m.faceCorners(f))
        {
            const glm::dvec3 q = m.position(m.cornerPoint(c)) / 40.0;
            uv[c] = cell + (drop == 0 ? glm::vec2(q.y, q.z) : drop == 1 ? glm::vec2(q.x, q.z) : glm::vec2(q.x, q.y));
            lo = glm::min(lo, uv[c]);
            hi = glm::max(hi, uv[c]);
        }
        islands.push_back({lo, hi});
    }
    return m;
}

/** Faces with a corner outside every island, and triangles squeezed under 2% of the uv area the mesh's density gives them. */
std::pair<size_t, size_t> uvFaults(const geo::Mesh& m, std::span<const std::pair<glm::vec2, glm::vec2>> islands)
{
    const std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
    size_t outside = 0, collapsed = 0;
    double uvAll = 0.0, worldAll = 0.0;
    std::vector<std::pair<double, double>> areas;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const std::vector<Index> cs = m.faceCorners(f);
        outside += std::ranges::none_of(islands, [&](const auto& isl) {
            return std::ranges::all_of(cs, [&](Index c) {
                return glm::all(glm::greaterThanEqual(uv[c], isl.first - 1e-5f)) && glm::all(glm::lessThanEqual(uv[c], isl.second + 1e-5f));
            });
        });
        for (size_t k = 1; k + 1 < cs.size(); ++k)
        {
            const glm::dvec2 a = uv[cs[0]], b = uv[cs[k]], c = uv[cs[k + 1]];
            const glm::dvec3 p = m.position(m.cornerPoint(cs[0])), q = m.position(m.cornerPoint(cs[k])), r = m.position(m.cornerPoint(cs[k + 1]));
            areas.push_back({0.5 * std::abs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)), 0.5 * glm::length(glm::cross(q - p, r - p))});
            uvAll += areas.back().first;
            worldAll += areas.back().second;
        }
    }
    for (const auto& [u, w] : areas)
        if (w > 1e-4 && u < 0.02 * w * uvAll / worldAll)
            ++collapsed;
    return {outside, collapsed};
}

/** Every edge of the mesh. */
std::vector<Index> allEdges(const geo::Mesh& m)
{
    std::vector<Index> out;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
            out.push_back(e);
    return out;
}

/** The long (z) edge of the bracket at profile point (x, y). */
Index longEdge(const geo::Mesh& m, double x, double y) { return edgeAt(m, {x, y, 0}, {x, y, 10}); }
} // namespace

TEST_CASE("fold edges on a bent sheet skip its thickness")
{
    geo::Mesh m = lBracket();
    REQUIRE(m.validate().empty());
    Index outer = longEdge(m, 0, 0), inner = longEdge(m, 1, 1);
    REQUIRE(outer != geo::kInvalid);
    REQUIRE(inner != geo::kInvalid);
    CHECK(geo::edgeIsConvex(m, outer));
    CHECK_FALSE(geo::edgeIsConvex(m, inner));
    CHECK(geo::edgeFoldAngle(m, outer) == doctest::Approx(90.0));

    geo::FoldEdgeOptions o;
    o.completeCorners = false;
    // Every edge is a 90 degree fold without the thickness filter.
    CHECK(geo::foldEdges(m, o).size() == 18);

    o.thin = 1.5;
    std::vector<Index> folds = geo::foldEdges(m, o);
    CHECK(folds == std::vector<Index>{std::min(outer, inner), std::max(outer, inner)});

    o.side = geo::FoldSide::Outside;
    CHECK(geo::foldEdges(m, o) == std::vector<Index>{outer});
    o.side = geo::FoldSide::Inside;
    CHECK(geo::foldEdges(m, o) == std::vector<Index>{inner});

    // The angle range and min length filter too.
    o.side = geo::FoldSide::Both;
    o.minAngle = 95.0;
    CHECK(geo::foldEdges(m, o).empty());
    o.minAngle = 30.0;
    o.minLength = 11.0;
    CHECK(geo::foldEdges(m, o).empty());

    // A bevel of just the folds keeps the mesh closed.
    geo::bevelEdges(m, folds, {.offset = 0.3, .segments = 2});
    m.compact();
    CHECK(m.validate().empty());
}

TEST_CASE("thin width measures across the face")
{
    geo::Mesh m = lBracket();
    Index e = longEdge(m, 10, 0);
    Index c = m.edgeCorner(e);
    double w0 = geo::faceWidthAt(m, m.cornerFace(c), e), w1 = geo::faceWidthAt(m, m.cornerFace(m.cornerRadial(c)), e);
    CHECK(std::min(w0, w1) == doctest::Approx(1.0));
    CHECK(std::max(w0, w1) == doctest::Approx(10.0));
}

TEST_CASE("fold corners get the loops that join them")
{
    // Ground grid 7 x 4 quads with two 2 x 2 blocks raised 1 cm, one ground quad apart.
    const int W = 7, H = 4;
    std::vector<glm::dvec3> pts;
    for (int y = 0; y <= H; ++y)
        for (int x = 0; x <= W; ++x)
            pts.push_back({double(x), double(y), 0.0});
    std::vector<Index> sizes, idx;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            sizes.push_back(4);
            for (int k : {y * (W + 1) + x, y * (W + 1) + x + 1, (y + 1) * (W + 1) + x + 1, (y + 1) * (W + 1) + x})
                idx.push_back(Index(k));
        }
    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    std::vector<Index> blocks;
    for (int y : {1, 2})
        for (int x : {1, 2, 4, 5})
            blocks.push_back(Index(y * W + x));
    geo::extrudeFaces(m, blocks, {.offset = 1.0});
    m.compact();
    REQUIRE(m.validate().empty());

    geo::FoldEdgeOptions o;
    o.completeCorners = false;
    std::vector<Index> plain = geo::foldEdges(m, o);
    CHECK(plain.size() == 40);

    o.completeCorners = true;
    std::vector<Index> full = geo::foldEdges(m, o);
    CHECK(full.size() == 42);
    Index joinLow = edgeAt(m, {3, 1, 0}, {4, 1, 0}), joinHigh = edgeAt(m, {3, 3, 0}, {4, 3, 0});
    CHECK(has(full, joinLow));
    CHECK(has(full, joinHigh));
    CHECK_FALSE(has(plain, joinLow));
    // Loops that run off the open border are dead ends.
    CHECK_FALSE(has(full, edgeAt(m, {3, 1, 0}, {3, 0, 0})));
}

TEST_CASE("edge matcher finds edges after the mesh is rebuilt")
{
    geo::Mesh m = lBracket();
    Index outer = longEdge(m, 0, 0);
    geo::EdgeRef ref = geo::edgeRef(m, outer);

    // Same ids, moved point: matched by id while the topology is unchanged.
    geo::Mesh moved = m;
    moved.setPosition(ref.p0, moved.position(ref.p0) + glm::dvec3(-1, 0, 0));
    geo::EdgeMatch how;
    CHECK(geo::EdgeMatcher(moved).find(ref, true, &how) == outer);
    CHECK(how == geo::EdgeMatch::Id);
    CHECK(geo::EdgeMatcher(moved).find(ref, false, &how) == geo::kInvalid);

    // Rebuilt with other ids: found by its endpoints.
    geo::Mesh other = m;
    geo::bevelEdges(other, std::vector<Index>{longEdge(other, 1, 1)}, {.offset = 0.2, .segments = 3});
    other.compact();
    Index want = longEdge(other, 0, 0);
    REQUIRE(want != geo::kInvalid);
    CHECK(geo::EdgeMatcher(other).find(ref, false, &how) == want);
    CHECK(how != geo::EdgeMatch::Lost);

    // Split in two: the collinear piece nearest the old midpoint.
    geo::Mesh split = m;
    split.splitEdge(longEdge(split, 0, 0), 0.3);
    Index e = geo::EdgeMatcher(split).find(ref, false, &how);
    CHECK(how == geo::EdgeMatch::Overlap);
    REQUIRE(e != geo::kInvalid);
    CHECK(split.edgeLength(e) == doctest::Approx(7.0));
}

namespace
{
std::shared_ptr<const geo::Mesh> inputOf(const std::string& mod)
{
    auto set = scene::autoBevel::chosen(scene::Graph::current(), scene::Graph::current().find(mod)->id());
    REQUIRE(set);
    return set->mesh;
}

/** Edit Edges on a modifier, picking exactly the edge between a and b on its input. */
void pickEdge(const std::string& cube, const std::string& mod, glm::dvec3 a, glm::dvec3 b)
{
    run("bevelEditEdges", {{"args", {mod}}});
    Index e = edgeAt(*inputOf(mod), a, b);
    REQUIRE(e != geo::kInvalid);
    run("selectComponents", {{"args", {cube}}, {"edges", {e}}});
    run("bevelApplyEdges", {{"args", {mod}}});
}

/** The bevelled edge of a one-edge modifier, as its endpoints. */
std::pair<glm::dvec3, glm::dvec3> pickedEdge(const std::string& mod)
{
    json info = run("bevelEdgeInfo", {{"args", {mod}}});
    REQUIRE(info["count"] == 1);
    CHECK(info["lost"] == 0);
    auto in = inputOf(mod);
    Index e = info["edges"][0].get<Index>();
    return {in->position(in->edgePoint(e, 0)), in->position(in->edgePoint(e, 1))};
}

bool sameEdge(std::pair<glm::dvec3, glm::dvec3> e, glm::dvec3 a, glm::dvec3 b)
{
    auto near = [](glm::dvec3 p, glm::dvec3 q) { return glm::length(p - q) < 1e-6; };
    return (near(e.first, a) && near(e.second, b)) || (near(e.first, b) && near(e.second, a));
}
} // namespace

TEST_CASE("bevel keeps every face's UVs inside its island across seams and at concave corners")
{
    std::vector<std::pair<glm::vec2, glm::vec2>> islands;
    geo::Mesh m = islandBracket(islands);
    geo::bevelEdges(m, allEdges(m), {.offset = 0.3, .segments = 2});
    const auto [outside, collapsed] = uvFaults(m, islands);
    CHECK(outside == 0);
    CHECK(collapsed == 0);
}

TEST_CASE("subdivision fits island interiors to the surface and keeps island corners")
{
    std::vector<std::pair<glm::vec2, glm::vec2>> islands;
    geo::Mesh m = islandBracket(islands);
    geo::bevelEdges(m, allEdges(m), {.offset = 0.3, .segments = 2});
    const geo::SubdivideOptions fit{.levels = 2, .uvs = geo::SubdivideUvs::FitSurface};
    // Interiors follow their island's border, so a solve against the export opens every sliver; blend's can't.
    uv::Stack stack;
    stack.push_back({"subdivide", [&](const geo::Mesh& in) { return geo::subdivide(in, fit); }, true});
    const uv::FinalRefineResult r = uv::refineFinal(m, {}, "uv0", stack);
    CHECK(r.method == uv::FinalMethod::Probed);
    CHECK(uvFaults(geo::subdivide(m, fit), islands).second == 0);

    // A flat grid's inner uvs land where linear interpolation puts them, and its corners stay where they were.
    geo::PlaneOptions po;
    po.subdivX = po.subdivZ = 4;
    const geo::Mesh plane = geo::makePlane(po);
    const geo::Mesh fine = geo::subdivide(plane, {.levels = 2, .sharpBoundary = true, .uvs = geo::SubdivideUvs::FitSurface});
    const std::span<const glm::vec2> uv = fine.cornerAttrs().get<glm::vec2>("uv0");
    const glm::dvec3 lo(-po.width / 2.0, 0.0, -po.depth / 2.0);
    size_t off = 0, corners = 0;
    for (Index c = 0; c < fine.cornerCapacity(); ++c)
    {
        const glm::dvec3 p = fine.position(fine.cornerPoint(c));
        const glm::dvec2 t((p.x - lo.x) / po.width, (p.z - lo.z) / po.depth);
        const bool corner = (t.x < 1e-9 || t.x > 1.0 - 1e-9) && (t.y < 1e-9 || t.y > 1.0 - 1e-9);
        corners += corner;
        const glm::vec2 u = uv[c];
        // The plane maps x and z linearly to u and v (either way round), so a smooth result is linear in position.
        const double du = std::min(std::abs(u.x - t.x), std::abs(u.x - (1.0 - t.x))) + std::min(std::abs(u.y - t.y), std::abs(u.y - (1.0 - t.y)));
        const double dv = std::min(std::abs(u.x - t.y), std::abs(u.x - (1.0 - t.y))) + std::min(std::abs(u.y - t.x), std::abs(u.y - (1.0 - t.x)));
        off += std::min(du, dv) > 1e-5;
    }
    CHECK(corners == 4);
    CHECK(off == 0);
}

TEST_CASE("blend subdivision uvs stay as they were")
{
    std::vector<std::pair<glm::vec2, glm::vec2>> islands;
    // Also with Fit Surface's spacing layers riding through the bevel, as a stack that feeds a subdivision carries them.
    for (bool spacing : {false, true})
    {
        geo::Mesh m = islandBracket(islands);
        if (spacing)
            geo::addFitSpacing(m);
        geo::bevelEdges(m, allEdges(m), {.offset = 0.3, .segments = 2});
        const geo::Mesh smooth = geo::subdivide(m, {.levels = 2, .uvs = geo::SubdivideUvs::Blend});
        CHECK(!geo::hasFitSpacing(smooth));
        // FNV-1a of every corner's uv bits: the output of the blend rules before Fit Surface existed.
        uint64_t h = 0xcbf29ce484222325ull;
        const std::span<const glm::vec2> uv = smooth.cornerAttrs().get<glm::vec2>("uv0");
        for (Index c = 0; c < smooth.cornerCapacity(); ++c)
            for (float f : {uv[c].x, uv[c].y})
                for (int k = 0; k < 4; ++k)
                {
                    h ^= (std::bit_cast<uint32_t>(f) >> (8 * k)) & 0xffu;
                    h *= 0x100000001b3ull;
                }
        CHECK(h == 10769423284830584747ull);
    }
}

TEST_CASE("autoBevel modifier bevels folds and stays live")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    std::string mod = run("autoBevel", {{"args", {cube}}, {"segments", 1}, {"offset", 5.0}})[0];
    CHECK(run("bevelEdgeInfo", {{"args", {mod}}})["count"] == 12);
    // A one-segment bevel of every cube edge: 6 faces, 12 strips, 8 corners.
    CHECK(faces(cube) == 26);
    run("setAttr", {{"args", {mod + ".minAngle", 100.0}}});
    CHECK(faces(cube) == 6);
    run("setAttr", {{"args", {mod + ".mode", "angle"}}});
    run("setAttr", {{"args", {mod + ".minAngle", 30.0}}});
    CHECK(faces(cube) == 26);
    run("setAttr", {{"args", {mod + ".mode", "weight"}}});
    CHECK(faces(cube) == 6);
}

TEST_CASE("Edit Edges stores overrides that survive upstream changes")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    const glm::dvec3 a0(-50, 50, 50), a1(50, 50, 50), b0(-50, -50, -50), b1(50, -50, -50);
    std::string first = run("autoBevel", {{"args", {cube}}, {"mode", "manual"}, {"segments", 1}, {"offset", 5.0}})[0];
    std::string second = run("autoBevel", {{"args", {cube}}, {"mode", "manual"}, {"segments", 2}, {"offset", 5.0}})[0];
    pickEdge(cube, first, a0, a1);
    pickEdge(cube, second, b0, b1);
    CHECK(sameEdge(pickedEdge(first), a0, a1));
    CHECK(sameEdge(pickedEdge(second), b0, b1));

    // Re-bevelling upstream renumbers the second modifier's input, but it keeps its edge.
    for (int segments : {3, 1, 4})
    {
        run("setAttr", {{"args", {first + ".segments", segments}}});
        CHECK(sameEdge(pickedEdge(second), b0, b1));
    }
    run("setAttr", {{"args", {first + ".offset", 12.0}}});
    CHECK(sameEdge(pickedEdge(second), b0, b1));

    // A direct edit under the stack compacts the base; the first modifier follows its edge to the new ids.
    run("selectComponents", {{"args", {cube}}, {"faces", {2}}});
    run("polyExtrude", {{"thickness", 10}});
    auto e = pickedEdge(first);
    CHECK(glm::length(e.first - e.second) == doctest::Approx(100.0));

    // Undo walks back one step at a time.
    run("undo");
    CHECK(sameEdge(pickedEdge(first), a0, a1));
}

TEST_CASE("Edit Edges shows the input, cancels, undoes and saves")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    std::string mod = run("autoBevel", {{"args", {cube}}, {"segments", 1}})[0];
    run("addModifier", {{"args", {cube}}, {"kind", "subdivision"}});
    int full = faces(cube);

    run("bevelEditEdges", {{"args", {mod}}});
    // This modifier and the subdivision after it pass through for display.
    CHECK(faces(cube) == 6);
    CHECK(scene::components::selected(scene::Graph::current().find(cube)->id(), scene::ComponentMode::Edge).size() == 12);
    CHECK_THROWS_AS(run("polyExtrude", {{"thickness", 1}}), CommandError);

    auto dir = std::filesystem::temp_directory_path() / "lila_tests";
    auto path = (dir / "autobevel_edit.lilaa").string();
    run("saveScene", {{"args", {path}}});
    run("newScene");
    run("openScene", {{"args", {path}}});
    CHECK(run("bevelEdgeInfo", {{"args", {mod}}})["editing"] == true);
    CHECK(faces(cube) == 6);

    run("bevelCancelEdges", {{"args", {mod}}});
    CHECK(faces(cube) == full);
    run("undo");
    CHECK(faces(cube) == 6);
    run("redo");

    // Deselect two edges and apply: plain auto becomes auto with overrides.
    run("bevelEditEdges", {{"args", {mod}}});
    run("selectComponents", {{"args", {cube}}, {"edges", {0, 1}}, {"deselect", true}});
    json r = run("bevelApplyEdges", {{"args", {mod}}});
    CHECK(r["removed"] == 2);
    CHECK(run("getAttr", {{"args", {mod + ".mode"}}}) == 1);
    CHECK(run("bevelEdgeInfo", {{"args", {mod}}})["count"] == 10);
    CHECK(run("bevelEdgeInfo", {{"args", {mod}}})["editing"] == false);
    run("undo");
    CHECK(run("bevelEdgeInfo", {{"args", {mod}}})["editing"] == true);
    run("bevelCancelEdges", {{"args", {mod}}});
}

TEST_CASE("a stack feeding a subdivision carries Fit Surface spacing from its cage, and never hands it on")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    std::string bevel = run("autoBevel", {{"args", {cube}}, {"segments", 2}})[0];
    std::string sub = run("addModifier", {{"args", {cube}}, {"kind", "subdivision"}});
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    auto* mod = scene::modifiers::asModifier(g.find(bevel));
    REQUIRE(mod);
    // The bevel's output is the subdivision's input: bunched by the bevel like the uvs.
    REQUIRE(mod->as<scene::MeshPtr>(mod->outAttr));
    CHECK(geo::hasFitSpacing(*mod->as<scene::MeshPtr>(mod->outAttr)));
    auto final = [&] { g.evaluate(); return g.find(cube)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh); };
    CHECK(!geo::hasFitSpacing(*final()));
    run("setAttr", {{"args", {sub + ".enabled", false}}});
    CHECK(!geo::hasFitSpacing(*final()));
    run("setAttr", {{"args", {sub + ".enabled", true}}});
    run("setAttr", {{"args", {sub + ".uvs", 0}}});
    CHECK(!geo::hasFitSpacing(*final()));
    // The uv tools' stack sees the same: spacing added at its start, gone from its output.
    const uv::Stack stack = uv::exportStack(*g.find(cube));
    CHECK(std::ranges::any_of(stack, &uv::StackStep::subdivides));
    CHECK(!geo::hasFitSpacing(uv::runStack(*scene::modifiers::cage(g, g.find(cube)->id()), stack)));
}

TEST_CASE("selectFoldEdges and measure")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"subdivisionsX", 2}})[0];
    // The four edges along x are split in two; the loop around the middle is flat.
    CHECK(run("selectFoldEdges", {{"args", {cube}}}) == 16);
    std::string mod = run("autoBevel", {{"args", {cube}}})[0];
    run("selectComponents", {{"args", {cube}}, {"edges", {0}}});
    double thin = run("bevelMeasureThin", {{"args", {mod}}}).get<double>();
    CHECK(thin > 0.0);
    CHECK(run("getAttr", {{"args", {mod + ".thin"}}}).get<double>() == doctest::Approx(thin));
}

TEST_CASE("polyBevelWeight feeds weight mode and polyCrease keeps subdivided edges sharp")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("selectComponents", {{"args", {cube}}, {"edges", {0}}});
    run("polyBevelWeight");
    std::string mod = run("autoBevel", {{"args", {cube}}, {"segments", 1}, {"offset", 5.0}, {"mode", "weight"}})[0];
    CHECK(run("bevelEdgeInfo", {{"args", {mod}}})["count"] == 1);

    std::string box = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("select", {{"args", {box}}});
    run("selectMode", {{"args", {"edge"}}});
    run("selectComponents", {{"args", {box}}, {"edges", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}}});
    run("polyCrease");
    run("polySubdivide", {{"args", {box}}, {"levels", 2}});
    double most = 0.0;
    const json data = run("meshData", {{"args", {box}}});
    for (const json& p : data["points"])
        most = std::max(most, p[0].get<double>());
    CHECK(most == doctest::Approx(50.0));
}
