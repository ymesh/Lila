// What GroomEvaluator does for hosts beyond upstream's restored cases: stroke documents warmed and dabs evaluated away
// from it, Sculpt targets, StrandMesh sources, clump centers, moving guides and the view handed on as rendered.
#include <gtest/gtest.h>

#include "groom/GroomEvaluator.h"
#include "groom/GroomEvaluatorAsync.h"
#include "groomlab/frame_time.h"
#include "groomlab/guide_hierarchy.h"
#include "test_helpers.h"

#include <cmath>
#include <stdexcept>

using lila::groom::DetachedEvaluation;
using lila::groom::GroomEvaluator;

namespace groomlab
{
namespace
{
const char* kGraph = R"({
  "version": 1,
  "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "guides", "type": "Guides"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 300.0}},
    {"name": "interpolate", "type": "Interpolate"},
    {"name": "clump", "type": "Clump", "params": {"strength": 0.6, "count": 40}},
    {"name": "width", "type": "Width"},
    {"name": "output", "type": "Output"}
  ],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interpolate", "input": "strands"},
    {"from": "guides", "to": "interpolate", "input": "guides"},
    {"from": "scalp", "to": "interpolate", "input": "scalp"},
    {"from": "interpolate", "to": "clump", "input": "strands"},
    {"from": "clump", "to": "width", "input": "strands"},
    {"from": "width", "to": "output", "input": "strands"}
  ],
  "output": "output"
})";

std::shared_ptr<const ScalpMesh> sphere()
{
    return test::makeSphere(12, 24);
}

std::string hierarchyJson()
{
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    for (int i = 0; i < 8; ++i)
    {
        const float a = 6.2831853f * static_cast<float>(i) / 8.0f;
        const Vec3f n = Vec3f{std::cos(a) * 0.6f, 0.8f, std::sin(a) * 0.6f}.normalized();
        h.addGuide(primary, test::straightCurve(n, n * 0.8f + Vec3f{0.0f, 0.0f, 0.2f}, 6));
    }
    const auto scalp = sphere();
    h.evaluate(scalp.get());
    return h.toJson();
}

GroomData groomData(const std::string& graph = kGraph)
{
    GroomData d;
    d.graphJson = graph;
    d.hierarchyJson = hierarchyJson();
    d.scalp = sphere();
    return d;
}

/** kGraph with `node` (JSON, named `name`) between width and the output. */
std::string withNode(const std::string& node, const std::string& name)
{
    std::string g = kGraph;
    const std::string output = R"({"name": "output", "type": "Output"})";
    g.replace(g.find(output), output.size(), node + ", " + output);
    const std::string link = R"({"from": "width", "to": "output", "input": "strands"})";
    g.replace(g.find(link), link.size(),
              R"({"from": "width", "to": ")" + name + R"(", "input": "strands"}, {"from": ")" + name +
                  R"(", "to": "output", "input": "strands"})");
    return g;
}

/** The stored guides with every point but the roots pushed along x: a comb's dab. */
std::shared_ptr<GuideHierarchy> combed(const GuideHierarchy& stored, float by)
{
    auto out = std::make_shared<GuideHierarchy>(stored);
    for (const HierarchyGuide& g : stored.guides())
    {
        std::vector<Vec3f> points = g.points;
        for (std::size_t p = 1; p < points.size(); ++p)
            points[p].x += by * static_cast<float>(p);
        out->setPoints(g.id, std::move(points));
    }
    return out;
}

GroomEvaluator::Request strokeRequest()
{
    GroomEvaluator::Request request;
    request.sculptDensity = 0.5f;
    return request;
}

/** A dab evaluated in place on a cold evaluator: what every other way of getting there must give. */
std::uint64_t coldDab(float by)
{
    GroomEvaluator e(groomData());
    const GroomEvaluator::Request request = strokeRequest();
    e.evaluate(request);
    e.setPreviewHierarchy(combed(*e.authoredHierarchy(), by));
    return e.evaluate(request).strands->contentHash();
}
} // namespace

TEST(EvaluatorStroke, AWarmedStrokeDocumentMakesTheFirstDabFromItsCache)
{
    GroomEvaluator e(groomData());
    const GroomEvaluator::Request request = strokeRequest();
    e.evaluate(request);
    EXPECT_FALSE(e.strokeDocument());
    EXPECT_TRUE(e.warmStroke(request));
    EXPECT_TRUE(e.strokeDocument());
    EXPECT_FALSE(e.warmStroke(request)); // warm already: nothing runs again
    const std::size_t viewRuns = e.evaluationCount();
    // Pressing with the guides as they are finds every result made.
    e.setPreviewHierarchy(std::make_shared<GuideHierarchy>(*e.authoredHierarchy()));
    e.evaluate(request);
    EXPECT_EQ(e.evaluationCount(), viewRuns);
    e.setPreviewHierarchy(combed(*e.authoredHierarchy(), 0.02f));
    EXPECT_EQ(e.evaluate(request).strands->contentHash(), coldDab(0.02f));

    // Released, the stroke document goes; the next warm makes it again.
    e.setPreviewHierarchy(nullptr);
    e.releaseStroke();
    EXPECT_FALSE(e.strokeDocument());
    EXPECT_TRUE(e.warmStroke(request));
}

TEST(EvaluatorStroke, AWarmRunAwayFromTheEvaluatorIsAdoptedAsItsStrokeDocument)
{
    GroomEvaluator e(groomData());
    const GroomEvaluator::Request request = strokeRequest();
    e.evaluate(request);
    const std::shared_ptr<DetachedEvaluation> warm = e.detachWarm(request);
    ASSERT_TRUE(warm);
    EXPECT_TRUE(warm->warm());
    lila::groom::DetachedRunner runner;
    runner.start(warm);
    ASSERT_EQ(runner.wait(), warm);
    EXPECT_TRUE(e.adoptWarm(*warm));
    const std::size_t viewRuns = e.evaluationCount();
    e.setPreviewHierarchy(std::make_shared<GuideHierarchy>(*e.authoredHierarchy()));
    e.evaluate(request);
    EXPECT_EQ(e.evaluationCount(), viewRuns);

    // A warm whose stroke document was evaluated meanwhile is not taken.
    e.setPreviewHierarchy(nullptr);
    const std::shared_ptr<DetachedEvaluation> stale = e.detachWarm(request);
    ASSERT_TRUE(stale);
    stale->run();
    e.releaseStroke();
    EXPECT_FALSE(e.adoptWarm(*stale));
}

TEST(EvaluatorStroke, ADabRunAwayFromTheEvaluatorLandsAsADabHere)
{
    GroomEvaluator e(groomData());
    const GroomEvaluator::Request request = strokeRequest();
    e.evaluate(request);
    EXPECT_FALSE(e.detachStroke(request)); // no stroke yet
    // The press makes the stroke document; the next dab is evaluated on another thread with a copy of the guides.
    std::shared_ptr<GuideHierarchy> tool = std::make_shared<GuideHierarchy>(*e.authoredHierarchy());
    e.setPreviewHierarchy(tool);
    e.evaluate(request);
    tool = combed(*e.authoredHierarchy(), 0.02f);
    e.setPreviewHierarchy(tool);
    const std::shared_ptr<DetachedEvaluation> dab = e.detachStroke(request);
    ASSERT_TRUE(dab);
    EXPECT_TRUE(e.strokeLent());
    EXPECT_FALSE(e.strokeDocument());
    // The tool goes on editing its own guides meanwhile; the run evaluates the copy it took.
    std::vector<Vec3f> moved = tool->guides().front().points;
    moved.back().y += 1.0f;
    tool->setPoints(tool->guides().front().id, std::move(moved));
    lila::groom::DetachedRunner runner;
    runner.start(dab);
    runner.wait();
    EXPECT_TRUE(e.adoptStroke(*dab));
    EXPECT_FALSE(e.strokeLent());
    const std::size_t runs = e.evaluationCount();
    EXPECT_EQ(e.evaluate(request).strands->contentHash(), coldDab(0.02f));
    EXPECT_EQ(e.evaluationCount(), runs); // every result was made by the run

    // A view at its own density has no stroke document to lend.
    GroomEvaluator::Request full;
    EXPECT_FALSE(e.detachStroke(full));
}

TEST(EvaluatorSculpt, TheTargetsOutputIsTheGuidesShown)
{
    const std::string graph = R"({"output": "output", "nodes": [
      {"name": "scalp", "type": "Scalp"},
      {"name": "sparse", "type": "Guides"},
      {"name": "even", "type": "Scatter", "params": {"density": 6.0, "randomness": 0.0, "length": 0.5, "previewAll": true}},
      {"name": "dense", "type": "Interpolate"},
      {"name": "toGuides", "type": "ToGuides"},
      {"name": "sculpt1", "type": "Sculpt", "params": {"strength": 1.0}},
      {"name": "roots", "type": "Scatter", "params": {"density": 60.0}},
      {"name": "hair", "type": "Interpolate"},
      {"name": "output", "type": "Output"}],
     "connections": [
      {"from": "scalp", "to": "even", "input": "scalp"},
      {"from": "even", "to": "dense", "input": "strands"},
      {"from": "sparse", "to": "dense", "input": "guides"},
      {"from": "scalp", "to": "dense", "input": "scalp"},
      {"from": "dense", "to": "toGuides", "input": "strands"},
      {"from": "toGuides", "to": "sculpt1", "input": "guides"},
      {"from": "scalp", "to": "sculpt1", "input": "scalp"},
      {"from": "scalp", "to": "roots", "input": "scalp"},
      {"from": "roots", "to": "hair", "input": "strands"},
      {"from": "sculpt1", "to": "hair", "input": "guides"},
      {"from": "scalp", "to": "hair", "input": "scalp"},
      {"from": "hair", "to": "output", "input": "strands"}]})";
    GroomEvaluator e(groomData(graph));
    const GroomEvaluator::Result& plain = e.evaluate(GroomEvaluator::Request{});
    EXPECT_EQ(plain.hierarchy, plain.authoredHierarchy);
    const std::size_t storedGuides = plain.hierarchy->guides().size();

    e.setSculptTarget("sculpt1");
    const GroomEvaluator::Result& targeted = e.evaluate(GroomEvaluator::Request{});
    ASSERT_TRUE(targeted.hierarchy);
    EXPECT_NE(targeted.hierarchy, targeted.authoredHierarchy);
    EXPECT_GT(targeted.hierarchy->guides().size(), storedGuides); // the dense guides the Sculpt node is fed
    EXPECT_EQ(e.sculptHierarchy(), targeted.hierarchy);

    const SculptInput in = e.sculptInput("sculpt1");
    ASSERT_TRUE(in.guides);
    EXPECT_EQ(in.guides->size(), targeted.hierarchy->guides().size());
    EXPECT_FLOAT_EQ(in.strength, 1.0f);
    EXPECT_THROW(e.sculptInput("hair"), std::runtime_error);
    EXPECT_THROW(e.sculptInput("missing"), std::runtime_error);

    e.setSculptTarget("");
    EXPECT_EQ(e.evaluate(GroomEvaluator::Request{}).hierarchy, e.last().authoredHierarchy);
    EXPECT_FALSE(e.sculptHierarchy());
}

TEST(EvaluatorViews, StrandMeshNodesHandOnTheirStrandsUntilTheyChange)
{
    GroomEvaluator e(groomData(withNode(R"({"name": "mesh", "type": "ToMesh"})", "mesh")));
    EXPECT_TRUE(e.hasStrandMeshNode());
    GroomEvaluator::Request request;
    request.wantStrandMeshes = true;
    const GroomEvaluator::Result& first = e.evaluate(request);
    ASSERT_EQ(first.strandMeshes.size(), 1u);
    EXPECT_EQ(first.strandMeshes.front().node, "mesh");
    ASSERT_TRUE(first.strandMeshes.front().strands);
    const StrandsPtr meshStrands = first.strandMeshes.front().strands;
    const std::uint64_t version = first.versions.strandMeshes;
    const GroomEvaluator::Result& again = e.evaluate(request);
    EXPECT_EQ(again.strandMeshes.front().strands, meshStrands);
    EXPECT_EQ(again.versions.strandMeshes, version);
    request.wantStrandMeshes = false;
    EXPECT_TRUE(e.evaluate(request).strandMeshes.empty());
    EXPECT_FALSE(GroomEvaluator(groomData()).hasStrandMeshNode());
}

TEST(EvaluatorViews, AViewedClumpHasItsCentersAndAViewedScatterItsRoots)
{
    GroomEvaluator e(groomData());
    GroomEvaluator::Request request;
    request.viewNode = "clump";
    const GroomEvaluator::Result& clumped = e.evaluate(request);
    ASSERT_TRUE(clumped.clumpCenters);
    EXPECT_FALSE(clumped.clumpCenters->empty());
    EXPECT_FALSE(clumped.viewedRoots);
    request.viewNode = "scatter";
    const GroomEvaluator::Result& scattered = e.evaluate(request);
    EXPECT_TRUE(scattered.viewedRoots);
    EXPECT_FALSE(scattered.clumpCenters);
}

TEST(EvaluatorViews, GuidesASimulateNodeMovesAreHandedOnToDraw)
{
    std::string graph = kGraph;
    const std::string link = R"({"from": "guides", "to": "interpolate", "input": "guides"})";
    graph.replace(graph.find(link), link.size(),
                  R"({"from": "guides", "to": "sim", "input": "guides"}, {"from": "sim", "to": "interpolate", "input": "guides"})");
    const std::string scatter = R"({"name": "scatter", "type": "Scatter", "params": {"density": 300.0}})";
    graph.replace(graph.find(scatter), scatter.size(), R"({"name": "sim", "type": "Simulate", "params": {"startFrame": 1}}, )" + scatter);
    GroomEvaluator e(groomData(graph));
    e.setFramesPerSecond(24.0);
    e.evaluate(GroomEvaluator::Request{});
    EXPECT_FALSE(e.last().movingGuides); // before its start frame the guides pass through
    std::vector<Vec3f> points = sphere()->points();
    GroomEvaluator::Request request;
    for (int frame = 1; frame <= 3; ++frame)
    {
        for (Vec3f& p : points)
            p.x += 0.1f;
        e.updateScalpPoints(points);
        request.time = frameToSeconds(frame, 24.0);
        e.evaluate(request);
    }
    ASSERT_TRUE(e.last().movingGuides);
    EXPECT_FALSE(e.last().movingGuides->empty());
    EXPECT_FALSE(GroomEvaluator(groomData()).evaluate(request).movingGuides);
}

TEST(EvaluatorViews, TheViewIsHandedOnAsRenderedOnlyWhenEvaluatedAlike)
{
    GroomEvaluator e(groomData());
    GroomEvaluator::Request request;
    request.time = 0.25;
    const StrandsPtr shown = e.evaluate(request).strands;
    EXPECT_EQ(e.viewedAsRendered(1.0f, 0.25), shown);
    EXPECT_FALSE(e.viewedAsRendered(0.5f, 0.25));
    EXPECT_FALSE(e.viewedAsRendered(1.0f, 0.5));
    request.viewNode = "clump";
    e.evaluate(request);
    EXPECT_FALSE(e.viewedAsRendered(1.0f, 0.25));
}
} // namespace groomlab
