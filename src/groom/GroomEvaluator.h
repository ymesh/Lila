// Adapted from GroomLab's groom_session.h/.cpp (github.com/thekesslereffect/groomlab, Apache-2.0,
// see src/groom/groomlab/LICENSE and NOTICE).
#pragma once

#include <groomlab/cards.h>
#include <groomlab/correctives.h>
#include <groomlab/flow.h>
#include <groomlab/follow_scalp.h>
#include <groomlab/graph.h>
#include <groomlab/graph_io.h>
#include <groomlab/groom.h>
#include <groomlab/hair_color.h>
#include <groomlab/parts.h>
#include <groomlab/paths.h>
#include <groomlab/scalp_maps.h>
#include <groomlab/sculpt.h>
#include <groomlab/simulation.h>
#include <groomlab/strand_geometry.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lila::groom
{
/** Cards rebuild while a brush stroke is live only when the last rebuild took no longer than this (ms). */
inline constexpr double kLiveCardBuildBudgetMs = 250.0;

/** What an evaluation's errors say while the scalp has another topology than the groom was made on. */
inline constexpr const char* kScalpTopologyChanged =
    "the scalp's vertex order or triangles differ from the groom's: strands are rooted again by position";

/**
 * Keeps one groom evaluated: a GroomLab graph on a scalp with its guides, poses, correctives, parts, flow, painted maps,
 * colliders and the scalp's earlier frames. Every input is parsed or bound once per change, the graph's results are
 * cached between evaluations (an edit re-runs only the nodes it reaches), and the result is the same bit for bit
 * however it was reached: fresh, incrementally, on any thread count.
 *
 * Following the scalp: the graph evaluates on the scalp's rest pose and each strand is carried to the scalp now by its
 * root's frame. Scaling with the scalp: the groom is evaluated k times its stored size, k from the scalp's rest area (or
 * the host's transform scale) against a reference, so the stored graph and guides stay at 1x.
 *
 * Setters no-op on the same pointer, text or content, so a host may call them on every evaluation. One thread at a
 * time; the graph evaluates in parallel inside.
 */
/**
 * The graph part of an evaluation run away from its evaluator on any thread (GroomEvaluator::detach and its stroke and
 * warm variants). It owns everything it reads: a copy of a document and the inputs as they were.
 */
class DetachedEvaluation;

class GroomEvaluator
{
public:
    struct Request
    {
        float density = 1.0f;
        /** Multiplies `density` while a stroke is live (hasPreview), on a stroke document of its own. */
        float sculptDensity = 1.0f;
        /** Points the result may hold; 0: no limit. */
        double pointBudget = 0.0;
        /** A Strands node to evaluate instead of the output ("" or another kind of node: the output). */
        std::string viewNode;
        double time = 0.0;
        /** Interactive view: renderOnly nodes are bypassed and viewportOnly ones run; off, the other way round. */
        bool viewport = false;
        /** The Cards nodes build their cards on this evaluation. */
        bool buildCards = false;
        /** Also the Cards nodes' cards, from the output at cardDensity with no point budget, on a copy of the graph of
         * their own, laid out in one atlas (the baked one's cells kept): Result::cards. */
        bool wantCards = false;
        float cardDensity = 1.0f;
        /** Also the StrandMesh nodes' strands (evaluateStrandMeshes at the request's time): Result::strandMeshes. */
        bool wantStrandMeshes = false;
        /** Result::bounds worked out (when the strands or guides changed); off, left as they were. */
        bool wantBounds = true;
        /** A node the host runs itself with what follows it: only what feeds its inputs is evaluated, and the strands
         * stay as they were. "" or no such node: the view as ever. */
        std::string stopAt;
        /** Following the scalp, the host carries the strands itself: they are handed on at rest with Result::follow. */
        bool hostCarries = false;
    };

    struct Bounds
    {
        groomlab::Vec3f lo, hi;
        /** False: nothing to bound. */
        bool valid = false;
    };

    struct Message
    {
        std::string text;
        bool error = true;
    };

    /** Counters that move only when what they name changed (a cache hit leaves them). */
    struct Versions
    {
        std::uint64_t evaluation = 0;
        std::uint64_t strands = 0;
        std::uint64_t hierarchy = 0;
        std::uint64_t cards = 0;
        std::uint64_t strandMeshes = 0;
    };

    /** What the last evaluation did, by node name. */
    struct Stats
    {
        /** The node shown. */
        std::string view;
        std::size_t strands = 0, points = 0, guides = 0;
        std::size_t cards = 0, cardStrands = 0, cardTriangles = 0, cardTextures = 0, cardFeathers = 0;
        /** Card maps are set, and some Cards node's cards no longer match what they were baked from. */
        bool cardsBaked = false, cardsStale = false;
        struct CardNode
        {
            std::size_t cards = 0, triangles = 0, textures = 0, feathers = 0;
            bool stale = false;
        };
        std::map<std::string, CardNode> cardNodes;
        /** The HairColor and RenderData nodes the look comes from, and every color node making the output. */
        std::string hairColor, renderData;
        std::vector<std::string> colorNodes, colorBlends;
        /** Milliseconds per node in its last run, and how it ran ("cached" when it didn't). */
        std::map<std::string, double> times;
        std::map<std::string, std::string> runs;
        /** Each node's strand channels, from its cached result. */
        std::map<std::string, std::vector<std::string>> channels;
        std::map<std::string, std::string> notes;
        int scalpHistoryFrames = 0;
        /** The placed instances whose strand the view has. */
        std::size_t instances = 0;
        std::size_t restEvaluations = 0;
        std::size_t cacheBytes = 0, cacheEvictions = 0;
        /** A stroke document is kept (a stroke made one, or warmStroke). */
        bool strokeDocument = false;
        std::vector<groomlab::SimStatus> simulations;

        /** Every field as a JSON object (the MCP and Python stats). */
        std::string toJson() const;
    };

    struct Result
    {
        /** Null on failure. */
        groomlab::StrandsPtr strands;
        /** Set (Request::hostCarries, following the scalp): the strands are at rest and this carries them to the scalp
         * now; placed() is the strands where they are. */
        groomlab::FollowCarryPtr follow;
        groomlab::StrandsPtr placed() const { return follow ? follow->carried() : strands; }
        /** Names this set of strands (the same serial: the same strands); strandsChanges says which strands differ from
         * those of serial changesBase, null when not known. */
        std::uint64_t strandsSerial = 0;
        std::uint64_t changesBase = 0;
        groomlab::ChangesPtr strandsChanges;
        std::string viewedNode;
        /** The node shown is a Scatter: its roots draw as dots. */
        bool viewedRoots = false;
        /** The node shown is a Clump: its clump centers, each its root on the scalp. Null otherwise. */
        std::shared_ptr<const std::vector<groomlab::Vec3f>> clumpCenters;
        /** What an Interpolate feeding what is shown takes after a GuideCache or Simulate node moved its guides this
         * frame; null when none moves them. */
        groomlab::GuidesPtr movingGuides;
        /** The density the strands were evaluated at. */
        float density = 1.0f;
        /** The strands' and the guides' points. */
        Bounds bounds;
        /** The scalp as evaluated: vertex maps as color sets, refined by the Scalp nodes' subdivision. */
        groomlab::MeshPtr scalp;
        /** The same before it is refined. */
        groomlab::MeshPtr cageScalp;
        /** The guides on the scalp, posed and corrected (the preview while one is set). */
        groomlab::HierarchyPtr hierarchy;
        /** The guides as stored, rooted on the scalp. */
        groomlab::HierarchyPtr authoredHierarchy;
        groomlab::PartsPtr parts;
        groomlab::FlowPtr flow;
        /** wantCards: the Cards nodes' cards laid out, the strands they were built from, and each node's summary. */
        std::shared_ptr<const groomlab::CardMesh> cards;
        groomlab::StrandsPtr cardStrands;
        std::vector<groomlab::CardNodeSummary> cardNodes;
        /** wantStrandMeshes: the StrandMesh nodes' strands. */
        std::vector<groomlab::StrandMeshSource> strandMeshes;
        Versions versions;
        std::vector<std::string> errors;
        /** To say once per change. */
        std::vector<Message> messages;
    };

    /** The default graph, no inputs. */
    GroomEvaluator();
    /** Throws GraphError for a bad graph, std::runtime_error for bad guide, part, flow, corrective or sculpt data. */
    explicit GroomEvaluator(groomlab::GroomData data);
    ~GroomEvaluator();
    GroomEvaluator(const GroomEvaluator&) = delete;
    GroomEvaluator& operator=(const GroomEvaluator&) = delete;

    // Stored inputs.

    /** Throws GraphError for a bad graph, leaving the evaluator as it was. The new graph takes the old one's cache. */
    void setGraphJson(const std::string& graphJson);
    /** No graph: evaluations make no strands and say `error`. Setting `graphJson` again does nothing. */
    void dropGraph(const std::string& graphJson, const std::string& error);
    bool hasGraph() const { return mDoc != nullptr; }
    const std::string& graphError() const { return mGraphError; }
    const std::string& hierarchyError() const { return mHierarchyError; }
    /** Guides as stored (GuideHierarchy::toJson; empty: none). A bad text keeps the error and no guides. */
    void setHierarchyJson(const std::string& json);
    void setHierarchy(std::shared_ptr<const groomlab::GuideHierarchy> hierarchy);
    /** The guides as stored at the groom's scale, not rooted. */
    std::shared_ptr<const groomlab::GuideHierarchy> storedHierarchy() const { return mParsedHierarchy; }
    void setPartsJson(const std::string& json);
    void setFlowJson(const std::string& json);
    void setCorrectivesJson(const std::string& json);
    /** Corrective layers with a slot take these weights on every frame (useLiveWeights). */
    void setCorrectiveWeights(std::map<int, float> weights);
    void clearCorrectiveWeights();
    /** The Sculpt nodes' edits (SculptSet JSON). */
    void setSculptsJson(const std::string& json);
    const groomlab::SculptSet& sculpts() const { return mSculpts; }
    /** The stored inputs that did not parse, "<label>: <why>". */
    std::vector<std::string> inputErrors() const;

    // Scalp and maps.

    /** World space at the evaluation time; may carry a rest pose and color sets. */
    void setScalp(groomlab::MeshPtr scalp);
    /** The scalp's points at another time on its topology; `restPoints` replace the rest pose. */
    void updateScalpPoints(std::vector<groomlab::Vec3f> points, const std::vector<groomlab::Vec3f>* restPoints = nullptr);
    const groomlab::MeshPtr& rawScalp() const { return mData.scalp; }
    /** The groom's other scalps by name, joined after its own (scalp_pieces.h). */
    void setScalps(groomlab::NamedScalps scalps);
    groomlab::MeshPtr joinedScalp();
    /** The topology the groom was made on; another one is said in every evaluation's errors. */
    void setScalpFingerprint(std::uint64_t fingerprint);
    /** The scalp as evaluated (maps laid on, refined). */
    groomlab::MeshPtr scalp();
    /** The scalp with its maps, not refined: what vertex maps are painted on. */
    groomlab::MeshPtr cageScalp();
    /** The Catmull-Clark levels the scalp is refined by (the most of scalpSubdivisions). */
    int scalpSubdivision() const;
    /** Each scalp's own levels by name ("scalp": the groom's own). */
    std::map<std::string, int> scalpSubdivisions() const;
    /** `joined` refined as the evaluator refines its scalp. */
    groomlab::MeshPtr refineAsScalp(const groomlab::MeshPtr& joined) const;
    void setScalpMapsJson(const std::string& json);
    void setScalpMaps(groomlab::ScalpMaps maps);
    const groomlab::ScalpMaps& scalpMaps() const { return mScalpMaps; }
    /** The scalp maps as stored: the text set, else the maps as JSON. */
    std::string scalpMapsJson() const { return mScalpMapsFromText ? mScalpMapsText : mScalpMaps.toJson(); }
    const std::string& scalpMapsError() const { return mScalpMapsError; }
    /** The map catalog when the scalp carries its vertex maps already; used while no scalp maps are set. */
    void setMapCatalog(groomlab::MapCatalog maps);
    /** Where the Cards nodes' baked maps are, "<map>=<path>;..." (parseCardMaps): the atlas their cells keep. */
    void setCardMaps(const std::string& cardMaps);
    /** What a scalp shows of map or guide layer `name` (`channel`: rgb, or r g b a as gray). */
    groomlab::ScalpMapView scalpMapView(const std::string& name, const std::string& channel = "rgb");

    // Moving inputs.

    /** Flat guides, world space, rooted. */
    void setGuides(groomlab::GuidesPtr guides);
    /** Hierarchy guides replaced by ID (simulated, cached). */
    void setGuidePoses(groomlab::GuidesPtr poses);
    groomlab::GuidesPtr guidePoses() const { return mData.guidePoses; }
    void setControls(std::map<std::string, float> controls);
    /** Meshes for ColliderMesh nodes by name; a null mesh removes the name. */
    void setMeshes(std::map<std::string, groomlab::MeshPtr> meshes);
    void setMesh(const std::string& name, groomlab::MeshPtr mesh);
    /** The scalp's earlier frames, for Jiggle nodes. */
    void setScalpHistory(groomlab::ScalpHistoryPtr history);
    void setPathResolver(groomlab::PathResolver resolver);
    void setPathResolver(std::shared_ptr<const groomlab::PathResolver> resolver);
    /** Off for one-shot evaluation: lower peak memory, nothing kept. */
    void setCacheIntermediates(bool enabled);
    /** Frames per second the time-dependent nodes count frames at (an NTSC rate made exact). */
    void setFramesPerSecond(double fps);
    double framesPerSecond() const { return mFps; }

    // Following the scalp.

    void setFollowScalp(bool on);
    bool followScalp() const { return mData.followScalp; }
    /** The last evaluation followed the scalp (on, and the scalp away from its rest pose). */
    bool following() const { return mFollowing; }

    // Scaling with the scalp.

    void setScaleSource(groomlab::ScaleSource source);
    groomlab::ScaleSource scaleSource() const { return mData.scaleSource; }
    void setScaleWithScalp(bool on);
    bool scaleWithScalp() const { return mData.scaleSource != groomlab::ScaleSource::Off; }
    /** The scalp's transform scale as the host measures it; 0 or less: none. Handed in before its scalp. */
    void setScaleTransform(double scale);
    double scaleTransform() const { return mScaleTransform; }
    void setScaleTransformReference(double scale);
    double scaleTransformReference() const
    {
        return mData.scaleTransformReference > 0.0 ? mData.scaleTransformReference : mFirstTransform;
    }
    /** A line for the user when the scalp's own area changed the groom's scale, and its serial (0: none yet). */
    const std::string& scaleNotice() const { return mScaleNotice; }
    std::uint64_t scaleNoticeSerial() const { return mScaleNoticeSerial; }
    /** The area the groom is 1x at; 0 or less: the first area measured. */
    void setScaleReference(double area);
    double scaleReference() const { return mData.scaleReference > 0.0 ? mData.scaleReference : mFirstArea; }
    double scaleArea() const { return mScaleArea; }
    /** k: what the groom is scaled by now. */
    double groomScale() const { return mGroomScale; }

    /** Whether the result depends on the time through the graph, the correctives or a scalp history. */
    bool readsTime() const;

    // Simulation (simulation.h).
    //
    // Each Simulate node keeps every frame from its start frame to the last one stepped. An evaluation at a time steps
    // a node whose cacheMode is "live" on to that frame from its last one when the frame is not cached; going back
    // reads the cache. A change of the node's settings, of the frame rate, or of what it is fed at a frame it shows
    // again drops its frames. Between two frames the two are blended; before its start frame, and while a preview is
    // set, a node passes its guides through.

    /** How the host's inputs at another time reach the evaluator (SimFeed); without one, frames on the way are stepped
     * in one go toward the inputs as they are. */
    void setSimulationFeed(groomlab::SimFeed feed) { mSimFeed = std::move(feed); }
    /** Off: an evaluation never steps a node; a frame neither cached nor baked shows the guides as groomed. */
    void setSimulationStepping(bool on) { mSimStepping = on; }
    /** The groom at a frame depends on earlier frames: a scalp history, or a Simulate node not covered by a bake that
     * matches its settings over max(firstFrame, its start) to lastFrame. */
    bool readsEarlierFrames(const std::string& viewNode, int firstFrame, int lastFrame) const;
    /** The Simulate nodes making the output (not bypassed), upstream first. */
    std::vector<std::string> simulateNodes() const;
    /** Every Simulate node making the output stepped on to `frame` from its last frame, in either cacheMode; the
     * inputs are put back as the last evaluation had them. */
    groomlab::SimRun simulateTo(int frame, const groomlab::SimProgress& progress = {});
    /** Steps the Simulate nodes an evaluation at `seconds` would step (live, stepping on, no bake covering it) on to the
     * last frame before `seconds` through the feed, so the frame at `seconds` is stepped from there as when playing.
     * The inputs are put back. */
    groomlab::SimRun simulateBefore(double seconds);
    /** Drops a node's frames ("": every node's); a bake stays. */
    void invalidateSimulation(const std::string& node = {});
    std::vector<groomlab::SimStatus> simulationStatus() const;
    /** A node's frame as simulated; null when not cached. */
    groomlab::GuidesPtr simulatedFrame(const std::string& node, int frame) const;
    /** A bake of a node; null removes it. While its stamp matches the node's settings its frames come from it. */
    void setBakedSimulation(const std::string& node, groomlab::BakedSimulationPtr baked);
    groomlab::BakedSimulationPtr bakedSimulation(const std::string& node) const;
    /** What a bake of the node made now records (SimSettings::hash of its params); 0 without the node. */
    std::uint64_t simulationStamp(const std::string& node) const;

    // Previews (a brush tool edits these between evaluations).

    /** Replaces the stored guides while set. `final`: shown as it is (poses and correctives already in it). */
    void setPreviewHierarchy(std::shared_ptr<groomlab::GuideHierarchy> preview, bool final = false);
    std::shared_ptr<groomlab::GuideHierarchy> previewHierarchy() const { return mPreviewHierarchy; }
    bool previewFinal() const { return mPreviewFinal; }
    /** Replace the stored parts or flow while set (bound to the scalp as evaluated). */
    void setPreviewOverlays(std::shared_ptr<const groomlab::PartSet> parts, std::shared_ptr<const groomlab::FlowSet> flow);
    bool hasPreviewOverlays() const { return mPreviewParts || mPreviewFlow; }
    /** `values` (`channels` per cage vertex: 1 gray, 4 color) stand in for the stored map `name`; null clears. */
    void setPreviewVertexMap(const std::string& name, std::shared_ptr<const std::vector<float>> values, int channels = 1);
    /** A texture map's live image, for scalpMapView only; null clears. */
    void setPreviewTextureMap(const std::string& name, std::shared_ptr<const groomlab::TexturePreview> image);
    std::uint64_t previewMapVersion() const { return mPreviewMapVersion; }
    /** A stroke that previews no guides is live (a scalp map stroke). Off: true when an evaluation took the stroke's
     * density, so the host evaluates again at the full one. */
    bool setLiveStroke(bool on);
    bool liveStroke() const { return mLiveStroke; }
    /** A brush stroke is live: a guide preview, or setLiveStroke. */
    bool hasPreview() const { return mPreviewHierarchy != nullptr || mLiveStroke; }
    /** A live param drag: the viewport's Subdivide nodes hand on their coarse levels (EvalContext::liveDrag). */
    void setLiveDrag(bool on) { mLiveDrag = on; }
    bool liveDrag() const { return mLiveDrag; }

    // Sculpt nodes (sculpt.h).

    /** The Sculpt node brushes shape ("" none): while set, Result::hierarchy is that node's output as editable guides. */
    void setSculptTarget(const std::string& node);
    const std::string& sculptTarget() const { return mSculptTarget; }
    /** A live stroke on Sculpt node `node`: `edits` stand in for its stored ones (null clears), `shown` is the view's
     * hierarchy meanwhile. */
    void setPreviewSculpt(const std::string& node, groomlab::SculptPtr edits, std::shared_ptr<const groomlab::GuideHierarchy> shown);
    bool hasPreviewSculpt() const { return mPreviewSculpt != nullptr; }
    const std::string& previewSculptNode() const { return mPreviewSculptNode; }
    /** What a stroke on Sculpt node `node` reads. Throws std::runtime_error: no graph, no such Sculpt node, nothing fed. */
    groomlab::SculptInput sculptInput(const std::string& node);
    /** The target's output as editable guides (the stroke's while one is live); null without a target. */
    groomlab::HierarchyPtr sculptHierarchy();

    // Evaluation.

    /** Never throws: a failure leaves no strands and says why in errors. Valid until the next evaluate. */
    const Result& evaluate(const Request& request);
    const Result& last() const { return mResult; }
    /** The output at `density` and `seconds`. Throws what the evaluation threw. */
    groomlab::StrandsPtr evaluate(float density, double seconds, bool buildCards = false);
    /** What evaluate(density, seconds) gives when the last view evaluated it alike (the output, no stroke, the same
     * density and time, no cards built, nothing thinned to a budget, every node in the same evaluation mode): its
     * strands placed, no evaluation. Null otherwise. */
    groomlab::StrandsPtr viewedAsRendered(float density, double seconds) const;
    /** One node by name ("" the output) with the last evaluation's inputs, in a scratch copy of the graph. */
    groomlab::Value evaluateNode(const std::string& name, float density, double seconds, bool buildCards = false);
    /** The output at full density with the last evaluation's inputs, on a copy of the graph of its own. Throws as the
     * graph does; null without a graph. */
    groomlab::StrandsPtr evaluateFullDensity(double seconds);
    void releaseFullDensity();
    /** The StrandMesh nodes making the output, each with its strands at `seconds` (its density, thinned to its
     * maxStrands, carried while following); a node whose strands didn't change hands the same pointer again. */
    std::vector<groomlab::StrandMeshSource> evaluateStrandMeshes(double seconds);
    void releaseStrandMeshes();

    // Strokes, and evaluating away from the evaluator.

    /** Evaluates the stroke document ahead (the graph at density times sculptDensity) with the last inputs, so a
     * stroke's first dab makes again only what it changed. True when a node was evaluated. */
    bool warmStroke(const Request& request);
    /** warmStroke run by DetachedEvaluation::run on any thread; adoptWarm takes the result in unless the stroke
     * document was evaluated, released or made again since. Null where warmStroke would evaluate nothing. */
    std::shared_ptr<DetachedEvaluation> detachWarm(const Request& request);
    bool adoptWarm(DetachedEvaluation& run);
    /** A live stroke's dab evaluated away from the evaluator: the stroke document is lent to the run until adoptStroke
     * gives it back, with a copy of the preview guides as they were. Null where only evaluate can do it. */
    std::shared_ptr<DetachedEvaluation> detachStroke(const Request& request);
    bool adoptStroke(DetachedEvaluation& run);
    bool strokeLent() const { return mStrokeLent; }
    /** Drops the stroke document and its cache (the brush tool put away). */
    void releaseStroke();
    bool strokeDocument() const { return mSculptDoc != nullptr; }
    /** The graph part of evaluate(request) on a copy of the document, run on any thread; adopt makes the evaluated copy
     * the evaluator's document. `after`: chained on a run not adopted yet. Null with no graph, a preview, Simulate
     * nodes or cards wanted. */
    std::shared_ptr<DetachedEvaluation> detach(const Request& request, const std::shared_ptr<DetachedEvaluation>& after = nullptr);
    bool adopt(DetachedEvaluation& run);

    // What the last evaluation used and made.

    /** The guides on the scalp, posed then corrected; null without guides. */
    groomlab::HierarchyPtr hierarchy();
    /** Posed, no correctives: what shot sculpts are recorded against. */
    groomlab::HierarchyPtr posedHierarchy();
    /** The guides as stored, rooted on the scalp: no poses, correctives or preview. */
    groomlab::HierarchyPtr authoredHierarchy() { return rootedHierarchy(); }
    /** Earlier scalp frames the graph reads (the Jiggle nodes'). */
    int scalpHistoryFrames(const std::string& viewNode = {}) const;
    std::vector<double> scalpHistoryLoopFrames(double now, const std::string& viewNode = {}) const;
    /** A Cards node, or a StrandMesh node, makes the output. */
    bool hasCardsNode() const;
    /** The graph holds a node of this name. */
    bool hasNode(const std::string& name) const { return mDoc && mDoc->nodesByName.count(name) != 0; }
    bool hasStrandMeshNode() const;
    /** The last evaluation wanted cards. */
    bool cardsEvaluated() const { return mCardDoc != nullptr; }
    /** A Subdivide node making the output refines the viewport's strands. */
    bool viewRefinesStrands() const;
    groomlab::HairLook hairLook() const;
    /** The look as JSON for hosts: the HairLook's fields, its "lobes", and the params of the nodes it comes from. */
    std::string hairLookJson() const;
    Stats stats() const;
    std::vector<std::pair<std::string, double>> nodeTimes() const;
    std::vector<std::pair<std::string, std::string>> nodeNotes() const;
    /** Operator evaluations since the graph was set, and of those the incremental ones. */
    std::size_t evaluationCount() const;
    std::size_t incrementalCount() const;
    /** The rest pose's evaluations (pinned or fitted instances). */
    std::size_t restEvaluations() const;
    const groomlab::GroomData& data() const { return mData; }
    /** The document the last evaluation used, its cache as it is; valid until the next setGraphJson. */
    const groomlab::GraphDocument* viewDocument() const { return viewDoc(); }

private:
    const groomlab::GraphDocument* viewDoc() const { return mViewSculpt && mSculptDoc ? mSculptDoc.get() : mDoc.get(); }
    std::unique_ptr<groomlab::GraphDocument> newDocument() const;
    void jobContext(DetachedEvaluation& job, float density, const Request& request) const;
    std::string shownName(const groomlab::GraphDocument& doc, const std::string& viewNode) const;
    groomlab::GuidesPtr sculptOutput(groomlab::GraphDocument& doc, const groomlab::EvalContext& ctx);
    std::shared_ptr<const std::vector<groomlab::Vec3f>> clumpCenterDots(groomlab::GraphDocument& doc, groomlab::NodeId node,
                                                                        const groomlab::EvalContext& ctx);
    void buildCards(const groomlab::EvalContext& ctx, float cardDensity, std::vector<Message>& messages);
    bool refreshCardAtlas(std::vector<Message>& messages);
    const groomlab::StrandBoxes& followBoxes(const groomlab::StrandsPtr& rest);
    std::unique_ptr<groomlab::GraphDocument> parseScaled(const std::string& graphJson) const;
    void installGraph(std::unique_ptr<groomlab::GraphDocument> parsed);
    void refreshScale();
    void absorbScaleTransform();
    void scaleHierarchy();
    std::map<std::string, float> scaledControls() const;

    int scalpLevelsOn(const groomlab::MeshPtr& joined, groomlab::MeshPtr* levelled) const;
    groomlab::ScalpHistoryPtr historyJoined();
    void bindScalp(std::vector<std::string>& errors);
    void bindRefined();
    groomlab::HierarchyPtr rootedHierarchy();
    void updateActiveHierarchy();
    void applyCorrectives(groomlab::GuideHierarchy& guides, const groomlab::ScalpMesh* on) const;
    std::uint64_t correctivesKey(std::uint64_t key) const;
    void bindOverlays();
    void prepareFollow(std::vector<Message>& messages);
    void prepareTime(double seconds, std::vector<std::string>& errors, std::vector<Message>& messages);
    void fillExternals(groomlab::ExternalInputs& externals) const;
    groomlab::EvalContext context(groomlab::ExternalInputs& externals) const;
    groomlab::StrandsPtr pinToRest(const groomlab::StrandsPtr& strands, const std::string& viewedNode,
                                   const groomlab::EvalContext& ctx);
    groomlab::StrandsPtr carry(const groomlab::StrandsPtr& strands, int slot);
    const groomlab::PathResolver* paths() const;

    struct SimNode
    {
        groomlab::SimCache cache;
        groomlab::BakedSimulationPtr baked;
        /** Its frame for the last evaluation (the external "sim:<node>"); null: none. */
        groomlab::GuidesPtr shown;
        std::uint64_t shownHash = 0;
        /** Its inputs' hash when last checked at checkedSeconds: other inputs at the same time start it again. */
        bool checked = false;
        double checkedSeconds = 0.0;
        std::uint64_t checkedHash = 0;
        /** The inputs its last frame was stepped toward. */
        std::uint64_t towardHash = 0;
    };
    struct SimRef
    {
        std::string name;
        groomlab::NodeId id = 0;
        SimNode* node = nullptr;
    };
    std::vector<std::pair<std::string, groomlab::NodeId>> simulateNodeIds(const std::string& viewNode) const;
    std::vector<SimRef> simRefs(const std::string& viewNode);
    void checkSimInputs(SimRef& ref, double seconds, const groomlab::ExternalInputs& externals);
    std::pair<groomlab::GuidesPtr, std::uint64_t> simFrameAt(const SimNode& node, double seconds) const;
    groomlab::EvalContext simContext(double seconds, const groomlab::ExternalInputs& externals) const;
    groomlab::SimInputs simInputs(const SimRef& ref, const groomlab::EvalContext& ctx, std::uint64_t* hash);
    void stepSimulations(const Request& request, std::vector<std::string>& errors);
    void advanceSimulations(std::vector<SimRef>& nodes, int target, double nowSeconds, const groomlab::SimProgress& progress,
                            groomlab::SimRun& run);
    bool bakeUsable(const SimNode& node, double seconds) const;

    groomlab::GroomData mData;
    bool mCacheIntermediates = true;
    std::shared_ptr<const groomlab::PathResolver> mPaths;
    double mFps = 24.0;

    // The graph, parsed once per change at the groom's scale; mDoc is its clone that keeps the cache.
    std::unique_ptr<groomlab::GraphDocument> mParsed;
    std::unique_ptr<groomlab::GraphDocument> mDoc;
    std::unique_ptr<groomlab::RestPose> mRestPose;
    bool mGraphSet = false;
    std::string mGraphError;

    double mGroomScale = 1.0;
    double mScaleArea = 0.0;
    double mFirstArea = 0.0;
    double mScaleTransform = 0.0;
    double mFirstTransform = 0.0;
    double mMeasuredArea = 0.0, mMeasuredTransform = 0.0;
    bool mScaleMeasured = false;
    std::string mScaleNotice;
    std::uint64_t mScaleNoticeSerial = 0;
    std::map<std::string, double> mControlFactors;

    bool mHierarchySet = false;
    bool mHierarchyFromText = false;
    std::shared_ptr<const groomlab::GuideHierarchy> mRawHierarchy;
    std::shared_ptr<const groomlab::GuideHierarchy> mParsedHierarchy;
    std::shared_ptr<const groomlab::GuideHierarchy> mReboundHierarchy;
    std::shared_ptr<const groomlab::GuideHierarchy> mReboundFrom;
    std::uint64_t mReboundRest = 0;
    std::string mHierarchyError;
    std::uint64_t mParsedVersion = 0;
    std::uint64_t mReportedVersion = 0;
    std::uint64_t mRootedVersion = 0;
    std::uint64_t mRootedScalp = 0;
    bool mRooted = false;
    groomlab::HierarchyPtr mHierarchy;
    std::shared_ptr<groomlab::GuideHierarchy> mPreviewHierarchy;
    bool mPreviewFinal = false;
    groomlab::HierarchyPtr mActiveHierarchy;
    groomlab::HierarchyPtr mPosedHierarchy;
    std::uint64_t mActiveKey = 0;
    std::uint64_t mPosesHash = 0;
    bool mActiveValid = false;
    groomlab::Correctives mCorrectives;
    std::string mCorrectiveError;
    std::uint64_t mCorrectivesVersion = 0;
    bool mLiveWeights = false;
    std::map<int, float> mWeights;
    double mTime = 0.0;

    groomlab::SculptSet mSculpts;
    std::string mSculptError;
    std::map<std::string, std::pair<groomlab::SculptPtr, std::uint64_t>> mSculptExternals;

    groomlab::PartSet mPartSet;
    std::string mPartError;
    std::shared_ptr<const groomlab::PartSet> mPreviewParts;
    groomlab::PartsPtr mParts;
    std::uint64_t mPartsKey = 0;
    groomlab::FlowSet mFlowSet;
    std::string mFlowError;
    std::shared_ptr<const groomlab::FlowSet> mPreviewFlow;
    groomlab::FlowPtr mFlow;
    std::uint64_t mFlowKey = 0;
    const groomlab::PartSet* mShownParts = &mPartSet;
    const groomlab::FlowSet* mShownFlow = &mFlowSet;
    groomlab::PartSet mRefinedParts;
    std::uint64_t mRefinedPartsKey = 0;
    groomlab::FlowSet mRefinedFlow;
    std::uint64_t mRefinedFlowKey = 0;

    groomlab::MeshPtr mJoined, mJoinedMain;
    groomlab::NamedScalps mJoinedExtras;
    groomlab::ScalpHistoryPtr mJoinedHistory, mJoinedHistoryIn;
    groomlab::MeshPtr mJoinedHistoryOn;
    groomlab::MeshPtr mScalp;
    groomlab::MeshPtr mCageScalp;
    groomlab::MeshPtr mRefinedFrom;
    groomlab::GuidesPtr mGuides;
    groomlab::GuidesPtr mGuidesIn;
    std::uint64_t mGuidesOn = 0;
    groomlab::ScalpHistoryPtr mHistory;
    groomlab::ScalpHistoryPtr mHistoryIn;
    groomlab::MeshPtr mHistoryOn;
    std::uint64_t mMappedKey = 0;
    groomlab::MeshPtr mMappedFrom;
    std::uint64_t mMappedMapsKey = 0;
    std::vector<std::string> mMapErrors;
    bool mScalpMapsSet = false;
    std::string mScalpMapsText;
    bool mScalpMapsFromText = false;
    groomlab::ScalpMaps mScalpMaps;
    std::uint64_t mScalpMapsHash = 0;
    std::string mScalpMapsError;

    std::uint64_t mFingerprintKey = 0;
    bool mFingerprintDiffers = false;

    // Following the scalp: the inputs on the rest pose, each kept while what it comes from is.
    bool mFollowing = false;
    bool mFollowNoRestSaid = false;
    groomlab::MeshPtr mFollowRest;
    std::uint64_t mFollowRestKey = 0;
    groomlab::HierarchyPtr mFollowRooted;
    std::uint64_t mFollowRootedKey = 0;
    groomlab::HierarchyPtr mFollowActive;
    std::uint64_t mFollowActiveKey = 0;
    groomlab::PartsPtr mFollowParts;
    std::uint64_t mFollowPartsKey = 0;
    groomlab::FlowPtr mFollowFlow;
    std::uint64_t mFollowFlowKey = 0;
    groomlab::GuidesPtr mFollowGuidesIn, mFollowGuides;
    groomlab::MeshPtr mFollowGuidesOn;
    groomlab::ScalpHistoryPtr mFollowHistoryIn, mFollowHistory;
    groomlab::MeshPtr mFollowHistoryOn;
    struct Carried
    {
        groomlab::StrandsPtr in;
        groomlab::MeshPtr from, to;
        groomlab::StrandsPtr out;
    };
    /** The view's, the cards', full density's and evaluateNode's. */
    Carried mCarried[4];
    std::shared_ptr<const groomlab::StrandBoxes> mFollowBoxes;
    std::weak_ptr<const groomlab::StrandSet> mFollowBoxesOf;
    groomlab::MeshPtr mFollowBoxesFrom;

    // Generations: a detached run is adopted only into the document it was taken from.
    std::uint64_t mDocGeneration = 0, mGraphGeneration = 0, mStrokeDocGeneration = 0;
    /** A live stroke's document (another density), the cards', full density's and the StrandMesh nodes' (by density). */
    std::unique_ptr<groomlab::GraphDocument> mSculptDoc, mCardDoc, mFullDoc;
    std::map<float, std::unique_ptr<groomlab::GraphDocument>> mMeshDocs;
    struct MeshMemo
    {
        groomlab::StrandsPtr in;
        groomlab::MeshPtr from, to;
        std::size_t maxStrands = 0;
        groomlab::StrandsPtr out;
    };
    std::map<std::string, MeshMemo> mMeshMemos;
    bool mViewSculpt = false;
    bool mLiveStroke = false, mStrokeDensityUsed = false, mStrokeLent = false;
    bool mLiveDrag = false;
    std::size_t mWarmEvaluations = 0, mWarmIncrementals = 0, mReleasedEvaluations = 0, mReleasedIncrementals = 0;
    std::vector<Message> mDetachedMessages;
    /** The last view's context at its own density, no externals: what sculptInput and sculptHierarchy evaluate at. */
    groomlab::EvalContext mLastViewContext;
    /** What the view (0) and a stroke (1) showed last, for strandsSerial and strandsChanges. */
    struct Shown
    {
        std::uint64_t serial = 0;
        groomlab::NodeId node = 0;
        std::uint64_t graphSerial = 0;
        std::weak_ptr<const groomlab::StrandSet> strands;
    };
    Shown mShown[2];
    std::uint64_t mStrandsSerial = 0;
    double mLastCardBuildMs = 0.0, mLastStrandMeshMs = 0.0;
    std::string mCardMapsText;
    std::map<std::string, std::string> mCardMaps;
    std::string mCardAtlasPath;
    std::uint64_t mCardAtlasStamp = 0;
    groomlab::CardAtlas mCardAtlas;

    std::string mPreviewMapName;
    std::shared_ptr<const std::vector<float>> mPreviewMapValues;
    int mPreviewMapChannels = 1;
    std::shared_ptr<const groomlab::TexturePreview> mPreviewMapImage;
    std::uint64_t mPreviewMapVersion = 0;
    std::uint64_t mMapViewKey = 0;
    std::shared_ptr<const std::vector<float>> mMapViewValues;

    std::string mSculptTarget;
    std::uint64_t mSculptsVersion = 0, mSculptShownVersion = ~std::uint64_t(0);
    groomlab::HierarchyPtr mSculptShown;
    groomlab::GuidesPtr mSculptShownFrom;
    groomlab::MeshPtr mSculptShownOn;
    groomlab::SculptPtr mPreviewSculpt;
    std::string mPreviewSculptNode;
    std::uint64_t mPreviewSculptHash = 0;
    std::shared_ptr<const groomlab::GuideHierarchy> mPreviewSculptShown;

    std::map<std::string, std::unique_ptr<SimNode>> mSims;
    groomlab::SimFeed mSimFeed;
    bool mSimStepping = true;
    /** The last evaluation's density and budget: what simulateTo evaluates inputs at. */
    float mSimDensity = 1.0f;
    double mSimPointBudget = 0.0;

    Result mResult;
    /** The strands or guides changed while no evaluation wanted bounds. */
    bool mBoundsStale = false;
    std::exception_ptr mFailure;
};

class DetachedEvaluation
{
public:
    DetachedEvaluation(const DetachedEvaluation&) = delete;
    DetachedEvaluation& operator=(const DetachedEvaluation&) = delete;
    /** Evaluates the copy taken (chained: a copy of what the run before evaluated, which must have run). Never throws:
     * a failure is kept in error(). */
    void run();
    bool ran() const { return mRan.load(std::memory_order_acquire); }
    std::exception_ptr error() const { return mError; }
    double milliseconds() const { return mMs; }
    const GroomEvaluator::Request& request() const { return mRequest; }
    bool warm() const { return mWarm; }
    bool stroke() const { return mStroke; }

private:
    friend class GroomEvaluator;
    DetachedEvaluation() = default;
    std::unique_ptr<groomlab::GraphDocument> mDoc;
    std::shared_ptr<DetachedEvaluation> mAfter;
    std::uint64_t mBasedOn = 0;
    groomlab::ExternalInputs mExternals;
    std::shared_ptr<const groomlab::PathResolver> mPaths;
    groomlab::EvalContext mCtx;
    std::string mShown;
    GroomEvaluator::Request mRequest;
    std::exception_ptr mError;
    double mMs = 0.0;
    std::atomic<bool> mRan{false};
    bool mWarm = false;
    const groomlab::GraphDocument* mWarmBase = nullptr;
    std::size_t mWarmBaseEvaluations = 0;
    std::uint64_t mWarmBaseGeneration = 0;
    bool mStroke = false;
    std::shared_ptr<groomlab::GuideHierarchy> mStrokePreview;
    std::uint64_t mStrokeGraph = 0;
};
} // namespace lila::groom
