// Adapted from GroomLab's groom_session.cpp (github.com/thekesslereffect/groomlab, Apache-2.0,
// see src/groom/groomlab/LICENSE and NOTICE).
#include "groom/GroomEvaluator.h"

#include <groomlab/card_files.h>
#include <groomlab/export_view.h>
#include <groomlab/frame_time.h>
#include <groomlab/guide_hierarchy.h>
#include <groomlab/instances.h>
#include <groomlab/operators.h>
#include <groomlab/presets.h>
#include <groomlab/random.h>
#include <groomlab/scalp_history.h>
#include <groomlab/scalp_mesh.h>
#include <groomlab/scalp_pieces.h>
#include <groomlab/strand_set.h>
#include <groomlab/subdivision.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace lila::groom
{
using namespace groomlab;

namespace groomEvaluator
{
std::uint64_t doubleBits(double d)
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof bits);
    return bits;
}

bool blank(const std::string& text)
{
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

bool colorNode(const std::string& type)
{
    return type == "HairColor" || type == "Bands" || type == "FeatherColor";
}

/** The Blend nodes making the output that mix two different colors. */
std::vector<std::string> colorBlendNodes(const GraphDocument& doc)
{
    std::vector<std::string> blends, out;
    doc.graph.forEachActiveUpstream(doc.output, [&](const std::string& at, const Operator& op) {
        if (op.typeName() == "Blend" && op.params().getString("include") != "shape")
            blends.push_back(at);
        return true;
    });
    for (const std::string& at : blends)
    {
        const auto it = doc.nodesByName.find(at);
        if (it == doc.nodesByName.end())
            continue;
        auto colorsOf = [&](std::size_t slot) {
            std::set<std::string> found;
            NodeId source = 0;
            if (doc.graph.inputSource(it->second, slot, &source))
                doc.graph.forEachActiveUpstream(source, [&](const std::string& path, const Operator& op) {
                    if (colorNode(op.typeName()))
                        found.insert(path);
                    return true;
                });
            return found;
        };
        if (colorsOf(0) != colorsOf(1))
            out.push_back(at);
    }
    return out;
}

/** Names the node that first makes points that aren't numbers, walking the output's first-input chain. */
std::string nonFiniteMessage(const GraphDocument* doc)
{
    std::string first;
    if (doc && doc->graph.hasNode(doc->output))
    {
        NodeId id = doc->output;
        for (int guard = 0; guard < 4096 && id; ++guard)
        {
            const StrandsPtr strands = valueAs<StrandsPtr>(doc->graph.cachedValue(id));
            if (strands && !strands->extent().finite)
                first = doc->nameOf(id);
            NodeId source = 0;
            if (doc->graph.inputSpecs(id).empty() || !doc->graph.inputSource(id, 0, &source))
                break;
            id = source;
        }
    }
    const std::string what = "the strands have points that are not numbers (NaN or infinite)";
    return first.empty() ? what + ": a node before the output made them" : "node '" + first + "' makes " + what;
}

/** The first node of `type` making the output, with its path. */
const Operator* activeNode(const GraphDocument& doc, const char* type, std::string* path)
{
    const Operator* found = nullptr;
    doc.graph.forEachActiveUpstream(doc.output, [&](const std::string& at, const Operator& op) {
        if (op.typeName() != type)
            return true;
        found = &op;
        if (path)
            *path = at;
        return false;
    });
    return found;
}
/**
 * The guides an Interpolate feeding `shown` takes, when a GuideCache or Simulate node on its guides input moved them this
 * frame: what the viewport draws as the moving guides. Every value read is the evaluation's own (cached).
 */
GuidesPtr movingGuidesOf(Graph& graph, NodeId shown, const EvalContext& ctx)
{
    auto isMotion = [&](NodeId id) {
        const std::string type = graph.node(id).typeName();
        return (type == "GuideCache" || type == "Simulate") && !graph.bypassed(id);
    };
    auto guidesSlot = [&](NodeId id) -> std::ptrdiff_t {
        const std::vector<PortSpec> specs = graph.inputSpecs(id);
        for (std::size_t slot = 0; slot < specs.size(); ++slot)
            if (specs[slot].name == "guides")
                return static_cast<std::ptrdiff_t>(slot);
        return -1;
    };
    std::vector<NodeId> stack{shown}, seen;
    while (!stack.empty())
    {
        const NodeId id = stack.back();
        stack.pop_back();
        if (std::find(seen.begin(), seen.end(), id) != seen.end())
            continue;
        seen.push_back(id);
        if (graph.node(id).typeName() == "Interpolate" && !graph.bypassed(id))
        {
            const std::ptrdiff_t slot = guidesSlot(id);
            NodeId source = 0;
            std::size_t output = 0;
            if (slot >= 0 && graph.inputSource(id, static_cast<std::size_t>(slot), &source, &output))
            {
                // Up the guides' chain to the nearest node that moves them.
                NodeId at = source;
                for (int steps = 0; at && steps < 64 && !isMotion(at); ++steps)
                {
                    const std::ptrdiff_t up = guidesSlot(at);
                    NodeId next = 0;
                    at = up >= 0 && graph.inputSource(at, static_cast<std::size_t>(up), &next) ? next : 0;
                }
                if (at)
                {
                    const GuidesPtr moved = valueAs<GuidesPtr>(graph.evaluateOutput(source, output, ctx));
                    NodeId fed = 0;
                    std::size_t fedOutput = 0;
                    const std::ptrdiff_t in = guidesSlot(at);
                    const GuidesPtr before = in >= 0 && graph.inputSource(at, static_cast<std::size_t>(in), &fed, &fedOutput)
                                                 ? valueAs<GuidesPtr>(graph.evaluateOutput(fed, fedOutput, ctx))
                                                 : nullptr;
                    if (moved && !moved->empty() && moved != before)
                        return moved;
                }
            }
        }
        for (std::size_t slot = 0; slot < graph.inputSpecs(id).size(); ++slot)
        {
            NodeId source = 0;
            if (graph.inputSource(id, slot, &source))
                stack.push_back(source);
        }
    }
    return nullptr;
}
} // namespace groomEvaluator

using namespace groomEvaluator;

// Construction and stored inputs.

GroomEvaluator::GroomEvaluator()
{
    mParsed = parseScaled(mData.graphJson);
    mDoc = std::make_unique<GraphDocument>(mParsed->clone());
    mGraphSet = true;
}

GroomEvaluator::GroomEvaluator(GroomData data) : mData(std::move(data))
{
    mParsed = parseScaled(mData.graphJson);
    mDoc = std::make_unique<GraphDocument>(mParsed->clone());
    mGraphSet = true;
    if (!blank(mData.hierarchyJson))
    {
        mRawHierarchy = std::make_shared<const GuideHierarchy>(GuideHierarchy::fromJson(mData.hierarchyJson));
        mParsedHierarchy = mRawHierarchy;
    }
    refreshScale();
    mHierarchySet = true;
    mHierarchyFromText = true;
    ++mParsedVersion;
    mPartSet = PartSet::fromJson(mData.partsJson);
    try
    {
        mFlowSet = FlowSet::fromJson(mData.flowJson);
        mCorrectives = Correctives::fromJson(mData.correctivesJson);
        mSculpts = SculptSet::fromJson(mData.sculptsJson);
    }
    catch (const std::invalid_argument& e)
    {
        throw std::runtime_error(e.what());
    }
    ++mCorrectivesVersion;
    for (const auto& [name, edits] : mSculpts.nodes)
        mSculptExternals[name] = {std::make_shared<const SculptEdits>(edits), edits.contentHash()};
    mPosesHash = mData.guidePoses ? mData.guidePoses->contentHash() : 0;
    mTime = mData.time;
}

GroomEvaluator::~GroomEvaluator() = default;

std::unique_ptr<GraphDocument> GroomEvaluator::parseScaled(const std::string& graphJson) const
{
    if (mGroomScale == 1.0)
        return std::make_unique<GraphDocument>(loadGraphJson(graphJson));
    loadGraphJson(graphJson); // a graph that does not load says why as stored, not scaled
    return std::make_unique<GraphDocument>(loadGraphJson(scaleGraphJson(graphJson, mGroomScale)));
}

void GroomEvaluator::setGraphJson(const std::string& graphJson)
{
    if (mGraphSet && graphJson == mData.graphJson)
        return;
    installGraph(parseScaled(graphJson));
    mControlFactors = controlScaleFactors(graphJson, mGroomScale);
    mData.graphJson = graphJson;
    mGraphSet = true;
    mGraphError.clear();
}

void GroomEvaluator::installGraph(std::unique_ptr<GraphDocument> parsed)
{
    // Each document is a clone of the one parse; nodes whose type, params, inputs and data are the same keep their results.
    auto adopt = [&](std::unique_ptr<GraphDocument>& old) {
        auto doc = std::make_unique<GraphDocument>(parsed->clone());
        doc->graph.setCacheIntermediates(mCacheIntermediates);
        if (old)
            doc->graph.adoptCache(old->graph);
        old = std::move(doc);
    };
    std::unique_ptr<RestPose> rest = mRestPose ? std::make_unique<RestPose>(parsed->clone(), mRestPose.get()) : nullptr;
    adopt(mDoc);
    ++mDocGeneration;
    ++mGraphGeneration;
    if (mSculptDoc)
        adopt(mSculptDoc);
    ++mStrokeDocGeneration;
    mWarmEvaluations = mWarmIncrementals = mReleasedEvaluations = mReleasedIncrementals = 0;
    if (mCardDoc)
        adopt(mCardDoc);
    if (mFullDoc)
        adopt(mFullDoc);
    for (auto& [density, doc] : mMeshDocs)
        adopt(doc);
    mRestPose = std::move(rest);
    mParsed = std::move(parsed);
}

void GroomEvaluator::dropGraph(const std::string& graphJson, const std::string& error)
{
    mParsed.reset();
    mDoc.reset();
    ++mDocGeneration;
    ++mGraphGeneration;
    mSculptDoc.reset();
    ++mStrokeDocGeneration;
    mWarmEvaluations = mWarmIncrementals = mReleasedEvaluations = mReleasedIncrementals = 0;
    mShown[0] = mShown[1] = Shown{};
    mCardDoc.reset();
    mFullDoc.reset();
    mMeshDocs.clear();
    mMeshMemos.clear();
    mRestPose.reset();
    mData.graphJson = graphJson;
    mGraphSet = true;
    mGraphError = error;
}

// Scaling with the scalp.

void GroomEvaluator::setScaleSource(ScaleSource source)
{
    if (source == mData.scaleSource)
        return;
    mData.scaleSource = source;
    refreshScale();
}

void GroomEvaluator::setScaleWithScalp(bool on)
{
    if (on == scaleWithScalp())
        return;
    setScaleSource(on ? ScaleSource::Area : ScaleSource::Off);
}

void GroomEvaluator::setScaleTransform(double scale)
{
    if (!(scale > 0.0) || !std::isfinite(scale))
        scale = 0.0;
    if (scale == mScaleTransform)
        return;
    mScaleTransform = scale;
    if (mFirstTransform <= 0.0)
        mFirstTransform = scale;
    // Under Area the transform is taken with the scalp handed in next, so a scaled transform and its area arrive together.
    if (mData.scaleSource == ScaleSource::Transform)
        refreshScale();
}

void GroomEvaluator::setScaleTransformReference(double scale)
{
    if (!(scale > 0.0) || !std::isfinite(scale))
        scale = 0.0;
    if (scale == mData.scaleTransformReference)
        return;
    mData.scaleTransformReference = scale;
    refreshScale();
}

void GroomEvaluator::setScaleReference(double area)
{
    if (!(area > 0.0) || !std::isfinite(area))
        area = 0.0;
    if (area == mData.scaleReference)
        return;
    mData.scaleReference = area;
    refreshScale();
}

void GroomEvaluator::refreshScale()
{
    // The rest pose's area: the same on every frame of a deforming scalp, and for a transform's scale frozen into points.
    mScaleArea = mData.scalp ? restSurfaceArea(*mData.scalp) : 0.0;
    if (!(mScaleArea > 0.0) || !std::isfinite(mScaleArea))
        mScaleArea = 0.0;
    if (mFirstArea <= 0.0)
        mFirstArea = mScaleArea;
    const double reference = scaleReference();
    const bool areaKnown = mScaleArea > 0.0 && reference > 0.0;
    double k = 1.0;
    switch (mData.scaleSource)
    {
    case ScaleSource::Off:
        break;
    case ScaleSource::Area:
        if (!areaKnown)
            return;
        k = std::sqrt(mScaleArea / reference);
        break;
    case ScaleSource::Transform:
        if (mScaleTransform > 0.0)
        {
            const double from = scaleTransformReference();
            if (!(from > 0.0))
                return;
            k = mScaleTransform / from;
        }
        else
        {
            // A host without transforms: the area's k on the first measure, then kept.
            if (mScaleMeasured || !areaKnown)
                return;
            k = std::sqrt(mScaleArea / reference);
        }
        break;
    }
    if (!std::isfinite(k) || k <= 0.0)
        k = 1.0;
    if (std::abs(k - 1.0) < 1e-4)
        k = 1.0;
    const double areaBefore = mMeasuredArea, transformBefore = mMeasuredTransform, kBefore = mGroomScale;
    const bool measuredBefore = mScaleMeasured;
    if (mData.scaleSource != ScaleSource::Off)
    {
        mMeasuredArea = mScaleArea;
        mMeasuredTransform = mScaleTransform;
        mScaleMeasured = true;
    }
    if (std::abs(k / mGroomScale - 1.0) < 1e-4)
        return; // the same k, or a moving scalp's float noise
    // k moving with the area while the transform stands is the scalp's own size changing: said once.
    if (mData.scaleSource == ScaleSource::Area && measuredBefore && areaBefore > 0.0 &&
        std::abs(mScaleArea / areaBefore - 1.0) > 1e-4 &&
        std::abs(mScaleTransform - transformBefore) <= 1e-4 * std::max(std::abs(transformBefore), 1e-12))
    {
        char line[200];
        std::snprintf(line, sizeof line,
                      "The scalp's area changed %.0f%%: the groom scaled x%.2f. Set Scale Reference to keep its sizes.",
                      std::abs(mScaleArea / areaBefore - 1.0) * 100.0, k / kBefore);
        mScaleNotice = line;
        ++mScaleNoticeSerial;
    }
    mGroomScale = k;
    mControlFactors = controlScaleFactors(mData.graphJson, k);
    if (mParsed && mGraphError.empty())
    {
        try
        {
            installGraph(parseScaled(mData.graphJson));
        }
        catch (const std::exception&)
        {
            // It loaded at the last k, and scaling changes only numbers.
        }
    }
    scaleHierarchy();
}

void GroomEvaluator::absorbScaleTransform()
{
    if (mScaleMeasured)
        mMeasuredTransform = mScaleTransform;
}

void GroomEvaluator::scaleHierarchy()
{
    std::shared_ptr<const GuideHierarchy> scaled = mRawHierarchy;
    if (mRawHierarchy && mRawHierarchy->scale() != static_cast<float>(mGroomScale))
    {
        auto copy = std::make_shared<GuideHierarchy>(*mRawHierarchy);
        copy->setScale(static_cast<float>(mGroomScale));
        scaled = std::move(copy);
    }
    if (scaled == mParsedHierarchy)
        return;
    mParsedHierarchy = std::move(scaled);
    ++mParsedVersion;
}

std::map<std::string, float> GroomEvaluator::scaledControls() const
{
    std::map<std::string, float> out = mData.controls;
    for (auto& [name, value] : out)
        if (const auto f = mControlFactors.find(name); f != mControlFactors.end())
            value = static_cast<float>(value * f->second);
    return out;
}

void GroomEvaluator::setHierarchyJson(const std::string& json)
{
    if (mHierarchySet && mHierarchyFromText && json == mData.hierarchyJson)
        return;
    mData.hierarchyJson = json;
    mHierarchySet = true;
    mHierarchyFromText = true;
    mHierarchyError.clear();
    try
    {
        mRawHierarchy = std::make_shared<const GuideHierarchy>(GuideHierarchy::fromJson(json));
    }
    catch (const std::exception& e)
    {
        mRawHierarchy.reset();
        mHierarchyError = std::string("guideData: ") + e.what();
    }
    mParsedHierarchy = mRawHierarchy;
    scaleHierarchy();
    ++mParsedVersion;
}

void GroomEvaluator::setHierarchy(std::shared_ptr<const GuideHierarchy> hierarchy)
{
    if (mHierarchySet && !mHierarchyFromText && hierarchy == mRawHierarchy)
        return;
    mData.hierarchyJson.clear();
    mHierarchySet = true;
    mHierarchyFromText = false;
    mHierarchyError.clear();
    mRawHierarchy = std::move(hierarchy);
    mParsedHierarchy = mRawHierarchy;
    scaleHierarchy();
    ++mParsedVersion;
}

void GroomEvaluator::setPartsJson(const std::string& json)
{
    if (json == mData.partsJson)
        return;
    mData.partsJson = json;
    mPartError.clear();
    try
    {
        mPartSet = PartSet::fromJson(json);
    }
    catch (const std::exception& e)
    {
        mPartSet = PartSet{};
        mPartError = std::string("partData: ") + e.what();
    }
}

void GroomEvaluator::setFlowJson(const std::string& json)
{
    if (json == mData.flowJson)
        return;
    mData.flowJson = json;
    mFlowError.clear();
    try
    {
        mFlowSet = FlowSet::fromJson(json);
    }
    catch (const std::exception& e)
    {
        mFlowSet = FlowSet{};
        mFlowError = std::string("flowData: ") + e.what();
    }
}

void GroomEvaluator::setCorrectivesJson(const std::string& json)
{
    if (json == mData.correctivesJson)
        return;
    mData.correctivesJson = json;
    mCorrectiveError.clear();
    try
    {
        mCorrectives = Correctives::fromJson(json);
    }
    catch (const std::exception& e)
    {
        mCorrectives = Correctives{};
        mCorrectiveError = std::string("correctiveData: ") + e.what();
    }
    ++mCorrectivesVersion;
}

void GroomEvaluator::setSculptsJson(const std::string& json)
{
    if (json == mData.sculptsJson)
        return;
    mData.sculptsJson = json;
    mSculptError.clear();
    try
    {
        mSculpts = SculptSet::fromJson(json);
    }
    catch (const std::exception& e)
    {
        mSculpts = SculptSet{};
        mSculptError = e.what();
    }
    // Shared again only where they changed: the other nodes keep their pointer and their cached results.
    std::map<std::string, std::pair<SculptPtr, std::uint64_t>> externals;
    for (const auto& [name, edits] : mSculpts.nodes)
    {
        const std::uint64_t hash = edits.contentHash();
        const auto it = mSculptExternals.find(name);
        if (it != mSculptExternals.end() && it->second.second == hash && *it->second.first == edits)
            externals[name] = it->second;
        else
            externals[name] = {std::make_shared<const SculptEdits>(edits), hash};
    }
    mSculptExternals = std::move(externals);
    ++mSculptsVersion;
}

void GroomEvaluator::setCorrectiveWeights(std::map<int, float> weights)
{
    if (mLiveWeights && weights == mWeights)
        return;
    mLiveWeights = true;
    mWeights = std::move(weights);
}

void GroomEvaluator::clearCorrectiveWeights()
{
    mLiveWeights = false;
    mWeights.clear();
}

std::vector<std::string> GroomEvaluator::inputErrors() const
{
    std::vector<std::string> out;
    for (const std::string* e : {&mHierarchyError, &mGraphError, &mPartError, &mFlowError, &mCorrectiveError, &mScalpMapsError, &mSculptError})
        if (!e->empty())
            out.push_back(*e);
    return out;
}

// Scalp and maps.

void GroomEvaluator::setScalpFingerprint(std::uint64_t fingerprint)
{
    if (fingerprint == mData.scalpFingerprint)
        return;
    mData.scalpFingerprint = fingerprint;
    mFingerprintKey = 0;
}

void GroomEvaluator::setScalp(MeshPtr scalp)
{
    if (scalp == mData.scalp || (scalp && mData.scalp && scalp->contentHash() == mData.scalp->contentHash()))
    {
        absorbScaleTransform();
        return; // keeps the old mesh with its closest-point tree
    }
    const bool sameSize = scalp && mData.scalp && scalp->restTotalArea() == mData.scalp->restTotalArea();
    mData.scalp = std::move(scalp);
    if (!sameSize)
        refreshScale();
}

void GroomEvaluator::updateScalpPoints(std::vector<Vec3f> points, const std::vector<Vec3f>* restPoints)
{
    if (!mData.scalp)
        throw std::runtime_error("updateScalpPoints: the groom has no scalp");
    if (points.size() != mData.scalp->pointCount())
        throw std::invalid_argument("updateScalpPoints: " + std::to_string(points.size()) + " points for a scalp of " +
                                    std::to_string(mData.scalp->pointCount()));
    const bool newRest = restPoints && *restPoints != mData.scalp->restPoints();
    if (points == mData.scalp->points() && !newRest)
        return;
    auto moved = std::make_shared<ScalpMesh>(mData.scalp->withPoints(std::move(points)));
    if (newRest)
        moved->setRestPoints(*restPoints);
    setScalp(std::move(moved));
}

void GroomEvaluator::setScalps(NamedScalps scalps)
{
    std::erase_if(scalps, [](const auto& s) { return !s.second; });
    if (scalps.size() == mData.scalps.size())
    {
        bool same = true;
        for (std::size_t i = 0; same && i < scalps.size(); ++i)
            same = scalps[i].first == mData.scalps[i].first &&
                   (scalps[i].second == mData.scalps[i].second || scalps[i].second->contentHash() == mData.scalps[i].second->contentHash());
        if (same)
            return;
    }
    mData.scalps = std::move(scalps);
}

MeshPtr GroomEvaluator::joinedScalp()
{
    if (mData.scalps.empty() || !mData.scalp)
    {
        mJoined.reset();
        mJoinedMain.reset();
        mJoinedExtras.clear();
        return mData.scalp;
    }
    if (mJoined && mJoinedMain == mData.scalp && mJoinedExtras == mData.scalps)
        return mJoined;
    // Only points moved (a deforming scalp's next frame): the joined mesh's points move.
    bool moved = mJoined && mJoinedMain && mJoinedExtras.size() == mData.scalps.size() && mData.scalp->samePointsLayout(*mJoinedMain);
    for (std::size_t i = 0; moved && i < mData.scalps.size(); ++i)
        moved = mData.scalps[i].first == mJoinedExtras[i].first && mData.scalps[i].second->samePointsLayout(*mJoinedExtras[i].second);
    if (moved)
    {
        std::vector<Vec3f> points(mData.scalp->points());
        points.reserve(mJoined->pointCount());
        for (const auto& [name, mesh] : mData.scalps)
            if (mJoined->piece(name))
                points.insert(points.end(), mesh->points().begin(), mesh->points().end());
        moved = points.size() == mJoined->pointCount();
        if (moved)
            mJoined = std::make_shared<const ScalpMesh>(mJoined->withPoints(std::move(points)));
    }
    if (!moved)
        mJoined = combineScalps(mData.scalp, mData.scalps);
    mJoinedMain = mData.scalp;
    mJoinedExtras = mData.scalps;
    return mJoined;
}

ScalpHistoryPtr GroomEvaluator::historyJoined()
{
    const MeshPtr joined = joinedScalp();
    if (!mData.scalpHistory || !joined || !mData.scalp || joined == mData.scalp)
        return mData.scalpHistory;
    if (mData.scalpHistory != mJoinedHistoryIn || joined != mJoinedHistoryOn)
    {
        // A frame of the groom's own scalp alone gets the other scalps' points now after it (they keep still).
        const std::size_t own = mData.scalp->pointCount(), whole = joined->pointCount();
        const ScalpHistory& in = *mData.scalpHistory;
        bool joinedFrames = true;
        for (const std::vector<Vec3f>& f : in.frames)
            joinedFrames = joinedFrames && (f.empty() || f.size() == whole);
        if (joinedFrames)
            mJoinedHistory = mData.scalpHistory;
        else
        {
            std::vector<std::vector<Vec3f>> frames;
            for (const std::vector<Vec3f>& f : in.frames)
            {
                if (f.size() == whole)
                    frames.push_back(f);
                else if (f.size() == own)
                {
                    std::vector<Vec3f> all(f);
                    all.insert(all.end(), joined->points().begin() + static_cast<std::ptrdiff_t>(own), joined->points().end());
                    frames.push_back(std::move(all));
                }
                else
                    frames.emplace_back();
            }
            mJoinedHistory = ScalpHistory::make(in.step, std::move(frames), in.loopSeconds);
        }
        mJoinedHistoryIn = mData.scalpHistory;
        mJoinedHistoryOn = joined;
    }
    return mJoinedHistory;
}

void GroomEvaluator::setScalpMapsJson(const std::string& json)
{
    if (mScalpMapsSet && mScalpMapsFromText && json == mScalpMapsText)
        return;
    mScalpMapsText = json;
    mScalpMapsSet = true;
    mScalpMapsFromText = true;
    mScalpMapsError.clear();
    try
    {
        mScalpMaps = ScalpMaps::fromJson(json);
    }
    catch (const std::exception& e)
    {
        mScalpMaps = ScalpMaps{};
        mScalpMapsError = e.what();
    }
    mScalpMapsHash = mScalpMaps.hash();
    mData.maps = mScalpMaps.catalog();
}

void GroomEvaluator::setScalpMaps(ScalpMaps maps)
{
    const std::uint64_t hash = maps.hash();
    if (mScalpMapsSet && !mScalpMapsFromText && hash == mScalpMapsHash)
        return;
    mScalpMapsText.clear();
    mScalpMapsSet = true;
    mScalpMapsFromText = false;
    mScalpMapsError.clear();
    mScalpMaps = std::move(maps);
    mScalpMapsHash = hash;
    mData.maps = mScalpMaps.catalog();
}

void GroomEvaluator::setMapCatalog(MapCatalog maps)
{
    if (mScalpMapsSet)
        return;
    mData.maps = std::move(maps);
}

MeshPtr GroomEvaluator::scalp()
{
    std::vector<std::string> errors;
    bindScalp(errors);
    return mScalp;
}

MeshPtr GroomEvaluator::cageScalp()
{
    std::vector<std::string> errors;
    bindScalp(errors);
    return mCageScalp;
}

std::map<std::string, int> GroomEvaluator::scalpSubdivisions() const
{
    const int matchRender = mData.scalp ? mData.scalp->renderSubdivision() : 0;
    std::map<std::string, int> renders;
    for (const auto& [name, mesh] : mData.scalps)
        if (mesh && validScalpName(name))
            renders.emplace(name, mesh->renderSubdivision());
    // Each Scalp node asks for the scalp it reads; one naming no other scalp asks for the groom's own.
    int own = -1;
    std::map<std::string, int> asked;
    if (mParsed)
        for (const NodeId id : mParsed->graph.nodeIds())
        {
            const Operator& op = mParsed->graph.node(id);
            if (op.typeName() != op::kScalp)
                continue;
            const std::string param = op.params().getString("subdivision");
            const auto it = renders.find(op.params().getString("input"));
            if (it == renders.end())
                own = std::max(own, subdivisionLevels(param, matchRender));
            else
            {
                const auto [at, added] = asked.emplace(it->first, -1);
                at->second = std::max(at->second, subdivisionLevels(param, it->second));
            }
        }
    if (own < 0)
        own = subdivisionLevels("matchRender", matchRender);
    std::map<std::string, int> out{{kMainScalpName, own}};
    for (const auto& [name, render] : renders)
    {
        const auto it = asked.find(name);
        out[name] = it == asked.end() ? own : it->second;
    }
    return out;
}

int GroomEvaluator::scalpSubdivision() const
{
    int levels = 0;
    for (const auto& [name, l] : scalpSubdivisions())
        levels = std::max(levels, l);
    return levels;
}

int GroomEvaluator::scalpLevelsOn(const MeshPtr& joined, MeshPtr* levelled) const
{
    if (levelled)
        *levelled = joined;
    if (!joined)
        return 0;
    if (joined->pieces().empty())
        return scalpSubdivision();
    const std::map<std::string, int> byScalp = scalpSubdivisions();
    int levels = 0;
    for (const auto& [name, l] : byScalp)
        levels = std::max(levels, l);
    if (levelled)
        *levelled = withScalpLevels(joined, byScalp, levels);
    return levels;
}

MeshPtr GroomEvaluator::refineAsScalp(const MeshPtr& joined) const
{
    MeshPtr cage;
    const int levels = scalpLevelsOn(joined, &cage);
    return subdivideScalp(cage, levels);
}

void GroomEvaluator::bindScalp(std::vector<std::string>& errors)
{
    if (!mScalpMapsError.empty())
        errors.push_back(mScalpMapsError);
    // Another topology than the groom was made on: worked out once per topology (a deforming scalp's frames keep it).
    if (const MeshPtr& own = mData.scalp; own && mData.scalpFingerprint)
    {
        const std::uint64_t key = hashCombine(hashCombine(hash64(0xF1A6), own->topologyHash()), static_cast<std::uint64_t>(own->pointCount()));
        if (key != mFingerprintKey)
        {
            mFingerprintKey = key;
            mFingerprintDiffers = own->topologyFingerprint() != mData.scalpFingerprint;
        }
        if (mFingerprintDiffers)
            errors.push_back(kScalpTopologyChanged);
    }
    const MeshPtr raw = joinedScalp();
    MeshPtr levelled;
    const int levels = scalpLevelsOn(raw, &levelled);
    const std::uint64_t levelsKey = levelled && levelled != raw ? levelled->piecesHash() : 0;
    const std::uint64_t previewKey = mPreviewMapValues ? mPreviewMapVersion : 0;
    const std::uint64_t key = hashCombine(hashCombine(hashCombine(raw ? raw->contentHash() : 0, mScalpMapsHash), previewKey), levelsKey);
    if (key != mMappedKey || !mCageScalp)
    {
        const std::uint64_t mapsKey = hashCombine(hashCombine(mScalpMapsHash, previewKey), levelsKey);
        if (raw && mCageScalp && mMappedFrom && mCageScalp != mMappedFrom && mapsKey == mMappedMapsKey && raw->samePointsLayout(*mMappedFrom))
        {
            // The same maps on the scalp's next frame: the color sets stay, the points move (the same mesh a fresh laying gives).
            mCageScalp = std::make_shared<const ScalpMesh>(mCageScalp->withPoints(raw->points()));
        }
        else
        {
            mMapErrors.clear();
            mCageScalp = withVertexMaps(levelled, mScalpMaps, mPreviewMapName, mPreviewMapValues.get(), &mMapErrors, mPreviewMapChannels);
        }
        mMappedKey = key;
        mMappedMapsKey = mapsKey;
        mMappedFrom = raw;
    }
    errors.insert(errors.end(), mMapErrors.begin(), mMapErrors.end());
    // The next frame of a refined scalp moves the last refined mesh's points (the plan is kept).
    if (levels == 0 || !mCageScalp)
    {
        mScalp = mCageScalp;
        mRefinedFrom.reset();
    }
    else if (mCageScalp != mRefinedFrom || !mScalp || mScalp->subdivisionLevel() != levels)
    {
        mScalp = subdivideScalp(mCageScalp, levels, mRefinedFrom, mScalp && mScalp != mCageScalp ? mScalp : nullptr);
        mRefinedFrom = mCageScalp;
    }
}

void GroomEvaluator::bindRefined()
{
    const bool refined = mScalp && mScalp->subdivisionLevel() > 0;
    // Flat guides were rooted by the host on its scalp: rooted again on the refined one.
    if (!refined || !mData.guides)
    {
        mGuides = mData.guides;
        mGuidesIn.reset();
    }
    else if (mData.guides != mGuidesIn || mScalp->surfaceHash() != mGuidesOn)
    {
        auto rooted = std::make_shared<GuideSet>(*mData.guides);
        rooted->rootOnMesh(*mScalp);
        mGuides = rooted;
        mGuidesIn = mData.guides;
        mGuidesOn = mScalp->surfaceHash();
    }
    const ScalpHistoryPtr scalpHistory = historyJoined();
    if (!refined || !scalpHistory)
    {
        mHistory = scalpHistory;
        mHistoryIn.reset();
    }
    else if (scalpHistory != mHistoryIn || mCageScalp != mHistoryOn)
    {
        const auto plan = SubdivisionPlan::of(*mCageScalp, mScalp->subdivisionLevel());
        std::vector<std::vector<Vec3f>> frames;
        const ScalpHistory& in = *scalpHistory;
        for (const std::vector<Vec3f>& f : in.frames)
            frames.push_back(f.size() == plan->cagePointCount() ? plan->refinePoints(f) : std::vector<Vec3f>{});
        mHistory = ScalpHistory::make(in.step, std::move(frames), in.loopSeconds);
        mHistoryIn = scalpHistory;
        mHistoryOn = mCageScalp;
    }
}

// Moving inputs.

void GroomEvaluator::setGuides(GuidesPtr guides)
{
    mData.guides = std::move(guides);
}

void GroomEvaluator::setGuidePoses(GuidesPtr poses)
{
    if (poses == mData.guidePoses)
        return;
    // The same poses read again keep the posed hierarchy.
    const std::uint64_t hash = poses ? poses->contentHash() : 0;
    if (poses && mData.guidePoses && hash == mPosesHash)
        return;
    mData.guidePoses = std::move(poses);
    mPosesHash = hash;
}

void GroomEvaluator::setControls(std::map<std::string, float> controls)
{
    mData.controls = std::move(controls);
}

void GroomEvaluator::setMeshes(std::map<std::string, MeshPtr> meshes)
{
    std::erase_if(meshes, [](const auto& m) { return !m.second; });
    mData.meshes = std::move(meshes);
}

void GroomEvaluator::setMesh(const std::string& name, MeshPtr mesh)
{
    if (mesh)
        mData.meshes[name] = std::move(mesh);
    else
        mData.meshes.erase(name);
}

void GroomEvaluator::setScalpHistory(ScalpHistoryPtr history)
{
    mData.scalpHistory = std::move(history);
}

void GroomEvaluator::setPathResolver(PathResolver resolver)
{
    mPaths = resolver ? std::make_shared<const PathResolver>(std::move(resolver)) : nullptr;
}

void GroomEvaluator::setPathResolver(std::shared_ptr<const PathResolver> resolver)
{
    mPaths = std::move(resolver);
}

const PathResolver* GroomEvaluator::paths() const
{
    return mPaths && *mPaths ? mPaths.get() : nullptr;
}

void GroomEvaluator::setCacheIntermediates(bool enabled)
{
    mCacheIntermediates = enabled;
    for (auto* doc : {&mDoc, &mSculptDoc, &mCardDoc, &mFullDoc})
        if (*doc)
            (*doc)->graph.setCacheIntermediates(enabled);
    for (auto& [density, doc] : mMeshDocs)
        doc->graph.setCacheIntermediates(enabled);
    if (mRestPose)
        mRestPose->setCacheIntermediates(enabled);
}

void GroomEvaluator::setFramesPerSecond(double fps)
{
    fps = canonicalFramesPerSecond(fps > 0.0 && std::isfinite(fps) ? fps : 24.0);
    if (fps == mFps)
        return;
    mFps = fps;
    for (auto& [name, node] : mSims)
        node->cache.clear();
}

void GroomEvaluator::setFollowScalp(bool on)
{
    mData.followScalp = on;
}

bool GroomEvaluator::readsTime() const
{
    if (mDoc && graphReadsTime(mDoc->graph, mDoc->output))
        return true;
    if (mData.scalpHistory)
        return true;
    for (const CorrectiveLayer& l : mCorrectives.layers)
    {
        if (!l.enabled || l.offsets.empty())
            continue;
        if (l.slot >= 0 && mLiveWeights)
            continue; // the host's live weight: the host knows whether it moves
        if (!l.weightCurve.empty())
        {
            for (const auto& [seconds, w] : l.weightCurve)
                if (w != l.weightCurve.front().second)
                    return true;
            continue;
        }
        if (!(l.holdBefore && l.holdAfter))
            return true; // it fades in or out
    }
    return false;
}

// Previews.

void GroomEvaluator::setPreviewHierarchy(std::shared_ptr<GuideHierarchy> preview, bool final)
{
    mPreviewHierarchy = std::move(preview);
    mPreviewFinal = mPreviewHierarchy && final;
}

void GroomEvaluator::setPreviewOverlays(std::shared_ptr<const PartSet> parts, std::shared_ptr<const FlowSet> flow)
{
    mPreviewParts = std::move(parts);
    mPreviewFlow = std::move(flow);
}

// Guides.

HierarchyPtr GroomEvaluator::rootedHierarchy()
{
    // On the scalp as given (refined when subdivided): painting a map changes color sets, never where guides sit.
    MeshPtr on = joinedScalp();
    if (on && scalpSubdivision() > 0)
    {
        std::vector<std::string> ignored;
        bindScalp(ignored);
        on = mScalp;
    }
    const std::uint64_t scalpKey = on ? on->surfaceHash() : 0;
    if (mRooted && mRootedVersion == mParsedVersion && scalpKey == mRootedScalp)
        return mHierarchy;
    mRooted = true;
    mRootedVersion = mParsedVersion;
    mRootedScalp = scalpKey;
    mHierarchy.reset();
    if (!mParsedHierarchy)
        return mHierarchy;
    std::shared_ptr<const GuideHierarchy> source = mParsedHierarchy;
    // Saved bound to another topology: bound again once on the rest pose, where the saved shapes belong, so the guides
    // follow a moving scalp instead of re-rooting at the nearest point each frame.
    const std::uint64_t bound = source->boundTopology();
    if (on && on->hasRest() && bound != 0 && bound != on->topologyHash())
    {
        const std::uint64_t restKey = hashCombine(on->restPoseHash(), on->topologyHash());
        if (mReboundFrom != source || mReboundRest != restKey)
        {
            auto rebound = std::make_shared<GuideHierarchy>(*source);
            const ScalpMesh rest = on->restPose();
            rebound->evaluate(&rest);
            mReboundHierarchy = std::move(rebound);
            mReboundFrom = source;
            mReboundRest = restKey;
        }
        source = mReboundHierarchy;
    }
    auto rooted = std::make_shared<GuideHierarchy>(*source);
    rooted->evaluate(on.get());
    mHierarchy = rooted;
    return mHierarchy;
}

std::uint64_t GroomEvaluator::correctivesKey(std::uint64_t key) const
{
    key = hashCombine(hashCombine(key, mCorrectivesVersion), static_cast<std::uint64_t>(mLiveWeights));
    for (const auto& [slot, w] : mWeights)
        key = hashFloat(hashCombine(key, static_cast<std::uint64_t>(slot)), w);
    return hashCombine(key, doubleBits(mTime));
}

void GroomEvaluator::applyCorrectives(GuideHierarchy& guides, const ScalpMesh* on) const
{
    if (!mLiveWeights)
    {
        mCorrectives.apply(guides, mTime, on);
        return;
    }
    Correctives live = mCorrectives;
    useLiveWeights(live, [&](int slot) {
        const auto it = mWeights.find(slot);
        return it == mWeights.end() ? 1.0f : it->second;
    });
    live.apply(guides, mTime, on);
}

void GroomEvaluator::updateActiveHierarchy()
{
    const HierarchyPtr base = mPreviewHierarchy ? HierarchyPtr(mPreviewHierarchy) : rootedHierarchy();
    if (mPreviewFinal)
    {
        mActiveHierarchy = base;
        mActiveValid = false;
        return;
    }
    const GuidesPtr& poses = mData.guidePoses;
    const bool posing = poses && base;
    const bool correcting = base && !mCorrectives.empty();
    std::uint64_t key = hashCombine(hash64(0xAC7E), reinterpret_cast<std::uintptr_t>(base.get()));
    key = hashCombine(key, mRootedVersion);
    key = hashCombine(key, mScalp ? mScalp->surfaceHash() : 0);
    key = hashCombine(key, posing ? mPosesHash : 0);
    if (correcting)
        key = correctivesKey(key);
    // A preview is edited in place between evaluations: always new.
    if (!mPreviewHierarchy && mActiveValid && key == mActiveKey)
        return;
    mActiveKey = key;
    mActiveValid = true;
    mPosedHierarchy = base;
    mActiveHierarchy = base;
    if (!posing && !correcting)
        return;
    auto working = std::make_shared<GuideHierarchy>(*base);
    if (posing)
    {
        applyGuidePoses(*working, *poses, mScalp.get());
        mPosedHierarchy = correcting ? nullptr : HierarchyPtr(working); // both: posedHierarchy() makes it on demand
    }
    if (correcting)
        applyCorrectives(*working, mScalp.get());
    mActiveHierarchy = working;
}

HierarchyPtr GroomEvaluator::hierarchy()
{
    if (!mParsedHierarchy || (mHierarchyFromText && blank(mData.hierarchyJson) && !mPreviewHierarchy))
        return nullptr;
    std::vector<std::string> errors;
    bindScalp(errors);
    updateActiveHierarchy();
    return mActiveHierarchy;
}

HierarchyPtr GroomEvaluator::posedHierarchy()
{
    if (!mPosedHierarchy && mData.guidePoses)
    {
        const HierarchyPtr base = mPreviewHierarchy ? HierarchyPtr(mPreviewHierarchy) : mHierarchy;
        if (base)
        {
            auto posed = std::make_shared<GuideHierarchy>(*base);
            applyGuidePoses(*posed, *mData.guidePoses, mScalp.get());
            mPosedHierarchy = posed;
        }
    }
    return mPosedHierarchy;
}

// Parts and flow.

void GroomEvaluator::bindOverlays()
{
    // Stored parts and flow are bound to the cage, so a new subdivision level leaves them where they were drawn; a refined
    // scalp gets them carried onto it. A preview is on the scalp as evaluated already.
    const std::shared_ptr<const SubdivisionPlan> plan =
        mScalp && mCageScalp && mScalp != mCageScalp ? refinementPlan(*mCageScalp, *mScalp) : nullptr;
    const std::uint64_t planKey =
        plan ? hashCombine(hashCombine(hashCombine(hashCombine(hash64(0xCA6E), mCageScalp->topologyHash()), mCageScalp->polygonHash()),
                                       static_cast<std::uint64_t>(plan->levels())),
                           mCageScalp->piecesHash())
             : 0;
    mShownParts = &mPartSet;
    if (mPreviewParts)
        mShownParts = mPreviewParts.get();
    else if (plan && !mPartSet.empty())
    {
        const std::uint64_t key = hashCombine(mPartSet.contentHash(), planKey);
        if (key != mRefinedPartsKey)
        {
            mRefinedParts = mPartSet.onRefined(*plan);
            mRefinedPartsKey = key;
        }
        mShownParts = &mRefinedParts;
    }
    mShownFlow = &mFlowSet;
    if (mPreviewFlow)
        mShownFlow = mPreviewFlow.get();
    else if (plan && !mFlowSet.empty())
    {
        // Directions turned on the rest pose where there is one: the same on every frame.
        const std::uint64_t key = hashCombine(hashCombine(mFlowSet.contentHash(), planKey), mCageScalp->restSurfaceHash());
        if (key != mRefinedFlowKey)
        {
            mRefinedFlow = mCageScalp->hasRest() ? mFlowSet.onRefined(*plan, mCageScalp->restPose(), mScalp->restPose())
                                                 : mFlowSet.onRefined(*plan, *mCageScalp, *mScalp);
            mRefinedFlowKey = key;
        }
        mShownFlow = &mRefinedFlow;
    }

    const std::uint64_t scalpHash = mScalp ? mScalp->surfaceHash() : 0; // color sets play no part
    const PartSet& parts = *mShownParts;
    const std::uint64_t partsKey = hashCombine(parts.contentHash(), scalpHash);
    if (partsKey != mPartsKey || (!mParts && !parts.empty() && mScalp) || (mParts && (parts.empty() || !mScalp)))
    {
        mParts = !parts.empty() && mScalp ? std::make_shared<const PartLines>(parts, *mScalp) : nullptr;
        mPartsKey = partsKey;
    }
    const FlowSet& flow = *mShownFlow;
    const std::uint64_t flowKey = hashCombine(flow.contentHash(), scalpHash);
    if (flowKey != mFlowKey || (!mFlow && !flow.empty() && mScalp) || (mFlow && (flow.empty() || !mScalp)))
    {
        mFlow = !flow.empty() && mScalp ? std::make_shared<const FlowField>(flow, *mScalp) : nullptr;
        mFlowKey = flowKey;
    }
}

// Following the scalp.

void GroomEvaluator::prepareFollow(std::vector<Message>& messages)
{
    mFollowing = false;
    if (!mData.followScalp || !mScalp)
        return;
    if (!mScalp->hasRest())
    {
        if (!mFollowNoRestSaid)
        {
            messages.push_back({"followScalp: the scalp has no rest pose (give the groom a rest mesh); the groom evaluates on the scalp as it is",
                                false});
            mFollowNoRestSaid = true;
        }
        return;
    }
    mFollowNoRestSaid = false;
    if (mScalp->points() == mScalp->restPoints())
        return; // at rest: as it is, to the bit
    mFollowing = true;

    const std::uint64_t restKey = mScalp->restPoseHash();
    if (!mFollowRest || restKey != mFollowRestKey)
    {
        mFollowRest = std::make_shared<const ScalpMesh>(mScalp->restPose());
        mFollowRestKey = restKey;
    }
    const std::uint64_t restSurface = mFollowRest->surfaceHash();

    const std::uint64_t rootedKey = hashCombine(hashCombine(hash64(0xF0110), mParsedVersion), restSurface);
    if (mFollowRootedKey != rootedKey || (mParsedHierarchy && !mFollowRooted))
    {
        mFollowRooted.reset();
        if (mParsedHierarchy)
        {
            auto rooted = std::make_shared<GuideHierarchy>(*mParsedHierarchy);
            rooted->evaluate(mFollowRest.get());
            mFollowRooted = rooted;
        }
        mFollowRootedKey = rootedKey;
    }

    // The active guides at rest: the preview taken back or the rooted guides, then the poses (world space now, taken
    // back by their roots) and the correctives.
    const bool previewing = mPreviewHierarchy != nullptr;
    const GuidesPtr& poses = mData.guidePoses;
    const bool correcting = !mCorrectives.empty();
    std::uint64_t key = hashCombine(hash64(0xF011AC), reinterpret_cast<std::uintptr_t>(mFollowRooted.get()));
    key = hashCombine(hashCombine(key, mFollowRootedKey), mScalp->surfaceHash());
    key = hashCombine(key, poses ? mPosesHash : 0);
    if (correcting)
        key = correctivesKey(key);
    if (previewing || key != mFollowActiveKey)
    {
        mFollowActiveKey = key;
        HierarchyPtr base = mFollowRooted;
        if (previewing)
            base = carryHierarchyBack(*mPreviewHierarchy, *mFollowRest, *mScalp);
        if (mPreviewFinal || !base || (!(poses && base) && !correcting))
            mFollowActive = base;
        else
        {
            auto working = std::make_shared<GuideHierarchy>(*base);
            if (poses)
                applyGuidePoses(*working, *carryPosesBack(poses, *base, *mFollowRest, *mScalp), mFollowRest.get());
            if (correcting)
                applyCorrectives(*working, mFollowRest.get());
            mFollowActive = working;
        }
    }

    const PartSet& parts = *mShownParts; // bound to mScalp, so to its rest pose too
    const std::uint64_t partsKey = hashCombine(parts.contentHash(), restSurface);
    if (partsKey != mFollowPartsKey || (!mFollowParts && !parts.empty()) || (mFollowParts && parts.empty()))
    {
        mFollowParts = parts.empty() ? nullptr : std::make_shared<const PartLines>(parts, *mFollowRest);
        mFollowPartsKey = partsKey;
    }
    const FlowSet& flow = *mShownFlow;
    const std::uint64_t flowKey = hashCombine(flow.contentHash(), restSurface);
    if (flowKey != mFollowFlowKey || (!mFollowFlow && !flow.empty()) || (mFollowFlow && flow.empty()))
    {
        mFollowFlow = flow.empty() ? nullptr : std::make_shared<const FlowField>(flow, *mFollowRest);
        mFollowFlowKey = flowKey;
    }

    if (mGuides != mFollowGuidesIn || mScalp != mFollowGuidesOn || !mGuides)
    {
        mFollowGuides = mGuides ? carryGuidesBack(mGuides, *mFollowRest, *mScalp) : nullptr;
        mFollowGuidesIn = mGuides;
        mFollowGuidesOn = mScalp;
    }
    if (mHistory != mFollowHistoryIn || mScalp != mFollowHistoryOn)
    {
        mFollowHistory = mHistory ? historyAtRest(*mHistory, *mFollowRest, *mScalp) : nullptr;
        mFollowHistoryIn = mHistory;
        mFollowHistoryOn = mScalp;
    }
}

StrandsPtr GroomEvaluator::carry(const StrandsPtr& strands, int slot)
{
    if (!mFollowing || !strands)
        return strands;
    Carried& c = mCarried[slot];
    if (c.out && c.in == strands && c.from == mFollowRest && c.to == mScalp)
        return c.out;
    c.in = strands;
    c.from = mFollowRest;
    c.to = mScalp;
    c.out = carryStrands(strands, *mFollowRest, *mScalp);
    return c.out;
}

// Evaluation.

void GroomEvaluator::prepareTime(double seconds, std::vector<std::string>& errors, std::vector<Message>& messages)
{
    bindScalp(errors);
    bindRefined();
    rootedHierarchy();
    mTime = seconds;
    updateActiveHierarchy();
    bindOverlays();
    prepareFollow(messages);
}

void GroomEvaluator::fillExternals(ExternalInputs& externals) const
{
    for (const auto& [name, node] : mSims)
        if (node->shown)
            externals.set(simulationExternal(name), node->shown, node->shownHash);
    for (const auto& [name, edits] : mSculptExternals)
        if (!mPreviewSculpt || name != mPreviewSculptNode)
            externals.set(sculptExternal(name), edits.first, edits.second);
    if (mPreviewSculpt)
        externals.set(sculptExternal(mPreviewSculptNode), mPreviewSculpt, mPreviewSculptHash);
    if (mFollowing)
        setGroomExternals(externals, mFollowRest, mFollowGuides, mFollowActive, mFollowParts, mFollowFlow);
    else
        setGroomExternals(externals, mScalp, mGuides, mActiveHierarchy, mParts, mFlow);
    setMeshExternals(externals, mData.meshes);
    setControlExternals(externals, scaledControls());
    setMapExternals(externals, mData.maps);
    setScalpHistoryExternal(externals, mFollowing ? mFollowHistory : mHistory);
}

EvalContext GroomEvaluator::context(ExternalInputs& externals) const
{
    EvalContext ctx;
    ctx.framesPerSecond = mFps;
    ctx.scale = mGroomScale; // the lattices sizes round to
    ctx.externals = &externals;
    ctx.paths = paths();
    return ctx;
}

StrandsPtr GroomEvaluator::pinToRest(const StrandsPtr& strands, const std::string& viewedNode, const EvalContext& ctx)
{
    // Instances pinned or fitted to their rest length: the rest pose, on its own copy of the graph, gives each its
    // distance from the root (a cache hit from frame to frame).
    if (!strands || !strands->instances() || !strands->instances()->needsRestPose())
    {
        mRestPose.reset();
        return strands;
    }
    if (!mRestPose)
    {
        mRestPose = std::make_unique<RestPose>(mParsed->clone());
        mRestPose->setCacheIntermediates(mCacheIntermediates);
    }
    RestInputs rest;
    rest.scalp = mFollowing ? mFollowRest : mScalp;
    if (mPreviewHierarchy && !mPreviewFinal)
    {
        rest.hierarchy = mFollowing ? HierarchyPtr(carryHierarchyBack(*mPreviewHierarchy, *mFollowRest, *mScalp)) : HierarchyPtr(mPreviewHierarchy);
        rest.hierarchyOnScalp = true;
    }
    else if (!(mHierarchyFromText && blank(mData.hierarchyJson)))
        rest.hierarchy = mParsedHierarchy;
    rest.guides = mFollowing ? mFollowGuides : mGuides;
    rest.parts = mShownParts;
    rest.flow = mShownFlow;
    rest.controls = scaledControls();
    rest.maps = mData.maps;
    return mRestPose->pin(strands, viewedNode, rest, ctx);
}

const GroomEvaluator::Result& GroomEvaluator::evaluate(const Request& request)
{
    std::vector<std::string> errors;
    std::vector<Message> messages;
    mFailure = nullptr;
    if (mParsedVersion != 0 && mReportedVersion != mParsedVersion)
    {
        mReportedVersion = mParsedVersion;
        if (!mHierarchyError.empty())
            messages.push_back({mHierarchyError, true});
    }

    const HierarchyPtr previousActive = mActiveHierarchy;
    const PartsPtr previousParts = mParts;
    const FlowPtr previousFlow = mFlow;
    prepareTime(request.time, errors, messages);
    messages.insert(messages.begin(), mDetachedMessages.begin(), mDetachedMessages.end());
    mDetachedMessages.clear();
    stepSimulations(request, errors);
    // A live brush stroke edits the preview in place: every evaluation is a change.
    bool hierarchyChanged = mActiveHierarchy.get() != previousActive.get() || mPreviewHierarchy != nullptr;
    const HierarchyPtr previousShown = mResult.hierarchy;
    for (const std::string* e : {&mCorrectiveError, &mPartError, &mFlowError})
        if (!e->empty())
            errors.push_back(*e);

    ExternalInputs externals;
    fillExternals(externals);
    EvalContext ctx = context(externals);
    ctx.previewDensity = request.density;
    // While a stroke is live, fewer strands keep the viewport responsive, on a document of their own (the view's cache
    // stays for the release).
    if (hasPreview())
        ctx.previewDensity *= request.sculptDensity;
    if (mLiveStroke)
        mStrokeDensityUsed = true;
    const bool sculpting = hasPreview() && ctx.previewDensity != request.density && mDoc;
    if (sculpting && !mSculptDoc)
        mSculptDoc = newDocument();
    mViewSculpt = sculpting;
    GraphDocument* const viewed = sculpting ? mSculptDoc.get() : mDoc.get();
    ctx.maxPoints = request.pointBudget;
    ctx.time = request.time;
    ctx.viewport = request.viewport;
    ctx.liveDrag = mLiveDrag;
    ctx.buildCards = request.buildCards;
    mLastViewContext = ctx;
    mLastViewContext.previewDensity = request.density;
    mLastViewContext.externals = nullptr;
    mLastViewContext.paths = nullptr;

    Result& out = mResult;
    out.density = std::clamp(ctx.previewDensity, 0.0f, 1.0f);
    const StrandsPtr previousStrands = out.strands;
    const std::uint64_t previousSerial = out.strandsSerial;
    const std::shared_ptr<const CardMesh> previousCards = out.cards;
    const std::uint64_t previousStrandMeshes = out.versions.strandMeshes;
    const std::string previousView = out.viewedNode;
    const bool previousRoots = out.viewedRoots;
    const std::shared_ptr<const std::vector<Vec3f>> previousCenters = out.clumpCenters;
    const FollowCarryPtr previousFollow = out.follow;
    out.strands.reset();
    out.follow.reset();
    NodeId stopAt = 0;
    if (viewed && !request.stopAt.empty())
        if (const auto it = viewed->nodesByName.find(request.stopAt); it != viewed->nodesByName.end())
            stopAt = it->second;
    auto failed = [&](const char* what) {
        mShown[0] = mShown[1] = Shown{};
        out.strandsSerial = out.changesBase = 0;
        out.strandsChanges.reset();
        mFailure = std::current_exception();
        errors.push_back(what);
        messages.push_back({what, true});
    };
    if (stopAt)
    {
        // The host runs this node and what follows: what feeds it, and the strands as they were.
        try
        {
            const std::size_t slots = viewed->graph.inputSpecs(stopAt).size();
            for (std::size_t slot = 0; slot < slots; ++slot)
            {
                NodeId source = 0;
                std::size_t output = 0;
                if (viewed->graph.inputSource(stopAt, slot, &source, &output))
                    viewed->graph.evaluateOutput(source, output, ctx);
            }
            out.strands = previousStrands;
            out.follow = previousFollow;
        }
        catch (const std::exception& e)
        {
            mFailure = std::current_exception();
            errors.push_back(e.what());
            messages.push_back({e.what(), true});
        }
    }
    else if (viewed)
    {
        try
        {
            NodeId shown = viewed->output;
            out.viewedNode.clear();
            out.viewedRoots = false;
            NodeId centersOf = 0;
            const auto it = viewed->nodesByName.find(request.viewNode);
            if (!request.viewNode.empty() && it != viewed->nodesByName.end() && viewed->graph.outputType(it->second) == PortType::Strands)
            {
                shown = it->second;
                out.viewedNode = request.viewNode;
                out.viewedRoots = viewed->graph.node(shown).typeName() == "Scatter";
                if (viewed->graph.node(shown).typeName() == op::kClump)
                    centersOf = shown;
            }
            const StrandsPtr evaluated = viewed->graph.evaluateStrands(shown, ctx);
            out.movingGuides = groomEvaluator::movingGuidesOf(viewed->graph, shown, ctx);
            if (centersOf)
            {
                const std::shared_ptr<const std::vector<Vec3f>> dots = clumpCenterDots(*viewed, centersOf, ctx);
                if (!previousCenters || *previousCenters != *dots)
                    out.clumpCenters = dots;
            }
            else
            {
                out.clumpCenters.reset();
            }
            const StrandsPtr strands = pinToRest(evaluated, out.viewedNode, ctx);
            if (mFollowing && request.hostCarries && strands && carriesPointsAndInstances(*strands))
            {
                out.strands = strands;
                const bool same = previousFollow && previousFollow->rest() == strands && previousFollow->from() == mFollowRest &&
                                  previousFollow->to() == mScalp;
                out.follow = same ? previousFollow : std::make_shared<const FollowCarry>(strands, mFollowRest, mScalp);
            }
            else
            {
                out.strands = carry(strands, 0);
            }
            // Which strands these are, and how they differ from the ones this document gave last: the graph's changes of
            // the node shown, when the view hands its strands on as evaluated (not carried, nor pinned to rest).
            Shown& was = mShown[sculpting ? 1 : 0];
            const std::uint64_t graphSerial = viewed->graph.resultSerial(shown);
            const bool asEvaluated = out.strands == evaluated && !mFollowing && graphSerial != 0;
            if (asEvaluated && was.serial != 0 && was.node == shown && was.graphSerial == graphSerial && was.strands.lock() == out.strands)
            {
                out.strandsSerial = was.serial;
                out.changesBase = was.serial;
                out.strandsChanges = viewed->graph.changesSince(shown, graphSerial);
            }
            else
            {
                out.strandsSerial = ++mStrandsSerial;
                out.changesBase = was.serial;
                out.strandsChanges =
                    asEvaluated && was.serial != 0 && was.node == shown ? viewed->graph.changesSince(shown, was.graphSerial) : nullptr;
                was.serial = asEvaluated ? out.strandsSerial : 0;
                was.node = shown;
                was.graphSerial = graphSerial;
                was.strands = out.strands;
            }
            if (request.wantCards)
            {
                // Mid-stroke too while a rebuild is quick enough to follow the brush; slower ones catch up at the end.
                if (!hasPreview() || mLastCardBuildMs <= kLiveCardBuildBudgetMs)
                    buildCards(ctx, request.cardDensity, messages);
            }
            else
            {
                mCardDoc.reset();
                out.cardStrands.reset();
                out.cards.reset();
                out.cardNodes.clear();
            }
            if (request.wantStrandMeshes)
            {
                if (!hasPreview() || mLastStrandMeshMs <= kLiveCardBuildBudgetMs)
                {
                    const auto started = std::chrono::steady_clock::now();
                    std::vector<StrandMeshSource> meshes = evaluateStrandMeshes(request.time);
                    mLastStrandMeshMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                    if (!sameStrandMeshes(meshes, out.strandMeshes))
                    {
                        out.strandMeshes = std::move(meshes);
                        ++out.versions.strandMeshes;
                    }
                }
            }
            else if (!out.strandMeshes.empty())
            {
                out.strandMeshes.clear();
                ++out.versions.strandMeshes;
            }
        }
        catch (const std::exception& e)
        {
            failed(e.what());
        }
        catch (...)
        {
            failed("evaluation failed");
        }
    }
    if (!mGraphError.empty())
        errors.insert(errors.begin(), mGraphError);
    if (!mHierarchyError.empty())
        errors.insert(errors.begin(), mHierarchyError);
    if (!mSculptError.empty())
        errors.push_back(mSculptError);

    // A Sculpt node brushes shape: its output is the guides shown (a live stroke's working guides).
    HierarchyPtr shownHierarchy = mActiveHierarchy;
    if (!mSculptTarget.empty())
    {
        mSculptShownVersion = mSculptsVersion;
        if (mPreviewSculptShown && mPreviewSculptNode == mSculptTarget)
        {
            shownHierarchy = mPreviewSculptShown;
            hierarchyChanged = true;
        }
        else if (viewed)
        {
            const GuidesPtr output = sculptOutput(*viewed, ctx);
            if (!output)
            {
                mSculptShown.reset();
                mSculptShownFrom.reset();
            }
            else if (output != mSculptShownFrom || mScalp != mSculptShownOn || !mSculptShown)
            {
                mSculptShown = groomlab::sculptHierarchy(*output, mSculptTarget, mFollowing ? nullptr : mScalp.get());
                mSculptShownFrom = output;
                mSculptShownOn = mScalp;
            }
            if (mSculptShown)
                shownHierarchy = mSculptShown;
        }
    }
    if (shownHierarchy != previousShown)
        hierarchyChanged = true;

    // What changed, for drawing: the same cached results (a static groom at another frame) leave the versions alone.
    auto carriedTo = [](const FollowCarryPtr& f) { return f ? f->to().get() : nullptr; };
    const bool strandsChanged = out.strands.get() != previousStrands.get() || carriedTo(out.follow) != carriedTo(previousFollow) ||
                                out.viewedNode != previousView || out.viewedRoots != previousRoots || out.clumpCenters != previousCenters;
    const bool cardsChanged = out.cards.get() != previousCards.get();
    const bool overlaysChanged = mParts.get() != previousParts.get() || mFlow.get() != previousFlow.get();
    if (strandsChanged)
        ++out.versions.strands;
    if (hierarchyChanged)
        ++out.versions.hierarchy;
    if (cardsChanged)
        ++out.versions.cards;
    if ((strandsChanged || hierarchyChanged) && !request.wantBounds)
        mBoundsStale = true;
    if ((strandsChanged || hierarchyChanged || mBoundsStale) && request.wantBounds)
    {
        mBoundsStale = false;
        constexpr float kBig = std::numeric_limits<float>::max();
        Vec3f lo(kBig, kBig, kBig), hi(-kBig, -kBig, -kBig);
        auto expand = [&](const Vec3f& p) {
            lo = Vec3f(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
            hi = Vec3f(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
        };
        if (out.strands)
        {
            // Only some strands changed since the view's last: their blocks measured again. Handed on at rest: each
            // strand's box at rest, where carrying puts it.
            const bool some = out.strandsChanges && !out.strandsChanges->all && previousStrands && out.changesBase == previousSerial;
            const StrandSet::Extent e = out.follow ? carriedBounds(*out.strands, followBoxes(out.strands), *out.follow->to())
                                        : some     ? out.strands->extentAfter(*previousStrands, out.strandsChanges->strands)
                                                   : out.strands->extent();
            if (!e.finite)
            {
                const std::string what = nonFiniteMessage(sculpting ? mSculptDoc.get() : mDoc.get());
                errors.push_back(what);
                messages.push_back({what, true});
            }
            if (!e.empty() && e.finite)
            {
                expand(e.lo);
                expand(e.hi);
            }
        }
        for (const HierarchyPtr& h : {mActiveHierarchy, shownHierarchy != mActiveHierarchy ? shownHierarchy : nullptr})
            if (h)
                for (const HierarchyGuide& g : h->guides())
                    for (const Vec3f& p : g.points)
                        expand(p);
        out.bounds.valid = lo.x <= hi.x;
        out.bounds.lo = out.bounds.valid ? lo : Vec3f();
        out.bounds.hi = out.bounds.valid ? hi : Vec3f();
    }
    const bool strandMeshesChanged = out.versions.strandMeshes != previousStrandMeshes;
    if (strandsChanged || hierarchyChanged || cardsChanged || overlaysChanged || strandMeshesChanged)
        ++out.versions.evaluation;

    out.scalp = mScalp;
    out.cageScalp = mCageScalp;
    out.hierarchy = shownHierarchy;
    out.authoredHierarchy = mHierarchy;
    out.parts = mParts;
    out.flow = mFlow;
    out.errors = std::move(errors);
    out.messages = std::move(messages);
    return out;
}

StrandsPtr GroomEvaluator::evaluate(float density, double seconds, bool buildCards)
{
    Request request;
    request.density = density;
    request.time = seconds;
    request.buildCards = buildCards;
    request.wantBounds = false;
    const Result& result = evaluate(request);
    if (mFailure)
        std::rethrow_exception(mFailure);
    return result.strands;
}

Value GroomEvaluator::evaluateNode(const std::string& name, float density, double seconds, bool buildCards)
{
    if (!mDoc)
        throw std::runtime_error(mGraphError.empty() ? "the groom has no graph" : mGraphError);
    // A scratch copy: results matching the evaluator's are reused, and nothing made here evicts them.
    GraphDocument scratch = mParsed->clone();
    scratch.graph.adoptCache(mDoc->graph);
    const NodeId id = name.empty() ? scratch.output : scratch.node(name);
    ExternalInputs externals;
    fillExternals(externals);
    EvalContext ctx = context(externals);
    ctx.previewDensity = std::clamp(density, 0.0f, 1.0f);
    ctx.time = seconds;
    ctx.buildCards = buildCards;
    Value value = scratch.graph.evaluate(id, ctx);
    if (const StrandsPtr strands = valueAs<StrandsPtr>(value))
        return Value(carry(strands, 3));
    return value;
}

// What the last evaluation made.

int GroomEvaluator::scalpHistoryFrames(const std::string& viewNode) const
{
    if (!mDoc)
        return 0;
    int frames = groomlab::scalpHistoryFrames(mDoc->graph, mDoc->output);
    if (!viewNode.empty())
        if (const auto viewed = mDoc->nodesByName.find(viewNode); viewed != mDoc->nodesByName.end())
            frames = std::max(frames, groomlab::scalpHistoryFrames(mDoc->graph, viewed->second));
    return frames;
}

std::vector<double> GroomEvaluator::scalpHistoryLoopFrames(double now, const std::string& viewNode) const
{
    if (!mDoc)
        return {};
    const int plain = scalpHistoryFrames(viewNode);
    std::vector<double> frames = groomlab::scalpHistoryLoopFrames(mDoc->graph, mDoc->output, now);
    if (!viewNode.empty())
        if (const auto viewed = mDoc->nodesByName.find(viewNode); viewed != mDoc->nodesByName.end())
        {
            const std::vector<double> more = groomlab::scalpHistoryLoopFrames(mDoc->graph, viewed->second, now);
            frames.insert(frames.end(), more.begin(), more.end());
        }
    // Past the frames the history holds anyway.
    std::erase_if(frames, [&](double f) {
        const double back = now - f;
        return std::abs(back - std::round(back)) < 1e-3 && back > -0.5 && back < plain + 0.5;
    });
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end(), [](double a, double b) { return std::abs(a - b) < 1e-6; }), frames.end());
    return frames;
}

HairLook GroomEvaluator::hairLook() const
{
    HairLook look;
    if (!mDoc)
        return look;
    if (const Operator* node = activeNode(*mDoc, "HairColor", &look.node))
        if (node->params().getString("mode", "melanin") == "melanin")
        {
            const Vec3f dye = node->params().getVec3("dye");
            look.dye = {dye.x, dye.y, dye.z};
        }
    auto on = [](const ParamSet& p, const char* name) {
        return p.has(name) && (std::holds_alternative<MapRef>(p.get(name)) || p.getDouble(name, 0.0) > 0.0);
    };
    bool highlight = false;
    mDoc->graph.forEachActiveUpstream(mDoc->output, [&](const std::string&, const Operator& op) {
        const ParamSet& p = op.params();
        if (op.typeName() == "HairColor" && !highlight)
        {
            highlight = true;
            // A param set to a map takes its default.
            auto number = [&](const char* name, float fallback) {
                return p.has(name) && std::holds_alternative<MapRef>(p.get(name)) ? fallback : static_cast<float>(p.getDouble(name, fallback));
            };
            look.specular = number("specular", kDefaultHairSpecular);
            look.roughness = number("roughness", kDefaultHairRoughness);
            look.specularRandom = number("specularRandom", 0.0f);
            look.roughnessRandom = number("roughnessRandom", 0.0f);
            look.tilt = number("tilt", kDefaultHairTilt);
        }
        if (op.typeName() == "FeatherColor")
        {
            if (!look.iridescent && on(p, "iridescence"))
            {
                look.iridescent = true;
                const Vec3f c = p.getVec3("iridescenceColor");
                look.iridescenceColor = {c.x, c.y, c.z};
                look.iridescenceShift = static_cast<float>(p.getDouble("iridescenceShift"));
            }
            look.colorsByRgb = true;
        }
        if (op.typeName() == "Bands" || op.typeName() == "HairColor")
            look.colorsByRgb = true;
        if (op.typeName() == "Wet" || (op.typeName() == "Attribute" && p.getString("name") == kWetnessChannel))
            look.wet = true;
        if (op.typeName() == "Simulate" && (p.getBool("water", false) || p.getDouble("rain", 0.0) > 0.0))
            look.wet = true;
        return true;
    });
    look.colorBlends = !colorBlendNodes(*mDoc).empty();
    return look;
}

std::string GroomEvaluator::hairLookJson() const
{
    const HairLook look = hairLook();
    const HairLobes lobes = hairLobes(look);
    nlohmann::json out = {{"node", look.node},
                          {"specular", look.specular},
                          {"roughness", look.roughness},
                          {"specularRandom", look.specularRandom},
                          {"roughnessRandom", look.roughnessRandom},
                          {"tilt", look.tilt},
                          {"dye", {look.dye[0], look.dye[1], look.dye[2]}},
                          {"colorsByRgb", look.colorsByRgb},
                          {"colorBlends", look.colorBlends},
                          {"iridescent", look.iridescent},
                          {"iridescenceColor", {look.iridescenceColor[0], look.iridescenceColor[1], look.iridescenceColor[2]}},
                          {"iridescenceShift", look.iridescenceShift},
                          {"wet", look.wet},
                          {"lobes",
                           {{"specularTint", lobes.specularTint},
                            {"roughness", lobes.roughness},
                            {"azimuthalRoughness", lobes.azimuthalRoughness},
                            {"ior", lobes.ior},
                            {"shift", lobes.shiftDegrees}}}};
    nlohmann::json hairColor, renderData, colors = nlohmann::json::array();
    if (mDoc)
        mDoc->graph.forEachActiveUpstream(mDoc->output, [&](const std::string& at, const Operator& op) {
            const std::string& type = op.typeName();
            if (type == "HairColor" && hairColor.is_null())
                hairColor = nlohmann::json::parse(paramsJson(op.params()));
            if (type == "RenderData" && renderData.is_null())
                renderData = nlohmann::json::parse(paramsJson(op.params()));
            if (colorNode(type))
                colors.push_back({{"path", at}, {"type", type}, {"params", nlohmann::json::parse(paramsJson(op.params()))}});
            return true;
        });
    out["hairColorParams"] = hairColor;
    out["renderDataParams"] = renderData;
    out["colorNodes"] = colors;
    return out.dump();
}

std::string GroomEvaluator::Stats::toJson() const
{
    nlohmann::json out;
    out["view"] = view;
    out["strands"] = strands;
    out["points"] = points;
    out["guides"] = guides;
    out["cards"] = cards;
    out["cardStrands"] = cardStrands;
    out["cardTriangles"] = cardTriangles;
    out["cardTextures"] = cardTextures;
    out["cardFeathers"] = cardFeathers;
    out["cardsBaked"] = cardsBaked;
    out["cardsStale"] = cardsStale;
    nlohmann::json nodes = nlohmann::json::object();
    for (const auto& [name, n] : cardNodes)
        nodes[name] = {{"cards", n.cards}, {"triangles", n.triangles}, {"textures", n.textures}, {"feathers", n.feathers}, {"stale", n.stale}};
    out["cardNodes"] = std::move(nodes);
    out["hairColor"] = hairColor;
    out["renderData"] = renderData;
    out["colorNodes"] = colorNodes;
    out["colorBlends"] = colorBlends;
    out["times"] = times;
    out["runs"] = runs;
    out["channels"] = channels;
    out["notes"] = notes;
    out["scalpHistory"] = {{"frames", scalpHistoryFrames}};
    out["instances"] = instances;
    out["restEvaluations"] = restEvaluations;
    out["cacheBytes"] = cacheBytes;
    out["cacheEvictions"] = cacheEvictions;
    out["strokeDocument"] = strokeDocument;
    nlohmann::json sims = nlohmann::json::object();
    for (const groomlab::SimStatus& s : simulations)
    {
        nlohmann::json ranges = nlohmann::json::array();
        for (const auto& [first, last] : s.ranges)
            ranges.push_back({first, last});
        sims[s.node] = {{"startFrame", s.startFrame}, {"ranges", ranges}, {"frames", s.frames}, {"live", s.live}, {"baked", s.baked},
                        {"bakeStale", s.bakeStale}, {"bakedFirst", s.bakedFirst}, {"bakedLast", s.bakedLast}, {"bakedFile", s.bakedFile},
                        {"stepMs", s.stepMs}, {"bytes", s.bytes}};
    }
    out["simulations"] = std::move(sims);
    return out.dump();
}

GroomEvaluator::Stats GroomEvaluator::stats() const
{
    Stats out;
    const Result& v = mResult;
    const StrandsPtr& strands = v.strands;
    out.view = v.viewedNode;
    out.strands = strands ? strands->strandCount() : 0;
    out.points = strands ? strands->pointCount() : 0;
    out.guides = mActiveHierarchy ? mActiveHierarchy->guides().size() : 0;
    out.cards = v.cards ? v.cards->cards.size() : 0;
    out.cardStrands = v.cardStrands ? v.cardStrands->strandCount() : 0;
    out.cardTriangles = v.cards ? v.cards->triangleCount() : 0;
    out.cardTextures = v.cards ? v.cards->textureCount() : 0;
    {
        // A Cards node is stale when its cards no longer match what the bake was made from (one the bake never saw too);
        // maps set by hand carry no stamp and are never stale.
        const auto coverage = mCardMaps.find("Coverage");
        const bool baked = coverage != mCardMaps.end() && !coverage->second.empty();
        const auto stampIt = mCardMaps.find("Strands");
        const std::string stamp = stampIt != mCardMaps.end() ? stampIt->second : std::string();
        const auto bakedHashes = parseCardsBakeStamp(stamp);
        if (v.cards)
            for (const CardNodeSummary& n : v.cardNodes)
            {
                const auto it = bakedHashes.find(n.node);
                const bool stale = baked && !stamp.empty() && (it == bakedHashes.end() || it->second != std::to_string(n.bakeHash));
                out.cardsStale = out.cardsStale || stale;
                out.cardFeathers += n.feathers;
                out.cardNodes[n.node] = {n.cards, n.triangles, n.textures, n.feathers, stale};
            }
        out.cardsBaked = baked;
    }
    if (mDoc)
    {
        const GraphDocument& doc = *viewDoc();
        out.cacheBytes = doc.graph.cachedBytes();
        out.cacheEvictions = doc.graph.cacheEvictions();
        activeNode(doc, "HairColor", &out.hairColor);
        activeNode(doc, "RenderData", &out.renderData);
        doc.graph.forEachActiveUpstream(doc.output, [&](const std::string& at, const Operator& op) {
            if (colorNode(op.typeName()))
                out.colorNodes.push_back(at);
            return true;
        });
        out.colorBlends = colorBlendNodes(doc);
        for (const auto& [name, id] : doc.nodesByName)
        {
            if (const auto time = doc.graph.lastEvaluationTimes().find(id); time != doc.graph.lastEvaluationTimes().end())
            {
                out.times[name] = time->second;
                if (const auto kind = doc.graph.lastRunKinds().find(id); kind != doc.graph.lastRunKinds().end())
                    out.runs[name] = time->second > 0.0 ? kind->second : "cached";
            }
            if (const auto note = doc.graph.lastNotes().find(id); note != doc.graph.lastNotes().end())
                out.notes[name] = note->second;
            std::vector<std::string> names;
            if (doc.graph.cachedChannels(id, &names))
                out.channels[name] = std::move(names);
        }
    }
    out.scalpHistoryFrames = scalpHistoryFrames(mResult.viewedNode);
    out.restEvaluations = restEvaluations();
    if (strands && strands->instances() && !strands->instances()->empty())
    {
        std::unordered_map<std::uint64_t, std::size_t> index;
        index.reserve(strands->strandCount());
        for (std::size_t s = 0; s < strands->strandCount(); ++s)
            index.emplace(strands->ids()[s], s);
        for (const auto& placed : strands->instances()->instances)
        {
            const auto it = index.find(placed.strand);
            if (it != index.end() && strands->strandPointCount(it->second) > 0)
                ++out.instances;
        }
    }
    out.strokeDocument = mSculptDoc != nullptr;
    out.simulations = simulationStatus();
    return out;
}

std::vector<std::pair<std::string, double>> GroomEvaluator::nodeTimes() const
{
    std::vector<std::pair<std::string, double>> out;
    if (!mDoc)
        return out;
    const auto& times = viewDoc()->graph.lastEvaluationTimes();
    for (const auto& [name, id] : mDoc->nodesByName)
        if (const auto it = times.find(id); it != times.end())
            out.emplace_back(name, it->second);
    return out;
}

std::vector<std::pair<std::string, std::string>> GroomEvaluator::nodeNotes() const
{
    std::vector<std::pair<std::string, std::string>> out;
    if (!mDoc)
        return out;
    const auto& notes = viewDoc()->graph.lastNotes();
    for (const auto& [name, id] : mDoc->nodesByName)
        if (const auto it = notes.find(id); it != notes.end())
            out.emplace_back(name, it->second);
    return out;
}

std::size_t GroomEvaluator::evaluationCount() const
{
    // The view's and a live stroke's (both show the graph's result); not the stroke document's warm evaluations.
    return (mDoc ? mDoc->graph.evaluationCount() : 0) + (mSculptDoc ? mSculptDoc->graph.evaluationCount() - mWarmEvaluations : 0) +
           mReleasedEvaluations;
}

std::size_t GroomEvaluator::incrementalCount() const
{
    return (mDoc ? mDoc->graph.incrementalCount() : 0) + (mSculptDoc ? mSculptDoc->graph.incrementalCount() - mWarmIncrementals : 0) +
           mReleasedIncrementals;
}

std::size_t GroomEvaluator::restEvaluations() const
{
    return mRestPose ? mRestPose->evaluations() : 0;
}

// Simulation.

namespace groomEvaluator
{
/** Two frames of one cache blended, `a` toward `b` by w (points and wetness). */
GuidesPtr blendFrames(const GuidesPtr& a, const GuidesPtr& b, float w)
{
    if (!a || !b || a->size() != b->size())
        return a;
    auto out = std::make_shared<GuideSet>(*a);
    for (std::size_t g = 0; g < out->size(); ++g)
    {
        Guide& guide = out->guides()[g];
        const Guide& next = b->guides()[g];
        std::vector<Vec3f>& points = guide.points;
        const std::vector<Vec3f>& to = next.points;
        if (to.size() != points.size())
            continue;
        for (std::size_t j = 0; j < points.size(); ++j)
            points[j] = points[j] + (to[j] - points[j]) * w;
        const auto wet = guide.pointAttributes.find(kWetnessChannel);
        const auto wetTo = next.pointAttributes.find(kWetnessChannel);
        if (wet == guide.pointAttributes.end() || wetTo == next.pointAttributes.end() ||
            wetTo->second.size() != wet->second.size())
            continue;
        for (std::size_t j = 0; j < wet->second.size(); ++j)
            wet->second[j] += (wetTo->second[j] - wet->second[j]) * w;
    }
    return out;
}

/** The frame at `seconds`, and how far past it toward the next (0: on the frame). */
int frameAt(double seconds, double fps, double* past)
{
    const double at = secondsToFrame(seconds, fps);
    const int frame = static_cast<int>(std::floor(at + 1e-6));
    double w = at - frame;
    if (w < 1e-6)
        w = 0.0;
    if (past)
        *past = w;
    return frame;
}
} // namespace groomEvaluator

std::vector<std::pair<std::string, NodeId>> GroomEvaluator::simulateNodeIds(const std::string& viewNode) const
{
    std::vector<std::pair<std::string, NodeId>> out;
    if (!mDoc)
        return out;
    const Graph& graph = mDoc->graph;
    auto upstream = [&](NodeId from) {
        std::set<NodeId> reached;
        std::vector<NodeId> stack{from};
        while (!stack.empty())
        {
            const NodeId id = stack.back();
            stack.pop_back();
            if (!graph.hasNode(id) || !reached.insert(id).second)
                continue;
            const std::size_t slots = graph.inputSpecs(id).size();
            for (std::size_t slot = 0; slot < slots; ++slot)
            {
                NodeId source = 0;
                if (graph.inputSource(id, slot, &source))
                    stack.push_back(source);
            }
        }
        return reached;
    };
    std::set<NodeId> reached = upstream(mDoc->output);
    const auto viewed = mDoc->nodesByName.find(viewNode);
    if (!viewNode.empty() && viewed != mDoc->nodesByName.end())
    {
        const std::set<NodeId> more = upstream(viewed->second);
        reached.insert(more.begin(), more.end());
    }
    // Ordered by how many Simulate nodes feed each, so a node's inputs are stepped before it.
    std::vector<std::pair<std::size_t, std::pair<std::string, NodeId>>> found;
    for (const auto& [name, id] : mDoc->nodesByName)
    {
        if (!reached.count(id) || graph.bypassed(id) || graph.node(id).typeName() != op::kSimulate)
            continue;
        std::size_t fedBy = 0;
        for (const NodeId other : upstream(id))
            if (other != id && graph.node(other).typeName() == op::kSimulate && !graph.bypassed(other))
                ++fedBy;
        found.push_back({fedBy, {name, id}});
    }
    std::sort(found.begin(), found.end());
    for (auto& f : found)
        out.push_back(std::move(f.second));
    return out;
}

std::vector<GroomEvaluator::SimRef> GroomEvaluator::simRefs(const std::string& viewNode)
{
    std::vector<SimRef> refs;
    for (const auto& [name, id] : simulateNodeIds(viewNode))
    {
        std::unique_ptr<SimNode>& node = mSims[name];
        if (!node)
            node = std::make_unique<SimNode>();
        node->cache.configure(SimSettings::fromParams(mDoc->graph.node(id).params()), mFps);
        refs.push_back({name, id, node.get()});
    }
    // A node gone from the graph lets its frames go; a bake set for it stays.
    for (auto& [name, node] : mSims)
        if (!mDoc || !mDoc->nodesByName.count(name))
            node->cache.clear();
    return refs;
}

bool GroomEvaluator::readsEarlierFrames(const std::string& viewNode, int firstFrame, int lastFrame) const
{
    if (scalpHistoryFrames(viewNode) > 0)
        return true;
    for (const auto& [name, id] : simulateNodeIds(viewNode))
    {
        const SimSettings settings = SimSettings::fromParams(mDoc->graph.node(id).params());
        const int from = std::max(firstFrame, settings.startFrame);
        if (from > lastFrame)
            continue;
        const auto it = mSims.find(name);
        const BakedSimulationPtr baked = it != mSims.end() ? it->second->baked : nullptr;
        const bool covered = baked && baked->read && (baked->stamp == 0 || baked->stamp == settings.hash()) &&
                             std::lround(secondsToFrame(baked->firstSeconds, mFps)) <= from &&
                             std::lround(secondsToFrame(baked->lastSeconds, mFps)) >= lastFrame;
        if (!covered)
            return true;
    }
    return false;
}

std::vector<std::string> GroomEvaluator::simulateNodes() const
{
    std::vector<std::string> out;
    for (const auto& [name, id] : simulateNodeIds({}))
        out.push_back(name);
    return out;
}

EvalContext GroomEvaluator::simContext(double seconds, const ExternalInputs& externals) const
{
    EvalContext ctx;
    ctx.previewDensity = mSimDensity;
    ctx.maxPoints = mSimPointBudget;
    ctx.time = seconds;
    ctx.framesPerSecond = mFps;
    ctx.scale = mGroomScale;
    ctx.externals = &externals;
    ctx.paths = paths();
    return ctx;
}

SimInputs GroomEvaluator::simInputs(const SimRef& ref, const EvalContext& ctx, std::uint64_t* hash)
{
    SimInputs in;
    Graph& graph = mDoc->graph;
    NodeId source = 0;
    std::size_t output = 0;
    if (graph.inputSource(ref.id, 0, &source, &output))
        in.guides = valueAs<GuidesPtr>(graph.evaluateOutput(source, output, ctx));
    if (graph.inputSource(ref.id, 1, &source, &output))
    {
        if (MeshPtr mesh = valueAs<MeshPtr>(graph.evaluateOutput(source, output, ctx)))
        {
            in.colliders.push_back(std::move(mesh));
            // A ColliderMesh node's own friction; negative: the Simulate node's.
            const Operator& from = graph.node(source);
            in.colliderFriction.push_back(from.typeName() == op::kColliderMesh
                                              ? static_cast<float>(from.params().getDouble("friction", -1.0))
                                              : -1.0f);
        }
    }
    if (graph.inputSource(ref.id, 2, &source, &output))
        in.water = valueAs<MeshPtr>(graph.evaluateOutput(source, output, ctx));
    in.scalp = mScalp;
    // Following, the graph runs on the rest pose: the guides are stepped on the scalp as it is (simFrameAt carries
    // the frames back).
    if (mFollowing && in.guides && mFollowRest && mScalp)
        in.guides = carryGuidesBack(in.guides, *mScalp, *mFollowRest);
    if (!in.guides)
        in.guides = std::make_shared<const GuideSet>();
    {
        // The mappable params as the graph would evaluate them for the node (painted, textured, fields plugged in).
        ParamSet params = graph.node(ref.id).params();
        const std::vector<std::string>& exposed = graph.paramInputs(ref.id);
        const std::size_t own = graph.node(ref.id).inputs().size();
        for (std::size_t k = 0; k < exposed.size(); ++k)
        {
            if (!graph.inputSource(ref.id, own + k, &source, &output))
                continue;
            const Value value = graph.evaluateOutput(source, output, ctx);
            if (FieldPtr field = valueAs<FieldPtr>(value))
                params.setField(exposed[k], field);
            const UniformPtr uniform = valueAs<UniformPtr>(value);
            if (uniform && !graph.paramDriven(ref.id, exposed[k]))
                if (FieldPtr field = uniformField(*uniform))
                    params.setField(exposed[k], field);
        }
        in.maps = simulationMaps(params, *in.guides, in.scalp);
    }
    if (hash)
    {
        std::uint64_t h = hashCombine(hash64(0x51A71), in.guides->contentHash());
        h = hashCombine(h, in.scalp ? in.scalp->surfaceHash() : 0);
        for (const MeshPtr& mesh : in.colliders)
            h = hashCombine(h, mesh->surfaceHash());
        if (in.water)
            h = hashCombine(hashCombine(h, 0x3A7E), in.water->surfaceHash());
        for (const float f : in.colliderFriction)
            if (f >= 0.0f)
                h = hashFloat(hashCombine(h, 0xF71C), f);
        if (in.maps)
            h = hashCombine(h, in.maps->hash());
        *hash = h;
    }
    return in;
}

bool GroomEvaluator::bakeUsable(const SimNode& node, double seconds) const
{
    const BakedSimulationPtr& baked = node.baked;
    if (!baked || !baked->read)
        return false;
    if (baked->stamp != 0 && baked->stamp != node.cache.settings().hash())
        return false;
    const double slack = 0.5 / mFps;
    return seconds >= baked->firstSeconds - slack && seconds <= baked->lastSeconds + slack;
}

std::pair<GuidesPtr, std::uint64_t> GroomEvaluator::simFrameAt(const SimNode& node, double seconds) const
{
    double w = 0.0;
    const int f0 = groomEvaluator::frameAt(seconds, mFps, &w);
    if (f0 < node.cache.startFrame())
        return {};
    GuidesPtr frame;
    std::uint64_t hash = 0;
    const bool baked = bakeUsable(node, seconds);
    if (baked)
    {
        frame = node.baked->read(seconds);
        hash = frame ? hashCombine(hashCombine(hash64(0xBA4ED), frame->contentHash()), node.baked->stamp) : 0;
    }
    else if (node.cache.has(f0))
    {
        frame = node.cache.frame(f0);
        hash = node.cache.frameHash(f0);
        if (w > 0.0 && node.cache.has(f0 + 1))
        {
            frame = groomEvaluator::blendFrames(frame, node.cache.frame(f0 + 1), static_cast<float>(w));
            hash = hashFloat(hashCombine(hash, node.cache.frameHash(f0 + 1)), static_cast<float>(w));
        }
    }
    if (frame && mFollowing && mFollowRest && mScalp)
    {
        if (baked)
        {
            // A bake keeps no roots: each guide takes its hierarchy guide's, or the scalp point nearest its first.
            auto rooted = std::make_shared<GuideSet>(*frame);
            for (Guide& g : rooted->guides())
            {
                if (const HierarchyGuide* h = mHierarchy ? mHierarchy->guide(static_cast<GuideId>(g.id)) : nullptr)
                    g.root = h->root;
                else if (!g.points.empty())
                    g.root = mScalp->closestPoint(g.points.front()).root;
            }
            frame = rooted;
        }
        frame = carryGuidesBack(frame, *mFollowRest, *mScalp);
        hash = hashCombine(hash, mScalp->surfaceHash());
    }
    return {frame, hash};
}

void GroomEvaluator::checkSimInputs(SimRef& ref, double seconds, const ExternalInputs& externals)
{
    SimNode& node = *ref.node;
    std::uint64_t hash = 0;
    simInputs(ref, simContext(seconds, externals), &hash);
    double w = 0.0;
    const int f = groomEvaluator::frameAt(seconds, mFps, &w);
    if (w == 0.0 && node.cache.has(f) && node.cache.inputHash(f) != 0)
    {
        if (hash != node.cache.inputHash(f))
            node.cache.clear();
    }
    else if (node.checked && node.checkedSeconds == seconds && hash != node.checkedHash)
        node.cache.clear();
    node.checked = true;
    node.checkedSeconds = seconds;
    node.checkedHash = hash;
}

void GroomEvaluator::stepSimulations(const Request& request, std::vector<std::string>& errors)
{
    for (auto& [name, node] : mSims)
    {
        node->shown.reset();
        node->shownHash = 0;
    }
    if (!mDoc || hasPreview())
        return;
    std::vector<SimRef> refs = simRefs(request.viewNode);
    if (refs.empty())
        return;
    mSimDensity = request.density;
    mSimPointBudget = request.pointBudget;
    const double t = request.time;
    double w = 0.0;
    const int f0 = groomEvaluator::frameAt(t, mFps, &w);
    const int f1 = w > 0.0 ? f0 + 1 : f0;
    try
    {
        // Upstream first: each node's frame is in the externals of the nodes it feeds.
        for (SimRef& ref : refs)
        {
            SimNode& node = *ref.node;
            if (f0 >= node.cache.startFrame() && !bakeUsable(node, t))
            {
                {
                    ExternalInputs externals;
                    fillExternals(externals);
                    checkSimInputs(ref, t, externals);
                }
                if (mSimStepping && node.cache.settings().live && !(node.cache.has(f0) && node.cache.has(f1)))
                {
                    std::vector<SimRef> one{ref};
                    SimRun run;
                    advanceSimulations(one, f1, t, {}, run);
                    for (const std::string& e : run.errors)
                        errors.push_back(e);
                }
            }
            std::tie(node.shown, node.shownHash) = simFrameAt(node, t);
        }
    }
    catch (const std::exception& e)
    {
        errors.push_back(std::string("simulation: ") + e.what());
    }
}

void GroomEvaluator::advanceSimulations(std::vector<SimRef>& nodes, int target, double nowSeconds,
                                        const SimProgress& progress, SimRun& run)
{
    const auto started = std::chrono::steady_clock::now();
    auto nextFrame = [](const SimNode& n) { return n.cache.started() ? n.cache.lastFrame() + 1 : n.cache.startFrame(); };
    int from = INT_MAX;
    for (const SimRef& ref : nodes)
        from = std::min(from, nextFrame(*ref.node));
    if (nodes.empty() || from > target)
        return;
    const int count = target - from + 1;
    auto stepError = [&](const SimRef& ref) {
        run.errors.push_back("Simulate '" + ref.name +
                             "': its guides changed (another count, other IDs or points per guide); it starts again "
                             "from frame " +
                             std::to_string(ref.node->cache.startFrame()));
    };

    // The inputs as they are at nowSeconds, put back after the frames the feed set.
    struct Moving
    {
        MeshPtr scalp;
        NamedScalps scalps;
        GuidesPtr guides, poses;
        std::uint64_t posesHash = 0;
        std::map<std::string, MeshPtr> meshes;
        std::map<std::string, float> controls;
        ScalpHistoryPtr history;
    };
    const Moving saved{mData.scalp,  mData.scalps,   mData.guides,   mData.guidePoses,
                       mPosesHash,   mData.meshes,   mData.controls, mData.scalpHistory};
    bool fed = false;
    std::vector<std::string> scratchErrors;
    std::vector<Message> scratchMessages;
    auto restore = [&] {
        if (!fed)
            return;
        mData.scalp = saved.scalp;
        mData.scalps = saved.scalps;
        mData.guides = saved.guides;
        mData.guidePoses = saved.poses;
        mPosesHash = saved.posesHash;
        mData.meshes = saved.meshes;
        mData.controls = saved.controls;
        mData.scalpHistory = saved.history;
        fed = false;
        scratchErrors.clear();
        scratchMessages.clear();
        prepareTime(nowSeconds, scratchErrors, scratchMessages);
    };
    try
    {
        for (int f = from; f <= target; ++f)
        {
            const double seconds = frameToSeconds(f, mFps);
            const bool now = std::abs(seconds - nowSeconds) <= 1e-9 * std::max(1.0, std::abs(nowSeconds));
            bool known = now; // the inputs are the host's at f
            if (now || !mSimFeed)
                restore();
            else
            {
                const bool wasFed = fed;
                fed = true;
                if (mSimFeed(seconds))
                {
                    known = true;
                    scratchErrors.clear();
                    scratchMessages.clear();
                    prepareTime(seconds, scratchErrors, scratchMessages);
                }
                else
                {
                    fed = wasFed;
                    restore();
                }
            }
            ExternalInputs externals;
            fillExternals(externals);
            for (const auto& [name, node] : mSims)
            {
                const auto [frame, frameHash] = simFrameAt(*node, seconds);
                externals.set(simulationExternal(name), frame ? Value(frame) : Value{}, frameHash);
            }
            const EvalContext ctx = simContext(seconds, externals);
            for (SimRef& ref : nodes)
            {
                SimNode& node = *ref.node;
                const int next = nextFrame(node);
                if (next > f)
                    continue;
                std::uint64_t hash = 0;
                const SimInputs in = simInputs(ref, ctx, &hash);
                // Frames whose inputs are unknown wait for the next known frame (or the last) and are stepped in one go
                // toward it, the roots moving evenly; stepping each toward the inputs shown would put the head's whole
                // motion into the first and whip the hair. They are stamped 0 so a later look with real inputs keeps
                // them.
                if (node.cache.started() && !known && f < target && hash != node.towardHash)
                    continue;
                const std::uint64_t stamp = known ? hash : 0;
                if (!node.cache.started())
                {
                    if (next != f)
                        continue;
                    node.cache.start(in, stamp);
                }
                else if (!node.cache.step(in, stamp, f - next + 1))
                {
                    stepError(ref);
                    continue;
                }
                node.towardHash = hash;
                const auto [frame, frameHash] = simFrameAt(node, seconds);
                externals.set(simulationExternal(ref.name), frame ? Value(frame) : Value{}, frameHash);
            }
            ++run.frames;
            if (progress && !progress(f - from + 1, count))
            {
                run.cancelled = true;
                break;
            }
        }
    }
    catch (...)
    {
        restore();
        throw;
    }
    restore();
    run.milliseconds += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

SimRun GroomEvaluator::simulateTo(int frame, const SimProgress& progress)
{
    SimRun run;
    if (!mDoc)
    {
        run.errors.push_back(mGraphError.empty() ? "the groom has no graph" : mGraphError);
        return run;
    }
    std::vector<std::string> errors;
    std::vector<Message> messages;
    prepareTime(mTime, errors, messages);
    std::vector<SimRef> refs = simRefs({});
    try
    {
        ExternalInputs externals;
        fillExternals(externals);
        for (SimRef& ref : refs)
            checkSimInputs(ref, mTime, externals);
        advanceSimulations(refs, frame, mTime, progress, run);
    }
    catch (const std::exception& e)
    {
        run.errors.push_back(std::string("simulation: ") + e.what());
    }
    return run;
}

SimRun GroomEvaluator::simulateBefore(double seconds)
{
    SimRun run;
    if (!mDoc || hasPreview() || !mSimStepping || !mSimFeed)
        return run;
    double w = 0.0;
    const int f0 = groomEvaluator::frameAt(seconds, mFps, &w);
    const int f1 = w > 0.0 ? f0 + 1 : f0;
    const int target = f1 - 1;
    std::vector<SimRef> refs;
    for (SimRef& ref : simRefs({}))
    {
        const SimNode& node = *ref.node;
        if (f0 >= node.cache.startFrame() && !bakeUsable(node, seconds) && node.cache.settings().live &&
            !(node.cache.has(f0) && node.cache.has(f1)))
            refs.push_back(ref);
    }
    try
    {
        advanceSimulations(refs, target, seconds, {}, run);
    }
    catch (const std::exception& e)
    {
        run.errors.push_back(std::string("simulation: ") + e.what());
    }
    return run;
}

void GroomEvaluator::invalidateSimulation(const std::string& node)
{
    for (auto& [name, sim] : mSims)
    {
        if (node.empty() || name == node)
        {
            sim->cache.clear();
            sim->shown.reset();
            sim->shownHash = 0;
        }
    }
}

std::vector<SimStatus> GroomEvaluator::simulationStatus() const
{
    std::vector<SimStatus> out;
    if (!mDoc)
        return out;
    for (const auto& [name, id] : mDoc->nodesByName)
    {
        const Operator& op = mDoc->graph.node(id);
        if (op.typeName() != op::kSimulate)
            continue;
        const SimSettings settings = SimSettings::fromParams(op.params());
        SimStatus s;
        s.node = name;
        s.startFrame = settings.startFrame;
        s.live = settings.live;
        s.stamp = settings.hash();
        if (const auto it = mSims.find(name); it != mSims.end())
        {
            const SimNode& node = *it->second;
            if (node.cache.settings().hash() == s.stamp) // frames of these settings; else none until stepped
            {
                s.ranges = node.cache.ranges();
                s.frames = node.cache.frameCount();
                s.stepMs = node.cache.lastStepMs();
                s.bytes = node.cache.bytes();
            }
            if (node.baked && node.baked->read)
            {
                const bool matches = node.baked->stamp == 0 || node.baked->stamp == s.stamp;
                s.baked = matches;
                s.bakeStale = !matches;
                s.bakedFirst = static_cast<int>(std::lround(secondsToFrame(node.baked->firstSeconds, mFps)));
                s.bakedLast = static_cast<int>(std::lround(secondsToFrame(node.baked->lastSeconds, mFps)));
                s.bakedFile = node.baked->file;
            }
        }
        out.push_back(std::move(s));
    }
    return out;
}

GuidesPtr GroomEvaluator::simulatedFrame(const std::string& node, int frame) const
{
    const auto it = mSims.find(node);
    return it == mSims.end() ? nullptr : it->second->cache.frame(frame);
}

void GroomEvaluator::setBakedSimulation(const std::string& node, BakedSimulationPtr baked)
{
    std::unique_ptr<SimNode>& sim = mSims[node];
    if (!sim)
        sim = std::make_unique<SimNode>();
    sim->baked = std::move(baked);
}

BakedSimulationPtr GroomEvaluator::bakedSimulation(const std::string& node) const
{
    const auto it = mSims.find(node);
    return it == mSims.end() ? nullptr : it->second->baked;
}

std::uint64_t GroomEvaluator::simulationStamp(const std::string& node) const
{
    if (!mDoc)
        return 0;
    const auto it = mDoc->nodesByName.find(node);
    if (it == mDoc->nodesByName.end() || mDoc->graph.node(it->second).typeName() != op::kSimulate)
        return 0;
    return SimSettings::fromParams(mDoc->graph.node(it->second).params()).hash();
}
} // namespace lila::groom
