#include "groom/GroomLab.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "curve/NodeHelpers.h"
#include "geo/Mesh.h"
#include "groom/Attrs.h"
#include "groom/Groom.h"
#include "geo/ColorSets.h"
#include "groom/GroomEvaluator.h"
#include "groom/GroomExport.h"
#include "groom/GroomLook.h"
#include "groom/LookDevice.h"
#include "groom/LookEngineDevice.h"

#include <groomlab/gpu/look_chain.h>
#include "groom/Locks.h"
#include "groom/GuideData.h"
#include "groom/GroomNodes.h"
#include "groom/GroomGraph.h"
#include "groom/GroomGnNodes.h"
#include "groom/LockOps.h"
#include "groom/GroomLabBridge.h"
#include "ops/Undo.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Simulation.h"

#include <groomlab/cards.h>
#include <groomlab/guide_hierarchy.h>
#include <groomlab/guide_set.h>
#include <groomlab/scalp_history.h>
#include <groomlab/scalp_mesh.h>
#include <groomlab/strand_set.h>

#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>

namespace lila::groom
{
namespace groomLabBridge
{
groomlab::Vec3f toVec(const glm::dvec3& p)
{
    return {float(p.x), float(p.y), float(p.z)};
}

/** The mesh's live points in order, moved by `matrix`: the scalp's points as scalpMesh numbers them. */
std::vector<groomlab::Vec3f> worldPoints(const geo::Mesh& mesh, const glm::dmat4& matrix)
{
    std::vector<groomlab::Vec3f> points;
    points.reserve(mesh.numPoints());
    for (geo::Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p))
            points.push_back(toVec(glm::dvec3(matrix * glm::dvec4(mesh.position(p), 1.0))));
    return points;
}

/** The scalp's faces fanned into triangles over its live points, renumbered densely and moved by `matrix`. */
std::shared_ptr<const groomlab::ScalpMesh> scalpMesh(const geo::Mesh& mesh, const glm::dmat4& matrix)
{
    std::vector<int32_t> remap(mesh.pointCapacity(), -1);
    int32_t next = 0;
    for (geo::Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p))
            remap[p] = next++;
    std::vector<groomlab::ScalpMesh::Triangle> triangles;
    // uv0 per triangle corner: texture maps paint and read through it.
    const std::span<const glm::vec2> uv = mesh.cornerAttrs().get<glm::vec2>("uv0");
    std::vector<groomlab::ScalpMesh::UV> uvs;
    for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        const std::vector<geo::Index> v = mesh.facePoints(f);
        const std::vector<geo::Index> c = uv.empty() ? std::vector<geo::Index>() : mesh.faceCorners(f);
        for (size_t i = 1; i + 1 < v.size(); ++i)
        {
            triangles.push_back({remap[v[0]], remap[v[i]], remap[v[i + 1]]});
            if (!c.empty())
                for (geo::Index k : {c[0], c[i], c[i + 1]})
                    uvs.push_back({uv[k].x, uv[k].y});
        }
    }
    auto out = std::make_shared<groomlab::ScalpMesh>(worldPoints(mesh, matrix), std::move(triangles));
    if (!uvs.empty())
        out->setCornerUVs(std::move(uvs));
    return out;
}

groomlab::ScalpMaps colorSetMaps(const geo::Mesh& mesh)
{
    groomlab::ScalpMaps out;
    for (const std::string& set : geo::colors::sets(mesh))
    {
        const std::string name = groomlab::sanitizeMapName(set);
        if (name.empty())
            continue;
        const std::vector<glm::vec4> colors = geo::colors::pointColors(mesh, set);
        bool gray = true;
        for (geo::Index p = 0; p < mesh.pointCapacity() && gray; ++p)
            if (mesh.pointValid(p))
            {
                const glm::vec4& c = colors[p];
                gray = std::abs(c.r - c.g) < 1e-4f && std::abs(c.r - c.b) < 1e-4f;
            }
        groomlab::ScalpMapEntry e;
        e.kind = "vertices";
        e.channels = gray ? 1 : 4;
        for (geo::Index p = 0; p < mesh.pointCapacity(); ++p)
            if (mesh.pointValid(p))
            {
                const glm::vec4& c = colors[p];
                if (gray)
                    e.values.push_back(std::clamp(c.r, 0.0f, 1.0f));
                else
                    for (int k = 0; k < 4; ++k)
                        e.values.push_back(std::clamp(c[k], 0.0f, 1.0f));
            }
        out.maps[name] = std::move(e);
    }
    return out;
}

groomlab::ScalpMaps withColorSetMaps(const std::string& storedJson, const groomlab::ScalpMaps& colorMaps)
{
    groomlab::ScalpMaps out = groomlab::ScalpMaps::fromJson(storedJson);
    for (const auto& [name, entry] : colorMaps.maps)
        out.maps.try_emplace(name, entry);
    return out;
}

std::shared_ptr<const groomlab::GuideSet> guideSet(const Splines& guides, const groomlab::ScalpMesh& scalp)
{
    auto set = std::make_shared<groomlab::GuideSet>();
    uint64_t next = 1;
    for (const Spline& s : guides)
    {
        if (s.size() < 2)
            continue;
        std::vector<groomlab::Vec3f> points;
        points.reserve(s.size());
        for (const glm::dvec3& p : s.points)
            points.push_back(toVec(p));
        set->add(next++, std::move(points));
    }
    set->rootOnMesh(scalp);
    return set;
}

Splines toSplines(const groomlab::StrandSet& strands)
{
    Splines out(strands.strandCount());
    const std::vector<float>& widths = strands.widths();
    const bool hasWidths = widths.size() == strands.pointCount();
    std::vector<std::pair<std::string, const std::vector<float>*>> pointChannels, strandChannels;
    for (const std::string& name : strands.pointChannelNames())
        if (const std::vector<float>* v = strands.pointChannel(name); v && v->size() == strands.pointCount() && !name.starts_with('_'))
            pointChannels.emplace_back(name, v);
    for (const std::string& name : strands.strandChannelNames())
        if (const std::vector<float>* v = strands.strandChannel(name); v && v->size() == strands.strandCount() && !name.starts_with('_'))
            strandChannels.emplace_back(name, v);
    for (size_t i = 0; i < out.size(); ++i)
    {
        Spline& s = out[i];
        s.kind = Kind::Poly;
        const size_t n = strands.strandPointCount(i), begin = strands.strandBegin(i);
        const groomlab::Vec3f* p = strands.strandPoints(i);
        s.points.reserve(n);
        for (size_t k = 0; k < n; ++k)
            s.points.push_back({p[k].x, p[k].y, p[k].z});
        if (hasWidths)
            s.pointAttrs[std::string(attr::kWidth)].assign(widths.begin() + ptrdiff_t(begin), widths.begin() + ptrdiff_t(begin + n));
        s.attrs[std::string(attr::kId)] = double(strands.ids()[i]);
        // Every channel under GroomLab's name: what the export maps to Unreal's groom_* attributes.
        for (const auto& [name, values] : pointChannels)
            s.pointAttrs[name].assign(values->begin() + ptrdiff_t(begin), values->begin() + ptrdiff_t(begin + n));
        for (const auto& [name, values] : strandChannels)
            s.attrs[name] = (*values)[i];
    }
    return out;
}

void setGraph(GroomEvaluator& evaluator, const std::string& graphJson)
{
    try
    {
        evaluator.setGraphJson(graphJson);
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error(std::string("groom graph: ") + e.what());
    }
}

/** The evaluator's strands at `seconds` on its scalp and guides as set; throws when it makes none. */
const GroomEvaluator::Result& evaluateAt(GroomEvaluator& evaluator, double seconds)
{
    GroomEvaluator::Request request;
    request.time = seconds;
    const GroomEvaluator::Result& result = evaluator.evaluate(request);
    if (!result.strands)
        throw std::runtime_error(result.errors.empty() ? std::string("groom graph: its output makes no strands") : result.errors.front());
    return result;
}

std::vector<std::string> warningsOf(const GroomEvaluator& evaluator, const GroomEvaluator::Result& result)
{
    std::vector<std::string> out = result.errors;
    for (const auto& [name, note] : evaluator.nodeNotes())
        out.push_back(name + ": " + note);
    return out;
}
} // namespace groomLabBridge

GrownStrands growStrands(const geo::Mesh& scalp, const Splines& guides, const std::string& graphJson, double seconds)
{
    LILA_ZONE();
    using namespace groomLabBridge;
    GroomEvaluator evaluator;
    // One evaluation: the first area measured is the reference, so k would be 1 anyway.
    evaluator.setScaleWithScalp(false);
    setGraph(evaluator, graphJson);
    const std::shared_ptr<const groomlab::ScalpMesh> mesh = scalpMesh(scalp);
    evaluator.setScalp(mesh);
    evaluator.setGuides(guides.empty() ? nullptr : guideSet(guides, *mesh));
    const GroomEvaluator::Result& result = evaluateAt(evaluator, seconds);
    GrownStrands out;
    out.strands = toSplines(*result.strands);
    out.warnings = warningsOf(evaluator, result);
    return out;
}

namespace
{
using namespace scene;
using namespace curve::helpers;

/** The groom nodes computed so far, for the hook that reads their scalps' earlier frames and feeds their simulations. */
struct Registry
{
    std::mutex mutex;
    std::set<NodeId> nodes;
};

Registry& registry()
{
    static Registry r;
    return r;
}

/** Guides at least this close to the mirror plane are the same guide drawn on it: no copy is made of it. */
constexpr float kOnPlane = 1e-4f;
/** Set on the IDs of the guides the groom mirrors across its symmetry plane. */
constexpr std::uint64_t kMirrorBit = std::uint64_t(1) << 62;

float& component(groomlab::Vec3f& v, int axis)
{
    return axis == 0 ? v.x : axis == 1 ? v.y : v.z;
}

/** The scalp as a Lila surface for the lock mesher. */
std::unique_ptr<Surface> surfaceOf(const geo::Mesh& m, const glm::dmat4& matrix)
{
    geo::Mesh out = m;
    for (geo::Index p = 0; p < out.pointCapacity(); ++p)
        if (out.pointValid(p))
            out.setPosition(p, D3(matrix * glm::dvec4(out.position(p), 1.0)));
    return std::make_unique<Surface>(std::move(out));
}

/** The Cards nodes' cards as a mesh: quads, uv0 the atlas cell, uv1 the unique world layout. */
geo::Mesh cardsMesh(const groomlab::CardMesh& c)
{
    geo::Mesh m;
    for (const groomlab::Vec3f& p : c.positions)
        m.addPoint({p.x, p.y, p.z});
    for (size_t q = 0; q + 3 < c.quads.size(); q += 4)
    {
        const geo::Index v[4] = {geo::Index(c.quads[q]), geo::Index(c.quads[q + 1]), geo::Index(c.quads[q + 2]), geo::Index(c.quads[q + 3])};
        const geo::Index f = m.addFace(std::span<const geo::Index>(v, 4));
        if (f == geo::kInvalid)
            continue;
        auto uv0 = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
        auto uv1 = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(1));
        size_t k = 0;
        m.forEachFaceCorner(f, [&](geo::Index cn) {
            const size_t i = v[k++];
            uv0[cn] = {i < c.u.size() ? c.u[i] : 0.0f, i < c.v.size() ? c.v[i] : 0.0f};
            uv1[cn] = {i < c.worldU.size() ? c.worldU[i] : 0.0f, i < c.worldV.size() ? c.worldV[i] : 0.0f};
        });
    }
    return m;
}

/**
 * The groom: its guides (the stored GroomLab hierarchy, posed on the scalp, plus any curves connected to inCurve)
 * through its graph. Hands out the hierarchy as lock splines (outGuides, what the guides curve shows and the lock tools
 * edit), the locks the Locks node's guides make (outCurve resolved, outMesh swept), the graph's strands (outStrands) and
 * its cards (outCards); each only when something reads it.
 */
class GroomNode final : public Node
{
public:
    /** A look preview (previewLook): params over the graph's, shown without being set. */
    groomlab::gpu::Overrides mPreview;
    bool mPreviewing = false;
    PreviewResult mPreviewResult;

    /** Set while an export evaluates the node: strands are made, and kept in mExported. */
    bool mExport = false;
    GroomExportFrame mExported;

    ~GroomNode() override
    {
        std::lock_guard lock(registry().mutex);
        registry().nodes.erase(id());
    }

    void compute(const EvalContext& ctx) override
    {
        LILA_ZONE();
        bool first = false;
        {
            std::lock_guard lock(registry().mutex);
            first = registry().nodes.insert(id()).second;
        }
        std::vector<std::string> warnings;
        const LockOptions o = lockOptionsOf(*this);
        mMissedHistory = mMissedFeed = false;
        mReferenceToStore = 0.0;
        try
        {
            const MeshPtr& m = as<MeshPtr>(idx(*this, "inMesh"));
            const glm::dmat4& matrix = as<glm::dmat4>(idx(*this, "meshMatrix"));
            if (m != mScalpFrom || matrix != mScalpMatrix)
            {
                mScalpFrom = m;
                mScalpMatrix = matrix;
                mScalp = m && m->numFaces() > 0 ? groomLabBridge::scalpMesh(*m, matrix) : nullptr;
                mSurface.reset();
                mColorMaps = m ? groomLabBridge::colorSetMaps(*m) : groomlab::ScalpMaps{};
                ++mColorMapsVersion;
            }
            const std::string& text = s(*this, "hierarchy");
            if (text != mHierarchyText || !mHierarchy)
            {
                mHierarchyText = text;
                mHierarchy = std::make_shared<const groomlab::GuideHierarchy>(text.empty() ? groomlab::GuideHierarchy()
                                                                                           : groomlab::GuideHierarchy::fromJson(text));
            }
            groomlab::GuideHierarchy posed = *mHierarchy;
            posed.evaluate(mScalp.get());
            setOutput(idx(*this, "outGuides"), makeCurveData(lockSplines(posed, o)));

            Graph& g = Graph::current();
            auto read = [&](const char* a) { return !g.destinations({id(), idx(*this, a), -1}).empty(); };
            const bool locks = read("outMesh") || read("outCurve");
            const bool strands = read("outStrands") || mExport;
            const bool cards = read("outCards");
            if (!locks && !strands && !cards)
            {
                report(warnings);
                return;
            }
            if (!mEvaluator)
                mEvaluator = std::make_unique<GroomEvaluator>();
            GroomEvaluator& e = *mEvaluator;
            setGraph(e);
            mPreviewResult = {};
            e.setPartsJson(s(*this, "parts"));
            e.setFlowJson(s(*this, "flow"));
            setScalpMaps(e, warnings);
            e.setSculptsJson(s(*this, "sculpts"));
            e.setCorrectivesJson(s(*this, "correctives"));
            e.setCardMaps(s(*this, "cardMaps"));
            e.setFramesPerSecond(anim::playback().fps);
            const bool scale = b(*this, "scaleWithScalp");
            e.setScaleWithScalp(scale);
            e.setScaleReference(f(*this, "scaleReference"));
            e.setScalp(mScalp);
            const DataPtr& curves = as<DataPtr>(idx(*this, "inCurve"));
            if (curves != mCurvesFrom || mGuidesHierarchy != mHierarchy || mGuidesScalp != mScalp || !mGuides)
            {
                mGuides = guides(posed, curve::splinesOf(value(idx(*this, "inCurve"))), mScalp.get());
                mCurvesFrom = curves;
                mGuidesHierarchy = mHierarchy;
                mGuidesScalp = mScalp;
            }
            e.setGuides(mGuides);
            e.setScalpHistory(historyAt(ctx.time));
            GroomEvaluator::Request request;
            request.time = b(*this, "useSceneTime") ? ctx.time : f(*this, "time");
            request.wantCards = cards || (mExport && e.hasCardsNode());
            // A graph from elsewhere (an imported .groom) has no Locks node: no locks from it.
            const bool lockNode = e.hasNode(groomGraph::kLocks);
            if (!strands && !cards && lockNode)
                request.stopAt = groomGraph::kLocks;
            const GroomEvaluator::Result* resultAt = &e.evaluate(request);
            // A look preview: the GPU from the strands just grown, else the CPU with the preview's params set.
            groomlab::StrandsPtr previewStrands;
            if (mPreviewing && strands)
            {
                if (gpuPreview(e, previewStrands, mPreviewResult.why))
                    mPreviewResult.gpu = true;
                else
                {
                    groomLabBridge::setGraph(e, previewGraph());
                    resultAt = &e.evaluate(request);
                    previewStrands = resultAt->strands;
                }
            }
            const GroomEvaluator::Result& result = *resultAt;
            for (const std::string& w : result.errors)
                warnings.push_back(w);
            for (const auto& [name, note] : e.nodeNotes())
                warnings.push_back(name + ": " + note);
            mMissedHistory = !historyAt(ctx.time) && e.scalpHistoryFrames() > 0;
            mMissedFeed = first && steppedPast(e);
            mReferenceToStore = scale && !(f(*this, "scaleReference") > 0.0) ? e.scaleReference() : 0.0;
            if (mExport)
            {
                mExported.strands = result.strands;
                mExported.cardStrands = result.cardStrands;
                mExported.scalp = mScalp;
                mExported.guides = groomlab::StrandSet();
                // The primary guides, what Unreal simulates the hair on.
                for (const groomlab::HierarchyGuide& gd : posed.guides())
                    if (gd.parent == groomlab::kNoGuide && gd.points.size() >= 2)
                        mExported.guides.addStrand(gd.id, gd.points, {}, gd.root);
            }
            if (strands)
            {
                const groomlab::StrandsPtr shown = previewStrands ? previewStrands : result.strands;
                if (shown != mStrandsFrom || !mStrands)
                {
                    mStrands = makeCurveData(shown ? groomLabBridge::toSplines(*shown) : Splines());
                    mStrandsFrom = shown;
                }
                setOutput(idx(*this, "outStrands"), mStrands);
                setOutput(idx(*this, "strandCount"), int64_t(result.strands ? result.strands->strandCount() : 0));
            }
            if (cards)
            {
                if (result.cards != mCardsFrom || !mCards)
                {
                    mCards = std::make_shared<geo::Mesh>(result.cards ? cardsMesh(*result.cards) : geo::Mesh());
                    mCardsFrom = result.cards;
                }
                setOutput(idx(*this, "outCards"), MeshPtr(mCards));
            }
            if (locks && lockNode)
            {
                const groomlab::GuidesPtr made =
                    groomlab::valueAs<groomlab::GuidesPtr>(e.evaluateNode(groomGraph::kLocks, 1.0f, request.time));
                Splines lockCurves;
                if (made)
                    for (const groomlab::Guide& guide : made->guides())
                        lockCurves.push_back(lockSplineOf(guide, o));
                if (!mSurface && m && m->numFaces() > 0)
                    mSurface = surfaceOf(*m, matrix);
                Splines resolved = resolveLocks(lockCurves, mSurface.get(), o, &warnings);
                LockMesh lm = lockMesh(resolved, o);
                setOutput(idx(*this, "lockCount"), int64_t(lm.locks));
                setOutput(idx(*this, "outCurve"), makeCurveData(std::move(resolved)));
                setOutput(idx(*this, "outMesh"), MeshPtr(std::make_shared<geo::Mesh>(std::move(lm.mesh))));
            }
        }
        catch (const std::exception& e)
        {
            // Nothing grows from a graph that can't load.
            warnings.push_back(e.what());
            setOutput(idx(*this, "outMesh"), MeshPtr());
            setOutput(idx(*this, "outCards"), MeshPtr());
            setOutput(idx(*this, "outStrands"), makeCurveData(Splines()));
            setOutput(idx(*this, "outCurve"), makeCurveData(Splines()));
            setOutput(idx(*this, "strandCount"), int64_t(0));
            setOutput(idx(*this, "lockCount"), int64_t(0));
            mStrands.reset();
            mStrandsFrom.reset();
        }
        report(warnings);
    }

    /**
     * Before an evaluation at `now` (scene seconds): reads the scalp's earlier frames the graph needs (its Jiggle
     * nodes'), moving the scene's time to each one not read yet. True when the node has a new history to compute with.
     */
    bool sampleHistory(Graph& g, double now)
    {
        if (!prepare())
            return dropHistory();
        const int frames = mEvaluator->scalpHistoryFrames();
        if (frames <= 0)
            return dropHistory();
        Sources src = sources(g);
        if (!src.mesh)
            return dropHistory();
        const auto pointsNow = [&]() -> std::vector<groomlab::Vec3f> {
            g.evaluateUpstream(src.reads);
            const Node* source = g.node(src.mesh->node);
            const MeshPtr mesh = source ? source->as<MeshPtr>(src.mesh->attr) : MeshPtr();
            return mesh ? groomLabBridge::worldPoints(*mesh, src.matrixAt(g)) : std::vector<groomlab::Vec3f>{};
        };
        const std::vector<groomlab::Vec3f> current = pointsNow();
        if (current.empty())
            return dropHistory();

        const double fps = anim::playback().fps;
        const auto key = [](double seconds) { return std::int64_t(std::llround(seconds * 1e6)); };
        std::vector<double> times;
        std::vector<std::int64_t> keys;
        for (int k = 1; k <= frames; ++k)
        {
            times.push_back(anim::secondsOf(anim::frameOf(now) - k));
            keys.push_back(key(times.back()));
        }
        std::vector<double> loopSeconds;
        for (const double frame : mEvaluator->scalpHistoryLoopFrames(anim::frameOf(now)))
        {
            times.push_back(anim::secondsOf(frame));
            loopSeconds.push_back(times.back());
            keys.push_back(key(times.back()));
        }
        bool moved = false;
        const groomlab::ScalpHistoryPtr history = mSampler.history(
            key(now), keys, 1.0 / fps, current,
            [&](std::size_t k) {
                moved = true;
                g.setTime(times[k], false);
                return pointsNow();
            },
            false, loopSeconds);
        if (moved)
            g.setTime(now, false);
        const bool changed = history != mHistory || mHistoryAt != now;
        mHistory = history;
        mHistoryAt = now;
        return changed;
    }

    /**
     * Before an evaluation at `now`: steps the graph's Simulate nodes over the frames before `now` they haven't stepped,
     * moving the scene's time to each and reading the scalp and guides there, so a jump ahead simulates as playing does.
     * True when it stepped any.
     */
    bool feedSimulation(Graph& g, double now)
    {
        if (!b(*this, "useSceneTime") || !prepare())
            return false;
        Sources src = sources(g);
        if (!src.mesh)
            return false;
        GroomEvaluator& e = *mEvaluator;
        e.setFramesPerSecond(anim::playback().fps);
        bool moved = false;
        e.setSimulationFeed([&](double seconds) {
            moved = true;
            g.setTime(seconds, false);
            g.evaluateUpstream(src.reads);
            const Node* source = g.node(src.mesh->node);
            const MeshPtr mesh = source ? source->as<MeshPtr>(src.mesh->attr) : MeshPtr();
            if (!mesh || mesh->numFaces() == 0)
                return false;
            const auto scalp = groomLabBridge::scalpMesh(*mesh, src.matrixAt(g));
            static const Splines none;
            const Node* curveNode = src.curves ? g.node(src.curves->node) : nullptr;
            const Splines& curves = curveNode ? curve::splinesOf(curveNode->value(src.curves->attr)) : none;
            groomlab::GuideHierarchy posed = mHierarchy ? *mHierarchy : groomlab::GuideHierarchy();
            posed.evaluate(scalp.get());
            e.setScalp(scalp);
            e.setGuides(guides(posed, curves, scalp.get()));
            return true;
        });
        const groomlab::SimRun run = e.simulateBefore(now);
        e.setSimulationFeed({});
        if (moved)
            g.setTime(now, false);
        return run.frames > 0;
    }

    /** The last compute needed the scalp's earlier frames and had none (its first, before the hook knew its graph). */
    bool missedHistory() const { return mMissedHistory; }
    /** The node's first compute stepped its Simulate nodes over earlier frames without the scalp there. */
    bool missedFeed() const { return mMissedFeed; }
    /** The area the groom is 1x at, measured on a compute while Scale Reference was 0, for the hook to store (0: none). */
    double referenceToStore() const { return mReferenceToStore; }

    void restartSimulation()
    {
        if (mEvaluator)
            mEvaluator->invalidateSimulation();
    }

private:
    /** Where the scalp, its matrix and the guide curves come from, for reading them at another time. */
    struct Sources
    {
        std::optional<PlugRef> mesh, matrix, curves;
        std::vector<NodeId> reads;

        glm::dmat4 matrixAt(Graph& g) const
        {
            const Node* n = matrix ? g.node(matrix->node) : nullptr;
            return n ? n->as<glm::dmat4>(matrix->attr) : glm::dmat4(1.0);
        }
    };

    Sources sources(Graph& g) const
    {
        Sources out;
        out.mesh = g.source({id(), idx(*this, "inMesh"), -1});
        out.matrix = g.source({id(), idx(*this, "meshMatrix"), -1});
        out.curves = g.source({id(), idx(*this, "inCurve"), -1});
        for (const auto& p : {out.mesh, out.matrix, out.curves})
            if (p)
                out.reads.push_back(p->node);
        return out;
    }

    /** The evaluator holds the node's graph; false when it can't load. */
    bool prepare()
    {
        if (!mEvaluator)
            mEvaluator = std::make_unique<GroomEvaluator>();
        try
        {
            setGraph(*mEvaluator);
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    /** The stored maps, with the scalp's color sets as maps of their own (painted with Lila's vertex paint). */
    void setScalpMaps(GroomEvaluator& e, std::vector<std::string>& warnings)
    {
        const std::string& maps = s(*this, "scalpMaps");
        if (mColorMaps.maps.empty())
        {
            e.setScalpMapsJson(maps);
            mMapsVersion = 0;
            return;
        }
        if (maps == mMapsText && mMapsVersion == mColorMapsVersion)
            return;
        try
        {
            e.setScalpMaps(groomLabBridge::withColorSetMaps(maps, mColorMaps));
        }
        catch (const std::exception& ex)
        {
            warnings.push_back(std::string("scalp maps: ") + ex.what());
        }
        mMapsText = maps;
        mMapsVersion = mColorMapsVersion;
    }

    void setGraph(GroomEvaluator& e) const
    {
        const std::string& graph = s(*this, "graph");
        groomLabBridge::setGraph(e, graph.empty() ? groomGraph::defaultGraph() : graph);
    }

    /** The stored graph with the preview's params set on it: what a preview the GPU can't run evaluates on the CPU. */
    std::string previewGraph() const
    {
        const std::string& stored = s(*this, "graph");
        nlohmann::json g = nlohmann::json::parse(stored.empty() ? groomGraph::defaultGraph() : stored);
        for (nlohmann::json& n : g["nodes"])
            if (const auto it = mPreview.find(n.value("name", std::string())); it != mPreview.end())
                for (const auto& [param, value] : it->second)
                    if (const double* d = std::get_if<double>(&value))
                        n["params"][param] = *d;
        return g.dump();
    }

    /**
     * The preview on the GPU: the look chain from the first overridden node, from the strands the evaluator last grew
     * there. False with why when it can't (the strands come from the CPU then).
     */
    bool gpuPreview(GroomEvaluator& e, groomlab::StrandsPtr& out, std::string& why)
    {
        namespace gpu = groomlab::gpu;
        const groomlab::GraphDocument* doc = e.viewDocument();
        if (!doc)
        {
            why = "the graph didn't load";
            return false;
        }
        if (!gpu::previewableOverrides(*doc, mPreview, &why))
            return false;
        gpu::LookChain full;
        if (!gpu::findLookChain(*doc, &full, &why))
            return false;
        const std::size_t first = gpu::firstOverridden(*doc, full, mPreview);
        if (first == gpu::kNoStage)
        {
            why = "the node isn't in the GPU look chain";
            return false;
        }
        const gpu::LookChain chain = gpu::chainFrom(*doc, full, first);
        gpu::LookRun run;
        groomlab::StrandsPtr base, output;
        const GroomEvaluator::Result& last = e.last();
        if (!gpu::readLookParams(*doc, chain, mPreview, &run.params, &why) || !gpu::lookStrands(*doc, chain, &base, &output, &why) ||
            !gpu::readLookFields(*doc, chain, last.scalp.get(), &run.fields, &why) || !gpu::lookFieldsHold(*doc, chain, run.fields, &why))
            return false;
        if (!mLookInputs)
            mLookInputs = std::make_unique<gpu::LookInputs>();
        if (!mLookInputs->read(*doc, chain, run.params, last.parts.get(), &run.inputs, &why, {}, last.scalp.get(), &run.fields, last.scalp))
            return false;
        if (!mEngine)
        {
            std::shared_ptr<LookDevice> device = makeLookDevice();
            if (!device)
            {
                why = "no GPU device (the UI makes one)";
                return false;
            }
            mEngine = makeDeviceLookEngine(std::move(device));
        }
        try
        {
            mEngine->setBase(base, last.scalp.get());
            mEngine->run(run, false);
            auto strands = std::make_shared<groomlab::StrandSet>(*output);
            strands->mutablePositions() = mEngine->readPositions();
            const std::vector<float> widths = mEngine->readWidths();
            if (widths.size() == strands->pointCount())
                strands->mutableWidths() = widths;
            out = std::move(strands);
            return true;
        }
        catch (const std::exception& ex)
        {
            why = std::string("the GPU run failed: ") + ex.what();
            mEngine.reset();
            return false;
        }
    }

    /** The hierarchy's guides with their mirror copies when the symmetry is on, then the connected curves. */
    groomlab::GuidesPtr guides(const groomlab::GuideHierarchy& posed, const Splines& curves, const groomlab::ScalpMesh* scalp) const
    {
        auto flat = std::make_shared<groomlab::GuideSet>(posed.flatten());
        const int axis = int(i(*this, "symmetry")) - 1;
        if (axis >= 0 && axis < 3)
        {
            std::vector<groomlab::Guide> copies;
            for (const groomlab::Guide& g : flat->guides())
            {
                groomlab::Guide c = g;
                bool onPlane = true;
                for (groomlab::Vec3f& p : c.points)
                {
                    onPlane = onPlane && std::abs(component(p, axis)) < kOnPlane;
                    component(p, axis) = -component(p, axis);
                }
                if (onPlane)
                    continue;
                c.id = g.id | kMirrorBit;
                component(c.frameUp, axis) = -component(c.frameUp, axis);
                copies.push_back(std::move(c));
            }
            for (groomlab::Guide& c : copies)
            {
                const std::uint64_t cid = c.id;
                flat->add(cid, c.points) = std::move(c);
            }
        }
        std::uint64_t next = 1;
        for (const groomlab::Guide& g : flat->guides())
            next = std::max(next, (g.id & ~kMirrorBit) + 1);
        for (const Spline& sp : curves)
        {
            if (sp.size() < 2)
                continue;
            std::vector<groomlab::Vec3f> points;
            for (const D3& p : sp.points)
                points.push_back(groomLabBridge::toVec(p));
            flat->add(next++, std::move(points));
        }
        if (scalp && !scalp->triangles().empty())
            flat->rootOnMesh(*scalp);
        return flat;
    }

    static bool steppedPast(const GroomEvaluator& evaluator)
    {
        for (const groomlab::SimStatus& status : evaluator.simulationStatus())
            if (status.frames > 1)
                return true;
        return false;
    }

    groomlab::ScalpHistoryPtr historyAt(double now) const { return mHistory && mHistoryAt == now ? mHistory : nullptr; }

    bool dropHistory()
    {
        const bool had = mHistory != nullptr;
        mHistory.reset();
        mSampler.clear();
        return had;
    }

    void report(const std::vector<std::string>& warnings)
    {
        std::string w;
        for (const std::string& line : warnings)
            w += (w.empty() ? "" : "; ") + line;
        if (!w.empty() && w != as<std::string>(idx(*this, "warning")))
            log::warn("{}: {}", name(), w);
        setOutput(idx(*this, "warning"), w);
    }

    MeshPtr mScalpFrom;
    glm::dmat4 mScalpMatrix{0.0};
    std::shared_ptr<const groomlab::ScalpMesh> mScalp;
    std::unique_ptr<Surface> mSurface;
    std::string mHierarchyText;
    std::shared_ptr<const groomlab::GuideHierarchy> mHierarchy;
    DataPtr mCurvesFrom;
    std::shared_ptr<const groomlab::GuideHierarchy> mGuidesHierarchy;
    std::shared_ptr<const groomlab::ScalpMesh> mGuidesScalp;
    groomlab::GuidesPtr mGuides;
    /** Keeps the graph's results between evaluations, so an edit re-runs only the nodes it reaches. */
    std::unique_ptr<GroomEvaluator> mEvaluator;
    std::unique_ptr<groomlab::gpu::LookEngine> mEngine;
    std::unique_ptr<groomlab::gpu::LookInputs> mLookInputs;
    groomlab::ScalpMaps mColorMaps;
    uint64_t mColorMapsVersion = 0, mMapsVersion = 0;
    std::string mMapsText;
    groomlab::StrandsPtr mStrandsFrom;
    DataPtr mStrands;
    std::shared_ptr<const groomlab::CardMesh> mCardsFrom;
    std::shared_ptr<geo::Mesh> mCards;
    groomlab::ScalpHistorySampler mSampler;
    groomlab::ScalpHistoryPtr mHistory;
    double mHistoryAt = 0.0;
    bool mMissedHistory = false;
    bool mMissedFeed = false;
    double mReferenceToStore = 0.0;
};

/**
 * Before each evaluation every groom whose graph reads the scalp's earlier frames gets them and its Simulate nodes step
 * the frames skipped; after it, a groom that computed without them (its first compute) gets them and computes again,
 * and a scalp area measured as the scale reference is stored on the node.
 */
void sampleHistories(Graph& g, bool after)
{
    if (sim::sampling())
        return;
    std::vector<NodeId> ids;
    {
        std::lock_guard lock(registry().mutex);
        ids.assign(registry().nodes.begin(), registry().nodes.end());
    }
    const double now = g.time();
    bool touched = false;
    for (NodeId id : ids)
    {
        Node* n = g.node(id);
        if (!n || n->type().name != "groom")
            continue;
        auto* node = static_cast<GroomNode*>(n);
        if (after && node->referenceToStore() > 0.0)
        {
            // Measured, not an edit: no undo step of its own.
            UndoStack& u = UndoStack::get();
            const bool was = u.enabled();
            u.setEnabled(false);
            g.setValue({id, node->type().attrIndex("scaleReference"), -1}, node->referenceToStore());
            u.setEnabled(was);
            touched = true;
        }
        const bool missedFeed = node->missedFeed();
        if (after && !node->missedHistory() && !missedFeed)
            continue;
        const bool sampled = node->sampleHistory(g, now);
        if (after && missedFeed)
            node->restartSimulation();
        const bool fed = (!after || missedFeed) && node->feedSimulation(g, now);
        if (fed || sampled || (after && missedFeed))
        {
            g.touch(id);
            touched = true;
        }
    }
    if (after && touched)
        g.evaluate();
}
} // namespace

PreviewResult previewLook(Graph& g, NodeId groom, const std::string& node, const std::map<std::string, double>& params)
{
    auto* n = dynamic_cast<GroomNode*>(g.node(groom));
    if (!n)
        return {false, "not a groom"};
    for (const auto& [param, value] : params)
        n->mPreview[node][param] = value;
    n->mPreviewing = true;
    g.touch(groom);
    g.evaluateUpstream({groom});
    return n->mPreviewResult;
}

void endPreviewLook(Graph& g, NodeId groom)
{
    auto* n = dynamic_cast<GroomNode*>(g.node(groom));
    if (!n || !n->mPreviewing)
        return;
    n->mPreview.clear();
    n->mPreviewing = false;
    g.touch(groom);
}

GroomExportFrame groomExportFrame(Graph& g, NodeId groom, double seconds)
{
    auto* n = dynamic_cast<GroomNode*>(g.node(groom));
    if (!n)
        throw std::invalid_argument("not a groom");
    n->mExport = true;
    n->mExported = {};
    g.setTime(seconds, false);
    g.touch(groom);
    try
    {
        g.evaluateUpstream({groom});
    }
    catch (...)
    {
        n->mExport = false;
        throw;
    }
    n->mExport = false;
    if (!n->mExported.strands)
        throw std::runtime_error(n->name() + " grew no strands at " + std::to_string(seconds) + " s");
    return std::move(n->mExported);
}

std::unique_ptr<scene::Node> makeGroomNode()
{
    return std::make_unique<GroomNode>();
}

void registerGroomLabType()
{
    Graph::addEvaluateHook(sampleHistories);
    registerLockOps();
    registerGroomGnNodes();
}
} // namespace lila::groom
