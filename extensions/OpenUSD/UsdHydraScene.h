#pragma once

#include "UsdStage.h"

#include <lila/Graph.h>
#include <lila/Interchange.h>
#include <lila/ViewportRenderers.h>

#include <pxr/imaging/hd/filteringSceneIndex.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>

#include <glm/glm.hpp>

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * The scene as Hydra scene indices. Lila's own nodes (meshes, curves, cameras, lights) are prims of a scene index kept
 * up to date by diffing the graph on each render: a moved node dirties its transform, a new mesh snapshot its primvars
 * (or its topology when that changed), so a drag or a deformation never rebuilds the rest. Meshes go in as their own
 * polygons with face-varying normals, UVs and colours, subdivided at their smooth preview level as Lila draws them. Lila's
 * materials are written as UsdPreviewSurface networks into a stage in memory and read through UsdImaging. Live USD
 * stages go in through UsdImaging too: each prim Lila draws itself (with its edits) is blanked out of the stage, and
 * every prim Lila has a node for is placed and shown as that node is, so the stage's other prims follow it.
 */
namespace lila::usd::hydra
{
/** Where Lila's nodes and materials sit in the merged scene. */
inline const pxr::SdfPath& nodesRoot()
{
    static const pxr::SdfPath p("/Lila");
    return p;
}
inline const pxr::SdfPath& materialsRoot()
{
    static const pxr::SdfPath p("/LilaMaterials");
    return p;
}
/** The prefix a live stage's prims get. */
pxr::SdfPath stageRoot(scene::NodeId stageNode);

/** What one viewport shows differently from another. */
struct ViewContext
{
    /** The view's isolate select set; null shows everything. */
    const scene::isolation::Set* isolation = nullptr;
    /** World units per device pixel at a point, to give curves the width Lila draws them with. */
    std::function<double(const glm::dvec3&)> unitsPerPixel;
    float curvePixels = 2.0f;
    glm::vec4 curveColor{0.392f, 0.392f, 0.392f, 1.0f};
    /** Where Hydra's float space is centred (hydraOrigin of the eye): prims sit relative to it, so Storm draws exactly far from the world origin. */
    glm::dvec3 origin{0.0};
};

/** Hydra moves its origin in steps this many cm apart, each a full transform update, so it rarely does. */
inline constexpr double kHydraOriginCell = 65536.0;

/** The eye snapped to kHydraOriginCell: within a cell every prim near the camera keeps float precision under 0.01 cm. */
inline glm::dvec3 hydraOrigin(const glm::dvec3& eye)
{
    // Within 1 km of the origin floats are exact, so ordinary scenes never resync.
    if (glm::length(eye) <= 1e5)
        return glm::dvec3(0.0);
    return glm::floor(eye / kHydraOriginCell + 0.5) * kHydraOriginCell;
}

/** Lila's meshes, curves, cameras and lights as Hydra prims, flat under nodesRoot() with world transforms. */
class LilaSceneIndex final : public pxr::HdSceneIndexBase
{
public:
    static pxr::TfRefPtr<LilaSceneIndex> New() { return pxr::TfCreateRefPtr(new LilaSceneIndex()); }

    pxr::HdSceneIndexPrim GetPrim(const pxr::SdfPath& path) const override;
    pxr::SdfPathVector GetChildPrimPaths(const pxr::SdfPath& path) const override;

    /** Main thread, g evaluated. bind(slot) is the material path a material slot binds to. */
    void sync(scene::Graph& g, const ViewContext& view, const std::function<pxr::SdfPath(const std::string& slot)>& bind);
    /** The prims added, changed and removed by the last sync, for tests. */
    struct Counts
    {
        size_t added = 0, dirtied = 0, removed = 0;
    };
    Counts lastSync() const { return mCounts; }
    /** Main thread, after a sync: what follows the camera alone (curve widths) for a view that moved with the scene unchanged. */
    void syncView(const ViewContext& view);
    /** The material slots the meshes use. */
    const std::set<std::string>& usedSlots() const { return mSlots; }
    /** Moves every prim so origin is at Hydra's origin; a no-op while it stays the same. */
    void setOrigin(const glm::dvec3& origin);

private:
    LilaSceneIndex() = default;

    struct MeshData;
    struct CurveData;
    struct Node
    {
        pxr::SdfPath path;
        pxr::TfToken type;
        std::shared_ptr<const MeshData> mesh;
        /** The mesh as this prim shows it: isolate select's hidden faces left out. */
        struct
        {
            pxr::HdContainerDataSourceHandle mesh, primvars;
            std::vector<pxr::VtIntArray> subsets;
        } shape;
        std::shared_ptr<const CurveData> curve;
        glm::dmat4 world{1.0};
        bool visible = true;
        size_t settings = 0;
        std::vector<pxr::SdfPath> bindings;
        /** Faces isolate select hides, as indices into the mesh's faces. */
        pxr::VtIntArray holes;
        float width = 0.0f;
        pxr::HdContainerDataSourceHandle own; // camera or light parameters
    };
    struct Prim
    {
        pxr::TfToken type;
        pxr::HdContainerDataSourceHandle ds;
    };

    void put(const pxr::SdfPath& path, Prim prim);
    void drop(const pxr::SdfPath& path);
    pxr::HdContainerDataSourceHandle nodeSource(const Node& n) const;
    /** The width n's curve draws with from view, keeping was's while it stays within 15%, so small moves dirty nothing. */
    static float curveWidth(const Node& n, const ViewContext& view, float was);
    std::shared_ptr<const MeshData> meshData(const scene::MeshPtr& mesh, int level);
    std::shared_ptr<const CurveData> curveData(const scene::Node& n);

    std::map<pxr::SdfPath, Prim> mPrims;
    std::map<pxr::SdfPath, std::set<pxr::SdfPath>> mChildren;
    std::unordered_map<scene::NodeId, Node> mNodes;
    glm::dvec3 mOrigin{0.0};
    std::map<std::pair<const void*, int>, std::pair<scene::MeshPtr, std::shared_ptr<const MeshData>>> mMeshes;
    std::set<std::string> mSlots;
    Counts mCounts;
};

/**
 * Lila's materials as UsdPreviewSurface networks in a stage in memory (written by MaterialWriter, as USD export writes
 * them), rewritten when their material node changes. stage() goes into Hydra through UsdImaging at materialsRoot().
 */
class MaterialLibrary
{
public:
    MaterialLibrary();
    pxr::UsdStageRefPtr stage() const { return mStage; }
    /** The material prim of a slot, under materialsRoot(). */
    pxr::SdfPath bind(const std::string& slot) const;
    /** Writes the slots not written yet, and those whose material changed since. */
    void sync(scene::Graph& g, const std::set<std::string>& used);
    /** A material node changed (Value, Created, Deleted, Renamed), or anything did (Reset, Cleared). */
    void changed(scene::Graph& g, scene::Change c, scene::NodeId id);

private:
    pxr::UsdStageRefPtr mStage;
    std::set<std::string> mWritten;
    std::set<std::string> mStale;
    bool mAllStale = false;
};

/**
 * Before UsdImaging flattens a live stage: prims Lila draws itself are blanked (left as transforms, so what is below
 * them still composes), and PointInstancers Lila realised as nodes are pruned with everything under them.
 */
class StageOverrides final : public pxr::HdSingleInputFilteringSceneIndexBase
{
public:
    static pxr::TfRefPtr<StageOverrides> New(const pxr::HdSceneIndexBaseRefPtr& input)
    {
        return pxr::TfCreateRefPtr(new StageOverrides(input));
    }

    void update(std::set<pxr::SdfPath> blank, std::set<pxr::SdfPath> pruned);

    pxr::HdSceneIndexPrim GetPrim(const pxr::SdfPath& path) const override;
    pxr::SdfPathVector GetChildPrimPaths(const pxr::SdfPath& path) const override;

protected:
    void _PrimsAdded(const pxr::HdSceneIndexBase& sender, const pxr::HdSceneIndexObserver::AddedPrimEntries& entries) override;
    void _PrimsRemoved(const pxr::HdSceneIndexBase& sender, const pxr::HdSceneIndexObserver::RemovedPrimEntries& entries) override;
    void _PrimsDirtied(const pxr::HdSceneIndexBase& sender, const pxr::HdSceneIndexObserver::DirtiedPrimEntries& entries) override;

private:
    explicit StageOverrides(const pxr::HdSceneIndexBaseRefPtr& input) : HdSingleInputFilteringSceneIndexBase(input) {}
    bool insidePruned(const pxr::SdfPath& path) const;

    std::set<pxr::SdfPath> mBlank;
    std::set<pxr::SdfPath> mPruned;
};

/**
 * After UsdImaging: places a live stage's prims in Lila's world (the stage node's place, axes and units), each prim Lila
 * has a node for where that node is now, and everything below it carried along, with the node's visibility. A moved
 * node dirties only the transforms of its prim and what is below it.
 */
class StagePlacement final : public pxr::HdSingleInputFilteringSceneIndexBase
{
public:
    static pxr::TfRefPtr<StagePlacement> New(const pxr::HdSceneIndexBaseRefPtr& input) { return pxr::TfCreateRefPtr(new StagePlacement(input)); }

    struct Placed
    {
        glm::dmat4 matrix{1.0}; // stage-space world matrix
        bool visible = true;
        bool operator==(const Placed&) const = default;
    };
    void update(const glm::dmat4& toWorld, std::map<pxr::SdfPath, Placed> placed);

    pxr::HdSceneIndexPrim GetPrim(const pxr::SdfPath& path) const override;
    pxr::SdfPathVector GetChildPrimPaths(const pxr::SdfPath& path) const override;

protected:
    void _PrimsAdded(const pxr::HdSceneIndexBase& sender, const pxr::HdSceneIndexObserver::AddedPrimEntries& entries) override;
    void _PrimsRemoved(const pxr::HdSceneIndexBase& sender, const pxr::HdSceneIndexObserver::RemovedPrimEntries& entries) override;
    void _PrimsDirtied(const pxr::HdSceneIndexBase& sender, const pxr::HdSceneIndexObserver::DirtiedPrimEntries& entries) override;

private:
    explicit StagePlacement(const pxr::HdSceneIndexBaseRefPtr& input) : HdSingleInputFilteringSceneIndexBase(input) {}
    /** The prim's stage-space world matrix as UsdImaging flattened it. */
    glm::dmat4 flattened(const pxr::SdfPath& path) const;

    glm::dmat4 mToWorld{1.0};
    std::map<pxr::SdfPath, Placed> mPlaced;
};

/** True when Lila's scene index draws the node (a mesh or curve with geometry, a camera, a light), so its stage prim is blanked. */
bool drawnByLila(const scene::Node& n);
} // namespace lila::usd::hydra
