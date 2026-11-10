#pragma once

#include "core/Export.h"

#include "brush/Brush.h"

#include <glm/glm.hpp>

#include <functional>
#include <vector>

/**
 * Stroke sampling, shared by every brush so they all feel the same: dabs at even spacing along the drag, pen pressure
 * through the pressure curves, a lazy mouse stabiliser, line strokes and jitter. The sampler works in screen pixels;
 * Stroke turns its samples into object-space Dabs through a surface query and hands them to a DabSink (a mesh brush
 * today, the GPU texture painter later).
 */
namespace lila::brush
{
/** One input event: widget pixels and pen pressure (1 for a mouse). */
struct StrokeInput
{
    glm::dvec2 position{0.0};
    double pressure = 1.0;
};

/** Where a dab goes on screen, with its per-dab variation. */
struct StrokeSample
{
    glm::dvec2 position{0.0};
    double pressure = 1.0;
    /** Unit direction of travel on screen, zero for the first dab. */
    glm::dvec2 direction{0.0};
    /** Screen distance along the stroke. */
    double distance = 0.0;
    uint32_t index = 0;
    /** Jitter's multipliers. */
    double size = 1.0;
    double opacity = 1.0;
    /** Tip angle, radians, counterclockwise on screen. */
    double angle = 0.0;
};

/** Screen-space stroke sampling: spacing, stabiliser, line strokes, jitter and tip angle. */
class LILA_SCENE_API StrokeSampler
{
public:
    /** Starts a stroke; freehand strokes put their first dab at the press. radiusPx sets the spacing and jitter scale. */
    void begin(const BrushSettings& s, const StrokeInput& in, double radiusPx);
    void move(const StrokeInput& in);
    /** Line strokes lay their dabs here, from the press to the last position; curve strokes along their smoothed path. */
    void end();
    void cancel();
    /** The radius the spacing follows, e.g. a world radius seen at a new depth. */
    void setRadius(double radiusPx);

    /** Samples produced since the last take. */
    std::vector<StrokeSample> take();

    bool active() const { return mActive; }
    StrokeMode mode() const { return mMode; }
    /** The stabilised brush position (the cursor, without a stabiliser). */
    glm::dvec2 brush() const { return mBrush; }
    glm::dvec2 cursor() const { return mCursor; }
    glm::dvec2 start() const { return mStart; }
    double spacingPx() const { return mStep; }
    /** A curve stroke's path so far, as drawn, for its preview. */
    const std::vector<glm::dvec2>& path() const { return mPath; }
    /** A curve stroke's path as it will be laid down: smoothed and evenly spaced. */
    std::vector<glm::dvec2> smoothedPath() const;

private:
    void stepTo(glm::dvec2 target, double pressure);
    void push(glm::dvec2 at, double pressure, glm::dvec2 dir);

    bool mActive = false;
    StrokeMode mMode = StrokeMode::Freehand;
    double mSpacing = 0.15;
    double mRadius = 1.0;
    double mStep = 1.0;
    double mLazy = 0.0;
    double mSmoothing = 0.0;
    Jitter mJitter;
    double mAngle = 0.0;
    bool mFollow = false;
    uint32_t mSeed = 0;

    glm::dvec2 mStart{0.0}, mCursor{0.0}, mBrush{0.0}, mLast{0.0}, mDir{0.0};
    double mPressure = 1.0, mLastPressure = 1.0;
    double mDistance = 0.0;
    uint32_t mIndex = 0;
    std::vector<StrokeSample> mOut;
    std::vector<glm::dvec2> mPath;
    std::vector<double> mPathPressure;
};

/** What a surface query returns for a screen position, in the painted mesh's object space. */
struct SurfaceHit
{
    glm::dvec3 center{0.0};
    glm::dvec3 normal{0.0, 1.0, 0.0};
    glm::dvec3 viewDir{0.0};
    glm::dvec3 eye{0.0};
    /** Screen +x at the hit, for the tip angle; zero when unknown. */
    glm::dvec3 right{0.0};
    /** Object units per screen pixel at the hit. */
    double pixelSize = 1.0;
    /** Object units per world unit. */
    double worldToLocal = 1.0;
};
using SurfaceQuery = std::function<bool(glm::dvec2 screen, SurfaceHit& out)>;

/** Takes the dabs of a stroke: a vertex color, weight, sculpt or texture brush. */
class DabSink
{
public:
    virtual ~DabSink() = default;
    virtual void dab(const Dab& d) = 0;
};

/** Pixels the brush covers at a hit for these settings (their world radius seen from there, for World). */
LILA_SCENE_API double radiusPixels(const BrushSettings& s, const SurfaceHit& hit);

/** A dab for a sample over a hit: radius, pressure curves, flow, jitter and tip; direction from the previous dab. */
LILA_SCENE_API Dab makeDab(const BrushSettings& s, const StrokeSample& sample, const SurfaceHit& hit, const Dab* previous);

/** A stroke from input events to dabs: sampler, surface query, pressure curves and the sink. */
class LILA_SCENE_API Stroke
{
public:
    /**
     * Starts a stroke. With requireHit a freehand stroke needs the press over the surface and returns false (starting
     * nothing) when it isn't; otherwise, and for line strokes, it starts anywhere and dabs land where the surface is.
     */
    bool begin(const BrushSettings& s, const StrokeInput& in, SurfaceQuery query, DabSink& sink, bool requireHit = true);
    void move(const StrokeInput& in);
    void end();
    void cancel();

    bool active() const { return mSampler.active(); }
    const BrushSettings& settings() const { return mSettings; }
    const StrokeSampler& sampler() const { return mSampler; }
    /** Every dab sent so far, in order. */
    const std::vector<Dab>& dabs() const { return mDabs; }

private:
    void flush();

    BrushSettings mSettings;
    StrokeSampler mSampler;
    SurfaceQuery mQuery;
    DabSink* mSink = nullptr;
    std::vector<Dab> mDabs;
};
} // namespace lila::brush
