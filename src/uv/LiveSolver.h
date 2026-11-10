#pragma once

#include "core/Export.h"

#include "uv/Chart.h"
#include "uv/FinalMesh.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace lila::uv
{
namespace detail
{
class FinalOperator;
struct CageHold;
}

struct LiveOptions
{
    /**
     * Stretch against compression, -1..1, like UVLayout's squash/stretch weighting. Positive lets triangles grow in uv
     * (stretch) more freely than they shrink, so compression is what gets fixed; negative the reverse; 0 penalises both
     * alike (plain ARAP).
     */
    double balance = 0.0;
    /** Pull of boundary vertices towards a smooth outline, 0..1. */
    double boundarySmooth = 0.0;
    /** Rest shape at the island's starting uv area, so it keeps its size; off solves at 3D scale. */
    bool keepScale = true;
    /** Never fold a triangle that is not folded already (the step is cut short before one flips). */
    bool injective = true;
    /** Symmetric Dirichlet (SLIM) iterations run once the island settles, for a lower-distortion fold-free finish. */
    int slimSteps = 0;
    /** Settled once no vertex moves more than this fraction of the island's size in an iteration. */
    double tolerance = 2e-5;
    /** Iterations per wake before an island gives up and settles anyway. */
    int maxIterations = 3000;
    /** Sessions relax the exported mesh's triangles when the mesh's modifiers move its surface. */
    bool finalMesh = true;
};

enum class LiveBrush : uint8_t
{
    Relax,          // eases the region towards its relaxed shape
    Flatten,        // re-solves the region from its 3D shape, the rest held
    SmoothBoundary, // evens out the outline
    Pin,
    Unpin,
    Straighten, // pulls vertices onto the line of the stroke
    Pinch,      // draws vertices towards the centre (invert pushes)
    Grab,       // drags the region along, soft falloff, the rest follows
    Stiffen,
    Soften,
};

LILA_SCENE_API std::string_view liveBrushName(LiveBrush b);
/** False when the name is not a brush. */
LILA_SCENE_API bool liveBrushFromName(std::string_view name, LiveBrush& out);

/** One brush dab, in uv space or on the surface (object space). */
struct LiveDab
{
    LiveBrush brush = LiveBrush::Relax;
    bool surface = false;
    glm::dvec2 center{0.0};
    glm::dvec3 center3{0.0};
    double radius = 0.1;
    double strength = 0.5;
    bool invert = false;
    /** Grab: this dab's cursor motion, uv. */
    glm::dvec2 delta{0.0};
    /** Straighten: where the stroke began (uv; for surface dabs the uv under that point is used). */
    glm::dvec2 lineFrom{0.0};
    glm::dvec3 lineFrom3{0.0};
    /** First dab of a stroke. */
    bool first = false;
};

/** Smooth falloff weight for a normalised distance (0 at the centre, 1 at the rim). */
LILA_SCENE_API double liveFalloff(double t);

/**
 * One uv island solved by local/global ARAP (Liu et al. 2008) with an asymmetric stretch/compress weighting, per-vertex
 * stiffness, hard pins, soft targets (dragged vertices) and an optional boundary smoothing term. The global matrix only
 * depends on topology, stiffness, pins and which vertices have targets, so it is factorised once (sparse LDLT) and
 * reused until one of those changes; each iteration is a local step and two back-substitutions. Warm-started from the
 * chart's current uvs, it never folds a triangle that is not already folded.
 */
class LILA_SCENE_API LiveIsland
{
public:
    LiveIsland() = default;
    /** stiffness per vertex, 1 when empty. */
    explicit LiveIsland(Chart chart, const LiveOptions& opts = {}, std::vector<float> stiffness = {});
    LiveIsland(LiveIsland&&) noexcept = default;
    LiveIsland& operator=(LiveIsland&&) noexcept = default;
    LiveIsland(const LiveIsland&) = delete;
    LiveIsland& operator=(const LiveIsland&) = delete;

    void setOptions(const LiveOptions& opts);
    const LiveOptions& options() const { return mOpts; }

    /** uvs are kept current after every change. */
    const Chart& chart() const { return mChart; }
    Index vertexCount() const { return mChart.vertexCount(); }
    std::span<const float> stiffness() const { return mStiff; }
    bool pinned(Index v) const { return mChart.pinned[v] != 0; }

    void setPinned(Index v, bool on);
    /** 0.05 (floppy) .. 20 (rigid); 1 is normal. */
    void setStiffness(Index v, double k);
    /** A soft pin: the vertex is pulled to `at` with the weight (1 follows closely). */
    void setTarget(Index v, glm::dvec2 at, double weight = 1.0);
    void clearTargets();
    bool hasTargets() const { return mTargetCount > 0; }
    /** Moves a vertex directly (brushes); the solver takes it from there. */
    void setUv(Index v, glm::dvec2 uv);

    /** Solve from scratch (LSCM, SLIM, Tutte when needed), keeping pins, placement and size; then it relaxes live. */
    bool flatten();

    /** One local/global iteration. With weights (per vertex, 0..1), only that much of each vertex's move is taken. */
    void step(std::span<const double> weights = {});
    /** Iterates until settled or the iteration limit; returns iterations run. */
    int solve(int maxIterations = -1);

    void wake();
    bool settled() const { return mSettled; }
    /** Iterations since the last wake. */
    int iterations() const { return mIters; }
    /** Energy of the last local step (area weighted, 0 for an isometry). */
    double energy() const { return mEnergy; }
    size_t folds() const;
    /** How often the global matrix was factorised: once per topology/pin/stiffness/target-set change. */
    int factorizations() const { return mFactorizations; }
    /** Bumped whenever uvs, pins or stiffness change. */
    uint64_t version() const { return mVersion; }

    /** Applies a brush dab. Returns true when it touched the island. */
    bool applyDab(const LiveDab& dab);

    /** Rest scale: uv units per 3D unit. */
    double restScale() const { return mScale; }

    /**
     * Relaxes the island's exported triangles (where the mesh's modifiers put its uvs) instead of its own; null goes
     * back to the chart's. Brushes, pins and targets still act on the chart's vertices.
     */
    void setFinal(std::shared_ptr<const FinalIsland> island);
    bool hasFinal() const { return mFinal != nullptr; }

private:
    struct Frame
    {
        glm::dvec2 g[3];
        double area = 0.0;
    };

    void prepare();
    void buildPattern();
    void ensureFactor();
    void markDirty() { mMatrixDirty = true; }
    double maxStep(const std::vector<glm::dvec2>& dir) const;
    void prepareFinal();
    void factorFinal();
    std::vector<glm::dvec2> finalUvs(const std::vector<glm::dvec2>& x, bool offsets) const;
    double finalStep(std::vector<double>& bu, std::vector<double>& bv) const;
    void pushToChart();
    void touched();
    void restoreCentroid(glm::dvec2 c);
    glm::dvec2 centroid() const;
    void alignTo(const std::vector<glm::dvec2>& before);
    void slimPolish();
    std::vector<double> dabWeights(const LiveDab& dab, glm::dvec2& centerUv, glm::dvec2& fromUv) const;

    glm::dvec2 toInternal(glm::dvec2 p) const { return {p.x * mSign, p.y}; }

    Chart mChart;
    LiveOptions mOpts;
    double mScale = 1.0;
    double mSign = 1.0; // -1 for mirrored islands, solved flipped
    double mLength = 1.0;
    std::vector<Frame> mFrames;
    std::shared_ptr<const FinalIsland> mFinal;
    std::vector<Frame> mFinalFrames;
    std::shared_ptr<const UninfluencedFill> mFinalFill;
    std::shared_ptr<const detail::FinalOperator> mFinalOp;
    std::shared_ptr<const detail::CageHold> mCageHold;
    std::vector<float> mStiff;
    std::vector<glm::dvec2> mX;
    std::vector<glm::dvec2> mPinAt;
    std::vector<glm::dvec2> mTarget;
    std::vector<double> mTargetW;
    size_t mTargetCount = 0;
    std::vector<Index> mBoundary;                  // boundary vertices
    std::vector<std::array<Index, 2>> mBoundaryNb; // previous and next along the loop
    std::vector<uint8_t> mOnBoundary;
    std::vector<Index> mAdjStart, mAdj; // vertex neighbours (CSR)
    glm::dvec3 mLo3{0.0}, mHi3{0.0};

    struct Factor;
    std::shared_ptr<Factor> mFactor;
    bool mMatrixDirty = true;
    int mFactorizations = 0;

    bool mSettled = true;
    bool mPolished = true;
    int mIters = 0;
    double mEnergy = 0.0;
    uint64_t mVersion = 1;
    glm::dvec2 mHold{0.0};
    bool mHoldValid = false;
    glm::dvec2 mStrokeFrom{0.0};
};

/** Live islands of the faces (all when empty): the uv set's islands, also split along "seam" edges, with pins ("uvPin") and stiffness ("uvStiffness"). */
LILA_SCENE_API std::vector<LiveIsland> liveIslands(const geo::Mesh& mesh, std::span<const Index> faces, std::string_view uvSet, const LiveOptions& opts = {});

/** Writes uvs, and with pinsAndStiffness the "uvPin" and "uvStiffness" corner attributes, of the islands back to the mesh. */
/**
 * Points each island at its exported triangles, so it relaxes them (LiveIsland::setFinal). Returns the map used; a
 * cage map (with a warning when the stack changes the surface) leaves the islands on their own triangles.
 */
LILA_SCENE_API FinalMap attachFinal(std::span<LiveIsland> islands, const geo::Mesh& cage, std::string_view uvSet, const Stack& stack);

LILA_SCENE_API void writeLiveIslands(geo::Mesh& mesh, std::span<const LiveIsland> islands, std::string_view uvSet, bool pinsAndStiffness = true);

/** Edge flags (per edge capacity) of the mesh's "seam" attribute, for ChartOptions::extraSeams. */
LILA_SCENE_API std::vector<uint8_t> seamFlags(const geo::Mesh& mesh);

/**
 * Runs live islands on a worker thread, steered from the UI thread through queued edits. It iterates while any island
 * is awake, publishes uvs at most ~60 times a second through double-buffered frames, and sleeps (no CPU) once every
 * island has settled. The UI thread never waits on it except in sync() and waitIdle().
 */
class LILA_SCENE_API LiveSolver
{
public:
    using Edit = std::function<void(std::vector<LiveIsland>&)>;

    /** One island's published state. Shared between frames while the island does not change. */
    struct IslandState
    {
        uint64_t version = 0;
        std::vector<glm::dvec2> uv;
        std::vector<uint8_t> pinned;
        std::vector<float> stiffness;
        bool settled = true;
    };

    struct Frame
    {
        uint64_t epoch = 0;      // bumped by load()
        uint64_t generation = 0; // bumped by every publish
        std::vector<std::shared_ptr<const IslandState>> islands;
        bool idle = true;
        int iterations = 0; // total since the last load
    };

    LiveSolver();
    ~LiveSolver();
    LiveSolver(const LiveSolver&) = delete;
    LiveSolver& operator=(const LiveSolver&) = delete;

    /** Replaces the islands. */
    void load(std::vector<LiveIsland> islands);
    /** Queues an edit; the worker runs it before its next iteration and wakes. */
    void edit(Edit fn);
    /** Runs fn on the worker between iterations and waits for it, and for the frame it publishes. */
    void sync(const Edit& fn);

    /** Paused, it applies edits but runs no iterations. */
    void setPaused(bool on);
    bool paused() const;

    std::shared_ptr<const Frame> frame() const;
    uint64_t generation() const;
    /** Nothing queued and no island awake (or paused). */
    bool idle() const;
    bool waitIdle(int milliseconds);

private:
    void run();
    void publish(bool idle);

    std::vector<LiveIsland> mIslands; // worker only
    int mIterations = 0;              // worker only
    uint64_t mEpoch = 0;              // worker only

    mutable std::mutex mMutex;
    std::condition_variable mWake;
    std::condition_variable mIdleCv;
    std::vector<Edit> mQueue;
    bool mStop = false;
    bool mPaused = false;
    bool mAwake = false;
    bool mBusy = false;
    uint64_t mPass = 0;     // worker only: the pass being run
    uint64_t mFinished = 0; // last pass completed, published included
    std::shared_ptr<const Frame> mFront;
    std::shared_ptr<Frame> mBack;
    std::thread mThread;
};
} // namespace lila::uv
