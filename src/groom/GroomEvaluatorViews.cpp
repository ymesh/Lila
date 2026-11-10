// Adapted from GroomLab's groom_session.cpp (github.com/thekesslereffect/groomlab, Apache-2.0,
// see src/groom/groomlab/LICENSE and NOTICE).
#include "groom/GroomEvaluator.h"
#include "groom/GroomDisplay.h"

#include <groomlab/random.h>
#include <groomlab/card_files.h>
#include <groomlab/operator_state.h>
#include <groomlab/operators.h>
#include <groomlab/preview_core.h>
#include <groomlab/scalp_paint.h>
#include <groomlab/strand_set.h>
#include <groomlab/subdivision.h>
#include <groomlab/texture.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>
#include <sstream>

namespace lila::groom
{
using namespace groomlab;

std::unique_ptr<GraphDocument> GroomEvaluator::newDocument() const
{
    auto doc = std::make_unique<GraphDocument>(mParsed->clone());
    doc->graph.setCacheIntermediates(mCacheIntermediates);
    return doc;
}

std::shared_ptr<const std::vector<Vec3f>> GroomEvaluator::clumpCenterDots(GraphDocument& doc, NodeId node, const EvalContext& ctx)
{
    auto dots = std::make_shared<std::vector<Vec3f>>();
    // Where a root is on the scalp now (the strands' roots are on its faces, following it or not).
    auto at = [&](const StrandRoot& root, const Vec3f& fallback) {
        return mScalp && mScalp->contains(root) ? mScalp->position(root) : fallback;
    };
    const Operator& op = doc.graph.node(node);
    GuidesPtr centers;
    StrandsPtr centerStrands;
    NodeId source = 0;
    std::size_t output = 0;
    const std::string from = op.params().getString("centersFrom");
    if (from != "pick")
    {
        // The centers input (guides or strands), when one is linked.
        if (!doc.graph.inputSource(node, 1, &source, &output))
            return dots;
        doc.graph.evaluateOutput(source, output, ctx);
        const Value value = doc.graph.cachedValue(source, output);
        centers = valueAs<GuidesPtr>(value);
        centerStrands = valueAs<StrandsPtr>(value);
    }
    else
    {
        // A Clump picking its own centers: the strands they are (their roots), else the centers' curves as it kept them.
        ClumpState state;
        const StrandsPtr made = valueAs<StrandsPtr>(doc.graph.cachedValue(node));
        if (!made || !clumpState(op, made, &state))
            return dots;
        const bool fed = doc.graph.inputSource(node, 0, &source, &output);
        if (fed)
            doc.graph.evaluateOutput(source, output, ctx);
        const StrandsPtr in = fed ? valueAs<StrandsPtr>(doc.graph.cachedValue(source, output)) : nullptr;
        if (in && !state.chosen.empty())
        {
            for (const std::size_t s : state.chosen)
                if (s < in->strandCount() && in->strandPointCount(s) > 0)
                    dots->push_back(at(in->roots()[s], in->strandPoints(s)[0]));
        }
        else
        {
            for (std::size_t c = 0; c * kClumpCenterSamples < state.curves.size(); ++c)
                dots->push_back(state.curves[c * kClumpCenterSamples]);
        }
        return dots;
    }
    // What Clump follows: centers of 2 points or more, or with centersFrom inputPoints any (their roots).
    const std::size_t least = from == "inputPoints" ? 1 : 2;
    if (centers)
    {
        for (const Guide& g : centers->guides())
            if (g.points.size() >= least)
                dots->push_back(at(g.root, g.points.front()));
    }
    else if (centerStrands)
    {
        const PreviewCore core(*centerStrands); // the strands Clump takes as centers: their core
        for (std::size_t s = 0; s < centerStrands->strandCount(); ++s)
            if (core.core(s) && centerStrands->strandPointCount(s) >= least)
                dots->push_back(at(centerStrands->roots()[s], centerStrands->strandPoints(s)[0]));
    }
    return dots;
}

const StrandBoxes& GroomEvaluator::followBoxes(const StrandsPtr& rest)
{
    if (!mFollowBoxes || mFollowBoxesOf.lock() != rest || mFollowBoxesFrom != mFollowRest)
    {
        mFollowBoxes = strandBoxes(*rest, *mFollowRest);
        mFollowBoxesOf = rest;
        mFollowBoxesFrom = mFollowRest;
    }
    return *mFollowBoxes;
}

// Cards.

void GroomEvaluator::setCardMaps(const std::string& cardMaps)
{
    if (cardMaps == mCardMapsText)
        return;
    mCardMapsText = cardMaps;
    mCardMaps = parseCardMaps(cardMaps);
}

void GroomEvaluator::buildCards(const EvalContext& ctx, float cardDensity, std::vector<Message>& messages)
{
    Result& out = mResult;
    // The card graph's own node cache decides what is stale, so a hit returns the same strands and the cards stay. Its
    // own copy of the graph: another density with cards doesn't evict the view's results.
    const auto started = std::chrono::steady_clock::now();
    if (!mCardDoc)
        mCardDoc = newDocument();
    EvalContext cardCtx = ctx;
    cardCtx.previewDensity = cardDensity;
    cardCtx.maxPoints = 0.0; // every strand goes into its card
    cardCtx.buildCards = true;
    StrandsPtr cardStrands = carry(mCardDoc->graph.evaluateStrands(mCardDoc->output, cardCtx), 1);
    auto layOut = [&] {
        auto cards = std::make_shared<CardMesh>(combineCardLayers(out.cardStrands->cardLayers()));
        layoutCards(*cards, cards->padding);
        // The baked atlas: rebuilt cards keep the cells they were baked with.
        if (!mCardAtlas.empty())
            applyCardAtlas(*cards, mCardAtlas, out.cardStrands.get());
        out.cardNodes = summarizeCardNodes(*cards, out.cardStrands.get());
        out.cards = std::move(cards);
    };
    if (!out.cardStrands || cardStrands != out.cardStrands)
    {
        out.cardStrands = std::move(cardStrands);
        out.cards.reset();
        out.cardNodes.clear();
        if (out.cardStrands && !out.cardStrands->cardLayers().empty())
        {
            refreshCardAtlas(messages);
            layOut();
        }
        mLastCardBuildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    }
    else if (out.cards && refreshCardAtlas(messages))
    {
        // A new bake (or its atlas gone): the same cards laid out for it, so the new maps fit at once.
        layOut();
    }
}

bool GroomEvaluator::refreshCardAtlas(std::vector<Message>& messages)
{
    const auto atlas = mCardMaps.find("Atlas");
    std::string path;
    if (atlas != mCardMaps.end() && !atlas->second.empty())
    {
        const ScopedPathResolver scope(paths());
        path = resolvePath(atlas->second);
    }
    const std::uint64_t stamp = path.empty() ? 0 : fileStamp(path);
    if (path == mCardAtlasPath && stamp == mCardAtlasStamp)
        return false;
    mCardAtlasPath = path;
    mCardAtlasStamp = stamp;
    mCardAtlas = CardAtlas{};
    if (!path.empty() && stamp != 0)
    {
        try
        {
            std::ifstream in(path, std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            mCardAtlas = CardAtlas::fromJson(text.str());
        }
        catch (const std::exception& e)
        {
            messages.push_back({std::string("card atlas ") + path + ": " + e.what() + " (the cards use a fresh layout)", true});
        }
    }
    return true;
}

bool GroomEvaluator::hasCardsNode() const
{
    if (!mDoc)
        return false;
    bool cards = false;
    mDoc->graph.forEachActiveUpstream(mDoc->output, [&](const std::string&, const Operator& op) {
        cards = op.typeName() == "Cards";
        return !cards;
    });
    return cards;
}

bool GroomEvaluator::hasStrandMeshNode() const
{
    if (!mDoc)
        return false;
    bool found = false;
    mDoc->graph.forEachActiveUpstream(mDoc->output, [&](const std::string&, const Operator& op) {
        found = op.typeName() == op::kToMesh;
        return !found;
    });
    return found;
}

bool GroomEvaluator::viewRefinesStrands() const
{
    if (!mDoc)
        return false;
    bool refines = false;
    mDoc->graph.forEachActiveUpstream(mDoc->output, [&](const std::string&, const Operator& op) {
        refines = refines || (op.typeName() == op::kSubdivide && op.params().getInt("viewportLevels") > 0);
        return !refines;
    });
    return refines;
}

// Other evaluations with the last inputs.

StrandsPtr GroomEvaluator::viewedAsRendered(float density, double seconds) const
{
    const Result& view = mResult;
    if (!mDoc || mFailure || !view.strands || !view.errors.empty() || hasPreview() || mViewSculpt || !view.viewedNode.empty())
        return nullptr;
    const EvalContext& was = mLastViewContext;
    if (was.previewDensity != density || was.time != seconds || was.buildCards)
        return nullptr;
    const Graph& graph = mDoc->graph;
    for (const NodeId id : graph.nodeIds())
    {
        if (was.viewport && graph.evaluationMode(id) != EvaluationMode::Always)
            return nullptr;
        if (was.maxPoints > 0.0 && graph.node(id).thinnedLastRun())
            return nullptr;
    }
    return view.placed();
}

StrandsPtr GroomEvaluator::evaluateFullDensity(double seconds)
{
    if (!mDoc)
        return nullptr;
    if (!mFullDoc)
    {
        mFullDoc = newDocument();
        mFullDoc->graph.adoptCache(mDoc->graph);
    }
    ExternalInputs externals;
    fillExternals(externals);
    EvalContext ctx = context(externals);
    ctx.previewDensity = 1.0f;
    ctx.time = seconds;
    return carry(mFullDoc->graph.evaluateStrands(mFullDoc->output, ctx), 2);
}

void GroomEvaluator::releaseFullDensity()
{
    mFullDoc.reset();
}

std::vector<StrandMeshSource> GroomEvaluator::evaluateStrandMeshes(double seconds)
{
    std::vector<StrandMeshSource> out;
    if (!mDoc || !mParsed)
        return out;
    std::vector<std::string> names;
    mParsed->graph.forEachActiveUpstream(mParsed->output, [&](const std::string& path, const Operator& op) {
        if (op.typeName() == op::kToMesh && mParsed->nodesByName.count(path) != 0)
            names.push_back(path);
        return true;
    });
    if (names.empty())
    {
        releaseStrandMeshes();
        return out;
    }
    ExternalInputs externals;
    fillExternals(externals);
    EvalContext ctx = context(externals);
    ctx.time = seconds;
    std::map<float, std::unique_ptr<GraphDocument>> docs; // the densities still asked for
    std::map<std::string, MeshMemo> memos;
    for (const std::string& name : names)
    {
        const NodeId id = mParsed->nodesByName.at(name);
        const StrandMeshSettings settings = strandMeshSettings(mParsed->graph.node(id).params());
        const float density = settings.density > 0.0f ? settings.density : 1.0f;
        auto doc = docs.find(density);
        if (doc == docs.end())
        {
            std::unique_ptr<GraphDocument> made;
            if (auto old = mMeshDocs.find(density); old != mMeshDocs.end())
            {
                made = std::move(old->second);
            }
            else
            {
                made = newDocument();
                if (density == 1.0f && mFullDoc)
                    made->graph.adoptCache(mFullDoc->graph);
            }
            doc = docs.emplace(density, std::move(made)).first;
        }
        ctx.previewDensity = density;
        // Cards upstream build only when the node may make a mesh of them.
        ctx.buildCards = settings.options.make == StrandMeshOptions::Make::Auto || settings.options.make == StrandMeshOptions::Make::Cards;
        const StrandsPtr in = doc->second->graph.evaluateStrands(doc->second->nodesByName.at(name), ctx);
        // The same strands on the same scalps as last time: the same result (hosts keep their mesh).
        MeshMemo memo;
        memo.in = in;
        memo.from = mFollowing ? mFollowRest : nullptr;
        memo.to = mFollowing ? mScalp : nullptr;
        memo.maxStrands = settings.maxStrands;
        const auto was = mMeshMemos.find(name);
        if (was != mMeshMemos.end() && was->second.in == memo.in && was->second.from == memo.from && was->second.to == memo.to &&
            was->second.maxStrands == memo.maxStrands)
        {
            memo.out = was->second.out;
        }
        else if (in)
        {
            StrandsPtr kept = in;
            if (settings.maxStrands > 0 && in->strandCount() > settings.maxStrands)
                kept = std::make_shared<StrandSet>(in->subset(thinStrandsById(*in, settings.maxStrands)));
            memo.out = mFollowing && mFollowRest && mScalp ? carryStrands(kept, *mFollowRest, *mScalp) : kept;
        }
        out.push_back({name, memo.out, settings.options});
        memos[name] = std::move(memo);
    }
    mMeshDocs = std::move(docs);
    mMeshMemos = std::move(memos);
    return out;
}

void GroomEvaluator::releaseStrandMeshes()
{
    mMeshDocs.clear();
    mMeshMemos.clear();
}

// Scalp maps.

ScalpMapView GroomEvaluator::scalpMapView(const std::string& name, const std::string& channel)
{
    ScalpMapView view;
    view.name = name;
    view.channel = channel;
    if (view.channel != "r" && view.channel != "g" && view.channel != "b" && view.channel != "a")
        view.channel = "rgb";
    if (view.name.empty() || !mScalp)
        return view;
    const auto it = mScalpMaps.maps.find(view.name);
    const bool previewing = mPreviewMapName == view.name;
    if (it != mScalpMaps.maps.end())
    {
        view.kind = it->second.kind;
        view.channels = it->second.channels == 4 ? 4 : 1;
        const std::uint64_t shownKey = hashText(hash64(0x5CA1AB), view.channels == 4 ? view.channel : std::string());
        if (view.kind == "vertices")
        {
            const std::uint64_t key = hashCombine(hashCombine(hashText(hash64(0x5CA1AA), view.name), mScalpMapsHash),
                                                  previewing && mPreviewMapValues ? mPreviewMapVersion : 0);
            const std::size_t per = static_cast<std::size_t>(view.channels);
            // Stored per vertex of the cage; a refined scalp shows them interpolated to its vertices.
            const ScalpMesh& cage = mCageScalp ? *mCageScalp : *mScalp;
            const int levels = mScalp->subdivisionLevel();
            auto shown = [&](const std::shared_ptr<const std::vector<float>>& values) -> std::shared_ptr<const std::vector<float>> {
                if (levels == 0)
                    return values;
                return std::make_shared<const std::vector<float>>(SubdivisionPlan::of(cage, levels)->refineVertexValues(*values, view.channels));
            };
            if (previewing && mPreviewMapValues && mPreviewMapValues->size() == cage.pointCount() * per)
            {
                view.values = shown(mPreviewMapValues);
            }
            else if (it->second.values.size() == cage.pointCount() * per)
            {
                const std::uint64_t refinedKey = hashCombine(key, mScalp->topologyHash());
                if (refinedKey != mMapViewKey || !mMapViewValues)
                    mMapViewValues = shown(std::make_shared<const std::vector<float>>(it->second.values));
                view.values = mMapViewValues;
                mMapViewKey = refinedKey;
            }
            else
            {
                view.kind.clear(); // doesn't fit this scalp
            }
            view.version = hashCombine(hashCombine(key, mScalp->contentHash()), shownKey);
        }
        else
        {
            {
                const ScopedPathResolver scope(paths());
                view.texture = resolvePath(it->second.texture);
            }
            if (previewing && mPreviewMapImage)
                view.preview = mPreviewMapImage;
            view.version = hashCombine(hashCombine(hashCombine(hashText(hash64(0x5CA1BB), view.texture), fileStamp(view.texture)),
                                                   hashCombine(previewing && mPreviewMapImage ? mPreviewMapVersion : 0, mScalp->contentHash())),
                                       shownKey);
        }
        return view;
    }
    // A guide layer: sampled at the scalp's vertices from the guides as they are now.
    const HierarchyPtr h = mActiveHierarchy;
    if (!h)
        return view;
    const std::vector<std::string> names = h->attributeNames();
    if (std::find(names.begin(), names.end(), view.name) == names.end())
        return view;
    view.kind = "guides";
    // The deepest level's guides, each one's root (on the scalp's rest pose) and its value of the layer; sampled again
    // only when those or the scalp changed (combing moves no root).
    const std::set<GroupId> deepest = h->deepestGroups();
    std::vector<Vec3f> roots;
    std::vector<float> values;
    std::uint64_t key = hashCombine(hashText(hash64(0x5CA1CC), view.name), mScalp->contentHash());
    for (const HierarchyGuide& g : h->guides())
    {
        if (!deepest.count(g.group) || g.points.size() < 2)
            continue;
        roots.push_back(mScalp->contains(g.root) ? mScalp->restPositionOr(g.root, g.points.front()) : g.points.front());
        float value = 0.0f;
        values.push_back(h->effectiveAttribute(g, view.name, &value) ? value : 0.0f);
        key = hashFloat(hashFloat(hashFloat(hashFloat(key, roots.back().x), roots.back().y), roots.back().z), values.back());
    }
    key = hashCombine(key, roots.size());
    if (key != mMapViewKey || !mMapViewValues)
    {
        mMapViewValues = std::make_shared<const std::vector<float>>(sampleRootValues(roots, values, vertexSamplePoints(*mScalp)));
        mMapViewKey = key;
    }
    view.values = mMapViewValues;
    view.version = key;
    return view;
}

void GroomEvaluator::setPreviewVertexMap(const std::string& name, std::shared_ptr<const std::vector<float>> values, int channels)
{
    mPreviewMapName = name;
    mPreviewMapValues = std::move(values);
    mPreviewMapChannels = channels == 4 ? 4 : 1;
    ++mPreviewMapVersion;
}

void GroomEvaluator::setPreviewTextureMap(const std::string& name, std::shared_ptr<const TexturePreview> image)
{
    mPreviewMapName = name;
    mPreviewMapImage = std::move(image);
    ++mPreviewMapVersion;
}
DisplayInputs displayInputs(const GroomEvaluator::Result& view, bool stroke)
{
    DisplayInputs in;
    in.strands = view.strands;
    in.viewedRoots = view.viewedRoots;
    in.clumpCenters = view.clumpCenters;
    in.movingGuides = view.movingGuides;
    in.scalp = view.scalp;
    in.hierarchy = view.hierarchy;
    in.parts = view.parts;
    in.flow = view.flow;
    in.cards = view.cards;
    in.strandsVersion = view.versions.strands;
    in.strandsSerial = view.strandsSerial;
    in.changesBase = view.changesBase;
    in.strandsChanges = view.strandsChanges;
    in.follow = view.follow;
    in.hierarchyVersion = view.versions.hierarchy;
    in.cardsVersion = view.versions.cards;
    in.strandMeshes = view.strandMeshes;
    in.strandMeshVersion = view.versions.strandMeshes;
    in.stroke = stroke;
    return in;
}
} // namespace lila::groom
