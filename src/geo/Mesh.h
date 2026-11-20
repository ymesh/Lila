#pragma once

#include "core/Export.h"
#include "core/UninitVector.h"

#include "geo/Attribute.h"

#include <glm/glm.hpp>

#include <array>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace lila::geo
{
struct Bounds
{
    glm::dvec3 min{std::numeric_limits<double>::max()};
    glm::dvec3 max{-std::numeric_limits<double>::max()};

    bool valid() const { return min.x <= max.x; }
    glm::dvec3 center() const { return (min + max) * 0.5; }
    glm::dvec3 size() const { return valid() ? max - min : glm::dvec3(0.0); }
    void extend(const glm::dvec3& p)
    {
        min = glm::min(min, p);
        max = glm::max(max, p);
    }
    void extend(const Bounds& b)
    {
        min = glm::min(min, b.min);
        max = glm::max(max, b.max);
    }
};

/** Color set state that is not per element (geo/ColorSets.h): the set displayed and painted, and the sets kept at half precision. */
struct ColorSetInfo
{
    std::string current;
    std::vector<std::string> half;
    /** Sets that carry data for export (masks, wind), never drawn as colours in the viewport. */
    std::vector<std::string> data;
    bool operator==(const ColorSetInfo&) const = default;
};

/** Old-to-new id maps produced by Mesh::compact(). kInvalid marks removed elements. */
struct MeshRemap
{
    std::vector<Index> points;
    std::vector<Index> edges;
    std::vector<Index> faces;
    std::vector<Index> corners;
};

enum ValidateFlags : uint32_t
{
    kValidateStructure = 1 << 0,  // broken links; always checked
    kValidateManifold = 1 << 1,   // edges with more than two faces
    kValidateWinding = 1 << 2,    // neighbouring faces with opposite orientation
    kValidateLoose = 1 << 3,      // edges without faces, points without edges
    kValidateDegenerate = 1 << 4, // zero-area faces, zero-length edges
    kValidateDefault = kValidateStructure | kValidateManifold | kValidateWinding | kValidateLoose,
    kValidateAll = 0xffffffff,
};

/**
 * Polygon mesh with BMesh-style topology: every point has a disk cycle of edges, every edge a radial
 * cycle of face corners, every face a cyclic list of corners. Non-manifold edges and points are
 * representable. Corner c of a face runs from cornerPoint(c) to cornerPoint(cornerNext(c)) along
 * cornerEdge(c).
 *
 * Removing elements only marks them deleted, so ids of everything else stay stable until compact().
 * Iterate with `for (Index f = 0; f < faceCapacity(); ++f) if (faceValid(f))`.
 */
class LILA_SCENE_API Mesh
{
public:
    Index pointCapacity() const { return Index(mPointPos.size()); }
    Index edgeCapacity() const { return Index(mEdges.size()); }
    Index faceCapacity() const { return Index(mFaces.size()); }
    Index cornerCapacity() const { return Index(mCorners.size()); }

    Index numPoints() const { return mNumPoints; }
    Index numEdges() const { return mNumEdges; }
    Index numFaces() const { return mNumFaces; }
    Index numCorners() const { return mNumCorners; }
    bool hasDeleted() const;
    /**
     * Same for every copy of this mesh until one of them changes its points, edges, faces or corners (moving points
     * keeps it), so data derived from topology alone can be reused across copies. Unique per process.
     */
    uint64_t topologyId() const { return mTopologyId; }
    /** Approximate heap bytes held: element arrays and attribute layers. */
    size_t memoryBytes() const;
    bool empty() const { return mNumPoints == 0; }

    bool pointValid(Index p) const { return p < mPointDead.size() && !mPointDead[p]; }
    bool edgeValid(Index e) const { return e < mEdges.size() && mEdges[e].v[0] != kInvalid; }
    bool faceValid(Index f) const { return f < mFaces.size() && mFaces[f].corner != kInvalid; }
    bool cornerValid(Index c) const { return c < mCorners.size() && mCorners[c].face != kInvalid; }

    // Points
    const glm::dvec3& position(Index p) const { return mPointPos[p]; }
    void setPosition(Index p, const glm::dvec3& pos) { mPointPos[p] = pos; }
    std::span<glm::dvec3> positions() { return mPointPos; }
    std::span<const glm::dvec3> positions() const { return mPointPos; }
    /** Any edge of the point's disk cycle, kInvalid for a loose point. */
    Index pointEdge(Index p) const { return mPointEdge[p]; }

    // Edges
    Index edgePoint(Index e, int i) const { return mEdges[e].v[i]; }
    Index edgeOtherPoint(Index e, Index p) const { return mEdges[e].v[0] == p ? mEdges[e].v[1] : mEdges[e].v[0]; }
    /** Any corner of the edge's radial cycle, kInvalid for a loose edge. */
    Index edgeCorner(Index e) const { return mEdges[e].corner; }
    /** Next edge in the disk cycle around p, which must be an endpoint of e. */
    Index edgeNextAround(Index e, Index p) const { return mEdges[e].next[side(e, p)]; }

    // Corners
    Index cornerPoint(Index c) const { return mCorners[c].point; }
    Index cornerEdge(Index c) const { return mCorners[c].edge; }
    Index cornerFace(Index c) const { return mCorners[c].face; }
    Index cornerNext(Index c) const { return mCorners[c].next; }
    Index cornerPrev(Index c) const { return mCorners[c].prev; }
    /** Next corner on the same edge, in another face (or itself on a boundary edge). */
    Index cornerRadial(Index c) const { return mCorners[c].radialNext; }
    Index cornerNextPoint(Index c) const { return mCorners[mCorners[c].next].point; }

    // Faces
    Index faceCorner(Index f) const { return mFaces[f].corner; }
    Index faceSize(Index f) const { return mFaces[f].size; }

    // Queries
    Index findEdge(Index a, Index b) const;
    /** The corner of face f at point p, kInvalid if p is not on f. */
    Index findCorner(Index f, Index p) const;
    Index edgeFaceCount(Index e) const;
    bool isBoundaryEdge(Index e) const;
    bool isManifoldEdge(Index e) const;
    bool isBoundaryPoint(Index p) const;
    Index pointValence(Index p) const;
    /** The face across edge e from face f when e has exactly two faces. */
    Index oppositeFace(Index e, Index f) const;
    double edgeLength(Index e) const;
    glm::dvec3 faceCentroid(Index f) const;
    /** Newell normal: unit length, kept stable for concave and non-planar faces. */
    glm::dvec3 faceNormal(Index f) const;
    /** Newell vector, twice the projected area in length. */
    glm::dvec3 faceAreaVector(Index f) const;
    double faceArea(Index f) const;
    std::vector<Index> facePoints(Index f) const;
    std::vector<Index> faceCorners(Index f) const;
    std::vector<Index> pointFaces(Index p) const;
    Bounds bounds() const;
    Bounds faceBounds(std::span<const Index> faces) const;

    template <class F> void forEachFaceCorner(Index f, F&& fn) const
    {
        const Index first = mFaces[f].corner;
        Index c = first;
        do
        {
            fn(c);
            c = mCorners[c].next;
        } while (c != first);
    }

    template <class F> void forEachPointEdge(Index p, F&& fn) const
    {
        const Index first = mPointEdge[p];
        if (first == kInvalid)
            return;
        Index e = first;
        do
        {
            const Index next = mEdges[e].next[side(e, p)];
            fn(e);
            e = next;
        } while (e != first);
    }

    template <class F> void forEachEdgeCorner(Index e, F&& fn) const
    {
        const Index first = mEdges[e].corner;
        if (first == kInvalid)
            return;
        Index c = first;
        do
        {
            const Index next = mCorners[c].radialNext;
            fn(c);
            c = next;
        } while (c != first);
    }

    /** Every corner whose point is p, each once. */
    template <class F> void forEachPointCorner(Index p, F&& fn) const
    {
        forEachPointEdge(p, [&](Index e) {
            forEachEdgeCorner(e, [&](Index c) {
                if (mCorners[c].point == p)
                    fn(c);
            });
        });
    }

    // Editing. New elements get default attributes unless a source is given.
    Index addPoint(const glm::dvec3& pos);
    /** Returns the existing edge between a and b if there is one. */
    Index addEdge(Index a, Index b);
    /** kInvalid if fewer than 3 points or a point repeats. */
    Index addFace(std::span<const Index> points);
    /**
     * Same, copying face attributes from srcFace and corner attributes from srcCorners[i] into corner i
     * (either may be kInvalid; srcCorners may be empty). Sources may be deleted elements.
     */
    Index addFace(std::span<const Index> points, Index srcFace, std::span<const Index> srcCorners);
    Index addFace(std::initializer_list<Index> points) { return addFace(std::span<const Index>(points.begin(), points.size())); }
    /** Edges and points stay, possibly loose. */
    void removeFace(Index f);
    /** Removes the edge and its faces. */
    void removeEdge(Index e);
    /** Removes the point, its edges and their faces. */
    void removePoint(Index p);
    /** Removes edges without faces, then points without edges, among the candidates. */
    void removeLoose(std::span<const Index> edges, std::span<const Index> points);
    void removeAllLoose();

    /**
     * Splits e at t (0 at edgePoint(e,0)) with a new point, returned. e keeps edgePoint(e,0); the new
     * edge runs from the new point to the old edgePoint(e,1) and copies e's attributes.
     */
    Index splitEdge(Index e, double t);
    /** Edge created by the last splitEdge call. */
    Index lastSplitEdge() const { return mLastSplitEdge; }
    /**
     * Cuts face f between the points of its corners a and b, which must not be neighbours. f keeps
     * a..b, the returned new face gets b..a. The new edge is lastSplitEdge().
     */
    Index splitFace(Index f, Index cornerA, Index cornerB);
    /** Removes e and merges its two faces into one, returned. kInvalid if that is not possible. */
    Index joinFaces(Index e);
    /** Reverses the winding of f, keeping corner attributes on their points. */
    void flipFace(Index f);
    /**
     * Moves the corners of f onto new points (same count, in order), keeping face and corner ids and
     * their attributes. Edges left without faces stay, possibly loose. False if the points are invalid.
     */
    bool setFacePoints(Index f, std::span<const Index> points);

    void clear();
    /** Drops deleted elements and makes face corners contiguous. */
    void compact(MeshRemap* remap = nullptr);
    /** Appends other's elements; returns the point id offset. Attribute layers are merged by name. */
    Index append(const Mesh& other);

    /** Problems found, one line each. Empty means valid for the given checks. */
    std::vector<std::string> validate(uint32_t flags = kValidateDefault) const;

    AttributeSet& attributes(Domain d) { return mAttrs[size_t(d)]; }
    const AttributeSet& attributes(Domain d) const { return mAttrs[size_t(d)]; }
    AttributeSet& pointAttrs() { return mAttrs[0]; }
    AttributeSet& edgeAttrs() { return mAttrs[1]; }
    AttributeSet& faceAttrs() { return mAttrs[2]; }
    AttributeSet& cornerAttrs() { return mAttrs[3]; }
    const AttributeSet& pointAttrs() const { return mAttrs[0]; }
    const AttributeSet& edgeAttrs() const { return mAttrs[1]; }
    const AttributeSet& faceAttrs() const { return mAttrs[2]; }
    const AttributeSet& cornerAttrs() const { return mAttrs[3]; }

    /** Names for face "material" ids. */
    std::vector<std::string>& materialNames() { return mMaterialNames; }
    const std::vector<std::string>& materialNames() const { return mMaterialNames; }

    ColorSetInfo& colorSetInfo() { return mColorSetInfo; }
    const ColorSetInfo& colorSetInfo() const { return mColorSetInfo; }

    void reserve(size_t points, size_t edges, size_t faces, size_t corners);

    /** Builds from a polygon soup: faceSizes[i] corners per face, indices into points. Bad faces are skipped. */
    static Mesh fromPolygons(std::span<const glm::dvec3> points, std::span<const Index> faceSizes,
                             std::span<const Index> faceIndices);

private:
    struct Edge
    {
        Index v[2];
        Index next[2];
        Index prev[2];
        Index corner;
    };
    struct Corner
    {
        Index point;
        Index edge;
        Index face;
        Index next;
        Index prev;
        Index radialNext;
        Index radialPrev;
    };
    struct Face
    {
        Index corner;
        Index size;
    };

    int side(Index e, Index p) const { return mEdges[e].v[0] == p ? 0 : 1; }
    Index newEdge(Index a, Index b);
    Index newCorner(Index point, Index face);
    void diskInsert(Index e, Index p);
    void diskRemove(Index e, Index p);
    void radialInsert(Index c, Index e);
    void radialRemove(Index c);
    void killCorner(Index c);
    static uint64_t newTopologyId();
    void touchTopology() { mTopologyId = newTopologyId(); }

    UninitVector<glm::dvec3> mPointPos;
    UninitVector<Index> mPointEdge;
    UninitVector<uint8_t> mPointDead;
    UninitVector<Edge> mEdges;
    UninitVector<Corner> mCorners;
    UninitVector<Face> mFaces;
    Index mNumPoints = 0;
    Index mNumEdges = 0;
    Index mNumFaces = 0;
    Index mNumCorners = 0;
    Index mLastSplitEdge = kInvalid;
    uint64_t mTopologyId = newTopologyId();
    AttributeSet mAttrs[size_t(Domain::Count)];
    std::vector<std::string> mMaterialNames;
    ColorSetInfo mColorSetInfo;
};
} // namespace lila::geo
