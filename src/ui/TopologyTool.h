#pragma once

#include "core/Export.h"

#include "geo/Bvh.h"
#include "geo/Symmetry.h"
#include "retopo/Handles.h"
#include "retopo/LiveRelax.h"
#include "retopo/SurfaceStroke.h"
#include "retopo/TopologyCommands.h"
#include "scene/Plug.h"
#include "ui/ViewportTool.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QObject;
class QTimer;

namespace lila::ui
{
/** What the Topology tool's left mouse button does. */
enum class TopoMode : uint8_t
{
    Pencil,
    Patch,
    Strip,
    Stroke,
    Cylinder,
    Region,
    Handles,
    Relax,
    Cap,
    Count
};

LILA_UI_API const char* topoModeLabel(TopoMode m);
/** The action that switches to the mode, e.g. "topo.patch". */
LILA_UI_API const char* topoModeAction(TopoMode m);

/**
 * The Topology tool: hand retopology over a live reference surface, one mode at a time (the toolkit and the Topology
 * menu switch modes). Shared in every mode:
 *
 *   Shift drag           live relax brush (Shift wheel resizes the brush)
 *   Shift click gap      fill it with a face between the retopo vertices around the cursor
 *   Ctrl Shift click     delete (Ctrl Alt click too) the face, vertex, guide, knot or stroke under the cursor
 *   Ctrl / Ctrl Alt wheel  over a live construct: its main / second count (sides, divisions, rows, points around);
 *                        the plain wheel always zooms
 *   Enter                freeze the active construct (and finish a cylinder); Esc cancels, then leaves
 *
 * Patches, strips, strokes, bridges, sweeps, cylinders and auto regions stay live (retopoConstruct records on the retopo
 * mesh) until their faces are edited by hand. Every edit runs as a command, so it undoes and echoes like any other.
 */
class LILA_UI_API TopologyTool : public ViewportTool
{
public:
    TopologyTool(scene::NodeId reference, std::string retopoName, TopoMode mode);
    ~TopologyTool() override;
    const char* name() const override { return "Topology"; }

    void activate() override;
    void deactivate() override;
    bool mousePress(QMouseEvent* e, const ViewInfo& view) override;
    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override;
    bool mouseRelease(QMouseEvent* e, const ViewInfo& view) override;
    bool wheel(QWheelEvent* e, const ViewInfo& view) override;
    bool keyPress(QKeyEvent* e, const ViewInfo& view) override;
    bool hotkey(const QKeyEvent& e, std::string_view action) override;
    void draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view) override;
    std::optional<QCursor> cursor() const override;
    bool ownsBrushRadius() const override { return true; }

    TopoMode mode() const { return mMode; }
    void setMode(TopoMode m);
    scene::NodeId reference() const { return mReference; }
    const std::string& retopoName() const { return mRetopoName; }
    /** The construct the toolkit's settings and Enter act on; 0 for none. */
    int activeConstruct() const { return mActive; }
    void setActiveConstruct(int id);
    /** Edits the construct's spec (world space) and regenerates it. False when it isn't live or the command failed. */
    bool editConstruct(int id, const std::function<void(nlohmann::json& spec)>& edit);
    /** Stops adding rings to the cylinder being built. */
    void finishCylinder() { mCylinder = 0; }
    /** Retopologises the marked region of the reference; false when nothing is marked or it failed. */
    bool runRegion();
    void clearRegion();
    size_t regionFaceCount() const;
    void clearHandles();

    /** Brush radius on screen, pixels (strip width, region brush, relax brush, handle reach). */
    static double brushRadius();
    static void setBrushRadius(double px);
    /** Edge length new constructs aim for, cm; 0 takes the retopo mesh's mean edge length (or the brush). */
    static double edgeLength();
    static void setEdgeLength(double cm);
    /** Pencil: how far, in pixels, the line trails the cursor; 0 draws at the cursor. */
    static double pencilLazyRadius();
    /** Pencil: 0 keeps the stroke as drawn, 1 smooths it most once it is finished. */
    static double pencilSmoothing();
    static bool qualityOverlay();
    static void setQualityOverlay(bool on);
    static retopo::LiveRelaxOptions relaxOptions();

private:
    enum class Drag
    {
        None,
        Stroke,     // sketching a path for the mode
        Line,       // cylinder: a straight line across the reference
        Paint,      // region: marking reference faces
        Erase,      // region: unmarking; pencil: erasing guides
        RelaxBrush, // live relax under the brush
        RelaxPoint, // live relax with one point dragged
        Knot,       // strip: moving a knot
        Pull,       // patch: biasing its poles
        Handle,     // handles: moving one
        GuideMove,  // pencil: carrying a guide along the surface
        Delete,
        BrushSize,  // B-drag: the brush radius
    };

    struct Hit
    {
        geo::Index point = geo::kInvalid;
        geo::Index edge = geo::kInvalid;
        geo::Index face = geo::kInvalid;
        int construct = 0;
    };

    // Scene cache.
    void refresh();
    scene::Node* retopoNode() const;
    bool surfaceHit(QPointF px, const ViewInfo& view, glm::dvec3& out) const;
    glm::dvec3 onSurface(const glm::dvec3& p) const;
    glm::dvec3 normalAt(const glm::dvec3& p) const;
    bool visible(const glm::dvec3& p, const ViewInfo& view) const;
    glm::dvec3 pointWorld(geo::Index p) const;
    std::vector<std::optional<QPointF>> projectPoints(const ViewInfo& view) const;
    Hit pick(QPointF px, const ViewInfo& view) const;
    /** The world corners of the face a Shift-click at px would fill between retopo vertices; empty when there is none. */
    std::vector<glm::dvec3> findGapFill(QPointF px, const ViewInfo& view) const;
    /** A border point of the retopo mesh within pick reach of px. */
    geo::Index pickBorderPoint(QPointF px, const ViewInfo& view) const;
    geo::Index pickBorderEdge(QPointF px, const ViewInfo& view) const;
    const retopo::ConstructRecord* record(int id) const;
    double edgeLengthNear(const glm::dvec3& at, const ViewInfo& view) const;
    double brushWorld(const glm::dvec3& at, const ViewInfo& view) const;

    // Commands.
    bool run(const char* command, const nlohmann::json& call, nlohmann::json* result = nullptr);
    nlohmann::json targetCall(nlohmann::json flags) const;
    int createConstruct(retopo::ConstructKind kind, const nlohmann::json& spec, double edgeLen);
    bool updateConstruct(int id, retopo::ConstructKind kind, const nlohmann::json& spec);
    void freezeActive();
    void deleteAt(QPointF px, const ViewInfo& view);

    // Borders of the retopo mesh, world space.
    struct BorderLoop
    {
        std::vector<geo::Index> points;
        std::vector<glm::dvec3> world;
        bool closed = false;
    };
    BorderLoop borderThrough(geo::Index point) const;
    /** The part of the loop from a to b (point ids on it) that is shorter, a and b included, in order a..b. */
    std::vector<glm::dvec3> borderArc(const BorderLoop& loop, geo::Index a, geo::Index b) const;

    // Modes.
    void finishStroke(const ViewInfo& view, bool erase);
    void commitPencil(const retopo::SurfaceStroke& s, const ViewInfo& view);
    /** Laplacian smoothing kept on the reference; ends stay put on an open polyline. */
    std::vector<glm::dvec3> smoothOnSurface(std::vector<glm::dvec3> pts, bool closed, double amount) const;
    void commitPatch(const retopo::SurfaceStroke& s, geo::Index a, geo::Index b, const ViewInfo& view);
    void fillLoopAt(geo::Index edge, const ViewInfo& view);
    /** The way a cap over this border loop bulges: out of the faces behind it. Null when it can't tell. */
    std::optional<glm::dvec3> capDirection(const std::vector<geo::Index>& loop) const;
    void commitStrip(const retopo::SurfaceStroke& s, const ViewInfo& view);
    void commitStroke(const retopo::SurfaceStroke& s, geo::Index a, geo::Index b, const ViewInfo& view);
    void commitCylinderLine(QPointF from, QPointF to, const ViewInfo& view);
    void paintRegion(QPointF px, const ViewInfo& view, bool on);
    void floodRegionFrom(QPointF px, const ViewInfo& view);
    void beginHandles(const std::vector<glm::dvec3>& path, const ViewInfo& view);
    void previewHandles();
    void commitHandles();
    bool wheelConstruct(int id, int delta, bool second, QPointF px, const ViewInfo& view);
    /** The construct the wheel would change at px: a waiting stroke, else the construct under it; 0 for none. */
    int wheelTarget(QPointF px, const ViewInfo& view) const;
    int mWheelId = 0;
    double mWheelAt = -1e9;
    int pickKnot(QPointF px, const ViewInfo& view, int* construct) const;
    int pickHandle(QPointF px, const ViewInfo& view) const;
    int pickGuide(QPointF px, const ViewInfo& view) const;
    int pickPendingStroke(QPointF px, const ViewInfo& view) const;
    void previewConstruct(int id, const nlohmann::json& spec);

    // Live relax.
    void beginRelax(const ViewInfo& view);
    void relaxBrushAt(QPointF px, const ViewInfo& view);
    void tickRelax();
    void commitRelax();

    // Drawing.
    void removeOwnDraw(render::FrameState& s) const;
    void drawRetopo(render::FrameState& s, OverlayPainter& p, const ViewInfo& view, bool here) const;
    void drawQuality(OverlayPainter& p, const ViewInfo& view) const;
    void drawGuides(OverlayPainter& p, const ViewInfo& view) const;
    void drawConstructs(render::FrameState& s, OverlayPainter& p, const ViewInfo& view, bool here) const;
    void drawSurfacePolyline(OverlayPainter& p, const ViewInfo& view, const std::vector<glm::dvec3>& pts, bool closed, float width,
                             const glm::vec4& color, float dash = 0.0f) const;
    void drawBrush(OverlayPainter& p, double radius) const;
    /** Strip mode: the width before drawing, then the band and its loops while drawing. */
    void drawStripPreview(OverlayPainter& p, const ViewInfo& view) const;
    ToolStatus status() const;
    /** The keys for the corner list: the mode's when nothing is held, else what the held modifiers do here. */
    std::vector<KeyHint> contextKeys(std::vector<KeyHint> modeKeys, const ViewInfo& view) const;

    TopoMode mMode;
    scene::NodeId mReference = scene::kNoNode;
    std::string mRetopoName;
    scene::NodeId mRetopoId = scene::kNoNode;

    scene::MeshPtr mRefMesh;
    glm::dmat4 mRefWorld{1.0};
    geo::Mesh mRefWorldMesh; // compacted, world space; face ids match what retopoConstruct's region takes
    geo::MeshBvh mRefBvh;
    std::vector<glm::dvec3> mRefFaceNormals;
    scene::MeshPtr mRetopo;
    glm::dmat4 mRetopoWorld{1.0};
    std::optional<geo::SymmetryPlane> mSymmetry; // world space
    std::vector<retopo::Guide> mGuides;
    std::string mGuidesText, mConstructsText;
    scene::MeshPtr mConstructsMesh;
    std::vector<retopo::ConstructRecord> mRecords;

    QPointF mCursor;
    std::string mCursorView;
    Qt::KeyboardModifiers mMods;
    Hit mHover;
    int mActive = 0;
    int mCylinder = 0; // the cylinder new lines add rings to

    Drag mDrag = Drag::None;
    QPointF mPressPx;
    double mBrushStart = 0.0;
    QPointF mLazyPx; // pencil: the point the line follows
    std::vector<glm::dvec3> mStroke; // surface hits while sketching
    std::vector<QPointF> mStrokePx;
    std::map<geo::Index, glm::dvec3> mMoved; // world preview positions of retopo points

    // Construct preview while dragging a knot or a pull: the generated faces in world space.
    int mPreviewId = 0;
    std::vector<glm::dvec3> mPreviewPos;
    std::vector<std::vector<geo::Index>> mPreviewFaces;
    nlohmann::json mPreviewSpec;
    int mDragConstruct = 0;
    int mDragKnot = -1;

    // Region marks, per face of mRefWorldMesh.
    std::vector<uint8_t> mRegion;

    // Handles, world space.
    retopo::HandleRig mRig;
    std::vector<retopo::HandleXform> mXforms;
    int mDragHandle = -1;
    /** The retopo mesh the handles were placed on; any other change to it (an undo) clears them. */
    scene::MeshPtr mRigMesh;
    /** Placed and not moved yet, so undo takes them away. */
    bool mRigFresh = false;

    // Live relax over the retopo mesh in world space.
    std::unique_ptr<geo::Mesh> mRelaxMesh;
    std::unique_ptr<retopo::LiveRelax> mRelax;
    geo::Index mRelaxPoint = geo::kInvalid;
    QPointF mRelaxPx;
    ViewInfo mRelaxView;
    std::unique_ptr<QTimer> mTimer;

    mutable std::string mRingKey; // single-ring cylinder previews, by spec
    mutable std::vector<glm::dvec3> mRing;

    int mDragGuide = -1;
    glm::dvec3 mGuideFrom{0.0};

    std::unique_ptr<QObject> mFilter;
};

/** A retopo mesh edge: a light line over a dark halo, so it reads over light and dark surfaces alike. bold for hover. */
LILA_UI_API void pushRetopoEdge(render::FrameState& s, const glm::vec3& a, const glm::vec3& b, const glm::vec4& color, bool bold = false);

/** Draws the polyline in runs, broken where visible(point) is false. */
LILA_UI_API void drawVisiblePolyline(OverlayPainter& p, const ViewInfo& view, const std::vector<glm::dvec3>& pts, bool closed, float width,
                                     const glm::vec4& color, float dash, const std::function<bool(const glm::dvec3&)>& visible);

/**
 * Draws a retopo mesh's guides (world space) and their symmetry images, so they read the same in every topology tool.
 * images(p) gives p's images under the mesh's symmetry, the identity left out. hot is the guide drawn thicker, -1 for none.
 */
LILA_UI_API void drawRetopoGuides(OverlayPainter& p, const ViewInfo& view, const std::vector<retopo::Guide>& guides,
                                  const std::function<std::vector<glm::dvec3>(const glm::dvec3&)>& images,
                                  const std::function<bool(const glm::dvec3&)>& visible, int hot = -1);

/** "Strip", "Patch"... for messages and readouts. */
LILA_UI_API const char* topoConstructLabel(retopo::ConstructKind k);

/** The running Topology tool, or null. */
LILA_UI_API TopologyTool* activeTopologyTool();

/** The topo.* actions, the Topology Toolkit's window action and the tool's prefs. */
LILA_UI_API void registerTopologyActions();
} // namespace lila::ui
