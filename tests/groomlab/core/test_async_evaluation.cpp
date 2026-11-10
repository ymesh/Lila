// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// lila::groom::AsyncEvaluation (async_evaluation.h): a session evaluated on a worker thread gives what the same
// session evaluated in place gives, evaluations chain in order on the session's cache, what a worker
// throws comes back once, and the session is the caller's again after wait().

#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>
#include <thread>

#include "groom/GroomEvaluatorAsync.h"
#include "groom/GroomEvaluator.h"
#include "groomlab/graph.h"
#include "groomlab/guide_hierarchy.h"
#include "test_helpers.h"

using lila::groom::GroomEvaluator;

namespace groomlab {
namespace {

const char* kGraph = R"({
  "version": 1,
  "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "guides", "type": "Guides"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 300.0}},
    {"name": "interpolate", "type": "Interpolate"},
    {"name": "noise", "type": "Noise", "params": {"amplitude": 0.05}},
    {"name": "width", "type": "Width"},
    {"name": "output", "type": "Output"}
  ],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interpolate", "input": "strands"},
    {"from": "guides", "to": "interpolate", "input": "guides"},
    {"from": "scalp", "to": "interpolate", "input": "scalp"},
    {"from": "interpolate", "to": "noise", "input": "strands"},
    {"from": "noise", "to": "width", "input": "strands"},
    {"from": "width", "to": "output", "input": "strands"}
  ],
  "output": "output"
})";

std::string graphWithAmplitude(float amplitude) {
    std::string g = kGraph;
    const std::string from = R"("amplitude": 0.05)";
    g.replace(g.find(from), from.size(), "\"amplitude\": " + std::to_string(amplitude));
    return g;
}

std::shared_ptr<const ScalpMesh> sphere() { return test::makeSphere(12, 24); }

// Eight guides; `bend` tilts their tips (a brush stroke's edit).
std::string hierarchyJson(float bend = 0.0f) {
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    TubeProfile tube;
    tube.width = {0.15f, 0.1f};
    for (int i = 0; i < 8; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / 8.0f;
        const Vec3f n = Vec3f{std::cos(a) * 0.6f, 0.8f, std::sin(a) * 0.6f}.normalized();
        h.addGuide(primary, test::straightCurve(n, n * 0.8f + Vec3f{bend, 0.0f, 0.2f}, 6), tube);
    }
    const auto scalp = sphere();
    h.evaluate(scalp.get());
    return h.toJson();
}

GroomData groomData() {
    GroomData d;
    d.graphJson = kGraph;
    d.hierarchyJson = hierarchyJson();
    d.scalp = sphere();
    return d;
}

// A node's cached result in the session's view document (null: none).
StrandsPtr cached(const GroomEvaluator& session, const char* node) {
    const GraphDocument* doc = session.viewDocument();
    if (!doc || !doc->nodesByName.count(node)) return nullptr;
    return valueAs<StrandsPtr>(doc->graph.cachedValue(doc->nodesByName.at(node)));
}

void expectSameStrands(const StrandSet& a, const StrandSet& b) {
    ASSERT_EQ(a.ids(), b.ids());
    ASSERT_EQ(a.offsets(), b.offsets());
    ASSERT_EQ(a.positions(), b.positions());
    ASSERT_EQ(a.widths(), b.widths());
}

TEST(AsyncEvaluation, GivesWhatTheSessionGivesInPlace) {
    GroomEvaluator inPlace(groomData()), async(groomData());
    const GroomEvaluator::Request request;
    const GroomEvaluator::Result& want = inPlace.evaluate(request);
    lila::groom::AsyncEvaluation worker(async);
    EXPECT_EQ(worker.state(), lila::groom::AsyncEvaluation::State::Idle);
    EXPECT_EQ(worker.wait(), nullptr);  // nothing started
    worker.start(request);
    EXPECT_TRUE(worker.pending());
    const GroomEvaluator::Result* got = worker.wait();
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got, &async.last());
    EXPECT_EQ(worker.state(), lila::groom::AsyncEvaluation::State::Idle);
    EXPECT_EQ(worker.evaluations(), 1u);
    EXPECT_GE(worker.lastMilliseconds(), 0.0);
    ASSERT_TRUE(got->strands && want.strands);
    expectSameStrands(*got->strands, *want.strands);
    EXPECT_EQ(got->versions.strands, want.versions.strands);
}

TEST(AsyncEvaluation, ACommitOffTheThreadReusesTheSessionsWorkAsInPlace) {
    // A brush stroke's commit (new guides) and a drag's (a new param), evaluated on the worker
    // after evaluations in place, give what the same edits evaluated in place give, and leave the
    // work of the nodes they do not change (Scatter's strands) as the session had it.
    GroomEvaluator inPlace(groomData()), async(groomData());
    const GroomEvaluator::Request request;
    inPlace.evaluate(request);
    const GroomEvaluator::Result before = async.evaluate(request);
    const StrandsPtr scattered = cached(async, "scatter");
    ASSERT_TRUE(scattered);

    inPlace.setHierarchyJson(hierarchyJson(0.3f));
    const GroomEvaluator::Result wantStroke = inPlace.evaluate(request);
    lila::groom::AsyncEvaluation worker(async);
    worker.start(request, [](GroomEvaluator& s) { s.setHierarchyJson(hierarchyJson(0.3f)); });  // parsed on the worker
    const GroomEvaluator::Result* stroke = worker.wait();
    ASSERT_TRUE(stroke && stroke->strands);
    expectSameStrands(*stroke->strands, *wantStroke.strands);
    EXPECT_NE(stroke->versions.strands, before.versions.strands);
    EXPECT_EQ(cached(async, "scatter"), scattered);  // not made again

    inPlace.setGraphJson(graphWithAmplitude(0.09f));
    const GroomEvaluator::Result wantDrag = inPlace.evaluate(request);
    async.setGraphJson(graphWithAmplitude(0.09f));  // a setter in place, then the evaluation off it
    worker.start(request);
    const GroomEvaluator::Result* drag = worker.wait();
    ASSERT_TRUE(drag && drag->strands);
    expectSameStrands(*drag->strands, *wantDrag.strands);
    EXPECT_EQ(cached(async, "scatter"), scattered);

    // Back in place: the session goes on from the worker's evaluation (nothing changed: the same strands).
    const GroomEvaluator::Result& again = async.evaluate(request);
    EXPECT_EQ(again.strands, drag->strands);
}

TEST(AsyncEvaluation, StartedAgainBeforeTakenChainsInOrder) {
    // A second commit before the first was taken (another stroke's release): the first runs to its
    // end, then the second on the session as the first left it; the result is the second's.
    GroomEvaluator inPlace(groomData()), async(groomData());
    const GroomEvaluator::Request request;
    inPlace.setGraphJson(graphWithAmplitude(0.02f));
    inPlace.setHierarchyJson(hierarchyJson(-0.2f));
    const GroomEvaluator::Result& want = inPlace.evaluate(request);
    lila::groom::AsyncEvaluation worker(async);
    worker.start(request, [](GroomEvaluator& s) { s.setGraphJson(graphWithAmplitude(0.02f)); });
    worker.start(request, [](GroomEvaluator& s) { s.setHierarchyJson(hierarchyJson(-0.2f)); });
    const GroomEvaluator::Result* got = worker.wait();
    ASSERT_TRUE(got && got->strands);
    expectSameStrands(*got->strands, *want.strands);
    EXPECT_EQ(worker.evaluations(), 2u);
    EXPECT_EQ(worker.wait(), nullptr);  // taken once
}

TEST(AsyncEvaluation, RunsOnAWorkerThreadAndSaysWhenReady) {
    GroomEvaluator session(groomData());
    lila::groom::AsyncEvaluation worker(session);
    std::thread::id ran;
    worker.start(GroomEvaluator::Request{}, [&](GroomEvaluator&) { ran = std::this_thread::get_id(); });
    while (!worker.ready()) std::this_thread::yield();
    EXPECT_EQ(worker.state(), lila::groom::AsyncEvaluation::State::Done);
    EXPECT_NE(ran, std::this_thread::get_id());
    const GroomEvaluator::Result* all = worker.wait();
    ASSERT_TRUE(all && all->strands);
    EXPECT_FALSE(worker.pending());
    // On fewer threads (a host's UI keeping cores for itself): the same strands.
    const StrandsPtr strands = all->strands;
    GroomEvaluator other(groomData());
    lila::groom::AsyncEvaluation two(other);
    two.setThreads(2);
    EXPECT_EQ(two.threads(), 2u);
    two.start(GroomEvaluator::Request{});
    const GroomEvaluator::Result* capped = two.wait();
    ASSERT_TRUE(capped && capped->strands);
    expectSameStrands(*capped->strands, *strands);
}

TEST(AsyncEvaluation, WhatTheWorkerThrowsComesBackOnceAndTheSessionGoesOn) {
    GroomEvaluator session(groomData());
    lila::groom::AsyncEvaluation worker(session);
    worker.start(GroomEvaluator::Request{}, [](GroomEvaluator&) { throw std::runtime_error("no memory"); });
    EXPECT_THROW(worker.wait(), std::runtime_error);
    EXPECT_FALSE(worker.pending());
    EXPECT_EQ(worker.wait(), nullptr);  // once
    worker.start(GroomEvaluator::Request{});
    const GroomEvaluator::Result* got = worker.wait();
    ASSERT_TRUE(got && got->strands);
    EXPECT_TRUE(got->errors.empty());
    const StrandsPtr strands = got->strands;
    // A graph that does not load, set on the worker: its GraphError comes back (a host reports it,
    // or sets the text in place with dropGraph); the session keeps its graph and goes on.
    worker.start(GroomEvaluator::Request{}, [](GroomEvaluator& s) { s.setGraphJson(R"({"nodes": [{"name": "x", "type": "NoSuchNode"}]})"); });
    EXPECT_THROW(worker.wait(), GraphError);
    worker.start(GroomEvaluator::Request{});
    const GroomEvaluator::Result* kept = worker.wait();
    ASSERT_TRUE(kept && kept->strands);
    expectSameStrands(*kept->strands, *strands);
}

TEST(AsyncEvaluation, LetGoWhileRunningWaitsForIt) {
    GroomEvaluator session(groomData());
    {
        lila::groom::AsyncEvaluation worker(session);
        worker.start(GroomEvaluator::Request{});
    }  // waits: the session is whole and the caller's again
    const GroomEvaluator::Result& view = session.evaluate(GroomEvaluator::Request{});
    EXPECT_TRUE(view.strands && view.strands->strandCount() > 0);
}

// ---- Detached evaluations (GroomEvaluator::detach, lila::groom::DetachedRunner) ------------------------------

// The stroke graph above with clumps and the clearance walk (what a creature's fur runs).
const char* kFurGraph = R"({
  "version": 1,
  "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "guides", "type": "Guides"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 300.0}},
    {"name": "interpolate", "type": "Interpolate", "params": {"keepClearance": true, "neighbors": 3}},
    {"name": "clump", "type": "Clump", "params": {"count": 40, "copyAmount": 0.3}},
    {"name": "noise", "type": "Noise", "params": {"amplitude": 0.05}},
    {"name": "width", "type": "Width"},
    {"name": "output", "type": "Output"}
  ],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interpolate", "input": "strands"},
    {"from": "guides", "to": "interpolate", "input": "guides"},
    {"from": "scalp", "to": "interpolate", "input": "scalp"},
    {"from": "interpolate", "to": "clump", "input": "strands"},
    {"from": "clump", "to": "noise", "input": "strands"},
    {"from": "noise", "to": "width", "input": "strands"},
    {"from": "width", "to": "output", "input": "strands"}
  ],
  "output": "output"
})";

// Eight guides, guide `which` (or every one, -1) bent by `bend`: a brush stroke's edit.
std::string bentHierarchyJson(int which, float bend) {
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    TubeProfile tube;
    tube.width = {0.15f, 0.1f};
    for (int i = 0; i < 8; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / 8.0f;
        const Vec3f n = Vec3f{std::cos(a) * 0.6f, 0.8f, std::sin(a) * 0.6f}.normalized();
        const float b = which < 0 || which == i ? bend : 0.0f;
        h.addGuide(primary, test::straightCurve(n, n * 0.8f + Vec3f{b, -0.3f * std::fabs(b), 0.2f}, 6), tube);
    }
    const auto scalp = sphere();
    h.evaluate(scalp.get());
    return h.toJson();
}

GroomData furData(const std::string& hierarchy) {
    GroomData d;
    d.graphJson = kFurGraph;
    d.hierarchyJson = hierarchy;
    d.scalp = sphere();
    return d;
}

// Every incremental result checked against a full evaluation of the same inputs (graph.h).
struct VerifyIncremental {
    VerifyIncremental() { Graph::setVerifyIncremental(true); }
    ~VerifyIncremental() { Graph::setVerifyIncremental(false); }
};

// A stroke's dabs in place while a commit is detached: the guides alone evaluated (the GPU draws
// the strands), a preview hierarchy set, then cleared.
void strokeDabsInPlace(GroomEvaluator& session, const GroomEvaluator::Request& request, float bend) {
    GroomEvaluator::Request dab = request;
    dab.stopAt = "interpolate";
    for (int k = 1; k <= 3; ++k) {
        session.setPreviewHierarchy(std::make_shared<GuideHierarchy>(GuideHierarchy::fromJson(bentHierarchyJson(2, bend * k / 3.0f))));
        session.evaluate(dab);
    }
    session.setPreviewHierarchy(nullptr);
}

TEST(DetachedEvaluation, ACommitRunAwayFromTheSessionIsWhatTheSessionMakesInPlace) {
    // A stroke's commit detached and run on a worker while the session evaluates the next stroke's
    // dabs in place: adopted, the session's evaluation makes nothing again and gives, to the bit,
    // what the session evaluating the commit in place gives, and what a new session gives.
    const VerifyIncremental verify;
    const GroomEvaluator::Request request;
    GroomEvaluator inPlace(furData(bentHierarchyJson(-1, 0.0f))), session(furData(bentHierarchyJson(-1, 0.0f)));
    inPlace.evaluate(request);
    const GroomEvaluator::Result before = session.evaluate(request);
    const StrandsPtr interpolatedBefore = cached(session, "interpolate");

    inPlace.setHierarchyJson(bentHierarchyJson(3, 0.4f));
    const StrandsPtr want = inPlace.evaluate(request).strands;
    session.setHierarchyJson(bentHierarchyJson(3, 0.4f));
    const std::size_t incrementals = session.incrementalCount();
    lila::groom::DetachedRunner runner;
    runner.start(session.detach(request));
    EXPECT_TRUE(runner.pending());
    strokeDabsInPlace(session, request, 0.3f);  // the session is the caller's meanwhile
    EXPECT_EQ(cached(session, "interpolate"), interpolatedBefore);  // its own document as it was
    const std::shared_ptr<lila::groom::DetachedEvaluation> run = runner.wait();
    ASSERT_TRUE(run && run->ran());
    EXPECT_FALSE(run->error());
    EXPECT_FALSE(runner.pending());
    ASSERT_TRUE(session.adopt(*run));
    EXPECT_GT(session.incrementalCount(), incrementals);  // the commit went on incrementally
    const std::size_t evaluations = session.evaluationCount();
    const GroomEvaluator::Result& landed = session.evaluate(request);
    EXPECT_EQ(session.evaluationCount(), evaluations);  // every result made already
    ASSERT_TRUE(landed.strands && want);
    expectSameStrands(*landed.strands, *want);
    EXPECT_NE(landed.versions.strands, before.versions.strands);
    ASSERT_TRUE(landed.strandsChanges && !landed.strandsChanges->all);  // the stroke's strands, for a partial upload
    EXPECT_GT(landed.strandsChanges->strands.size(), 0u);
    GroomEvaluator fresh(furData(bentHierarchyJson(3, 0.4f)));
    expectSameStrands(*fresh.evaluate(request).strands, *landed.strands);
}

TEST(DetachedEvaluation, ChainedCommitsGoOnFromTheOneBefore) {
    // A second stroke's commit detached before the first was adopted: it copies what the first made
    // once that has run, and makes again only what the second stroke changed; adopting it gives the
    // second stroke's groom, as in place and as a new session.
    const VerifyIncremental verify;
    const GroomEvaluator::Request request;
    GroomEvaluator session(furData(bentHierarchyJson(-1, 0.0f)));
    session.evaluate(request);
    lila::groom::DetachedRunner runner;
    runner.setThreads(3);
    session.setHierarchyJson(bentHierarchyJson(3, 0.4f));
    runner.start(session.detach(request, runner.last()));
    strokeDabsInPlace(session, request, -0.2f);
    session.setHierarchyJson(bentHierarchyJson(5, -0.3f));
    const std::shared_ptr<lila::groom::DetachedEvaluation> first = runner.last();
    runner.start(session.detach(request, first));
    EXPECT_EQ(runner.started(), 2u);
    const std::shared_ptr<lila::groom::DetachedEvaluation> second = runner.wait();
    ASSERT_TRUE(first->ran() && second && second->ran());
    EXPECT_NE(second, first);
    EXPECT_FALSE(second->error());
    ASSERT_TRUE(session.adopt(*second));
    const std::size_t evaluations = session.evaluationCount();
    const GroomEvaluator::Result& landed = session.evaluate(request);
    EXPECT_EQ(session.evaluationCount(), evaluations);
    const GraphDocument* doc = session.viewDocument();
    EXPECT_EQ(doc->graph.lastRunKinds().at(doc->nodesByName.at("interpolate")).rfind("incremental", 0), 0u);
    GroomEvaluator fresh(furData(bentHierarchyJson(5, -0.3f)));
    ASSERT_TRUE(landed.strands);
    expectSameStrands(*fresh.evaluate(request).strands, *landed.strands);
    // Undone (the first stroke's guides again): in place from the adopted results, as a new session.
    session.setHierarchyJson(bentHierarchyJson(3, 0.4f));
    GroomEvaluator undone(furData(bentHierarchyJson(3, 0.4f)));
    expectSameStrands(*undone.evaluate(request).strands, *session.evaluate(request).strands);
}

TEST(DetachedEvaluation, NotAdoptedOnceTheSessionTookAnotherGraph) {
    const GroomEvaluator::Request request;
    GroomEvaluator session(furData(bentHierarchyJson(-1, 0.0f)));
    session.evaluate(request);
    session.setHierarchyJson(bentHierarchyJson(1, 0.4f));
    lila::groom::DetachedRunner runner;
    runner.start(session.detach(request));
    session.setGraphJson(graphWithAmplitude(0.07f));  // another document
    EXPECT_EQ(session.detach(request, runner.last()), nullptr);  // nothing to go on from
    const std::shared_ptr<lila::groom::DetachedEvaluation> run = runner.wait();
    EXPECT_FALSE(session.adopt(*run));
    const GroomEvaluator::Result& view = session.evaluate(request);  // the session's own evaluation
    GroomData d = furData(bentHierarchyJson(1, 0.4f));
    d.graphJson = graphWithAmplitude(0.07f);
    GroomEvaluator fresh(d);
    ASSERT_TRUE(view.strands);
    expectSameStrands(*fresh.evaluate(request).strands, *view.strands);
}

TEST(DetachedEvaluation, NothingDetachedWhileAStrokeIsLiveOrForTheSessionsOwnWork) {
    const GroomEvaluator::Request request;
    GroomEvaluator session(furData(bentHierarchyJson(-1, 0.0f)));
    session.evaluate(request);
    session.setPreviewHierarchy(std::make_shared<GuideHierarchy>(GuideHierarchy::fromJson(bentHierarchyJson(1, 0.2f))));
    EXPECT_EQ(session.detach(request), nullptr);
    session.setPreviewHierarchy(nullptr);
    GroomEvaluator::Request cards = request;
    cards.wantCards = true;
    EXPECT_EQ(session.detach(cards), nullptr);
    GroomEvaluator::Request dab = request;
    dab.stopAt = "interpolate";
    EXPECT_EQ(session.detach(dab), nullptr);
    EXPECT_NE(session.detach(request), nullptr);
}

TEST(DetachedEvaluation, RunnerLetGoWhileRunningWaitsForThem) {
    const GroomEvaluator::Request request;
    GroomEvaluator session(furData(bentHierarchyJson(-1, 0.0f)));
    session.evaluate(request);
    std::shared_ptr<lila::groom::DetachedEvaluation> run;
    {
        lila::groom::DetachedRunner runner;
        session.setHierarchyJson(bentHierarchyJson(2, 0.3f));
        runner.start(session.detach(request));
        session.setHierarchyJson(bentHierarchyJson(4, 0.3f));
        runner.start(session.detach(request, runner.last()));
        run = runner.last();
    }  // waits for both
    EXPECT_TRUE(run->ran());
    EXPECT_TRUE(session.adopt(*run));
}

}  // namespace
}  // namespace groomlab
