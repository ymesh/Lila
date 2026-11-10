#pragma once

#include "core/Export.h"
#include "core/ExtensionContext.h"
#include "core/Registry.h"

#include "brush/Brush.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "geo/Symmetry.h"
#include "render/RenderMesh.h"

#include <glm/glm.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * Sculpting on a dense mesh: brushes, mask and freeze layers, exact mirror strokes, and the GPU copy kept in step with
 * only the vertices each dab touched. Everything is in the mesh's object space.
 */
namespace lila::sculpt
{
using geo::Index;

enum class Brush : uint8_t
{
    Draw,
    ClayStrips,
    Inflate,
    Grab,
    Smooth,
    Flatten,
    Pinch,
    Crease,
    Scrape,
    /** A narrow V groove that pinches its edges in. */
    DamStandard,
    /** Cuts flat to a plane just under the brush. */
    TrimDynamic,
    /** Smooths only along the brush normal and eases toward its plane: crisp flat forms. */
    Polish,
    /** A cloth fold: a ridge along the stroke, pinched from the sides. */
    Fold,
    Mask,
    Relax,
    Freeze,
    /** Colour only: paints the polypaint layer without moving anything. */
    Paint,
    /** Raises what dips below the average plane up to it; Ctrl scrapes. */
    Fill,
    /** Builds up rounded layers of clay toward a plane just above the surface. */
    Clay,
    /** Raises a plateau to one height over where the stroke started, however often it passes. */
    Layer,
    /** Swells a rounded ball: draws out and pushes points away from the centre. */
    Blob,
    /** Pushes points away from the brush centre; Ctrl pinches. */
    Magnify,
    /** Flattens onto the plane under the stroke's first dab, held for the whole stroke. */
    Planar,
    /** Pulls the surface under the cursor along with it, thinning as it goes: horns and tendrils. */
    SnakeHook,
    /** Grab with a soft, wide reach that falls off smoothly, keeping the form's volume. */
    ElasticGrab,
    /** Pushes the surface along the stroke, sliding over it. */
    Nudge,
    /** Pushes along the stroke and presses flat, like a thumb dragged through clay. */
    Thumb,
    /** Twists the patch under the brush around its normal as the cursor circles. */
    Rotate,
    /** Bends the part under the brush around the ring where it meets the rest, like a limb at its joint. */
    Pose,
    /** Drags the open borders under the brush and the few rings of points next to them. */
    Boundary,
    Count,
    /** A registered brush, named by BrushSettings::custom. */
    Custom = 0xff,
};

enum class Falloff : uint8_t
{
    Smooth,
    Sphere,
    Root,
    Sharp,
    Linear,
    Constant,
    Count
};

/** Brushes the cursor drags across the view plane from where the stroke began, instead of laying a trail of dabs. */
LILA_SCENE_API bool dragsCursor(Brush b);
LILA_SCENE_API std::string_view brushName(Brush b);
LILA_SCENE_API std::optional<Brush> brushFromName(std::string_view name);
LILA_SCENE_API std::string_view falloffName(Falloff f);
LILA_SCENE_API std::optional<Falloff> falloffFromName(std::string_view name);
/** Weight at t = distance / radius: 1 at the centre, 0 from the rim on. The brush core's shape of the same name. */
LILA_SCENE_API double falloffWeight(Falloff f, double t);
inline brush::Shape toShape(Falloff f) { return brush::Shape(f); }

struct BrushSettings
{
    Brush brush = Brush::Draw;
    /** The registered brush's name when brush is Custom. */
    std::string custom;
    /** Any brush core falloff: the sculpt shapes, bell, or a soft select curve. */
    brush::Falloff falloff = brush::Shape::Smooth;
    /** 0 to 1. */
    double strength = 0.5;
    /** Ctrl: dig instead of add, unmask, unfreeze, magnify instead of pinch. */
    bool invert = false;
    /** Leave points facing away from the view, so a stroke doesn't reach through thin parts. */
    bool frontFacesOnly = false;
    /** Copies of every dab about an object axis, on top of the mesh's mirror symmetry. */
    brush::Radial radial;
    /** Shape brushes also paint color where they move points; the Paint brush always does. */
    bool paint = false;
    /** With paint: color goes only where a stroke digs in, more the deeper it digs (dark seams along Dam Standard lines). */
    bool cavityPaint = false;
    /** Linear RGB. */
    glm::vec3 color{1.0f};
    /** Paint and Mask: blur the color or mask under the brush instead of laying it down (Shift). */
    bool blur = false;
    /** Smooth: keep even curvature and volume; off, plain neighbour averaging, which also flattens the form. */
    bool keepVolume = true;
    /** Pinch: how far it presses the surface in as it draws points together, 0 for none, 1 the default. */
    double pinchDepth = 1.0;
};

/** The brush's name: a built-in's, or the registered one's. */
LILA_SCENE_API std::string brushId(const BrushSettings& s);
/** Resolves a built-in or registered brush name into s. False when nothing has that name. */
LILA_SCENE_API bool setBrush(BrushSettings& s, std::string_view name);

class SculptMesh;

/** One point a registered brush reaches in a dab. */
struct BrushSample
{
    Index point = 0;
    /** Object space, as the dab began. */
    glm::dvec3 position{0.0};
    glm::dvec3 normal{0.0, 1.0, 0.0};
    /** Falloff times strength, pen pressure and flow, less the mask: how far this point should go, usually 0 to 1. */
    double weight = 0.0;
};

/** The area a registered brush works in for one dab. */
struct BrushArea
{
    const SculptMesh& mesh;
    const brush::Dab& dab;
    /** Weighted average normal and position of the points under the brush. */
    glm::dvec3 normal{0.0, 1.0, 0.0};
    glm::dvec3 center{0.0};
    double radius = 0.0;
    /** -1 while inverted (Ctrl), else 1. */
    double sign = 1.0;
};

/**
 * A sculpt brush added by an extension (or Lila). displace gives each point's move for one dab, in object space. It
 * runs on many threads at once, so it may only read: the mesh, its normals and neighbours stay still until the dab is
 * applied. Mask, freeze, symmetry, radial copies, painting, undo and the viewport update are handled around it.
 */
struct BrushDef
{
    std::string label;
    /** An icon name from Lila's icon set. */
    std::string icon;
    std::string doc;
    std::function<glm::dvec3(const BrushArea&, const BrushSample&)> displace;
};

/** Registered brushes by name. Built-in names are reserved. Main thread only. */
LILA_SCENE_API Registry<BrushDef>& brushRegistry();
/** Owned by ctx and removed when it unloads. 0, with a log message, when the name is taken or displace is empty. */
LILA_SCENE_API ext::Context::Handle registerBrush(ext::Context& ctx, std::string name, BrushDef def);

/**
 * One brush stamp: the shared brush dab. Sculpting reads center, normal, radius, opacity times flow (the amount),
 * viewDir, delta (grab), direction (clay strips' square footprint) and the tip shape.
 */
using Dab = brush::Dab;

/** What a stroke or mask operation changed, enough to undo and redo it. */
struct StrokeRecord
{
    std::vector<Index> points;
    std::vector<glm::dvec3> before, after;
    std::vector<float> maskBefore, maskAfter;
    std::vector<uint8_t> frozenBefore, frozenAfter;
    /** Linear RGBA per point, empty when the mesh had no polypaint yet. */
    std::vector<glm::vec4> colorBefore, colorAfter;
    /**
     * Where the viewport tumbled around before and after the stroke (world space), when the stroke moved it; undo and
     * redo hand them to sculpt::setPivotHandler.
     */
    bool pivotMoved = false;
    std::optional<glm::dvec3> pivotBefore, pivotAfter;

    bool empty() const { return points.empty(); }
};

enum class MaskOp : uint8_t
{
    Clear,
    Fill,
    Invert,
    Blur,
};

enum class Layer : uint8_t
{
    Mask,
    Freeze,
};

/**
 * A mesh being sculpted. Topology never changes; positions, the soft mask (1 = fully masked), the freeze flags (frozen
 * points never move) and the polypaint colors do. Strokes run in parallel and are deterministic for the same dabs.
 */
class LILA_SCENE_API SculptMesh
{
public:
    SculptMesh() = default;
    explicit SculptMesh(geo::Mesh mesh);

    const geo::Mesh& mesh() const { return mMesh; }
    std::span<const glm::vec3> normals() const { return mNormals; }
    std::span<const float> mask() const { return mMask; }
    std::span<const uint8_t> frozen() const { return mFrozen; }
    /** Polypaint, linear RGBA per point; empty until something is painted or loaded. */
    std::span<const glm::vec4> colors() const { return mColor; }
    /** True when any point's color is not white. */
    bool painted() const;
    const geo::MeshBvh& bvh() const { return mBvh; }
    std::span<const Index> neighbours(Index p) const
    {
        return {mAdj.data() + mAdjOffsets[p], mAdj.data() + mAdjOffsets[p + 1]};
    }

    /** Mirror strokes across the plane, matching points within tolerance. nullopt turns symmetry off. */
    void setSymmetry(const std::optional<geo::SymmetryPlane>& plane, double tolerance);
    /**
     * Repeat strokes through the mesh's symmetry, mirror or radial, matching points within tolerance. With radial
     * symmetry the brush's own radial copies are left out: the mesh's orbits repeat every stroke exactly.
     */
    void setSymmetry(const std::optional<geo::SymmetrySpec>& spec, double tolerance);
    bool symmetric() const { return !mSymmetry.empty(); }
    const geo::SymmetryMap& symmetry() const { return mSymmetry; }

    bool raycast(const glm::dvec3& origin, const glm::dvec3& dir, geo::BvhHit& hit) const;
    /** Smooth normal at a hit, blended from the point normals. */
    glm::dvec3 normalAt(const geo::BvhHit& hit) const;

    void beginStroke(const BrushSettings& settings);
    void dab(const Dab& d);
    /** Finishes the stroke; the record holds every point it changed. */
    StrokeRecord endStroke();
    bool stroking() const { return mStroking; }

    /** Puts back the record's before (undo) or after (redo) state. */
    void apply(const StrokeRecord& r, bool undo);
    StrokeRecord maskOp(MaskOp op, Layer layer = Layer::Mask, int iterations = 1);
    void setPositions(std::span<const glm::dvec3> positions);
    void setMask(std::span<const float> mask);
    void setColors(std::span<const glm::vec4> colors);
    /** Fills every unmasked point with color (weighted by 1 - mask), or clears the paint back to white. */
    StrokeRecord fillColor(const glm::vec3& color);
    StrokeRecord clearColor();

    /** One vertex per point, smooth shaded, edited in place as the mesh changes. */
    std::shared_ptr<const render::RenderMesh> renderMesh();

    /** Changes whenever positions do. */
    uint64_t version() const { return mVersion; }
    /** Changes whenever the mask or freeze layers do. */
    uint64_t maskVersion() const { return mMaskVersion; }
    /** Changes whenever the polypaint does. */
    uint64_t colorVersion() const { return mColorVersion; }

private:
    struct Side
    {
        Dab dab;
        std::vector<Index> points;
        std::vector<double> weights; // falloff only
        glm::dvec3 areaNormal{0.0};
        glm::dvec3 areaCenter{0.0};
        /** Held brushes: the cursor's travel since the stroke began, where Rotate's angle starts, and Pose's joint. */
        glm::dvec3 travel{0.0};
        glm::dvec3 rotateFrom{0.0};
        glm::dvec3 pivot{0.0};
    };

    void gather(Side& s, bool forGrab);
    /** The held brushes' region and weights, gathered once at the stroke's first dab. */
    void gatherHeld(Side& s);
    void addHeldDisplacement(Side& s, const glm::dvec3& delta, double amount);
    void computeArea(Side& s) const;
    void touch(Index p);
    void addDisplacement(const Side& s);
    void addMaskChange(const Side& s);
    void addPaint(const Side& s);
    /** Each point the dab's copies overlap on takes their moves averaged by falloff, so the copies blend instead of stacking. */
    void blendSides();
    bool painting() const;
    void ensureColor();
    StrokeRecord colorChange(const std::vector<glm::vec4>& next);
    void symmetrizeChanges();
    void commitChanges();
    void refresh(std::span<const Index> moved);
    void updateNormals(std::span<const Index> points);
    void writeVertices(std::span<const Index> points);
    /** The vertex colour drawn: the polypaint, darkened where masked and lightened where frozen. */
    glm::vec4 shownColor(Index p) const;
    glm::dvec3 originalPosition(Index p) const;
    glm::dvec3 projectToOriginal(Index p, const glm::dvec3& at) const;
    /** Neighbour average less the point: zero on a flat, even patch. */
    glm::dvec3 laplacian(Index p) const;
    /**
     * Smooth's full-strength move: the bi-Laplacian, which removes noise but leaves an even curvature (a sphere, a
     * cylinder, a rounded edge) where it is, so forms keep their volume and silhouette.
     */
    glm::dvec3 smoothStep(Index p) const;
    double pointWeight(Index p, double falloff) const;

    geo::Mesh mMesh;
    geo::MeshBvh mBvh;
    std::vector<glm::vec3> mNormals;
    std::vector<float> mMask;
    std::vector<uint8_t> mFrozen;
    std::vector<uint32_t> mAdjOffsets;
    std::vector<Index> mAdj;
    std::vector<uint32_t> mTriOffsets;
    std::vector<Index> mTris; // triangles around each point
    geo::SymmetryMap mSymmetry;

    BrushSettings mSettings;
    /** The registered brush's displace for the stroke, copied so unregistering mid-stroke is safe. */
    std::function<glm::dvec3(const BrushArea&, const BrushSample&)> mCustom;
    brush::FalloffTable mFalloffTable;
    bool mStroking = false;
    std::vector<Side> mGrab;
    /** Planar: each side's plane (centre, normal), set by the stroke's first dab. */
    std::vector<std::pair<glm::dvec3, glm::dvec3>> mPlanes;
    // Per stroke: the state before a point was first touched.
    std::vector<uint32_t> mStrokeStamp;
    std::vector<uint32_t> mRecordIndex;
    uint32_t mStroke = 0;
    StrokeRecord mRecord;

    // Per dab: pending changes.
    std::vector<uint32_t> mDabStamp;
    uint32_t mDabId = 0;
    std::vector<Index> mChanged;
    std::vector<glm::dvec3> mDisp;
    std::vector<float> mMaskDelta;
    std::vector<int8_t> mFreezeSet; // -1 clear, 1 set, 0 untouched
    std::vector<float> mPaint;      // how far toward the brush color, 0 to 1
    std::vector<double> mBlend;     // summed falloff of the copies that moved each point
    std::vector<glm::vec4> mColor;

    std::vector<uint32_t> mGatherStamp;
    uint32_t mGatherId = 0;

    std::shared_ptr<render::RenderMesh> mRender;
    /** The render vertices of each point (one per uv seam side), [mPointVertOffsets[p], mPointVertOffsets[p + 1]). */
    std::vector<uint32_t> mPointVertOffsets;
    std::vector<uint32_t> mPointVerts;
    std::vector<glm::vec2> mVertUv;
    uint64_t mVersion = 1;
    uint64_t mMaskVersion = 1;
    uint64_t mColorVersion = 1;
};
} // namespace lila::sculpt
