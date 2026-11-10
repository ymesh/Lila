// Adapted from GroomLab's groom_session.cpp (github.com/thekesslereffect/groomlab, Apache-2.0,
// see src/groom/groomlab/LICENSE and NOTICE).
#include "groom/GroomEvaluator.h"

#include <groomlab/guide_hierarchy.h>
#include <groomlab/operators.h>

#include <chrono>
#include <stdexcept>

namespace lila::groom
{
using namespace groomlab;

std::string GroomEvaluator::shownName(const GraphDocument& doc, const std::string& viewNode) const
{
    const auto it = doc.nodesByName.find(viewNode);
    return !viewNode.empty() && it != doc.nodesByName.end() && doc.graph.outputType(it->second) == PortType::Strands ? viewNode
                                                                                                                    : std::string();
}

void GroomEvaluator::jobContext(DetachedEvaluation& job, float density, const Request& request) const
{
    job.mPaths = paths() ? mPaths : nullptr;
    EvalContext& ctx = job.mCtx;
    ctx.previewDensity = density;
    ctx.maxPoints = request.pointBudget;
    ctx.time = request.time;
    ctx.framesPerSecond = mFps;
    ctx.scale = mGroomScale;
    ctx.viewport = request.viewport;
    ctx.liveDrag = mLiveDrag;
    ctx.externals = &job.mExternals;
    ctx.paths = job.mPaths.get();
    ctx.buildCards = request.buildCards;
    job.mRequest = request;
}

// Live strokes.

bool GroomEvaluator::setLiveStroke(bool on)
{
    if (on == mLiveStroke)
        return false;
    mLiveStroke = on;
    if (on)
    {
        mStrokeDensityUsed = false;
        return false;
    }
    const bool again = mStrokeDensityUsed; // something evaluated at the stroke's density
    mStrokeDensityUsed = false;
    return again;
}

bool GroomEvaluator::warmStroke(const Request& request)
{
    const float density = request.density * request.sculptDensity;
    // Nothing evaluated yet: no inputs bound to warm it with.
    if (!mDoc || hasPreview() || density == request.density || mResult.versions.evaluation == 0)
        return false;
    if (!mSculptDoc)
        mSculptDoc = newDocument();
    // The inputs as the last evaluation bound them: nothing here binds or roots again, so the view's next evaluation sees
    // its changes as ever.
    ExternalInputs externals;
    fillExternals(externals);
    EvalContext ctx = context(externals);
    ctx.previewDensity = density;
    ctx.maxPoints = request.pointBudget;
    ctx.time = request.time;
    ctx.viewport = request.viewport;
    ctx.liveDrag = mLiveDrag;
    const std::string shown = shownName(*mSculptDoc, request.viewNode);
    Graph& graph = mSculptDoc->graph;
    const std::size_t evaluations = graph.evaluationCount(), incrementals = graph.incrementalCount();
    try
    {
        graph.evaluate(shown.empty() ? mSculptDoc->output : mSculptDoc->nodesByName.at(shown), ctx);
    }
    catch (...)
    {
        // The view says it.
    }
    mWarmEvaluations += graph.evaluationCount() - evaluations;
    mWarmIncrementals += graph.incrementalCount() - incrementals;
    return graph.evaluationCount() != evaluations;
}

std::shared_ptr<DetachedEvaluation> GroomEvaluator::detachWarm(const Request& request)
{
    const float density = request.density * request.sculptDensity;
    if (!mDoc || hasPreview() || density == request.density || mResult.versions.evaluation == 0)
        return nullptr;
    if (!mSculptDoc)
        mSculptDoc = newDocument();
    std::shared_ptr<DetachedEvaluation> job(new DetachedEvaluation);
    fillExternals(job->mExternals);
    jobContext(*job, density, request);
    job->mCtx.buildCards = false;
    job->mShown = shownName(*mSculptDoc, request.viewNode);
    job->mDoc = std::make_unique<GraphDocument>(mSculptDoc->clone());
    job->mBasedOn = mDocGeneration;
    job->mWarm = true;
    job->mWarmBase = mSculptDoc.get();
    job->mWarmBaseEvaluations = mSculptDoc->graph.evaluationCount();
    job->mWarmBaseGeneration = mStrokeDocGeneration;
    return job;
}

bool GroomEvaluator::adoptWarm(DetachedEvaluation& run)
{
    if (!run.mWarm || !run.ran() || run.mError || !run.mDoc || !mSculptDoc || hasPreview())
        return false;
    if (mSculptDoc.get() != run.mWarmBase || mSculptDoc->graph.evaluationCount() != run.mWarmBaseEvaluations ||
        mStrokeDocGeneration != run.mWarmBaseGeneration)
        return false;
    // Not counted, as warmStroke's are not: the copy's evaluations join the warm ones.
    mWarmEvaluations += run.mDoc->graph.evaluationCount();
    mWarmIncrementals += run.mDoc->graph.incrementalCount();
    run.mDoc->graph.addCounts(mSculptDoc->graph.evaluationCount(), mSculptDoc->graph.incrementalCount());
    std::swap(mSculptDoc, run.mDoc);
    return true;
}

std::shared_ptr<DetachedEvaluation> GroomEvaluator::detachStroke(const Request& request)
{
    // Map, part and flow previews, the Simulate nodes' stepping, cards and a host's stopAt only evaluate does.
    if (!mDoc || !mSculptDoc || mStrokeLent || !mPreviewHierarchy || mLiveStroke || mPreviewParts || mPreviewFlow || mPreviewMapValues ||
        mPreviewMapImage || !mSims.empty() || request.wantCards || request.wantStrandMeshes || !request.stopAt.empty())
        return nullptr;
    const float density = request.density * request.sculptDensity;
    if (density == request.density)
        return nullptr; // the view's own document: nothing to lend
    // The inputs as evaluate(request) binds them now, the preview a copy of the tool's guides as they are; the active
    // hierarchy put back after, so nothing the caller reads meanwhile is the run's.
    const std::shared_ptr<GuideHierarchy> live = mPreviewHierarchy;
    const HierarchyPtr active = mActiveHierarchy, posed = mPosedHierarchy;
    const bool activeValid = mActiveValid;
    const std::uint64_t activeKey = mActiveKey;
    std::shared_ptr<DetachedEvaluation> job(new DetachedEvaluation);
    job->mStrokePreview = std::make_shared<GuideHierarchy>(*live);
    mPreviewHierarchy = job->mStrokePreview;
    std::vector<std::string> errors; // said by the evaluation that takes the run in
    prepareTime(request.time, errors, mDetachedMessages);
    fillExternals(job->mExternals);
    mPreviewHierarchy = live;
    mActiveHierarchy = active;
    mPosedHierarchy = posed;
    mActiveValid = activeValid;
    mActiveKey = activeKey;
    jobContext(*job, density, request);
    job->mShown = shownName(*mSculptDoc, request.viewNode);
    job->mDoc = std::move(mSculptDoc); // lent: nothing else evaluates it until adopted
    job->mBasedOn = mDocGeneration;
    job->mStroke = true;
    job->mStrokeGraph = mGraphGeneration;
    mStrokeLent = true;
    return job;
}

bool GroomEvaluator::adoptStroke(DetachedEvaluation& run)
{
    if (!run.mStroke || !run.ran())
        return false;
    mStrokeLent = false;
    // The guides the run evaluated stand for the tool's while the host shows its result.
    if (mPreviewHierarchy && run.mStrokePreview)
        mPreviewHierarchy = run.mStrokePreview;
    // The document back, unless the graph was set again or a stroke document was made meanwhile.
    if (!run.mDoc || run.mStrokeGraph != mGraphGeneration || mSculptDoc)
        return false;
    mSculptDoc = std::move(run.mDoc);
    return true;
}

void GroomEvaluator::releaseStroke()
{
    if (!mSculptDoc)
        return;
    mReleasedEvaluations += mSculptDoc->graph.evaluationCount() - mWarmEvaluations;
    mReleasedIncrementals += mSculptDoc->graph.incrementalCount() - mWarmIncrementals;
    mWarmEvaluations = mWarmIncrementals = 0;
    mSculptDoc.reset();
    ++mStrokeDocGeneration;
    mViewSculpt = false;
    mShown[1] = Shown{};
}

// Detached evaluations.

std::shared_ptr<DetachedEvaluation> GroomEvaluator::detach(const Request& request, const std::shared_ptr<DetachedEvaluation>& after)
{
    // A preview's stroke document, the Simulate nodes' stepping and the cards' document only evaluate does.
    if (!mDoc || hasPreview() || !mSims.empty() || request.wantCards || request.wantStrandMeshes || !request.stopAt.empty())
        return nullptr;
    if (after && (after->mWarm || after->mBasedOn != mDocGeneration))
        return nullptr;
    std::shared_ptr<DetachedEvaluation> job(new DetachedEvaluation);
    // What binding the inputs says is said by the evaluation that takes the result in.
    std::vector<std::string> errors;
    prepareTime(request.time, errors, mDetachedMessages);
    fillExternals(job->mExternals);
    jobContext(*job, request.density, request);
    job->mShown = shownName(*mDoc, request.viewNode);
    if (after)
    {
        job->mAfter = after;
        job->mBasedOn = after->mBasedOn;
    }
    else
    {
        job->mDoc = std::make_unique<GraphDocument>(mDoc->clone());
        job->mBasedOn = mDocGeneration;
    }
    return job;
}

bool GroomEvaluator::adopt(DetachedEvaluation& run)
{
    if (run.mWarm || !run.ran() || run.mError || !run.mDoc || !mDoc || run.mBasedOn != mDocGeneration)
        return false;
    run.mDoc->graph.addCounts(mDoc->graph.evaluationCount(), mDoc->graph.incrementalCount());
    std::swap(mDoc, run.mDoc);
    ++mDocGeneration;
    return true;
}

void DetachedEvaluation::run()
{
    const auto started = std::chrono::steady_clock::now();
    try
    {
        if (mAfter)
        {
            // The evaluation before ran (the host runs them in order): this one goes on from what it made.
            if (!mAfter->ran())
                throw std::logic_error("a detached evaluation ran before the one it follows");
            if (mAfter->mError)
                std::rethrow_exception(mAfter->mError);
            mDoc = std::make_unique<GraphDocument>(mAfter->mDoc->clone());
            mDoc->graph.addCounts(mAfter->mDoc->graph.evaluationCount(), mAfter->mDoc->graph.incrementalCount());
            mAfter.reset();
        }
        const auto it = mDoc->nodesByName.find(mShown);
        mDoc->graph.evaluateStrands(mShown.empty() || it == mDoc->nodesByName.end() ? mDoc->output : it->second, mCtx);
    }
    catch (...)
    {
        mError = std::current_exception();
    }
    mAfter.reset();
    mMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    mRan.store(true, std::memory_order_release);
}

// Sculpt nodes.

void GroomEvaluator::setSculptTarget(const std::string& node)
{
    if (node == mSculptTarget)
        return;
    mSculptTarget = node;
    ++mSculptsVersion;
    mSculptShown.reset();
    mSculptShownFrom.reset();
    mSculptShownOn.reset();
}

void GroomEvaluator::setPreviewSculpt(const std::string& node, SculptPtr edits, std::shared_ptr<const GuideHierarchy> shown)
{
    mPreviewSculpt = std::move(edits);
    mPreviewSculptNode = mPreviewSculpt ? node : std::string();
    mPreviewSculptHash = mPreviewSculpt ? mPreviewSculpt->contentHash() : 0;
    mPreviewSculptShown = mPreviewSculpt ? std::move(shown) : nullptr;
}

SculptInput GroomEvaluator::sculptInput(const std::string& node)
{
    if (!mDoc)
        throw std::runtime_error(mGraphError.empty() ? "the groom has no graph" : mGraphError);
    const auto found = mDoc->nodesByName.find(node);
    if (found == mDoc->nodesByName.end())
        throw std::runtime_error("the graph has no node named '" + node + "'");
    if (mDoc->graph.node(found->second).typeName() != op::kSculpt)
        throw std::runtime_error("node '" + node + "' is a " + mDoc->graph.node(found->second).typeName() + ", not a Sculpt node");
    // A scratch copy of the view's document at the last view's settings: what the view made is reused, nothing evicted.
    GraphDocument scratch = mParsed->clone();
    scratch.graph.adoptCache(mDoc->graph);
    const NodeId id = scratch.node(node);
    ExternalInputs externals;
    fillExternals(externals);
    EvalContext ctx = mLastViewContext;
    ctx.framesPerSecond = mFps;
    ctx.scale = mGroomScale;
    ctx.externals = &externals;
    ctx.paths = paths();
    SculptInput in;
    in.node = node;
    NodeId source = 0;
    std::size_t output = 0;
    if (!scratch.graph.inputSource(id, 0, &source, &output))
        throw std::runtime_error("Sculpt node '" + node + "' is fed no guides");
    in.guides = valueAs<GuidesPtr>(scratch.graph.evaluateOutput(source, output, ctx));
    if (!in.guides)
        throw std::runtime_error("Sculpt node '" + node + "' is fed no guides");
    if (scratch.graph.inputSource(id, 1, &source, &output))
        in.scalp = valueAs<MeshPtr>(scratch.graph.evaluateOutput(source, output, ctx));
    in.strength = static_cast<float>(mDoc->graph.node(found->second).params().getDouble("strength"));
    if (const SculptEdits* stored = mSculpts.node(node))
        in.edits = *stored;
    return in;
}

GuidesPtr GroomEvaluator::sculptOutput(GraphDocument& doc, const EvalContext& ctx)
{
    const auto found = doc.nodesByName.find(mSculptTarget);
    if (found == doc.nodesByName.end() || doc.graph.node(found->second).typeName() != op::kSculpt)
        return nullptr;
    try
    {
        return valueAs<GuidesPtr>(doc.graph.evaluate(found->second, ctx));
    }
    catch (const std::exception&)
    {
        return nullptr; // the evaluation's own errors say why
    }
}

HierarchyPtr GroomEvaluator::sculptHierarchy()
{
    if (mSculptTarget.empty())
        return nullptr;
    if (mPreviewSculptShown && mPreviewSculptNode == mSculptTarget)
        return mPreviewSculptShown;
    if (mSculptShownVersion != mSculptsVersion)
    {
        // Not looked for since the target or edits changed.
        Request request;
        request.density = mLastViewContext.previewDensity;
        request.pointBudget = mLastViewContext.maxPoints;
        request.time = mLastViewContext.time;
        request.viewport = mLastViewContext.viewport;
        evaluate(request);
    }
    return mSculptShown;
}
} // namespace lila::groom
