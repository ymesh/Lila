// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Several scalps on one groom (scalp_pieces.h): joining them, cutting each one's own mesh out of the
// joined one (refined too), a Scatter per scalp merged into one output with every root on the joined
// mesh, the groom's own scalp's strands as without the others, groom files, vertex maps painted
// before a scalp was added, and a Scalp node naming a scalp the groom does not have.

#include <gtest/gtest.h>

#include <cmath>
#include <set>

#include "groomlab/groom.h"
#include "groomlab/groom_file.h"
#include "groom/GroomEvaluator.h"
#include "groom/GroomStrokeHost.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/scalp_history.h"
#include "groomlab/scalp_maps.h"
#include "groomlab/scalp_pieces.h"
#include "groomlab/stroke.h"
#include "groomlab/subdivision.h"
#include "test_helpers.h"

using lila::groom::GroomEvaluator;

namespace groomlab {
namespace {

constexpr double kFrameSeconds = 1.0 / 24.0;

// A plane moved to `at` (an ear beside a head), with UVs and a color set when asked.
MeshPtr movedPlane(Vec3f at, int cells = 4, float size = 1.0f, bool uvs = false, const char* colorSet = nullptr) {
    const MeshPtr plane = test::makePlane(cells, size);
    std::vector<Vec3f> points = plane->points();
    for (Vec3f& p : points) p = p + at;
    auto mesh = std::make_shared<ScalpMesh>(std::move(points), plane->triangles());
    if (uvs) {
        std::vector<ScalpMesh::UV> corners;
        for (const ScalpMesh::Triangle& t : mesh->triangles()) {
            for (const std::int32_t v : t) corners.push_back({plane->points()[v].x / size, plane->points()[v].z / size});
        }
        mesh->setCornerUVs(std::move(corners));
    }
    if (colorSet) mesh->setCornerColors(colorSet, std::vector<ScalpMesh::Color>(mesh->triangleCount() * 3, {0.5f, 0.5f, 0.5f, 1.0f}));
    return mesh;
}

// Scatter on the groom's own scalp, and with `ears` a second Scatter on the scalp "ears", merged.
std::string branchesGraph(bool ears, const std::string& subdivision = "off", const std::string& earsName = "ears") {
    std::string nodes = R"({"name": "scalp", "type": "Scalp", "params": {"subdivision": ")" + subdivision + R"("}},
    {"name": "scatter", "type": "Scatter", "params": {"density": 400.0, "length": 0.2}},
    {"name": "output", "type": "Output"})";
    std::string connections = R"({"from": "scalp", "to": "scatter", "input": "scalp"})";
    std::string last = "scatter";
    if (ears) {
        nodes += R"(,
    {"name": "earScalp", "type": "Scalp", "params": {"input": ")" + earsName + R"(", "subdivision": ")" + subdivision + R"("}},
    {"name": "earScatter", "type": "Scatter", "params": {"density": 300.0, "length": 0.1, "seed": 7}},
    {"name": "merge", "type": "Merge"})";
        connections += R"(,
    {"from": "earScalp", "to": "earScatter", "input": "scalp"},
    {"from": "scatter", "to": "merge", "input": "a"},
    {"from": "earScatter", "to": "merge", "input": "b"})";
        last = "merge";
    }
    connections += R"(,
    {"from": ")" + last + R"(", "to": "output", "input": "strands"})";
    return R"({"version": 1, "nodes": [)" + nodes + R"(], "connections": [)" + connections + R"(], "output": "output"})";
}

TEST(ScalpPieces, OneScalpJoinsToItself) {
    const MeshPtr head = test::makeSphere(8, 16);
    EXPECT_EQ(combineScalps(head, {}), head);
    EXPECT_EQ(combineScalps(head, {{"ears", nullptr}}), head);
    EXPECT_EQ(combineScalps(head, {{"scalp", movedPlane({3, 0, 0})}}), head);  // the groom's own name
    EXPECT_EQ(combineScalps(head, {{"1ears", movedPlane({3, 0, 0})}}), head);  // not a name
    EXPECT_TRUE(scalpPieces(head).empty());
}

TEST(ScalpPieces, JoinedMeshKeepsTheOwnScalpFirst) {
    const MeshPtr head = movedPlane({0, 0, 0}, 4, 1.0f, true, "density");
    const MeshPtr ears = movedPlane({3, 0, 0}, 2);
    const MeshPtr joined = combineScalps(head, {{"ears", ears}, {"ears", movedPlane({6, 0, 0})}});
    ASSERT_NE(joined, head);
    ASSERT_EQ(joined->pieces().size(), 2u);  // the second "ears" is left out
    EXPECT_EQ(joined->pointCount(), head->pointCount() + ears->pointCount());
    EXPECT_EQ(joined->triangleCount(), head->triangleCount() + ears->triangleCount());
    const ScalpPiece* own = joined->piece("scalp");
    const ScalpPiece* other = joined->piece("ears");
    ASSERT_TRUE(own && other);
    EXPECT_EQ(own->firstTriangle, 0u);
    EXPECT_EQ(other->firstTriangle, head->triangleCount());
    EXPECT_EQ(other->firstPoint, head->pointCount());
    for (std::size_t t = 0; t < head->triangleCount(); ++t) EXPECT_EQ(joined->triangles()[t], head->triangles()[t]);
    // UVs and color sets where one scalp has them: the other gets zeros (alpha 1).
    ASSERT_TRUE(joined->hasUVs());
    EXPECT_EQ(joined->cornerUVs().size(), joined->triangleCount() * 3);
    ASSERT_TRUE(joined->hasColorSet("density"));
    const StrandRoot onEars{static_cast<std::int32_t>(other->firstTriangle), 0.2f, 0.2f};
    EXPECT_EQ(joined->colorAt("density", onEars)[0], 0.0f);
    EXPECT_EQ(joined->colorAt("density", {0, 0.2f, 0.2f})[0], 0.5f);
    // The pieces cut back out: each its own scalp, roots landing on the joined mesh.
    const auto pieces = scalpPieces(joined);
    ASSERT_EQ(pieces.size(), 2u);
    const MeshPtr earPiece = pieces.at("ears");
    EXPECT_TRUE(earPiece->isPiece());
    EXPECT_EQ(earPiece->whole(), joined);
    EXPECT_EQ(earPiece->triangleCount(), ears->triangleCount());
    EXPECT_EQ(earPiece->points(), ears->points());
    const StrandRoot local{1, 0.3f, 0.3f};
    const StrandRoot global = toWholeRoot(*earPiece, local);
    EXPECT_EQ(global.face, static_cast<std::int32_t>(other->firstTriangle) + 1);
    EXPECT_EQ(joined->position(global), earPiece->position(local));
    EXPECT_EQ(toPieceRoot(*earPiece, global).face, 1);
    EXPECT_EQ(toPieceRoot(*earPiece, StrandRoot{0, 0, 0}).face, -1);  // on the groom's own scalp
    EXPECT_EQ(wholeScalp(earPiece), joined);
    EXPECT_EQ(scalpPieces(joined).at("ears"), earPiece);  // cut once per mesh
}

TEST(ScalpPieces, RefinedPiecesAreTheJoinedMeshCut) {
    const MeshPtr head = test::makeSphere(6, 12);
    const MeshPtr ears = movedPlane({3, 0, 0}, 3);
    const MeshPtr joined = combineScalps(head, {{"ears", ears}});
    for (int levels = 1; levels <= 2; ++levels) {
        const MeshPtr refined = subdivideScalp(joined, levels);
        const auto pieces = scalpPieces(refined);
        ASSERT_EQ(pieces.size(), 2u);
        const MeshPtr earPiece = pieces.at("ears");
        const MeshPtr alone = subdivideScalp(ears, levels);
        ASSERT_EQ(earPiece->triangleCount(), alone->triangleCount());
        EXPECT_EQ(earPiece->subdivisionLevel(), levels);
        ASSERT_TRUE(earPiece->cage());
        EXPECT_TRUE(refinementPlan(*earPiece->cage(), *earPiece));
        // Each triangle where the ears refined alone put it, and where the joined mesh has it.
        for (std::size_t t = 0; t < earPiece->triangleCount(); ++t) {
            const StrandRoot r{static_cast<std::int32_t>(t), 0.25f, 0.25f};
            EXPECT_LT((earPiece->position(r) - alone->position(r)).length(), 1e-5f);
            EXPECT_EQ(refined->position(toWholeRoot(*earPiece, r)), earPiece->position(r));
        }
    }
}

TEST(ScalpPieces, ScatterPerScalpMergesOnTheJoinedMesh) {
    const MeshPtr head = test::makeSphere(8, 16);
    const MeshPtr ears = movedPlane({3, 0, 0});
    GroomData alone;
    alone.scalp = head;
    alone.graphJson = branchesGraph(false);
    const StrandsPtr own = GroomEvaluator(alone).evaluate(1.0f, 0.0);
    ASSERT_GT(own->strandCount(), 0u);

    GroomData data;
    data.scalp = head;
    data.scalps = {{"ears", ears}};
    data.graphJson = branchesGraph(true);
    GroomEvaluator groom(data);
    const StrandsPtr both = groom.evaluate(1.0f, 0.0);
    const MeshPtr joined = groom.scalp();
    ASSERT_EQ(joined->pieces().size(), 2u);
    const std::size_t headTriangles = head->triangleCount();
    std::size_t onHead = 0, onEars = 0;
    for (std::size_t s = 0; s < both->strandCount(); ++s) {
        const StrandRoot& root = both->roots()[s];
        ASSERT_TRUE(joined->contains(root));
        // Each strand grows from its root on the joined mesh.
        EXPECT_LT((both->strandPoints(s)[0] - joined->position(root)).length(), 1e-5f);
        if (static_cast<std::size_t>(root.face) < headTriangles) {
            ++onHead;
        } else {
            ++onEars;
            EXPECT_NEAR(both->strandPoints(s)[0].y, 0.0f, 1e-5f);  // on the ears' plane
            EXPECT_GE(both->strandPoints(s)[0].x, 3.0f - 1e-5f);
        }
    }
    EXPECT_GT(onEars, 100u);
    // The groom's own scalp's strands are the ones it grows without the ears, to the bit.
    ASSERT_EQ(onHead, own->strandCount());
    for (std::size_t s = 0; s < own->strandCount(); ++s) {
        EXPECT_EQ(both->ids()[s], own->ids()[s]);
        EXPECT_EQ(both->roots()[s].face, own->roots()[s].face);
        for (std::size_t k = 0; k < own->strandPointCount(s); ++k) EXPECT_EQ(both->strandPoints(s)[k], own->strandPoints(s)[k]);
    }
}

TEST(ScalpPieces, SubdivisionMatchRenderTakesEachScalpsLevel) {
    const MeshPtr head = test::makeSphere(8, 16);
    auto ears = std::make_shared<ScalpMesh>(*movedPlane({3, 0, 0}, 3));
    ears->setRenderSubdivision(2, "test");
    GroomData data;
    data.scalp = head;  // renders without subdivision
    data.scalps = {{"ears", ears}};
    data.graphJson = branchesGraph(true, "matchRender");
    GroomEvaluator groom(data);
    EXPECT_EQ(groom.scalpSubdivision(), 2);  // the ears' Scalp node asks for theirs
    const StrandsPtr strands = groom.evaluate(1.0f, 0.0);
    const MeshPtr joined = groom.scalp();
    EXPECT_EQ(joined->subdivisionLevel(), 2);
    std::size_t onEars = 0;
    for (std::size_t s = 0; s < strands->strandCount(); ++s) {
        const StrandRoot& root = strands->roots()[s];
        ASSERT_TRUE(joined->contains(root));
        EXPECT_LT((strands->strandPoints(s)[0] - joined->position(root)).length(), 1e-5f);
        onEars += strands->strandPoints(s)[0].x >= 3.0f - 1e-4f;
    }
    EXPECT_GT(onEars, 50u);
}

TEST(ScalpPieces, DeformingScalpsKeepTheirStrands) {
    const MeshPtr head = test::makeSphere(8, 16);
    const MeshPtr ears = movedPlane({3, 0, 0});
    GroomData data;
    data.scalp = head;
    data.scalps = {{"ears", ears}};
    data.graphJson = branchesGraph(true);
    GroomEvaluator groom(data);
    const StrandsPtr first = groom.evaluate(1.0f, 0.0);
    // The next frame: every point moved up (a rest pose keeps the placement).
    auto moved = [](const MeshPtr& m) {
        std::vector<Vec3f> points = m->points();
        for (Vec3f& p : points) p.y += 0.5f;
        auto out = std::make_shared<ScalpMesh>(m->withPoints(points));
        out->setRestPoints(m->points());
        return MeshPtr(out);
    };
    groom.setScalp(moved(head));
    groom.setScalps({{"ears", moved(ears)}});
    const StrandsPtr next = groom.evaluate(1.0f, 0.0);
    ASSERT_EQ(next->strandCount(), first->strandCount());
    for (std::size_t s = 0; s < next->strandCount(); ++s) {
        EXPECT_EQ(next->ids()[s], first->ids()[s]);
        EXPECT_NEAR(next->strandPoints(s)[0].y - first->strandPoints(s)[0].y, 0.5f, 1e-4f);
    }
}

TEST(ScalpPieces, InterpolateOnAScalpsPieceReadsTheJoinedMesh) {
    const MeshPtr head = test::makeSphere(8, 16);
    const MeshPtr ears = movedPlane({3, 0, 0});
    const MeshPtr joined = combineScalps(head, {{"ears", ears}});
    // Guides on the ears leaning along +x, rooted on the joined mesh as a host roots them.
    auto guides = std::make_shared<GuideSet>();
    std::uint64_t id = 1;
    for (float x : {3.2f, 3.5f, 3.8f}) {
        for (float z : {0.2f, 0.5f, 0.8f}) guides->add(id++, test::straightCurve({x, 0, z}, Vec3f(1, 1, 0).normalized() * 0.3f));
    }
    guides->rootOnMesh(*joined);
    GroomData data;
    data.scalp = head;
    data.scalps = {{"ears", ears}};
    data.guides = guides;
    data.graphJson = R"({"version": 1, "nodes": [
    {"name": "earScalp", "type": "Scalp", "params": {"input": "ears"}},
    {"name": "guides", "type": "Guides"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 200.0}},
    {"name": "interpolate", "type": "Interpolate"},
    {"name": "output", "type": "Output"}],
    "connections": [
    {"from": "earScalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interpolate", "input": "strands"},
    {"from": "guides", "to": "interpolate", "input": "guides"},
    {"from": "earScalp", "to": "interpolate", "input": "scalp"},
    {"from": "interpolate", "to": "output", "input": "strands"}], "output": "output"})";
    GroomEvaluator groom(data);
    const StrandsPtr strands = groom.evaluate(1.0f, 0.0);
    ASSERT_GT(strands->strandCount(), 50u);
    for (std::size_t s = 0; s < strands->strandCount(); ++s) {
        const std::size_t n = strands->strandPointCount(s);
        const Vec3f along = strands->strandPoints(s)[n - 1] - strands->strandPoints(s)[0];
        EXPECT_GT(along.x, 0.1f);  // leaning as the guides do
        EXPECT_GE(static_cast<std::size_t>(strands->roots()[s].face), head->triangleCount());
    }
}

TEST(ScalpPieces, FollowingTheScalpsCarriesEveryStrand) {
    const MeshPtr head = test::makeSphere(8, 16);
    const MeshPtr ears = movedPlane({3, 0, 0});
    auto moved = [](const MeshPtr& m) {
        std::vector<Vec3f> points = m->points();
        for (Vec3f& p : points) p.x += 1.0f;
        auto out = std::make_shared<ScalpMesh>(m->withPoints(points));
        out->setRestPoints(m->points());
        return MeshPtr(out);
    };
    GroomData data;
    data.scalp = head;
    data.scalps = {{"ears", ears}};
    data.graphJson = branchesGraph(true);
    const StrandsPtr rest = GroomEvaluator(data).evaluate(1.0f, 0.0);
    data.scalp = moved(head);
    data.scalps = {{"ears", moved(ears)}};
    data.followScalp = true;
    GroomEvaluator groom(data);
    const StrandsPtr carried = groom.evaluate(1.0f, 0.0);
    ASSERT_EQ(carried->strandCount(), rest->strandCount());
    for (std::size_t s = 0; s < carried->strandCount(); ++s) {
        EXPECT_NEAR(carried->strandPoints(s)[0].x - rest->strandPoints(s)[0].x, 1.0f, 1e-4f);
    }
}

TEST(ScalpPieces, GroomFileKeepsTheScalps) {
    GroomFile file;
    file.data.scalp = test::makeSphere(8, 16);
    file.data.scalps = {{"ears", movedPlane({3, 0, 0}, 4, 1.0f, true, "tint")}, {"brows", movedPlane({0, 3, 0}, 2)}};
    file.data.graphJson = branchesGraph(true);
    const GroomFile back = groomFileFromJson(groomFileToJson(file));
    ASSERT_EQ(back.data.scalps.size(), 2u);
    EXPECT_EQ(back.data.scalps[0].first, "ears");
    EXPECT_EQ(back.data.scalps[1].first, "brows");
    EXPECT_EQ(back.data.scalps[0].second->contentHash(), file.data.scalps[0].second->contentHash());
    EXPECT_EQ(back.data.scalps[1].second->contentHash(), file.data.scalps[1].second->contentHash());
    const StrandsPtr a = GroomEvaluator(file.data).evaluate(1.0f, 0.0);
    const StrandsPtr b = GroomEvaluator(back.data).evaluate(1.0f, 0.0);
    EXPECT_EQ(a->contentHash(), b->contentHash());
    // Turned to Z up and back: the same scalps.
    GroomFile turned = back;
    convertUpAxis(turned, "Z");
    EXPECT_NE(turned.data.scalps[0].second->points().back(), back.data.scalps[0].second->points().back());
    convertUpAxis(turned, "Y");
    EXPECT_EQ(turned.data.scalps[0].second->points(), back.data.scalps[0].second->points());
}

TEST(ScalpPieces, VertexMapPaintedBeforeAScalpWasAdded) {
    const MeshPtr head = movedPlane({0, 0, 0});
    const MeshPtr joined = combineScalps(head, {{"ears", movedPlane({3, 0, 0}, 2)}});
    ScalpMaps maps;
    maps.maps["density"] = {"vertices", std::vector<float>(head->pointCount(), 0.75f), ""};
    std::vector<std::string> errors;
    const MeshPtr mapped = withVertexMaps(joined, maps, "", nullptr, &errors, 1);
    EXPECT_TRUE(errors.empty());
    ASSERT_TRUE(mapped->hasColorSet("density"));
    EXPECT_FLOAT_EQ(mapped->colorAt("density", {0, 0.2f, 0.2f})[0], 0.75f);
    const StrandRoot onEars{static_cast<std::int32_t>(head->triangleCount()), 0.2f, 0.2f};
    EXPECT_FLOAT_EQ(mapped->colorAt("density", onEars)[0], 0.0f);
}

TEST(ScalpPieces, ScalpNodeNamingNoScalp) {
    GroomData data;
    data.scalp = test::makeSphere(8, 16);
    data.scalps = {{"ears", movedPlane({3, 0, 0})}};
    data.graphJson = branchesGraph(true, "off", "tail");
    GroomEvaluator groom(data);
    const GroomEvaluator::Result& view = groom.evaluate(GroomEvaluator::Request{});
    ASSERT_TRUE(view.strands);
    for (std::size_t s = 0; s < view.strands->strandCount(); ++s) {
        EXPECT_LT(static_cast<std::size_t>(view.strands->roots()[s].face), data.scalp->triangleCount());
    }
    bool said = false;
    for (const auto& [node, note] : groom.nodeNotes()) said = said || (node == "earScalp" && note.find("tail") != std::string::npos);
    EXPECT_TRUE(said);
}

// ---- Scripted strokes on the other scalps ------------------------------------------------------

TEST(ScalpPieces, AScriptedStrokeReachesAnOtherScalp) {
    // The scripted path (scriptedStroke through the session's host, as Python's StrokeHost drives
    // it) brushes on the scalps joined, as the interactive brush does: a guide placed on the ears
    // is rooted there.
    const MeshPtr head = test::makeSphere(8, 16);
    const MeshPtr ears = movedPlane({3, 0, 0});
    GroomData data;
    data.scalp = head;
    data.scalps = {{"ears", ears}};
    data.graphJson = branchesGraph(true);
    GroomEvaluator session(data);
    lila::groom::GroomStrokeHost host(session);
    BrushToolSettings s;
    s.setMode("place");
    s.place.matchNeighbors = false;
    const Vec3f at{3.4f, 0.0f, 0.6f};
    scriptedStroke(host, s, {at});
    const GuideHierarchy h = GuideHierarchy::fromJson(session.data().hierarchyJson);
    ASSERT_EQ(h.guides().size(), 1u);
    const HierarchyGuide& guide = h.guides().front();
    EXPECT_GE(static_cast<std::size_t>(guide.root.face), head->triangleCount());
    EXPECT_LT((session.scalp()->position(guide.root) - at).length(), 1e-4f);
}

// ---- Jiggle on the other scalps ----------------------------------------------------------------

TEST(ScalpPieces, HairOnAMovingOtherScalpJiggles) {
    const MeshPtr head = test::makeSphere(8, 16);
    const MeshPtr ears = movedPlane({3, 0, 0});
    std::string graph = branchesGraph(true);
    const std::string last = R"({"from": "merge", "to": "output", "input": "strands"})";
    ASSERT_NE(graph.find(last), std::string::npos);
    graph.replace(graph.find(last), last.size(),
                  R"({"from": "merge", "to": "jiggle", "input": "strands"}, {"from": "jiggle", "to": "output", "input": "strands"})");
    graph.replace(graph.find(R"({"name": "output")"), 0, R"({"name": "jiggle", "type": "Jiggle"}, )");
    GroomData data;
    data.scalp = head;
    data.scalps = {{"ears", ears}};
    data.graphJson = graph;
    GroomEvaluator groom(data);
    const int frames = groom.scalpHistoryFrames();
    ASSERT_GT(frames, 0);
    const StrandsPtr still = groom.evaluate(1.0f, 0.0);
    ASSERT_TRUE(still);
    const std::size_t headTriangles = head->triangleCount();
    // The head kept still; the ears came in along z over the last frames.
    auto pastEars = [&](int k) {
        std::vector<Vec3f> points = ears->points();
        for (Vec3f& p : points) p.z -= 0.05f * static_cast<float>(std::min(k, 4));
        return points;
    };
    std::vector<std::vector<Vec3f>> joinedPast, ownPast;
    for (int k = 1; k <= frames; ++k) {
        std::vector<Vec3f> all = head->points();
        const std::vector<Vec3f> e = pastEars(k);
        all.insert(all.end(), e.begin(), e.end());
        joinedPast.push_back(std::move(all));
        ownPast.push_back(head->points());
    }
    // Every scalp's own earlier frames: the ears' hair jiggles, the head's stays as it was.
    groom.setScalpHistory(ScalpHistory::make(kFrameSeconds, joinedPast));
    const StrandsPtr moving = groom.evaluate(1.0f, 0.0);
    ASSERT_EQ(moving->strandCount(), still->strandCount());
    std::size_t earsMoved = 0, earStrands = 0;
    for (std::size_t s = 0; s < still->strandCount(); ++s) {
        const bool onEars = static_cast<std::size_t>(still->roots()[s].face) >= headTriangles;
        const std::size_t n = still->strandPointCount(s);
        EXPECT_EQ(moving->strandPoints(s)[0], still->strandPoints(s)[0]);  // roots stay
        const float tip = (moving->strandPoints(s)[n - 1] - still->strandPoints(s)[n - 1]).length();
        if (onEars) {
            ++earStrands;
            earsMoved += tip > 1e-4f;
        } else {
            EXPECT_EQ(tip, 0.0f) << "a strand on the still head moved";
        }
    }
    ASSERT_GT(earStrands, 100u);
    EXPECT_EQ(earsMoved, earStrands);
    // The groom's own scalp's frames alone: the ears keep still (as a host reading only it gives).
    groom.setScalpHistory(ScalpHistory::make(kFrameSeconds, ownPast));
    const StrandsPtr headOnly = groom.evaluate(1.0f, 0.0);
    EXPECT_EQ(headOnly->positions(), still->positions());
}

// ---- Each scalp at its own subdivision level ---------------------------------------------------

// The ears' Scatter alone on `scalp` (a groom whose own scalp it is), as branchesGraph's ears.
std::string earsAloneGraph(const std::string& subdivision) {
    return R"({"version": 1, "nodes": [
    {"name": "earScalp", "type": "Scalp", "params": {"subdivision": ")" + subdivision + R"("}},
    {"name": "earScatter", "type": "Scatter", "params": {"density": 300.0, "length": 0.1, "seed": 7}},
    {"name": "output", "type": "Output"}],
    "connections": [{"from": "earScalp", "to": "earScatter", "input": "scalp"},
    {"from": "earScatter", "to": "output", "input": "strands"}], "output": "output"})";
}

TEST(ScalpPieces, PlanRefinesEachScalpAtItsOwnLevel) {
    const MeshPtr head = test::makeSphere(6, 12);
    const MeshPtr ears = movedPlane({3, 0, 0}, 3, 1.0f, true);
    const MeshPtr brows = movedPlane({0, 3, 0}, 2);
    const MeshPtr joined = combineScalps(head, {{"ears", ears}, {"brows", brows}});
    // The head as it is, the ears twice, the brows once (the most: 2).
    const MeshPtr levelled = withScalpLevels(joined, {{"scalp", 0}, {"ears", 2}, {"brows", 1}}, 2);
    ASSERT_NE(levelled, joined);
    EXPECT_EQ(levelled->piece("scalp")->subdivision, 0);
    EXPECT_EQ(levelled->piece("ears")->subdivision, -1);  // the whole's
    EXPECT_EQ(levelled->piece("brows")->subdivision, 1);
    EXPECT_EQ(withScalpLevels(levelled, {{"scalp", 0}, {"ears", 2}, {"brows", 1}}, 2), levelled);  // nothing new
    EXPECT_EQ(withScalpLevels(joined, {{"scalp", 1}, {"ears", 1}}, 1), joined);  // one level: one plan, as ever
    EXPECT_EQ(pieceSubdivisionLevels(*levelled, 2), (std::vector<int>{0, 2, 1}));
    EXPECT_TRUE(pieceSubdivisionLevels(*joined, 2).empty());

    const MeshPtr refined = subdivideScalp(levelled, 2);
    EXPECT_EQ(refined->subdivisionLevel(), 2);
    const MeshPtr earsRefined = subdivideScalp(ears, 2), browsRefined = subdivideScalp(brows, 1);
    ASSERT_EQ(refined->triangleCount(), head->triangleCount() + earsRefined->triangleCount() + browsRefined->triangleCount());
    ASSERT_EQ(refined->pointCount(), head->pointCount() + earsRefined->pointCount() + browsRefined->pointCount());
    ASSERT_TRUE(refined->hasUVs());
    const auto plan = refinementPlan(*levelled, *refined);
    ASSERT_TRUE(plan);
    // Each scalp's triangles are its own refinement's, in turn: the head's its own.
    auto expectSame = [&](const ScalpMesh& own, std::size_t at) {
        for (std::size_t t = 0; t < own.triangleCount(); ++t) {
            const StrandRoot r{static_cast<std::int32_t>(t), 0.25f, 0.3f};
            const StrandRoot w{static_cast<std::int32_t>(at + t), 0.25f, 0.3f};
            EXPECT_EQ(refined->position(w), own.position(r)) << t;
        }
    };
    expectSame(*head, 0);
    expectSame(*earsRefined, head->triangleCount());
    expectSame(*browsRefined, head->triangleCount() + earsRefined->triangleCount());
    // A root on the cage lands on its scalp's refinement and back.
    const auto earsPlan = SubdivisionPlan::of(*ears, 2);
    for (const StrandRoot onCage : {StrandRoot{3, 0.2f, 0.3f}, StrandRoot{static_cast<std::int32_t>(head->triangleCount()) + 4, 0.1f, 0.6f},
                                    StrandRoot{static_cast<std::int32_t>(joined->triangleCount()) - 1, 0.3f, 0.3f}}) {
        const StrandRoot on = plan->refinedRoot(onCage);
        ASSERT_TRUE(refined->contains(on));
        const StrandRoot back = plan->cageRoot(on);
        EXPECT_EQ(back.face, onCage.face);
        EXPECT_NEAR(back.u, onCage.u, 1e-5f);
        EXPECT_NEAR(back.v, onCage.v, 1e-5f);
    }
    EXPECT_EQ(plan->refinedRoot({3, 0.2f, 0.3f}).face, 3);  // the head is not refined
    const StrandRoot onEars = plan->refinedRoot({static_cast<std::int32_t>(head->triangleCount()) + 4, 0.1f, 0.6f});
    const StrandRoot alone = earsPlan->refinedRoot({4, 0.1f, 0.6f});
    EXPECT_EQ(onEars.face, alone.face + static_cast<std::int32_t>(head->triangleCount()));
    // The pieces: each at its own level.
    const auto pieces = scalpPieces(refined);
    ASSERT_EQ(pieces.size(), 3u);
    EXPECT_EQ(pieces.at("scalp")->subdivisionLevel(), 0);
    EXPECT_EQ(pieces.at("scalp")->triangleCount(), head->triangleCount());
    EXPECT_EQ(pieces.at("ears")->subdivisionLevel(), 2);
    EXPECT_EQ(pieces.at("ears")->points(), earsRefined->points());
    EXPECT_EQ(pieces.at("brows")->subdivisionLevel(), 1);
    EXPECT_EQ(pieces.at("brows")->triangleCount(), browsRefined->triangleCount());
    // Vertex values refined per scalp.
    std::vector<float> values(levelled->pointCount());
    for (std::size_t v = 0; v < values.size(); ++v) values[v] = levelled->points()[v].x;
    const std::vector<float> fine = plan->refineVertexValues(values, 1);
    ASSERT_EQ(fine.size(), refined->pointCount());
    for (std::size_t v = 0; v < fine.size(); ++v) EXPECT_NEAR(fine[v], refined->points()[v].x, 0.35f);
}

TEST(ScalpPieces, EachScalpRefinesAtItsOwnLevelInTheSession) {
    const MeshPtr head = test::makeSphere(8, 16);  // renders without subdivision
    auto ears = std::make_shared<ScalpMesh>(*movedPlane({3, 0, 0}, 3));
    ears->setRenderSubdivision(2, "test");
    const MeshPtr joined = combineScalps(head, {{"ears", ears}});
    // Maps painted on both scalps: a value per point of the scalps joined.
    ScalpMaps maps;
    std::vector<float> tint(joined->pointCount());
    for (std::size_t v = 0; v < tint.size(); ++v) tint[v] = 0.1f + 0.05f * static_cast<float>(v % 7);
    maps.maps["tint"] = {"vertices", tint, ""};
    // Guides on both, rooted on the joined mesh as a host roots them.
    GuideHierarchy h;
    const GroupId group = h.addGroup("primary");
    TubeProfile tube;
    tube.width = {0.2f, 0.1f};
    for (const Vec3f& at : {Vec3f{0, 1, 0}, Vec3f{0.6f, 0.8f, 0}, Vec3f{3.3f, 0, 0.4f}, Vec3f{3.7f, 0, 0.6f}}) {
        const Vec3f up = at.x > 2.0f ? Vec3f{0, 1, 0} : at.normalized();
        h.addGuide(group, test::straightCurve(at, up * 0.3f, 6), tube);
    }
    h.evaluate(joined.get());

    GroomData data;
    data.scalp = head;
    data.scalps = {{"ears", ears}};
    data.graphJson = branchesGraph(true, "matchRender");
    data.hierarchyJson = h.toJson();
    GroomEvaluator session(data);
    session.setScalpMaps(maps);
    EXPECT_EQ(session.scalpSubdivisions(), (std::map<std::string, int>{{"scalp", 0}, {"ears", 2}}));
    EXPECT_EQ(session.scalpSubdivision(), 2);
    const GroomEvaluator::Result& view = session.evaluate(GroomEvaluator::Request{});
    ASSERT_TRUE(view.strands) << (view.errors.empty() ? "" : view.errors.front());
    const MeshPtr scalp = view.scalp;
    const MeshPtr earsRefined = subdivideScalp(ears, 2);
    // The head as it is, then the ears refined twice.
    ASSERT_EQ(scalp->triangleCount(), head->triangleCount() + earsRefined->triangleCount());
    for (std::size_t v = 0; v < head->pointCount(); ++v) EXPECT_EQ(scalp->points()[v], head->points()[v]);
    const std::size_t headTriangles = head->triangleCount();

    // The head's strands are the ones it grows alone (matchRender: not refined), to the bit.
    GroomData headAlone;
    headAlone.scalp = head;
    headAlone.graphJson = branchesGraph(false, "matchRender");
    const StrandsPtr own = GroomEvaluator(headAlone).evaluate(1.0f, 0.0);
    std::vector<std::size_t> onHead, onEars;
    for (std::size_t s = 0; s < view.strands->strandCount(); ++s) {
        const StrandRoot& root = view.strands->roots()[s];
        ASSERT_TRUE(scalp->contains(root));
        EXPECT_LT((view.strands->strandPoints(s)[0] - scalp->position(root)).length(), 1e-5f);
        (static_cast<std::size_t>(root.face) < headTriangles ? onHead : onEars).push_back(s);
    }
    ASSERT_EQ(onHead.size(), own->strandCount());
    for (std::size_t i = 0; i < onHead.size(); ++i) {
        const std::size_t s = onHead[i];
        EXPECT_EQ(view.strands->ids()[s], own->ids()[i]);
        EXPECT_EQ(view.strands->roots()[s].face, own->roots()[i].face);
        EXPECT_EQ(view.strands->strandPoints(s)[0], own->strandPoints(i)[0]);
    }
    // The ears' strands sit where the ears alone (refined at their level) put theirs.
    GroomData earsAlone;
    earsAlone.scalp = ears;
    earsAlone.graphJson = earsAloneGraph("matchRender");
    const StrandsPtr theirs = GroomEvaluator(earsAlone).evaluate(1.0f, 0.0);
    ASSERT_EQ(onEars.size(), theirs->strandCount());
    ASSERT_GT(onEars.size(), 50u);
    for (std::size_t i = 0; i < onEars.size(); ++i) {
        const std::size_t s = onEars[i];
        EXPECT_EQ(view.strands->roots()[s].face, theirs->roots()[i].face + static_cast<std::int32_t>(headTriangles));
        EXPECT_LT((view.strands->strandPoints(s)[0] - theirs->strandPoints(i)[0]).length(), 1e-5f);
    }
    // The maps on the refined scalp read as on each scalp's own: the head's corners as painted,
    // the ears' refined as the ears alone refine them.
    ASSERT_TRUE(scalp->hasColorSet("tint"));
    for (std::size_t t = 0; t < headTriangles; t += 5) {
        for (int k = 0; k < 3; ++k) {
            const auto v = static_cast<std::size_t>(head->triangles()[t][static_cast<std::size_t>(k)]);
            const StrandRoot corner{static_cast<std::int32_t>(t), k == 1 ? 1.0f : 0.0f, k == 2 ? 1.0f : 0.0f};
            EXPECT_NEAR(scalp->colorAt("tint", corner)[0], tint[v], 1e-6f);
        }
    }
    ScalpMaps earMaps;
    earMaps.maps["tint"] = {"vertices", std::vector<float>(tint.begin() + static_cast<std::ptrdiff_t>(head->pointCount()), tint.end()), ""};
    GroomEvaluator earSession(earsAlone);
    earSession.setScalpMaps(earMaps);
    const MeshPtr earScalp = earSession.scalp();
    ASSERT_EQ(earScalp->triangleCount(), earsRefined->triangleCount());
    for (std::size_t t = 0; t < earScalp->triangleCount(); t += 3) {
        const StrandRoot r{static_cast<std::int32_t>(t), 0.3f, 0.3f};
        EXPECT_NEAR(scalp->colorAt("tint", {static_cast<std::int32_t>(headTriangles + t), 0.3f, 0.3f})[0], earScalp->colorAt("tint", r)[0], 1e-5f);
    }
    // The guides rooted on the refined scalp: the head's on its own triangles where they were.
    ASSERT_TRUE(view.hierarchy);
    std::size_t headGuides = 0, earGuides = 0;
    for (const HierarchyGuide& g : view.hierarchy->guides()) {
        ASSERT_TRUE(scalp->contains(g.root));
        EXPECT_LT((scalp->position(g.root) - g.points.front()).length(), 1e-4f);
        const HierarchyGuide* authored = h.guide(g.id);
        ASSERT_TRUE(authored);
        if (static_cast<std::size_t>(authored->root.face) < headTriangles) {
            ++headGuides;
            EXPECT_LT(static_cast<std::size_t>(g.root.face), headTriangles);
            EXPECT_LT((scalp->position(g.root) - joined->position(authored->root)).length(), 1e-6f);
        } else {
            ++earGuides;
            EXPECT_GE(static_cast<std::size_t>(g.root.face), headTriangles);
        }
    }
    EXPECT_EQ(headGuides, 2u);
    EXPECT_EQ(earGuides, 2u);
    // The Scalp nodes say each scalp's own level.
    for (const auto& [node, note] : session.nodeNotes()) {
        if (node == "earScalp") EXPECT_NE(note.find("x2"), std::string::npos) << note;
        if (node == "scalp") EXPECT_EQ(note.find("Catmull-Clark"), std::string::npos) << note;
    }
}

}  // namespace
}  // namespace groomlab
