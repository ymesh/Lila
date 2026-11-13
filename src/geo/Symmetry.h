#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lila::geo
{
/** Points p with dot(normal, p) == offset. */
struct SymmetryPlane
{
    glm::dvec3 normal{1.0, 0.0, 0.0};
    double offset = 0.0;

    double distance(const glm::dvec3& p) const { return glm::dot(normal, p) - offset; }
    glm::dvec3 reflect(const glm::dvec3& p) const { return p - 2.0 * distance(p) * normal; }
    glm::dvec3 project(const glm::dvec3& p) const { return p - distance(p) * normal; }
};

/**
 * Plane normal to axis 0, 1 or 2 at offset along it (an object's pivot). Reflecting across it maps one coordinate v to
 * 2 * offset - v, which is exact at offset 0.
 */
LILA_SCENE_API SymmetryPlane axisPlane(int axis, double offset = 0.0);

/**
 * count-fold rotation about an axis through origin, and with mirror a mirror plane through the axis per wedge as well
 * (dihedral symmetry). Element g < count turns by g wedges; element count + k reflects across the reference plane (the
 * plane holding the axis and the reference direction) and then turns by k wedges. Element 0 is the identity.
 */
class LILA_SCENE_API RadialSymmetry
{
public:
    RadialSymmetry() = default;
    /** reference is made perpendicular to axis; a zero or parallel one picks a direction of its own. count is clamped to 1-kMaxRadialCount. */
    RadialSymmetry(const glm::dvec3& origin, const glm::dvec3& axis, int count, bool mirror, const glm::dvec3& reference = glm::dvec3(0.0));

    int count() const { return mCount; }
    bool mirror() const { return mMirror; }
    /** Number of group elements: count, or twice that with mirror. */
    int order() const { return mMirror ? 2 * mCount : mCount; }
    const glm::dvec3& origin() const { return mOrigin; }
    const glm::dvec3& axis() const { return mAxis; }
    const glm::dvec3& reference() const { return mReference; }
    /** 0, 1 or 2 when the axis is a coordinate axis, else -1. */
    int axisIndex() const { return mAxisIndex; }

    bool reflects(int g) const { return g >= mCount; }
    /** The element that applies b, then a. */
    int compose(int a, int b) const;
    int inverse(int g) const;

    const glm::dmat3& linear(int g) const { return mLinear[size_t(g)]; }
    glm::dvec3 apply(int g, const glm::dvec3& p) const;
    glm::dvec3 applyVector(int g, const glm::dvec3& v) const { return mLinear[size_t(g)] * v; }

    /** Radians about the axis from the reference direction, in [0, 2 pi). */
    double angle(const glm::dvec3& p) const;
    /** The angle a wedge spans: 2 pi / count, or pi / count with mirror (the fundamental wedge). */
    double wedgeAngle() const;
    /** The element that carries the fundamental wedge onto the one holding p. */
    int wedgeOf(const glm::dvec3& p) const;
    double axisDistance(const glm::dvec3& p) const;
    glm::dvec3 projectToAxis(const glm::dvec3& p) const;
    /** The plane a reflecting element mirrors across, through the axis. */
    SymmetryPlane mirrorPlane(int g) const;
    /**
     * The closest position every element in stabilizer leaves in place: the axis when it holds a rotation, else the one
     * mirror plane, else p itself. Exact for coordinate axes.
     */
    glm::dvec3 projectFixed(const glm::dvec3& p, std::span<const int> stabilizer) const;

    bool operator==(const RadialSymmetry& o) const
    {
        return mOrigin == o.mOrigin && mAxis == o.mAxis && mReference == o.mReference && mCount == o.mCount && mMirror == o.mMirror;
    }

private:
    glm::dvec3 mOrigin{0.0};
    glm::dvec3 mAxis{0.0, 1.0, 0.0};
    glm::dvec3 mReference{0.0, 0.0, 1.0};
    int mCount = 1;
    bool mMirror = false;
    int mAxisIndex = 1;
    std::vector<glm::dmat3> mLinear{glm::dmat3(1.0)};
};

/** Largest radial count: the persistent layer packs the element into 8 bits. */
inline constexpr int kMaxRadialCount = 64;

/** The direction perpendicular to axis turned by phase degrees about it from the next coordinate axis (z for y, x for z, y for x). */
LILA_SCENE_API glm::dvec3 radialReference(const glm::dvec3& axis, double phaseDegrees = 0.0);

/** Radial symmetry about coordinate axis 0-2 through origin, wedge 0 starting phase degrees past the next axis. */
LILA_SCENE_API RadialSymmetry axisRadial(int axis, const glm::dvec3& origin, int count, bool mirror, double phaseDegrees = 0.0);

/**
 * Point int layer that keeps the symmetry pairs for an axis on the mesh, so they survive compaction, saving, undo and
 * any amount of position drift. Partners hold +k and -k (k > 0, the sign is the side), on-plane points hold
 * kSymmetryOnPlane, 0 is unpaired.
 */
LILA_SCENE_API std::string symmetryLayerName(int axis);
inline constexpr int32_t kSymmetryOnPlane = INT32_MIN;

/**
 * Point int layer that keeps radial orbits on the mesh: orbit * 256 + slot + 1, kSymmetryOnPlane on the axis, 0
 * unmatched. Named for the group, its axis and its reference, so changing any of them starts afresh.
 */
LILA_SCENE_API std::string radialLayerName(const RadialSymmetry& radial);
/** True for the name of any symmetry layer, mirror or radial. */
LILA_SCENE_API bool isSymmetryLayer(const std::string& name);
/** Drops every stored symmetry layer, mirror and radial. */
LILA_SCENE_API void removeSymmetryLayers(Mesh& mesh);

/** Zeroes every stored symmetry key from firstPoint on: what an op copied onto the points it created means nothing. */
LILA_SCENE_API void clearSymmetryKeys(Mesh& mesh, Index firstPoint);

/** A mesh's symmetry setting: a mirror plane across a coordinate axis, or a radial group. Element 0 is the identity. */
struct LILA_SCENE_API SymmetrySpec
{
    int axis = 0;
    double offset = 0.0;
    std::optional<RadialSymmetry> radial;

    bool isRadial() const { return radial.has_value(); }
    /** Number of elements: 2 for a mirror (element 1 reflects). */
    int order() const { return radial ? radial->order() : 2; }
    bool reflects(int g) const { return radial ? radial->reflects(g) : g == 1; }
    SymmetryPlane plane() const { return axisPlane(axis, offset); }
    glm::dvec3 apply(int g, const glm::dvec3& p) const;
    glm::dvec3 applyVector(int g, const glm::dvec3& v) const;
    /** p moved onto the plane, or the axis or a mirror plane, when it lies within tolerance of it; else p. */
    glm::dvec3 snap(const glm::dvec3& p, double tolerance) const;
};

/** How SymmetryMap::persistent resolved the points. */
struct SymmetrySync
{
    Index stored = 0;      // paired or on-plane from the stored layer
    Index topological = 0; // had no stored partner, matched through their paired neighbours
    Index byPosition = 0;  // had no stored partner, matched by position within tolerance
    Index unmatched = 0;
};

struct SymmetryCheck
{
    /**
     * Largest distance between a point and the reflection of its partner, of an on-plane point from the plane, or of an
     * unmatched point's reflection from the surface.
     */
    double maxAsymmetry = 0.0;
    Index worstPoint = kInvalid;
    Index pairs = 0;   // radial: orbits that are not fixed
    Index onPlane = 0; // radial: points on the axis or a mirror plane
    Index unmatched = 0;
};

/**
 * Symmetry of a mesh's points: the mirror partner of every point across a plane, or for radial symmetry each point's
 * orbit, its image under every element of the group. A mirror is the order 2 case (element 1 reflects), so the generic
 * calls (order, image, wedge, enforceWedge...) serve both. Points on the plane (or axis) map to themselves. Ids refer
 * to the mesh the map was built from; rebuild after topology changes.
 */
class LILA_SCENE_API SymmetryMap
{
public:
    /** Matches each point to the nearest point at its reflection, within tolerance. */
    static SymmetryMap build(const Mesh& mesh, const SymmetryPlane& plane = {}, double tolerance = 1e-3);
    /** Matches each point's image under every element to the nearest point there, within tolerance. */
    static SymmetryMap buildRadial(const Mesh& mesh, const RadialSymmetry& radial, double tolerance = 1e-3);

    /**
     * Matches by topology, walking outward from seedEdge, which must lie on the plane with two faces.
     * Works on meshes whose positions are no longer symmetric. Empty map if the topology is not symmetric.
     */
    static SymmetryMap buildTopological(const Mesh& mesh, Index seedEdge, const SymmetryPlane& plane = {});
    /**
     * Matches by topology, walking outward from seedEdge (anywhere with a face) and the faces its rotated (and
     * reflected) copies land nearest to. Works while positions have drifted by less than a wedge. Empty map if the
     * topology is not symmetric.
     */
    static SymmetryMap buildRadialTopological(const Mesh& mesh, Index seedEdge, const RadialSymmetry& radial);

    /**
     * Map from the pairs stored on the mesh for an axis plane. Points without a valid stored partner are matched among
     * themselves, first through their paired neighbours, then by position within tolerance, and the new pairs are
     * stored back. A mesh without the layer gets it built here.
     */
    static SymmetryMap persistent(Mesh& mesh, int axis, double tolerance, SymmetrySync* sync = nullptr, double offset = 0.0);
    /** The same for radial symmetry, from the orbits stored in its layer. */
    static SymmetryMap persistent(Mesh& mesh, const RadialSymmetry& radial, double tolerance, SymmetrySync* sync = nullptr);
    /** The mirror or radial persistent map the spec describes. */
    static SymmetryMap persistent(Mesh& mesh, const SymmetrySpec& spec, double tolerance, SymmetrySync* sync = nullptr);
    /** build() or buildRadial() as the spec describes, by position. */
    static SymmetryMap buildFor(const Mesh& mesh, const SymmetrySpec& spec, double tolerance = 1e-3);

    /** Writes this map into the mesh's layer for axis, replacing what was there. */
    void store(Mesh& mesh, int axis) const;
    /** Writes this map into its own layer: the axis plane's for a mirror, the radial one for radial symmetry. */
    void store(Mesh& mesh) const;

    /**
     * Sets each given point's follower to the exact reflection of its partner on the driving side (drive > 0: the
     * positive side), and on-plane points exactly onto the plane. Radial: drive > 0 drives from wedge 0, else wedge 1.
     */
    void enforce(Mesh& mesh, std::span<const Index> points, int drive) const;
    /** enforce() over every point. */
    void symmetrize(Mesh& mesh, int drive) const;
    SymmetryCheck check(const Mesh& mesh) const;

    /** +1 or -1 for the side a paired point belongs to, 0 on the plane or unpaired. Radial: +1 in wedge 0, -1 in the others. */
    int side(Index p) const;
    /** The side most of the points are on, +1 when tied. */
    int majoritySide(std::span<const Index> points) const;

    const SymmetryPlane& plane() const { return mPlane; }
    /** 0, 1 or 2 for a plane normal to an axis, -1 for any other plane (and radial symmetry). */
    int axis() const { return mAxis; }
    double tolerance() const { return mTolerance; }
    bool empty() const { return mRadial ? mOrbit.empty() : mMirror.empty(); }

    /** The partner across the plane; for radial symmetry the image under element 1 (one wedge on). */
    Index mirrorPoint(Index p) const { return mRadial ? image(1, p) : p < mMirror.size() ? mMirror[p] : kInvalid; }
    /** On the plane; for radial symmetry fixed() (on the axis or a dihedral mirror plane). */
    bool onPlane(Index p) const { return mRadial ? fixed(p) : p < mOnPlane.size() && mOnPlane[p]; }
    /** Points without a partner. */
    Index unmatchedCount() const { return mUnmatched; }

    Index mirrorEdge(const Mesh& mesh, Index e) const;
    Index mirrorFace(const Mesh& mesh, Index f) const;

    /** The selection plus its mirror (every image, for radial symmetry), sorted and unique. */
    std::vector<Index> expandPoints(std::span<const Index> points) const;
    std::vector<Index> expandEdges(const Mesh& mesh, std::span<const Index> edges) const;
    std::vector<Index> expandFaces(const Mesh& mesh, std::span<const Index> faces) const;

    /** Just the mirror images (element 1's, for radial symmetry), in input order; kInvalid where there is none. */
    std::vector<Index> mirrorPoints(std::span<const Index> points) const;
    std::vector<Index> mirrorEdges(const Mesh& mesh, std::span<const Index> edges) const;
    std::vector<Index> mirrorFaces(const Mesh& mesh, std::span<const Index> faces) const;

    /** Snaps the given points (or all points of the mesh from firstPoint on) within tolerance onto the plane (radial: the axis or a mirror plane). */
    void snapToPlane(Mesh& mesh, std::span<const Index> points) const;
    void snapNewPoints(Mesh& mesh, Index firstPoint) const;

    // Generic over mirror and radial symmetry.

    bool isRadial() const { return mRadial.has_value(); }
    /** The radial group; only meaningful when isRadial(). */
    const RadialSymmetry& radial() const;
    /** Number of group elements: 2 for a mirror. */
    int order() const;
    /** p's image under element g: g 0 is p, for a mirror g 1 its partner. kInvalid when unmatched. */
    Index image(int g, Index p) const;
    /** The member of p's orbit in slot g: g's image of the orbit's point in wedge 0 (a mirror's positive side). */
    Index orbitPoint(Index p, int g) const;
    Index imageEdge(const Mesh& mesh, int g, Index e) const;
    Index imageFace(const Mesh& mesh, int g, Index f) const;
    /** Position and direction images under element g. */
    glm::dvec3 transform(int g, const glm::dvec3& p) const;
    glm::dvec3 transformVector(int g, const glm::dvec3& v) const;
    /** True when element g reverses orientation (a mirror's element 1, a dihedral group's reflections). */
    bool reflects(int g) const;
    /** The element that undoes g. */
    int inverse(int g) const;
    /** The element applying b, then a. */
    int compose(int a, int b) const;
    /** The slot a matched point fills (a mirror's positive side 0, negative 1); -1 for fixed or unmatched points. */
    int wedge(Index p) const;
    /** The wedge most of the points are in, 0 when tied or none are. */
    int majorityWedge(std::span<const Index> points) const;
    /**
     * The points split by the wedge (side) they lie in, unmatched ones with the majority's, so a symmetric selection merges
     * per side. One group of them all when any is fixed: a selection reaching the plane merges onto it.
     */
    std::vector<std::vector<Index>> wedgeGroups(std::span<const Index> points) const;
    /** True when p is its orbit's point in wedge w, so it drives the rest (fixed and unmatched points always do). */
    bool drives(Index p, int w) const;
    /** On the plane, the axis, or a dihedral mirror plane: some element besides the identity leaves it in place. */
    bool fixed(Index p) const;
    /** pos moved onto the set p must stay on (its plane or axis); pos unchanged for points that are not fixed. */
    glm::dvec3 constrain(Index p, const glm::dvec3& pos) const;
    /** Sets each given point's orbit to the exact images of its member in wedge w, fixed points onto their plane or axis. */
    void enforceWedge(Mesh& mesh, std::span<const Index> points, int w) const;
    /** enforceWedge() over every point. */
    void symmetrizeWedge(Mesh& mesh, int w) const;
    /** Valid points without a partner or orbit, for highlighting. */
    std::vector<Index> unmatchedPoints(const Mesh& mesh) const;
    /**
     * Replaces every orbit's values (per point id) by their symmetric average: each member's value carried back to slot 0
     * by its element, averaged, and carried out again. positions: points (fixed ones also land on their plane or axis);
     * else directions, such as displacements.
     */
    void averageOrbits(std::span<glm::dvec3> values, bool positions) const;

private:
    glm::dvec3 reflected(const glm::dvec3& p) const;
    /** The elements that leave an orbit's slot 0 point in place, identity included. */
    std::vector<int> stabilizer(Index orbit) const;
    /** Adds an orbit from the points at p's image under every element (images[g] for g); false if it clashes. */
    bool addOrbit(const Mesh& mesh, std::span<const Index> images, int repElement);
    void countUnmatched(const Mesh& mesh);
    /** The slot p's key stores: its own, except an orbit's slot 0 point on a mirror plane, which stores its reflection. */
    int storedSlot(Index p) const;

    SymmetryPlane mPlane;
    int mAxis = -1;
    double mTolerance = 1e-3;
    std::vector<Index> mMirror;
    std::vector<uint8_t> mOnPlane;
    std::vector<int8_t> mSide;
    Index mUnmatched = 0;

    // Radial: per point its orbit and slot (kInvalid and -1 when unmatched), per orbit its point in every slot.
    std::optional<RadialSymmetry> mRadial;
    std::vector<Index> mOrbit;
    std::vector<int16_t> mSlot;
    std::vector<Index> mOrbitPoints;
    std::vector<uint8_t> mFixed;
};

/** Runs after an op: snaps new points and the old on-plane points it moved back onto the plane (radial: the axis or a mirror plane). */
LILA_SCENE_API void applySymmetrySnap(Mesh& mesh, const SymmetryMap* symmetry, Index firstNewPoint);
} // namespace lila::geo
